// The joint between a pipe's held sound and its release, measured with the
// bumpometer (Bumpometer.h) over many moments of letting go.
//
// A release that meets the sustain out of phase dips or swells by the timing
// of the key: "sometimes a bump at the note-off" (#565). Every release is put
// in phase with the sound it follows, as GrandOrgue does, whether or not the
// definition's row asks; and a release whose definition gives no crossfade
// length takes one by the pipe's pitch, long in the bass and short in the
// treble, as GrandOrgue does.
#include "MpTest.h"
#include "Bumpometer.h"

#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_sampler/VoiceEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kRate = 48000.0;
constexpr double kHz = 200.0;  // 240 frames a period
constexpr double kPi = 3.141592653589793;

// A pipe's waveform: three harmonics.
double voice(double t, double phase) {
  const double w = 2.0 * kPi * kHz * t + phase;
  return 0.6 * std::sin(w) + 0.25 * std::sin(2.0 * w + 0.7) + 0.12 * std::sin(3.0 * w + 1.9);
}

// The attack: the pipe speaking, its level rising over the first 60 ms, then
// held, with a seamless loop over its last half second.
mp::SampleBuffer makeAttack() {
  mp::SampleBuffer b;
  b.numChannels = 1;
  b.sampleRate = kRate;
  b.numFrames = 72000;
  b.frames.resize(static_cast<size_t>(b.numFrames));
  for (int i = 0; i < b.numFrames; ++i) {
    const double t = i / kRate;
    b.frames[static_cast<size_t>(i)] = static_cast<float>((1.0 - std::exp(-t / 0.06)) * voice(t, 0.0));
  }
  b.loopStart = 48000;  // 200 periods in
  b.loopEnd = 72000;    // 100 periods later
  return b;
}

// The release: the same pipe dying away, recorded a quarter period out of
// phase with the attack's start.
mp::SampleBuffer makeRelease() {
  mp::SampleBuffer b;
  b.numChannels = 1;
  b.sampleRate = kRate;
  b.numFrames = 72000;
  b.frames.resize(static_cast<size_t>(b.numFrames));
  for (int i = 0; i < b.numFrames; ++i) {
    const double t = i / kRate;
    b.frames[static_cast<size_t>(i)] = static_cast<float>(std::exp(-t / 0.25) * voice(t, kPi / 2.0));
  }
  return b;
}

// The moments the key lets go, in frames: a hundred across the attack and
// the loop, and a hundred within one period, well into the loop.
std::vector<int64_t> moments() {
  std::vector<int64_t> out;
  for (int k = 0; k < 100; ++k) out.push_back(960 + static_cast<int64_t>(k) * 660);
  for (int k = 0; k < 100; ++k) out.push_back(57600 + (static_cast<int64_t>(k) * 240) / 100);
  return out;
}

// The attack's first 300 ms, while the pipe's level still rises: a release
// begun there starts louder than the sound it follows, which no phase
// mends. GrandOrgue scales such a release down by how soon the key let go.
constexpr int64_t kAttackFrames = 14400;

struct Sweep {
  mp::bump::Summary held;    // let go once the pipe had settled
  mp::bump::Summary attack;  // let go in its first 300 ms
  int aligned = 0;
  std::vector<std::pair<int64_t, double>> worst;  // frame, dB, after the attack
};

// Every moment, the key let go and the joint measured. `rowAsks` is the
// definition's own PhaseAlignAutomatically; `engineAligns` the program's
// switch.
Sweep sweep(bool engineAligns, bool rowAsks, double crossfadeMs) {
  const mp::SampleBuffer attack = makeAttack();
  const mp::SampleBuffer tail = makeRelease();
  mp::Pipe pipe;
  pipe.pipeId = 100;
  pipe.midiNote = 55;
  mp::PipeLayer layer;
  layer.layerId = 1;
  mp::AttackSample a;
  a.id = 11;
  a.sample.sampleId = 1;
  a.sample.resolvedPitchHz = kHz;
  layer.attacks.push_back(a);
  mp::ReleaseSample r;
  r.id = 21;
  r.sample.sampleId = 2;
  r.sample.resolvedPitchHz = kHz;
  r.releaseCrossfadeMs = crossfadeMs;
  r.phaseAlign = rowAsks;
  layer.releases.push_back(r);
  pipe.layers.push_back(std::move(layer));
  auto provider = [&](mp::Id id) -> const mp::SampleBuffer* {
    return id == 1 ? &attack : id == 2 ? &tail : nullptr;
  };

  const int64_t xfade = static_cast<int64_t>(crossfadeMs * kRate / 1000.0);
  const int64_t after = xfade + 4800;
  std::vector<mp::bump::Joint> joints, early;
  std::vector<int64_t> heldAt;
  Sweep s;
  for (const int64_t at : moments()) {
    mp::VoiceEngine eng;
    eng.prepare(kRate, 4, 1);
    eng.setSampleProvider(provider);
    eng.setReleasePhaseAlign(engineAligns);
    eng.setReleaseLogging(true);
    mp::VoiceStart start;
    start.pipe = &pipe;
    start.layer = &pipe.layers[0];
    start.velocity = 90;
    start.ratio = 1.0;
    start.gain = 1.0f;
    if (eng.startVoice(start, 7) < 0) continue;

    std::vector<float> y(static_cast<size_t>(at + after), 0.0f);
    auto render = [&](int64_t from, int64_t to) {
      while (from < to) {
        const int n = static_cast<int>(std::min<int64_t>(4096, to - from));
        float* out[1] = {y.data() + from};
        eng.render(out, 1, n);
        from += n;
      }
    };
    render(0, at);
    mp::NoteRelease rel;
    rel.velocity = 64;
    eng.noteOff(7, rel);
    std::vector<mp::VoiceEngine::ReleaseEvent> events;
    eng.takeReleaseEvents(events);
    if (!events.empty() && events.back().aligned) ++s.aligned;
    render(at, at + after);
    const mp::bump::Joint j = mp::bump::measure(y, kRate, at, xfade, kRate / kHz);
    if (at < kAttackFrames) {
      early.push_back(j);
    } else {
      joints.push_back(j);
      heldAt.push_back(at);
    }
  }
  // The worst moments, for a reader chasing them.
  std::vector<size_t> order(joints.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return joints[x].bumpDb > joints[y].bumpDb; });
  for (size_t i = 0; i < order.size() && i < 4; ++i)
    s.worst.push_back({heldAt[order[i]], joints[order[i]].bumpDb});
  s.held = mp::bump::summarise(std::move(joints));
  s.attack = mp::bump::summarise(std::move(early));
  return s;
}

