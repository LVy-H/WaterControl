#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace water {

constexpr std::size_t kMeterCount = 3;
constexpr std::size_t kHouseCount = 2;
constexpr std::size_t kLogCapacity = 32;

enum Fault : std::uint32_t {
    NoFault = 0,
    UnaccountedFlow = 1U << 0,
    House1LowFlow = 1U << 1,
    House2LowFlow = 1U << 2,
    MeterMismatch = 1U << 3,
    SourceUnsafe = 1U << 4,
    ControlDeadlineMissed = 1U << 5,
    InvalidConfiguration = 1U << 6
};

enum class Origin : std::uint8_t { Local, Remote };
enum class CommandKind : std::uint8_t {
    None, Pump2Off, Pump2On, IsolateHouse1, AllowHouse1,
    IsolateHouse2, AllowHouse2, ResetFaults
};

struct Command {
    Command() noexcept = default;
    Command(CommandKind command_kind, Origin command_origin, std::uint32_t timestamp) noexcept
        : kind(command_kind), origin(command_origin), issued_at_ms(timestamp) {}
    CommandKind kind = CommandKind::None;
    Origin origin = Origin::Local;
    std::uint32_t issued_at_ms = 0;
};

struct Config {
    // Illustrative coefficients until each physical meter is calibrated.
    std::array<float, kMeterCount> pulses_per_litre{{450.0F, 450.0F, 450.0F}};
    std::uint32_t flow_window_ms = 1000;
    std::uint32_t valve_settle_ms = 300;
    std::uint32_t source_prime_ms = 500;
    std::uint32_t hydraulic_settle_ms = 2000;
    std::uint32_t fault_dwell_ms = 3000;
    std::uint32_t button_debounce_ms = 50;
    std::uint32_t command_ttl_ms = 2000;
    std::uint32_t max_control_gap_ms = 250;
    float minimum_flow_lpm = 0.3F;
    float absolute_balance_tolerance_lpm = 0.4F;
    float relative_balance_tolerance = 0.15F;
};

struct Inputs {
    std::uint32_t now_ms = 0;
    std::array<std::uint32_t, kMeterCount> pulse_total{{0, 0, 0}};
    bool house1_switch = false;
    bool button2_pressed = false;
    // NC healthy contacts: an open contact or broken wire removes permission.
    std::array<bool, kHouseCount> tank_permit{{false, false}};
    bool source_permit = false;
    bool network_online = false;
    Command command{};
};

struct Outputs {
    bool source_pump = false;
    std::array<bool, kHouseCount> house_pump{{false, false}};
    std::array<bool, kHouseCount> valve_open{{false, false}};
    bool alarm = false;
};

enum class Phase : std::uint8_t { Idle, OpeningValves, PrimingSource, Running, Faulted };
enum class Severity : std::uint8_t { Info, Warning, Error };
enum class EventCode : std::uint8_t {
    Boot, NetworkOnline, NetworkOffline, RequestOn, RequestOff,
    RejectFull, RejectSource, RejectIsolated, RejectFault, RejectOffline,
    RejectExpired, RejectReset, TankPermit, SourcePermit, RemoteCancelled,
    IsolationChanged,
    ValveOpened, ValveClosed, PumpStarted, PumpStopped,
    FaultUnaccountedFlow, FaultLowFlow, FaultMeterMismatch, FaultSourceUnsafe,
    FaultControlDeadline, FaultConfiguration, FaultsCleared
};

enum class Subject : std::uint8_t { Controller, Source, House1, House2, Valve1, Valve2, Network };

struct Event {
    Event() noexcept = default;
    Event(std::uint32_t event_sequence, std::uint32_t timestamp, Severity level,
          EventCode event_code, Subject event_subject, std::int32_t event_value) noexcept
        : sequence(event_sequence), time_ms(timestamp), severity(level),
          code(event_code), subject(event_subject), value(event_value) {}
    std::uint32_t sequence = 0;
    std::uint32_t time_ms = 0;
    Severity severity = Severity::Info;
    EventCode code = EventCode::Boot;
    Subject subject = Subject::Controller;
    // Unit is determined by eventUnit(): state, fault_bits, ms, or mL/min.
    std::int32_t value = 0;
};

