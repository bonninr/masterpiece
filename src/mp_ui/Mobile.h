#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace mp::ui {

// Phones and tablets. One window at a time, filling the screen, worked by a
// finger rather than a mouse: a dialog sized for a desktop opened beside the
// console instead, at its desktop size, with a title bar too thin to touch.
#if JUCE_ANDROID || JUCE_IOS
inline constexpr bool kMobile = true;
#else
inline constexpr bool kMobile = false;
#endif

// The part of the screen an app may draw in: the whole display less the
// system's own bars.
juce::Rectangle<int> screenArea();

// On a phone or tablet, makes a window fill the screen, with a title bar
// tall enough to touch and its close button in reach. Does nothing elsewhere.
void fitToScreen(juce::DocumentWindow& window);

// DialogWindow::LaunchOptions::launchAsync, with the dialog fitted to the
// screen on a phone or tablet. Every dialog opens through here.
juce::DialogWindow* launchDialog(juce::DialogWindow::LaunchOptions& options);

// Closes the front-most window other than `mainWindow`, as its own close
// button would: Android's back button. False when there was none, so the
// system's own back behaviour applies. A window whose properties set
// kStaysOpen is never closed this way.
bool closeFrontWindow(juce::Component* mainWindow);
inline const juce::Identifier kStaysOpen{"mpStaysOpen"};

// On a phone or tablet, the default look made for a finger: the same colours,
// with menu rows tall enough to touch. Installed once at startup and removed
// before shutdown; nothing happens elsewhere.
void installTouchLook();
void removeTouchLook();

}  // namespace mp::ui
