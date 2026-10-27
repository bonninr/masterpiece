// The web console: everything comes from the control API of the program that
// serves this page (protocol/control.yaml). The panels are built from the
// organ as loaded, and follow every change through the event stream.
"use strict";

const api = "api/v1";
const $ = (id) => document.getElementById(id);
const state = { session: -1, divisions: [], switches: [], controls: [], generals: [], divisionals: [],
  eqpresets: [], temperaments: [], program: {}, el: { switch: [], control: [], general: [], divisional: [] },
  all: { switch: [], control: [], rank: [] } };

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
  const pct = `${Math.round((state.program.loadProgress || 0) / 10)}%`;
  $("state").textContent = s === 1 ? `loading ${pct}` : s === 0 ? "no organ loaded" : "";
  const p = $("loadpct");
  if (p) p.textContent = pct;
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
  // While an organ loads, the lists answer "busy": the page shows the load and
  // its progress, and builds when the event stream says the organ is ready.
  const [id0, prog0] = await Promise.all([get(""), get("program")]);
  if (prog0.organState !== 2) {
    state.session = -1;
    state.program = prog0;
    state.divisions = []; state.switches = []; state.controls = [];
    $("organ").textContent = id0.organ || "Masterpiece";
    build();
    showProgram();
    await loadPages();
    return;
  }
  const [id, prog, divs, sw, ctl, gen, divl, eq, tem, trem, encl, wind, pairs] = await Promise.all([
    get(""), get("program"), get("divisions"), get("switches"), get("controls"), get("generals"),
    get("divisionals"), get("eqpresets"), get("temperaments"), get("tremulants"), get("enclosures"), get("windchests"),
    get("pairs")]);
  state.tremulants = trem.items; state.enclosures = encl.items; state.windchests = wind.items; state.pairs = pairs.items;
  state.session = id.session;
  state.program = prog;
  state.divisions = divs.items; state.switches = sw.items; state.controls = ctl.items;
  state.generals = gen.items; state.divisionals = divl.items; state.eqpresets = eq.items; state.temperaments = tem.items;
  $("organ").textContent = id.organ || "Masterpiece";
  document.title = id.organ ? `${id.organ} - Masterpiece` : "Masterpiece";
  build();
  await loadPages();
}

