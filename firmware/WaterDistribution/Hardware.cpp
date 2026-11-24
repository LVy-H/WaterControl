#include <Arduino.h>
#include "Hardware.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "src/water/controller.hpp"

#ifndef WATER_ENABLE_CLOUD
#define WATER_ENABLE_CLOUD 0
#endif
#if WATER_ENABLE_CLOUD
#include "water_cloud_credentials.hpp"
#define BLYNK_HEARTBEAT 1
#define BLYNK_TIMEOUT_MS 1000
#include <BlynkSimpleEsp32_SSL.h>
#include <WiFi.h>
#endif

namespace {
constexpr uint32_t kControlPeriodMs = 20;
constexpr uint32_t kHeartbeatTtlMs = 2000;
constexpr uint32_t kCommandTtlMs = 2000;
constexpr uint8_t kMeters[] = {27, 26, 32};
constexpr uint8_t kTankPermit[] = {36, 33};
constexpr uint8_t kSourcePermit = 35;
constexpr uint8_t kRequest1 = 34;
constexpr uint8_t kButton2 = 14;
constexpr uint8_t kSourcePump = 13;
constexpr uint8_t kHousePumps[] = {17, 23};
constexpr uint8_t kValves[] = {25, 4};
constexpr uint8_t kGreen = 18;
constexpr uint8_t kRed = 16;
constexpr uint8_t kBuzzer = 19;
// Demonstration drivers are active HIGH. Verify each real driver's OFF level.
constexpr bool kSourceActiveHigh = true;
constexpr bool kHouseActiveHigh[] = {true, true};
constexpr bool kValveActiveHigh[] = {true, true};

struct NetworkHealth {
    uint32_t heartbeat_ms = 0;
    uint32_t session = 0;
    bool online = false;
};
struct QueuedCommand {
    water::Command command{};
    uint32_t session = 0;
};
struct Counters {
    uint32_t command_drops = 0;
    uint32_t stale_remote_commands = 0;
    uint32_t log_queue_drops = 0;
    uint32_t missed_ring_events = 0;
    uint32_t notification_drops = 0;
    uint32_t format_failures = 0;
    uint32_t serial_input_errors = 0;
    uint32_t startup_error = 0;
};
struct Telemetry {
    Telemetry() = default;
    Telemetry(const water::Snapshot& snapshot, uint32_t overwrites)
        : state(snapshot), ring_overwrites(overwrites) {}
    water::Snapshot state{};
    uint32_t ring_overwrites = 0;
};
struct FaultNotice {
    FaultNotice() = default;
    FaultNotice(uint32_t timestamp, uint32_t faults, uint32_t newly_added)
        : time_ms(timestamp), fault_mask(faults), newly_latched(newly_added) {}
    uint32_t time_ms = 0;
    uint32_t fault_mask = 0;
    uint32_t newly_latched = 0;
};

portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pulseTotals[3] = {0, 0, 0};
portMUX_TYPE sharedMux = portMUX_INITIALIZER_UNLOCKED;
NetworkHealth networkHealth{};
Counters counters{};

void increment(uint32_t Counters::*field, uint32_t amount = 1) {
    portENTER_CRITICAL(&sharedMux);
    counters.*field += amount;
    portEXIT_CRITICAL(&sharedMux);
}
NetworkHealth readNetworkHealth() {
    portENTER_CRITICAL(&sharedMux);
    const NetworkHealth copy = networkHealth;
    portEXIT_CRITICAL(&sharedMux);
    return copy;
}
#if WATER_ENABLE_CLOUD
void publishNetworkHealth(bool online) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&sharedMux);
    if (networkHealth.online != online) ++networkHealth.session;
    networkHealth.online = online;
    networkHealth.heartbeat_ms = now;
    portEXIT_CRITICAL(&sharedMux);
}
#endif

