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

// A name as a player reads it: sets mark a helper with "__" and often a short
// tag after it ("__Img_14. GO  Principale 8"); both go, and doubled spaces.
std::string readable(const std::string& raw) {
  std::string n = raw;
  if (n.rfind("__", 0) == 0) {
    n.erase(0, n.find_first_not_of('_'));
    const auto cut = n.find('_');
    if (cut != std::string::npos && cut > 0 && cut < 8 && n.substr(0, cut).find(' ') == std::string::npos)
      n.erase(0, cut + 1);
  }
  std::string out;
  for (char c : n)
    if (!(c == ' ' && !out.empty() && out.back() == ' ')) out += c;
  return out;
}

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
  ranks_.clear();
  switchIndex_.clear();

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
  // A coupler is no object of its own in the organ file: it is the switch a
  // key action waits on. It belongs where the keys are played that it
  // couples from, as on a console: a tirasse with the pedal.
  std::unordered_map<int64_t, int64_t> couplerInto;  // player switch -> division id
  auto divisionOfKeyboard = [&m](Id keyboard) -> int64_t {
    for (const auto& [did, d] : m.divisions)
      for (Id k : d.keyboardIds)
        if (k == keyboard) return static_cast<int64_t>(did);
    return 0;
  };
  auto noteCoupler = [&](const KeyAction& a) {
    if (a.conditionSwitchId == 0) return;
    const auto top = static_cast<int64_t>(proc_.playerSwitchFor(a.conditionSwitchId));
    if (roles.count(top)) return;
    roles[top] = Role::Coupler;
    couplerInto.emplace(top, a.sourceKeyboard != 0 ? divisionOfKeyboard(static_cast<Id>(a.sourceKeyboard))
                                                   : static_cast<int64_t>(a.sourceDivision));
  };
  for (const auto& a : m.keyActions) noteCoupler(a);
  for (const auto& [did, d] : m.divisions) {
    (void)did;
    for (const auto& a : d.keyActions) noteCoupler(a);
  }

  // What the console shows. A large set has thousands of switches the player
  // never touches -- pallets, noises, the wiring between drawn knobs -- so an
  // organ with console pictures lists its stops, couplers and tremulants and
  // whatever else is drawn and can be clicked. One with no pictures lists
  // every switch the player could move.
  bool anyDrawn = false;
  for (const auto& [sid, s] : m.switches)
    if (s.dispInstanceId != 0) anyDrawn = true;

  for (int64_t id : sortedIds(m.switches)) {
    const Id sid = static_cast<Id>(id);
    // A key drawn as a switch is a key: notes travel over MIDI.
    if (m.keyboardKeys.count(sid)) continue;
    // Only the switch the player moves; one the wiring drives follows it.
    if (proc_.playerSwitchFor(sid) != sid) continue;
    const Switch& s = m.switches.at(sid);
    const bool musical = roles.count(id) > 0 || s.isCoupler || s.isTremulant;
    SwitchEntry e;
    // Every switch is reachable; `console` marks what the console shows.
    // "__" is how sets mark a helper the player never sees as such: the click
    // target behind a drawn knob ("__Img_14. GO  Principale 8"), a jamb page's
    // copy. The switch it works is shown in its place.
    e.console = musical || !anyDrawn || (s.dispInstanceId != 0 && s.clickable && s.name.rfind("__", 0) != 0);
    e.id = id;
    e.name = readable(s.name);
    e.latching = s.latching;
    if (auto it = roles.find(id); it != roles.end()) e.role = it->second;
    else if (s.isCoupler) e.role = Role::Coupler;
    else if (s.isTremulant) e.role = Role::Tremulant;
    else if (!s.latching) e.role = Role::Piston;
    if (auto it = couplerInto.find(id); it != couplerInto.end() && e.role == Role::Coupler)
      e.division = divisionIndex(it->second);
    if (auto it = stopOf.find(id); it != stopOf.end()) {
      const Stop& stop = m.stops.at(static_cast<Id>(it->second));
      e.division = divisionIndex(static_cast<int64_t>(stop.divisionId));
      e.available = !proc_.stopLocked(stop.stopId);
      if (e.name.empty()) e.name = stop.name;
    }
    switchIndex_[id] = static_cast<int>(switches_.size());
    switches_.push_back(std::move(e));
  }

  // The same for the shoes and wheels: those drawn and clickable, or every one
  // the player could move on an organ with no pictures.
  bool anyControlDrawn = false;
  for (const auto& [cid, c] : m.continuousControls)
    if (c.imageSetInstanceId != 0) anyControlDrawn = true;
  for (int64_t id : sortedIds(m.continuousControls)) {
    const Id cid = static_cast<Id>(id);
    if (proc_.playerControlFor(cid) != cid) continue;
    const ContinuousControl& c = m.continuousControls.at(cid);
    controls_.push_back({id, readable(c.name), -1,
                         !anyControlDrawn || (c.imageSetInstanceId != 0 && c.clickable)});
  }
  for (int64_t id : sortedIds(m.keyboards)) keyboards_.push_back({id, m.keyboards.at(static_cast<Id>(id)).name, -1});
  for (int64_t id : sortedIds(m.ranks)) ranks_.push_back({id, readable(m.ranks.at(static_cast<Id>(id)).name), -1});

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
    case Type::Rank: return static_cast<int>(ranks_.size());
    case Type::Note: return static_cast<int>(keyboards_.size()) * 128;
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
    case Type::Rank:
      return juce::roundToInt(proc_.voicing().live().rank(static_cast<Id>(ranks_[static_cast<size_t>(index)].id)).gainDb * 10.0f);
    case Type::Note:
      return proc_.keyDownOnKeyboard(static_cast<Id>(keyboards_[static_cast<size_t>(index / 128)].id), index % 128) ? 1 : 0;
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
      o->setProperty("console", s.console);
      break;
    }
    case Type::Control: {
      const auto& c = controls_[static_cast<size_t>(index)];
      o->setProperty("name", text(c.name));
      o->setProperty("odfId", static_cast<juce::int64>(c.id));
      o->setProperty("console", c.console);
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
    case Type::General: {
      o->setProperty("name", juce::String(index + 1));
      o->setProperty("set", proc_.playerCombinations().generalSet(index + 1));
      std::vector<PlayerCombinations::Change> changes;
      proc_.playerCombinations().recallGeneral(index + 1, changes);
      o->setProperty("holds", holds(changes));
      break;
    }
    case Type::Divisional: {
      const auto& d = divisionals_[static_cast<size_t>(index)];
      o->setProperty("name", text(d.name) + " " + juce::String(d.division));
      o->setProperty("division", divisionIndex(d.id));
      o->setProperty("set", proc_.playerCombinations().divisionalSet(static_cast<Id>(d.id), d.division));
      std::vector<PlayerCombinations::Change> changes;
      proc_.playerCombinations().recallDivisional(static_cast<Id>(d.id), d.division, changes);
      o->setProperty("holds", holds(changes));
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
    case Type::Rank: {
      const auto& r = ranks_[static_cast<size_t>(index)];
      o->setProperty("name", text(r.name));
      o->setProperty("odfId", static_cast<juce::int64>(r.id));
      o->setProperty("tuning", juce::roundToInt(proc_.voicing().live().rank(static_cast<Id>(r.id)).tuningCents * 10.0f));
      break;
    }
    case Type::Note: {
      static const char* kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      const int note = index % 128;
      o->setProperty("name", text(keyboards_[static_cast<size_t>(index / 128)].name) + " " + kNames[note % 12] +
                                 juce::String(note / 12 - 1));
      break;
    }
    case Type::Program:
      break;
  }
  if (info != nullptr && info->hasValue) o->setProperty("value", valueOf(type, index));
  return v;
}

