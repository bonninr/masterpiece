// Toolbar icons (#276): a picture and a tooltip where the picture says it
// at a glance -- Panic's stop sign. The drawings are from Lucide; see
// Icons.cpp for their licence.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace mp::ui {

enum class Icon { Panic, Save };

// The icon in one colour, 24 units square.
std::unique_ptr<juce::Drawable> makeIcon(Icon icon, juce::Colour colour);

// A button that shows an icon. The text it is given stays its name, for the
// tooltip's first word and for a screen reader, but is not drawn.
class IconButton : public juce::TextButton {
public:
  IconButton(Icon icon, const juce::String& name, const juce::String& tooltip,
             juce::Colour colour = juce::Colour(0xffd6dbe4));
  void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
  std::unique_ptr<juce::Drawable> normal_, dimmed_;
};

}  // namespace mp::ui