StaticQueue_t commandQueueControl, logQueueControl, ioMailboxControl;
uint8_t commandStorage[16 * sizeof(QueuedCommand)];
uint8_t logStorage[64 * sizeof(water::Event)];
uint8_t ioStorage[sizeof(Telemetry)];
QueueHandle_t commandQueue = nullptr;
QueueHandle_t logQueue = nullptr;
QueueHandle_t ioMailbox = nullptr;
#if WATER_ENABLE_CLOUD
StaticQueue_t cloudMailboxControl;
uint8_t cloudStorage[sizeof(Telemetry)];
QueueHandle_t cloudMailbox = nullptr;
StaticQueue_t noticeQueueControl;
uint8_t noticeStorage[8 * sizeof(FaultNotice)];
QueueHandle_t noticeQueue = nullptr;
#endif
StaticTask_t controlTaskControl, ioTaskControl;
StackType_t controlStack[2048], ioStack[2048];
#if WATER_ENABLE_CLOUD
StaticTask_t cloudTaskControl;
StackType_t cloudStack[6144];
#endif

void IRAM_ATTR mainPulse() {
    portENTER_CRITICAL_ISR(&pulseMux);
    ++pulseTotals[0];
    portEXIT_CRITICAL_ISR(&pulseMux);
}
void IRAM_ATTR house1Pulse() {
    portENTER_CRITICAL_ISR(&pulseMux);
    ++pulseTotals[1];
    portEXIT_CRITICAL_ISR(&pulseMux);
}
void IRAM_ATTR house2Pulse() {
    portENTER_CRITICAL_ISR(&pulseMux);
    ++pulseTotals[2];
    portEXIT_CRITICAL_ISR(&pulseMux);
}

void initializeOutput(uint8_t pin, bool activeHigh) {
    // Set the output latch before enabling the driver. External bias is still mandatory.
    gpio_set_level(static_cast<gpio_num_t>(pin), activeHigh ? 0 : 1);
    gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
}
void writeOutput(uint8_t pin, bool on, bool activeHigh) {
    gpio_set_level(static_cast<gpio_num_t>(pin), on == activeHigh ? 1 : 0);
}
void initializePins() {
    initializeOutput(kSourcePump, kSourceActiveHigh);
    for (size_t i = 0; i < 2; ++i) {
        initializeOutput(kHousePumps[i], kHouseActiveHigh[i]);
        initializeOutput(kValves[i], kValveActiveHigh[i]);
    }
    initializeOutput(kGreen, true);
    initializeOutput(kRed, true);
    initializeOutput(kBuzzer, true);
    pinMode(kTankPermit[0], INPUT);  // External 10 kohm pull-up to 3.3 V.
    pinMode(kTankPermit[1], INPUT_PULLUP);
    pinMode(kSourcePermit, INPUT);  // External 10 kohm pull-up to 3.3 V.
    pinMode(kRequest1, INPUT);      // External 10 kohm pull-down to ground.
    pinMode(kButton2, INPUT_PULLUP);
    for (const auto pin : kMeters) pinMode(pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(kMeters[0]), mainPulse, RISING);
    attachInterrupt(digitalPinToInterrupt(kMeters[1]), house1Pulse, RISING);
    attachInterrupt(digitalPinToInterrupt(kMeters[2]), house2Pulse, RISING);
}

void applyOutputs(const water::Outputs& next, water::Outputs& previous) {
    // Stop affected pumps first; never pulse an unchanged output.
    for (size_t i = 0; i < 2; ++i) {
        if (previous.house_pump[i] && !next.house_pump[i])
            writeOutput(kHousePumps[i], false, kHouseActiveHigh[i]);
    }
    if (previous.source_pump && !next.source_pump)
        writeOutput(kSourcePump, false, kSourceActiveHigh);
    // Valve sequencing delays belong only to the shared controller.
    for (size_t i = 0; i < 2; ++i) {
        if (!previous.valve_open[i] && next.valve_open[i])
            writeOutput(kValves[i], true, kValveActiveHigh[i]);
    }
    for (size_t i = 0; i < 2; ++i) {
        if (previous.valve_open[i] && !next.valve_open[i])
            writeOutput(kValves[i], false, kValveActiveHigh[i]);
    }
    if (!previous.source_pump && next.source_pump)
        writeOutput(kSourcePump, true, kSourceActiveHigh);
    for (size_t i = 0; i < 2; ++i) {
        if (!previous.house_pump[i] && next.house_pump[i])
            writeOutput(kHousePumps[i], true, kHouseActiveHigh[i]);
    }
    if (previous.alarm != next.alarm) {
        writeOutput(kRed, next.alarm, true);
        writeOutput(kBuzzer, next.alarm, true);
    }
    if (previous.source_pump != next.source_pump)
        writeOutput(kGreen, next.source_pump, true);
    previous = next;
}

