#include "plant.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace water::sim {
namespace {
constexpr double kTankCapacityLitres = 2.0;
constexpr double kSourceCapacityLitres = 20.0;
constexpr double kTankFullLitres = kTankCapacityLitres * 0.95;
constexpr double kSourceMinimumLitres = 0.05;
constexpr double kPulsesPerLitre = 450.0;
constexpr std::uint32_t kTickMs = 20;
}

Plant::Plant() noexcept {
    reset();
}

void Plant::reset() noexcept {
    controller_ = Controller{};
    inputs_ = Inputs{};
    inputs_.network_online = true;
    faults_ = InjectedFaults{};
    tank_litres_ = {{0.4, 0.4}};
    source_litres_ = kSourceCapacityLitres;
    fractional_pulses_.fill(0.0);
    delivered_lpm_.fill(0.0F);
    refreshPermits();
    controller_.update(inputs_);
}

void Plant::refreshPermits() noexcept {
    inputs_.tank_permit[0] = tank_litres_[0] < kTankFullLitres && !faults_.tank1_wire_open;
    inputs_.tank_permit[1] = tank_litres_[1] < kTankFullLitres && !faults_.tank2_wire_open;
    inputs_.source_permit = source_litres_ > kSourceMinimumLitres && !faults_.source_wire_open;
}

void Plant::tick(std::uint32_t elapsed_ms) noexcept {
    const Outputs& outputs = controller_.snapshot().outputs;
    std::array<double, kHouseCount> branch_flow{{0.0, 0.0}};
    double main_leak_flow = 0.0;
    if (outputs.source_pump && source_litres_ > 0.0) {
        if (outputs.house_pump[0] && outputs.valve_open[0] && !faults_.blockage1) {
            branch_flow[0] = 3.0;
        }
        if (outputs.house_pump[1] && outputs.valve_open[1] && !faults_.blockage2) {
            branch_flow[1] = 4.0;
        }
        main_leak_flow = faults_.main_leak ? 1.5 : 0.0;
    }
    const double minutes = static_cast<double>(elapsed_ms) / 60000.0;
    const double requested_volume = (branch_flow[0] + branch_flow[1] + main_leak_flow) * minutes;
    if (requested_volume > source_litres_) {
        const double scale = source_litres_ / requested_volume;
        branch_flow[0] *= scale;
        branch_flow[1] *= scale;
        main_leak_flow *= scale;
    }
    const double downstream_loss = faults_.downstream_leak1 ? std::min(1.5, branch_flow[0]) : 0.0;
    delivered_lpm_[0] = static_cast<float>(branch_flow[0] - downstream_loss);
    delivered_lpm_[1] = static_cast<float>(branch_flow[1]);
    for (std::size_t house = 0; house < kHouseCount; ++house) {
        tank_litres_[house] = std::min(kTankCapacityLitres,
            tank_litres_[house] + static_cast<double>(delivered_lpm_[house]) * minutes);
    }
    const double main_flow = branch_flow[0] + branch_flow[1] + main_leak_flow;
    source_litres_ = std::max(0.0, source_litres_ - main_flow * minutes);
    const std::array<double, kMeterCount> meter_flow{{
        faults_.main_meter_stuck ? 0.0 : main_flow, branch_flow[0], branch_flow[1]
    }};
    for (std::size_t meter = 0; meter < kMeterCount; ++meter) {
        fractional_pulses_[meter] += meter_flow[meter] * minutes * kPulsesPerLitre;
        const double whole_pulses = std::floor(fractional_pulses_[meter]);
        inputs_.pulse_total[meter] += static_cast<std::uint32_t>(whole_pulses);
        fractional_pulses_[meter] -= whole_pulses;
    }
    inputs_.now_ms += elapsed_ms;
    refreshPermits();
    controller_.update(inputs_);
    inputs_.command = Command{};
}

void Plant::advance(std::uint32_t duration_ms) noexcept {
    while (duration_ms > 0) {
        const std::uint32_t step = std::min(duration_ms, kTickMs);
        tick(step);
        duration_ms -= step;
    }
}

void Plant::command(CommandKind kind, Origin origin) noexcept {
    inputs_.command = Command{kind, origin, inputs_.now_ms};
    advance(kTickMs);
}

void Plant::pressButton() noexcept {
    inputs_.button2_pressed = true;
    advance(80);
    inputs_.button2_pressed = false;
    advance(80);
}

void Plant::setHouse1Switch(bool on) noexcept {
    inputs_.house1_switch = on;
    advance(kTickMs);
}

void Plant::setNetwork(bool online) noexcept {
    inputs_.network_online = online;
    advance(kTickMs);
}

void Plant::setTankPercent(std::size_t house, double percent) noexcept {
    tank_litres_[house] = kTankCapacityLitres * percent / 100.0;
    advance(kTickMs);
}

void Plant::setSourcePercent(double percent) noexcept {
    source_litres_ = kSourceCapacityLitres * percent / 100.0;
    advance(kTickMs);
}

bool Plant::setFault(const char* name, bool enabled) noexcept {
    bool* target = nullptr;
    if (std::strcmp(name, "main_leak") == 0) target = &faults_.main_leak;
    else if (std::strcmp(name, "blockage1") == 0) target = &faults_.blockage1;
    else if (std::strcmp(name, "blockage2") == 0) target = &faults_.blockage2;
    else if (std::strcmp(name, "main_meter_stuck") == 0) target = &faults_.main_meter_stuck;
    else if (std::strcmp(name, "tank1_wire_open") == 0) target = &faults_.tank1_wire_open;
    else if (std::strcmp(name, "tank2_wire_open") == 0) target = &faults_.tank2_wire_open;
    else if (std::strcmp(name, "source_wire_open") == 0) target = &faults_.source_wire_open;
    else if (std::strcmp(name, "downstream_leak1") == 0) target = &faults_.downstream_leak1;
    if (target == nullptr) return false;
    *target = enabled;
    advance(kTickMs);
    return true;
}

double Plant::tankPercent(std::size_t house) const noexcept {
    return tank_litres_[house] * 100.0 / kTankCapacityLitres;
}

double Plant::sourcePercent() const noexcept {
    return source_litres_ * 100.0 / kSourceCapacityLitres;
}

}  // namespace water::sim
