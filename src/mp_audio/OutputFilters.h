// An output's own filters (#1188): a high-pass, a low-pass, a delay and a
// polarity, the tools of a loudspeaker management processor, so that an
// installation needs no crossover of its own. Nothing about the bands is
// decided here: the designer gives each output the filters its driver needs.
// A high-pass and a low-pass on one output make a band-pass; outputs fed by
// the same signal through the sends, with Linkwitz-Riley edges that meet, add
// back up flat.
//
// Each filter is Butterworth or Linkwitz-Riley, of 12 or 24 dB an octave:
//   Butterworth      -3 dB at its frequency, the flattest band: for keeping
//                    the lowest notes from a speaker, or the highest.
//   Linkwitz-Riley   -6 dB at its frequency: a low-pass and a high-pass at the
//                    same frequency add up flat. At 24 dB an octave the two
//                    are in phase; at 12 dB an octave one of the two outputs
//                    is inverted, or they cancel at the crossover.
// Each is the bilinear transform of its analogue filter, warped to land on
// its frequency exactly, so a low-pass and a high-pass at the same frequency
// stay complementary up to the top of the band.
//
// The delay, up to 100 ms, sets drivers that stand at different depths in
// time with each other: 1 ms is 34 cm of sound in air.
//
// Off by default. While every part is off the processor does not make one,
// and the output is what the engine made. Built on the message thread, under
// the audio lock; the audio thread only runs it.
//
// JUCE-free: the tests drive it without audio.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace mp {

struct BusFilters {
  enum Type : int { Butterworth12 = 0, Butterworth24 = 1, LinkwitzRiley12 = 2, LinkwitzRiley24 = 3 };
  static constexpr float kMinHz = 20.0f;
  static constexpr float kMaxHz = 20000.0f;
  static constexpr float kMaxDelayMs = 100.0f;

  float highPassHz = 0.0f;  // 20..20000, or 0 for off
  int highPassType = LinkwitzRiley24;
  float lowPassHz = 0.0f;   // 20..20000, or 0 for off
  int lowPassType = LinkwitzRiley24;
  float delayMs = 0.0f;     // 0..100
  bool invert = false;

  bool active() const { return highPassHz > 0.0f || lowPassHz > 0.0f || delayMs > 0.0f || invert; }
  bool operator==(const BusFilters& o) const {
    return highPassHz == o.highPassHz && highPassType == o.highPassType && lowPassHz == o.lowPassHz &&
           lowPassType == o.lowPassType && delayMs == o.delayMs && invert == o.invert;
  }
  // Every value brought into range: a frequency under 20 Hz is off, one over
  // 20 kHz is 20 kHz; an unknown type is Linkwitz-Riley 24.
  BusFilters clamped() const {
    BusFilters c = *this;
    const auto hz = [](float v) { return v < kMinHz ? 0.0f : std::min(v, kMaxHz); };
    const auto type = [](int t) { return t >= Butterworth12 && t <= LinkwitzRiley24 ? t : LinkwitzRiley24; };
    c.highPassHz = hz(highPassHz);
    c.lowPassHz = hz(lowPassHz);
    c.highPassType = type(highPassType);
    c.lowPassType = type(lowPassType);
    c.delayMs = std::clamp(delayMs, 0.0f, kMaxDelayMs);
    return c;
  }
};

class OutputFilters {
public:
  static constexpr int kMaxChannels = 2;  // an output is a pair, or mono
  struct Section {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };

  // Message thread, before the audio runs: the filters' figures and the
  // delay's memory are made here.
  void prepare(const BusFilters& s, double sampleRate) {
    settings_ = s.clamped();
    rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    sections_ = design(settings_, rate_);
    state_.assign(static_cast<size_t>(kMaxChannels) * sections_.size(), {0.0, 0.0});
    delayFrames_ = static_cast<int>(std::lround(settings_.delayMs * 0.001 * rate_));
    for (auto& line : delay_) line.assign(delayFrames_ > 0 ? static_cast<size_t>(delayFrames_) : 0, 0.0f);
    write_ = 0;
  }
  const BusFilters& settings() const { return settings_; }
  int delayFrames() const { return delayFrames_; }

