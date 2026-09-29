// FB200 Studio UI: editors built from parameter_docs, chat over /api/chat.
const $ = (sel) => document.querySelector(sel);
const HDR = { "Content-Type": "application/json", "X-FB200-App": "1" };
const HID_BLOCKS = ["gate", "comp", "amp", "cab", "mod", "reverb"];

let DOCS = null;          // parameter_docs
let seq = 0;              // next chat event
const measurements = [];  // audio_test results with a response

async function getJSON(url) {
  const r = await fetch(url);
  return r.json();
}

async function post(url, body) {
  const r = await fetch(url, { method: "POST", headers: HDR, body: JSON.stringify(body || {}) });
  const data = await r.json().catch(() => ({}));
  return { status: r.status, ...data };
}

// Run a tool; persistent tools ask first (the server refuses them without `confirmed`).
async function tool(name, args = {}, { quiet = false } = {}) {
  let res = await post(`/api/tools/${name}`, { args });
  if (res.status === 409 && res.confirm) {
    if (!(await ask(`Run ${name}?`, `This ${res.confirm}.`, args))) return null;
    res = await post(`/api/tools/${name}`, { args, confirmed: true });
  }
  if (!res.ok) {
    if (!quiet) toast(`${name}: ${res.error}`);
    return null;
  }
  return res.result;
}

function toast(text) {
  const div = document.createElement("div");
  div.className = "msg err";
  div.textContent = text;
  $("#chat-log").append(div);
  div.scrollIntoView({ block: "end" });
}

function ask(title, text, args) {
  const dlg = $("#confirm-dlg");
  $("#confirm-title").textContent = title;
  $("#confirm-text").textContent = text;
  $("#confirm-args").textContent = args && Object.keys(args).length ? JSON.stringify(args, null, 1) : "";
  dlg.returnValue = "";
  dlg.showModal();
  return new Promise((resolve) => dlg.addEventListener("close", () => resolve(dlg.returnValue === "yes"), { once: true }));
}

// ---------------------------------------------------------------- editors

function fieldWidget(name, spec, value, onChange) {
  const row = document.createElement("div");
  row.className = "field" + (/^no effect/.test(spec.meaning) ? " dead" : "");
  row.title = spec.meaning;
  const label = document.createElement("label");
  label.textContent = name;
  const out = document.createElement("output");
  let input;
  if (spec.unit === "bool") {
    input = document.createElement("input");
    input.type = "checkbox";
    input.checked = !!value;
    input.addEventListener("change", () => onChange(input.checked));
  } else if (spec.options) {
    input = document.createElement("select");
    for (const [k, v] of Object.entries(spec.options)) input.add(new Option(`${k} ${v}`, k));
    input.value = String(value ?? spec.min);
    input.addEventListener("change", () => onChange(Number(input.value)));
  } else {
    input = document.createElement("input");
    input.type = "range";
    input.min = spec.min;
    input.max = spec.max;
    input.step = spec.unit === "q" ? 0.05 : 1;
    input.value = value ?? spec.min;
    out.textContent = input.value;
    input.addEventListener("input", () => (out.textContent = input.value));
    input.addEventListener("change", () => onChange(Number(input.value)));
  }
  input.id = `f-${Math.random().toString(36).slice(2)}`;
  input.setAttribute("aria-label", `${name}: ${spec.meaning}`);
  label.htmlFor = input.id;
  row.append(label, input, out);
  return row;
}

function blockCard(name, doc, values, setter) {
  const card = document.createElement("details");
  card.className = "card";
  card.open = true;
  card.dataset.block = name;
  const en = values.enabled ?? values.on;
  if (en === 0 || en === false) card.classList.add("bypassed");
  const sum = document.createElement("summary");
  sum.innerHTML = `<h3>${name}</h3>`;
  const desc = document.createElement("p");
  desc.className = "desc";
  desc.textContent = doc.summary;
  const fields = document.createElement("div");
  fields.className = "fields";
  for (const [f, spec] of Object.entries(doc.fields)) {
    fields.append(fieldWidget(f, spec, values[f], (v) => setter(f, v)));
  }
  card.append(sum, desc, fields);
  return card;
}

