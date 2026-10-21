// A control the accessibility layer can draw or retire.
//
// The console paints its own stops, couplers and pistons, and the plain stop
// list is made of buttons: neither is a plain control to a screen reader.
// Each of them carries this, and a handler that reads it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace mp::ui {

// What the accessibility layer needs from a control the player draws.
class AccessibleToggle {
public:
  virtual ~AccessibleToggle() = default;

  // The name a screen reader reads.
  virtual juce::String accessibleName() const = 0;
  // Whether the control is drawn now.
  virtual bool accessibleDrawn() const = 0;
  // Draws the control or retires it.
  virtual void accessibleToggle() = 0;
};

// The handler of such a control: a toggle button whose state says whether
// the control is drawn, and whose toggle action draws it or retires it.
class ToggleAccessibilityHandler final : public juce::AccessibilityHandler {
public:
  ToggleAccessibilityHandler(juce::Component& toWrap, AccessibleToggle& control,
                             juce::AccessibilityRole role
                                 = juce::AccessibilityRole::toggleButton)
      : juce::AccessibilityHandler(
            toWrap, role,
            juce::AccessibilityActions()
                .addAction(juce::AccessibilityActionType::press,
                           [this] { control_.accessibleToggle(); })
                .addAction(juce::AccessibilityActionType::toggle,
                           [this] { control_.accessibleToggle(); })),
        control_(control) {}

  juce::AccessibleState getCurrentState() const override {
    auto state = juce::AccessibilityHandler::getCurrentState();
    // A piston is pressed and let go: there is no state to report.
    if (getRole() != juce::AccessibilityRole::button) {
      state = state.withCheckable();
      if (control_.accessibleDrawn()) state = state.withChecked();
    }
    return state;
  }

  juce::String getTitle() const override { return control_.accessibleName(); }
  juce::String getDescription() const override {
    return getComponent().getDescription();
  }
  // The help a screen reader offers: the tooltip, as a button's own help is.
  juce::String getHelp() const override {
    const auto& help = getComponent().getHelpText();
    if (help.isNotEmpty()) return help;
    // The component behind the handler is not itself const.
    auto& component = const_cast<juce::Component&>(getComponent());
    if (auto* tip = dynamic_cast<juce::TooltipClient*>(&component))
      return tip->getTooltip();
    return {};
  }

private:
  AccessibleToggle& control_;
};

} // namespace mp::ui
