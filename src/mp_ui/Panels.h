// Panels: the organ as plain rectangles, for touchscreens (#237).
//
// A panel shows the organ's sections -- each division's stops, couplers and
// tremulant, the organ's own pistons, its expression controls -- as equal
// rectangles with the name in the middle. A bar along the top switches whole
// sections on and off; every section that is on is shown side by side, so all
// of them on is the whole organ on one screen. An edit mode hides single
// elements. An organ has up to two panels, each a tab beside the console pages
// or a window of its own on any screen, and Save keeps how each is set up.
//
// Every element stands for something the organ already has, so its MIDI
// mapping is that stop's, piston's or control's own.
#pragma once

#include "../mp_audio/MasterpieceProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace mp::ui {

// One rectangle.
struct PanelElement {
  enum class Kind { Stop, Switch, Piston, Control };
  Kind kind = Kind::Stop;
  // A stop's id; a coupler's or tremulant's switch; a piston's switch; a
  // continuous control.
  Id id = 0;
  std::string name;     // "Trompete"
  std::string footage;  // "8'", empty when the name has none
  // Saved with the panel to say which elements are hidden: kind and id,
  // "s12", "w340", "p501", "c7".
  std::string key() const;
};

// One column of the panel: a division, the pistons, or the controls.
struct PanelSection {
  enum class Kind { Division, Presets, Controls, Other };
  Kind kind = Kind::Division;
  Id divisionId = 0;
  std::string key;    // "d801", "presets", "controls", "other"
  std::string title;  // shown on the section bar and over the column
  std::vector<PanelElement> elements;
};

// The organ as panel sections, in its own order: each division with its
// stops, then the couplers that feed it, then its tremulant; the organ's own
// pistons; its swells and crescendo. A coupler or tremulant that cannot be
// placed in a division goes to a last section, "Other".
std::vector<PanelSection> panelSections(const MasterpieceProcessor& p);

// "Trompete harm. 8'" -> "Trompete harm.", "8'". Recognises footages written
// with a prime or an apostrophe, whole or fractional ("2 2/3'", "1 1/3"),
// and roman ranks for mixtures ("IV").
void splitFootage(const std::string& label, std::string& name, std::string& footage);

// How one panel is set up. New sections and elements show by default: the
// layout lists what was switched off, so an organ updated with more stops
// shows them without a visit to edit mode.
struct PanelLayout {
  std::set<std::string> sectionsOff;
  std::set<std::string> hidden;
  // Names the player gave elements, by element key: a switch the organ names
  // "SW_GO_TRMLT" can read "Tremulant" (#276). The organ's own name otherwise.
  std::map<std::string, std::string> names;
  // The panel's own name, on its tab and its window; "Panel 1" when empty.
  std::string title;
  bool detached = false;
  int x = 0, y = 0, w = 0, h = 0;  // the detached window, when it is one
  int scheme = 0;                  // PanelScheme, as a number

  std::string toText() const;
  bool fromText(const std::string& text);
  bool operator==(const PanelLayout& o) const;
};

// Up to two panels per organ, in Masterpiece/organs/<organ>.mppanels.
std::vector<PanelLayout> loadPanelLayouts(const MasterpieceProcessor& p);
bool savePanelLayouts(const MasterpieceProcessor& p, const std::vector<PanelLayout>& panels);
constexpr int kMaxPanels = 2;

// Colours. In every scheme the text contrasts with its rectangle, lit or not.
enum class PanelScheme { Dark = 0, Light = 1, HighContrast = 2, ByDivision = 3 };
juce::StringArray panelSchemeNames();

// The panel itself.
class PanelView : public juce::Component, private juce::Timer {
public:
  PanelView(MasterpieceProcessor& p, int index, PanelLayout layout);
  ~PanelView() override;

  // Build again from the organ, after a load.
  void rebuild();
  const PanelLayout& layout() const { return layout_; }
  bool hasUnsavedChanges() const { return !(layout_ == saved_); }
  // Keep the current setup as saved (after the editor wrote it).
  void markSaved() { saved_ = layout_; }
  // Its name: the player's, or "Panel <n>".
  juce::String title() const;
  // Its place among the organ's panels, after one before it was deleted.
  void setIndex(int index) { index_ = index; }
  // Set by the editor: the panel was renamed, so its tab and window follow.
  std::function<void()> onRenamed;
  // Ask for a new name for the panel.
  void askForTitle();
  // Set by the editor: write every panel's layout now.
  std::function<void()> onSave;
  // Set by the editor: detach into a window, or come back to the tab strip.
  std::function<void()> onDetachToggle;
  void setDetached(bool detached);
  // Where its window is, kept with the layout.
  void setWindowBounds(juce::Rectangle<int> b) {
    layout_.x = b.getX(); layout_.y = b.getY(); layout_.w = b.getWidth(); layout_.h = b.getHeight();
  }

  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  class Cell;
  class Fader;
  void timerCallback() override;
  void layoutSections();
  void setEditing(bool editing);
  void changed();
  // Ask for a new name for one element; empty gives it the organ's name back.
  void askForName(const PanelElement& e);
  // The element as shown: the player's name when it has one.
  PanelElement shown(const PanelElement& e) const;

  MasterpieceProcessor& proc_;
  int index_ = 0;
  PanelLayout layout_, saved_;
  std::vector<PanelSection> sections_;
  bool editing_ = false;

  juce::OwnedArray<juce::TextButton> sectionButtons_;
  juce::TextButton edit_{"Edit"}, save_{"Save"}, detach_{"Detach"}, rename_{"Rename"};
  juce::ComboBox scheme_;
  juce::Viewport viewport_;
  juce::Component body_;
  juce::OwnedArray<juce::Label> titles_;
  juce::OwnedArray<juce::Component> cells_;
};

// A panel in a window of its own.
class PanelWindow : public juce::DocumentWindow {
public:
  PanelWindow(const juce::String& title, PanelView& view);
  void closeButtonPressed() override;
  std::function<void()> onClose;
};

}  // namespace mp::ui
