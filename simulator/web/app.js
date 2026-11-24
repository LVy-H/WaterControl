"use strict";

const $ = (id) => document.getElementById(id);
let state = null;
let connected = false;
let pending = 0;
let tail = Promise.resolve();
let playing = false;
let scenarioRunning = false;

const allPumpsOff = (s) => !s.outputs.source_pump && s.outputs.house_pump.every((value) => !value);
const faultIs = (name) => (s) => s.faults.includes(name) && s.outputs.alarm && allPumpsOff(s);
const step = (label, action, expected, check) => ({ label, action, expected, check });
const scenarios = [
  {
    name: "Normal filling",
    description: "Observe native valve settling and source priming before the House 2 pump runs.",
    steps: [
      step("Reset the engine", { action: "reset" }, "Idle, pumps off", (s) => s.phase === "IDLE" && allPumpsOff(s)),
      step("Request House 2 remotely", { action: "pump2", value: true }, "Demand accepted; branch pump still off during startup", (s) => s.requested[1] && !s.outputs.house_pump[1]),
      step("Advance 2 seconds", { action: "advance", value: 2000 }, "Source and House 2 pumps on, valve open, no latched fault", (s) => s.outputs.source_pump && s.outputs.house_pump[1] && s.outputs.valve_open[1] && s.fault_mask === 0),
    ],
  },
  {
    name: "Full tank and open float wire",
    description: "Both full level and a broken NC float wire must stop the associated branch immediately. The controller sees the same unsafe contact.",
    steps: [
      step("Reset the engine", { action: "reset" }),
      step("Request House 2 remotely", { action: "pump2", value: true }),
      step("Advance 2 seconds", { action: "advance", value: 2000 }, "House 2 pump on", (s) => s.outputs.house_pump[1]),
      step("Inject a full House 2 tank", { action: "tank", house: 2, value: 100 }, "Tank permission removed; House 2 pump off", (s) => !s.tank_permit[1] && !s.outputs.house_pump[1]),
      step("Reset for the broken-wire observation", { action: "reset" }),
      step("Request House 2 remotely", { action: "pump2", value: true }),
      step("Advance 2 seconds", { action: "advance", value: 2000 }, "House 2 pump on", (s) => s.outputs.house_pump[1]),
      step("Open the House 2 float wire", { action: "fault", name: "tank2_wire_open", value: true }, "Permission removed and pump off despite a tank below full", (s) => !s.tank_permit[1] && !s.outputs.house_pump[1] && s.tank_percent[1] < 100),
    ],
  },
  {
    name: "Sustained unaccounted main flow",
    description: "Inject a main leak while filling. Wait through real flow sampling, stabilization and fault persistence; expect UNACCOUNTED_FLOW, not house-localized leak diagnosis.",
    steps: [
      step("Reset the engine", { action: "reset" }),
      step("Request House 2 remotely", { action: "pump2", value: true }),
      step("Establish flow for 3 seconds", { action: "advance", value: 3000 }),
      step("Inject a leak before the branch meters", { action: "fault", name: "main_leak", value: true }),
      step("Advance 12 seconds", { action: "advance", value: 12000 }, "UNACCOUNTED_FLOW latched, alarm on, all pumps off", faultIs("UNACCOUNTED_FLOW")),
    ],
  },
  {
    name: "Low branch flow under blockage",
    description: "Inject House 1 blockage and observe HOUSE_1_LOW_FLOW. That label is evidence of low measured flow, not proof of the physical cause.",
    steps: [
      step("Reset the engine", { action: "reset" }),
      step("Turn on the local House 1 switch", { action: "house1", value: true }),
      step("Establish flow for 3 seconds", { action: "advance", value: 3000 }),
      step("Inject House 1 blockage", { action: "fault", name: "blockage1", value: true }),
      step("Advance 12 seconds", { action: "advance", value: 12000 }, "HOUSE_1_LOW_FLOW latched, alarm on, all pumps off", faultIs("HOUSE_1_LOW_FLOW")),
    ],
  },
  {
    name: "Cloud loss and local independence",
    description: "Disconnect the simulated network: remote House 2 demand is cancelled, House 1 remains local, and the physical House 2 button can request operation offline.",
    steps: [
      step("Reset the engine", { action: "reset" }),
      step("Turn on the local House 1 switch", { action: "house1", value: true }),
      step("Request House 2 remotely", { action: "pump2", value: true }),
      step("Advance 2 seconds", { action: "advance", value: 2000 }),
      step("Disconnect the simulated network", { action: "network", value: false }, "House 2 demand cancelled, House 1 demand retained", (s) => !s.network_online && !s.requested[1] && s.requested[0]),
      step("Press the local House 2 button", { action: "press" }, "Local House 2 demand accepted offline", (s) => !s.network_online && s.requested[1]),
      step("Advance 2 seconds", { action: "advance", value: 2000 }, "Both house pumps operate offline without a fault", (s) => s.outputs.house_pump.every(Boolean) && s.fault_mask === 0),
      step("Reconnect the simulated network", { action: "network", value: true }, "Network online; local demand retained", (s) => s.network_online && s.requested[1]),
    ],
  },
  {
    name: "Downstream leak detection blind spot",
    description: "A leak after the House 1 branch meter still passes both main and branch meters. Compare measured branch flow with plant-delivered flow; balance detection cannot identify this loss.",
    steps: [
      step("Reset the engine", { action: "reset" }),
      step("Turn on the local House 1 switch", { action: "house1", value: true }),
      step("Establish flow for 3 seconds", { action: "advance", value: 3000 }),
      step("Inject a leak after the House 1 meter", { action: "fault", name: "downstream_leak1", value: true }),
      step("Advance 12 seconds", { action: "advance", value: 12000 }, "No latched fault; measured branch flow exceeds water delivered to the tank", (s) => s.flow_valid && s.fault_mask === 0 && s.outputs.house_pump[0] && s.flow_lpm[1] > s.delivered_lpm[0] + 0.1),
    ],
  },
];