async function renderBlocks() {
  const host = $("#blocks");
  const [fx, delay, eq] = await Promise.all([
    tool("get_effects", {}, { quiet: true }),
    tool("set_delay", {}, { quiet: true }),
    tool("set_eq", {}, { quiet: true }),
  ]);
  host.replaceChildren();
  if (!fx) {
    host.innerHTML = '<p class="muted">No pedal on HID: connect the FB200 (open firmware) over USB and press Refresh.</p>';
  }
  const order = ["gate", "comp", "amp", "cab", "eq", "mod", "delay", "reverb"];
  for (const name of order) {
    if (HID_BLOCKS.includes(name) && fx) {
      const doc = DOCS.blocks[name];
      host.append(blockCard(name, doc, fx[name], async (f, v) => {
        const args = { [f]: f === "enabled" ? !!v : v };
        const back = await tool(doc.tool, args);
        if (back && f === (name === "amp" ? "model" : "type")) renderBlocks();   // defaults loaded
      }));
    } else if (name === "delay" && delay) {
      host.append(blockCard("delay", DOCS.blocks.delay, delay, (f, v) => tool("set_delay", { [f]: v })));
    } else if (name === "eq" && eq) {
      host.append(eqCard(eq));
    }
  }
}

function eqCard(eq) {
  const doc = DOCS.eq;
  const values = { on: eq.on, hpf_hz: eq.hpf_hz, lpf_hz: eq.lpf_hz };
  const top = { on: doc.fields.on, hpf_hz: doc.fields.hpf_hz, lpf_hz: doc.fields.lpf_hz };
  const card = blockCard("eq", { summary: doc.summary, fields: top }, values, (f, v) => tool("set_eq", { [f]: v }));
  const table = document.createElement("table");
  table.className = "eqbands";
  table.innerHTML = "<tr><th>band</th><th>Hz</th><th>dB</th><th>q</th></tr>";
  for (const b of eq.bands) {
    const tr = document.createElement("tr");
    const cells = [["freq_hz", 30, 10000, 1], ["gain_db", -15, 15, 0.5], ["q", 0.3, 4, 0.05]].map(([k, lo, hi, st]) => {
      const inp = document.createElement("input");
      Object.assign(inp, { type: "number", min: lo, max: hi, step: st, value: b[k] });
      inp.setAttribute("aria-label", `band ${b.band} ${k}`);
      inp.addEventListener("change", () => tool("set_eq", { band: b.band, [k]: Number(inp.value) }));
      const td = document.createElement("td");
      td.append(inp);
      return td;
    });
    const th = document.createElement("td");
    th.textContent = b.band;
    tr.append(th, ...cells);
    table.append(tr);
  }
  card.querySelector(".fields").append(table);
  return card;
}

async function renderPresets() {
  const res = await tool("preset_list", {}, { quiet: true });
  const list = $("#preset-list");
  list.replaceChildren();
  if (!res) return;
  for (const p of res.presets) {
    const li = document.createElement("li");
    li.className = p.index === res.current ? "cur" : "";
    const bank = `${Math.floor(p.index / 4) + 1}${"ABCD"[p.index % 4]}`;
    li.innerHTML = `<span class="idx">${bank}</span><span class="nm"></span><button class="ren" type="button" title="Rename">Rename</button>`;
    li.querySelector(".nm").textContent = p.name || "(empty)";
    li.addEventListener("click", async (ev) => {
      if (ev.target.classList.contains("ren")) {
        const name = prompt(`New name for preset ${bank} (max 20)`, p.name);
        if (name && (await tool("rename_preset", { index: p.index, name }))) renderPresets();
        return;
      }
      if (await tool("preset", { index: p.index })) reloadAll();
    });
    list.append(li);
  }
}

