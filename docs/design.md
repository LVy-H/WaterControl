# Controller analysis and redesign

This design turns the document's fragments into one local safety controller, executable on a desktop and reused by ESP32 firmware.
The simulator establishes behavior under specified inputs; it does not establish physical safety or identify hydraulic facts that the sensors cannot observe.

## Original design assessment

The supplied Word document describes an ESP32 water-distribution demonstration with three hall meters, two household pumps, a source pump, two valves, a tank float, LCD, LEDs/buzzer, and Blynk.
Section 13 is a tutorial, not a complete sketch.
Its cautions about calibration, relay polarity, and sensor placement are valid; the missing implementations still have to be supplied.

| Area | Evidence in the original document | Redesign |
| --- | --- | --- |
| Program assembly | Setup/connection/timers described, not fully implemented | Runnable native engine and separate ESP32 adapter |
| Flow | Pulse ISRs exist; no conversion updates flow variables | Cumulative counter snapshots, actual elapsed time, individual calibration |
| Fault diagnosis | Flags and display exist; no tested detection algorithm | Explicit balance/low-flow/mismatch conditions, settling and persistence |
| Float safety | Commands check cached float before the loop refreshes it | Raw permission sampled before every command; final output gating |
| Valve control | GPIO declarations without control handlers | Explicit branch isolation, demand cancellation, ordered GPIO transitions |
| Cloud coupling | Float reads call virtualWrite each loop | Local control owner; independently paced networking |
| Logging | No structured operational log implementation | Numeric fixed records, stable English codes, bounded ring/queues, visible losses |
| Operator diagnosis | Per-house leak flags imply unsupported localization | Names state measured evidence, not an unproven physical cause |
| Restart | No complete boot/reconnect/reset policy | No held-input boot start, cloud replay, or automatic fault recovery |

The original flow formula multiplies volume/time by 60 while defining time in minutes.
Multiplication by 60 is appropriate for seconds; for milliseconds the pulse-based equation below uses 60000.

## Modules and ownership

| Module | Interface | Implementation and ownership |
| --- | --- | --- |
| Controller | `Controller::update(Inputs)`; read snapshot and event ring | Single control owner, bounded work, no I/O, no C++ heap allocation |
| Simulated plant | Inputs, commands, deterministic time advancement | Volume integration and discrete pulse generation; intentionally simplified hydraulics |
| Native protocol | One ASCII command / one JSON response | Desktop-only parsing and presentation |
| Browser adapter | Validated HTTP actions, rendered snapshots | Python process bridge and plain JavaScript; no second safety implementation |
| ESP32 adapter | Raw contacts/pulses and bounded command handoffs | GPIO control owner, background UART/LCD owner, optional background Blynk owner |

The common seam is the controller input/output contract, not a replacement Arduino library.
Both adapters exercise the real controller rather than implementing similar-looking safety decisions in another language.
Only the desktop presentation and protocol parser use dynamic strings.
Third-party Arduino, LCD, TLS, and Wi-Fi libraries may allocate internally.

The production Arduino project is `firmware/WaterDistribution/`.
Its `src/` directory is the single owner of the pure controller; the sketch's hardware adapter and the desktop Makefile both compile it directly.
The `.ino` owns Arduino entry points; `Hardware.cpp` owns real I/O and task startup.
Only `simulator/` owns simulated pulse sources, tank volumes, fault injection, virtual time advancement and the desktop HTTP interface.
Copying the Arduino sketch folder therefore cannot pull simulator code into a board build.

## Required hardware changes

This is an intentional redesign, not a claim that the original parts list already supports every safety function.

1. Put the source pump behind its own rated driver instead of running it permanently.
2. Put the house-1 pump behind a rated driver; convert its maintained power switch into a low-voltage request input.
3. Add a house-1 high-level permit and a source low-water permit, both wired normally closed while healthy.
4. Use de-energized-closed valves, verified driver polarities, output OFF-state bias, fuses and suitable transient protection.
5. Use independent hardwired protective interlocks where equipment risk requires them.