const phaseLabels = {
  IDLE: "Idle", OPENING_VALVES: "Opening valves", PRIMING_SOURCE: "Priming source",
  RUNNING: "Running", FAULTED: "Fault latched",
};
const faultLabels = {
  UNACCOUNTED_FLOW: "Unaccounted main flow",
  HOUSE_1_LOW_FLOW: "House 1 low flow",
  HOUSE_2_LOW_FLOW: "House 2 low flow",
  METER_MISMATCH: "Inconsistent meters",
  SOURCE_UNSAFE: "Source inhibited",
  CONTROL_DEADLINE_MISSED: "Control deadline missed",
  INVALID_CONFIGURATION: "Invalid configuration",
};

function controls() {
  const busy = pending > 0 || scenarioRunning;
  document.querySelectorAll("[data-control]").forEach((element) => {
    element.disabled = !connected || busy;
  });
  $("refresh").disabled = busy;
  $("play").disabled = !connected || scenarioRunning;
  $("play").textContent = playing ? "Pause clock" : "Run clock";
  $("play").setAttribute("aria-pressed", String(playing));
  $("clock-status").textContent = playing ? `Running at ${$("speed").value}×` : "Paused";
  $("scenario").disabled = busy;
  if (state) $("ack").disabled = !connected || busy || state.fault_mask === 0;
}

function failure(error) {
  connected = false;
  playing = false;
  $("error").hidden = false;
  $("error").textContent = `${error.message || error} Use Refresh to reconnect; displayed values may be stale.`;
  $("connection").textContent = state ? "Last known state" : "Engine unavailable";
}

function schedule(operation) {
  pending += 1;
  controls();
  const job = tail.then(async () => {
    try {
      await operation();
    } catch (error) {
      failure(error);
    } finally {
      pending -= 1;
      controls();
    }
  });
  tail = job;
  return job;
}

async function request(action) {
  const options = action ? {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(action),
  } : {};
  const response = await fetch(action ? "/api/action" : "/api/state", {
    ...options, cache: "no-store", signal: AbortSignal.timeout(15000),
  });
  let result;
  try {
    result = await response.json();
  } catch (_error) {
    throw new Error(`HTTP ${response.status}: invalid server response. Controller state is unknown.`);
  }
  if (!response.ok || result.error) throw new Error(result.error || `HTTP ${response.status}`);
  state = result;
  connected = true;
  $("error").hidden = true;
  $("connection").textContent = "Engine ready";
  render();
  return state;
}

