# Water-distribution controller

Run and inspect the redesigned ESP32 water controller without hardware.
The desktop simulator and ESP32 firmware compile the same C++ controller; browser JavaScript only sends commands and renders its results.
All code, comments, identifiers, and event records are English.

Status: software-in-the-loop implementation with illustrative hydraulics and a separate ESP32/Wokwi adapter.
Real electrical operation, hydraulic thresholds, and timing require hardware validation.

## Run the simulator

Requirements: Python 3.10+, GNU Make, and a C++17 compiler (`g++` or compatible).
No Python packages, Node packages, Blynk account, credentials, or hardware are required.

```sh
python3 simulator/server.py
```

Open **http://127.0.0.1:8765**.
The launcher builds the native engine when needed and serves a local-only UI.
Use `--port 8766` if the default port is already occupied.
Stop with Ctrl+C; simulation state is in memory and resets on restart.
One running server represents one shared simulated system, not separate sessions per browser tab.

Start with the guided scenarios, or use the physical switch/button, remote pump control, isolation controls, and fault injections.
The virtual clock can pause, step, or advance faster than wall time.
Injected source/tank levels are plant inputs, not a workaround that edits controller decisions.
Full/open tank contacts remove permission at 95% tank capacity in this illustrative model.

The model uses 2 L house tanks, a 20 L source reservoir, branch flows of 3 and 4 L/min, a 1.5 L/min upstream leak, and a 1.5 L/min downstream house-1 leak.
These values are examples, not measurements or validated pump/pipe specifications.
The flow sources generate discrete pulses, which the real controller converts back to measured flow.

## Check the controller

```sh
make check
```

This compiles with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer, then runs deterministic scenarios through the controller and plant.
The scenarios cover startup sequencing, simultaneous float/ON commands, no automatic restart, persistence, anomaly classifications, offline local operation, valve isolation, button bounce, command expiry, clock/pulse rollover, per-meter calibration, logging, and C++ allocation-free updates.
See [`tests/scenarios.cpp`](tests/scenarios.cpp) for the executable behavioral contract.

For a machine-readable desktop interface:

```sh
make .build/water-sim
printf 'pump2 1\nadvance 10000\nstate\n' | .build/water-sim --stdio
```

Each input line produces one JSON response; `state` does not advance time.
The HTTP interface is `GET /api/state` and `POST /api/action`.
Native command parsing lives in [`simulator/engine.cpp`](simulator/engine.cpp); HTTP action validation lives in [`simulator/server.py`](simulator/server.py).

## What changed from the document

- Safety decisions run locally, independently of display, logs, Wi-Fi, and Blynk.
- Healthy normally-closed float circuits provide permission; an open wire removes it.
- Source pump and both house pumps are controller-owned. The house-1 switch is a low-voltage request, not a bypass around safety.
- Valves settle before the source pump starts; branch pumps wait for source priming.
- Flow comes from elapsed-time pulse deltas with separate calibration coefficients.
- Sustained faults latch shutdown; acknowledgement and a new demand are required.
- Remote-owned house-2 demand stops on lost connectivity; local controls remain usable. Reconnect never restores old commands.
- Logs are fixed-size transition events with explicit units, bounded retention, and visible loss/suppression counts.

Three flow meters cannot reliably localize every leak.
The diagnostic names deliberately say **unaccounted flow**, **low branch flow**, and **meter mismatch**, not an unproven location or physical cause.
The downstream-leak scenario demonstrates a genuine blind spot instead of pretending to detect it.

## Use Arduino IDE later

Copy the complete [`firmware/WaterDistribution`](firmware/WaterDistribution) folder, including `Hardware.cpp`, `Hardware.h`, and `src/`.
Open [`WaterDistribution.ino`](firmware/WaterDistribution/WaterDistribution.ino) in Arduino IDE.
Install **esp32 by Espressif Systems 2.0.17** and **LiquidCrystal I2C 1.1.2** from Arduino's Library Manager, then choose **ESP32 Dev Module**.
This is ESP32 firmware, not an Arduino Uno/Nano sketch.
The default build is offline and needs no Blynk account.
Read the [IDE setup, calibration, and wiring requirements](docs/firmware.md#arduino-ide) before connecting loads.

The simulator imports only `src/water/controller.hpp` and `src/controller.cpp` from this sketch's `src/` module.
The firmware never imports the simulated plant, desktop engine, HTTP server, or browser code.
There is one controller implementation, not an edited firmware copy.

## Engineering details

- [Analysis, architecture, safety policy, and logging contract](docs/design.md)
- [ESP32 builds, changed wiring, Blynk datastreams, and Wokwi instructions](docs/firmware.md)
- [`firmware/WaterDistribution/src/water/controller.hpp`](firmware/WaterDistribution/src/water/controller.hpp): pure controller interface and bounded events
- [`firmware/WaterDistribution/src/controller.cpp`](firmware/WaterDistribution/src/controller.cpp): shared controller implementation

Keep `.pio/` private: a cloud firmware image contains its configured credentials.
Neither the simulator nor an ESP32 compile proves real pump, pressure, valve, mains, or overfill safety.
