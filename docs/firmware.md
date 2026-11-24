# ESP32 firmware and GPIO simulation

Build the offline ESP32 firmware to exercise the shared controller with Wokwi switches, pulse sources, and output LEDs.
The cloud environment adds real Blynk TLS integration; it requires your own account configuration and credentials.
Neither path validates pumps, electrical protection, pressure, valve travel, or water delivery.
No hardware or Wokwi browser run is claimed here.

## Build and run

### Arduino IDE

The complete board project is `firmware/WaterDistribution/`.
Copy that **whole folder**, not only the `.ino` text: Arduino compiles the hardware adapter and the `src/` controller module too.
No simulator files, Python process, web server, or PlatformIO installation are required on the board.

1. Install Arduino IDE 2.
2. Add `https://espressif.github.io/arduino-esp32/package_esp32_index.json` to **Preferences → Additional Boards Manager URLs**.
3. In Boards Manager, install **esp32 by Espressif Systems, version 2.0.17**.
   This firmware targets its IDF 4.4 watchdog interface; do not silently substitute the 3.x board package.
4. In Library Manager, install **LiquidCrystal I2C by Frank de Brabander, version 1.1.2**.
5. Open `firmware/WaterDistribution/WaterDistribution.ino`; choose **Tools → Board → ESP32 Arduino → ESP32 Dev Module**.
6. Press **Verify**. You can compile without a board. Select the actual USB port and **Upload** only when you have the ESP32.
7. Open Serial Monitor at **115200 baud**. Send `ack` with a newline for a local safe fault acknowledgement.

The sketch is for a classic ESP32-WROOM DevKit, not Uno, Nano, ESP32-C3, or ESP32-S3.
Before enabling physical loads, set driver polarity and confirm the pin map in `Hardware.cpp`, and replace `Config::pulses_per_litre` in `src/water/controller.hpp` with separately measured coefficients.
The 450 defaults are illustrative; they do not certify sensor calibration.
The Arduino Library Manager's LCD 1.1.2 package advertises AVR-only metadata, so ESP32 builds emit an architecture warning.
Compilation and physical LCD compatibility are separate checks; verify the display on the actual board.

| File | Responsibility |
| --- | --- |
| `WaterDistribution.ino` | Arduino `setup()` / `loop()` entry points |
| `Hardware.h` / `Hardware.cpp` | Real GPIO, ISR pulse capture, watchdog, ordered outputs, UART/LCD, optional cloud task |
| `src/water/controller.hpp` / `src/controller.cpp` | Pure, allocation-free safety controller shared with the desktop simulator |
| Repository `simulator/` | Desktop-only pulse/volume model, native protocol and web UI; never part of the sketch |

Cloud mode is optional. To use it in Arduino IDE, install **Blynk 1.3.2**, set `WATER_ENABLE_CLOUD` to `1` in `Hardware.cpp`, and create the local sketch header `water_cloud_credentials.hpp` defining `BLYNK_TEMPLATE_ID`, `BLYNK_TEMPLATE_NAME`, `BLYNK_AUTH_TOKEN`, `WATER_WIFI_SSID`, and `WATER_WIFI_PASSWORD` with your own values.
That header is ignored by Git; do not share a credential-bearing sketch or firmware image.
The PlatformIO cloud build creates the same header privately from environment variables instead.

### PlatformIO

From the repository root, with PlatformIO Core 6 installed:

```sh
pio run -e esp32-offline
pio device monitor -b 115200
```

The environment pins Espressif32 6.9.0, Arduino-ESP32 2.0.17 (PlatformIO package 3.20017.0), the ESP32 GCC 8.4.0 toolchain, and LCD 1.1.4.
The cloud environment additionally pins Blynk 1.3.2.
`platformio.ini` builds the same `firmware/WaterDistribution/` Arduino sketch, its hardware adapter, and its pure `src/` controller module.
There is no firmware copy of the controller and no JavaScript safety implementation.
The default environment needs no Wi-Fi, Blynk token, or network service.

For Wokwi for VS Code, build offline, open `diagram.json`, then run **Wokwi: Start Simulator**.
`wokwi.toml` points to `.pio/build/esp32-offline/firmware.bin` and `firmware.elf`.
The application-only image is sufficient here because there is no custom partition scheme, filesystem, or OTA implementation.
The Wokwi extension/CLI may require its own license or token; that is separate from Blynk credentials.

