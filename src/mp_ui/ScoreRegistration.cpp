#include "ScoreRegistration.h"

#include "../mp_audio/MasterpieceProcessor.h"
#include "../mp_control/PlayerCombinations.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace mp::ui {

namespace {
// The shoes and sliders a player moves on the console: drawn, clickable, and
// fed by no other control.
std::vector<Id> playerControls(const MasterpieceProcessor& proc) {
  std::vector<Id> out;
  for (const auto& [id, c] : proc.organModel().continuousControls)
    if (c.imageSetInstanceId != 0 && c.clickable && proc.playerControlFor(id) == id) out.push_back(id);
  std::sort(out.begin(), out.end());
  return out;
}

bool engaged(const MasterpieceProcessor& proc, const PlayerCombinations::Element& e) {
  return e.kind == PlayerCombinations::ElementKind::Stop ? proc.stopEngaged(e.id) : proc.switchEngaged(e.id);
}
}  // namespace

std::vector<ScoreOrganState> parseScoreStates(const std::string& text) {
  std::vector<ScoreOrganState> out;
  juce::StringArray lines;
  lines.addLines(juce::String::fromUTF8(text.c_str()));
  for (const auto& line : lines) {
    const int tab = line.indexOfChar('\t');
    if (tab <= 0) continue;
    const auto json = juce::JSON::parse(line.substring(tab + 1));
    if (!json.isObject()) continue;
    ScoreOrganState s;
    s.organ = line.substring(0, tab).toStdString();
    s.organName = json["name"].toString();
    if (const auto* drawn = json["drawn"].getArray())
      for (const auto& d : *drawn) s.drawn.push_back(d.toString().toStdString());
    s.gainDb = static_cast<float>(static_cast<double>(json["gain"]));
    s.eq.fromLine(json["eq"].toString().toStdString());
    if (const auto* controls = json["controls"].getDynamicObject())
      for (const auto& p : controls->getProperties())
        s.controls[static_cast<Id>(p.name.toString().getLargeIntValue())] = static_cast<int>(p.value);
    out.push_back(std::move(s));
  }
  return out;
}

std::string writeScoreStates(const std::vector<ScoreOrganState>& states) {
  std::string out;
  for (const auto& s : states) {
    auto* o = new juce::DynamicObject();
    o->setProperty("name", s.organName);
    juce::Array<juce::var> drawn;
    for (const auto& d : s.drawn) drawn.add(juce::String(d));
    o->setProperty("drawn", drawn);
    o->setProperty("gain", static_cast<double>(s.gainDb));
    o->setProperty("eq", juce::String(s.eq.toLine()));
    auto* controls = new juce::DynamicObject();
    for (const auto& [id, v] : s.controls) controls->setProperty(juce::String(static_cast<juce::int64>(id)), v);
    o->setProperty("controls", juce::var(controls));
    const auto json = juce::JSON::toString(juce::var(o), juce::JSON::FormatOptions{}.withSpacing(juce::JSON::Spacing::none));
    if (!out.empty()) out += "\n";
    out += s.organ + "\t" + json.toStdString();
  }
  return out;
}

const ScoreOrganState* stateForOrgan(const std::vector<ScoreOrganState>& states, const std::string& organ) {
  if (organ.empty()) return nullptr;
  for (const auto& s : states)
    if (s.organ == organ) return &s;
  return nullptr;
}

ScoreOrganState captureScoreState(MasterpieceProcessor& proc) {
  ScoreOrganState s;
  s.organ = proc.organKey();
  s.organName = juce::String::fromUTF8(proc.organModel().organName.c_str());
  for (const auto& e : proc.playerCombinations().elements())
    if (engaged(proc, e)) s.drawn.push_back(PlayerCombinations::keyOf(e));
  // The parameter is a gain; the file holds decibels to a tenth, as the volume
  // slider shows them, with -40 for silence.
  if (const auto* g = proc.apvts().getRawParameterValue("masterGain"))
    s.gainDb = std::round(juce::Decibels::gainToDecibels(g->load(), -40.0f) * 10.0f) / 10.0f;
  s.eq = proc.voicingEq();
  for (const Id c : playerControls(proc)) s.controls[c] = proc.continuousControlValue(c);
  return s;
}

void applyScoreState(MasterpieceProcessor& proc, const ScoreOrganState& state) {
  const std::set<std::string> drawn(state.drawn.begin(), state.drawn.end());
  for (const auto& e : proc.playerCombinations().elements()) {
    const bool on = drawn.count(PlayerCombinations::keyOf(e)) != 0;
    if (on == engaged(proc, e)) continue;
    if (e.kind == PlayerCombinations::ElementKind::Stop) proc.setStopEngaged(e.id, on);
    else proc.setSwitchEngaged(e.id, on);
  }
  if (auto* p = proc.apvts().getParameter("masterGain")) {
    const float g = state.gainDb <= -40.0f ? 0.0f : juce::Decibels::decibelsToGain(state.gainDb);
    p->setValueNotifyingHost(p->convertTo0to1(g));
  }
  proc.setVoicingEq(state.eq);
  const auto controls = playerControls(proc);
  for (const auto& [id, v] : state.controls)
    if (std::find(controls.begin(), controls.end(), id) != controls.end() && proc.continuousControlValue(id) != v)
      proc.setContinuousControl(id, v);
}

juce::StringArray describeScoreState(const MasterpieceProcessor& proc, const ScoreOrganState& state) {
  juce::StringArray lines;
  const std::set<std::string> drawn(state.drawn.begin(), state.drawn.end());
  juce::StringArray stops;
  for (const auto& e : proc.playerCombinations().elements())
    if (drawn.count(PlayerCombinations::keyOf(e)) != 0) stops.add(juce::String::fromUTF8(e.name.c_str()));
  lines.add(stops.isEmpty() ? TRANS("No stops drawn") : stops.joinIntoString(", "));
  lines.add(TRANS("Volume") + ": " + juce::String(state.gainDb, 1) + " dB");
  lines.add(TRANS("Voicing EQ") + ": " + (state.eq.on ? TRANS("On") : TRANS("Off")));
  for (const auto& [id, v] : state.controls) {
    const auto c = proc.organModel().continuousControls.find(id);
    if (c == proc.organModel().continuousControls.end()) continue;
    lines.add(juce::String::fromUTF8(c->second.name.c_str()) + ": " + juce::String(v * 100 / 127) + "%");
  }
  return lines;
}

}  // namespace mp::ui
