#include "plant.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>

namespace {
using water::sim::Plant;

void printBoolean(bool value) {
    std::cout << (value ? "true" : "false");
}

template <typename T, std::size_t N>
void printArray(const std::array<T, N>& values) {
    std::cout << '[';
    for (std::size_t index = 0; index < N; ++index) {
        if (index != 0) std::cout << ',';
        if constexpr (std::is_same_v<T, bool>) printBoolean(values[index]);
        else std::cout << values[index];
    }
    std::cout << ']';
}

void printState(const Plant& plant) {
    const auto& controller = plant.controller();
    const auto& state = controller.snapshot();
    const auto& faults = plant.faults();
    const auto& log = controller.events();
    std::cout << std::fixed << std::setprecision(4)
              << "{\"time_ms\":" << state.time_ms << ",\"phase\":\"" << water::phaseName(state.phase)
              << "\",\"network_online\":";
    printBoolean(state.network_online);
    std::cout << ",\"requested\":"; printArray(state.requested);
    std::cout << ",\"isolated\":"; printArray(state.isolated);
    std::cout << ",\"tank_permit\":"; printArray(state.tank_permit);
    std::cout << ",\"source_permit\":"; printBoolean(state.source_permit);
    std::cout << ",\"outputs\":{\"source_pump\":"; printBoolean(state.outputs.source_pump);
    std::cout << ",\"house_pump\":"; printArray(state.outputs.house_pump);
    std::cout << ",\"valve_open\":"; printArray(state.outputs.valve_open);
    std::cout << ",\"alarm\":"; printBoolean(state.outputs.alarm);
    std::cout << "},\"flow_lpm\":"; printArray(state.flow_lpm);
    std::cout << ",\"flow_valid\":"; printBoolean(state.flow_valid);
    std::cout << ",\"warning\":"; printBoolean(state.warning);
    std::cout << ",\"fault_mask\":" << state.faults << ",\"faults\":[";
    bool first_fault = true;
    for (std::uint32_t bit = 1; bit <= water::InvalidConfiguration; bit <<= 1U) {
        if ((state.faults & bit) != 0) {
            if (!first_fault) std::cout << ',';
            std::cout << '"' << water::faultName(static_cast<water::Fault>(bit)) << '"';
            first_fault = false;
        }
    }
    std::cout << "],\"tank_percent\":[" << plant.tankPercent(0) << ',' << plant.tankPercent(1)
              << "],\"source_percent\":" << plant.sourcePercent() << ",\"delivered_lpm\":";
    printArray(plant.deliveredLpm());
    std::cout << ",\"injected_faults\":{";
    const std::array<const char*, 8> fault_names{{"main_leak", "blockage1", "blockage2", "main_meter_stuck",
        "tank1_wire_open", "tank2_wire_open", "source_wire_open", "downstream_leak1"}};
    const std::array<bool, 8> fault_values{{faults.main_leak, faults.blockage1, faults.blockage2,
        faults.main_meter_stuck, faults.tank1_wire_open, faults.tank2_wire_open,
        faults.source_wire_open, faults.downstream_leak1}};
    for (std::size_t index = 0; index < fault_names.size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout << '"' << fault_names[index] << "\":";
        printBoolean(fault_values[index]);
    }
    std::cout << "},\"house1_switch\":"; printBoolean(plant.inputs().house1_switch);
    std::cout << ",\"rejected_commands\":" << state.rejected_commands
              << ",\"suppressed_logs\":" << state.suppressed_logs
              << ",\"log_overwrites\":" << log.overwritten()
              << ",\"log_capacity\":" << water::kLogCapacity << ",\"logs\":[";
    for (std::size_t index = 0; index < log.size(); ++index) {
        const auto& event = log.at(index);
        if (index != 0) std::cout << ',';
        std::cout << "{\"sequence\":" << event.sequence << ",\"time_ms\":" << event.time_ms
                  << ",\"level\":\"" << water::severityName(event.severity)
                  << "\",\"code\":\"" << water::eventName(event.code)
                  << "\",\"subject\":\"" << water::subjectName(event.subject)
                  << "\",\"value\":" << event.value << ",\"unit\":\""
                  << water::eventUnit(event.code) << "\"}";
    }
    std::cout << "]}\n" << std::flush;
}

bool finished(std::istringstream& stream) {
    stream >> std::ws;
    return stream.eof();
}

bool booleanArgument(std::istringstream& stream, bool& value) {
    int number = -1;
    if (!(stream >> number) || (number != 0 && number != 1) || !finished(stream)) return false;
    value = number == 1;
    return true;
}

bool percentageArgument(std::istringstream& stream, double& value) {
    return static_cast<bool>(stream >> value) && std::isfinite(value) && value >= 0.0 && value <= 100.0 && finished(stream);
}

bool execute(Plant& plant, const std::string& line) {
    std::istringstream stream(line);
    std::string command;
    stream >> command;
    if (command == "state" || command == "reset" || command == "press" || command == "ack") {
        if (!finished(stream)) return false;
        if (command == "reset") plant.reset();
        else if (command == "press") plant.pressButton();
        else if (command == "ack") plant.command(water::CommandKind::ResetFaults, water::Origin::Local);
        return true;
    }
    if (command == "advance") {
        long long milliseconds = -1;
        if (!(stream >> milliseconds) || milliseconds < 0 || milliseconds > 60000 || !finished(stream)) return false;
        plant.advance(static_cast<std::uint32_t>(milliseconds));
        return true;
    }
    if (command == "pump2" || command == "house1" || command == "network") {
        bool on = false;
        if (!booleanArgument(stream, on)) return false;
        if (command == "pump2") plant.command(on ? water::CommandKind::Pump2On : water::CommandKind::Pump2Off);
        else if (command == "house1") plant.setHouse1Switch(on);
        else plant.setNetwork(on);
        return true;
    }
    if (command == "isolate") {
        int house = 0;
        bool isolated = false;
        if (!(stream >> house) || (house != 1 && house != 2) || !booleanArgument(stream, isolated)) return false;
        const auto kind = house == 1
            ? (isolated ? water::CommandKind::IsolateHouse1 : water::CommandKind::AllowHouse1)
            : (isolated ? water::CommandKind::IsolateHouse2 : water::CommandKind::AllowHouse2);
        plant.command(kind);
        return true;
    }
    if (command == "fault") {
        std::string name;
        bool enabled = false;
        if (!(stream >> name) || !booleanArgument(stream, enabled)) return false;
        return plant.setFault(name.c_str(), enabled);
    }
    if (command == "tank") {
        int house = 0;
        double percent = 0.0;
        if (!(stream >> house) || (house != 1 && house != 2) || !percentageArgument(stream, percent)) return false;
        plant.setTankPercent(static_cast<std::size_t>(house - 1), percent);
        return true;
    }
    if (command == "source") {
        double percent = 0.0;
        if (!percentageArgument(stream, percent)) return false;
        plant.setSourcePercent(percent);
        return true;
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--stdio")) {
        std::cerr << "Usage: water-sim [--stdio]\nOne command per stdin line; JSON responses on stdout.\n";
        return 2;
    }
    Plant plant;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (execute(plant, line)) printState(plant);
        else std::cout << "{\"error\":\"Invalid simulator command or argument\"}\n" << std::flush;
    }
    return 0;
}
