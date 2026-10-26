// Render on every logical processor (#422), on by default: on, the organ
// renders on every processor, with no ceiling; off, on every processor but
// one, eight at most. The setting takes effect at once and is kept for the
// next start.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"

using mp::test::Category;

namespace {

class EveryProcessorTest final : public mp::test::Test {
public:
  EveryProcessorTest() : Test("functional.audio.every-processor", Category::Functional) {}
  void run() override {
    using P = mp::MasterpieceProcessor;
    MP_CHECK(P::autoRenderThreads(8, false) == 7, "off, eight processors render on seven");
    MP_CHECK(P::autoRenderThreads(8, true) == 8, "on, eight processors render on eight");
    MP_CHECK(P::autoRenderThreads(24, false) == 8, "off, at most eight");
    MP_CHECK(P::autoRenderThreads(24, true) == 24, "on, no ceiling of eight");
    MP_CHECK(P::autoRenderThreads(2, false) == 1 && P::autoRenderThreads(2, true) == 2,
             "two processors: one off, two on");
    MP_CHECK(P::autoRenderThreads(1, true) == 1 && P::autoRenderThreads(0, true) == 1,
             "never below the audio thread");

    const int cores = P::usableProcessors();
    if (cores <= 0) return;

    auto render = [](P& proc) {
      juce::AudioBuffer<float> buf(2, 256);
      juce::MidiBuffer none;
      proc.processBlock(buf, none);
    };

    {
      P proc;
      MP_CHECK(proc.everyProcessor(), "on by default");
      proc.prepareToPlay(48000.0, 256);
      const int on = proc.renderThreadCount();
      MP_CHECK(on == P::autoRenderThreads(cores, true),
               "every processor by default: " + std::to_string(on) + " render threads on " +
                   std::to_string(cores));
      proc.setEveryProcessor(false);
      MP_CHECK(proc.renderThreadCount() == P::autoRenderThreads(cores, false),
               "turned off, the threads change at once: " + std::to_string(proc.renderThreadCount()));
      render(proc);
      proc.releaseResources();
    }
    {
      // The application starts the audio before it reads the saved settings.
      P again;
      again.prepareToPlay(48000.0, 256);
      MP_CHECK(again.renderThreadCount() == P::autoRenderThreads(cores, true),
               "before the settings are read, every processor");
      again.loadGlobalDefaults();
      MP_CHECK(!again.everyProcessor(), "turned off, it stays off at the next start");
      MP_CHECK(again.renderThreadCount() == P::autoRenderThreads(cores, false),
               "once the settings are read, every processor but one");
      again.setEveryProcessor(true);
      MP_CHECK(again.renderThreadCount() == P::autoRenderThreads(cores, true),
               "turned on again, every processor");
      render(again);
      again.releaseResources();
    }
  }
};
static EveryProcessorTest g_everyProcessor;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
