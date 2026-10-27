// Back up and restore, from the Settings menu: this organ's settings or all
// of them to one file and back, and parts of another organ's settings copied
// to this one. The files themselves are SettingsArchive's; this is the
// asking, and reading in what was written.
#include "Ui.h"

#include "Mobile.h"
#include "../mp_audio/SettingsArchive.h"

#include <unordered_set>

namespace mp::ui {

namespace {

namespace sa = mp::settings;

void tell(const juce::String& title, const juce::String& message, bool problem = false) {
  juce::AlertWindow::showMessageBoxAsync(
      problem ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::InfoIcon, title, message);
}

juce::String savedIn(const juce::File& backup) {
  return backup == juce::File()
             ? juce::String()
             : "\n\n" + TRANS("What was there before is saved in %1.").replace("%1", backup.getFullPathName());
}

juce::File defaultFolder() {
  return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}

}  // namespace

void MasterpieceEditor::exportSettings(bool all) {
  // On disk first, so the file holds what the player sees.
  proc_.saveEverythingPending();
  const juce::String key(proc_.organKey());
  const juce::String name = juce::String::fromUTF8(proc_.organModel().organName.c_str());
  const juce::String fileName = all ? juce::String("Masterpiece settings.zip") : key + " settings.zip";
  backupChooser_ = std::make_unique<juce::FileChooser>(
      all ? TRANS("Export all settings") : TRANS("Export this organ's settings"),
      defaultFolder().getChildFile(fileName), "*.zip");
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  backupChooser_->launchAsync(
      juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
          juce::FileBrowserComponent::warnAboutOverwriting,
      [self, all, key, name](const juce::FileChooser& fc) {
        if (self == nullptr) return;
        juce::File zip = fc.getResult();
        if (zip == juce::File()) return;
        if (!zip.hasFileExtension("zip")) zip = zip.withFileExtension("zip");
        juce::String error;
        const auto root = MasterpieceProcessor::settingsRoot();
        const bool ok = all ? sa::exportAll(root, sa::programVersion(), zip, error)
                            : sa::exportOrgan(root, key, name.isNotEmpty() ? name : key, sa::programVersion(), zip,
                                              error);
        if (ok)
          tell(TRANS("Settings exported"), TRANS("Saved to %1.").replace("%1", zip.getFullPathName()));
        else
          tell(TRANS("Settings not exported"), error, true);
      });
}

void MasterpieceEditor::importSettings() {
  backupChooser_ = std::make_unique<juce::FileChooser>(TRANS("Import settings"), defaultFolder(), "*.zip");
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  backupChooser_->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [self](const juce::FileChooser& fc) {
        if (self == nullptr) return;
        const juce::File zip = fc.getResult();
        if (zip == juce::File()) return;
        sa::Contents c;
        juce::String error;
        if (!sa::inspect(zip, c, error)) {
          tell(TRANS("Settings not imported"), error, true);
          return;
        }
        const juce::String loadedKey(self->proc_.organKey());
        const bool hasOrgan = !self->proc_.loadedOrganFile().getFullPathName().isEmpty();
        const bool touchesLoaded = hasOrgan && (c.scope == sa::Scope::All || c.organKey == loadedKey);
        juce::String what;
        if (c.scope == sa::Scope::All) {
          what = TRANS("Every setting in this file: the program's own and those of each organ in it. They replace "
                       "the settings you have now.");
        } else {
          const juce::String who = c.organName.isNotEmpty() ? c.organName : c.organKey;
          what = TRANS("The settings of %1. They replace that organ's settings.").replace("%1", who);
          if (!touchesLoaded)
            what << " " << TRANS("It is not the organ loaded: they are used the next time it is loaded.");
        }
        what << "\n\n"
             << TRANS("%1 files, written by Masterpiece %2. What is there now is saved in the backups folder first.")
                    .replace("%1", juce::String(c.paths.size()))
                    .replace("%2", c.program);
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, TRANS("Import settings"), what, TRANS("Import"),
            TRANS("Cancel"), nullptr,
            juce::ModalCallbackFunction::create([self, zip, c, touchesLoaded](int choice) {
              if (self == nullptr || choice != 1) return;
              // What the player has now goes into the backup too.
              self->proc_.saveEverythingPending();
              juce::File backup;
              juce::String err;
              if (!sa::restore(zip, MasterpieceProcessor::settingsRoot(), sa::programVersion(), backup, err)) {
                tell(TRANS("Settings not imported"), err, true);
                return;
              }
              self->adoptRestoredSettings(touchesLoaded, c.scope == sa::Scope::All);
              tell(TRANS("Settings imported"), TRANS("The settings are in place.") + savedIn(backup));
            }));
      });
}