async function renderIrs() {
  const slots = await tool("ir_list", {}, { quiet: true });
  const list = $("#ir-list");
  const sel = $("#ir-slot");
  list.replaceChildren();
  sel.replaceChildren();
  for (let s = 1; s <= 9; s++) sel.add(new Option(`${s}`, s));
  if (!slots) return;
  for (const s of slots) {
    const li = document.createElement("li");
    li.textContent = s.name ?? "(empty)";
    if (s.name) {
      const del = document.createElement("button");
      del.type = "button";
      del.textContent = "Delete";
      del.addEventListener("click", async () => (await tool("ir_delete", { slot: s.slot })) && renderIrs());
      const use = document.createElement("button");
      use.type = "button";
      use.textContent = "Use as cab";
      use.addEventListener("click", async () => (await tool("set_cab", { enabled: true, type: 10 + s.slot })) && renderBlocks());
      li.append(use, del);
    }
    list.append(li);
  }
}

async function importIr(ev) {
  ev.preventDefault();
  const file = $("#ir-file").files[0];
  if (!file) return toast("choose a WAV file first");
  const up = await fetch(`/api/upload?filename=${encodeURIComponent(file.name)}`, {
    method: "POST", headers: { "X-FB200-App": "1" }, body: file,
  }).then((r) => r.json());
  if (!up.ok) return toast(`upload: ${up.error}`);
  const num = (id) => ($(id).value ? Number($(id).value) : null);
  const args = {
    slot: Number($("#ir-slot").value), wav_path: up.path, name: $("#ir-name").value || up.name,
    channel: $("#ir-channel").value, trim: $("#ir-trim").checked, lowcut: num("#ir-lowcut"),
    highcut: num("#ir-highcut"), minphase: $("#ir-minphase").checked, normalize: $("#ir-normalize").checked,
  };
  if (await tool("ir_import", args)) renderIrs();
}

async function renderSmall() {
  const g = DOCS.global;
  const drums = $("#drums");
  drums.replaceChildren();
  const d = await tool("drums", {}, { quiet: true });
  for (const [f, spec] of Object.entries(g.drums)) {
    drums.append(fieldWidget(f, spec, d ? d[f] : undefined, (v) => tool("drums", { [f]: v })));
  }
  const out = $("#output");
  out.replaceChildren();
  for (const [f, spec] of Object.entries(g.set_output)) {
    out.append(fieldWidget(f, spec, f === "gain_db" ? 0 : false, (v) => tool("set_output", { [f]: v })));
  }
  const glob = $("#globals");
  glob.replaceChildren();
  const s = await tool("settings", {}, { quiet: true });
  for (const [f, spec] of Object.entries(g.settings)) {
    if (f === "tuner") continue;
    glob.append(fieldWidget(f, spec, s ? s[f] : undefined, (v) => tool("settings", { [f]: Number(v) })));
  }
}

async function renderStatus() {
  const st = await getJSON("/api/status");
  const pill = $("#conn");
  const ok = st.connected && st.console;
  pill.className = `pill ${ok ? "on" : "off"}`;
  pill.textContent = ok ? `connected ${st.info.firmware_version || ""}` : st.connected ? "HID only" : "not connected";
  pill.title = st.error || "";
  $("#current").textContent = st.preset ? `preset ${st.preset.index}` : "";
}

async function reloadAll() {
  await renderStatus();
  await Promise.all([renderPresets(), renderBlocks(), renderIrs(), renderSmall()]);
}

// ---------------------------------------------------------------- chat

const steps = new Map();   // tool_use id -> details element

function add(el) {
  $("#chat-log").append(el);
  el.scrollIntoView({ block: "end" });
  return el;
}

function msg(cls, text) {
  const div = document.createElement("div");
  div.className = `msg ${cls}`;
  div.textContent = text;
  return add(div);
}

function pre(value) {
  const p = document.createElement("pre");
  p.textContent = typeof value === "string" ? value : JSON.stringify(value, null, 1);
  return p;
}