// What a recall would draw, without recalling it: a stop by the switch the
// player moves for it, a coupler or tremulant by its own switch.
juce::var ControlModel::holds(const std::vector<PlayerCombinations::Change>& changes) const {
  juce::Array<juce::var> out;
  const OrganModel& m = proc_.organModel();
  for (const auto& c : changes) {
    if (!c.engage) continue;
    Id sid = c.id;
    if (c.kind == PlayerCombinations::ElementKind::Stop) {
      const auto it = m.stops.find(c.id);
      if (it == m.stops.end() || it->second.controllingSwitchId == 0) continue;
      sid = proc_.playerSwitchFor(it->second.controllingSwitchId);
    }
    if (const auto at = switchIndex_.find(static_cast<int64_t>(sid)); at != switchIndex_.end()) out.add(at->second);
  }
  return out;
}

bool ControlModel::consoleObject(Type type, int index) const {
  if (type == Type::Switch) return switches_[static_cast<size_t>(index)].console;
  if (type == Type::Control) return controls_[static_cast<size_t>(index)].console;
  return true;
}

juce::var ControlModel::list(Type type, Error& error, bool all) {
  if (!ensure(error)) return {};
  if (type == Type::Program) return program();
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  o->setProperty("session", session());
  o->setProperty("type", juce::String(std::string(typeInfo(type)->name)));
  juce::Array<juce::var> items;
  const int n = count(type);
  for (int i = 0; i < n; ++i)
    if (all || consoleObject(type, i)) items.add(object(type, i));
  // `count` is every object, so indices run 0 to count - 1 whatever is listed.
  o->setProperty("count", n);
  o->setProperty("listed", items.size());
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
  } else if (type == Type::Rank) {
    if (value < info->min || value > info->max) return Error::BadValue;
    // As the Voicing tab does: heard from the next note.
    const auto id = static_cast<Id>(ranks_[static_cast<size_t>(index)].id);
    PipeVoicing pv = proc_.voicing().live().rank(id);
    pv.gainDb = static_cast<float>(value) / 10.0f;
    proc_.voicing().live().setRank(id, pv);
  } else if (type == Type::Note) {
    if (value < 0 || value > 1) return Error::BadValue;
    // Through the on-screen keyboard's path, on the keyboard's channel.
    const int channel = proc_.channelForKeyboard(static_cast<Id>(keyboards_[static_cast<size_t>(index / 128)].id));
    if (channel < 1 || channel > 16) return Error::NotAllowed;
    if (value == 1)
      proc_.keyboardState().noteOn(channel, index % 128, 0.8f);
    else
      proc_.keyboardState().noteOff(channel, index % 128, 0.0f);
    return Error::None;  // the key is down from the next audio block
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
        case Command::Quit: return quitOrPowerOff(false);
        case Command::PowerOff: return quitOrPowerOff(true);
      }
      return Error::None;
    default:
      return Error::ReadOnly;
  }
}

// Off unless the player allowed it, and never in the first two minutes: a
// noisy input learned for this would otherwise shut a console down at every
// power-on, before anyone could reach it.
Error ControlModel::quitOrPowerOff(bool powerOff) {
  if (!proc_.remoteSettings().allowPowerOff || !proc_.onPowerRequest) return Error::NotAllowed;
  if (juce::Time::getMillisecondCounter() - startedMs_ < 120000) return Error::Busy;
  // After this request has been answered.
  auto request = proc_.onPowerRequest;
  juce::Timer::callAfterDelay(300, [request, powerOff] { request(powerOff); });
  return Error::None;
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
