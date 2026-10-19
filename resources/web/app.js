// The web console: everything comes from the control API of the program that
// serves this page (protocol/control.yaml). The panels are built from the
// organ as loaded, and follow every change through the event stream.
"use strict";

const api = "api/v1";
const $ = (id) => document.getElementById(id);
const state = { session: -1, divisions: [], switches: [], controls: [], generals: [], divisionals: [],
  eqpresets: [], temperaments: [], program: {}, el: { switch: [], control: [], general: [], divisional: [] } };

function headers() {
  const h = { "Content-Type": "application/json" };
  const t = localStorage.getItem("mp-token");
  if (t) h.Authorization = "Bearer " + t;
  return h;
}
const url = (path) => (path ? `${api}/${path}` : api);
async function get(path) {
  const r = await fetch(url(path));
  if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.status);
  return r.json();
}
async function send(method, path, body) {
  const r = await fetch(url(path), { method, headers: headers(), body: body ? JSON.stringify(body) : "{}" });
  if (!r.ok) {
    const e = (await r.json().catch(() => ({}))).error || r.status;
    flash(e === "not-allowed" ? "Changes are not allowed, or the token is wrong (Access, below)" : `Refused: ${e}`);
  }
}
const setValue = (type, i, value) => send("PUT", `${type}/${i}/value`, { value });
const press = (type, i) => send("POST", `${type}/${i}/press`);
const setProgram = (prop, value) => send("PUT", `program/${prop}`, { value });

function flash(text) {
  $("state").textContent = text;
  clearTimeout(flash.t);
  flash.t = setTimeout(showState, 4000);
}
function showState() {
  const s = state.program.organState;
  $("state").textContent = s === 1 ? `loading ${Math.round((state.program.loadProgress || 0) / 10)}%` : s === 0 ? "no organ loaded" : "";
}

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v);
  }
  for (const c of children) e.append(c);
  return e;
}

// ---- building the panels -------------------------------------------------

async function load() {
  const [id, prog, divs, sw, ctl, gen, divl, eq, tem] = await Promise.all([
    get(""), get("program"), get("divisions"), get("switches"), get("controls"), get("generals"),
    get("divisionals"), get("eqpresets"), get("temperaments")]);
  state.session = id.session;
  state.program = prog;
  state.divisions = divs.items; state.switches = sw.items; state.controls = ctl.items;
  state.generals = gen.items; state.divisionals = divl.items; state.eqpresets = eq.items; state.temperaments = tem.items;
  $("organ").textContent = id.organ || "Masterpiece";
  document.title = id.organ ? `${id.organ} - Masterpiece` : "Masterpiece";
  build();
}

function build() {
  const main = $("panels");
  main.replaceChildren();
  state.el = { switch: [], control: [], general: [], divisional: [] };
  if (!state.switches.length && !state.controls.length) {
    main.append(el("div", { class: "empty" }, state.program.organState === 1 ? "Loading the organ..." : "No organ loaded."));
    fillSelects();
    return;
  }
  // One panel per division, in the organ's order, then what belongs to none.
  const groups = state.divisions.map((d, i) => ({ name: d.name || `Division ${i + 1}`, items: [] }));
  const none = { name: "General", items: [] };
  const pistons = [], others = [];
  for (const s of state.switches) {
    if (s.role === "piston") pistons.push(s);
    else if (s.role === "other") others.push(s);
    else (s.division >= 0 && groups[s.division] ? groups[s.division] : none).items.push(s);
  }
  for (const g of [...groups, none]) if (g.items.length) main.append(switchPanel(g.name, g.items));
  if (pistons.length) main.append(switchPanel("The organ's pistons", pistons, "pistons"));
  if (state.controls.length) main.append(controlPanel());
  main.append(combinationPanel());
  if (others.length) {
    const p = switchPanel("Other switches", others);
    p.classList.add("wide");
    main.append(p);
  }
  fillSelects();
  refreshAll();
}