function renderEvent(ev) {
  switch (ev.type) {
    case "user": msg("user", ev.text); break;
    case "text": msg("bot", ev.text); break;
    case "thinking": {
      const d = document.createElement("details");
      d.className = "step";
      d.innerHTML = "<summary>thinking</summary>";
      d.append(pre(ev.text));
      add(d);
      break;
    }
    case "tool_use": {
      const d = document.createElement("details");
      d.className = "step";
      d.innerHTML = `<summary>tool <b></b> <span class="state">...</span></summary>`;
      d.querySelector("b").textContent = ev.tool;
      d.append(pre(ev.input));
      steps.set(ev.id, d);
      add(d);
      break;
    }
    case "tool_result": {
      const d = steps.get(ev.id);
      if (d) {
        const s = d.querySelector(".state");
        s.textContent = ev.ok ? "ok" : "failed";
        s.className = `state ${ev.ok ? "good" : "bad"}`;
        d.append(pre(ev.result));
      }
      if (ev.tool === "audio_test" && ev.ok) showMeasurement(ev.result);
      break;
    }
    case "confirm": {
      const box = document.createElement("div");
      box.className = "ask";
      box.id = `ask-${ev.id}`;
      box.innerHTML = `<b></b> <span></span><menu><button class="warn" value="1">Allow</button><button value="0">Decline</button></menu>`;
      box.querySelector("b").textContent = ev.tool;
      box.querySelector("span").textContent = `${ev.reason}. ${JSON.stringify(ev.input)}`;
      box.querySelectorAll("button").forEach((b) => b.addEventListener("click", () =>
        post("/api/chat/confirm", { id: ev.id, approve: b.value === "1" })));
      add(box);
      break;
    }
    case "confirmed": {
      const box = document.getElementById(`ask-${ev.id}`);
      if (box) box.querySelector("menu").textContent = ev.approved ? "allowed" : "declined";
      break;
    }
    case "error": msg("err", ev.message); break;
    case "done": reloadAll(); break;
    case "reset": $("#chat-log").replaceChildren(); steps.clear(); break;
    default: break;
  }
}

function showMeasurement(r) {
  const lvl = r.left ? `${r.left.rms_dbfs} dBFS rms, peak ${r.left.peak_dbfs}` : "";
  const extra = r.thd ? `, THD ${JSON.stringify(r.thd)}` : "";
  const div = document.createElement("div");
  div.className = "meas";
  div.textContent = `audio_test ${r.signal}: ${lvl}${extra}${r.warning ? ` (${r.warning})` : ""}`;
  add(div);
  if (r.response_db) {
    measurements.push({ label: `#${measurements.length + 1} ${r.left?.rms_dbfs ?? ""} dBFS`, bands: r.response_db });
    drawChart();
  }
}

function drawChart() {
  const svg = $("#chart");
  $("#measure").hidden = false;
  const shown = measurements.slice(-2);
  const keys = Object.keys(shown[shown.length - 1].bands);
  const W = 320, H = 140, L = 30, B = 18, lo = -24, hi = 12;
  const x = (i) => L + (i * (W - L - 8)) / Math.max(1, keys.length - 1);
  const y = (v) => 6 + ((hi - Math.max(lo, Math.min(hi, v))) * (H - B - 6)) / (hi - lo);
  let s = "";
  for (const g of [12, 0, -12, -24]) {
    s += `<line x1="${L}" x2="${W - 4}" y1="${y(g)}" y2="${y(g)}" stroke="var(--line)"/>`;
    s += `<text x="2" y="${y(g) + 4}" font-size="9" fill="var(--muted)">${g}</text>`;
  }
  keys.forEach((k, i) => {
    const t = Number(k) >= 1000 ? `${Number(k) / 1000}k` : k;
    s += `<text x="${x(i) - 8}" y="${H - 4}" font-size="9" fill="var(--muted)">${t}</text>`;
  });
  const colors = shown.length === 2 ? ["var(--chart1)", "var(--chart2)"] : ["var(--chart2)"];
  shown.forEach((m, j) => {
    const pts = keys.filter((k) => k in m.bands).map((k) => `${x(keys.indexOf(k))},${y(m.bands[k])}`).join(" ");
    s += `<polyline points="${pts}" fill="none" stroke="${colors[j]}" stroke-width="2"/>`;
    for (const k of keys) if (k in m.bands) {
      s += `<circle cx="${x(keys.indexOf(k))}" cy="${y(m.bands[k])}" r="3" fill="${colors[j]}"><title>${k} Hz: ${m.bands[k]} dB</title></circle>`;
    }
  });
  svg.innerHTML = s;
  $("#chart-legend").innerHTML = shown.map((m, j) =>
    `<span><i style="background:${colors[j]}"></i>${j === 0 && shown.length === 2 ? "before" : "latest"} ${m.label}</span>`).join("");
}

