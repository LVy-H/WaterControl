#include "water/controller.hpp"
#include "plant.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {
bool forbid_allocation = false;
unsigned passed = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

bool near(float actual, float expected, float tolerance = 0.15F) {
    return std::fabs(actual - expected) <= tolerance;
}

void pass(const char* name) {
    ++passed;
    std::printf("PASS %s\n", name);
}

bool allPumpsOff(const water::Snapshot& state) {
    return !state.outputs.source_pump && !state.outputs.house_pump[0] && !state.outputs.house_pump[1];
}

water::Inputs healthyInputs() {
    water::Inputs inputs;
    inputs.tank_permit = {{true, true}};
    inputs.source_permit = true;
    inputs.network_online = true;
    return inputs;
}

std::size_t eventsWithCode(const water::Controller& controller, water::EventCode code) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < controller.events().size(); ++index) {
        if (controller.events().at(index).code == code) ++count;
    }
    return count;
}

void startupSequence() {
    water::sim::Plant plant;
    require(allPumpsOff(plant.controller().snapshot()), "boot must leave all pumps off");
    plant.command(water::CommandKind::Pump2On);
    require(plant.controller().snapshot().outputs.valve_open[1], "demand opens the branch valve first");
    require(allPumpsOff(plant.controller().snapshot()), "no pump may start before valve settling");
    plant.advance(280);
    require(!plant.controller().snapshot().outputs.source_pump, "source pump waits full valve-settle interval");
    plant.advance(20);
    require(plant.controller().snapshot().outputs.source_pump, "source pump starts after valve-settle interval");
    require(!plant.controller().snapshot().outputs.house_pump[1], "branch pump waits for source priming");
    plant.advance(480);
    require(!plant.controller().snapshot().outputs.house_pump[1], "branch pump cannot prime early");
    plant.advance(20);
    require(plant.controller().snapshot().outputs.house_pump[1], "branch pump starts after source priming");
    pass("sequenced valve/source/branch startup");
}

void bootDemandMustBeReleased() {
    water::Controller controller;
    auto inputs = healthyInputs();
    inputs.house1_switch = true;
    inputs.button2_pressed = true;
    controller.update(inputs);
    for (unsigned step = 0; step < 20; ++step) {
        inputs.now_ms += 20;
        controller.update(inputs);
    }
    require(allPumpsOff(controller.snapshot()), "held switches at reboot must not restart pumps");
    inputs.house1_switch = false;
    inputs.button2_pressed = false;
    for (unsigned step = 0; step < 5; ++step) {
        inputs.now_ms += 20;
        controller.update(inputs);
    }
    inputs.house1_switch = true;
    inputs.now_ms += 20;
    controller.update(inputs);
    require(controller.snapshot().requested[0], "fresh physical demand after release must be accepted");
    pass("boot with held inputs requires release and a fresh request");
}

void balancedFlowAndIdle() {
    water::sim::Plant plant;
    plant.advance(10000);
    require(plant.controller().snapshot().faults == water::NoFault, "zero idle flow is not a blockage");
    plant.setHouse1Switch(true);
    plant.command(water::CommandKind::Pump2On);
    plant.advance(10000);
    const auto& state = plant.controller().snapshot();
    require(state.faults == water::NoFault && !state.warning, "balanced filling must not cause an alarm");
    require(near(state.flow_lpm[0], 7.0F) && near(state.flow_lpm[1], 3.0F) && near(state.flow_lpm[2], 4.0F),
            "generated pulse totals must become the corresponding physical flow rates");
    require(state.outputs.house_pump[0] && state.outputs.house_pump[1], "both houses can fill concurrently");
    pass("idle and simultaneous balanced filling");
}

void freshFloatWinsOverCommand() {
    water::Controller controller;
    auto inputs = healthyInputs();
    controller.update(inputs);
    inputs.now_ms = 20;
    inputs.tank_permit[1] = false;
    inputs.command = {water::CommandKind::Pump2On, water::Origin::Remote, inputs.now_ms};
    controller.update(inputs);
    require(!controller.snapshot().requested[1] && allPumpsOff(controller.snapshot()),
            "raw full/open float must veto an ON command in the same update");
    require(eventsWithCode(controller, water::EventCode::RejectFull) == 1, "float veto needs a precise rejection event");
    pass("simultaneous full float and remote ON cannot energize a pump");
}