### Configured cloud build

Export these variables in your shell, then build:

```sh
export WATER_BLYNK_TEMPLATE_ID='your-template-id'
export WATER_BLYNK_TEMPLATE_NAME='Water Distribution'
export WATER_BLYNK_AUTH_TOKEN='your-device-token'
export WATER_WIFI_SSID='your-network-name'
export WATER_WIFI_PASSWORD='your-network-password'
pio run -e esp32-cloud
```

For Wokwi Wi-Fi use SSID `Wokwi-GUEST` and an empty password, subject to the simulator's network gateway availability.
To run that build in Wokwi, change both paths in `wokwi.toml` from `esp32-offline` to `esp32-cloud`.
Never put real tokens in source control, build logs, screenshots, or issue reports.
The pre-build script creates `.pio/build/esp32-cloud/water_cloud_credentials.hpp` with owner-only file permissions.
The compiled firmware also contains credentials: treat the entire `.pio/` directory and firmware images as sensitive, and keep `.pio/` ignored by version control.
Missing required variables stop the cloud build with their names, without printing their values.
An empty Wi-Fi password is supported; the other four variables must be nonempty.

The cloud task uses Blynk's ESP32 TLS adapter and its bundled root CA, without disabling certificate validation.
It waits for Wi-Fi and an SNTP clock before connecting; blocked DNS/TLS or unavailable time service does not block control.
The protocol connection timeout is 500 ms, but the library's underlying TLS handshake can take longer.
The control owner's 2-second freshness check cancels remote authority if the network task stops refreshing its status.
A fresh status means the network task last observed Wi-Fi and Blynk protocol connectivity; it is not independent proof of an Internet path or a hard 2-second detection bound for every network failure.
Blynk heartbeat/timeout processing detects silent server loss when the network task is running.

## Hardware pin redesign

This pin map targets the **classic ESP32-WROOM DevKit**, not WROVER/PSRAM variants, ESP32-S3, or ESP32-C3.
GPIO16/17 must be available on the actual module; they are not universally free across ESP32 variants.
All voltages below are logic-side voltages.

| Function | GPIO | Required connection / meaning |
| --- | --- | --- |
| Main / house 1 / house 2 meter | 27 / 26 / 32 | Rising-edge pulses, separately calibrated |
| House 1 tank permit | 36 (`VP` in Wokwi) | External 10 kohm pull-up to 3.3 V; NC healthy contact to ground |
| House 2 tank permit | 33 | Internal pull-up enabled; external 10 kohm pull-up also shown |
| Source-water permit | 35 | External 10 kohm pull-up to 3.3 V; NC healthy contact to ground |
| House 1 maintained request | 34 | External 10 kohm pull-down; low-voltage switch to 3.3 V |
| House 2 momentary request | 14 | Button to ground, internal pull-up, LOW pressed |
| Source pump driver | 13 | **New** dedicated active-HIGH demonstration driver output |
| House 1 pump driver | 17 | **New** driver, replaces the unused yellow LED assignment |
| House 2 pump driver | 23 | Active-HIGH demonstration driver output |
| House 1 / house 2 valve driver | 25 / 4 | HIGH requests open; de-energized hardware must close |
| Running / alarm LED | 18 / 16 | Source pump command / latched alarm |
| Active buzzer driver | 19 | Active-HIGH alarm command, not a passive-piezo tone signal |
| LCD SDA / SCL | 21 / 22 | I2C address 0x27 |

GPIO34–39 have **no internal pull-ups or pull-downs**.
The resistors on GPIO34/35/36 are mandatory, not optional substitutes for `INPUT_PULLUP`.
NC healthy float loops read LOW only while closed and safe.
A full tank, unsafe source, or broken loop wire reads HIGH and removes permission.
A short to ground can falsely report healthy; this simple contact circuit does not detect every wiring fault.
The source float must be installed so that **adequate** source water closes its contact, unlike a tank high-level float that opens when full.

### Electrical work required before using real equipment

- ESP32 inputs are 3.3 V logic. Do not connect a 5 V hall-meter output directly.
  Determine whether each meter output is push-pull or open-collector, then use a suitable level translator or a correctly rated 3.3 V pull-up arrangement permitted by that meter's datasheet.
  Calibrate pulses/litre for each installed meter; the default 450 pulses/litre is illustrative, not a measured specification.
