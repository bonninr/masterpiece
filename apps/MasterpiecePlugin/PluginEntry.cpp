#include "../../src/mp_audio/MasterpieceProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new mp::MasterpieceProcessor();
}
