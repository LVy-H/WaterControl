#include "water/controller.hpp"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <limits>

namespace water {
namespace {
Subject houseSubject(std::size_t house) noexcept {
    return house == 0 ? Subject::House1 : Subject::House2;
}

Subject valveSubject(std::size_t house) noexcept {
    return house == 0 ? Subject::Valve1 : Subject::Valve2;
}

std::int32_t boundedInt(std::uint32_t value) noexcept {
    const auto limit = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    return static_cast<std::int32_t>(std::min(value, limit));
}

std::int32_t millilitresPerMinute(float value) noexcept {
    const double scaled = static_cast<double>(value) * 1000.0;
    if (scaled >= std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (scaled <= std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return std::isfinite(scaled) ? static_cast<std::int32_t>(std::round(scaled)) : 0;
}

bool validDuration(std::uint32_t duration) noexcept {
    return duration > 0 && duration < 0x80000000U;
}
}  // namespace

void EventLog::append(std::uint32_t now_ms, Severity severity, EventCode code,
                      Subject subject, std::int32_t value) noexcept {
    entries_[next_] = Event{++sequence_, now_ms, severity, code, subject, value};
    next_ = (next_ + 1) % kLogCapacity;
    if (count_ < kLogCapacity) {
        ++count_;
    } else {
        ++overwritten_;
    }
}

const Event& EventLog::at(std::size_t oldest_index) const noexcept {
    assert(oldest_index < count_);
    return entries_[(next_ + kLogCapacity - count_ + oldest_index) % kLogCapacity];
}

Controller::Controller(const Config& config) noexcept : config_(config) {
    for (float coefficient : config_.pulses_per_litre) {
        configuration_valid_ = configuration_valid_ && std::isfinite(coefficient) && coefficient > 0.0F;
    }
    configuration_valid_ = configuration_valid_
        && validDuration(config_.flow_window_ms)
        && validDuration(config_.valve_settle_ms)
        && validDuration(config_.source_prime_ms)
        && validDuration(config_.hydraulic_settle_ms)
        && validDuration(config_.fault_dwell_ms)
        && validDuration(config_.button_debounce_ms)
        && validDuration(config_.command_ttl_ms)
        && validDuration(config_.max_control_gap_ms)
        && std::isfinite(config_.minimum_flow_lpm) && config_.minimum_flow_lpm > 0.0F
        && std::isfinite(config_.absolute_balance_tolerance_lpm)
        && config_.absolute_balance_tolerance_lpm >= 0.0F
        && std::isfinite(config_.relative_balance_tolerance)
        && config_.relative_balance_tolerance >= 0.0F && config_.relative_balance_tolerance < 1.0F;
}

void Controller::emit(EventCode code, Subject subject, std::int32_t value, Severity severity) noexcept {
    events_.append(state_.time_ms, severity, code, subject, value);
}

void Controller::reject(EventCode code, Subject subject, std::int32_t value) noexcept {
    ++state_.rejected_commands;
    if (have_last_rejection_ && last_rejection_code_ == code && last_rejection_subject_ == subject
        && state_.time_ms - last_rejection_ms_ < 1000U) {
        ++state_.suppressed_logs;
        return;
    }
    emit(code, subject, value, Severity::Warning);
    have_last_rejection_ = true;
    last_rejection_code_ = code;
    last_rejection_subject_ = subject;
    last_rejection_ms_ = state_.time_ms;
}

void Controller::requestPump(std::size_t house, bool on, Origin origin) noexcept {
    if (on) {
        if (!state_.tank_permit[house]) {
            reject(EventCode::RejectFull, houseSubject(house));
            return;
        }
        if (!state_.source_permit) {
            reject(EventCode::RejectSource, houseSubject(house));
            return;
        }
        if (state_.faults != NoFault) {
            reject(EventCode::RejectFault, houseSubject(house), boundedInt(state_.faults));
            return;
        }
        if (state_.isolated[house]) {
            reject(EventCode::RejectIsolated, houseSubject(house));
            return;
        }
    }
    if (house == 1) {
        pump2_owner_ = origin;
    }
    if (state_.requested[house] != on) {
        state_.requested[house] = on;
        emit(on ? EventCode::RequestOn : EventCode::RequestOff, houseSubject(house), on ? 1 : 0);
    }
}

void Controller::latchFault(Fault fault, EventCode code, Subject subject, std::int32_t value) noexcept {
    if ((state_.faults & static_cast<std::uint32_t>(fault)) == 0) {
        state_.faults |= static_cast<std::uint32_t>(fault);
        emit(code, subject, value, Severity::Error);
    }
    requestPump(0, false, Origin::Local);
    requestPump(1, false, Origin::Local);
    switch1_armed_ = false;
}

void Controller::processCommand(const Inputs& inputs) noexcept {
    const Command& command = inputs.command;
    if (command.kind == CommandKind::None) {
        return;
    }
    // A STOP is safe even when stale or received during connection teardown.
    if (command.kind == CommandKind::Pump2Off) {
        requestPump(1, false, command.origin);
        return;
    }
    if (command.origin == Origin::Remote) {
        if (!state_.network_online) {
            reject(EventCode::RejectOffline, Subject::Network);
            return;
        }
        const std::uint32_t age = inputs.now_ms - command.issued_at_ms;
        if (age > config_.command_ttl_ms) {
            reject(EventCode::RejectExpired, Subject::Network, boundedInt(age));
            return;
        }
    }
    switch (command.kind) {
    case CommandKind::Pump2On:
        requestPump(1, true, command.origin);
        break;
    case CommandKind::IsolateHouse1:
    case CommandKind::IsolateHouse2:
    case CommandKind::AllowHouse1:
    case CommandKind::AllowHouse2: {
        const bool first = command.kind == CommandKind::IsolateHouse1 || command.kind == CommandKind::AllowHouse1;
        const std::size_t house = first ? 0 : 1;
        const bool isolate = command.kind == CommandKind::IsolateHouse1 || command.kind == CommandKind::IsolateHouse2;
        if (state_.isolated[house] != isolate) {
            emit(EventCode::IsolationChanged, valveSubject(house), isolate ? 1 : 0);
        }
        state_.isolated[house] = isolate;
        if (isolate) {
            requestPump(house, false, Origin::Local);
            if (house == 0) {
                switch1_armed_ = false;
            }
        }
        break;
    }
    case CommandKind::ResetFaults:
        if (state_.faults == NoFault) {
            break;
        }
        if (!configuration_valid_ || !inputs.source_permit || inputs.house1_switch
            || inputs.button2_pressed || state_.requested[0] || state_.requested[1]) {
            reject(EventCode::RejectReset, Subject::Controller);
            break;
        }
        state_.faults = NoFault;
        state_.warning = false;
        abnormal_elapsed_ms_.fill(0);
        last_hydraulic_change_ms_ = inputs.now_ms;
        emit(EventCode::FaultsCleared, Subject::Controller);
        break;
    case CommandKind::None:
    case CommandKind::Pump2Off:
        break;
    }
}

void Controller::updateButton(const Inputs& inputs) noexcept {
    if (inputs.button2_pressed != button_raw_) {
        button_raw_ = inputs.button2_pressed;
        button_changed_ms_ = inputs.now_ms;
    }
    if (!button_stable_ && !button_raw_) {
        button_armed_ = true;
    }
    if (inputs.now_ms - button_changed_ms_ >= config_.button_debounce_ms && button_stable_ != button_raw_) {
        button_stable_ = button_raw_;
        if (!button_stable_) {
            button_armed_ = true;
        } else if (button_armed_) {
            button_armed_ = false;
            requestPump(1, !state_.requested[1], Origin::Local);
        }
    }
}

void Controller::updateOutputs() noexcept {
    const Outputs& old = state_.outputs;
    Outputs next{};
    next.alarm = state_.faults != NoFault;
    if (!next.alarm && state_.source_permit) {
        bool any_ready_valve = false;
        for (std::size_t house = 0; house < kHouseCount; ++house) {
            next.valve_open[house] = state_.requested[house] && state_.tank_permit[house] && !state_.isolated[house];
            if (next.valve_open[house] && !old.valve_open[house]) {
                valve_opened_ms_[house] = state_.time_ms;
            }
            any_ready_valve = any_ready_valve || (next.valve_open[house]
                && state_.time_ms - valve_opened_ms_[house] >= config_.valve_settle_ms);
        }
        next.source_pump = any_ready_valve;
        if (next.source_pump && !old.source_pump) {
            source_started_ms_ = state_.time_ms;
        }
        for (std::size_t house = 0; house < kHouseCount; ++house) {
            next.house_pump[house] = next.source_pump && next.valve_open[house]
                && state_.time_ms - valve_opened_ms_[house] >= config_.valve_settle_ms
                && state_.time_ms - source_started_ms_ >= config_.source_prime_ms;
        }
    }
    bool changed = false;
    for (std::size_t house = 0; house < kHouseCount; ++house) {
        if (next.valve_open[house] != old.valve_open[house]) {
            changed = true;
            emit(next.valve_open[house] ? EventCode::ValveOpened : EventCode::ValveClosed,
                 valveSubject(house), next.valve_open[house] ? 1 : 0);
        }
        if (next.house_pump[house] != old.house_pump[house]) {
            changed = true;
            emit(next.house_pump[house] ? EventCode::PumpStarted : EventCode::PumpStopped,
                 houseSubject(house), next.house_pump[house] ? 1 : 0);
        }
    }
    if (next.source_pump != old.source_pump) {
        changed = true;
        emit(next.source_pump ? EventCode::PumpStarted : EventCode::PumpStopped,
             Subject::Source, next.source_pump ? 1 : 0);
    }
    if (changed) {
        last_hydraulic_change_ms_ = state_.time_ms;
        abnormal_elapsed_ms_.fill(0);
        state_.warning = false;
    }
    state_.outputs = next;
    if (next.alarm) {
        state_.phase = Phase::Faulted;
    } else if (next.house_pump[0] || next.house_pump[1]) {
        state_.phase = Phase::Running;
    } else if (next.source_pump) {
        state_.phase = Phase::PrimingSource;
    } else if (next.valve_open[0] || next.valve_open[1]) {
        state_.phase = Phase::OpeningValves;
    } else {
        state_.phase = Phase::Idle;
    }
}

void Controller::sampleFlow(const Inputs& inputs) noexcept {
    const std::uint32_t elapsed = inputs.now_ms - last_flow_ms_;
    if (elapsed < config_.flow_window_ms) {
        return;
    }
    for (std::size_t meter = 0; meter < kMeterCount; ++meter) {
        const std::uint32_t pulses = inputs.pulse_total[meter] - previous_pulses_[meter];
        previous_pulses_[meter] = inputs.pulse_total[meter];
        state_.flow_lpm[meter] = (static_cast<float>(pulses) / config_.pulses_per_litre[meter])
            * (60000.0F / static_cast<float>(elapsed));
        if (!std::isfinite(state_.flow_lpm[meter])) {
            latchFault(MeterMismatch, EventCode::FaultMeterMismatch, Subject::Controller, 0);
        }
    }
    state_.flow_valid = true;
    last_flow_ms_ = inputs.now_ms;
    // The complete sample window, not just its endpoint, must be settled.
    if (state_.faults != NoFault || inputs.now_ms - last_hydraulic_change_ms_ < config_.hydraulic_settle_ms
        || inputs.now_ms - last_hydraulic_change_ms_ - config_.hydraulic_settle_ms < elapsed) {
        abnormal_elapsed_ms_.fill(0);
        state_.warning = false;
        return;
    }
    const float residual = state_.flow_lpm[0] - state_.flow_lpm[1] - state_.flow_lpm[2];
    const float tolerance = std::max(config_.absolute_balance_tolerance_lpm,
        config_.relative_balance_tolerance * std::max(state_.flow_lpm[0], state_.flow_lpm[1] + state_.flow_lpm[2]));
    const std::array<bool, 4> abnormal{{
        residual > tolerance,
        residual < -tolerance,
        state_.outputs.house_pump[0] && state_.flow_lpm[1] < config_.minimum_flow_lpm,
        state_.outputs.house_pump[1] && state_.flow_lpm[2] < config_.minimum_flow_lpm
    }};
    state_.warning = false;
    for (std::size_t condition = 0; condition < abnormal.size(); ++condition) {
        if (abnormal[condition]) {
            const std::uint32_t remaining = config_.fault_dwell_ms - abnormal_elapsed_ms_[condition];
            abnormal_elapsed_ms_[condition] += std::min(elapsed, remaining);
            state_.warning = true;
        } else {
            abnormal_elapsed_ms_[condition] = 0;
        }
    }
    if (abnormal_elapsed_ms_[0] >= config_.fault_dwell_ms) {
        latchFault(UnaccountedFlow, EventCode::FaultUnaccountedFlow, Subject::Source, millilitresPerMinute(residual));
    }
    if (abnormal_elapsed_ms_[1] >= config_.fault_dwell_ms) {
        latchFault(MeterMismatch, EventCode::FaultMeterMismatch, Subject::Controller, millilitresPerMinute(residual));
    }
    for (std::size_t house = 0; house < kHouseCount; ++house) {
        if (abnormal_elapsed_ms_[house + 2] >= config_.fault_dwell_ms) {
            latchFault(house == 0 ? House1LowFlow : House2LowFlow, EventCode::FaultLowFlow,
                       houseSubject(house), millilitresPerMinute(state_.flow_lpm[house + 1]));
        }
    }
}

const Snapshot& Controller::update(const Inputs& inputs) noexcept {
    state_.time_ms = inputs.now_ms;
    if (!initialized_) {
        initialized_ = true;
        last_update_ms_ = inputs.now_ms;
        last_flow_ms_ = inputs.now_ms;
        last_hydraulic_change_ms_ = inputs.now_ms;
        previous_pulses_ = inputs.pulse_total;
        previous_switch1_ = inputs.house1_switch;
        button_raw_ = inputs.button2_pressed;
        button_stable_ = inputs.button2_pressed;
        button_changed_ms_ = inputs.now_ms;
        emit(EventCode::Boot, Subject::Controller);
        if (!configuration_valid_) {
            latchFault(InvalidConfiguration, EventCode::FaultConfiguration, Subject::Controller, 0);
        }
    } else {
        const std::uint32_t gap = inputs.now_ms - last_update_ms_;
        if (gap > config_.max_control_gap_ms) {
            latchFault(ControlDeadlineMissed, EventCode::FaultControlDeadline, Subject::Controller, boundedInt(gap));
        }
    }
    last_update_ms_ = inputs.now_ms;
    for (std::size_t house = 0; house < kHouseCount; ++house) {
        if (state_.tank_permit[house] != inputs.tank_permit[house]) {
            emit(EventCode::TankPermit, houseSubject(house), inputs.tank_permit[house] ? 1 : 0);
        }
        state_.tank_permit[house] = inputs.tank_permit[house];
        if (!inputs.tank_permit[house]) {
            requestPump(house, false, Origin::Local);
            if (house == 0) {
                switch1_armed_ = false;
            }
        }
    }
    if (state_.source_permit != inputs.source_permit) {
        emit(EventCode::SourcePermit, Subject::Source, inputs.source_permit ? 1 : 0);
    }
    state_.source_permit = inputs.source_permit;
    if (!inputs.source_permit) {
        latchFault(SourceUnsafe, EventCode::FaultSourceUnsafe, Subject::Source, 0);
    }
    if (state_.network_online != inputs.network_online) {
        emit(inputs.network_online ? EventCode::NetworkOnline : EventCode::NetworkOffline, Subject::Network,
             inputs.network_online ? 1 : 0);
    }
    state_.network_online = inputs.network_online;
    if (!state_.network_online && pump2_owner_ == Origin::Remote && state_.requested[1]) {
        requestPump(1, false, Origin::Local);
        emit(EventCode::RemoteCancelled, Subject::House2);
    }
    if (!inputs.house1_switch) {
        requestPump(0, false, Origin::Local);
        switch1_armed_ = true;
    } else if (!previous_switch1_ && switch1_armed_) {
        switch1_armed_ = false;
        requestPump(0, true, Origin::Local);
    }
    previous_switch1_ = inputs.house1_switch;
    processCommand(inputs);
    updateButton(inputs);
    updateOutputs();
    if (configuration_valid_) {
        const std::uint32_t faults_before_sample = state_.faults;
        sampleFlow(inputs);
        if (state_.faults != faults_before_sample) {
            updateOutputs();
        }
    }
    return state_;
}

const char* phaseName(Phase phase) noexcept {
    switch (phase) {
    case Phase::Idle: return "IDLE";
    case Phase::OpeningValves: return "OPENING_VALVES";
    case Phase::PrimingSource: return "PRIMING_SOURCE";
    case Phase::Running: return "RUNNING";
    case Phase::Faulted: return "FAULTED";
    }
    return "INVALID_PHASE";
}

const char* severityName(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info: return "INFO";
    case Severity::Warning: return "WARN";
    case Severity::Error: return "ERROR";
    }
    return "INVALID_LEVEL";
}

const char* eventName(EventCode code) noexcept {
    switch (code) {
    case EventCode::Boot: return "BOOT";
    case EventCode::NetworkOnline: return "NETWORK_ONLINE";
    case EventCode::NetworkOffline: return "NETWORK_OFFLINE";
    case EventCode::RequestOn: return "REQUEST_ON";
    case EventCode::RequestOff: return "REQUEST_OFF";
    case EventCode::RejectFull: return "REJECT_TANK_INHIBIT";
    case EventCode::RejectSource: return "REJECT_SOURCE_INHIBIT";
    case EventCode::RejectIsolated: return "REJECT_ISOLATED";
    case EventCode::RejectFault: return "REJECT_LATCHED_FAULT";
    case EventCode::RejectOffline: return "REJECT_OFFLINE";
    case EventCode::RejectExpired: return "REJECT_EXPIRED";
    case EventCode::RejectReset: return "REJECT_UNSAFE_ACK";
    case EventCode::TankPermit: return "TANK_PERMIT_CHANGED";
    case EventCode::SourcePermit: return "SOURCE_PERMIT_CHANGED";
    case EventCode::RemoteCancelled: return "REMOTE_DEMAND_CANCELLED";
    case EventCode::IsolationChanged: return "ISOLATION_POLICY_CHANGED";
    case EventCode::ValveOpened: return "VALVE_OPEN_COMMANDED";
    case EventCode::ValveClosed: return "VALVE_CLOSE_COMMANDED";
    case EventCode::PumpStarted: return "PUMP_ON_COMMANDED";
    case EventCode::PumpStopped: return "PUMP_OFF_COMMANDED";
    case EventCode::FaultUnaccountedFlow: return "FAULT_UNACCOUNTED_FLOW";
    case EventCode::FaultLowFlow: return "FAULT_LOW_BRANCH_FLOW";
    case EventCode::FaultMeterMismatch: return "FAULT_METER_MISMATCH";
    case EventCode::FaultSourceUnsafe: return "FAULT_SOURCE_UNSAFE";
    case EventCode::FaultControlDeadline: return "FAULT_CONTROL_DEADLINE";
    case EventCode::FaultConfiguration: return "FAULT_CONFIGURATION";
    case EventCode::FaultsCleared: return "FAULTS_ACKNOWLEDGED";
    }
    return "INVALID_EVENT";
}

const char* subjectName(Subject subject) noexcept {
    switch (subject) {
    case Subject::Controller: return "CONTROLLER";
    case Subject::Source: return "SOURCE";
    case Subject::House1: return "HOUSE_1";
    case Subject::House2: return "HOUSE_2";
    case Subject::Valve1: return "VALVE_1";
    case Subject::Valve2: return "VALVE_2";
    case Subject::Network: return "NETWORK";
    }
    return "INVALID_SUBJECT";
}

const char* eventUnit(EventCode code) noexcept {
    switch (code) {
    case EventCode::FaultUnaccountedFlow:
    case EventCode::FaultLowFlow:
    case EventCode::FaultMeterMismatch: return "mL/min";
    case EventCode::FaultControlDeadline:
    case EventCode::RejectExpired: return "ms";
    case EventCode::RejectFault: return "fault_bits";
    default: return "state";
    }
}

const char* faultName(Fault fault) noexcept {
    switch (fault) {
    case NoFault: return "NONE";
    case UnaccountedFlow: return "UNACCOUNTED_FLOW";
    case House1LowFlow: return "HOUSE_1_LOW_FLOW";
    case House2LowFlow: return "HOUSE_2_LOW_FLOW";
    case MeterMismatch: return "METER_MISMATCH";
    case SourceUnsafe: return "SOURCE_UNSAFE";
    case ControlDeadlineMissed: return "CONTROL_DEADLINE_MISSED";
    case InvalidConfiguration: return "INVALID_CONFIGURATION";
    }
    return "INVALID_FAULT";
}

std::size_t formatEvent(const Event& event, char* buffer, std::size_t capacity) noexcept {
    if (capacity == 0) {
        return 0;
    }
    const int length = std::snprintf(buffer, capacity,
        "t_ms=%" PRIu32 " seq=%" PRIu32 " level=%s code=%s subject=%s value=%" PRId32 " unit=%s\n",
        event.time_ms, event.sequence, severityName(event.severity), eventName(event.code),
        subjectName(event.subject), event.value, eventUnit(event.code));
    if (length < 0 || static_cast<std::size_t>(length) >= capacity) {
        buffer[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(length);
}

}  // namespace water