- Never drive a motor, solenoid, relay coil, or high-current buzzer from a GPIO.
  Use appropriately rated transistor/MOSFET/isolated relay drivers, gate/base resistors as needed, and an external OFF-state bias that remains effective during reset and boot.
  Active-LOW relay modules require changing the polarity constants in `firmware/WaterDistribution/Hardware.cpp` and rechecking reset behavior; software initialization alone cannot suppress every boot transient.
- Use separate correctly rated load supplies, fusing, motor protection, coil flyback suppression for DC loads, and appropriate snubbers/surge protection where required.
  Define grounding or galvanic isolation deliberately; do not defeat isolation by blindly joining grounds.
  Mains wiring requires qualified design and installation, not the LED diagram.
- A typical 5 V LCD backpack may pull SDA/SCL to 5 V.
  Use a bidirectional I2C level shifter with correctly placed pull-ups, or a documented 3.3 V-compatible module.
  Wokwi's direct 5 V LCD connection is a digital-simulation shortcut, **not** a safe real-world wiring instruction.
- Confirm valves are normally closed and physically fail closed, and verify valve travel/pressure against the configured 300 ms settling delay.
  Confirm the 500 ms source-priming delay is appropriate; add hardware sensing/interlocks if a timer cannot establish safe operation.
- The watchdog is a software-stall recovery measure, not a safety contactor.
  During a CPU stall, a GPIO can retain its last energized level until reset.
  Use independent hardwired low-water/overfill/emergency-stop interlocks where required by the equipment risk assessment.
  Driver polarity, watchdog timeout/reset outputs, ISR pulse integrity, timing jitter, electrical noise immunity, and hydraulic behavior still need bench validation.

The maintained house 1 switch is a **request input only**, never a mains switch bypassing the controller.
A request already HIGH at boot is ignored until it is released and asserted again.
For a fault acknowledgement, release demands, restore safe floats, then send `ack` followed by LF in the 115200-baud serial monitor, or use cloud V10.
Acknowledgement does not restart a pump: issue a new demand afterward.

## Task, timing, and logging contract

| Owner | Scheduling | Responsibilities |
| --- | --- | --- |
| Control | Core 1, priority 4, nominal 20 ms | Read raw safety contacts, snapshot cumulative pulse totals, consume at most one queued command, update the shared controller, order changed GPIO writes, enqueue records/snapshots, feed its watchdog |
| I/O | Core 0, priority 1, 5 ms yield | Drain UART records in bounded chunks, accept serial `ack`, refresh LCD from a snapshot every 500 ms, publish adapter counters every 5 s |
| Cloud, when enabled | Core 0, priority 1, 10 ms yield outside library calls | Wi-Fi/SNTP/TLS/Blynk, command handlers, paced state publication, fault-transition notifications |

LCD/I2C and all Serial formatting/output are outside the control task and ISRs.
A stalled display or cloud task cannot hold a control-owned bus or mutex.
ISRs only increment protected `uint32_t` pulse totals; the control owner copies all three under the same short critical section.
Unsigned pulse and time arithmetic tolerates counter wrap.
There is no heap allocation or formatting in the controller/ISR logging path.
Arduino, LCD, networking, and TLS libraries may allocate internally; this is not a claim of a globally heap-free firmware.

The control task uses IDF 4.4's `esp_task_wdt_init(2, true)`, subscribes itself, and feeds after applying the outputs.
Initialization/feed failures leave or drive outputs OFF and expose `startup_error` in adapter statistics.
The shared controller separately latches a missed control deadline when its configured 250 ms maximum update gap is exceeded.
Neither mechanism proves that real hardware meets a 20 ms deadline.
Static task stacks reserve 8 KiB each for control and I/O, plus 24 KiB for cloud when enabled; this excludes SDK tasks, TLS heap, and library buffers.
ESP-IDF's task-creation stack-depth argument is **bytes**, unlike upstream FreeRTOS's usual words.
Actual flash, heap headroom, stack high-water marks, current draw, and worst-case timings must be measured on the chosen hardware/build.

Output latches are set OFF before GPIO output mode is enabled.
On changes, affected branch pumps stop first, then the source pump if no longer requested; valves open/close before any newly enabled pumps are written.
A source pump still serving another open branch is not pulsed when one branch closes.
The shared controller owns opening/priming delays; unchanged outputs are never toggled to manufacture a sequence.
Logs, LCD, LEDs, and Blynk pump state represent **commanded GPIO/controller state**, not contactor, valve-position, or motor-current feedback.
The board has no such feedback sensors.