std::string figures(const mp::bump::Summary& s) {
  char text[200];
  std::snprintf(text, sizeof text, "worst %.2f dB, median %.2f dB, spread %.2f dB, click %.1f dB over %d releases",
                s.worstBumpDb, s.medianBumpDb, s.spreadDb, s.worstClickDb, s.count);
  return text;
}

class ReleaseBlendTest final : public mp::test::Test {
public:
  ReleaseBlendTest() : Test("functional.voice.release-blend", Category::Functional) {}
  void run() override {
    for (const double ms : {10.0, 50.0}) {
      // A definition that does not ask for alignment, as many leave it.
      const Sweep asRecorded = sweep(false, false, ms);
      const Sweep played = sweep(true, false, ms);
      const std::string label = std::to_string(static_cast<int>(ms)) + " ms crossfade: ";
      std::printf("release-blend: %sas recorded %s\n", label.c_str(), figures(asRecorded.held).c_str());
      std::printf("release-blend: %sas played   %s, %d aligned\n", label.c_str(), figures(played.held).c_str(),
                  played.aligned);
      std::printf("release-blend: %sin the attack, as recorded %s\n", label.c_str(),
                  figures(asRecorded.attack).c_str());
      std::printf("release-blend: %sin the attack, as played   %s\n", label.c_str(), figures(played.attack).c_str());
      for (const auto& [at, db] : played.worst)
        std::printf("release-blend:   let go at %.1f ms: %.2f dB\n", at * 1000.0 / kRate, db);
      MP_CHECK(asRecorded.held.count + asRecorded.attack.count == 200 &&
                   played.held.count + played.attack.count == 200,
               label + "every release sounds");
      MP_CHECK(asRecorded.held.spreadDb > 3.0,
               label + "sanity: out of phase, the joint depends on the moment the key lets go: " +
                   figures(asRecorded.held));
      MP_CHECK(played.aligned == 200, label + "every release is put in phase, asked or not: " +
                                          std::to_string(played.aligned) + " of 200");
      MP_CHECK(played.held.worstBumpDb < 3.0,
               label + "and the joint is smooth at every moment: " + figures(played.held));
    }
  }
};
static ReleaseBlendTest g_releaseBlend;

// The crossfade a release takes when its definition gives none: by the
// pipe's MIDI note, 184 ms up to note 42, 6 ms from note 87, in a straight
// line between, as GrandOrgue takes it.
class ReleaseCrossfadeByPitchTest final : public mp::test::Test {
public:
  ReleaseCrossfadeByPitchTest() : Test("functional.odf.release-crossfade-by-pitch", Category::Functional) {}
  void run() override {
    std::ifstream in(std::string(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string fixture = ss.str();
    const std::string given = "<ReleaseCrossfadeLengthMs>10</ReleaseCrossfadeLengthMs>";
    const std::string note = "<NormalMIDINoteNumber>36</NormalMIDINoteNumber>";
    MP_CHECK(fixture.find(given) != std::string::npos && fixture.find(note) != std::string::npos,
             "sanity: the fixture states its release's crossfade and its pipe's note");

    auto crossfade = [&](bool withLength, int midiNote) {
      std::string xml = fixture;
      if (!withLength) xml.erase(xml.find(given), given.size());
      xml.replace(xml.find(note), note.size(),
                  "<NormalMIDINoteNumber>" + std::to_string(midiNote) + "</NormalMIDINoteNumber>");
      mp::OdfLoader loader;
      mp::OdfLoader::Options opts;
      mp::OrganModel m;
      mp::OdfDiagnostics d;
      if (!loader.loadFromXmlString(xml, "minimal.Organ_Hauptwerk_xml", opts, m, d)) return -1.0;
      for (const auto& [id, rank] : m.ranks)
        for (const auto& pipe : rank.pipes)
          for (const auto& layer : pipe.layers)
            for (const auto& r : layer.releases) return r.releaseCrossfadeMs;
      return -1.0;
    };
    MP_CHECK(crossfade(true, 36) == 10.0, "a length the definition gives is kept: " +
                                              std::to_string(crossfade(true, 36)));
    MP_CHECK(crossfade(false, 36) == 184.0, "none given, a bass pipe: " + std::to_string(crossfade(false, 36)));
    MP_CHECK(crossfade(false, 64) == 95.0, "none given, note 64: " + std::to_string(crossfade(false, 64)));
    MP_CHECK(crossfade(false, 90) == 6.0, "none given, a treble pipe: " + std::to_string(crossfade(false, 90)));
  }
};
static ReleaseCrossfadeByPitchTest g_releaseCrossfadeByPitch;

}  // namespace