async function pollChat() {
  for (;;) {
    try {
      const res = await getJSON(`/api/chat/events?after=${seq}&wait=20`);
      for (const ev of res.events) {
        seq = ev.seq + 1;
        renderEvent(ev);
      }
    } catch {
      await new Promise((r) => setTimeout(r, 2000));
    }
  }
}

// ---------------------------------------------------------------- settings

async function loadConfig() {
  const cfg = await getJSON("/api/config");
  for (const sel of [$("#chat-model"), $("#set-model")]) {
    sel.replaceChildren(...cfg.models.map((m) => new Option(m, m)));
    sel.value = cfg.model;
  }
  $("#set-key-state").textContent = cfg.key_set
    ? `A key is set (${cfg.key_source === "env" ? "ANTHROPIC_API_KEY" : cfg.path}).`
    : "No key: set ANTHROPIC_API_KEY or enter one here (stored in " + cfg.path + ", mode 600).";
  return cfg;
}

async function main() {
  DOCS = (await getJSON("/api/docs")).parameters;
  await loadConfig();
  $("#refresh").addEventListener("click", reloadAll);
  $("#save").addEventListener("click", async () => (await tool("save_preset")) && renderPresets());
  $("#ir-form").addEventListener("submit", importIr);
  $("#tuner-on").addEventListener("click", async () => {
    const t = await tool("tuner", { on: true });
    const notes = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    $("#tuner-read").textContent = t && t.valid ? `${notes[t.note] ?? t.note}${t.oct} ${t.cents > 0 ? "+" : ""}${t.cents}c ${t.freq ?? ""} Hz` : "no signal";
  });
  $("#tuner-off").addEventListener("click", async () => { await tool("tuner", { on: false }); $("#tuner-read").textContent = "--"; });
  $("#chat-form").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const text = $("#chat-input").value.trim();
    if (!text) return;
    const res = await post("/api/chat", { text, model: $("#chat-model").value });
    if (!res.ok) return toast(res.error);
    $("#chat-input").value = "";
  });
  $("#chat-input").addEventListener("keydown", (ev) => {
    if (ev.key === "Enter" && !ev.shiftKey) { ev.preventDefault(); $("#chat-form").requestSubmit(); }
  });
  $("#chat-stop").addEventListener("click", () => post("/api/chat/stop"));
  $("#chat-reset").addEventListener("click", async () => {
    const r = await post("/api/chat/reset");
    if (!r.ok) toast(r.error);
  });
  $("#open-settings").addEventListener("click", () => $("#settings-dlg").showModal());
  $("#settings-dlg").addEventListener("close", async () => {
    const v = $("#settings-dlg").returnValue;
    if (v === "save" || v === "clear") {
      const body = { model: $("#set-model").value };
      if (v === "clear") body.api_key = "";
      else if ($("#set-key").value) body.api_key = $("#set-key").value;
      const r = await post("/api/config", body);
      if (r.status !== 200) toast(r.error);
    }
    $("#set-key").value = "";
    loadConfig();
  });
  pollChat();
  reloadAll();
}

main();