All handoffs are statically bounded: 16 commands, 64 event records, one latest-telemetry I/O mailbox, and, when cloud is enabled, one additional telemetry mailbox and 8 cloud fault notices.
Queue sends in control never wait.
Remote commands carry a receipt timestamp and connection session; disconnected, stale-heartbeat, old-session, and older-than-2-second commands are rejected without replay.
Serial acknowledgements remain available offline.
Mailbox overwrite is intentional latest-state behavior, not an event history.

Controller events use the shared bounded English `formatEvent` record, preserving sequence, timestamp, severity, code, subject, value, and unit.
Numeric flow-event values use mL/min, rather than allocating float-formatted log strings.
The I/O task holds one fixed formatted record and writes no more than 64 UART bytes per iteration, limited by available TX capacity.
It never pops a second event while the first is pending.
Events that cannot enter the 64-record queue are counted, rather than blocking a safety update.
The core's 32-entry ring is retained independently; its ordinary history overwrites are distinct from transport losses.

Every 5 seconds, when the current UART record finishes, `ADAPTER_STATS` reports cumulative:

- `command_drops`: full ingress queue;
- `stale_remote_commands`: offline, stale, expired, or old-session remote commands;
- `log_queue_drops`: events discarded because the UART queue was full;
- `missed_ring_events`: new core events overwritten before adapter extraction;
- `ring_overwrites`: core history retention overwrites, not necessarily lost UART records;
- `suppressed_logs`: repeated controller rejections rate-limited by the core;
- `notification_drops`: cloud notices discarded offline, expired after 10 seconds, or overflowed;
- `format_failures`, `serial_input_errors`, and `startup_error`.

These counters make loss visible after background I/O recovers; no firmware can print through a permanently unavailable UART or frozen CPU.
`startup_error` uses additive flags: 1 watchdog setup, 2 watchdog feed, 4 I/O task creation, 8 control task creation, 16 cloud task creation.
An I/O task creation failure has no UART consumer; its counter then requires debugger inspection rather than promising a printed diagnostic.
Cloud notices are attempts, not delivery receipts; Blynk/account delivery failures are not observable through this local drop counter.

## Blynk template and intentional V0–V17 remap

Create numeric virtual datastreams with these meanings.
Use integer 0/1 datastreams for booleans, an integer fault mask for V17 (0–127), and double streams in L/min for V0–V2.
Only V6, V10, V11, and V12 accept commands.
Configure V10 as a momentary/push button and the other command controls as 0/1 switches.
Disable **Sync with latest server value** for command datastreams; the firmware never calls `syncAll` or `syncVirtual`.
There is no reconnect command restoration.

| Virtual pin | Meaning |
| --- | --- |
| V0 / V1 / V2 | Main / house 1 / house 2 measured flow, L/min |
| V3 | House 2 tank inhibited: 0 healthy, 1 full/open loop |
| V4 | Latched unaccounted main flow |
| V5 | Either branch has latched low flow |
| V6 | House 2 remote request on write; current house 2 pump output command on publish |
| V7 / V8 | House 1 / house 2 latched low-flow diagnostic |
| V9 | Latched inconsistent-meter diagnostic |
| V10 | Safe fault acknowledgement when written 1; publishes 0 |
| V11 / V12 | House 1 / house 2 isolation: 1 closed, 0 allowed |
| V13 / V14 | House 1 / source pump output command |
| V15 | Source permit absent: 1 unsafe/open loop; raw input, not latched fault state |
| V16 | House 1 tank inhibited: 1 full/open loop |
| V17 | Latched fault bitmask from `water::Fault` |

**This redesign replaces the earlier V7–V10 meanings.**
Recreate/update the template and widgets; old leak-localization labels or reset controls must not be retained under these pin numbers.
Unaccounted main flow does not localize a leak to a house.
Low branch flow is not proof of blockage, and inconsistent meters are not proof of sensor failure.
Leakage downstream of a branch meter can remain invisible to the balance calculation.