  // Audio thread: `channels` filtered in place, at most a pair.
  void process(float* const* channels, int numChannels, int numFrames) {
    const int chans = std::min(numChannels, kMaxChannels);
    const float sign = settings_.invert ? -1.0f : 1.0f;
    for (int ch = 0; ch < chans; ++ch) {
      float* x = channels[ch];
      if (x == nullptr) continue;
      for (size_t s = 0; s < sections_.size(); ++s) {
        const Section& q = sections_[s];
        auto& z = state_[static_cast<size_t>(ch) * sections_.size() + s];
        double z1 = z[0], z2 = z[1];
        for (int i = 0; i < numFrames; ++i) {
          const double in = x[i];
          const double out = q.b0 * in + z1;
          z1 = q.b1 * in - q.a1 * out + z2;
          z2 = q.b2 * in - q.a2 * out;
          x[i] = static_cast<float>(out);
        }
        z[0] = z1;
        z[1] = z2;
      }
      if (sign < 0.0f)
        for (int i = 0; i < numFrames; ++i) x[i] = -x[i];
      if (delayFrames_ > 0) {
        // A ring of exactly the delay: each frame read out before the one
        // going in takes its place.
        auto& line = delay_[static_cast<size_t>(ch)];
        size_t w = write_;
        for (int i = 0; i < numFrames; ++i) {
          const float in = x[i];
          x[i] = line[w];
          line[w] = in;
          if (++w == line.size()) w = 0;
        }
        if (ch + 1 == chans) write_ = w;
      }
    }
  }

  // The sections of both filters, high-pass first.
  static std::vector<Section> design(const BusFilters& s, double rate) {
    std::vector<Section> out;
    if (s.highPassHz > 0.0f) add(out, true, s.highPassHz, s.highPassType, rate);
    if (s.lowPassHz > 0.0f) add(out, false, s.lowPassHz, s.lowPassType, rate);
    return out;
  }

  // The filters' level at `hz`, in dB, from their own figures: what the
  // panel draws and what the tests measure against. The delay changes no
  // level, and the polarity none.
  static double responseDb(const BusFilters& s, double hz, double rate = 48000.0) {
    const auto sections = design(s.clamped(), rate);
    const std::complex<double> z1 = std::polar(1.0, -2.0 * 3.14159265358979323846 * hz / rate);
    double db = 0.0;
    for (const auto& q : sections) {
      const auto h = (q.b0 + q.b1 * z1 + q.b2 * z1 * z1) / (1.0 + q.a1 * z1 + q.a2 * z1 * z1);
      db += 20.0 * std::log10(std::max(std::abs(h), 1e-15));
    }
    return db;
  }

private:
  // One filter's sections: Butterworth 12 is one of Q 1/sqrt 2; Butterworth
  // 24 two, of Q 1 / (2 cos(pi/8)) and 1 / (2 cos(3pi/8)); Linkwitz-Riley 24
  // two Butterworth 12s; Linkwitz-Riley 12 two first-order sections.
  static void add(std::vector<Section>& out, bool high, double hz, int type, double rate) {
    hz = std::min(hz, rate * 0.49);
    switch (type) {
      case BusFilters::Butterworth12: out.push_back(second(high, hz, 1.0 / std::sqrt(2.0), rate)); break;
      case BusFilters::Butterworth24:
        out.push_back(second(high, hz, 0.54119610014619698, rate));
        out.push_back(second(high, hz, 1.3065629648763766, rate));
        break;
      case BusFilters::LinkwitzRiley12:
        out.push_back(first(high, hz, rate));
        out.push_back(first(high, hz, rate));
        break;
      default:
        out.push_back(second(high, hz, 1.0 / std::sqrt(2.0), rate));
        out.push_back(second(high, hz, 1.0 / std::sqrt(2.0), rate));
        break;
    }
  }
  // The analogue filters through the bilinear transform, prewarped to `hz`.
  static Section second(bool high, double hz, double q, double rate) {
    const double k = std::tan(3.14159265358979323846 * hz / rate);
    const double norm = 1.0 / (1.0 + k / q + k * k);
    Section s;
    if (high) {
      s.b0 = norm;
      s.b1 = -2.0 * norm;
      s.b2 = norm;
    } else {
      s.b0 = k * k * norm;
      s.b1 = 2.0 * k * k * norm;
      s.b2 = k * k * norm;
    }
    s.a1 = 2.0 * (k * k - 1.0) * norm;
    s.a2 = (1.0 - k / q + k * k) * norm;
    return s;
  }
  static Section first(bool high, double hz, double rate) {
    const double k = std::tan(3.14159265358979323846 * hz / rate);
    const double norm = 1.0 / (1.0 + k);
    Section s;
    s.b0 = high ? norm : k * norm;
    s.b1 = high ? -norm : k * norm;
    s.a1 = (k - 1.0) * norm;
    return s;
  }

  BusFilters settings_;
  double rate_ = 48000.0;
  std::vector<Section> sections_;
  std::vector<std::array<double, 2>> state_;
  std::array<std::vector<float>, kMaxChannels> delay_;
  size_t write_ = 0;
  int delayFrames_ = 0;
};

}  // namespace mp
