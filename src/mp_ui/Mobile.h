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

// The whole screen the app has, the bars included: where a full-screen
// window goes, with its content placed in screenArea().
juce::Rectangle<int> screenBounds();

#if JUCE_IOS
// The window scene's bounds now and its safe part, from UIKit (IosAccess.mm).
// False before the app has a scene.
bool sceneBounds(juce::Rectangle<int>& whole, juce::Rectangle<int>& safe);
// The scenes' windows, one per line, for the log (IosAccess.mm).
juce::String describeWindows();
#endif

// On a phone or tablet, makes a window fill the screen, with a title bar
// tall enough to touch and its close button in reach. Does nothing elsewhere.
void fitToScreen(juce::DocumentWindow& window);

// Shows or hides a window that floats over the console (combinations,
// recorder), above every other window while `onTop`. On Linux the window is
// made afresh each time it opens, with `onTop` fixed until it closes: X11
// takes "above" only when a window is created, so JUCE changes it on an open
// window by destroying and remaking it, and a window manager forgets it while
// the window is hidden (#140: on top the first time, not after reopening, and
// a window remade on every change of focus caught clicks where it had been).
void showFloating(juce::DocumentWindow& window, bool show, bool onTop);

// On a phone or tablet every panel and dialog is shown inside the main window,
// in this layer over the console: one window for the system to place, turn
// and give touches to. Separate windows each needed their own size, safe area
// and place in front, and a hidden one still took every touch on iOS (#197).
// Menus and alerts keep windows of their own: they are made and destroyed,
// never left hidden, and a menu inside the main window lost its items from
// the accessibility tree. Set by the main window on a phone or tablet.
void setPanelHost(juce::Component* host);
juce::Component* panelHost();

// Whether a shown window's "above" may follow the program in and out of the
// front. Not on Linux; see showFloating.
#if JUCE_LINUX
constexpr bool kLiveOnTop = false;
#else
constexpr bool kLiveOnTop = true;
#endif

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

// Whether a file name is something a phone or tablet opens: a .rar or .orgue
// package. Always true on a desktop, which opens loose definitions too.
bool isMobilePackage(const juce::String& fileName);

// Where organs brought onto a phone or tablet are kept: the app's own
// storage, which the loader can read by path.
juce::File importedOrgansFolder();

// Android hands a chosen file over as a document, not a path, and the loader
// reads paths (the archive reader seeks through a package). A document in
// the device's own storage is read where it is, through a descriptor the
// system opens for it; `done` receives a path that reads it, and nothing is
// copied. Otherwise (a cloud drive streams, and cannot seek) the document is
// copied into importedOrgansFolder() first, with a progress window and a
// Cancel button, and `done` receives the copy; or nothing, if it was
// cancelled or failed. A copy already there with the same name and size is
// reused rather than copied again.
void importDocument(const juce::URL& document, std::function<void(juce::File)> done);

// Makes again, at startup, the paths of documents read in place in earlier
// sessions, so the last organ and the favourites reopen. Before any organ is
// loaded; does nothing on the desktop.
void restoreLinkedDocuments();

#if JUCE_IOS
// iPhone and iPad lend a picked document or folder through a security-scoped
// URL (#197). Starts that access from the bookmark the file chooser keeps in
// `picked`, holds it until the program ends, and remembers it for the next
// start; returns the path to read it by, or nothing when the loan failed.
// Nothing is copied: a package is read where it is. See IosAccess.mm.
juce::File holdPickedAccess(const juce::URL& picked);
// Lends again, at startup, what was picked in earlier sessions.
void restoreHeldAccess();
#endif

}  // namespace mp::ui