The cloud task sends at most one `virtualWrite` every 200 ms: at most 5 writes/s overall, not per datastream.
An 18-pin sweep therefore takes approximately 3.6 seconds, longer if networking stalls.
Reconnect starts with current V6, V11, V12, and V10 values, then current remaining telemetry; it does not replay historical commands.
V6 displays an output command, so it can stay OFF during valve settling/priming even after a valid request.
Do not use its dashboard appearance as a safety interlock.

Create the template event code `water_fault` and configure desired notifications in the Blynk console.
A newly latched fault-bit transition enqueues one bounded English message containing its time and masks; repeated samples of the same fault do not notify repeatedly.
Messages are paced to at most one per second, expire after 10 seconds, and are discarded while offline.
Account limits, event cooldowns, mobile settings, TLS trust, and cloud delivery must be checked with the actual account.

## Using the Wokwi circuit

The three square-wave generators replace hall meters; they are **digital test inputs**, not pump-driven simulated fluid.
The diagram intentionally uses no real motors, coils, or fluid model.
Every output LED has its own 330 ohm series resistor.
The active-buzzer output is also an LED indicator, so the simulation does not model buzzer current or sound.

| Diagram part IDs | Left position | Right position |
| --- | --- | --- |
| `tank1Healthy`, `tank2Healthy`, `sourceHealthy` | NC loop grounded: healthy | Open loop: full/unsafe/broken-wire condition |
| `house1Request` | Request OFF | Request ON |
| `mainFlow`, `house1Flow`, `house2Flow` | Meter grounded: zero pulses | Corresponding clock connected |

All switches start left: floats healthy, demands OFF, and no flow pulses.
The house 2 pushbutton toggles local demand after the core's 50 ms debounce; hold it at least 80 ms and release it at least 80 ms.
Each flow gate switch right connects its generator: main 30 Hz, each branch 15 Hz.
At the illustrative 450 pulses/litre these correspond to 4 L/min and 2 L/min respectively.

For a balanced two-branch example, request house 1 and house 2, then turn on all three flow gates as the pump indicators start.
To model one active branch at 2 L/min, change `mainClock` frequency to `15`, connect main and that branch's flow gates, and leave the unused branch grounded.
For no-flow diagnostics, leave the requested branch's gate grounded after its pump indicator starts.
For unaccounted flow, set main frequency higher than the sum of enabled branch frequencies and sustain it beyond the controller's settling/persistence windows.
Open a float-loop switch to exercise raw safety stopping; open the source loop to latch source unsafe.
After an alarm, restore healthy inputs, turn requests OFF, send serial `ack`, then make a new demand.

Clock sources do not automatically stop when the LEDs go OFF, and switches are not a hydraulic model.
Manually set pulse gates/frequencies consistently with the scenario.
For automated water-balance/volume scenarios, use the native software-in-the-loop simulator instead.
A successful Wokwi run would establish only the exercised CPU/GPIO behavior, not electrical or fluid safety.

## Primary references

- [Espressif GPIO restrictions, including GPIO34–39 pull resistors](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/peripherals/gpio.html)
- [Espressif IDF 4.4.7 watchdog API](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/system/wdts.html)
- [Espressif FreeRTOS task APIs and stack units](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32/api-reference/system/freertos.html)
- [Pinned PlatformIO Espressif32 platform package manifest](https://github.com/platformio/platform-espressif32/blob/v6.9.0/platform.json)
- [Blynk 1.3.2 ESP32 TLS adapter source](https://github.com/blynkkk/blynk-library/blob/v1.3.2/src/BlynkSimpleEsp32_SSL.h)
- [Blynk state synchronization and command replay behavior](https://docs.blynk.io/en/blynk-library-firmware-api/state-syncing)
- [Blynk event API and description length](https://docs.blynk.io/en/blynk-library-firmware-api/log-event)
- [Wokwi diagram schema](https://docs.wokwi.com/diagram-format), [ESP32 DevKitC pin definition](https://github.com/wokwi/wokwi-boards/blob/main/boards/esp32-devkit-c-v4/board.json), and [PlatformIO integration](https://docs.wokwi.com/vscode/platformio)
- [Wokwi clock generator](https://docs.wokwi.com/parts/wokwi-clock-generator), [slide switch](https://docs.wokwi.com/parts/wokwi-slide-switch), [LCD1602](https://docs.wokwi.com/parts/wokwi-lcd1602), and [LED](https://docs.wokwi.com/parts/wokwi-led)
