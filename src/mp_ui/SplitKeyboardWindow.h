// The split keyboard's window (#358): the keyboard drawn with its zones and
// split points, the zones, the bass from the hands, the presets, which manual
// each role plays, and the split's own MIDI controls. Opened from the View
// menu; it can stay open while playing, and the keys light in their zone's
// colour as they are played.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#include "../mp_audio/MasterpieceProcessor.h"

namespace mp::ui {

class SplitKeyboardWindow : public juce::DocumentWindow {
public:
  explicit SplitKeyboardWindow(MasterpieceProcessor& p);
  void closeButtonPressed() override;
};

}  // namespace mp::ui
