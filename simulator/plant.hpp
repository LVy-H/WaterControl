#pragma once

#include "water/controller.hpp"

#include <array>
#include <cstdint>

namespace water::sim {

struct InjectedFaults {
    bool main_leak = false;
    bool blockage1 = false;
    bool blockage2 = false;
    bool main_meter_stuck = false;
    bool tank1_wire_open = false;
    bool tank2_wire_open = false;
    bool source_wire_open = false;
    bool downstream_leak1 = false;
};

// An illustrative volume/pulse model, not a calibrated pressure/pipe solver.
class Plant {
public:
    Plant() noexcept;
    void reset() noexcept;
    void advance(std::uint32_t duration_ms) noexcept;
    void command(CommandKind kind, Origin origin = Origin::Remote) noexcept;
    void pressButton() noexcept;
    void setHouse1Switch(bool on) noexcept;
    void setNetwork(bool online) noexcept;
    void setTankPercent(std::size_t house, double percent) noexcept;
    void setSourcePercent(double percent) noexcept;
    bool setFault(const char* name, bool enabled) noexcept;

    const Controller& controller() const noexcept { return controller_; }
    const Inputs& inputs() const noexcept { return inputs_; }
    const InjectedFaults& faults() const noexcept { return faults_; }
    const std::array<float, kHouseCount>& deliveredLpm() const noexcept { return delivered_lpm_; }
    double tankPercent(std::size_t house) const noexcept;
    double sourcePercent() const noexcept;
private:
    Controller controller_{};
    Inputs inputs_{};
    InjectedFaults faults_{};
    std::array<double, kHouseCount> tank_litres_{{0.4, 0.4}};
    double source_litres_ = 20.0;
    std::array<double, kMeterCount> fractional_pulses_{{0.0, 0.0, 0.0}};
    std::array<float, kHouseCount> delivered_lpm_{{0.0F, 0.0F}};
    void tick(std::uint32_t elapsed_ms) noexcept;
    void refreshPermits() noexcept;
};

}  // namespace water::sim