function build() {
  const main = $("panels");
  main.replaceChildren();
  state.el = { switch: [], control: [], general: [], divisional: [], tremulant: [], enclosure: [], windchest: [], division: [], pair: [] };
  state.all = { switch: [], control: [], rank: [] };
  if (!state.switches.length && !state.controls.length) {
    main.append(state.program.organState === 1
      ? el("div", { class: "empty" }, "Loading the organ: ", el("span", { id: "loadpct" }, ""))
      : el("div", { class: "empty" }, "No organ loaded."));
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
  if (pistons.length) main.append(pistonPanel(pistons));
  if (state.controls.length) main.append(controlPanel());
  main.append(levelPanel());
  main.append(organPanel());
  main.append(combinationPanel());
  if (others.length) {
    const p = switchPanel("Other switches", others);
    p.classList.add("wide");
    main.append(p);
  }
  main.append(allPanel());
  fillSelects();
  refreshAll();
}

// Every switch and control the organ has, its internal ones too, loaded when
// opened: a large set has thousands. With a filter, and the keyboards.
function allPanel() {
  const det = el("details", { class: "all" }, el("summary", {}, "Every switch and control"));
  const sec = el("section", { class: "wide" }, det);
  let loaded = false;
  det.addEventListener("toggle", async () => {
    if (!det.open || loaded) return;
    loaded = true;
    const [sw, ctl, kb, rk] = await Promise.all([get("switches?all=1"), get("controls?all=1"), get("keyboards"), get("ranks")]);
    const filter = el("input", { type: "search", placeholder: "Filter by name or number" });
    const swBox = el("div", { class: "stops" });
    const ctlBox = el("div", { class: "controls" });
    for (const s of sw.items) {
      const b = el("button", { class: s.role, "data-name": `${s.index} ${s.name}`.toLowerCase(),
        title: `#${s.index}, file id ${s.odfId}, ${s.role}` }, s.name || `#${s.index}`);
      b.addEventListener("click", () => s.latching ? setValue("switches", s.index, "toggle") : press("switches", s.index));
      b.classList.toggle("on", s.value === 1);
      state.all.switch[s.index] = b;
      swBox.append(b);
    }
    for (const c of ctl.items) {
      const out = el("span", { class: "dim" }, String(c.value));
      const r = el("input", { type: "range", min: 0, max: 127, value: c.value });
      r.addEventListener("input", () => { out.textContent = r.value; setValue("controls", c.index, Number(r.value)); });
      state.all.control[c.index] = { r, out };
      const label = el("label", { class: "control", "data-name": `${c.index} ${c.name}`.toLowerCase() },
        el("span", {}, c.name || `#${c.index}`), r, out);
      ctlBox.append(label);
    }
    const rankBox = el("div", { class: "controls" });
    for (const r of rk.items) {
      const out = el("span", { class: "dim" }, `${(r.value / 10).toFixed(1)} dB`);
      const s = el("input", { type: "range", min: -240, max: 120, step: 5, value: r.value });
      s.addEventListener("input", () => { out.textContent = `${(s.value / 10).toFixed(1)} dB`; setValue("ranks", r.index, Number(s.value)); });
      state.all.rank[r.index] = { r: s, out, db: true };
      rankBox.append(el("label", { class: "control", "data-name": `${r.index} ${r.name}`.toLowerCase() },
        el("span", {}, r.name || `#${r.index}`), s, out));
    }
    filter.addEventListener("input", () => {
      const q = filter.value.trim().toLowerCase();
      for (const e of [...swBox.children, ...ctlBox.children, ...rankBox.children])
        e.style.display = !q || e.dataset.name.includes(q) ? "" : "none";
    });
    const keys = el("div", { class: "dim" }, kb.items.map((k) => `${k.name}: MIDI channel ${k.channel}`).join(" · "));
    det.append(filter,
      el("h2", {}, `Switches (${sw.count})`), swBox,
      el("h2", {}, `Controls (${ctl.count})`), ctlBox,
      el("h2", {}, `Rank levels (${rk.count}), heard from the next note`), rankBox,
      el("h2", {}, "Keyboards"), keys);
  });
  // ?openall opens it at once, for a screenshot.
  if (location.search.includes("openall")) setTimeout(() => { det.open = true; }, 0);
  return sec;
}

function switchButtons(items, kind = "stops") {
  const box = el("div", { class: kind === "pistons" ? "stops pistons" : "stops" });
  for (const s of items) {
    const b = el("button", { class: s.role + (s.available ? "" : " unavailable"),
      title: s.available ? s.name : `${s.name}: not available in this set` }, s.name || `Switch ${s.index}`);
    if (s.available) b.addEventListener("click", () =>
      s.latching ? setValue("switches", s.index, "toggle") : press("switches", s.index));
    state.el.switch[s.index] = b;
    box.append(b);
  }
  return box;
}

function switchPanel(title, items, kind = "stops") {
  return el("section", {}, el("h2", {}, title), switchButtons(items, kind));
}

// The organ's own pistons: those of no division first, the generals among
// them, then each division's divisionals under its name, in the organ's order.
function pistonPanel(pistons) {
  const sec = el("section", {}, el("h2", {}, "The organ's pistons"));
  const loose = pistons.filter((s) => !(s.division >= 0 && state.divisions[s.division]));
  if (loose.length) sec.append(switchButtons(loose, "pistons"));
  state.divisions.forEach((d, i) => {
    const mine = pistons.filter((s) => s.division === i);
    if (mine.length) sec.append(el("div", { class: "dim" }, d.name || `Division ${i + 1}`), switchButtons(mine, "pistons"));
  });
  return sec;
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

// The live levels: a fader for each division and each output pair, heard at
// once; full is the organ as voiced.
function levelText(v) { return v <= 0 ? "off" : `${(-40 * (1 - v / 127)).toFixed(1)} dB`; }
function levelPanel() {
  const box = el("div", { class: "controls" });
  const fader = (kind, plural, item, name) => {
    const out = el("span", { class: "dim" }, levelText(item.value));
    const r = el("input", { type: "range", min: 0, max: 127, value: item.value });
    r.addEventListener("input", () => { out.textContent = levelText(Number(r.value)); setValue(plural, item.index, Number(r.value)); });
    state.el[kind][item.index] = { r, out };
    box.append(el("label", { class: "control" }, el("span", {}, name), r, out));
  };
  state.divisions.forEach((d, i) => fader("division", "divisions", d, d.name || `Division ${i + 1}`));
  for (const p of state.pairs || []) fader("pair", "pairs", p, p.name);
  return el("section", {}, el("h2", {}, "Levels"), box);
}
function showLevel(kind, i, v) {
  const f = state.el[kind][i];
  if (f && document.activeElement !== f.r) { f.r.value = v; f.out.textContent = levelText(v); }
}

// The tremulants, the swell boxes' shades, the wind and the room.
function organPanel() {
  const sec = el("section", {}, el("h2", {}, "Tremulants, enclosures, wind and room"));
  if (state.tremulants.length) {
    const row = el("div", { class: "stops" });
    for (const t of state.tremulants) {
      const b = el("button", { class: "tremulant", title: `${(t.rate / 100).toFixed(2)} Hz, depth ${(t.depth / 10).toFixed(1)}%` }, t.name);
      b.addEventListener("click", () => setValue("tremulants", t.index, t.value === 1 ? 0 : 1));
      b.classList.toggle("on", t.value === 1);
      state.el.tremulant[t.index] = { b, t };
      row.append(b);
    }
    sec.append(row);
  }
  for (const e of state.enclosures) {
    const m = el("meter", { min: 0, max: 127, value: e.value });
    const out = el("span", { class: "dim" }, `${Math.round(e.value / 1.27)}% open`);
    state.el.enclosure[e.index] = { m, out };
    sec.append(el("label", { class: "control" }, el("span", {}, e.name), m, out));
  }
  // The wind, folded: a large set has dozens of reservoirs and chests.
  if (state.windchests.length) {
    const box = el("details", { class: "dim wind" },
      el("summary", {}, `Wind, of nominal pressure (${state.windchests.length})`));
    for (const w of state.windchests) {
      const v = el("span", {}, `${(w.value / 10).toFixed(1)}%`);
      state.el.windchest[w.index] = v;
      box.append(el("div", {}, `${w.name}: `, v));
    }
    sec.append(box);
  }
  const room = el("button", { id: "roomOn", title: "The room: the convolution reverb" }, "Room");
  room.addEventListener("click", () => setProgram("roomOn", state.program.roomOn ? 0 : 1));
  const wet = el("input", { id: "roomWet", type: "range", min: 0, max: 100 });
  wet.addEventListener("input", () => setProgram("roomWet", Number(wet.value)));
  sec.append(el("div", { class: "stepper" }, room, el("span", { class: "dim" }, "wet"), wet));
  return sec;
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
  for (const b of [state.el.switch[i], state.all.switch[i]]) if (b) b.classList.toggle("on", v === 1);
}
function showControl(i, v) {
  for (const c of [state.el.control[i], state.all.control[i]])
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
  if (p === undefined || p === "roomOn") { const b = $("roomOn"); if (b) b.classList.toggle("lit", s.roomOn === 1); }
  if (p === undefined || p === "roomWet") { const w = $("roomWet"); if (w && document.activeElement !== w) w.value = s.roomWet; }
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

// ---- the organ's own console pages -----------------------------------------

// A page is drawn by the program, as its window draws it, and sent as a
// picture; after a change only the parts drawn anew are fetched again. Taps
// and drags go back to the program, which carries them out as clicks.
const page = { list: [], index: -1, layout: 0, serial: -1, busy: false, again: false, down: false,
  chain: Promise.resolve(), move: null, soon: 0 };

// The address names a page (#page=2) for a device kept on one jamb; else
// the choice last made on this device.
function chosenView() {
  const m = location.hash.match(/page=(\d+)/);
  if (m) return m[1];
  try { return localStorage.getItem("mp-view") || "panels"; } catch { return "panels"; }
}

async function loadPages() {
  try { page.list = state.session >= 0 ? (await get("pages")).pages || [] : []; } catch { page.list = []; }
  page.index = -1;
  const v = $("view");
  v.replaceChildren(el("option", { value: "panels" }, "Panels"),
    ...page.list.map((p) => el("option", { value: String(p.index) }, p.name || `Page ${p.index + 1}`)));
  const want = chosenView();
  v.value = page.list.some((p) => String(p.index) === want) ? want : "panels";
  showView();
}

// A set that ships portrait jambs shows them on a screen held upright.
function layoutFor(p) {
  const tall = innerHeight > innerWidth;
  const l = p.layouts.find((l) => l.portrait === tall);
  return l ? l.layout : 0;
}

function showView() {
  const v = $("view").value;
  const i = v === "panels" ? -1 : Number(v);
  $("panels").hidden = i >= 0;
  $("page").hidden = i < 0;
  $("pageOnly").hidden = i < 0;
  if (i < 0) document.body.classList.remove("pageonly");
  const p = page.list.find((p) => p.index === i);
  const layout = p ? layoutFor(p) : 0;
  if (i === page.index && layout === page.layout) return;
  page.index = i;
  page.layout = layout;
  page.serial = -1;
  if (i >= 0) drawPage();
}

async function picture(area) {
  const q = new URLSearchParams({ layout: page.layout, ...(area || {}) });
  const r = await fetch(url(`pages/${page.index}/picture?${q}`), { cache: "no-store" });
  if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.status);
  return { serial: Number(r.headers.get("X-Serial")), bitmap: await createImageBitmap(await r.blob()) };
}

// Brings the canvas up to date: the whole page the first time, then only the
// parts the program drew anew. A call made while one runs runs once more after.
async function drawPage() {
  if (page.busy) { page.again = true; return; }
  page.busy = true;
  try {
    do {
      page.again = false;
      const i = page.index, c = $("pageCanvas"), g = c.getContext("2d");
      if (i < 0) break;
      if (page.serial < 0) {
        const p = await picture();
        if (i !== page.index) continue;
        c.width = p.bitmap.width;
        c.height = p.bitmap.height;
        g.drawImage(p.bitmap, 0, 0);
        page.serial = p.serial;
        continue;
      }
      const ch = await get(`pages/${i}/changes?layout=${page.layout}&since=${page.serial}`);
      if (i !== page.index) continue;
      if (ch.full) { page.serial = -1; page.again = true; continue; }
      const parts = await Promise.all(ch.areas.map((a) => picture(a)));
      if (i !== page.index) continue;
      ch.areas.forEach((a, k) => g.drawImage(parts[k].bitmap, a.x, a.y));
      page.serial = ch.serial;
    } while (page.again);
  } catch (e) {
    page.serial = -1;
    flash(`Page: ${e.message || e}`);
  } finally { page.busy = false; }
}

function pageChanged() {
  if (page.index < 0) return;
  clearTimeout(page.soon);
  page.soon = setTimeout(drawPage, 30);
}

// Where the finger is, in the page's own pixels.
function pagePoint(e) {
  const c = $("pageCanvas"), r = c.getBoundingClientRect();
  return { x: Math.round((e.clientX - r.left) * c.width / r.width),
           y: Math.round((e.clientY - r.top) * c.height / r.height) };
}

// Sent one after another, in order; of the moves waiting, only the latest.
function sendPointer(phase, at) {
  const i = page.index, layout = page.layout;
  if (phase === "move") {
    const first = page.move === null;
    page.move = at;
    if (!first) return;
    page.chain = page.chain.then(() => {
      const a = page.move;
      page.move = null;
      return send("POST", `pages/${i}/pointer`, { layout, phase: "move", ...a });
    }).then(pageChanged);
    return;
  }
  page.chain = page.chain.then(() => send("POST", `pages/${i}/pointer`, { layout, phase, ...at })).then(pageChanged);
}

{
  const c = $("pageCanvas");
  c.addEventListener("pointerdown", (e) => {
    if (page.index < 0 || page.serial < 0) return;
    e.preventDefault();
    c.setPointerCapture(e.pointerId);
    page.down = true;
    sendPointer("down", pagePoint(e));
  });
  c.addEventListener("pointermove", (e) => { if (page.down) sendPointer("move", pagePoint(e)); });
  const up = (e) => {
    if (!page.down) return;
    page.down = false;
    sendPointer("up", pagePoint(e));
  };
  c.addEventListener("pointerup", up);
  c.addEventListener("pointercancel", up);
  c.addEventListener("contextmenu", (e) => e.preventDefault());
}
$("view").addEventListener("change", () => {
  const v = $("view").value;
  try { localStorage.setItem("mp-view", v); } catch {}
  history.replaceState(null, "", v === "panels" ? location.pathname + location.search : `#page=${v}`);
  showView();
});
$("pageOnly").addEventListener("click", () => document.body.classList.add("pageonly"));
$("pageBack").addEventListener("click", () => document.body.classList.remove("pageonly"));
addEventListener("resize", showView);
addEventListener("hashchange", () => { const v = chosenView(); if ($("view").value !== v) { $("view").value = v; showView(); } });
// What changes with no event (a key lit, a control being learned) is looked
// for every two seconds as well.
setInterval(() => { if (page.index >= 0 && !document.hidden) drawPage(); }, 2000);

// ---- the event stream -----------------------------------------------------

// A reload asked for while one runs runs again after it: the organ can become
// ready while the page is still reading the state of the load before.
let reloading = false, reloadAgain = false;
async function reload() {
  if (reloading) { reloadAgain = true; return; }
  reloading = true;
  try {
    do {
      reloadAgain = false;
      try { await load(); } catch (e) { flash(String(e.message || e)); }
    } while (reloadAgain);
  } finally { reloading = false; }
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
    if (state.program.organState !== 2) return;  // nothing built yet: the ready event builds it
    if (c.session !== state.session) return reload();
    pageChanged();
    if (c.type === "switch") showSwitch(c.index, c.value);
    else if (c.type === "control") showControl(c.index, c.value);
    else if (c.type === "tremulant") { const t = state.el.tremulant[c.index]; if (t) { t.t.value = c.value; t.b.classList.toggle("on", c.value === 1); } }
    else if (c.type === "enclosure") { const e = state.el.enclosure[c.index]; if (e) { e.m.value = c.value; e.out.textContent = `${Math.round(c.value / 1.27)}% open`; } }
    else if (c.type === "division" || c.type === "pair") showLevel(c.type, c.index, c.value);
    else if (c.type === "windchest") { const w = state.el.windchest[c.index]; if (w) w.textContent = `${(c.value / 10).toFixed(1)}%`; }
    else if (c.type === "rank") { const r = state.all.rank[c.index]; if (r && document.activeElement !== r.r) { r.r.value = c.value; r.out.textContent = `${(c.value / 10).toFixed(1)} dB`; } }
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