class EventLog {
public:
    void append(std::uint32_t now_ms, Severity severity, EventCode code,
                Subject subject, std::int32_t value) noexcept;
    const Event& at(std::size_t oldest_index) const noexcept;
    std::size_t size() const noexcept { return count_; }
    std::uint32_t overwritten() const noexcept { return overwritten_; }
    std::uint32_t newestSequence() const noexcept { return sequence_; }
private:
    std::array<Event, kLogCapacity> entries_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    std::uint32_t sequence_ = 0;
    std::uint32_t overwritten_ = 0;
};

struct Snapshot {
    std::uint32_t time_ms = 0;
    std::array<float, kMeterCount> flow_lpm{{0.0F, 0.0F, 0.0F}};
    bool flow_valid = false;
    std::array<bool, kHouseCount> requested{{false, false}};
    std::array<bool, kHouseCount> isolated{{false, false}};
    std::array<bool, kHouseCount> tank_permit{{false, false}};
    bool source_permit = false;
    bool network_online = false;
    Outputs outputs{};
    Phase phase = Phase::Idle;
    std::uint32_t faults = NoFault;
    bool warning = false;
    std::uint32_t rejected_commands = 0;
    std::uint32_t suppressed_logs = 0;
};

// One owner calls update at least every max_control_gap_ms. No I/O or allocation.
// An enabling command never overrides a raw safety input or a latched fault.
class Controller {
public:
    explicit Controller(const Config& config = Config{}) noexcept;
    const Snapshot& update(const Inputs& inputs) noexcept;
    const Snapshot& snapshot() const noexcept { return state_; }
    const EventLog& events() const noexcept { return events_; }
private:
    Config config_;
    Snapshot state_{};
    EventLog events_{};
    bool initialized_ = false;
    bool configuration_valid_ = true;
    bool switch1_armed_ = false;
    bool previous_switch1_ = false;
    bool button_raw_ = false;
    bool button_stable_ = false;
    bool button_armed_ = false;
    Origin pump2_owner_ = Origin::Local;
    std::uint32_t last_update_ms_ = 0;
    std::uint32_t last_flow_ms_ = 0;
    std::uint32_t button_changed_ms_ = 0;
    std::uint32_t last_hydraulic_change_ms_ = 0;
    std::uint32_t source_started_ms_ = 0;
    std::array<std::uint32_t, kMeterCount> previous_pulses_{{0, 0, 0}};
    std::array<std::uint32_t, kHouseCount> valve_opened_ms_{{0, 0}};
    std::array<std::uint32_t, 4> abnormal_elapsed_ms_{{0, 0, 0, 0}};
    bool have_last_rejection_ = false;
    EventCode last_rejection_code_ = EventCode::Boot;
    Subject last_rejection_subject_ = Subject::Controller;
    std::uint32_t last_rejection_ms_ = 0;

    void emit(EventCode code, Subject subject, std::int32_t value = 0,
              Severity severity = Severity::Info) noexcept;
    void reject(EventCode code, Subject subject, std::int32_t value = 0) noexcept;
    void requestPump(std::size_t house, bool on, Origin origin) noexcept;
    void processCommand(const Inputs& inputs) noexcept;
    void updateButton(const Inputs& inputs) noexcept;
    void updateOutputs() noexcept;
    void sampleFlow(const Inputs& inputs) noexcept;
    void latchFault(Fault fault, EventCode code, Subject subject, std::int32_t value) noexcept;
};

const char* phaseName(Phase phase) noexcept;
const char* severityName(Severity severity) noexcept;
const char* eventName(EventCode code) noexcept;
const char* subjectName(Subject subject) noexcept;
const char* eventUnit(EventCode code) noexcept;
const char* faultName(Fault fault) noexcept;
// Bounded ASCII record; returns bytes excluding NUL, or 0 when capacity is too small.
std::size_t formatEvent(const Event& event, char* buffer, std::size_t capacity) noexcept;

}  // namespace water
