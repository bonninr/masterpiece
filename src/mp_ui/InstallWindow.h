// Install and Uninstall organs (#1024), after the component installer of the
// format's own program: a package is read as its components -- organ
// definitions, sample packages, other files -- and each is listed with its
// version in the package, the version installed, and what to do with it.
// Installing writes them into the organs folder, the player's choice of drive.
// Uninstalling lists what that folder holds, which organs need each package,
// and removes what the player picks.
#pragma once

#include "../mp_archive/OrganInstaller.h"
#include "../mp_audio/MasterpieceProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace mp::ui {

class InstallPanel : public juce::Component, private juce::TableListBoxModel, private juce::Timer {
public:
  enum class Mode { Install, Uninstall };
  using Open = std::function<void(const juce::File&)>;

  InstallPanel(MasterpieceProcessor& p, Mode mode, juce::File package, Open open);
  ~InstallPanel() override;

  // Install: asks for the package first when none is given.
  static void showInstall(MasterpieceProcessor& p, Open open, juce::File package = {});
  static void showUninstall(MasterpieceProcessor& p, Open open);

  void paint(juce::Graphics& g) override;
  void resized() override;

  // For the test hooks and the window title.
  Mode mode() const { return mode_; }

private:
  struct Row {
    InstallComponent offered;               // Install: from the package; Uninstall: installed
    std::optional<InstallComponent> had;    // Install: the same component installed
    juce::String elsewhere;                 // Install: installed in another library, there
    InstallState state = InstallState::New;
    bool act = false;                       // install it, or uninstall it
    juce::StringArray neededBy;             // Uninstall: organs that need this package
    bool inUse = false;                     // Uninstall: the organ on the console, or its package
  };
  class ActionBox;

  // TableListBoxModel
  int getNumRows() override;
  void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
  void paintCell(juce::Graphics&, int row, int column, int w, int h, bool selected) override;
  juce::Component* refreshComponentForCell(int row, int column, bool selected, juce::Component* existing) override;
  juce::String getCellTooltip(int row, int column) override;

  void timerCallback() override;

  void analyse();
  void scanInstalled();
  void showRows();
  void setAction(int row, bool act);
  void updateSummary();
  void chooseFolder();
  void start();
  // The first install asks where organs go before anything is written: the
  // folder offered and the room on its drive, or another folder (#1245).
  void askWhere();
  void finishInstall(bool ok, const juce::String& error);
  void finishUninstall(bool ok, const juce::String& error);
  void setBusy(bool busy, const juce::String& what = {});
  juce::String actionText(const Row& r, bool act) const;
  juce::String stateText(const Row& r) const;
  juce::File root() const { return proc_.organsFolder(); }

  MasterpieceProcessor& proc_;
  const Mode mode_;
  juce::File package_;
  Open open_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<double>> fraction_ = std::make_shared<std::atomic<double>>(0.0);
  std::shared_ptr<OrganArchive> archive_;
  juce::File smallFiles_;       // where the package's definitions were unpacked
  bool smallFilesOwned_ = false;  // made for this install, removed after it
  std::vector<Row> rows_;
  bool busy_ = false;
  juce::Array<juce::File> justInstalled_;

  juce::Label heading_, folderLabel_, summary_, status_;
  juce::TextButton folderButton_{TRANS("Change...")};
  juce::TableListBox table_;
  double progress_ = 0.0;
  juce::ProgressBar bar_{progress_};
  juce::TextButton go_, cancel_button_{TRANS("Cancel")}, openButton_, close_{TRANS("Close")};
  std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace mp::ui