function act(action) {
  return schedule(() => request(action));
}

function showTab(name, focus = false) {
  document.querySelectorAll("[data-tab]").forEach((tab) => {
    const selected = tab.dataset.tab === name;
    tab.setAttribute("aria-selected", String(selected));
    tab.tabIndex = selected ? 0 : -1;
    $(`${tab.dataset.tab}-panel`).hidden = !selected;
    if (selected && focus) tab.focus();
  });
}

function level(id, percent) {
  const vessel = $(id);
  vessel.setAttribute("aria-valuenow", percent.toFixed(1));
  const water = vessel.querySelector(".water");
  const height = Number(water.dataset.height) * percent / 100;
  water.setAttribute("height", String(height));
  water.setAttribute("y", String(Number(water.dataset.bottom) - height));
  $(`${id}-level`).textContent = `${percent.toFixed(1)}%`;
}

function commandSymbol(id, active, description) {
  $(id).classList.toggle("on", active);
  $(id).setAttribute("aria-label", `${description}: ${active ? "on / open" : "off / closed"}`);
}

function setInput(id, value) {
  if (document.activeElement !== $(id)) {
    $(id).value = value.toFixed(1);
    $(`${id}-draft`).textContent = `${value.toFixed(1)}%`;
  }
}

function renderLogs() {
  if (!state) return;
  const severity = $("log-level").value;
  const query = $("log-search").value.trim().toLowerCase();
  const recent = state.logs.slice(-Math.min(32, state.log_capacity));
  const rows = recent.filter((event) =>
    (severity === "ALL" || event.level === severity) &&
    (!query || `${event.code} ${event.subject} ${event.value} ${event.unit}`.toLowerCase().includes(query))
  ).reverse();
  const fragment = document.createDocumentFragment();
  for (const event of rows) {
    const row = document.createElement("tr");
    row.dataset.level = event.level;
    const fields = [
      [event.sequence, "numeric"], [event.time_ms, "numeric"],
      [event.level, "severity-cell"], [event.code, "code-cell"],
      [event.subject, "subject-cell"], [event.value, "numeric"], [event.unit, "unit-cell"],
    ];
    for (const [value, className] of fields) {
      const cell = document.createElement("td");
      cell.className = className;
      const content = className === "severity-cell" ? document.createElement("span") : cell;
      content.textContent = String(value);
      if (content !== cell) cell.append(content);
      row.append(cell);
    }
    fragment.append(row);
  }
  if (!rows.length) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 7;
    cell.className = "empty-log";
    cell.textContent = query || severity !== "ALL"
      ? "No matching events. Clear the search or change severity."
      : "No events yet. Start a pump or run a scenario.";
    row.append(cell);
    fragment.append(row);
  }
  $("logs").replaceChildren(fragment);
  $("log-counts").textContent = `${rows.length} of ${recent.length}`;
  $("log-overwritten").textContent = String(state.log_overwrites);
  $("log-suppressed").textContent = String(state.suppressed_logs);
  $("log-rejected").textContent = String(state.rejected_commands);
}