void MasterpieceEditor::copySettingsFrom() {
  const auto root = MasterpieceProcessor::settingsRoot();
  const juce::String to(proc_.organKey());
  juce::StringArray sources = sa::organKeys(root);
  sources.removeString(to);
  if (sources.isEmpty()) {
    tell(TRANS("Copy settings from another organ"), TRANS("No other organ has settings saved yet."));
    return;
  }

  // The window and the three choices, kept alive for as long as it is open.
  struct Dialog {
    juce::AlertWindow window{TRANS("Copy settings from another organ"),
                             TRANS("They replace this organ's own. They mean something only between versions of "
                                   "the same organ, a demo and the full set: the ids of stops, ranks and pipes "
                                   "must be the same."),
                             juce::MessageBoxIconType::QuestionIcon};
    juce::ToggleButton midi{TRANS("MIDI map")}, combinations{TRANS("Combinations")},
        voicing{TRANS("Mixer and voicing")};
  };
  auto d = std::make_shared<Dialog>();
  d->window.addComboBox("from", sources, TRANS("From"));
  for (auto* t : {&d->midi, &d->combinations, &d->voicing}) {
    t->setToggleState(true, juce::dontSendNotification);
    t->setSize(320, 26);
    d->window.addCustomComponent(t);
  }
  d->window.addButton(TRANS("Copy"), 1, juce::KeyPress(juce::KeyPress::returnKey));
  d->window.addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  d->window.enterModalState(
      true, juce::ModalCallbackFunction::create([self, d, sources, to, root](int result) {
        if (self == nullptr || result != 1) return;
        const juce::String from = sources[d->window.getComboBoxComponent("from")->getSelectedItemIndex()];
        const int parts = (d->midi.getToggleState() ? sa::MidiMap : 0) |
                          (d->combinations.getToggleState() ? sa::Combinations : 0) |
                          (d->voicing.getToggleState() ? sa::MixerAndVoicing : 0);
        if (from.isEmpty() || parts == 0) return;

        // How much of the mixer and voicing names something in this organ.
        juce::String fit;
        if ((parts & sa::MixerAndVoicing) != 0) {
          const auto ids = sa::voicingIds(root, from);
          const auto& model = self->proc_.organModel();
          int ranks = 0, pipes = 0;
          for (auto id : ids.ranks)
            if (model.ranks.count(static_cast<Id>(id)) != 0) ++ranks;
          if (!ids.pipes.isEmpty()) {
            std::unordered_set<Id> here;
            for (const auto& [rid, rank] : model.ranks)
              for (const auto& pipe : rank.pipes) here.insert(pipe.pipeId);
            for (auto id : ids.pipes)
              if (here.count(static_cast<Id>(id)) != 0) ++pipes;
          }
          if (!ids.ranks.isEmpty() || !ids.pipes.isEmpty())
            fit = "\n\n" + TRANS("Of the %1 ranks and %2 pipes its mixer and voicing name, %3 and %4 are in this organ.")
                               .replace("%1", juce::String(ids.ranks.size()))
                               .replace("%2", juce::String(ids.pipes.size()))
                               .replace("%3", juce::String(ranks))
                               .replace("%4", juce::String(pipes));
        }
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, TRANS("Copy settings from another organ"),
            TRANS("Copy from %1 to this organ? What this organ has now is saved in the backups folder first.")
                    .replace("%1", from) +
                fit,
            TRANS("Copy"), TRANS("Cancel"), nullptr,
            juce::ModalCallbackFunction::create([self, from, to, parts, root](int choice) {
              if (self == nullptr || choice != 1) return;
              self->proc_.saveEverythingPending();
              juce::File backup;
              juce::String err;
              if (!sa::copy(root, from, to, parts, sa::programVersion(), backup, err)) {
                tell(TRANS("Settings not copied"), err, true);
                return;
              }
              self->adoptRestoredSettings(true, false);
              tell(TRANS("Settings copied"), TRANS("The settings are in place.") + savedIn(backup));
            }));
      }));
}

void MasterpieceEditor::adoptRestoredSettings(bool loadedOrganTouched, bool globalTouched) {
  if (!loadedOrganTouched && !globalTouched) return;
  // What is in memory is older than the files now: nothing of it may be
  // written back before they are read.
  proc_.holdSavesUntilReload();
  if (!proc_.loadedOrganFile().getFullPathName().isEmpty()) {
    // A load reads the organ's files and the program's own.
    reloadOrgan();
    return;
  }
  proc_.loadGlobalDefaults();
  proc_.releaseSaves();
}

}  // namespace mp::ui
