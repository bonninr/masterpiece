#include "BassManager.h"

#include <algorithm>

namespace mp {

void BassManager::prepare(double sampleRate, int maxBlock) {
  rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  filter_.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
  filter_.prepare({rate_, static_cast<juce::uint32>(std::max(1, maxBlock)), 2});
  if (hz_ > 0.0f) filter_.setCutoffFrequency(hz_);
  filter_.reset();
}

void BassManager::setCrossoverHz(float hz) {
  const float next = hz <= 0.0f ? 0.0f : std::clamp(hz, kMinHz, kMaxHz);
  if (next == hz_) return;
  hz_ = next;
  if (hz_ > 0.0f) filter_.setCutoffFrequency(hz_);
  filter_.reset();
}

void BassManager::process(float* left, float* right, float* sub, int numFrames) {
  if (hz_ <= 0.0f || left == nullptr || sub == nullptr) return;
  for (int i = 0; i < numFrames; ++i) {
    float lowL = 0.0f, highL = 0.0f;
    filter_.processSample(0, left[i], lowL, highL);
    left[i] = highL;
    float low = lowL;
    if (right != nullptr) {
      float lowR = 0.0f, highR = 0.0f;
      filter_.processSample(1, right[i], lowR, highR);
      right[i] = highR;
      low += lowR;
    }
    sub[i] += low;
  }
}

}  // namespace mp