function switchPanel(title, items, kind = "stops") {
  const box = el("div", { class: kind === "pistons" ? "stops pistons" : "stops" });
  for (const s of items) {
    const b = el("button", { class: s.role + (s.available ? "" : " unavailable"),
      title: s.available ? s.name : `${s.name}: not available in this set` }, s.name || `Switch ${s.index}`);
    if (s.available) b.addEventListener("click", () =>
      s.latching ? setValue("switches", s.index, "toggle") : press("switches", s.index));
    state.el.switch[s.index] = b;
    box.append(b);
  }
  return el("section", {}, el("h2", {}, title), box);
}

function controlPanel() {
  const box = el("div", { class: "controls" });
  for (const c of state.controls) {
    const out = el("span", { class: "dim" });
    const r = el("input", { type: "range", min: 0, max: 127 });
    r.addEventListener("input", () => { out.textContent = r.value; setValue("controls", c.index, Number(r.value)); });
    state.el.control[c.index] = { r, out };
    box.append(el("label", { class: "control" }, el("span", {}, c.name || `Control ${c.index}`), r, out));
  }
  return el("section", {}, el("h2", {}, "Shoes and controls"), box);
}

function combinationPanel() {
  const sec = el("section", { class: "wide" }, el("h2", {}, "Combinations"));
  const gen = el("div", { class: "stops pistons" });
  for (const g of state.generals) {
    const b = el("button", { title: `General ${g.name}` }, g.name);
    b.addEventListener("click", () => press("generals", g.index));
    state.el.general[g.index] = b;
    gen.append(b);
  }
  gen.append(el("button", { onclick: () => press("commands", 1), title: "General cancel" }, "0"));
  const setter = el("button", { id: "setter", title: "While on, a piston stores what is drawn" }, "Set");
  setter.addEventListener("click", () => setProgram("setter", state.program.setter ? 0 : 1));
  gen.append(setter);
  sec.append(el("div", { class: "dim" }, "Generals"), gen);

  const byDiv = new Map();
  for (const d of state.divisionals) {
    if (!byDiv.has(d.division)) byDiv.set(d.division, []);
    byDiv.get(d.division).push(d);
  }
  for (const [div, list] of byDiv) {
    const row = el("div", { class: "stops pistons" });
    for (const d of list) {
      const b = el("button", { title: d.name }, d.name.split(" ").pop());
      b.addEventListener("click", () => press("divisionals", d.index));
      state.el.divisional[d.index] = b;
      row.append(b);
    }
    const name = state.divisions[div] ? state.divisions[div].name : "Divisionals";
    sec.append(el("div", { class: "dim" }, name), row);
  }

  const frame = el("span", { id: "frame" }, "");
  sec.append(el("div", { class: "dim" }, "Sequencer"), el("div", { class: "stepper" },
    el("button", { onclick: () => press("commands", 4), title: "First frame" }, "|<"),
    el("button", { onclick: () => press("commands", 3), title: "Previous frame" }, "<"),
    frame,
    el("button", { onclick: () => press("commands", 2), title: "Next frame" }, ">"),
    el("button", { onclick: () => press("commands", 5), title: "Last frame" }, ">|")));
  return sec;
}

function fillSelects() {
  const fill = (sel, items, current) => {
    sel.replaceChildren(...items.map((it) => el("option", { value: it.index }, it.name)));
    if (current >= 0) sel.value = current;
  };
  fill($("temperament"), state.temperaments, state.program.temperament);
  fill($("eqpreset"), [{ index: -1, name: "Edited or off" }, ...state.eqpresets], state.program.eqPreset ?? -1);
}

// ---- showing values -------------------------------------------------------