See the exact [pin map and electrical limits](firmware.md#hardware-pin-redesign).
The controller cannot verify relay contacts, valve position, motor current, pressure, grounding, or actual delivered water with the supplied sensors.
Its output state and logs are **commands**, not proof that a load operated.
An open contact is fail-inhibiting; a short to ground can still falsely report healthy.

## Safety policy

- Start with all pumps OFF and valves closed.
- A pre-existing house-1 request or held house-2 button at boot requires release before a fresh action.
- A raw missing tank permit immediately cancels that house's demand. Restoring the contact never restores demand.
- A missing source permit stops all pumps, closes valves, and latches `SOURCE_UNSAFE`.
- An isolated branch cannot start; isolation cancels its demand. Allowing it again does not replay that demand.
- A requested valve opens first, settles for 300 ms, then permits the source pump. Branch pumps wait another 500 ms after source startup.
- Adding a second house while the source runs still waits for that house's valve to settle.
- A confirmed flow anomaly or control-update gap above 250 ms latches all-output shutdown.
- Fault acknowledgement requires a healthy source, house-1 switch OFF, house-2 button released, and no outstanding demands. Invalid configuration cannot be acknowledged away.
- Acknowledgement clears fault state, not the physical cause. A fresh request can encounter and relatch an unrepaired flow problem.
- Local house-2 button actions take precedence over a remote command in the same control update.
- Remote ON/allow/reset commands require an online input and receipt age at most 2 seconds. The core always accepts STOP, even when old.
- Losing connectivity cancels remote-owned house-2 demand. Local demand remains usable offline. Reconnect publishes current state and does not synchronize old commands.

The ESP32 adapter also checks network-task freshness and connection-session identity before delivering queued remote commands.
A networking stall does not stall local safety work.
A software update-gap detector only reacts when execution resumes; the watchdog and independent physical interlocks address different failure modes.
Neither a timer nor a GPIO command proves that a valve is open or that a source is primed.
All delays and thresholds require equipment-specific validation.

## Flow calculation and diagnosis

For meter `i`, with calibrated coefficient `K[i]` in pulses/litre:

```text
pulse_delta[i] = current_total[i] - previous_total[i]   (uint32 modular subtraction)
flow_lpm[i] = pulse_delta[i] / K[i] * 60000 / elapsed_ms
```

Counters are cumulative; the ESP32 adapter snapshots all three inside one short ISR-safe critical section.
It never resets a counter concurrently with an interrupt.
Each coefficient must be measured separately on the installed meter and operating flow range.
The simulation's identical 450 pulses/litre coefficients are illustrative only.

Sampling nominally occurs every 1000 ms, but conversion uses the actual elapsed interval.
Only a complete settled sample window contributes to fault confirmation.
Any hydraulic output change clears the pending persistence counters.
A healthy sample resets its corresponding pending condition rather than letting unrelated transient events accumulate.

```text
residual = main_flow - house1_flow - house2_flow
balance_tolerance = max(0.4 L/min, 15% * max(main_flow, sum_of_branch_flows))
```

After 2000 ms of hydraulic settling, each abnormal condition must persist across 3000 ms of qualifying samples:

| Condition | Diagnostic | What it does not prove |
| --- | --- | --- |
| Residual above tolerance | `UNACCOUNTED_FLOW` | Leak location, or that meter calibration is correct |
| Residual below negative tolerance | `METER_MISMATCH` | Which meter failed, or that a sensor rather than topology is wrong |
| Requested/running branch below 0.3 L/min | `HOUSE_1_LOW_FLOW` / `HOUSE_2_LOW_FLOW` | Blockage rather than source, pump, plumbing, or meter trouble |
| Raw source permission absent | `SOURCE_UNSAFE`, immediate | Whether the cause is low water or an open circuit |

All flow diagnostics are conservative shutdown triggers, not autonomous root-cause diagnoses.
The model omits pressure, pump curves, compliance, backflow, air pockets, valve travel, water hammer, analog noise, and calibration drift.
A fault can be below threshold or remain unobservable.

### Sensor observability

The main meter is upstream of the split; each house meter is downstream of the split.
A leak between the main meter and the branch meters can create a positive residual but does not identify a unique branch from that residual alone.
A leak after a branch meter can leave all three readings balanced because that meter also counts the lost water.
The simulator exposes delivered flow as **plant truth** to illustrate that blind spot; firmware does not have that extra measurement.
Localizing leaks requires a different measurement layout, controlled isolation experiments, pressure/position feedback, or additional metering.
No software classifier can recover information absent from its inputs.

## Embedded logging contract

### Record meaning

An event is a fixed-size numeric record containing:

- `sequence`: unsigned event order within this boot; modular `uint32_t` sequence arithmetic;
- `time_ms`: boot-relative monotonic `uint32_t` time, not UTC;
- severity: `INFO`, `WARN`, or `ERROR`;
- a stable English event code;
- subject: controller, source, a house, a valve, or network;
- one signed integer value, with units defined by the code.

`formatEvent` renders bounded ASCII only in a background/desktop adapter:

```text
t_ms=7000 seq=42 level=ERROR code=FAULT_UNACCOUNTED_FLOW subject=SOURCE value=1500 unit=mL/min
```

This says the controller confirmed a positive 1.5 L/min balance residual at that time.
It does not say that house 1 is leaking, that a valve physically closed, or that a phone received an alert.
`PUMP_ON_COMMANDED` and `VALVE_CLOSE_COMMANDED` deliberately distinguish output commands from feedback.
A tank-permit value of zero means full/open loop, not a diagnosis of which one.

Flow diagnostic values use signed **mL/min**; deadline/expired-command values use **ms**; fault rejection uses **fault_bits**; input/output/policy transitions use **state**.
Codes without a numeric measurement use zero.
Booleans are 0/1, not strings or locale-dependent text.
No tokens, passwords, SSIDs, or personal data are logged.

### Volume and retention

- The core holds the newest 32 records. Its memory cost is `32 * sizeof(Event)` plus ring metadata.
- Normal flow samples are telemetry, not logs. A stable idle/running state produces no repeated status lines.
- Faults emit on the transition into a newly latched bit, not every sample.
- Consecutive identical rejection code/subject pairs emit at most once per second; every rejection and suppressed record is still counted.
- Isolation-policy changes are logged even when an already-closed valve requires no GPIO transition.
- The bounded formatter returns zero rather than presenting a truncated record as valid.
- No ISR or control update formats strings, writes UART/I2C, calls networking, or waits for a logging queue.
- Core ring overwrites are retention events, not necessarily transport losses: a background adapter may already have exported those records.
- Queue drops, missed ring extraction, suppressed rejections, and formatting failures remain separate counters.
- Serial output resumes with available records and counters after a background stall; it cannot promise records through a permanently unavailable UART.

The ESP32 adapter uses a 64-event serial queue, one pending formatted record, bounded available-capacity UART writes, and five-second adapter statistics.
Cloud notifications are separate fault-transition attempts with bounded queues and expiry; they are not delivery receipts.
The browser log is a view of the core ring, not a second unbounded history.
No flash logging is added: endurance, crash retention, and persistent audit storage would require a separate storage design.

## What simulation can establish

The native scenarios exercise actual controller behavior, including timer/counter wrap, unequal calibration, simultaneous safety/ON inputs, command ownership, restart policy, and overflow semantics.
A C++ allocation guard also exercises normal and fault paths without `operator new`.
Address/undefined-behavior sanitizers cover the executed native paths; they are not a proof of all MCU behavior.
The compiler's native object sizes are not a measurement of firmware heap, stack headroom, flash consumption, or power.

Wokwi can additionally exercise the compiled ESP32 CPU, interrupts, GPIO, and LCD with substitute digital inputs.
Its independent clock sources are not a fluid model and must be set consistently by the operator.
The local simulator needs no cloud account; a real Blynk integration requires its own credentials and account-side configuration.

Before real use, measure sensor coefficients/range, signal levels, output reset states, ISR loss, worst-case scheduling/stack headroom, actual valve travel/priming, hydraulic fault thresholds, and protective-interlock operation.
Test under noisy supplies and stalled display/network conditions on the actual board.

## Guidance and primary sources

- [Embedded-systems engineering skill](https://github.com/Jeffallan/claude-skills/blob/main/plugin/skills/embedded-systems/SKILL.md): reviewed for workflow, RTOS separation, synchronization, bounded resources, watchdogs and measurement. It is guidance, not a certification standard; ESP32-specific APIs/units come from Espressif.
- [NASA/JPL F Prime memory allocation guidance](https://fprime.jpl.nasa.gov/devel/docs/user-manual/framework/memory-management/memory-allocation/): motivates avoiding runtime heap allocation in the control path. This project does not claim flight-software compliance.
- [Espressif watchdog guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/wdts.html): keep ISRs/critical sections short and avoid blocking; pinned firmware API references are in the firmware guide.
- [Blynk timers and flood prevention](https://docs.blynk.io/en/blynk-library-firmware-api/blynk-timer): do not transmit every loop; pace cloud publication independently.
- [Wokwi ESP32 capabilities](https://docs.wokwi.com/guides/esp32) and [network limitations/privacy](https://docs.wokwi.com/guides/esp32-wifi): digital MCU simulation with a separate internet gateway, not electrical or hydraulic validation.

## Interface design

The browser is an operating surface for experiments, not a marketing dashboard.
The old nested-card layout repeated safety explanations and buried event history.
The replacement keeps the connected distribution network, controls and event console in one workspace.

| Choice | Reason |
| --- | --- |
| Self-hosted IBM Plex Sans 400/600 | Productive engineering UI typography; works offline without remote font requests |
| IBM Plex Mono | Used only for measurements, timestamps, identifiers and raw event codes |
| Neutral light surfaces, blue action/water accent | Readable in ordinary desk lighting; color identifies interaction and commanded flow |
| Amber warning / red fault states | Semantic state only, paired with text; no decorative rainbow or pulsing activity |
| Connected pipe schematic | Shows the actual source/split/branch topology rather than unrelated metric cards |
| Controls / Faults / Scenarios tabs | Keep tasks findable without a page of repeated instructions |
| Bounded event console | Sticky headers, aligned numeric columns, exact codes/units, combined search/severity filtering and visible retention counters |
| Responsive composition | Controls stack below the network on narrow screens; only the schematic and table scroll horizontally |

Blue pump/valve symbols show controller commands, not measured physical movement.
Delivery remains explicitly labeled model truth.
Dangerous or stale states are reported; the interface never edits plant/controller state to manufacture a successful scenario.

Design references:

- [Anthropic frontend-design guidance](https://github.com/anthropics/skills/blob/main/skills/frontend-design/SKILL.md)
- [Frontend Slop Guard](https://github.com/airtaxi/frontend-slop-guard): used as an explicit anti-pattern checklist, not as an authority on subjective quality
- [Impeccable Operate guidance](https://github.com/pbakaus/impeccable/blob/main/.agents/skills/impeccable/reference/operate.md)
- [Carbon productive typography](https://carbondesignsystem.com/building-blocks/foundations/typography)
- [Carbon table specifications](https://www.carbondesignsystem.com/building-blocks/core/components/data-table/specifications)
- [Carbon semantic status guidance](https://www.carbondesignsystem.com/building-blocks/core/patterns/status-indicators)
- [IBM Plex source and SIL Open Font License](https://github.com/IBM/plex); the bundled fonts retain `simulator/web/fonts/LICENSE.txt`