void floatStopAndNoRestart() {
    water::sim::Plant plant;
    plant.command(water::CommandKind::Pump2On);
    plant.advance(2000);
    plant.setFault("tank2_wire_open", true);
    require(allPumpsOff(plant.controller().snapshot()), "an open float wire immediately removes pump permission");
    require(!plant.controller().snapshot().requested[1], "float inhibit discards demand rather than merely masking output");
    plant.setFault("tank2_wire_open", false);
    plant.advance(1000);
    require(allPumpsOff(plant.controller().snapshot()), "repairing float wire must not restart a discarded request");
    plant.setHouse1Switch(true);
    plant.advance(1000);
    plant.setTankPercent(0, 100.0);
    require(allPumpsOff(plant.controller().snapshot()), "house1 also needs a working full-tank interlock");
    plant.setTankPercent(0, 20.0);
    plant.advance(1000);
    require(allPumpsOff(plant.controller().snapshot()), "held house1 switch must not restart after tank drains");
    plant.setHouse1Switch(false);
    plant.setHouse1Switch(true);
    plant.advance(1000);
    require(plant.controller().snapshot().outputs.house_pump[0], "release/reassert re-arms the house1 request");
    pass("float/open-wire stop and explicit restart for both houses");
}

void sourceFaultAndAcknowledgement() {
    water::sim::Plant plant;
    plant.command(water::CommandKind::Pump2On);
    plant.advance(2000);
    plant.setFault("source_wire_open", true);
    require(allPumpsOff(plant.controller().snapshot()) && plant.controller().snapshot().outputs.alarm,
            "unsafe source must stop all pumps and latch alarm");
    plant.command(water::CommandKind::ResetFaults, water::Origin::Local);
    require(plant.controller().snapshot().faults & water::SourceUnsafe, "ACK cannot clear a source fault while wire remains open");
    const auto fault_events = eventsWithCode(plant.controller(), water::EventCode::FaultSourceUnsafe);
    plant.advance(10000);
    require(eventsWithCode(plant.controller(), water::EventCode::FaultSourceUnsafe) == fault_events,
            "a persistent source fault must not log on each loop");
    plant.setFault("source_wire_open", false);
    require(plant.controller().snapshot().outputs.alarm, "repair alone cannot clear a latched alarm");
    plant.command(water::CommandKind::ResetFaults, water::Origin::Local);
    require(plant.controller().snapshot().faults == water::NoFault && allPumpsOff(plant.controller().snapshot()),
            "safe ACK clears latch without restarting pumps");
    pass("source dry/open interlock, latched alarm, and safe acknowledgement");
}

void mainLeakPersistsThenLatches() {
    water::sim::Plant plant;
    plant.command(water::CommandKind::Pump2On);
    plant.advance(4000);
    plant.setFault("main_leak", true);
    plant.advance(1000);
    require(plant.controller().snapshot().faults == water::NoFault, "one abnormal sample cannot confirm a leak");
    plant.advance(5000);
    require(plant.controller().snapshot().faults == water::UnaccountedFlow,
            "persistent upstream imbalance must be classified without invented house localisation");
    require(allPumpsOff(plant.controller().snapshot()), "confirmed imbalance must shut down all pumps");
    const auto errors = eventsWithCode(plant.controller(), water::EventCode::FaultUnaccountedFlow);
    plant.setFault("main_leak", false);
    plant.advance(10000);
    require(plant.controller().snapshot().faults == water::UnaccountedFlow,
            "zero flow after shutdown must not erase the fault that caused it");
    require(eventsWithCode(plant.controller(), water::EventCode::FaultUnaccountedFlow) == errors,
            "latched leak must generate one error event, not repeated telemetry-like logs");
    pass("persistent main imbalance, shutdown, and single fault event");
}

