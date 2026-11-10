// Console profiles (#866): the window that sets one up, and the window that
// maps an organ from it.
//
// A profile is the player's console written down once: each tab's printed
// name, the keyboard it sits on and the MIDI it sends. When an organ opens,
// its unmapped stops, couplers, tremulants and pistons are matched to the
// tabs (ConsoleProfile.h); the strong matches are mapped at once and the
// weak ones are listed in "Map from console profile" for the player to tick.
// The player's own mappings for an organ are never touched.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "../mp_audio/MasterpieceProcessor.h"
#include "ConsoleProfiles.h"

namespace mp::ui {

// The matches for the loaded organ, each with a tick box.
class ProfileMatchPanel : public juce::Component, private juce::ListBoxModel {
public:
  explicit ProfileMatchPanel(MasterpieceProcessor& p);

  // The profile in use and the organ as it is now. Strong matches start
  // ticked, weak ones not. False when there is no profile in use.
  bool refresh();
  // Map the strong matches without asking, as an organ opens. Returns how
  // many controls were mapped; the list then holds what is left.
  int applyStrong();
  // Map the ticked matches. Returns how many controls were mapped.
  int applyTicked();

  int matchCount() const { return static_cast<int>(matches_.size()); }
  const ProfileMatch& match(int i) const { return matches_[static_cast<size_t>(i)]; }
  bool ticked(int i) const { return ticks_[static_cast<size_t>(i)]; }
  void setTicked(int i, bool on);
  juce::String tabText(int i) const;
  juce::String controlText(int i) const;
  void setNote(const juce::String& s) { note_.setText(s, juce::dontSendNotification); }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  int getNumRows() override { return matchCount(); }
  void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
  void listBoxItemClicked(int row, const juce::MouseEvent&) override;
  void showState();

  MasterpieceProcessor& proc_;
  juce::File file_;
  ConsoleProfile profile_;
  std::vector<OrganControl> controls_;
  std::vector<ProfileMatch> matches_;
  std::vector<bool> ticks_;

  juce::Label title_;
  juce::Label note_;
  juce::ListBox list_;
  juce::TextButton apply_{TRANS("Map ticked")};
  juce::TextButton all_{TRANS("Tick all")};
};

class ProfileMatchWindow : public juce::DocumentWindow {
public:
  explicit ProfileMatchWindow(MasterpieceProcessor& p);
  ~ProfileMatchWindow() override;
  void closeButtonPressed() override;
  ProfileMatchPanel& panel() { return *panel_; }

private:
  std::unique_ptr<ProfileMatchPanel> panel_;
};

// The profiles: choose the one in use, and set up its tabs.
class ConsoleProfilePanel : public juce::Component, private juce::Timer, private juce::ListBoxModel {
public:
  explicit ConsoleProfilePanel(MasterpieceProcessor& p);
  ~ConsoleProfilePanel() override;

  // The profiles on disk and the one in use, read again.
  void reload();
  // A new profile, made the one in use. Empty, or with this organ's mapped
  // controls when `fromOrgan`.
  bool newProfile(const juce::String& name, bool fromOrgan);
  bool renameProfile(const juce::String& name);
  void deleteProfile();
  // None when the file is empty.
  void useProfile(const juce::File& file);

  void addTab();
  void removeTab(int tab);
  void setLabel(int tab, const std::string& label);
  void setKind(int tab, ConsoleTabKind kind);
  void setKeyboard(int tab, int keyboard);
  void setFromKeyboard(int tab, int keyboard);
  // This organ's mapped controls added as tabs, with their messages.
  int addFromOrgan();
  // The next message from the console becomes the tab's.
  void learn(int tab);
  void stopLearning();
  void poll();

  const ConsoleProfile& profile() const { return profile_; }
  const juce::File& file() const { return file_; }
  int learningTab() const { return learning_; }
  juce::String midiText(int tab) const;

  // "Map this organ now".
  std::function<void()> onMapNow;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  class Row;
  void timerCallback() override { poll(); }
  int getNumRows() override { return static_cast<int>(profile_.tabs.size()); }
  void paintListBoxItem(int, juce::Graphics&, int, int, bool) override {}
  juce::Component* refreshComponentForRow(int row, bool selected, juce::Component* existing) override;
  void save();
  void showState();
  void askName(const juce::String& title, const juce::String& start, std::function<void(juce::String)> done);

  MasterpieceProcessor& proc_;
  juce::File file_;
  ConsoleProfile profile_;
  juce::Array<juce::File> files_;
  int learning_ = -1;

  juce::Label chooserLabel_;
  juce::ComboBox chooser_;
  juce::TextButton new_{TRANS("New...")};
  juce::TextButton rename_{TRANS("Rename...")};
  juce::TextButton delete_{TRANS("Delete")};
  juce::Label note_;
  juce::ListBox list_;
  juce::TextButton addTab_{TRANS("Add tab")};
  juce::TextButton addOrgan_{TRANS("Add this organ's mappings")};
  juce::TextButton mapNow_{TRANS("Map this organ...")};
};

class ConsoleProfileWindow : public juce::DocumentWindow {
public:
  explicit ConsoleProfileWindow(MasterpieceProcessor& p);
  ~ConsoleProfileWindow() override;
  void closeButtonPressed() override;
  ConsoleProfilePanel& panel() { return *panel_; }

private:
  std::unique_ptr<ConsoleProfilePanel> panel_;
};

}  // namespace mp::ui