function render() {
  const milliseconds = state.time_ms;
  $("time").textContent = `${String(Math.floor(milliseconds / 60000)).padStart(2, "0")}:${String(Math.floor(milliseconds / 1000) % 60).padStart(2, "0")}.${String(milliseconds % 1000).padStart(3, "0")}`;
  $("phase").textContent = phaseLabels[state.phase] || state.phase;
  $("phase").dataset.alarm = String(state.outputs.alarm);
  $("alarm").textContent = state.outputs.alarm ? "Alarm" : "No alarm";
  $("alarm").parentElement.dataset.alarm = String(state.outputs.alarm);
  $("ack").hidden = !state.outputs.alarm;
  $("safety").textContent = state.faults.length
    ? state.faults.map((fault) => faultLabels[fault] || fault).join("; ")
    : state.tank_permit.some((permit) => !permit)
      ? "Tank permission removed; affected demand cancelled."
      : "Local safety interlocks ready";
  level("source-tank", state.source_percent);
  commandSymbol("source-pump", state.outputs.source_pump, "Source pump command");
  $("source-permit").textContent = state.source_permit ? "Permit ready" : "Source inhibited";
  $("source-permit").classList.toggle("inhibited", !state.source_permit);
  $("main-flow").textContent = state.flow_valid ? state.flow_lpm[0].toFixed(2) : "—";
  $("flow-valid").textContent = state.flow_valid ? "1 s flow samples" : "Waiting for flow sample";
  $("warning").hidden = !state.warning;
  $("pipe-source").classList.toggle("on", state.outputs.source_pump);
  for (let index = 0; index < 2; index += 1) {
    const house = index + 1;
    level(`tank${house}`, state.tank_percent[index]);
    commandSymbol(`valve${house}`, state.outputs.valve_open[index], `House ${house} valve command`);
    commandSymbol(`pump${house}`, state.outputs.house_pump[index], `House ${house} pump command`);
    $(`pipe${house}`).classList.toggle("on", state.outputs.house_pump[index]);
    $(`flow${house}`).textContent = state.flow_valid ? state.flow_lpm[house].toFixed(2) : "—";
    $(`delivered${house}`).textContent = `Delivery ${state.delivered_lpm[index].toFixed(2)} L/min`;
    $(`permit${house}`).textContent = state.tank_permit[index] ? "Permit ready" : "Tank inhibited";
    $(`permit${house}`).classList.toggle("inhibited", !state.tank_permit[index]);
    $(`request${house}`).textContent = state.isolated[index]
      ? "Branch isolated" : state.requested[index] ? "Demand on" : "No demand";
    $(`isolate${house}`).textContent = state.isolated[index] ? "Allow" : "Isolate";
    $(`isolate${house}`).setAttribute("aria-pressed", String(state.isolated[index]));
    $(`isolate${house}`).setAttribute("aria-label", `${state.isolated[index] ? "Allow" : "Isolate"} House ${house}`);
    setInput(`tank${house}-input`, state.tank_percent[index]);
  }
  setInput("source-input", state.source_percent);
  $("house1-switch").textContent = state.house1_switch ? "On" : "Off";
  $("house1-switch").setAttribute("aria-pressed", String(state.house1_switch));
  $("network-state").textContent = state.network_online ? "Connected" : "Disconnected";
  $("network").textContent = state.network_online ? "Disconnect" : "Reconnect";
  $("remote-on").classList.toggle("selected", state.requested[1]);
  $("remote-off").classList.toggle("selected", !state.requested[1]);
  let injections = 0;
  document.querySelectorAll("[data-fault]").forEach((button) => {
    const active = state.injected_faults[button.dataset.fault];
    if (active) injections += 1;
    button.querySelector(".fault-action").textContent = active ? "Remove" : "Inject";
    button.setAttribute("aria-pressed", String(active));
    button.setAttribute("aria-label", `${active ? "Remove" : "Inject"} ${button.dataset.label}`);
  });
  $("injection-count").hidden = injections === 0;
  $("injection-count").textContent = String(injections);
  renderLogs();
}

function showScenario() {
  const scenario = scenarios[Number($("scenario").value)];
  $("scenario-description").textContent = scenario.description;
  $("scenario-result").textContent = "Resets the model, then checks each stated observation.";
  $("scenario-result").dataset.status = "ready";
  $("scenario-steps").replaceChildren(...scenario.steps.map((item) => {
    const row = document.createElement("li");
    row.textContent = item.label;
    row.title = item.expected || "Apply this action to the native engine.";
    return row;
  }));
}