function showSwitch(i, v) {
  const b = state.el.switch[i];
  if (b) b.classList.toggle("on", v === 1);
}
function showControl(i, v) {
  const c = state.el.control[i];
  if (c && document.activeElement !== c.r) { c.r.value = v; c.out.textContent = v; }
}
function showPiston(kind, i, v) {
  const b = state.el[kind][i];
  if (b) b.classList.toggle("lit", v === 1);
}
function showProgram(p) {
  const s = state.program;
  if (p === undefined || p === "masterGain") {
    if (document.activeElement !== $("gain")) $("gain").value = s.masterGain;
    $("gainText").textContent = `${(s.masterGain / 10).toFixed(1)} dB`;
  }
  if (p === undefined || p === "transpose") $("trans").textContent = s.transpose > 0 ? `+${s.transpose}` : s.transpose;
  if (p === undefined || p === "temperament") $("temperament").value = s.temperament;
  if (p === undefined || p === "eqPreset") $("eqpreset").value = s.eqPreset;
  if (p === undefined || p === "stepperFrame") { const f = $("frame"); if (f) f.textContent = s.stepperFrame ? `frame ${s.stepperFrame}` : "no frame"; }
  if (p === undefined || p === "setter") { const b = $("setter"); if (b) b.classList.toggle("lit", s.setter === 1); }
  if (p === undefined || p === "soundingVoices") $("voices").textContent = `${s.soundingVoices ?? 0} voices`;
  if (p === undefined || p === "lateBlocks") $("late").textContent = s.lateBlocks ? `${s.lateBlocks} late blocks` : "";
  if (p === undefined || p === "organState" || p === "loadProgress") showState();
}

async function refreshAll() {
  const [sw, ctl, gen, divl] = await Promise.all([get("switches/values"), get("controls/values"),
    get("generals/values"), get("divisionals/values")]);
  sw.values.forEach((v, i) => showSwitch(i, v));
  ctl.values.forEach((v, i) => showControl(i, v));
  gen.values.forEach((v, i) => showPiston("general", i, v));
  divl.values.forEach((v, i) => showPiston("divisional", i, v));
  showProgram();
}

// ---- the event stream -----------------------------------------------------

let reloading = false;
async function reload() {
  if (reloading) return;
  reloading = true;
  try { await load(); } catch (e) { flash(String(e.message || e)); } finally { reloading = false; }
}

function connect() {
  const es = new EventSource(`${api}/events`);
  es.addEventListener("hello", (e) => {
    $("conn").textContent = "connected"; $("conn").className = "on";
    if (JSON.parse(e.data).session !== state.session) reload();
  });
  es.addEventListener("change", (e) => {
    const c = JSON.parse(e.data);
    if (c.type === "program") {
      state.program[c.property] = c.value;
      if (c.property === "session" && c.value !== state.session) return reload();
      if (c.property === "organState" && c.value === 2 && state.session !== c.session) return reload();
      return showProgram(c.property);
    }
    if (c.session !== state.session) return reload();
    if (c.type === "switch") showSwitch(c.index, c.value);
    else if (c.type === "control") showControl(c.index, c.value);
    else if (c.type === "general") showPiston("general", c.index, c.value);
    else if (c.type === "divisional") showPiston("divisional", c.index, c.value);
    else if (c.type === "eqpreset" && c.value === 1) $("eqpreset").value = c.index;
  });
  es.onerror = () => { $("conn").textContent = "reconnecting"; $("conn").className = "off"; };
}

// ---- the top bar ----------------------------------------------------------

$("gain").addEventListener("input", (e) => { $("gainText").textContent = `${(e.target.value / 10).toFixed(1)} dB`; setProgram("masterGain", Number(e.target.value)); });
$("transUp").addEventListener("click", () => setProgram("transpose", Math.min(12, (state.program.transpose || 0) + 1)));
$("transDown").addEventListener("click", () => setProgram("transpose", Math.max(-12, (state.program.transpose || 0) - 1)));
$("temperament").addEventListener("change", (e) => press("temperaments", Number(e.target.value)));
$("eqpreset").addEventListener("change", (e) => { const i = Number(e.target.value); if (i >= 0) press("eqpresets", i); });
$("panic").addEventListener("click", () => press("commands", 0));
$("token").value = localStorage.getItem("mp-token") || "";
$("token").addEventListener("change", (e) => localStorage.setItem("mp-token", e.target.value));

// ?snapshot draws the page once and leaves the stream closed, for a
// screenshot taken by a browser that waits for the network to go quiet.
reload().then(() => { if (!location.search.includes("snapshot")) connect(); });