void enqueueCommand(water::CommandKind kind, water::Origin origin) {
    QueuedCommand item;
    item.command = {kind, origin, millis()};
    if (origin == water::Origin::Remote) {
        const auto health = readNetworkHealth();
        if (!health.online || uint32_t(item.command.issued_at_ms - health.heartbeat_ms) > kHeartbeatTtlMs) {
            increment(&Counters::stale_remote_commands);
            return;
        }
        item.session = health.session;
    }
    if (xQueueSend(commandQueue, &item, 0) != pdTRUE) increment(&Counters::command_drops);
}

void controlTask(void*) {
    // IDF 4.4 API: timeout in seconds, panic enabled. Never use the IDF 5 struct API here.
    if (esp_task_wdt_init(2, true) != ESP_OK || esp_task_wdt_add(nullptr) != ESP_OK) {
        increment(&Counters::startup_error, 1);
        vTaskDelete(nullptr);  // Outputs remain at their initialized OFF levels.
    }
    water::Controller controller;
    water::Outputs previous;
    uint32_t lastEventSequence = 0;
#if WATER_ENABLE_CLOUD
    uint32_t previousFaults = 0;
#endif
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        water::Inputs input;
        input.now_ms = millis();
        portENTER_CRITICAL(&pulseMux);
        for (size_t i = 0; i < 3; ++i) input.pulse_total[i] = pulseTotals[i];
        portEXIT_CRITICAL(&pulseMux);
        input.tank_permit[0] = digitalRead(kTankPermit[0]) == LOW;
        input.tank_permit[1] = digitalRead(kTankPermit[1]) == LOW;
        input.source_permit = digitalRead(kSourcePermit) == LOW;
        input.house1_switch = digitalRead(kRequest1) == HIGH;
        input.button2_pressed = digitalRead(kButton2) == LOW;
        const auto health = readNetworkHealth();
        input.network_online = health.online && uint32_t(input.now_ms - health.heartbeat_ms) <= kHeartbeatTtlMs;
        QueuedCommand queued;
        if (xQueueReceive(commandQueue, &queued, 0) == pdTRUE) {
            const bool remote = queued.command.origin == water::Origin::Remote;
            if (remote && (!input.network_online || queued.session != health.session ||
                           uint32_t(input.now_ms - queued.command.issued_at_ms) > kCommandTtlMs)) {
                increment(&Counters::stale_remote_commands);
            } else {
                input.command = queued.command;
            }
        }
        const auto& state = controller.update(input);
        applyOutputs(state.outputs, previous);
        const auto& events = controller.events();
        // Transfer each new event once, even if the UART consumer is stalled.
        bool firstNew = true;
        for (size_t i = 0; i < events.size() && events.newestSequence() != lastEventSequence; ++i) {
            const auto& event = events.at(i);
            const uint32_t distance = event.sequence - lastEventSequence;
            if (distance == 0 || distance >= 0x80000000U) continue;
            if (firstNew && distance > 1) increment(&Counters::missed_ring_events, distance - 1);
            firstNew = false;
            if (xQueueSend(logQueue, &event, 0) != pdTRUE) increment(&Counters::log_queue_drops);
            lastEventSequence = event.sequence;
        }
        const Telemetry telemetry{state, events.overwritten()};
        xQueueOverwrite(ioMailbox, &telemetry);
#if WATER_ENABLE_CLOUD
        xQueueOverwrite(cloudMailbox, &telemetry);
        const uint32_t newFaults = state.faults & ~previousFaults;
        if (newFaults != 0) {
            const FaultNotice notice{input.now_ms, state.faults, newFaults};
            if (xQueueSend(noticeQueue, &notice, 0) != pdTRUE) increment(&Counters::notification_drops);
        }
        previousFaults = state.faults;
#endif
        if (esp_task_wdt_reset() != ESP_OK) {
            water::Outputs off;
            off.alarm = true;
            applyOutputs(off, previous);
            increment(&Counters::startup_error, 2);
            vTaskDelete(nullptr);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kControlPeriodMs));
    }
}

