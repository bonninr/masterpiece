// Ready settings for the compressor and the limiter together, chosen from
// one menu on the Audio tab. A preset sets both and every value of each;
// moving any control afterwards leaves the menu on no preset.
#pragma once

#include <array>

namespace mp {

struct OutputDynamicsPreset {
  const char* name;  // in English, translated where it is shown
  bool compressorOn;
  float thresholdDb, ratio, attackMs, releaseMs, makeupDb;
  int highPassHz;
  bool limiterOn;
  float ceilingDb, limiterReleaseMs;
};

// The detector's high-pass keeps the pedal out of the level the compressor
// listens to, so the bass sounds in full in every preset that compresses.
inline constexpr std::array<OutputDynamicsPreset, 6> kOutputDynamicsPresets{{
    {"Both off", false, -18.0f, 3.0f, 10.0f, 200.0f, 0.0f, 120, false, -1.0f, 100.0f},
    {"Limiter only", false, -18.0f, 3.0f, 10.0f, 200.0f, 0.0f, 120, true, -1.0f, 100.0f},
    {"Gentle", true, -18.0f, 2.0f, 20.0f, 400.0f, 4.0f, 120, true, -1.0f, 100.0f},
    {"Small speakers", true, -24.0f, 3.0f, 10.0f, 250.0f, 8.0f, 120, true, -1.0f, 100.0f},
    {"Laptop speakers", true, -30.0f, 4.0f, 5.0f, 200.0f, 12.0f, 200, true, -1.0f, 80.0f},
    {"Headphones, quiet practice", true, -30.0f, 2.5f, 10.0f, 300.0f, 10.0f, 80, true, -3.0f, 150.0f},
}};

} // namespace mp
