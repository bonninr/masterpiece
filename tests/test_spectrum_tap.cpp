// SPDX-License-Identifier: GPL-3.0-only
// The Equalizer tab's spectrum: the analysis reads a sine at its level and
// frequency, and the tap takes nothing while no window has it open.
#include "MpTest.h"

#include "../src/mp_audio/SpectrumTap.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

std::vector<float> tone(double hz, double amplitude, int frames) {
  std::vector<float> out(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
    out[static_cast<size_t>(i)] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / kRate));
  return out;
}

size_t loudestBin(const std::vector<float>& db) {
  return static_cast<size_t>(std::max_element(db.begin() + 1, db.end()) - db.begin());
}

class SpectrumAnalyseTest final : public mp::test::Test {
public:
  SpectrumAnalyseTest() : Test("functional.spectrum.analyse", Category::Functional) {}
  void run() override {
    constexpr int n = mp::SpectrumTap::kFft;
    const double binHz = kRate / n;
    // A sine on bin 85 (996 Hz) at half scale reads -6 dBFS there.
    const auto s = tone(85 * binHz, 0.5, n);
    const auto db = mp::SpectrumTap::analyse(s.data());
    MP_CHECK(db.size() == static_cast<size_t>(n / 2 + 1), "one value per bin up to half the rate");
    MP_CHECK(loudestBin(db) == 85, "the loudest bin is the tone's: " + std::to_string(loudestBin(db)));
    MP_CHECK(std::fabs(db[85] - (-6.02f)) < 0.3f, "a half-scale sine reads -6 dBFS: " + std::to_string(db[85]));
    // Far from the tone the window leaves nothing to speak of.
    MP_CHECK(db[430] < -90.0f, "5 kHz away from a 1 kHz tone is below -90 dBFS: " + std::to_string(db[430]));
    // Off a bin centre the level drops by at most the Hann window's 1.4 dB.
    const auto off = tone(85.5 * binHz, 0.5, n);
    const auto dbOff = mp::SpectrumTap::analyse(off.data());
    const float peak = *std::max_element(dbOff.begin() + 1, dbOff.end());
    MP_CHECK(peak > -6.02f - 1.5f && peak < -5.9f, "between two bins it reads within 1.5 dB: " + std::to_string(peak));
  }
};

class SpectrumTapTest final : public mp::test::Test {
public:
  SpectrumTapTest() : Test("functional.spectrum.tap", Category::Functional) {}
  void run() override {
    mp::SpectrumTap tap;
    auto left = tone(440.0, 0.25, 8192);
    auto right = left;
    const float* ch[2] = {left.data(), right.data()};
    tap.push(ch, 2, 8192);
    MP_CHECK(tap.samplesPushed() == 0, "closed, the tap takes nothing");
    tap.open();
    for (int i = 0; i < 8192; i += 256) {
      const float* block[2] = {left.data() + i, right.data() + i};
      tap.push(block, 2, 256);
    }
    MP_CHECK(tap.samplesPushed() == 8192, "open, it takes every frame");
    const auto db = tap.spectrumDb();
    const double hz = loudestBin(db) * kRate / mp::SpectrumTap::kFft;
    MP_CHECK(std::fabs(hz - 440.0) < 12.0, "it reads the tone at 440 Hz: " + std::to_string(hz));
    MP_CHECK(db[loudestBin(db)] > -14.0f && db[loudestBin(db)] < -11.0f,
             "both channels at -12 dBFS fold to -12 dBFS: " + std::to_string(db[loudestBin(db)]));
    tap.open();
    tap.close();
    MP_CHECK(tap.isOpen(), "a second window closing leaves it open for the first");
    tap.close();
    MP_CHECK(!tap.isOpen(), "and it closes with the last");
  }
};

SpectrumAnalyseTest g_spectrumAnalyse;
SpectrumTapTest g_spectrumTap;

}  // namespace