void transientImbalanceResetsDwell() {
    water::sim::Plant plant;
    plant.command(water::CommandKind::Pump2On);
    plant.advance(4000);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        plant.setFault("main_leak", true);
        plant.advance(1000);
        plant.setFault("main_leak", false);
        plant.advance(3000);
    }
    require(plant.controller().snapshot().faults == water::NoFault,
            "separated transient imbalances must not accumulate into a persistent fault");
    pass("transient imbalance does not accumulate across healthy samples");
}

void blockageAndMeterMismatch() {
    water::sim::Plant blocked;
    blocked.setFault("blockage2", true);
    blocked.command(water::CommandKind::Pump2On);
    blocked.advance(10000);
    require(blocked.controller().snapshot().faults == water::House2LowFlow,
            "blocked demanded branch is low flow, not a localised leak or proven sensor failure");
    require(allPumpsOff(blocked.controller().snapshot()), "low-flow fault must shut down output");
    water::sim::Plant failed_meter;
    failed_meter.setFault("main_meter_stuck", true);
    failed_meter.command(water::CommandKind::Pump2On);
    failed_meter.advance(10000);
    require(failed_meter.controller().snapshot().faults == water::MeterMismatch,
            "more branch flow than main flow indicates inconsistent meters");
    pass("demanded low-flow and inconsistent-meter classifications");
}

void downstreamLeakIsNotObservable() {
    water::sim::Plant plant;
    plant.setFault("downstream_leak1", true);
    plant.setHouse1Switch(true);
    plant.advance(10000);
    const auto& state = plant.controller().snapshot();
    require(state.faults == water::NoFault && near(state.flow_lpm[0], state.flow_lpm[1]),
            "balanced meter readings cannot identify leakage after a branch meter");
    require(near(state.flow_lpm[1], 3.0F) && near(plant.deliveredLpm()[0], 1.5F),
            "model must show reduced delivery rather than fake leak detection");
    pass("downstream leak blind spot is explicit, not falsely detected");
}

void cloudLossAndLocalIndependence() {
    water::sim::Plant plant;
    plant.setHouse1Switch(true);
    plant.command(water::CommandKind::Pump2On);
    plant.advance(2000);
    plant.setNetwork(false);
    require(plant.controller().snapshot().outputs.house_pump[0] && !plant.controller().snapshot().outputs.house_pump[1],
            "cloud loss cancels remote house2 demand but preserves local house1 operation");
    plant.command(water::CommandKind::Pump2On);
    require(!plant.controller().snapshot().requested[1], "remote ON must be rejected offline");
    plant.setNetwork(true);
    plant.advance(1000);
    require(!plant.controller().snapshot().requested[1], "reconnection cannot replay previous remote ON");
    plant.setNetwork(false);
    plant.pressButton();
    plant.advance(1000);
    require(plant.controller().snapshot().outputs.house_pump[1], "local house2 button works offline");
    plant.advance(1000);
    require(plant.controller().snapshot().outputs.house_pump[1], "local demand is not cancelled by offline policy");
    pass("cloud loss, no replay, and independent local controls");
}

void isolationStopsAndRequiresNewDemand() {
    water::sim::Plant plant;
    plant.setHouse1Switch(true);
    plant.command(water::CommandKind::Pump2On);
    plant.advance(2000);
    plant.command(water::CommandKind::IsolateHouse1);
    require(!plant.controller().snapshot().outputs.house_pump[0] && !plant.controller().snapshot().outputs.valve_open[0],
            "isolation stops branch pump and closes its valve");
    require(plant.controller().snapshot().outputs.house_pump[1], "isolating house1 cannot stop a healthy house2 request");
    plant.command(water::CommandKind::AllowHouse1);
    plant.advance(1000);
    require(!plant.controller().snapshot().requested[0], "reopening permission cannot replay held physical demand");
    plant.setHouse1Switch(false);
    plant.setHouse1Switch(true);
    plant.advance(1000);
    require(plant.controller().snapshot().outputs.house_pump[0], "fresh switch cycle restarts an allowed branch");
    pass("branch isolation and deliberate restart");
}

