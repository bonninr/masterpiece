#include "ProcessorTrace.h"
#include "MasterpieceProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace mp {

namespace {
// MasterpieceProcessor::kCoupledSwitchBase.
constexpr Id kEngineSwitchBase = 0x40000000ull;
std::string ms(double t) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", t);
  return buf;
}

template <typename Map>
std::string nameIn(const Map& m, Id id) {
  const auto it = m.find(id);
  return it == m.end() ? std::string() : it->second.name;
}
}  // namespace

ProcessorTraceResult runProcessorTrace(const juce::File& definition, OrganNetwork::Kind kind,
                                       const TraceScript& script, const ProcessorTraceOptions& options) {
  ProcessorTraceResult result;
  MasterpieceProcessor proc;
  proc.setNetworkOverride(kind);
  proc.setSampleStandIns(options.standIns);
  proc.setRateAndBufferSizeDetails(options.sampleRate, options.blockSize);
  proc.prepareToPlay(options.sampleRate, options.blockSize);
  const auto load = proc.loadOrgan(definition, options.maxFramesPerSample, false);
  if (!load.ok) {
    result.error = load.error;
    return result;
  }
  result.loaded = true;
  const OrganModel& model = proc.organModel();
  proc.clearChannelAssignments();

  std::vector<Id> switches, controls;
  for (const auto& kv : model.switches) switches.push_back(kv.first);
  for (const auto& kv : model.continuousControls) controls.push_back(kv.first);
  std::sort(switches.begin(), switches.end());
  std::sort(controls.begin(), controls.end());
  std::vector<uint8_t> was(switches.size(), 0);
  std::vector<int> valueWas(controls.size(), 0);
  for (size_t i = 0; i < controls.size(); ++i) {
    const auto& c = model.continuousControls.at(controls[i]);
    valueWas[i] = std::clamp(c.defaultValue, std::min(c.minValue, c.maxValue), std::max(c.minValue, c.maxValue));
  }
  std::set<Id> speaking;
  std::map<Id, Id> rankOfPipe;
  for (const auto& [rankId, rank] : model.ranks)
    for (const Pipe& p : rank.pipes) rankOfPipe.emplace(p.pipeId, rankId);

  auto flush = [&](const std::string& t) {
    std::vector<std::string> lines;
    for (size_t i = 0; i < switches.size(); ++i) {
      const uint8_t on = proc.switchEngaged(switches[i]) ? 1 : 0;
      if (on == was[i]) continue;
      was[i] = on;
      // The engine's own switches (the legacy network's coupled inputs, from
      // kCoupledSwitchBase up) are its implementation, not the organ's: a
      // kind of their own, which the A/B diff leaves out.
      const bool engine = switches[i] >= kEngineSwitchBase;
      lines.push_back(std::string(engine ? "engine-switch " : "switch ") + std::to_string(switches[i]) +
                      (on != 0 ? " on" : " off") + "  | " + nameIn(model.switches, switches[i]));
    }
    for (size_t i = 0; i < controls.size(); ++i) {
      const int v = proc.continuousControlValue(controls[i]);
      if (v == valueWas[i]) continue;
      valueWas[i] = v;
      lines.push_back("control " + std::to_string(controls[i]) + " " + std::to_string(v) + "  | " +
                      nameIn(model.continuousControls, controls[i]));
    }
    std::set<Id> now;
    const VoiceEngine& voices = proc.voiceEngine();
    for (int i = 0; i < voices.poolSize(); ++i) {
      const Voice& v = voices.voice(i);
      if (v.phase == VoicePhase::Attack || v.phase == VoicePhase::Loop) now.insert(v.pipeId);
    }
    auto pipeLine = [&](Id pipe, bool on) {
      const auto r = rankOfPipe.find(pipe);
      const Id rank = r == rankOfPipe.end() ? 0 : r->second;
      lines.push_back("pipe " + std::to_string(rank) + ":" + std::to_string(pipe) + (on ? " on" : " off") + "  | " +
                      nameIn(model.ranks, rank));
    };
    for (Id p : now)
      if (speaking.count(p) == 0) pipeLine(p, true);
    for (Id p : speaking)
      if (now.count(p) == 0) pipeLine(p, false);
    speaking.swap(now);
    std::sort(lines.begin(), lines.end());
    for (const auto& l : lines) result.lines.push_back(t + " " + l);
  };

  juce::AudioBuffer<float> buffer(2, options.blockSize);
  juce::MidiBuffer midi;
  const double blockMs = 1000.0 * options.blockSize / options.sampleRate;
  double t = 0.0;
  auto block = [&] {
    buffer.clear();
    proc.processBlock(buffer, midi);
    midi.clear();
    t += blockMs;
  };
  for (int i = 0; i < options.settleBlocks; ++i) block();
  t = 0.0;
  flush("init");

  std::map<Id, int> channelOf;
  for (const TraceStep& s : script.steps) {
    switch (s.kind) {
      case TraceStep::Kind::Tick: continue;  // the block size is the tick here
      case TraceStep::Kind::Wait: {
        const auto n = static_cast<long>(std::ceil(s.ms / blockMs - 1e-9));
        for (long i = 0; i < n; ++i) {
          block();
          flush(ms(t));
        }
        continue;
      }
      case TraceStep::Kind::Key: {
        auto ch = channelOf.find(s.id);
        if (ch == channelOf.end()) {
          const int next = static_cast<int>(channelOf.size()) + 1;
          if (next > 16) continue;
          proc.setKeyboardForChannel(next, s.id);
          ch = channelOf.emplace(s.id, next).first;
        }
        midi.addEvent(s.value != 0 ? juce::MidiMessage::noteOn(ch->second, s.note, static_cast<juce::uint8>(s.value))
                                   : juce::MidiMessage::noteOff(ch->second, s.note),
                      0);
        break;
      }
      case TraceStep::Kind::Switch: proc.setSwitchEngaged(s.id, s.value != 0); break;
      case TraceStep::Kind::Stop: proc.setStopEngaged(s.id, s.value != 0); break;
      case TraceStep::Kind::Piston:
        proc.setSwitchEngaged(s.id, true);
        proc.setSwitchEngaged(s.id, false);
        break;
      case TraceStep::Kind::Control: proc.setControlValue(s.id, s.value); break;
    }
    block();
    flush(ms(t));
  }
  return result;
}

} // namespace mp