void updateLcd(LiquidCrystal_I2C& lcd, const Telemetry& telemetry) {
    char line[17];
    const auto& state = telemetry.state;
    snprintf(line, sizeof(line), "%-16.16s", water::phaseName(state.phase));
    lcd.setCursor(0, 0);
    lcd.print(line);
    snprintf(line, sizeof(line), "P:%u%u%u F:%03lx     ",
             unsigned(state.outputs.source_pump), unsigned(state.outputs.house_pump[0]),
             unsigned(state.outputs.house_pump[1]), static_cast<unsigned long>(state.faults));
    lcd.setCursor(0, 1);
    lcd.print(line);
}

void ioTask(void*) {
    Serial.begin(115200);
    Wire.begin(21, 22);
    Wire.setTimeOut(20);
    LiquidCrystal_I2C lcd(0x27, 16, 2);
    lcd.init();
    lcd.backlight();
    Telemetry telemetry;
    uint32_t lastLcd = 0, lastStats = 0;
    char pending[512];
    size_t pendingSize = 0, pendingOffset = 0;
    char command[12];
    size_t commandSize = 0;
    bool discardCommand = false;
    for (;;) {
        const uint32_t now = millis();
        xQueuePeek(ioMailbox, &telemetry, 0);
        if (uint32_t(now - lastLcd) >= 500) {
            updateLcd(lcd, telemetry);
            lastLcd = now;
        }
        // Local acknowledgement is available with the cloud disabled: send "ack" + LF.
        for (unsigned i = 0; i < 16 && Serial.available() > 0; ++i) {
            const char ch = static_cast<char>(Serial.read());
            if (ch == '\r') continue;
            if (ch == '\n') {
                command[commandSize] = '\0';
                if (!discardCommand && strcmp(command, "ack") == 0)
                    enqueueCommand(water::CommandKind::ResetFaults, water::Origin::Local);
                else if (discardCommand || commandSize != 0)
                    increment(&Counters::serial_input_errors);
                commandSize = 0;
                discardCommand = false;
            } else if (commandSize + 1 < sizeof(command) && !discardCommand) {
                command[commandSize++] = ch;
            } else {
                discardCommand = true;
            }
        }
        if (pendingOffset == pendingSize) {
            pendingSize = pendingOffset = 0;
            if (uint32_t(now - lastStats) >= 5000) {
                portENTER_CRITICAL(&sharedMux);
                const Counters copy = counters;
                portEXIT_CRITICAL(&sharedMux);
                const int count = snprintf(pending, sizeof(pending),
                    "t_ms=%lu level=INFO code=ADAPTER_STATS command_drops=%lu stale_remote_commands=%lu "
                    "log_queue_drops=%lu missed_ring_events=%lu ring_overwrites=%lu suppressed_logs=%lu "
                    "notification_drops=%lu format_failures=%lu serial_input_errors=%lu startup_error=%lu\n",
                    static_cast<unsigned long>(now), static_cast<unsigned long>(copy.command_drops),
                    static_cast<unsigned long>(copy.stale_remote_commands), static_cast<unsigned long>(copy.log_queue_drops),
                    static_cast<unsigned long>(copy.missed_ring_events), static_cast<unsigned long>(telemetry.ring_overwrites),
                    static_cast<unsigned long>(telemetry.state.suppressed_logs), static_cast<unsigned long>(copy.notification_drops),
                    static_cast<unsigned long>(copy.format_failures), static_cast<unsigned long>(copy.serial_input_errors),
                    static_cast<unsigned long>(copy.startup_error));
                if (count > 0 && static_cast<size_t>(count) < sizeof(pending)) pendingSize = static_cast<size_t>(count);
                else increment(&Counters::format_failures);
                lastStats = now;
            } else {
                water::Event event;
                if (xQueueReceive(logQueue, &event, 0) == pdTRUE) {
                    pendingSize = water::formatEvent(event, pending, sizeof(pending) - 1);
                    if (pendingSize == 0) increment(&Counters::format_failures);
                    else if (pending[pendingSize - 1] != '\n') pending[pendingSize++] = '\n';
                }
            }
        }
        const int available = Serial.availableForWrite();
        if (available > 0 && pendingOffset < pendingSize) {
            size_t count = pendingSize - pendingOffset;
            if (count > static_cast<size_t>(available)) count = static_cast<size_t>(available);
            if (count > 64) count = 64;
            pendingOffset += Serial.write(reinterpret_cast<const uint8_t*>(pending + pendingOffset), count);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

#if WATER_ENABLE_CLOUD
bool publishActualOnReconnect = false;

void publishVirtualPin(uint8_t pin, const water::Snapshot& state) {
    switch (pin) {
        case 0: case 1: case 2: Blynk.virtualWrite(pin, state.flow_lpm[pin]); break;
        case 3: Blynk.virtualWrite(pin, !state.tank_permit[1]); break;
        case 4: Blynk.virtualWrite(pin, bool(state.faults & water::UnaccountedFlow)); break;
        case 5: Blynk.virtualWrite(pin, bool(state.faults & (water::House1LowFlow | water::House2LowFlow))); break;
        case 6: Blynk.virtualWrite(pin, state.outputs.house_pump[1]); break;
        case 7: Blynk.virtualWrite(pin, bool(state.faults & water::House1LowFlow)); break;
        case 8: Blynk.virtualWrite(pin, bool(state.faults & water::House2LowFlow)); break;
        case 9: Blynk.virtualWrite(pin, bool(state.faults & water::MeterMismatch)); break;
        case 10: Blynk.virtualWrite(pin, 0); break;
        case 11: case 12: Blynk.virtualWrite(pin, state.isolated[pin - 11]); break;
        case 13: Blynk.virtualWrite(pin, state.outputs.house_pump[0]); break;
        case 14: Blynk.virtualWrite(pin, state.outputs.source_pump); break;
        case 15: Blynk.virtualWrite(pin, !state.source_permit); break;
        case 16: Blynk.virtualWrite(pin, !state.tank_permit[0]); break;
        case 17: Blynk.virtualWrite(pin, state.faults); break;
    }
}

void cloudTask(void*) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WATER_WIFI_SSID, WATER_WIFI_PASSWORD);
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Blynk.config(BLYNK_AUTH_TOKEN);
    Telemetry telemetry;
    const uint8_t order[] = {6, 11, 12, 10, 13, 14, 17, 15, 16, 3, 4, 5, 7, 8, 9, 0, 1, 2};
    size_t nextPin = 0;
    uint32_t lastPublish = millis(), lastConnect = millis() - 5000U;
    uint32_t lastNotice = millis() - 1000U;
    for (;;) {
        uint32_t now = millis();
        xQueuePeek(cloudMailbox, &telemetry, 0);
        if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) {
            publishNetworkHealth(false);
            if (Blynk.connected()) Blynk.disconnect();
        } else if (!Blynk.connected()) {
            publishNetworkHealth(false);
            if (uint32_t(now - lastConnect) >= 5000) {
                lastConnect = now;
                // TLS/DNS may exceed this protocol timeout; the control owner never waits here.
                Blynk.connect(500);
            }
        }
        if (Blynk.connected()) {
            Blynk.run();
            publishNetworkHealth(Blynk.connected() && WiFi.status() == WL_CONNECTED);
        }
        now = millis();
        if (publishActualOnReconnect) {
            nextPin = 0;
            publishActualOnReconnect = false;
        }
        if (Blynk.connected() && uint32_t(now - telemetry.state.time_ms) <= kHeartbeatTtlMs &&
            uint32_t(now - lastPublish) >= 200) {
            publishVirtualPin(order[nextPin], telemetry.state);
            nextPin = (nextPin + 1) % sizeof(order);
            lastPublish = now;
        }
        FaultNotice notice;
        if (xQueuePeek(noticeQueue, &notice, 0) == pdTRUE) {
            if (!Blynk.connected() || uint32_t(now - notice.time_ms) > 10000) {
                xQueueReceive(noticeQueue, &notice, 0);
                increment(&Counters::notification_drops);
            } else if (uint32_t(now - lastNotice) >= 1000) {
                xQueueReceive(noticeQueue, &notice, 0);
                char message[128];
                snprintf(message, sizeof(message), "Water shutdown: time_ms=%lu fault_mask=%lu newly_latched=%lu",
                         static_cast<unsigned long>(notice.time_ms), static_cast<unsigned long>(notice.fault_mask),
                         static_cast<unsigned long>(notice.newly_latched));
                Blynk.logEvent("water_fault", message);
                lastNotice = now;
            }
        }
        if (!Blynk.connected()) publishNetworkHealth(false);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif
}  // namespace

#if WATER_ENABLE_CLOUD
BLYNK_CONNECTED() {
    // Authentication succeeded: authorize this session before its first command callback.
    publishNetworkHealth(WiFi.status() == WL_CONNECTED);
    // Publish current output commands; never synchronize historical commands.
    publishActualOnReconnect = true;
}
BLYNK_WRITE(V6) {
    enqueueCommand(param.asInt() == 1 ? water::CommandKind::Pump2On : water::CommandKind::Pump2Off,
                   water::Origin::Remote);
}
BLYNK_WRITE(V10) {
    if (param.asInt() == 1) enqueueCommand(water::CommandKind::ResetFaults, water::Origin::Remote);
}
BLYNK_WRITE(V11) {
    enqueueCommand(param.asInt() == 1 ? water::CommandKind::IsolateHouse1 : water::CommandKind::AllowHouse1,
                   water::Origin::Remote);
}
BLYNK_WRITE(V12) {
    enqueueCommand(param.asInt() == 1 ? water::CommandKind::IsolateHouse2 : water::CommandKind::AllowHouse2,
                   water::Origin::Remote);
}
#endif

void water::hardware::begin() {
    initializePins();
    commandQueue = xQueueCreateStatic(16, sizeof(QueuedCommand), commandStorage, &commandQueueControl);
    logQueue = xQueueCreateStatic(64, sizeof(water::Event), logStorage, &logQueueControl);
    ioMailbox = xQueueCreateStatic(1, sizeof(Telemetry), ioStorage, &ioMailboxControl);
#if WATER_ENABLE_CLOUD
    cloudMailbox = xQueueCreateStatic(1, sizeof(Telemetry), cloudStorage, &cloudMailboxControl);
    noticeQueue = xQueueCreateStatic(8, sizeof(FaultNotice), noticeStorage, &noticeQueueControl);
#endif
    // ESP-IDF's stack depth parameter is bytes, unlike upstream FreeRTOS words.
    const auto io = xTaskCreateStaticPinnedToCore(ioTask, "water-io", sizeof(ioStack), nullptr, 1,
                                                ioStack, &ioTaskControl, 0);
    if (!io) {
        increment(&Counters::startup_error, 4);
        return;
    }
    const auto control = xTaskCreateStaticPinnedToCore(controlTask, "water-control", sizeof(controlStack), nullptr, 4,
                                                     controlStack, &controlTaskControl, 1);
    if (!control) {
        increment(&Counters::startup_error, 8);
        return;
    }
#if WATER_ENABLE_CLOUD
    if (!xTaskCreateStaticPinnedToCore(cloudTask, "water-cloud", sizeof(cloudStack), nullptr, 1,
                                      cloudStack, &cloudTaskControl, 0))
        increment(&Counters::startup_error, 16);
#endif
}