void buttonBounceAndHold() {
    water::Controller controller;
    auto inputs = healthyInputs();
    controller.update(inputs);
    for (unsigned edge = 0; edge < 6; ++edge) {
        inputs.button2_pressed = edge % 2 == 0;
        inputs.now_ms += 10;
        controller.update(inputs);
    }
    require(!controller.snapshot().requested[1], "short bouncing edges cannot toggle demand");
    inputs.button2_pressed = true;
    for (unsigned step = 0; step < 15; ++step) {
        inputs.now_ms += 20;
        controller.update(inputs);
    }
    require(controller.snapshot().requested[1], "a stable press toggles demand exactly once while held");
    require(eventsWithCode(controller, water::EventCode::RequestOn) == 1,
            "held button cannot generate repeated ON events");
    pass("nonblocking debounce and single action per press");
}

void commandAgeAndDeadline() {
    water::Controller controller;
    auto inputs = healthyInputs();
    inputs.now_ms = 5000;
    controller.update(inputs);
    inputs.now_ms += 20;
    inputs.command = {water::CommandKind::Pump2On, water::Origin::Remote, 1000};
    controller.update(inputs);
    require(!controller.snapshot().requested[1], "expired ON cannot start the pump");
    require(eventsWithCode(controller, water::EventCode::RejectExpired) == 1, "expired command is visible in logs");
    inputs.now_ms += 20;
    inputs.command = {water::CommandKind::Pump2On, water::Origin::Remote, inputs.now_ms};
    controller.update(inputs);
    require(controller.snapshot().requested[1], "fresh remote command is accepted");
    inputs.now_ms += 20;
    inputs.command = {water::CommandKind::Pump2Off, water::Origin::Remote, 0};
    controller.update(inputs);
    require(!controller.snapshot().requested[1], "old STOP remains safe to apply");
    inputs.now_ms += 251;
    inputs.command = {water::CommandKind::Pump2On, water::Origin::Local, inputs.now_ms};
    controller.update(inputs);
    require(controller.snapshot().faults == water::ControlDeadlineMissed && allPumpsOff(controller.snapshot()),
            "missed control deadline latches shutdown before accepting any fresh ON");
    pass("command expiry, safe STOP, and control deadline fault");
}

void timePulseRolloverAndCalibration() {
    water::Config config;
    config.pulses_per_litre = {{450.0F, 600.0F, 300.0F}};
    water::Controller controller(config);
    auto inputs = healthyInputs();
    inputs.now_ms = std::numeric_limits<std::uint32_t>::max() - 600;
    inputs.pulse_total.fill(std::numeric_limits<std::uint32_t>::max() - 10);
    controller.update(inputs);
    for (unsigned step = 0; step < 6; ++step) {
        inputs.now_ms += step == 4 ? 150U : (step == 5 ? 250U : 200U);
        if (step == 5) {
            inputs.pulse_total[0] += 27;
            inputs.pulse_total[1] += 12;
            inputs.pulse_total[2] += 12;
        }
        controller.update(inputs);
    }
    const auto& state = controller.snapshot();
    require(state.faults == water::NoFault, "millisecond rollover cannot cause a spurious deadline fault");
    require(state.flow_valid && near(state.flow_lpm[0], 3.0F, 0.001F)
        && near(state.flow_lpm[1], 1.0F, 0.001F) && near(state.flow_lpm[2], 2.0F, 0.001F),
        "flow must use per-meter calibration, actual1200ms window, and wrap-safe pulse deltas");
    pass("time/pulse rollover, unequal calibration, and irregular sample duration");
}

void badConfigurationFailsClosed() {
    water::Config config;
    config.pulses_per_litre[1] = 0.0F;
    water::Controller controller(config);
    auto inputs = healthyInputs();
    inputs.command = {water::CommandKind::Pump2On, water::Origin::Local, 0};
    controller.update(inputs);
    require(controller.snapshot().faults == water::InvalidConfiguration && allPumpsOff(controller.snapshot()),
            "invalid calibration must fail closed instead of divide by zero");
    pass("invalid calibration fails closed");
}

