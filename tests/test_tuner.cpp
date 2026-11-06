// SPDX-License-Identifier: GPL-3.0-only
// The tuner (View > Tuner): the nearest note in a temperament, and the pitch
// and harmonics read from a tone, to a fraction of a cent.
#include "MpTest.h"
#include "../src/mp_core/Temperament.h"

#include <cmath>
#include <string>
#include <vector>

#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/SpectrumTap.h"
#endif

namespace {

using mp::test::Category;

constexpr double kPi = 3.14159265358979323846;

class NearestTemperedNoteTest final : public mp::test::Test {
public:
  NearestTemperedNoteTest() : Test("functional.tuner.nearest-note", Category::Functional) {}
  void run() override {
    const mp::Temperament equal{"Equal", std::vector<double>(12, 0.0)};
    auto a = mp::nearestTemperedNote(440.0, 440.0, equal);
    MP_CHECK(a.midiNote == 69 && std::fabs(a.cents) < 1e-9, "440 Hz is A4 in tune");
    a = mp::nearestTemperedNote(445.0, 440.0, equal);
    MP_CHECK(a.midiNote == 69 && std::fabs(a.cents - 19.56) < 0.01,
             "445 Hz is A4 19.6 cents sharp: " + std::to_string(a.cents));
    a = mp::nearestTemperedNote(415.0, 415.0, equal);
    MP_CHECK(a.midiNote == 69 && std::fabs(a.cents) < 1e-9, "at A 415 the note is still A4");
    // A temperament that lowers E by 10 cents: an E 1 cent above that reads +1.
    mp::Temperament lowE{"Low E", std::vector<double>(12, 0.0)};
    lowE.centsOffset12[4] = -10.0;
    const double e = mp::pipeTargetHz(64, 8, 440.0, 0.0, lowE, 0) * std::pow(2.0, 1.0 / 1200.0);
    a = mp::nearestTemperedNote(e, 440.0, lowE);
    MP_CHECK(a.midiNote == 64 && std::fabs(a.cents - 1.0) < 1e-6,
             "E is measured against the temperament's E: " + std::to_string(a.cents));
    // The same pitch is 9 cents flat of equal temperament.
    a = mp::nearestTemperedNote(e, 440.0, equal);
    MP_CHECK(a.midiNote == 64 && std::fabs(a.cents + 9.0) < 1e-6, "and 9 cents flat of equal");
    MP_CHECK(mp::nearestTemperedNote(0.0, 440.0, equal).cents == 0.0, "no pitch reads nothing");
  }
};

NearestTemperedNoteTest g_nearestTemperedNote;

#ifdef MP_TEST_HAS_AUDIO

constexpr double kRate = 48000.0;

// A pipe-like tone: the given amplitude per harmonic, each at its own phase.
std::vector<float> pipe(double hz, const std::vector<double>& amplitudes, int frames) {
  std::vector<float> out(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    double s = 0.0;
    for (size_t h = 0; h < amplitudes.size(); ++h)
      s += amplitudes[h] * std::sin(2.0 * kPi * hz * static_cast<double>(h + 1) * i / kRate + 0.7 * h);
    out[static_cast<size_t>(i)] = static_cast<float>(s);
  }
  return out;
}

double centsBetween(double a, double b) { return 1200.0 * std::log2(a / b); }

class TunerPitchTest final : public mp::test::Test {
public:
  TunerPitchTest() : Test("functional.tuner.pitch", Category::Functional) {}
  void run() override {
    constexpr int n = mp::SpectrumTap::kPitchSpan;
    // Middle C 3.7 cents sharp, harmonics at -6, -12 and -20 dB, then -40 dB.
    const double c4 = 261.6255653 * std::pow(2.0, 3.7 / 1200.0);
    const auto tone = pipe(c4, {0.3, 0.15, 0.075, 0.03, 0.003, 0.003, 0.003, 0.003}, n);
    const auto p = mp::SpectrumTap::measurePitch(tone.data(), n, kRate);
    MP_CHECK(p.found, "a held middle C is found");
    MP_CHECK(std::fabs(centsBetween(p.hz, c4)) < 0.2,
             "its pitch is read within 0.2 cents: " + std::to_string(centsBetween(p.hz, c4)));
    MP_CHECK(std::fabs(p.levelDb - (-10.46f)) < 0.5f, "the fundamental at 0.3 reads -10.5 dBFS: " +
                                                           std::to_string(p.levelDb));
    const float expected[] = {0.0f, -6.02f, -12.04f, -19.98f, -40.0f};
    for (int h = 0; h < 5; ++h)
      MP_CHECK(std::fabs(p.harmonicsDb[static_cast<size_t>(h)] - expected[h]) < 1.0f,
               "harmonic " + std::to_string(h + 1) + " reads " + std::to_string(p.harmonicsDb[static_cast<size_t>(h)]));

    // A low C, its octave as loud as itself: the lowest peak is the note.
    const double c2 = 65.40639 * std::pow(2.0, -12.0 / 1200.0);
    const auto low = pipe(c2, {0.2, 0.2, 0.1, 0.05}, n);
    const auto q = mp::SpectrumTap::measurePitch(low.data(), n, kRate);
    MP_CHECK(q.found && std::fabs(centsBetween(q.hz, c2)) < 0.5,
             "C2 12 cents flat is read as such: " + std::to_string(centsBetween(q.hz, c2)));
    const auto low2 = mp::nearestTemperedNote(q.hz, 440.0, mp::Temperament{"Equal", std::vector<double>(12, 0.0)});
    MP_CHECK(low2.midiNote == 36 && std::fabs(low2.cents + 12.0) < 0.5, "and named C2, 12 cents flat");

    // A blower's rumble at 70 Hz 16 dB under A4 is the lowest peak; with the
    // floor the window sets from the held key, a 32' stop's pitch under it,
    // the note is read.
    auto rumble = pipe(70.0, {0.05}, n);
    const auto a4 = pipe(440.0, {0.3, 0.1, 0.05}, n);
    for (size_t i = 0; i < rumble.size(); ++i) rumble[i] += a4[i];
    const auto unbounded = mp::SpectrumTap::measurePitch(rumble.data(), n, kRate);
    MP_CHECK(unbounded.found && std::fabs(unbounded.hz - 70.0) < 1.0,
             "with no floor the rumble is the lowest peak: " + std::to_string(unbounded.hz));
    const auto bounded = mp::SpectrumTap::measurePitch(rumble.data(), n, kRate, 0.9 * 440.0 / 4.0);
    MP_CHECK(bounded.found && std::fabs(centsBetween(bounded.hz, 440.0)) < 0.2,
             "above the floor the note is read: " + std::to_string(bounded.hz));

    // Silence, and a blower 100 dB down, are no note.
    const std::vector<float> silence(static_cast<size_t>(n), 0.0f);
    MP_CHECK(!mp::SpectrumTap::measurePitch(silence.data(), n, kRate).found, "silence is no note");
    const auto hum = pipe(50.0, {1e-5}, n);
    MP_CHECK(!mp::SpectrumTap::measurePitch(hum.data(), n, kRate).found, "a sound 100 dB down is no note");
    MP_CHECK(!mp::SpectrumTap::measurePitch(tone.data(), mp::SpectrumTap::kFft - 1, kRate).found,
             "fewer samples than one analysis read nothing");
  }
};

// The tuner reads the newest samples the tap took, in order.
class TunerLatestTest final : public mp::test::Test {
public:
  TunerLatestTest() : Test("functional.tuner.latest", Category::Functional) {}
  void run() override {
    mp::SpectrumTap tap;
    tap.open();
    std::vector<float> ramp(10000);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i);
    for (size_t i = 0; i < ramp.size(); i += 500) {
      const float* ch[1] = {ramp.data() + i};
      tap.push(ch, 1, 500);
    }
    std::vector<float> got(mp::SpectrumTap::kPitchSpan);
    tap.latest(got.data(), mp::SpectrumTap::kPitchSpan);
    bool ordered = true;
    for (int i = 0; i < mp::SpectrumTap::kPitchSpan; ++i)
      ordered = ordered && got[static_cast<size_t>(i)] == static_cast<float>(10000 - mp::SpectrumTap::kPitchSpan + i);
    MP_CHECK(ordered, "the newest samples, oldest first, across the ring's wrap");
    tap.close();
  }
};

TunerPitchTest g_tunerPitch;
TunerLatestTest g_tunerLatest;

#endif  // MP_TEST_HAS_AUDIO

}  // namespace