function runScenario() {
  if (scenarioRunning || pending || !connected) return;
  playing = false;
  scenarioRunning = true;
  showScenario();
  const scenario = scenarios[Number($("scenario").value)];
  schedule(async () => {
    let index = 0;
    try {
      for (const item of scenario.steps) {
        $("scenario-result").textContent = `Running ${index + 1}/${scenario.steps.length}: ${item.label}`;
        const result = await request(item.action);
        const row = $("scenario-steps").children[index];
        if (item.check && !item.check(result)) {
          row.dataset.status = "failed";
          throw new Error(`Scenario step ${index + 1} failed: expected ${item.expected}.`);
        }
        row.dataset.status = "done";
        index += 1;
      }
      $("scenario-result").textContent = `Completed ${scenario.steps.length} actions. All stated observations matched.`;
      $("scenario-result").dataset.status = "done";
    } catch (error) {
      $("scenario-result").textContent = `Stopped at step ${index + 1}: ${error.message}`;
      $("scenario-result").dataset.status = "failed";
      throw error;
    } finally {
      scenarioRunning = false;
    }
  });
}

document.querySelectorAll("[data-tab]").forEach((tab, index, tabs) => {
  tab.addEventListener("click", () => showTab(tab.dataset.tab));
  tab.addEventListener("keydown", (event) => {
    let target = index;
    if (event.key === "ArrowRight") target = (index + 1) % tabs.length;
    else if (event.key === "ArrowLeft") target = (index + tabs.length - 1) % tabs.length;
    else if (event.key === "Home") target = 0;
    else if (event.key === "End") target = tabs.length - 1;
    else return;
    event.preventDefault();
    showTab(tabs[target].dataset.tab, true);
  });
});
$("play").addEventListener("click", () => { playing = !playing; controls(); });
$("speed").addEventListener("change", controls);
$("step").addEventListener("click", () => { playing = false; act({ action: "advance", value: 1000 }); });
$("refresh").addEventListener("click", () => schedule(() => request()));
$("reset").addEventListener("click", () => { playing = false; act({ action: "reset" }); });
$("house1-switch").addEventListener("click", () => act({ action: "house1", value: !state.house1_switch }));
$("house2-button").addEventListener("click", () => act({ action: "press" }));
$("ack").addEventListener("click", () => act({ action: "ack" }));
$("remote-on").addEventListener("click", () => act({ action: "pump2", value: true }));
$("remote-off").addEventListener("click", () => act({ action: "pump2", value: false }));
$("network").addEventListener("click", () => act({ action: "network", value: !state.network_online }));
for (const house of [1, 2]) {
  $(`isolate${house}`).addEventListener("click", () => act({ action: "isolate", house, value: !state.isolated[house - 1] }));
  $(`tank${house}-form`).addEventListener("submit", (event) => {
    event.preventDefault();
    if (!connected || pending || scenarioRunning) return;
    act({ action: "tank", house, value: Number($(`tank${house}-input`).value) });
  });
}
$("source-form").addEventListener("submit", (event) => {
  event.preventDefault();
  if (!connected || pending || scenarioRunning) return;
  act({ action: "source", value: Number($("source-input").value) });
});
$("advance-form").addEventListener("submit", (event) => {
  event.preventDefault();
  if (!connected || pending || scenarioRunning) return;
  playing = false;
  act({ action: "advance", value: Number($("advance-input").value) });
});
document.querySelectorAll("[data-fault]").forEach((button) => {
  button.addEventListener("click", () => act({ action: "fault", name: button.dataset.fault, value: !state.injected_faults[button.dataset.fault] }));
});
for (const id of ["tank1-input", "tank2-input", "source-input"]) {
  $(id).addEventListener("input", () => {
    $(`${id}-draft`).textContent = `${Number($(id).value).toFixed(1)}%`;
  });
}
$("log-level").addEventListener("change", renderLogs);
$("log-search").addEventListener("input", renderLogs);
scenarios.forEach((scenario, index) => {
  const option = document.createElement("option");
  option.value = String(index);
  option.textContent = scenario.name;
  $("scenario").append(option);
});
$("scenario").addEventListener("change", showScenario);
$("run-scenario").addEventListener("click", runScenario);
showScenario();
controls();
schedule(() => request());

async function clock() {
  if (playing && connected && pending === 0 && !scenarioRunning) {
    await act({ action: "advance", value: 250 * Number($("speed").value) });
  }
  window.setTimeout(clock, 250);
}
window.setTimeout(clock, 250);