void boundedLogsAndMeaningfulUnits() {
    water::sim::Plant plant;
    const auto boot_sequence = plant.controller().events().newestSequence();
    plant.advance(60000);
    require(plant.controller().events().newestSequence() == boot_sequence,
            "steady-state sampling must not produce per-loop or per-sample logs");
    plant.setTankPercent(1, 100.0);
    for (unsigned request = 0; request < 100; ++request) plant.command(water::CommandKind::Pump2On);
    require(plant.controller().snapshot().rejected_commands == 100 && plant.controller().snapshot().suppressed_logs >= 95,
            "repeated rejections must be counted while log traffic is rate limited");
    for (unsigned edge = 0; edge < 60; ++edge) plant.setNetwork(edge % 2 != 0);
    const auto& log = plant.controller().events();
    require(log.size() == water::kLogCapacity && log.overwritten() > 0,
            "log overflow must stay bounded and expose discarded history");
    for (std::size_t index = 1; index < log.size(); ++index) {
        require(log.at(index).sequence == log.at(index - 1).sequence + 1,
                "overwritten ring must retain the newest ordered records");
    }
    require(log.at(log.size() - 1).sequence == log.newestSequence(), "newest event must survive ring wrap");
    const water::Event event{42, 7000, water::Severity::Error, water::EventCode::FaultUnaccountedFlow,
                             water::Subject::Source, 1500};
    char record[192];
    require(water::formatEvent(event, record, sizeof(record)) > 0, "fault record must format into the bounded transport buffer");
    require(std::strcmp(record, "t_ms=7000 seq=42 level=ERROR code=FAULT_UNACCOUNTED_FLOW subject=SOURCE value=1500 unit=mL/min\n") == 0,
            "log contract must carry exact code, subject, timestamp, severity, value and flow units");
    char short_record[8];
    require(water::formatEvent(event, short_record, sizeof(short_record)) == 0 && short_record[0] == '\0',
            "a transport must never emit a truncated record as if it were valid");
    for (unsigned code = 0; code <= static_cast<unsigned>(water::EventCode::FaultsCleared); ++code) {
        const water::Event worst{std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(),
            water::Severity::Error, static_cast<water::EventCode>(code), water::Subject::Controller,
            std::numeric_limits<std::int32_t>::min()};
        require(water::formatEvent(worst, record, sizeof(record)) > 0, "all published codes must fit the firmware log record buffer");
    }
    pass("bounded ordered logs, suppression counters, and exact embedded record contract");
}

void noControlHeapAllocation() {
    water::Controller controller;
    auto inputs = healthyInputs();
    forbid_allocation = true;
    for (unsigned step = 0; step < 1000; ++step) {
        inputs.now_ms += 20;
        inputs.command = step % 100 == 0
            ? water::Command{water::CommandKind::Pump2On, water::Origin::Local, inputs.now_ms}
            : water::Command{};
        controller.update(inputs);
    }
    forbid_allocation = false;
    require(controller.snapshot().faults & water::House2LowFlow,
            "allocation-free exercise must include a real fault path, not only idle updates");
    pass("controller update and fault paths make no C++ heap allocations");
}
}  // namespace

void* operator new(std::size_t size) {
    if (forbid_allocation) {
        std::fputs("FAIL: controller attempted a heap allocation\n", stderr);
        std::abort();
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) std::abort();
    return memory;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    startupSequence();
    bootDemandMustBeReleased();
    balancedFlowAndIdle();
    freshFloatWinsOverCommand();
    floatStopAndNoRestart();
    sourceFaultAndAcknowledgement();
    mainLeakPersistsThenLatches();
    transientImbalanceResetsDwell();
    blockageAndMeterMismatch();
    downstreamLeakIsNotObservable();
    cloudLossAndLocalIndependence();
    isolationStopsAndRequiresNewDemand();
    buttonBounceAndHold();
    commandAgeAndDeadline();
    timePulseRolloverAndCalibration();
    badConfigurationFailsClosed();
    boundedLogsAndMeaningfulUnits();
    noControlHeapAllocation();
    std::printf("%u scenarios passed. sizeof(Controller)=%zu bytes; sizeof(Event)=%zu bytes; log capacity=%zu.\n",
                passed, sizeof(water::Controller), sizeof(water::Event), water::kLogCapacity);
    return 0;
}
