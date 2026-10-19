#include "ControlModel.h"

#include "../mp_audio/MasterpieceProcessor.h"
#include "../mp_core/Temperament.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace mp::api {

namespace {

template <typename Map>
std::vector<int64_t> sortedIds(const Map& m) {
  std::vector<int64_t> ids;
  ids.reserve(m.size());
  for (const auto& [id, v] : m) {
    (void)v;
    ids.push_back(static_cast<int64_t>(id));
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

}  // namespace

ControlModel::ControlModel(MasterpieceProcessor& p) : proc_(p) {}

int ControlModel::session() { return proc_.organLoads(); }

bool ControlModel::ensure(Error& error) {
  if (proc_.organLoading()) {
    error = Error::Busy;
    return false;
  }
  if (builtFor_ != proc_.organLoads()) rebuild();
  error = Error::None;
  return true;
}

void ControlModel::rebuild() {
  builtFor_ = proc_.organLoads();
  const OrganModel& m = proc_.organModel();
  switches_.clear();
  controls_.clear();
  divisions_.clear();
  keyboards_.clear();
  divisionals_.clear();
  temperaments_.clear();

  for (int64_t id : sortedIds(m.divisions)) {
    const auto& d = m.divisions.at(static_cast<Id>(id));
    divisions_.push_back({id, d.name, -1});
  }

  // What each switch the player moves does: the stops, couplers and
  // tremulants it reaches through the organ's wiring.
  std::unordered_map<int64_t, Role> roles;
  std::unordered_map<int64_t, int64_t> stopOf;  // player switch -> a stop it draws
  for (const auto& [stopId, stop] : m.stops) {
    if (stop.controllingSwitchId == 0) continue;
    const auto top = static_cast<int64_t>(proc_.playerSwitchFor(stop.controllingSwitchId));
    roles[top] = Role::Stop;
    stopOf.emplace(top, static_cast<int64_t>(stopId));
  }
  for (const auto& [tid, trem] : m.tremulants) {
    (void)tid;
    if (trem.controllingSwitchId == 0) continue;
    const auto top = static_cast<int64_t>(proc_.playerSwitchFor(trem.controllingSwitchId));
    if (!roles.count(top)) roles[top] = Role::Tremulant;
  }

  for (int64_t id : sortedIds(m.switches)) {
    const Id sid = static_cast<Id>(id);
    // A key drawn as a switch is a key: notes travel over MIDI.
    if (m.keyboardKeys.count(sid)) continue;
    // Only the switch the player moves; one the wiring drives follows it.
    if (proc_.playerSwitchFor(sid) != sid) continue;
    const Switch& s = m.switches.at(sid);
    SwitchEntry e;
    e.id = id;
    e.name = s.name;
    e.latching = s.latching;
    if (auto it = roles.find(id); it != roles.end()) e.role = it->second;
    else if (s.isCoupler) e.role = Role::Coupler;
    else if (s.isTremulant) e.role = Role::Tremulant;
    else if (!s.latching) e.role = Role::Piston;
    if (auto it = stopOf.find(id); it != stopOf.end()) {
      const Stop& stop = m.stops.at(static_cast<Id>(it->second));
      e.division = divisionIndex(static_cast<int64_t>(stop.divisionId));
      e.available = !proc_.stopLocked(stop.stopId);
      if (e.name.empty()) e.name = stop.name;
    }
    switches_.push_back(std::move(e));
  }

  for (int64_t id : sortedIds(m.continuousControls)) {
    const Id cid = static_cast<Id>(id);
    if (proc_.playerControlFor(cid) != cid) continue;
    controls_.push_back({id, m.continuousControls.at(cid).name, -1});
  }
  for (int64_t id : sortedIds(m.keyboards)) keyboards_.push_back({id, m.keyboards.at(static_cast<Id>(id)).name, -1});

  const auto& player = proc_.playerCombinations();
  for (const auto& d : player.divisions())
    for (int n = 1; n <= player.divisionalCount(); ++n)
      divisionals_.push_back({static_cast<int64_t>(d.divisionId), d.name, n});

  temperaments_.push_back("");
  for (const auto& t : temperamentLibrary()) temperaments_.push_back(t.name);
}

int ControlModel::divisionIndex(int64_t divisionId) const {
  for (size_t i = 0; i < divisions_.size(); ++i)
    if (divisions_[i].id == divisionId) return static_cast<int>(i);
  return -1;
}

int ControlModel::count(Type type) {
  Error e;
  if (!ensure(e)) return 0;
  switch (type) {
    case Type::Program: return kProgramCount;
    case Type::Switch: return static_cast<int>(switches_.size());
    case Type::Control: return static_cast<int>(controls_.size());
    case Type::Division: return static_cast<int>(divisions_.size());
    case Type::Keyboard: return static_cast<int>(keyboards_.size());
    case Type::General: return proc_.playerCombinations().generalCount();
    case Type::Divisional: return static_cast<int>(divisionals_.size());
    case Type::Eqpreset: return static_cast<int>(proc_.voicingEqPresets().all().size());
    case Type::Temperament: return static_cast<int>(temperaments_.size());
    case Type::Command: return kCommandCount;
  }
  return 0;
}

int ControlModel::valueOf(Type type, int index) {
  const auto& player = proc_.playerCombinations();
  switch (type) {
    case Type::Program: return programValue(static_cast<ProgramProp>(index));
    case Type::Switch: return proc_.switchEngaged(static_cast<Id>(switches_[static_cast<size_t>(index)].id)) ? 1 : 0;
    case Type::Control: return proc_.continuousControlValue(static_cast<Id>(controls_[static_cast<size_t>(index)].id));
    case Type::General: return player.litGeneral() == index + 1 ? 1 : 0;
    case Type::Divisional: {
      const auto& d = divisionals_[static_cast<size_t>(index)];
      return player.litDivisional(static_cast<Id>(d.id)) == d.division ? 1 : 0;
    }
    case Type::Eqpreset: {
      const auto all = proc_.voicingEqPresets().all();
      return proc_.currentVoicingEqPreset() == all[static_cast<size_t>(index)].id ? 1 : 0;
    }
    case Type::Temperament:
      return proc_.temperamentChoice() == temperaments_[static_cast<size_t>(index)] ? 1 : 0;
    default:
      return 0;
  }
}

int ControlModel::programValue(ProgramProp prop) {
  switch (prop) {
    case ProgramProp::Session: return proc_.organLoads();
    case ProgramProp::OrganState:
      return proc_.organLoading() ? 1 : (proc_.loadedOrganFile() == juce::File() ? 0 : 2);
    case ProgramProp::LoadProgress: {
      const auto& lp = proc_.loadProgress();
      const int total = lp.total.load();
      return proc_.organLoading() ? (total > 0 ? lp.done.load() * 1000 / total : 0) : 1000;
    }
    case ProgramProp::MasterGain: {
      const auto* g = proc_.apvts().getRawParameterValue("masterGain");
      const float gain = g != nullptr ? g->load() : 1.0f;
      return juce::roundToInt(juce::Decibels::gainToDecibels(gain, -40.0f) * 10.0f);
    }
    case ProgramProp::Transpose: return proc_.transpose();
    case ProgramProp::Temperament: {
      for (size_t i = 0; i < temperaments_.size(); ++i)
        if (temperaments_[i] == proc_.temperamentChoice()) return static_cast<int>(i);
      return 0;
    }
    case ProgramProp::StepperFrame: return proc_.playerCombinations().frame();
    case ProgramProp::Setter: return proc_.captureMode() ? 1 : 0;
    case ProgramProp::EqOn: return proc_.voicingEq().on ? 1 : 0;
    case ProgramProp::EqPreset: {
      const int id = proc_.currentVoicingEqPreset();
      const auto all = proc_.voicingEqPresets().all();
      for (size_t i = 0; i < all.size(); ++i)
        if (all[i].id == id) return static_cast<int>(i);
      return -1;
    }
    case ProgramProp::Polyphony: return proc_.polyphonyLimit();
    case ProgramProp::SoundingVoices: return proc_.soundingVoices();
    case ProgramProp::LateBlocks: return static_cast<int>(std::min<int64_t>(proc_.lateBlocksTotal(), 2097151));
    case ProgramProp::RealtimeThreads: return proc_.realtimeThreads() ? 1 : 0;
    case ProgramProp::MidiRecorder: return proc_.recorder().isRecording() ? 1 : 0;
    case ProgramProp::AudioRecorder: return proc_.audioRecorder().isRecording() ? 1 : 0;
    case ProgramProp::Metronome: return proc_.metronome().enabled() ? 1 : 0;
    case ProgramProp::MetronomeTempo: return juce::roundToInt(proc_.metronome().tempo());
  }
  return 0;
}

juce::var ControlModel::object(Type type, int index) {
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  o->setProperty("index", index);
  const TypeInfo* info = typeInfo(type);
  switch (type) {
    case Type::Switch: {
      const auto& s = switches_[static_cast<size_t>(index)];
      o->setProperty("name", text(s.name));
      o->setProperty("odfId", static_cast<juce::int64>(s.id));
      o->setProperty("role", juce::String(std::string(kRoleNames[static_cast<int>(s.role)])));
      o->setProperty("latching", s.latching);
      o->setProperty("division", s.division);
      o->setProperty("available", s.available);
      break;
    }
    case Type::Control: {
      const auto& c = controls_[static_cast<size_t>(index)];
      o->setProperty("name", text(c.name));
      o->setProperty("odfId", static_cast<juce::int64>(c.id));
      break;
    }
    case Type::Division:
      o->setProperty("name", text(divisions_[static_cast<size_t>(index)].name));
      o->setProperty("odfId", static_cast<juce::int64>(divisions_[static_cast<size_t>(index)].id));
      break;
    case Type::Keyboard: {
      const auto& k = keyboards_[static_cast<size_t>(index)];
      o->setProperty("name", text(k.name));
      o->setProperty("odfId", static_cast<juce::int64>(k.id));
      o->setProperty("channel", proc_.channelForKeyboard(static_cast<Id>(k.id)));
      break;
    }
    case Type::General:
      o->setProperty("name", juce::String(index + 1));
      o->setProperty("set", proc_.playerCombinations().generalSet(index + 1));
      break;
    case Type::Divisional: {
      const auto& d = divisionals_[static_cast<size_t>(index)];
      o->setProperty("name", text(d.name) + " " + juce::String(d.division));
      o->setProperty("division", divisionIndex(d.id));
      o->setProperty("set", proc_.playerCombinations().divisionalSet(static_cast<Id>(d.id), d.division));
      break;
    }
    case Type::Eqpreset: {
      const auto all = proc_.voicingEqPresets().all();
      const auto& p = all[static_cast<size_t>(index)];
      o->setProperty("name", text(p.name));
      o->setProperty("own", !p.factory());
      break;
    }
    case Type::Temperament: {
      const auto& t = temperaments_[static_cast<size_t>(index)];
      o->setProperty("name", t.empty() ? juce::String("Organ's own") : text(t));
      break;
    }
    case Type::Command:
      o->setProperty("name", juce::String(std::string(kCommandNames[index])));
      break;
    case Type::Program:
      break;
  }
  if (info != nullptr && info->hasValue) o->setProperty("value", valueOf(type, index));
  return v;
}

juce::var ControlModel::list(Type type, Error& error) {
  if (!ensure(error)) return {};
  if (type == Type::Program) return program();
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  o->setProperty("session", session());
  o->setProperty("type", juce::String(std::string(typeInfo(type)->name)));
  juce::Array<juce::var> items;
  const int n = count(type);
  for (int i = 0; i < n; ++i) items.add(object(type, i));
  o->setProperty("count", n);
  o->setProperty("items", items);
  return v;
}

juce::var ControlModel::describe(Type type, int index, Error& error) {
  if (!ensure(error)) return {};
  if (index < 0 || index >= count(type)) {
    error = Error::OutOfRange;
    return {};
  }
  return object(type, index);
}

bool ControlModel::get(Type type, int index, int& value, Error& error) {
  if (!ensure(error)) return false;
  const TypeInfo* info = typeInfo(type);
  if (info == nullptr || (type != Type::Program && !info->hasValue)) {
    error = Error::UnknownType;
    return false;
  }
  if (index < 0 || index >= count(type)) {
    error = Error::OutOfRange;
    return false;
  }
  value = valueOf(type, index);
  return true;
}

juce::var ControlModel::dump(Type type, Error& error) {
  if (!ensure(error)) return {};
  const TypeInfo* info = typeInfo(type);
  if (info == nullptr || !info->hasValue) {
    error = Error::UnknownType;
    return {};
  }
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  juce::Array<juce::var> values;
  const int n = count(type);
  for (int i = 0; i < n; ++i) values.add(valueOf(type, i));
  o->setProperty("session", session());
  o->setProperty("type", juce::String(std::string(info->name)));
  o->setProperty("first", 0);
  o->setProperty("count", n);
  o->setProperty("values", values);
  return v;
}

Error ControlModel::set(Type type, int index, int& value) {
  Error e;
  if (!ensure(e)) return e;
  if (type == Type::Program) {
    e = setProgram(static_cast<ProgramProp>(index), value);
    if (e == Error::None) value = programValue(static_cast<ProgramProp>(index));
    return e;
  }
  const TypeInfo* info = typeInfo(type);
  if (info == nullptr) return Error::UnknownType;
  if (index < 0 || index >= count(type)) return Error::OutOfRange;
  if (info->access == Access::Press) {
    // A press-type object takes 1 as a press and 0 as nothing.
    if (value == 1) e = press(type, index);
    value = valueOf(type, index);
    return e;
  }
  if (info->access != Access::Set) return Error::ReadOnly;
  if (type == Type::Switch) {
    const auto id = static_cast<Id>(switches_[static_cast<size_t>(index)].id);
    if (value == 2) value = proc_.switchEngaged(id) ? 0 : 1;
    if (value < 0 || value > 1) return Error::BadValue;
    proc_.setSwitchEngaged(id, value == 1);
  } else if (type == Type::Control) {
    if (value < info->min || value > info->max) return Error::BadValue;
    proc_.setControlValue(static_cast<Id>(controls_[static_cast<size_t>(index)].id), value);
  }
  value = valueOf(type, index);
  return Error::None;
}

Error ControlModel::press(Type type, int index) {
  Error e;
  if (!ensure(e)) return e;
  if (index < 0 || index >= count(type)) return Error::OutOfRange;
  switch (type) {
    case Type::General:
      proc_.pressGeneral(index + 1);
      return Error::None;
    case Type::Divisional: {
      const auto& d = divisionals_[static_cast<size_t>(index)];
      proc_.pressDivisional(static_cast<Id>(d.id), d.division);
      return Error::None;
    }
    case Type::Eqpreset:
      proc_.recallVoicingEqPreset(proc_.voicingEqPresets().all()[static_cast<size_t>(index)].id);
      return Error::None;
    case Type::Temperament:
      proc_.setTemperament(temperaments_[static_cast<size_t>(index)]);
      return Error::None;
    case Type::Switch: {
      // A piston's switch, or a drawstop toggled.
      const auto id = static_cast<Id>(switches_[static_cast<size_t>(index)].id);
      proc_.setSwitchEngaged(id, switches_[static_cast<size_t>(index)].latching ? !proc_.switchEngaged(id) : true);
      return Error::None;
    }
    case Type::Command:
      switch (static_cast<Command>(index)) {
        case Command::Panic: proc_.releaseAllKeys(); break;
        case Command::GeneralCancel: proc_.pressGeneralCancel(); break;
        case Command::StepperNext: proc_.stepperNext(); break;
        case Command::StepperPrevious: proc_.stepperPrev(); break;
        case Command::StepperFirst: proc_.stepperFirst(); break;
        case Command::StepperLast: proc_.stepperLast(); break;
        case Command::EqPresetNext: proc_.stepVoicingEqPreset(1); break;
        case Command::EqPresetPrevious: proc_.stepVoicingEqPreset(-1); break;
      }
      return Error::None;
    default:
      return Error::ReadOnly;
  }
}

Error ControlModel::setProgram(ProgramProp prop, int value) {
  const int i = static_cast<int>(prop);
  if (i < 0 || i >= kProgramCount) return Error::OutOfRange;
  const ProgramPropInfo& info = kProgram[i];
  if (!info.writable) return Error::ReadOnly;
  if (value < info.min || value > info.max) return Error::BadValue;
  switch (prop) {
    case ProgramProp::MasterGain: {
      const float db = static_cast<float>(value) / 10.0f;
      const float g = db <= -40.0f ? 0.0f : juce::Decibels::decibelsToGain(db);
      if (auto* p = proc_.apvts().getParameter("masterGain")) p->setValueNotifyingHost(p->convertTo0to1(g));
      proc_.markMasterGainDirty();
      break;
    }
    case ProgramProp::Transpose: proc_.setTranspose(value); break;
    case ProgramProp::Temperament:
      if (value >= static_cast<int>(temperaments_.size())) return Error::BadValue;
      proc_.setTemperament(temperaments_[static_cast<size_t>(value)]);
      break;
    case ProgramProp::StepperFrame: proc_.stepperGoto(value); break;
    case ProgramProp::Setter: proc_.setCaptureMode(value == 1); break;
    case ProgramProp::EqOn: {
      auto eq = proc_.voicingEq();
      eq.on = value == 1;
      proc_.setVoicingEq(eq);
      break;
    }
    case ProgramProp::EqPreset: {
      const auto all = proc_.voicingEqPresets().all();
      if (value < 0 || value >= static_cast<int>(all.size())) return Error::BadValue;
      proc_.recallVoicingEqPreset(all[static_cast<size_t>(value)].id);
      break;
    }
    case ProgramProp::Polyphony: proc_.setPolyphonyLimit(value); break;
    case ProgramProp::RealtimeThreads: proc_.setRealtimeThreads(value == 1); break;
    case ProgramProp::Metronome: proc_.metronome().setEnabled(value == 1); break;
    case ProgramProp::MetronomeTempo: proc_.metronome().setTempo(value); break;
    default: return Error::ReadOnly;
  }
  return Error::None;
}

juce::var ControlModel::program() {
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  for (const auto& p : kProgram) {
    // While loading, only what does not read the model.
    if (proc_.organLoading() && (p.prop == ProgramProp::Temperament || p.prop == ProgramProp::EqPreset)) continue;
    o->setProperty(juce::Identifier(juce::String(std::string(p.name))), programValue(p.prop));
  }
  return v;
}

juce::var ControlModel::identity() {
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  o->setProperty("protocol", juce::String(std::string(kProtocolName)));
  o->setProperty("version", juce::String(kMajor) + "." + juce::String(kMinor));
  o->setProperty("program", "Masterpiece");
  o->setProperty("programVersion", juce::String(MP_VERSION));
  o->setProperty("session", session());
  o->setProperty("organ", proc_.loadedOrganFile() == juce::File() ? juce::var()
                                                                  : juce::var(proc_.organModel().organName.empty()
                                                                                  ? proc_.loadedOrganFile().getFileNameWithoutExtension()
                                                                                  : text(proc_.organModel().organName)));
  juce::Array<juce::var> types;
  for (const auto& t : kTypes) types.add(juce::String(std::string(t.plural)));
  o->setProperty("types", types);
  return v;
}

void ControlModel::poll(std::vector<Change>& out, bool readings) {
  // The program's own properties, loading or not.
  if (seenProgram_.size() != static_cast<size_t>(kProgramCount)) seenProgram_.assign(kProgramCount, INT32_MIN);
  for (int i = 0; i < kProgramCount; ++i) {
    const auto prop = static_cast<ProgramProp>(i);
    if (proc_.organLoading() && (prop == ProgramProp::Temperament || prop == ProgramProp::EqPreset)) continue;
    const bool reading = prop == ProgramProp::SoundingVoices || prop == ProgramProp::LoadProgress ||
                         prop == ProgramProp::LateBlocks;
    if (reading && !readings) continue;
    const int v = programValue(prop);
    if (v != seenProgram_[static_cast<size_t>(i)]) {
      seenProgram_[static_cast<size_t>(i)] = v;
      out.push_back({Type::Program, i, v});
    }
  }
  Error e;
  if (!ensure(e)) return;
  if (polledFor_ != builtFor_) {
    // A new organ: the lists are new; clients dump again.
    polledFor_ = builtFor_;
    seen_.assign(std::size(kTypes), {});
    for (const auto& t : kTypes) {
      if (!t.hasValue || t.type == Type::Program) continue;
      auto& s = seen_[static_cast<size_t>(t.type)];
      const int n = count(t.type);
      s.resize(static_cast<size_t>(n));
      for (int i = 0; i < n; ++i) s[static_cast<size_t>(i)] = valueOf(t.type, i);
    }
    return;
  }
  for (const auto& t : kTypes) {
    if (!t.hasValue || t.type == Type::Program) continue;
    auto& s = seen_[static_cast<size_t>(t.type)];
    const int n = count(t.type);
    if (static_cast<int>(s.size()) != n) s.assign(static_cast<size_t>(n), INT32_MIN);  // presets added or removed
    for (int i = 0; i < n; ++i) {
      const int v = valueOf(t.type, i);
      if (v != s[static_cast<size_t>(i)]) {
        s[static_cast<size_t>(i)] = v;
        out.push_back({t.type, i, v});
      }
    }
  }
}

}  // namespace mp::api
