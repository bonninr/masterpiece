#include "Settings.h"
#include "../mp_audio/DataFolder.h"
#include "../mp_audio/MidiPorts.h"
#include "../mp_audio/SystemMemory.h"
#include "../mp_core/OrganStructure.h"

#include <array>
#include <cmath>
#include <map>
#include <iterator>
#include <limits>

#include "ManualDialog.h"
#include "MidiEventDialog.h"
#include "../mp_control/GrandOrgueMidi.h"
#include "Ui.h"
#include "UpdateCheck.h"
#include "Mobile.h"
#include "../mp_audio/Translations.h"

namespace mp::ui {
namespace {

// A compression level's item in the level list.
int levelItem(SplitRiceLevel level) { return static_cast<int>(level) + 1; }

// The sample cache's files in a folder: one per organ or a single one, and
// the part file of a write that stopped.
juce::Array<juce::File> sampleCacheFiles(const juce::File& dir) {
  juce::Array<juce::File> files;
  if (!dir.isDirectory()) return files;
  dir.findChildFiles(files, juce::File::findFiles, false, "samples*.mpcache");
  dir.findChildFiles(files, juce::File::findFiles, false, "samples*.mpcache.part");
  return files;
}

void styleLabel(juce::Label& l, const juce::String& text) {
  l.setText(text, juce::dontSendNotification);
  l.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
}

// A rank as the Outputs and Voicing tabs name it (#634): its name, with its
// number in the definition only where another rank has the same name, and
// the number alone where it has none.
juce::String rankTitle(const OrganModel& m, Id rankId) {
  const auto it = m.ranks.find(rankId);
  const juce::String number(static_cast<int>(rankId));
  if (it == m.ranks.end() || it->second.name.empty()) return number;
  int same = 0;
  for (const auto& [id, r] : m.ranks)
    if (r.name == it->second.name) ++same;
  const juce::String name(it->second.name);
  return same > 1 ? name + " (" + number + ")" : name;
}

void styleNote(juce::Label& l, const juce::String& text) {
  l.setText(text, juce::dontSendNotification);
  l.setColour(juce::Label::textColourId, juce::Colours::grey);
  l.setJustificationType(juce::Justification::topLeft);
  l.setFont(juce::Font(juce::FontOptions(12.0f)));
}

// The height a label's text takes wrapped to this width, with a line spare.
int wrappedHeight(const juce::Label& l, int width) {
  juce::AttributedString s(l.getText());
  s.setFont(l.getFont());
  juce::TextLayout layout;
  const auto border = l.getBorderSize();
  layout.createLayout(s, static_cast<float>(width - border.getLeftAndRight()));
  return static_cast<int>(std::ceil(layout.getHeight() + l.getFont().getHeight())) +
         border.getTopAndBottom();
}

constexpr int kRow = 28;
// The Release length choices, in milliseconds; 0 keeps each release whole.
// The two shortest are a tracker action's quick release.
constexpr int kReleaseLengthsMs[] = {0, 150, 300, 500, 1000, 1500, 2000, 3000, 4000};
constexpr int kReleaseLengthCount = static_cast<int>(std::size(kReleaseLengthsMs));
constexpr int kGap = 6;

// The recorder's buttons are sized from their own text (#429): a fixed width
// that fits the English label squeezed the French one, whose words are
// longer.
constexpr int kMargin = 12;         // the panel's own margin
constexpr int kMinButtonWidth = 70; // narrower than this and it stops looking like a button
constexpr int kButtonPadding = 12;  // the room the text has inside the button

// The font a text button of this height is drawn with: the look and feel's
// own choice, which is the font the button will draw its text in.
juce::Font buttonFont(juce::TextButton& button, int height) {
  return button.getLookAndFeel().getTextButtonFont(button, height);
}

// The width a button needs for its text: the text measured in the button's
// font, with the padding around it, and never below the minimum.
int buttonWidth(const juce::TextButton& button, const juce::Font& font) {
  const int text = juce::GlyphArrangement::getStringWidthInt(font, button.getButtonText());
  return juce::jmax(kMinButtonWidth, text + 2 * kButtonPadding);
}

// The width a row of buttons takes.
int rowWidth(std::initializer_list<juce::TextButton*> buttons, const juce::Font& font) {
  int total = 0;
  for (auto* b : buttons) total += buttonWidth(*b, font) + kGap;
  return total > 0 ? total - kGap : 0; // no gap after the last button
}

// The combo box's ids for the resident formats. Twenty bits sits at 4: the
// other three keep the ids they had, so a saved choice still means the same
// thing.
int storageItemId(SampleStorage s) {
  switch (s) {
    case SampleStorage::Int16: return 2;
    case SampleStorage::Int20: return 4;
    case SampleStorage::Float32: return 3;
    case SampleStorage::Int24: break;
  }
  return 1;
}

SampleStorage storageOfItem(int id) {
  switch (id) {
    case 2: return SampleStorage::Int16;
    case 4: return SampleStorage::Int20;
    case 3: return SampleStorage::Float32;
    default: break;
  }
  return SampleStorage::Int24;
}

} // namespace

// ---------------------------------------------------------------- engine

EnginePanel::EnginePanel(MasterpieceProcessor& p, Part part) : proc_(p), part_(part) {
  // Before a single control is wired: what Revert goes back to.
  openSwitch_ = proc_.engineSwitch();
  openPreload_ = proc_.preloadHeadFrames();
  openStorage_ = proc_.sampleStorage();
  openStream_ = proc_.streamReleases();
  openStreamPercent_ = proc_.streamHeadPercent();
  openReleaseBuffer_ = proc_.releaseBufferSeconds();
  openMono_ = proc_.loadMono();
  openCompress_ = proc_.codeSamples();
  openLevel_ = proc_.codeLevel();
  openProgramCouplers_ = proc_.programCouplers();
  addAndMakeVisible(programCouplers_);
  programCouplers_.setToggleState(openProgramCouplers_, juce::dontSendNotification);
  programCouplers_.onClick = [this] {
    proc_.setProgramCouplers(programCouplers_.getToggleState());
  };
  openGoTremulantPitch_ = proc_.grandOrgueTremulantPitch();
  openGoSwellFilter_ = proc_.grandOrgueSwellFilter();
  addAndMakeVisible(goTremulantPitch_);
  goTremulantPitch_.setToggleState(openGoTremulantPitch_, juce::dontSendNotification);
  goTremulantPitch_.setTooltip(TRANS("Adds a pitch swing to this organ's tremulants, in step with the level "
                                     "swing its definition sets"));
  goTremulantPitch_.onClick = [this] {
    proc_.setGrandOrgueTremulantPitch(goTremulantPitch_.getToggleState());
  };
  addAndMakeVisible(goSwellFilter_);
  goSwellFilter_.setToggleState(openGoSwellFilter_, juce::dontSendNotification);
  goSwellFilter_.setTooltip(TRANS("Darkens the tone of this organ's swell boxes as they close, on top of the "
                                  "level change its definition sets"));
  goSwellFilter_.onClick = [this] {
    proc_.setGrandOrgueSwellFilter(goSwellFilter_.getToggleState());
  };
  openRate_ = proc_.loadSampleRate();
  openReleases_ = proc_.releaseChoice();
  openReleaseMs_ = proc_.releaseMaxMs();
  openSparse_ = proc_.sparseEvery();
  openCache_ = proc_.cacheMode();
  openLoadTicks_ = proc_.loadTicks();
  openReopen_ = proc_.reopenLastOrgan();
  openFaster_ = proc_.fasterEngine();
  openRealtime_ = proc_.realtimeThreads();
  openEveryProcessor_ = proc_.everyProcessor();
  openMemLimit_ = proc_.memoryLimitSettingMB();
  openPolyphony_ = proc_.polyphonyLimit();

  for (auto* b : {&simpleWav_, &wind_, &bellows_, &tremulant_, &enclosure_, &voicing_,
                  &originalPitch_}) {
    addAndMakeVisible(*b);
    b->onClick = [this] { pushSwitches(); };
  }

  const auto sw = proc_.engineSwitch();
  simpleWav_.setToggleState(sw.simpleWavOnly, juce::dontSendNotification);
  wind_.setToggleState(sw.enableWindModel, juce::dontSendNotification);
  bellows_.setToggleState(sw.enableBellowsBoard, juce::dontSendNotification);
  tremulant_.setToggleState(sw.enableTremulant, juce::dontSendNotification);
  enclosure_.setToggleState(sw.enableEnclosure, juce::dontSendNotification);
  voicing_.setToggleState(sw.enableVoicing, juce::dontSendNotification);
  originalPitch_.setToggleState(sw.playAtOriginalOrganPitch,
                               juce::dontSendNotification);

  // The profile is the control most players should ever need. It is first on
  // the panel and it moves all four settings below it; those stay visible
  // because someone who knows their machine should not have to accept a
  // profile to get at them.
  addAndMakeVisible(profileLabel_);
  styleLabel(profileLabel_, TRANS("Quality"));
  addAndMakeVisible(profile_);
  // Named for what they do, not for a machine they suit. The figures that
  // actually decide whether one fits are in the readout below the controls,
  // measured on the organ in front of you rather than assumed.
  profile_.addItem(TRANS("Best: as recorded, all in memory"), 1);
  profile_.addItem(TRANS("High: as recorded, compressed, releases streamed"), 2);
  profile_.addItem(TRANS("Recommended: 16-bit, releases streamed"), 3);
  profile_.addItem(TRANS("Compact: 16-bit, compressed, releases streamed"), 4);
  profile_.addItem(TRANS("Small: 16-bit mono, compressed, releases streamed"), 5);
  profile_.addItem(TRANS("Smallest: as Small, short releases only"), 6);
  profile_.addItem(TRANS("Custom"), 7);
  profile_.onChange = [this] {
    if (applyingProfile_) return;
    if (profile_.getSelectedId() != 7) applyProfile(profile_.getSelectedId());
  };

  addAndMakeVisible(preloadLabel_);
  styleLabel(preloadLabel_, TRANS("Preloaded per sample"));
  addAndMakeVisible(preload_);
  // Two real choices. An attack's head is always extended to the end of its
  // sustain loop, and in an organ sample the loop runs nearly to the end of
  // the file, so a second or two past it, or a share of the file, came to
  // the same thing as the loop alone. The seconds an earlier version
  // offered are still shown while they are in force.
  preload_.addItem(TRANS("Whole samples (most memory)"), 1);
  preload_.addItem(TRANS("Up to the loop (less memory)"), 4);
  showPreload(proc_.preloadHeadFrames());
  preload_.onChange = [this] {
    const int id = preload_.getSelectedId();
    if (id == 2 || id == 3) return;  // an earlier version's choice, left as it is
    proc_.setPreloadHeadFrames(id == 4 ? 1 : 0);
    syncProfile();
  };

  addAndMakeVisible(storageLabel_);
  styleLabel(storageLabel_, TRANS("Resident format"));
  addAndMakeVisible(storage_);
  storage_.addItem(TRANS("As recorded (16 or 24-bit)"), 1);
  storage_.addItem(TRANS("20-bit"), 4);
  storage_.addItem(TRANS("16-bit"), 2);
  storage_.addItem(TRANS("32-bit float"), 3);
  storage_.setSelectedId(storageItemId(proc_.sampleStorage()), juce::dontSendNotification);
  storage_.onChange = [this] {
    proc_.setSampleStorage(storageOfItem(storage_.getSelectedId()));
    enableCompress();
    syncProfile();
  };

  // Converting on the way in, rather than once per voice on the way out. A
  // set recorded at 96 kHz and played through a 48 kHz device is otherwise
  // held at twice the size it needs. Converting up is never offered: it
  // invents nothing and costs the memory anyway.
  addAndMakeVisible(rateLabel_);
  styleLabel(rateLabel_, TRANS("Sample rate"));
  addAndMakeVisible(rate_);
  rate_.addItem(TRANS("As recorded"), 1);
  rate_.addItem(TRANS("48 kHz"), 2);
  rate_.addItem(TRANS("44.1 kHz"), 3);
  const auto hz = proc_.loadSampleRate();
  rate_.setSelectedId(hz >= 47000.0 && hz <= 49000.0   ? 2
                      : hz >= 43000.0 && hz <= 45000.0 ? 3
                                                       : 1,
                      juce::dontSendNotification);
  rate_.onChange = [this] {
    const int id = rate_.getSelectedId();
    proc_.setLoadSampleRate(id == 2 ? 48000.0 : id == 3 ? 44100.0 : 0.0);
  };

  // Which releases, and how much of each: for a set played in a room with
  // its own reverberation, under an impulse response, or on a small machine.
  addAndMakeVisible(releasesLabel_);
  styleLabel(releasesLabel_, TRANS("Releases"));
  addAndMakeVisible(releases_);
  releases_.addItem(TRANS("As the organ has them"), 1);
  releases_.addItem(TRANS("The longest only"), 2);
  releases_.addItem(TRANS("The shortest only"), 3);
  releases_.addItem(TRANS("None: notes fade when let go"), 4);
  releases_.setSelectedId(static_cast<int>(proc_.releaseChoice()) + 1, juce::dontSendNotification);
  releases_.setTooltip(TRANS("Which of each pipe's releases are loaded. Many organs record a short "
                             "and a long release for each pipe; one of them uses less memory and "
                             "loads faster."));
  releases_.onChange = [this] {
    proc_.setReleaseChoice(static_cast<SampleLibrary::ReleaseChoice>(releases_.getSelectedId() - 1));
    syncProfile();
  };
  addAndMakeVisible(releaseLengthLabel_);
  styleLabel(releaseLengthLabel_, TRANS("Release length"));
  addAndMakeVisible(releaseLength_);
  showReleaseLength(proc_.releaseMaxMs());
  releaseLength_.setTooltip(TRANS("Each release is kept up to this length and fades out over its "
                                  "last quarter. Shorter releases use less memory: for an organ "
                                  "played in a room with its own reverberation, or under an "
                                  "impulse response.") +
                            " " +
                            TRANS("The two shortest end a note quickly, as a tracker action does: for "
                                  "fast repeated notes."));
  // Where a tail stops: below what can be heard, it costs what a note costs.
  addAndMakeVisible(releaseCutoffLabel_);
  styleLabel(releaseCutoffLabel_, TRANS("Release tails end below"));
  addAndMakeVisible(releaseCutoff_);
  {
    static const int kDb[] = {0, -80, -90, -100};
    releaseCutoff_.addItem(TRANS("Never: every tail plays out"), 1);
    for (int i = 1; i < 4; ++i)
      releaseCutoff_.addItem(TRANS("%1 dB").replace("%1", juce::String(kDb[i])), i + 1);
    int id = 3;
    for (int i = 0; i < 4; ++i)
      if (kDb[i] == proc_.releaseCutoffDb()) id = i + 1;
    releaseCutoff_.setSelectedId(id, juce::dontSendNotification);
    releaseCutoff_.setTooltip(TRANS("A release tail stops once it has stayed below this level for a "
                                    "twentieth of a second. Tails only fade, and a fast passage on "
                                    "many stops leaves hundreds of them sounding far below hearing, "
                                    "each costing as much as a note. Each tail is judged on its "
                                    "own: with many stops drawn, -80 dB can end the last decay of a "
                                    "full chord audibly early, which is why -90 dB is the default."));
    releaseCutoff_.onChange = [this] {
      static const int kDbs[] = {0, -80, -90, -100};
      const int i = releaseCutoff_.getSelectedId() - 1;
      if (i >= 0 && i < 4) proc_.setReleaseCutoffDb(kDbs[i]);
      proc_.markSettingsDirty();
    };
  }
  addAndMakeVisible(releaseShortLabel_);
  styleLabel(releaseShortLabel_, TRANS("Release of a short note"));
  addAndMakeVisible(releaseShort_);
  releaseShort_.addItem(TRANS("At the recorded level"), 1);
  releaseShort_.addItem(TRANS("Scaled to the time held"), 2);
  releaseShort_.setSelectedId(proc_.releaseShortNoteGain() ? 2 : 1, juce::dontSendNotification);
  releaseShort_.setTooltip(TRANS("A note let go during its attack has not reached the level its "
                                 "release was recorded from. Scaled, the release starts lower, up "
                                 "to the recorded level at the end of the attack."));
  releaseShort_.onChange = [this] {
    proc_.setReleaseShortNoteGain(releaseShort_.getSelectedId() == 2);
    proc_.markSettingsDirty();
  };
  releaseLength_.onChange = [this] {
    const int id = releaseLength_.getSelectedId();
    if (id >= 1 && id <= kReleaseLengthCount) proc_.setReleaseMaxMs(kReleaseLengthsMs[id - 1]);
  };

  // How many notes of a rank are loaded. A note that is not loaded plays its
  // nearest loaded neighbour's samples, retuned to its own pitch, so the
  // samples it would have played are never read.
  addAndMakeVisible(sparseLabel_);
  styleLabel(sparseLabel_, TRANS("Notes loaded"));
  addAndMakeVisible(sparse_);
  sparse_.addItem(TRANS("All"), 1);
  sparse_.addItem(TRANS("Every second note"), 2);
  sparse_.addItem(TRANS("Every third note"), 3);
  sparse_.addItem(TRANS("Half, by the sides of the organ"), 4);
  sparse_.setSelectedId(proc_.sparseEvery(), juce::dontSendNotification);
  sparse_.setTooltip(TRANS("Saves memory: a note that is not loaded plays its nearest loaded "
                            "neighbour's samples, retuned to its own pitch. Applies when the "
                            "organ is loaded again."));
  sparse_.onChange = [this] { proc_.setSparseEvery(sparse_.getSelectedId()); };

  // Keeping the decoded samples, so the next load of the same organ is a read
  // rather than twelve thousand decodes. One file per organ (#1033), replaced
  // when the organ's settings change: each organ played loads quickly the
  // second time, and an organ can opt out in its own settings.
  addAndMakeVisible(cacheLabel_);
  styleLabel(cacheLabel_, TRANS("Sample cache"));
  addAndMakeVisible(cache_);
  cache_.addItem(TRANS("On"), 2);
  cache_.addItem(TRANS("Off"), 3);
  cache_.setSelectedId(proc_.cacheMode() == SampleLibrary::CacheMode::Off ? 3 : 2, juce::dontSendNotification);
  cache_.setTooltip(TRANS("On: each organ keeps one sample cache, so its next load is quick. A change to its "
                          "loading settings rebuilds that cache in place. An organ's own settings can turn "
                          "its cache on or off."));
  cache_.onChange = [this] {
    proc_.setCacheMode(cache_.getSelectedId() == 3 ? SampleLibrary::CacheMode::Off
                                                   : SampleLibrary::CacheMode::PerOrgan);
  };

  // When the cache of a new load is written: before the organ plays, as
  // GrandOrgue does, or while it plays, in its silences.
  addAndMakeVisible(cacheWhenLabel_);
  styleLabel(cacheWhenLabel_, TRANS("Save the sample cache"));
  addAndMakeVisible(cacheWhen_);
  cacheWhen_.addItem(TRANS("Before the organ plays"), 1);
  cacheWhen_.addItem(TRANS("In silence, while it plays"), 2);
  cacheWhen_.setSelectedId(proc_.cacheWhilePlaying() ? 2 : 1, juce::dontSendNotification);
  cacheWhen_.setTooltip(TRANS("Before the organ plays: the first load of an organ takes longer, and "
                              "nothing competes with its sound. In silence: the organ plays at once, "
                              "and the samples are saved whenever nothing sounds; until they are, the "
                              "next load reads them again."));
  cacheWhen_.onChange = [this] { proc_.setCacheWhilePlaying(cacheWhen_.getSelectedId() == 2); };

  // Said beside the choices while one of them will rebuild the cache: the
  // question comes when the window closes, and the wait after it.
  addChildComponent(rebuildNote_);
  styleLabel(rebuildNote_, TRANS("The cache is rebuilt when this window closes. It can take several minutes."));
  rebuildNote_.setColour(juce::Label::textColourId, juce::Colour(0xffe0b050));

  // Where that cache is written. It is as large as the organs played through
  // it, so a machine with a small fast disk and a large slow one needs to be
  // able to say which holds it.
  addAndMakeVisible(cacheDirLabel_);
  styleLabel(cacheDirLabel_, TRANS("Cache folder"));
  addAndMakeVisible(cacheDirValue_);
  cacheDirValue_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  showCacheDir();
  addAndMakeVisible(cacheDirChoose_);
  cacheDirChoose_.onClick = [this] {
    cacheDirChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Where should the sample cache be written?"), proc_.cacheDirectory());
    cacheDirChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc) {
          const auto dir = pickedFile(fc);
          if (dir.getFullPathName().isEmpty()) return;
          proc_.setCacheDirectory(dir);
          showCacheDir();
        });
  };
  addAndMakeVisible(cacheDirDefault_);
  cacheDirDefault_.onClick = [this] {
    proc_.setCacheDirectory(juce::File());
    showCacheDir();
  };
  // The cache files in that folder, removed in one click: they run to
  // gigabytes, and a disk they filled needs them gone without a load (#519).
  addAndMakeVisible(cacheDelete_);
  cacheDelete_.onClick = [this] {
    if (proc_.cacheWriting()) return;
    for (const auto& f : sampleCacheFiles(proc_.cacheDirectory())) f.deleteFile();
    showCacheDir();
  };

  // Where the player keeps packages: a composite looks here, and in the
  // folders directly in it, for the organs it plays (#289).
  addAndMakeVisible(packageDirLabel_);
  styleLabel(packageDirLabel_, TRANS("Package folder"));
  packageDirLabel_.setTooltip(TRANS("A composite organ looks here, and in the folders directly in it, for the "
                                    "organs it plays."));
  addAndMakeVisible(packageDirValue_);
  packageDirValue_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  showPackageDir();
  addAndMakeVisible(packageDirChoose_);
  packageDirChoose_.onClick = [this] {
    packageDirChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Where are packages kept?"),
        proc_.packageDirectory().getFullPathName().isNotEmpty()
            ? proc_.packageDirectory()
            : juce::File::getSpecialLocation(juce::File::userHomeDirectory));
    packageDirChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc) {
          const auto dir = pickedFile(fc);
          if (dir.getFullPathName().isEmpty()) return;
          proc_.setPackageDirectory(dir);
          showPackageDir();
        });
  };
  addAndMakeVisible(packageDirClear_);
  packageDirClear_.onClick = [this] {
    proc_.setPackageDirectory(juce::File());
    showPackageDir();
  };

  // Where this organ's OrganInstallationPackages is. Normally worked out
  // from the definition's path; a set whose folders are linked in from
  // another tree can leave that path pointing nowhere near the audio, and
  // then only the player knows. Takes effect the next time it loads.
  addAndMakeVisible(displayModeLabel_);
  styleLabel(displayModeLabel_, TRANS("Display mode"));
  addAndMakeVisible(displayMode_);
  displayMode_.addItem(TRANS("Simple"), 1);
  displayMode_.addItem(TRANS("Advanced"), 2);
  displayMode_.setTooltip(TRANS("Simple: the main settings, with presets. Advanced: every setting."));
  displayMode_.onChange = [this] {
    const bool simple = displayMode_.getSelectedId() == 1;
    if (simple != proc_.simpleMode()) proc_.setSimpleMode(simple);
  };

  addAndMakeVisible(organRootLabel_);
  styleLabel(organRootLabel_, TRANS("Organ folder"));
  organRootLabel_.setTooltip(TRANS("Folder holding this organ's OrganInstallationPackages"));
  addAndMakeVisible(organRootValue_);
  organRootValue_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  showOrganRoot();
  addAndMakeVisible(organRootChoose_);
  organRootChoose_.onClick = [this] {
    organRootChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Which folder holds OrganInstallationPackages?"),
        juce::File(proc_.organRootDir()));
    organRootChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc) {
          const auto dir = pickedFile(fc);
          if (dir.getFullPathName().isEmpty()) return;
          proc_.setOrganRootOverride(dir);
          proc_.markSettingsDirty();
          showOrganRoot();
        });
  };
  addAndMakeVisible(organRootDefault_);
  organRootDefault_.onClick = [this] {
    proc_.setOrganRootOverride(juce::File());
    proc_.markSettingsDirty();
    showOrganRoot();
  };

  addAndMakeVisible(stream_);
  stream_.setToggleState(proc_.streamReleases(), juce::dontSendNotification);
  stream_.onClick = [this] {
    proc_.setStreamReleases(stream_.getToggleState());
    enableStreamControls(stream_.getToggleState());
    syncProfile();
  };
  // How much of each release stays in memory while streaming. More gives a
  // slow disk longer to catch up before a release goes quiet (#120).
  addAndMakeVisible(streamHead_);
  streamHead_.addItem(TRANS("Hold the first second"), 1);
  streamHead_.addItem(TRANS("Hold 25% of each release"), 2);
  streamHead_.addItem(TRANS("Hold 50% of each release"), 3);
  streamHead_.addItem(TRANS("Hold 75% of each release"), 4);
  showStreamPercent(proc_.streamHeadPercent());
  streamHead_.onChange = [this] {
    const int id = streamHead_.getSelectedId();
    proc_.setStreamHeadPercent(id == 2 ? 25 : id == 3 ? 50 : id == 4 ? 75 : 0);
  };
  // The release buffer (#548): Automatic times reads of the organ's own
  // files at each load and picks the length that disk needs.
  addAndMakeVisible(releaseBufferLabel_);
  styleLabel(releaseBufferLabel_, TRANS("Release buffer"));
  addAndMakeVisible(releaseBuffer_);
  releaseBuffer_.setTitle(TRANS("Release buffer"));
  showReleaseBuffer(proc_.releaseBufferSeconds());
  releaseBuffer_.setTooltip(
      TRANS("How much of each streamed release a voice reads ahead of where it plays. Automatic "
            "times reads of the organ's own files at each load: a quarter of a second on an SSD, "
            "up to 2 s on a hard disk. A longer buffer gives a busy disk more time and uses more "
            "memory per voice."));
  releaseBuffer_.onChange = [this] {
    static const double kSeconds[] = {0.0, 0.25, 0.5, 1.0, 2.0};
    const int i = releaseBuffer_.getSelectedId() - 1;
    if (i >= 0 && i < 5) proc_.setReleaseBufferSeconds(kSeconds[i]);
    fillPolyphony();
  };
  enableStreamControls(proc_.streamReleases());

  addAndMakeVisible(mono_);
  mono_.setToggleState(proc_.loadMono(), juce::dontSendNotification);
  mono_.onClick = [this] {
    proc_.setLoadMono(mono_.getToggleState());
    syncProfile();
  };

  // Lossless compression of the resident samples (#447): what plays is the
  // same to the bit, for less memory and a little more work per voice.
  addAndMakeVisible(compress_);
  compress_.setToggleState(proc_.codeSamples(), juce::dontSendNotification);
  compress_.setTooltip(
      TRANS("16- and 24-bit samples are held in a lossless compressed form and decoded as "
            "they play. The sound is identical; memory use is lower. Applies at the next load."));
  compress_.onClick = [this] {
    proc_.setCodeSamples(compress_.getToggleState());
    enableCompress();
    syncProfile();
  };
  // Light holds a little more in memory and decodes with the least work, for
  // a computer that runs short of time at medium. High and Max hold 16-bit
  // samples in less memory for more work; a 24-bit sample is coded medium.
  addAndMakeVisible(compressLevel_);
  compressLevel_.addItem(TRANS("Light compression"), levelItem(mp::SplitRiceLevel::Light));
  compressLevel_.addItem(TRANS("Medium compression"), levelItem(mp::SplitRiceLevel::Medium));
  compressLevel_.addItem(TRANS("High compression"), levelItem(mp::SplitRiceLevel::High));
  compressLevel_.addItem(TRANS("Maximum compression"), levelItem(mp::SplitRiceLevel::Max));
  compressLevel_.setSelectedId(levelItem(proc_.codeLevel()), juce::dontSendNotification);
  compressLevel_.setTooltip(
      TRANS("Light compression takes the least processor time per voice. Medium compression "
            "holds the samples in about a third of their memory. High and Maximum compression "
            "hold 16-bit samples in less memory for more processor time. Changes apply at the "
            "next load."));
  compressLevel_.onChange = [this] {
    proc_.setCodeLevel(static_cast<mp::SplitRiceLevel>(compressLevel_.getSelectedId() - 1));
  };
  enableCompress();

  // Audible load progress for large organs. General config: written to the
  // global file at once rather than kept for an organ, and off unless asked.
  addAndMakeVisible(loadTicks_);
  loadTicks_.setToggleState(proc_.loadTicks(), juce::dontSendNotification);
  loadTicks_.setTooltip(
      TRANS("Play a short tap at each 10% of an organ load, so a large set can be "
            "followed by ear."));
  loadTicks_.onClick = [this] { proc_.setLoadTicks(loadTicks_.getToggleState()); };

  // Startup and memory: general config, written to the global file at once.
  addAndMakeVisible(reopen_);
  reopen_.setToggleState(proc_.reopenLastOrgan(), juce::dontSendNotification);
  reopen_.setTooltip(
      TRANS("Off by default. On, the organ last loaded completely opens at start, "
            "after an unexpected close too."));
  reopen_.onClick = [this] { proc_.setReopenLastOrgan(reopen_.getToggleState()); };
  // The vector engine, on unless the player turns it off. It changes at
  // once, so the same chord can be heard both ways.
  addAndMakeVisible(faster_);
  faster_.setToggleState(proc_.fasterEngine(), juce::dontSendNotification);
  faster_.setTooltip(TRANS("Renders several samples at a time with the processor's vector unit (AVX2 on "
                           "a PC, NEON on ARM): the same sound, with more polyphony to spare. On by default. "
                           "If anything sounds wrong with it on, turn it off and please report it."));
  faster_.onClick = [this] {
    proc_.setFasterEngine(faster_.getToggleState());
    showFasterUnit();
    syncPerformance();
  };
  showFasterUnit();
  // Real-time render threads, on by default. Android gives an app no way to
  // have them.
  if (MasterpieceProcessor::realtimeThreadsSupported())
    addAndMakeVisible(realtime_);
  else
    addChildComponent(realtime_);
  realtime_.setToggleState(proc_.realtimeThreads(), juce::dontSendNotification);
  realtime_.setTooltip(TRANS("Makes the threads that render the organ real-time threads, so the system runs "
                             "them first, on the fastest cores: MMCSS on Windows, SCHED_FIFO or rtkit on Linux, "
                             "the audio workgroup on macOS."));
  realtime_.onClick = [this] {
    proc_.setRealtimeThreads(realtime_.getToggleState());
    showRealtimeUnit();
    syncPerformance();
  };
  showRealtimeUnit();
  addAndMakeVisible(everyProcessor_);
  everyProcessor_.setToggleState(proc_.everyProcessor(), juce::dontSendNotification);
  everyProcessor_.setTooltip(TRANS("Renders the organ on every logical processor, with none kept for the "
                                   "interface, disk streaming and the system: more polyphony on a machine "
                                   "doing nothing else. Off: every processor but one, eight at most."));
  everyProcessor_.onClick = [this] {
    proc_.setEveryProcessor(everyProcessor_.getToggleState());
    syncPerformance();
  };
  addAndMakeVisible(portable_);
  portable_.setToggleState(proc_.keepsPortableCopy(), juce::dontSendNotification);
  portable_.setTooltip(TRANS("Keeps this organ's own sample cache, whatever the Sample cache setting says, "
                             "and a copy of its definition and console pictures, so it opens with the drive "
                             "holding its installation files unplugged. Made at the next complete load; costs "
                             "the cache's size on this disk. Streamed release tails still come from the "
                             "installation files: turn streaming off for an organ that must play without them."));
  portable_.onClick = [this] { proc_.setKeepPortableCopy(portable_.getToggleState()); };

  // A cache for the organs played most, none for one that is rarely played
  // or very large (#1033).
  addAndMakeVisible(organCacheLabel_);
  styleLabel(organCacheLabel_, TRANS("Sample cache for this organ"));
  addAndMakeVisible(organCache_);
  organCache_.addItem(TRANS("As in Settings"), 1);
  organCache_.addItem(TRANS("Its own cache"), 2);
  organCache_.addItem(TRANS("No cache"), 3);
  using OrganCache = MasterpieceProcessor::OrganCache;
  organCache_.setSelectedId(proc_.organCache() == OrganCache::Own    ? 2
                            : proc_.organCache() == OrganCache::None ? 3
                                                                     : 1,
                            juce::dontSendNotification);
  organCache_.setTooltip(TRANS("Its own cache: this organ keeps a sample cache whatever the Sample cache "
                               "setting says, so its next load is quick. No cache: this organ loads from its "
                               "files every time and uses no disk for a cache. Takes effect at the next load."));
  organCache_.onChange = [this] {
    const int id = organCache_.getSelectedId();
    proc_.setOrganCache(id == 2   ? MasterpieceProcessor::OrganCache::Own
                        : id == 3 ? MasterpieceProcessor::OrganCache::None
                                  : MasterpieceProcessor::OrganCache::AsProgram);
  };
  addAndMakeVisible(organCacheDelete_);
  organCacheDelete_.onClick = [this] {
    proc_.deleteOrganCache();
    showOrganCache();
    showCacheDir();
  };
  showOrganCache();

  addAndMakeVisible(memLimitLabel_);
  styleLabel(memLimitLabel_, TRANS("Memory limit for samples"));
  addAndMakeVisible(memLimit_);
  memLimit_.setTooltip(
      TRANS("A load that would take more than this stops and says so, instead of "
            "running the computer out of memory."));
  fillMemLimit();
  memLimit_.onChange = [this] {
    const int id = memLimit_.getSelectedId();
    // Ids above 1000 are a size in megabytes plus 1000; 1 is the default.
    proc_.setMemoryLimitMB(id == 1 ? 0 : id - 1000);
  };

  addAndMakeVisible(polyphonyLabel_);
  styleLabel(polyphonyLabel_, TRANS("Polyphony limit"));
  addAndMakeVisible(polyphony_);
  polyphony_.setTooltip(TRANS("The most pipes that sound at once, applied as soon as it is chosen. Past it, a new pipe takes the place of one that is dying away, or of the quietest. Each choice shows the memory the voices take, more with release streaming on; a lower number keeps a small computer from falling behind on full organ."));
  fillPolyphony();
  // Any number can be typed over the list, for a limit between its steps
  // (#422).
  polyphony_.setEditableText(true);
  polyphony_.onChange = [this] {
    const int id = polyphony_.getSelectedId();
    if (id == 0) {
      // Typed: the first number in the text, kept within what the engine
      // takes; anything else puts the list back as it was.
      const int typed = polyphony_.getText().retainCharacters("0123456789").getIntValue();
      if (typed > 0) proc_.setPolyphonyLimit(typed);
      fillPolyphony();
    } else {
      // Ids above 1000 are a number of voices plus 1000; 1 is every voice.
      proc_.setPolyphonyLimit(id == 1 ? 0 : id - 1000);
    }
    syncPerformance();
  };

  addAndMakeVisible(performanceLabel_);
  styleLabel(performanceLabel_, TRANS("Performance"));
  addAndMakeVisible(performance_);
  // Named by what the player gets, and chosen by ear (#634).
  performance_.addItem(TRANS("Most voices: every processor core, 8192 voices"), 1);
  performance_.addItem(TRANS("Balanced (default): every core, the default number of voices"), 2);
  performance_.addItem(TRANS("Steady: one core left free for the system and other programs"), 3);
  performance_.addItem(TRANS("Fewer voices: 1024"), 4);
  performance_.addItem(TRANS("Fewest voices: 512, for a Raspberry Pi or a tablet"), 5);
  performance_.addItem(TRANS("Custom"), 6);
  performance_.setTooltip(TRANS("Sets the audio engine's threads and the number of voices together, and applies "
                                "at once. Start with Balanced. If the sound crackles in a full chord, move down "
                                "the list until it plays clean. Steady helps when other programs run beside the "
                                "organ."));
  performance_.onChange = [this] {
    if (performance_.getSelectedId() <= 5) applyPerformance(performance_.getSelectedId());
  };
  syncPerformance();

  syncProfile();

  // Nothing on this panel writes to disk on its own. Changes are live the
  // moment they are made, and what happens to them afterwards is the
  // player's to say: forget them, keep them for this session, give them to
  // this organ, or make them where every organ starts.
  for (auto* b : {&revert_, &keep_, &saveOrgan_, &saveGlobal_})
    addAndMakeVisible(*b);
  revert_.onClick = [this] { revert(); };
  keep_.onClick = [this] { closeDialog(); };
  addChildComponent(reload_);
  reload_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff8a5a12));
  reload_.setTooltip(TRANS("These settings take effect when the organ is loaded; this loads it again now"));
  reload_.onClick = [this] {
    if (onReload) onReload();
  };
  saveOrgan_.onClick = [this] {
    proc_.saveSettings();
    showSaved(saveOrgan_);
  };
  saveGlobal_.onClick = [this] {
    proc_.saveGlobalDefaults();
    showSaved(saveGlobal_);
  };

  addAndMakeVisible(memory_);
  styleLabel(memory_, "");
  stream_.setTooltip(TRANS("Holds only the start of each release in memory and reads the rest from disk as it "
                           "plays. Needs a disk that keeps up: the readout below counts any underrun."));
  storage_.setTooltip(TRANS("What a held sample costs. 16-bit halves the memory of 24-bit; each sample is "
                            "scaled by its own peak first."));
  preload_.setTooltip(TRANS("\"Up to the loop\" keeps each attack to the end of its sustain loop, all a held "
                            "note plays."));
  profile_.setTooltip(TRANS("Sets the format, stereo or mono, compression and streaming together. Takes "
                            "effect on the next organ load."));
  applyPart();
  startTimerHz(2);
}

EnginePanel::~EnginePanel() {
  stopTimer();
  // Whatever the window leaves -- saved, kept, reverted or just closed -- is
  // what the next load of this organ uses (#90).
  proc_.keepLoadingChoiceForSession();
}

void EnginePanel::fillMemLimit() {
  memLimit_.clear(juce::dontSendNotification);
  const int ram = juce::SystemStats::getMemorySizeInMegabytes();
  auto gb = [](int mb) { return juce::String(mb / 1024.0, 1) + " GB"; };
  memLimit_.addItem(TRANS("Default: 80% of memory (%1)")
                        .replace("%1", gb(MasterpieceProcessor::defaultMemoryLimitMB())), 1);
  const int current = proc_.memoryLimitSettingMB();
  bool listed = current == 0;
  for (int pct : {50, 60, 70, 90}) {
    const int mb = ram * pct / 100;
    memLimit_.addItem(TRANS("%1% of memory (%2)").replace("%1", juce::String(pct)).replace("%2", gb(mb)),
                      1000 + mb);
    if (mb == current) listed = true;
  }
  // A size set in the file that is none of the above is still shown as itself.
  if (!listed) memLimit_.addItem(gb(current), 1000 + current);
  memLimit_.setSelectedId(current == 0 ? 1 : 1000 + current, juce::dontSendNotification);
}

void EnginePanel::fillPolyphony() {
  polyphony_.clear(juce::dontSendNotification);
  const auto mb = [this](int voices) {
    return juce::String(juce::jmax<int64_t>(1, proc_.voicePoolBytes(voices) / (1024 * 1024)));
  };
  const int def = proc_.defaultPolyphony();
  polyphony_.addItem(TRANS("Default: %1 voices (%2 MB)").replace("%1", juce::String(def)).replace("%2", mb(def)), 1);
  const int current = proc_.polyphonyLimit();
  bool listed = current == 0;
  for (int voices : {256, 512, 768, 1024, 2048, 3072, 4096, 6144, 8192, 10240}) {
    polyphony_.addItem(TRANS("%1 voices (%2 MB)").replace("%1", juce::String(voices)).replace("%2", mb(voices)),
                       1000 + voices);
    if (voices == current) listed = true;
  }
  // A number set in the file that is none of the above is still shown as itself.
  if (!listed)
    polyphony_.addItem(TRANS("%1 voices (%2 MB)").replace("%1", juce::String(current)).replace("%2", mb(current)),
                       1000 + current);
  polyphony_.setSelectedId(current == 0 ? 1 : 1000 + current, juce::dontSendNotification);
}

bool EnginePanel::cacheRebuildPending() const {
  // With an organ loaded, a cache in use, and a setting that changes what it
  // holds differing from what the organ was loaded with.
  return proc_.loadedOrganFile().getFullPathName().isNotEmpty() &&
         proc_.cacheMode() != SampleLibrary::CacheMode::Off && proc_.cacheChoicesChangedSinceLoad();
}

void EnginePanel::revert() {
  proc_.setEngineSwitch(openSwitch_);
  proc_.setPreloadHeadFrames(openPreload_);
  proc_.setSampleStorage(openStorage_);
  proc_.setStreamReleases(openStream_);
  proc_.setStreamHeadPercent(openStreamPercent_);
  proc_.setReleaseBufferSeconds(openReleaseBuffer_);
  proc_.setLoadMono(openMono_);
  mono_.setToggleState(openMono_, juce::dontSendNotification);
  proc_.setCodeSamples(openCompress_);
  compress_.setToggleState(openCompress_, juce::dontSendNotification);
  proc_.setCodeLevel(openLevel_);
  compressLevel_.setSelectedId(levelItem(openLevel_), juce::dontSendNotification);
  proc_.setProgramCouplers(openProgramCouplers_);
  programCouplers_.setToggleState(openProgramCouplers_, juce::dontSendNotification);
  proc_.setGrandOrgueTremulantPitch(openGoTremulantPitch_);
  goTremulantPitch_.setToggleState(openGoTremulantPitch_, juce::dontSendNotification);
  proc_.setGrandOrgueSwellFilter(openGoSwellFilter_);
  goSwellFilter_.setToggleState(openGoSwellFilter_, juce::dontSendNotification);
  proc_.setLoadTicks(openLoadTicks_);
  loadTicks_.setToggleState(openLoadTicks_, juce::dontSendNotification);
  proc_.setReopenLastOrgan(openReopen_);
  reopen_.setToggleState(openReopen_, juce::dontSendNotification);
  if (proc_.fasterEngine() != openFaster_) proc_.setFasterEngine(openFaster_);
  faster_.setToggleState(openFaster_, juce::dontSendNotification);
  showFasterUnit();
  if (proc_.realtimeThreads() != openRealtime_) proc_.setRealtimeThreads(openRealtime_);
  realtime_.setToggleState(openRealtime_, juce::dontSendNotification);
  showRealtimeUnit();
  proc_.setEveryProcessor(openEveryProcessor_);
  everyProcessor_.setToggleState(openEveryProcessor_, juce::dontSendNotification);
  proc_.setMemoryLimitMB(openMemLimit_);
  fillMemLimit();
  proc_.setPolyphonyLimit(openPolyphony_);
  fillPolyphony();
  proc_.setLoadSampleRate(openRate_);
  proc_.setReleaseChoice(openReleases_);
  releases_.setSelectedId(static_cast<int>(openReleases_) + 1, juce::dontSendNotification);
  proc_.setReleaseMaxMs(openReleaseMs_);
  showReleaseLength(openReleaseMs_);
  proc_.setSparseEvery(openSparse_);
  sparse_.setSelectedId(openSparse_, juce::dontSendNotification);
  proc_.setCacheMode(openCache_);
  cache_.setSelectedId(openCache_ == SampleLibrary::CacheMode::Off ? 3 : 2, juce::dontSendNotification);
  rate_.setSelectedId(openRate_ >= 47000.0 && openRate_ <= 49000.0   ? 2
                      : openRate_ >= 43000.0 && openRate_ <= 45000.0 ? 3
                                                                     : 1,
                      juce::dontSendNotification);
  syncProfile();

  simpleWav_.setToggleState(openSwitch_.simpleWavOnly, juce::dontSendNotification);
  wind_.setToggleState(openSwitch_.enableWindModel, juce::dontSendNotification);
  bellows_.setToggleState(openSwitch_.enableBellowsBoard, juce::dontSendNotification);
  tremulant_.setToggleState(openSwitch_.enableTremulant, juce::dontSendNotification);
  enclosure_.setToggleState(openSwitch_.enableEnclosure, juce::dontSendNotification);
  voicing_.setToggleState(openSwitch_.enableVoicing, juce::dontSendNotification);
  originalPitch_.setToggleState(openSwitch_.playAtOriginalOrganPitch,
                                juce::dontSendNotification);
  storage_.setSelectedId(storageItemId(openStorage_), juce::dontSendNotification);
  enableCompress();
  stream_.setToggleState(openStream_, juce::dontSendNotification);
  showStreamPercent(openStreamPercent_);
  showReleaseBuffer(openReleaseBuffer_);
  enableStreamControls(openStream_);
  showPreload(openPreload_);

  // simpleWavOnly greys the rest out; restoring the states has to restore
  // that too, or the panel lies about what is reachable.
  const bool detailed = !openSwitch_.simpleWavOnly;
  for (auto* b : {&wind_, &tremulant_, &enclosure_, &voicing_})
    b->setEnabled(detailed);
  bellows_.setEnabled(detailed && wind_.getToggleState());
  syncPerformance();
}

// Says what the faster engine found on this machine, so a report can say it.
void EnginePanel::showFasterUnit() {
  const juce::String base = TRANS("Faster audio engine");
  const juce::String unit = proc_.fasterEngineUnit();
  faster_.setButtonText(!proc_.fasterEngine() ? base
                        : unit.isNotEmpty()   ? TRANS("Faster audio engine: using %1").replace("%1", unit)
                                              : TRANS("Faster audio engine: not available on this "
                                                      "processor"));
}

// Says how real-time threads are had here, or that the system gives none, so a
// player on Linux without the rights sees it where the box is.
void EnginePanel::showRealtimeUnit() {
  const juce::String base = TRANS("Real-time render threads");
  const juce::String how = MasterpieceProcessor::realtimeMethod();
  realtime_.setButtonText(!proc_.realtimeThreads() ? base
                          : how.isNotEmpty()       ? TRANS("Real-time render threads: using %1").replace("%1", how)
                                                   : TRANS("Real-time render threads: no real-time rights "
                                                           "on this system"));
}

void EnginePanel::closeDialog() {
  if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
    dw->closeButtonPressed();
}

void EnginePanel::pushSwitches() {
  auto sw = proc_.engineSwitch();
  sw.simpleWavOnly = simpleWav_.getToggleState();
  sw.enableWindModel = wind_.getToggleState();
  sw.enableBellowsBoard = bellows_.getToggleState();
  sw.enableTremulant = tremulant_.getToggleState();
  sw.enableEnclosure = enclosure_.getToggleState();
  sw.enableVoicing = voicing_.getToggleState();
  sw.playAtOriginalOrganPitch = originalPitch_.getToggleState();
  proc_.setEngineSwitch(sw);

  // simpleWavOnly bypasses everything; showing the others as still-on would
  // be a lie about what the engine is doing.
  const bool detailed = !sw.simpleWavOnly;
  for (auto* b : {&wind_, &tremulant_, &enclosure_, &voicing_})
    b->setEnabled(detailed);
  bellows_.setEnabled(detailed && wind_.getToggleState());
}

void EnginePanel::showReleaseLength(int ms) {
  const auto seconds = [](int v) { return juce::String(v / 1000.0, v % 100 ? 2 : v % 1000 ? 1 : 0); };
  releaseLength_.clear(juce::dontSendNotification);
  releaseLength_.addItem(TRANS("Whole"), 1);
  for (int i = 1; i < kReleaseLengthCount; ++i)
    releaseLength_.addItem(TRANS("Up to %1 s").replace("%1", seconds(kReleaseLengthsMs[i])), i + 1);
  int id = 1;
  for (int i = 0; i < kReleaseLengthCount; ++i)
    if (kReleaseLengthsMs[i] == ms) id = i + 1;
  // A length set in the file and not in the list is shown as it is.
  if (ms > 0 && id == 1) {
    releaseLength_.addItem(TRANS("Up to %1 s").replace("%1", juce::String(ms / 1000.0, 2)), kReleaseLengthCount + 1);
    id = kReleaseLengthCount + 1;
  }
  releaseLength_.setSelectedId(id, juce::dontSendNotification);
}

bool EnginePanel::loadingChanged() const {
  const auto sw = proc_.engineSwitch();
  return proc_.sampleStorage() != openStorage_ || proc_.streamReleases() != openStream_ ||
         proc_.releaseChoice() != openReleases_ || proc_.releaseMaxMs() != openReleaseMs_ ||
         proc_.sparseEvery() != openSparse_ ||
         proc_.streamHeadPercent() != openStreamPercent_ ||
         proc_.releaseBufferSeconds() != openReleaseBuffer_ ||
         proc_.loadMono() != openMono_ || proc_.loadSampleRate() != openRate_ ||
         proc_.codeSamples() != openCompress_ || proc_.codeLevel() != openLevel_ ||
         proc_.programCouplers() != openProgramCouplers_ ||
         proc_.grandOrgueTremulantPitch() != openGoTremulantPitch_ ||
         proc_.grandOrgueSwellFilter() != openGoSwellFilter_ ||
         proc_.preloadHeadFrames() != openPreload_ ||
         proc_.polyphonyLimit() != openPolyphony_ ||
         sw.simpleWavOnly != openSwitch_.simpleWavOnly ||
         sw.enableWindModel != openSwitch_.enableWindModel ||
         sw.enableBellowsBoard != openSwitch_.enableBellowsBoard ||
         sw.enableTremulant != openSwitch_.enableTremulant ||
         sw.enableEnclosure != openSwitch_.enableEnclosure ||
         sw.enableVoicing != openSwitch_.enableVoicing ||
         sw.playAtOriginalOrganPitch != openSwitch_.playAtOriginalOrganPitch;
}

void EnginePanel::showSaved(juce::TextButton& button) {
  saveOrgan_.setButtonText(TRANS("Save for this organ"));
  saveGlobal_.setButtonText(TRANS("Save as default"));
  button.setButtonText(TRANS("Saved"));
  savedUntilMs_ = juce::Time::getMillisecondCounter() + 2000;
}

void EnginePanel::timerCallback() {
  if (savedUntilMs_ != 0 && juce::Time::getMillisecondCounter() >= savedUntilMs_) {
    savedUntilMs_ = 0;
    saveOrgan_.setButtonText(TRANS("Save for this organ"));
    saveGlobal_.setButtonText(TRANS("Save as default"));
  }
  // Always there while an organ is loaded (#134), and marked when a change
  // here is waiting for it.
  if (shownSimple_ != proc_.simpleMode()) {
    applyPart();
    resized();
  }
  // The scrolling tab around it follows the rows it shows.
  if (const int h = contentHeight(); h != fittedHeight_) {
    fittedHeight_ = h;
    if (auto* host = findParentComponentOfClass<ScrollHost>()) host->fit();
  }
  const bool offer = part_ == Part::Organ && onReload != nullptr &&
                     proc_.loadedOrganFile().getFullPathName().isNotEmpty();
  if (reload_.isVisible() != offer) reload_.setVisible(offer);
  const bool pending = offer && (loadingChanged() || (alsoPending && alsoPending()));
  const bool rebuild = part_ == Part::Organ && !proc_.simpleMode() && cacheRebuildPending();
  if (rebuildNote_.isVisible() != rebuild) rebuildNote_.setVisible(rebuild);
  const juce::String reloadText = pending ? TRANS("Load the organ to apply") : TRANS("Load the organ");
  if (reload_.getButtonText() != reloadText) {
    reload_.setButtonText(reloadText);
    reload_.setColour(juce::TextButton::buttonColourId,
                      pending ? juce::Colour(0xff8a5a12) : juce::Colour(0xff2a2f38));
  }

  const auto bytes = proc_.sampleLibrary().residentBytes();
  const auto fmt = proc_.sampleStorage();
  const juce::String format = fmt == SampleStorage::Int16     ? juce::String("16-bit")
                              : fmt == SampleStorage::Int20   ? juce::String("20-bit")
                              : fmt == SampleStorage::Float32 ? TRANS("32-bit float")
                                                              : TRANS("as recorded");
  juce::String text = TRANS("Resident samples: %1 MB (%2 samples, %3, %4%5)")
                          .replace("%1", juce::String(bytes / (1024 * 1024)))
                          .replace("%2", juce::String(proc_.sampleLibrary().residentCount()))
                          .replace("%3", format)
                          .replace("%4", proc_.loadMono() ? TRANS("mono") : TRANS("stereo"))
                          .replace("%5", proc_.loadSampleRate() > 0.0
                                             ? ", " + juce::String(proc_.loadSampleRate() / 1000.0, 1) + " kHz"
                                             : juce::String());
  if (const auto coded = proc_.sampleLibrary().codedCount(); coded > 0)
    text += "  -  " + TRANS("%1 held compressed").replace("%1", juce::String(static_cast<int>(coded)));
  const auto streamed = proc_.sampleLibrary().streamedCount();
  if (streamed > 0)
    text += "  -  " + TRANS("%1 streamed, %2 MB left on disk")
                              .replace("%1", juce::String(static_cast<int>(streamed)))
                              .replace("%2", juce::String(proc_.sampleLibrary().streamedBytesSaved() /
                                                          (1024 * 1024)));
  if (const auto under = proc_.streamUnderruns(); under > 0)
    text += "  -  " + TRANS("DISK TOO SLOW: %1 underrun(s)").replace("%1", juce::String(static_cast<int>(under)));
  text += "  -  " + TRANS("Voice pool: %1 voices, %2 MB")
                        .replace("%1", juce::String(proc_.voicePool()))
                        .replace("%2", juce::String(proc_.voicePoolBytes(proc_.voicePool()) / (1024 * 1024)));
  memory_.setText(text, juce::dontSendNotification);
}

void EnginePanel::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff1b1e24)); }

// The memory settings as combinations worth having, rather than as four
// independent questions. Measured on a 44-stop, 17 GB set these span 21.6 GB
// down to 2.3 GB; the individual controls below can still reach anything.
namespace {
// The quality presets, in the order the box lists them.
struct QualityPreset {
  SampleStorage storage;
  bool mono, stream, compress;
  SampleLibrary::ReleaseChoice releases;
};
constexpr QualityPreset kQuality[] = {
    {SampleStorage::Int24, false, false, false, SampleLibrary::ReleaseChoice::All},
    {SampleStorage::Int24, false, true, true, SampleLibrary::ReleaseChoice::All},
    {SampleStorage::Int16, false, true, false, SampleLibrary::ReleaseChoice::All},
    {SampleStorage::Int16, false, true, true, SampleLibrary::ReleaseChoice::All},
    {SampleStorage::Int16, true, true, true, SampleLibrary::ReleaseChoice::All},
    {SampleStorage::Int16, true, true, true, SampleLibrary::ReleaseChoice::Shortest},
};

// The performance presets: from the most the computer can give to what a
// small one keeps up with. Polyphony 0 is the computer's own default.
struct PerformancePreset {
  bool faster, realtime, everyProcessor;
  int polyphony;
};
constexpr PerformancePreset kPerformance[] = {
    {true, true, true, 8192},
    {true, true, true, 0},
    {true, true, false, 0},
    {true, true, false, 1024},
    {true, true, false, 512},
};
}  // namespace

void EnginePanel::applyProfile(int id) {
  if (id < 1 || id > static_cast<int>(std::size(kQuality))) return;
  applyingProfile_ = true;
  const auto& p = kQuality[id - 1];
  proc_.setSampleStorage(p.storage);
  proc_.setLoadMono(p.mono);
  proc_.setStreamReleases(p.stream);
  proc_.setCodeSamples(p.compress);
  proc_.setReleaseChoice(p.releases);
  proc_.setPreloadHeadFrames(0);

  storage_.setSelectedId(storageItemId(p.storage), juce::dontSendNotification);
  enableCompress();
  mono_.setToggleState(p.mono, juce::dontSendNotification);
  stream_.setToggleState(p.stream, juce::dontSendNotification);
  compress_.setToggleState(p.compress, juce::dontSendNotification);
  releases_.setSelectedId(static_cast<int>(p.releases) + 1, juce::dontSendNotification);
  enableStreamControls(p.stream);
  preload_.setSelectedId(1, juce::dontSendNotification);
  applyingProfile_ = false;
}

void EnginePanel::applyPerformance(int id) {
  if (id < 1 || id > static_cast<int>(std::size(kPerformance))) return;
  const auto& p = kPerformance[id - 1];
  proc_.setFasterEngine(p.faster);
  faster_.setToggleState(p.faster, juce::dontSendNotification);
  showFasterUnit();
  if (MasterpieceProcessor::realtimeThreadsSupported()) {
    proc_.setRealtimeThreads(p.realtime);
    realtime_.setToggleState(p.realtime, juce::dontSendNotification);
    showRealtimeUnit();
  }
  proc_.setEveryProcessor(p.everyProcessor);
  everyProcessor_.setToggleState(p.everyProcessor, juce::dontSendNotification);
  proc_.setPolyphonyLimit(p.polyphony);
  fillPolyphony();
}

void EnginePanel::syncPerformance() {
  int id = static_cast<int>(std::size(kPerformance)) + 1;  // Custom
  for (size_t i = 0; i < std::size(kPerformance); ++i) {
    const auto& p = kPerformance[i];
    const bool realtime = !MasterpieceProcessor::realtimeThreadsSupported() || proc_.realtimeThreads() == p.realtime;
    if (proc_.fasterEngine() == p.faster && realtime && proc_.everyProcessor() == p.everyProcessor &&
        proc_.polyphonyLimit() == p.polyphony) {
      id = static_cast<int>(i) + 1;
      break;
    }
  }
  performance_.setSelectedId(id, juce::dontSendNotification);
}

// Compression codes 16- and 24-bit samples; 20-bit and 32-bit float are held as
// they are. The box keeps its tick, so going back to 16 or 24 bits finds the
// player's choice where it was.
void EnginePanel::enableCompress() {
  const auto st = proc_.sampleStorage();
  const bool coded = st == SampleStorage::Int16 || st == SampleStorage::Int24;
  compress_.setEnabled(coded);
  compressLevel_.setEnabled(coded && proc_.codeSamples());
  // High and Max pack 16-bit samples; a 24-bit one would come out medium.
  const bool narrow = st == SampleStorage::Int16;
  compressLevel_.setItemEnabled(levelItem(SplitRiceLevel::High), narrow);
  compressLevel_.setItemEnabled(levelItem(SplitRiceLevel::Max), narrow);
}

void EnginePanel::syncProfile() {
  int id = static_cast<int>(std::size(kQuality)) + 1;  // Custom, until the settings match a preset
  if (proc_.preloadHeadFrames() == 0)
    for (size_t i = 0; i < std::size(kQuality); ++i) {
      const auto& p = kQuality[i];
      if (proc_.sampleStorage() == p.storage && proc_.loadMono() == p.mono &&
          proc_.streamReleases() == p.stream && proc_.codeSamples() == p.compress &&
          proc_.releaseChoice() == p.releases) {
        id = static_cast<int>(i) + 1;
        break;
      }
    }
  applyingProfile_ = true;
  profile_.setSelectedId(id, juce::dontSendNotification);
  applyingProfile_ = false;
}

// The folder as the player sees it: the chosen one, or the default place
// named as such, since an empty field would look like a missing setting.
void EnginePanel::showCacheDir() {
  const auto chosen = proc_.cacheDirectorySetting();
  cacheDirValue_.setText(chosen.getFullPathName().isEmpty()
                             ? TRANS("Default (%1)").replace("%1", proc_.cacheDirectory().getFullPathName())
                             : chosen.getFullPathName(),
                         juce::dontSendNotification);
  cacheDirValue_.setTooltip(proc_.cacheDirectory().getFullPathName());
  juce::int64 bytes = 0;
  for (const auto& f : sampleCacheFiles(proc_.cacheDirectory())) bytes += f.getSize();
  cacheDelete_.setEnabled(bytes > 0 && !proc_.cacheWriting());
  cacheDelete_.setTooltip(
      bytes > 0 ? TRANS("The sample cache in this folder: %1. Each organ reads its samples again on its next load.")
                      .replace("%1", juce::File::descriptionOfSizeInBytes(bytes))
                : TRANS("No sample cache in this folder"));
}

void EnginePanel::showOrganCache() {
  const auto file = proc_.organCacheFile();
  juce::int64 bytes = 0;
  if (file != juce::File())
    bytes = file.getSize() + file.getSiblingFile(file.getFileName() + ".part").getSize();
  organCacheDelete_.setEnabled(bytes > 0 && !proc_.cacheWriting());
  organCacheDelete_.setTooltip(
      bytes > 0 ? TRANS("This organ's sample cache: %1. The organ reads its samples again on its next load.")
                      .replace("%1", juce::File::descriptionOfSizeInBytes(bytes))
                : TRANS("No sample cache for this organ"));
}

void EnginePanel::showPackageDir() {
  const auto dir = proc_.packageDirectory();
  packageDirValue_.setText(dir.getFullPathName().isEmpty() ? TRANS("None") : dir.getFullPathName(),
                           juce::dontSendNotification);
  packageDirValue_.setTooltip(dir.getFullPathName());
  packageDirClear_.setEnabled(dir.getFullPathName().isNotEmpty());
}

// The folder as the player sees it: the one they named, or the one derived
// from the definition's path, marked as derived so the two cannot be
// confused.
void EnginePanel::showOrganRoot() {
  const auto chosen = proc_.organRootOverride();
  organRootValue_.setText(chosen.getFullPathName().isEmpty()
                              ? TRANS("From the definition (%1)").replace("%1", juce::String(proc_.organRootDir()))
                              : TRANS("%1  -- loads next time").replace("%1", chosen.getFullPathName()),
                          juce::dontSendNotification);
}

void EnginePanel::showPreload(int64_t frames) {
  const int64_t rate = 48000;
  const int id = frames == 0 ? 1 : frames == 1 ? 4 : frames >= 2 * rate ? 2 : 3;
  if ((id == 2 || id == 3) && preload_.indexOfItemId(id) < 0)
    preload_.addItem(id == 2 ? TRANS("Loop + 2 s (earlier choice)") : TRANS("Loop + 1 s (earlier choice)"), id);
  preload_.setSelectedId(id, juce::dontSendNotification);
}

void EnginePanel::showStreamPercent(int percent) {
  streamHead_.setSelectedId(percent >= 75 ? 4 : percent >= 50 ? 3 : percent >= 25 ? 2 : 1,
                             juce::dontSendNotification);
}

void EnginePanel::showReleaseBuffer(double seconds) {
  releaseBuffer_.clear(juce::dontSendNotification);
  // 0.25, 0.5, 1, 2: as few digits as the length needs.
  const auto secs = [](double s) {
    auto t = juce::String(s, 2);
    while (t.endsWithChar('0')) t = t.dropLastCharacters(1);
    if (t.endsWithChar('.')) t = t.dropLastCharacters(1);
    return TRANS("%1 s").replace("%1", t);
  };
  // Automatic names the length the loaded organ's disk was given.
  releaseBuffer_.addItem(proc_.loadedOrganFile().getFullPathName().isNotEmpty() &&
                                 proc_.releaseBufferSeconds() <= 0.0
                             ? TRANS("Automatic (%1)").replace("%1", secs(proc_.streamSeconds()))
                             : TRANS("Automatic"),
                         1);
  static const double kSeconds[] = {0.25, 0.5, 1.0, 2.0};
  int id = 1;
  for (int i = 0; i < 4; ++i) {
    releaseBuffer_.addItem(secs(kSeconds[i]), i + 2);
    if (seconds > 0.0 && std::abs(seconds - kSeconds[i]) < 0.001) id = i + 2;
  }
  // A length set in the file and not in the list is shown as it is.
  if (seconds > 0.0 && id == 1) {
    releaseBuffer_.addItem(secs(seconds), 6);
    id = 6;
  }
  releaseBuffer_.setSelectedId(id, juce::dontSendNotification);
}

void EnginePanel::enableStreamControls(bool on) {
  streamHead_.setEnabled(on);
  releaseBuffer_.setEnabled(on);
  releaseBufferLabel_.setEnabled(on);
}

void EnginePanel::layoutFooter(juce::Rectangle<int> footer) {
  // Widths in proportion to the labels as they first read.
  const std::pair<juce::TextButton*, int> buttons[] = {
      {&revert_, 130}, {&keep_, 120}, {&reload_, 230}, {&saveOrgan_, 160}, {&saveGlobal_, 150}};
  int wanted = 0;
  for (const auto& b : buttons) wanted += b.second;
  const int count = static_cast<int>(std::size(buttons));
  const int room = footer.getWidth() - kGap * (count - 1);
  int x = footer.getX();
  for (int i = 0; i < count; ++i) {
    // The last takes what rounding left, so the row ends flush.
    const int w = i == count - 1 ? footer.getRight() - x : room * buttons[i].second / wanted;
    buttons[i].first->setBounds(juce::Rectangle<int>(x, footer.getY(), w, footer.getHeight()).reduced(0, 2));
    x += w + kGap;
  }
}

void EnginePanel::applyPart() {
  shownSimple_ = proc_.simpleMode();
  const bool simple = shownSimple_;
  using List = std::initializer_list<juce::Component*>;
  const List organ{&simpleWav_, &wind_, &bellows_, &tremulant_, &enclosure_, &voicing_, &originalPitch_,
                   &profileLabel_, &profile_, &preloadLabel_, &preload_, &storageLabel_, &storage_,
                   &rateLabel_, &rate_, &sparseLabel_, &sparse_, &releasesLabel_, &releases_,
                   &releaseLengthLabel_, &releaseLength_, &releaseShortLabel_, &releaseShort_, &releaseCutoffLabel_, &releaseCutoff_,
                   &stream_, &streamHead_, &releaseBufferLabel_, &releaseBuffer_, &mono_, &compress_,
                   &compressLevel_, &programCouplers_, &goTremulantPitch_, &goSwellFilter_, &portable_, &organCacheLabel_, &organCache_, &organCacheDelete_, &organRootLabel_, &organRootValue_, &organRootChoose_,
                   &organRootDefault_, &memory_, &revert_, &keep_, &saveOrgan_, &saveGlobal_};
  const List organSimple{&profileLabel_, &profile_, &memory_, &revert_, &keep_, &saveOrgan_, &saveGlobal_};
  const List performance{&performanceLabel_, &performance_, &faster_, &realtime_, &everyProcessor_,
                         &memLimitLabel_, &memLimit_, &polyphonyLabel_, &polyphony_};
  const List performanceSimple{&performanceLabel_, &performance_};
  const List general{&displayModeLabel_, &displayMode_, &reopen_, &loadTicks_, &cacheDirLabel_,
                     &cacheDirValue_, &cacheDirChoose_, &cacheDirDefault_, &cacheDelete_, &packageDirLabel_,
                     &packageDirValue_, &packageDirChoose_, &packageDirClear_, &cacheLabel_, &cache_,
                     &cacheWhenLabel_, &cacheWhen_};
  // The cache's choices too: the first load of a large organ waits on it (#565).
  const List generalSimple{&displayModeLabel_, &displayMode_, &reopen_, &cacheLabel_, &cache_,
                           &cacheWhenLabel_, &cacheWhen_};
  displayMode_.setSelectedId(simple ? 1 : 2, juce::dontSendNotification);
  const List& mine = part_ == Part::Organ ? organ : part_ == Part::Performance ? performance : general;
  const List& few = part_ == Part::Organ ? organSimple
                    : part_ == Part::Performance ? performanceSimple
                                                 : generalSimple;
  const auto in = [](const List& l, juce::Component* c) { return std::find(l.begin(), l.end(), c) != l.end(); };
  for (auto* c : getChildren()) {
    // The timer shows these two when they apply.
    if (c == &reload_ || c == &rebuildNote_) continue;
    bool show = in(mine, c) && (!simple || in(few, c));
    if (c == &realtime_) show = show && MasterpieceProcessor::realtimeThreadsSupported();
    if (c == &goTremulantPitch_ || c == &goSwellFilter_) show = show && proc_.grandOrgueSet();
    c->setVisible(show);
  }
  // Simple mode hides the controls that Custom stands for, so there Custom
  // names settings that match no preset and cannot be chosen.
  profile_.setItemEnabled(7, !simple);
  performance_.setItemEnabled(6, !simple);
}

int EnginePanel::contentHeight() const {
  // The lowest row's bottom, as resized() last placed them, and the footer.
  int bottom = 0;
  for (auto* c : getChildren())
    if (c->isVisible() && c != &reload_ && c != &revert_ && c != &keep_ && c != &saveOrgan_ &&
        c != &saveGlobal_)
      bottom = juce::jmax(bottom, c->getBottom());
  return bottom + 12 + (part_ == Part::Organ ? kRow + 4 + kGap : 0);
}

void EnginePanel::resized() {
  auto r = getLocalBounds().reduced(12);
  // The buttons first, at their full height, whatever the rows above need
  // (#149). Read left to right they go from discarding to committing hardest.
  // "Load the organ" is laid out even while it is hidden, so the others do
  // not jump when it appears.
  if (part_ == Part::Organ) {
    layoutFooter(r.removeFromBottom(kRow + 4));
    r.removeFromBottom(kGap);
  }
  // Every row at its full height however little room is left, so that
  // contentHeight() can say what the rows need and the tab around them scroll
  // to all of them. Cut to the room, the last rows came out with no height
  // and were measured that way (the Loading tab on a Mac, D565).
  r.setHeight(1 << 20);
  // A setting a preset above it sets is indented under it (#634); its box
  // stays in line with the others.
  constexpr int kIndent = 20;
  const auto line = [&r](juce::Component& c, int indent = 0) {
    if (!c.isVisible()) return;
    c.setBounds(r.removeFromTop(kRow).withTrimmedLeft(indent));
    r.removeFromTop(2);
  };
  const auto pair = [&r](juce::Component& label, juce::Component& box, int w, int indent = 0) {
    if (!box.isVisible()) return;
    auto row = r.removeFromTop(kRow);
    label.setBounds(row.removeFromLeft(180).withTrimmedLeft(indent));
    box.setBounds(row.removeFromLeft(w));
    r.removeFromTop(6);
  };
  const auto folder = [&r](juce::Label& label, juce::Label& value,
                           std::initializer_list<std::pair<juce::Component*, int>> buttons) {
    if (!value.isVisible()) return;
    auto row = r.removeFromTop(kRow);
    label.setBounds(row.removeFromLeft(180));
    for (const auto& [b, w] : buttons) {
      b->setBounds(row.removeFromRight(w).reduced(0, 2));
      row.removeFromRight(6);
    }
    value.setBounds(row);
    r.removeFromTop(6);
  };

  // This organ: what it plays through, then how it is loaded.
  {
    std::vector<juce::Component*> boxes;
    // A GrandOrgue set's two (#833) make a row of their own, each under the
    // effect it adds to.
    for (auto* b : std::initializer_list<juce::Component*>{&simpleWav_, &wind_, &bellows_, &tremulant_, &enclosure_, &voicing_,
                                                           &originalPitch_, &goTremulantPitch_, &goSwellFilter_})
      if (b->isVisible()) boxes.push_back(b);
    // Two columns where the longest label fits in half the width.
    const size_t columns = r.getWidth() >= 600 ? 2 : 1;
    for (size_t i = 0; i < boxes.size(); i += columns) {
      auto row = r.removeFromTop(kRow);
      const int w = row.getWidth() / static_cast<int>(columns);
      for (size_t c = 0; c < columns && i + c < boxes.size(); ++c) boxes[i + c]->setBounds(row.removeFromLeft(w));
      r.removeFromTop(2);
    }
  }
  if (simpleWav_.isVisible()) r.removeFromTop(kGap);
  pair(profileLabel_, profile_, 380);
  if (profile_.isVisible()) r.removeFromTop(4);
  // What the quality preset sets, under it.
  pair(preloadLabel_, preload_, 260, kIndent);
  pair(storageLabel_, storage_, 260, kIndent);
  pair(releasesLabel_, releases_, 300, kIndent);
  if (stream_.isVisible()) {
    auto streamRow = r.removeFromTop(kRow).withTrimmedLeft(kIndent);
    streamHead_.setBounds(streamRow.removeFromRight(230).reduced(0, 2));
    stream_.setBounds(streamRow);
    r.removeFromTop(2);
  }
  line(mono_, kIndent);
  if (compress_.isVisible()) {
    auto compressRow = r.removeFromTop(kRow).withTrimmedLeft(kIndent);
    compressLevel_.setBounds(compressRow.removeFromRight(230).reduced(0, 2));
    compress_.setBounds(compressRow);
    r.removeFromTop(2);
  }
  if (profile_.isVisible()) r.removeFromTop(kGap);
  pair(rateLabel_, rate_, 260);
  pair(sparseLabel_, sparse_, 300);
  pair(releaseLengthLabel_, releaseLength_, 300);
  pair(releaseCutoffLabel_, releaseCutoff_, 300);
  pair(releaseShortLabel_, releaseShort_, 300);
  pair(releaseBufferLabel_, releaseBuffer_, 300);
  line(programCouplers_);
  line(portable_);
  if (organCache_.isVisible()) {
    auto row = r.removeFromTop(kRow);
    organCacheLabel_.setBounds(row.removeFromLeft(180));
    organCache_.setBounds(row.removeFromLeft(200));
    row.removeFromLeft(8);
    organCacheDelete_.setBounds(row.removeFromLeft(200));
    r.removeFromTop(6);
  }
  folder(organRootLabel_, organRootValue_, {{&organRootChoose_, 90}, {&organRootDefault_, 80}});
  // The cache note's row is kept while the organ's rows show, so nothing
  // jumps when it appears.
  if (part_ == Part::Organ && !proc_.simpleMode()) rebuildNote_.setBounds(r.removeFromTop(kRow));
  if (memory_.isVisible()) {
    r.removeFromTop(kGap);
    memory_.setBounds(r.removeFromTop(kRow * 2));
  }

  // Performance: the engine, for every organ.
  pair(performanceLabel_, performance_, 380);
  if (performance_.isVisible()) r.removeFromTop(4);
  // What the performance preset sets, under it.
  line(faster_, kIndent);
  line(realtime_, kIndent);
  line(everyProcessor_, kIndent);
  if (faster_.isVisible()) r.removeFromTop(4);
  pair(polyphonyLabel_, polyphony_, 300, kIndent);
  if (performance_.isVisible()) r.removeFromTop(kGap);
  pair(memLimitLabel_, memLimit_, 300);

  // General: the display mode, start, folders and the cache, for every organ.
  pair(displayModeLabel_, displayMode_, 300);
  if (displayMode_.isVisible()) r.removeFromTop(kGap);
  line(reopen_);
  line(loadTicks_);
  if (reopen_.isVisible()) r.removeFromTop(4);
  folder(cacheDirLabel_, cacheDirValue_, {{&cacheDirChoose_, 90}, {&cacheDirDefault_, 80}, {&cacheDelete_, 130}});
  folder(packageDirLabel_, packageDirValue_, {{&packageDirChoose_, 90}, {&packageDirClear_, 80}});
  pair(cacheLabel_, cache_, 300);
  pair(cacheWhenLabel_, cacheWhen_, 300);
}

// ---------------------------------------------------------------- reverb

ReverbPanel::ReverbPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(enabled_);
  enabled_.setToggleState(proc_.convolver().enabled(), juce::dontSendNotification);
  enabled_.onClick = [this] {
    proc_.convolver().setEnabled(enabled_.getToggleState());
    proc_.markSettingsDirty();
  };

  addAndMakeVisible(load_);
  load_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Choose an impulse response"), proc_.dialogFolder("ir"), "*.wav;*.aiff;*.aif;*.flac");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                            if (fc.getURLResults().isEmpty()) return;  // cancelled
                            const auto f = pickedFile(fc);
                            proc_.rememberDialogFolder("ir", f);
                            juce::String why;
                            if (!f.existsAsFile())
                              why = TRANS("The file could not be read where it is.");
                            else if (!proc_.convolver().loadImpulseResponse(f))
                              why = TRANS("%1 is not an impulse response Masterpiece can read (WAV, AIFF or FLAC).")
                                        .replace("%1", f.getFileName());
                            if (why.isNotEmpty()) {
                              juce::Logger::writeToLog("impulse response not loaded: " +
                                                       f.getFullPathName() + ": " + why);
                              juce::AlertWindow::showMessageBoxAsync(
                                  juce::MessageBoxIconType::WarningIcon,
                                  TRANS("The impulse response was not loaded"), why);
                              return;
                            }
                            irName_.setText(TRANS("IR: %1").replace("%1", f.getFileName()), juce::dontSendNotification);
                            enabled_.setToggleState(true, juce::sendNotification);
                            proc_.markSettingsDirty();
                          });
  };

  addAndMakeVisible(clear_);
  clear_.onClick = [this] {
    proc_.convolver().clear();
    irName_.setText(TRANS("No IR loaded"), juce::dontSendNotification);
    proc_.markSettingsDirty();
  };

  addAndMakeVisible(irName_);
  styleLabel(irName_, proc_.convolver().hasImpulseResponse()
                          ? TRANS("IR: %1").replace("%1", proc_.convolver().impulseResponseName())
                          : TRANS("No IR loaded"));

  addAndMakeVisible(mixLabel_);
  styleLabel(mixLabel_, TRANS("Wet"));
  addAndMakeVisible(mix_);
  mix_.setRange(0.0, 1.0, 0.01);
  styleFader(mix_, fader::evenTicks(0.0, 1.0), fader::kNoUnity);
  mix_.setValue(proc_.convolver().mix(), juce::dontSendNotification);
  mix_.onValueChange = [this] {
    proc_.convolver().setMix(static_cast<float>(mix_.getValue()));
    proc_.markSettingsDirty();
  };

  addAndMakeVisible(midi_);
  midi_.setTooltip(TRANS("Learn a knob or a pedal for the wet level"));
  midi_.onClick = [this] { midiMenu(); };
  startTimerHz(10);
}

ReverbPanel::~ReverbPanel() { stopTimer(); }

void ReverbPanel::midiMenu() {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    proc_.cancelLearn();
    return;
  }
  juce::PopupMenu menu;
  const auto existing = map.bindingsFor(MidiTargetKind::ReverbWet, 0);
  if (existing.empty()) menu.addSectionHeader(TRANS("Not mapped"));
  for (const MidiBinding* b : existing) menu.addSectionHeader(MidiEventDialog::describeBinding(*b));
  menu.addItem(1, TRANS("Learn: move the knob or pedal"));
  menu.addItem(2, TRANS("Clear mapping"), !existing.empty());
  juce::Component::SafePointer<ReverbPanel> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&midi_), [self](int choice) {
    if (self == nullptr) return;
    if (choice == 1) self->proc_.beginLearn(MidiTargetKind::ReverbWet, 0, false);
    if (choice == 2) {
      self->proc_.editMidiMap([](MidiMap& map) { map.unbindTarget(MidiTargetKind::ReverbWet, 0); });
      self->proc_.saveMidiMap();
    }
  });
}

void ReverbPanel::timerCallback() {
  if (!mix_.isMouseButtonDown()) {
    const double now = proc_.convolver().mix();
    if (std::abs(mix_.getValue() - now) > 0.004) mix_.setValue(now, juce::dontSendNotification);
  }
  const auto& map = proc_.midiMap();
  const bool learning = map.learning() && map.learningKind() == MidiTargetKind::ReverbWet;
  midi_.setButtonText(learning ? TRANS("Move it...") : TRANS("MIDI"));
}

void ReverbPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  enabled_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  auto row = r.removeFromTop(kRow);
  // As wide as its words, in any language.
  load_.setBounds(row.removeFromLeft(juce::jmax(110, load_.getBestWidthForHeight(kRow))));
  row.removeFromLeft(kGap);
  clear_.setBounds(row.removeFromLeft(80));
  row.removeFromLeft(kGap);
  irName_.setBounds(row);
  r.removeFromTop(kGap);
  row = r.removeFromTop(kRow);
  mixLabel_.setBounds(row.removeFromLeft(60));
  mix_.setBounds(row.removeFromLeft(320));
  row.removeFromLeft(kGap);
  midi_.setBounds(row.removeFromLeft(70));
  r.removeFromTop(kGap);
}

// ------------------------------------------------------------- metronome

MetronomePanel::MetronomePanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(enabled_);
  enabled_.setToggleState(proc_.metronome().enabled(), juce::dontSendNotification);
  enabled_.onClick = [this] {
    proc_.metronome().setEnabled(enabled_.getToggleState());
  };

  addAndMakeVisible(tempoLabel_);
  styleLabel(tempoLabel_, TRANS("Tempo"));
  addAndMakeVisible(tempo_);
  tempo_.setRange(20.0, 300.0, 1.0);
  tempo_.setValue(proc_.metronome().tempo(), juce::dontSendNotification);
  tempo_.setTextValueSuffix(" bpm");
  tempo_.onValueChange = [this] { proc_.metronome().setTempo(tempo_.getValue()); };

  addAndMakeVisible(beatsLabel_);
  styleLabel(beatsLabel_, TRANS("Beats per bar"));
  addAndMakeVisible(beats_);
  beats_.setRange(0.0, 12.0, 1.0);
  beats_.setValue(proc_.metronome().beatsPerBar(), juce::dontSendNotification);
  beats_.onValueChange = [this] {
    proc_.metronome().setBeatsPerBar(static_cast<int>(beats_.getValue()));
  };

  addAndMakeVisible(levelLabel_);
  styleLabel(levelLabel_, TRANS("Level"));
  addAndMakeVisible(level_);
  level_.setRange(0.0, 1.0, 0.01);
  styleFader(level_, fader::evenTicks(0.0, 1.0), fader::kNoUnity);
  level_.setValue(proc_.metronome().level(), juce::dontSendNotification);
  level_.onValueChange = [this] {
    proc_.metronome().setLevel(static_cast<float>(level_.getValue()));
  };

  addAndMakeVisible(beat_);
  styleLabel(beat_, "");
  startTimerHz(12);
}

MetronomePanel::~MetronomePanel() {
  stopTimer();
  proc_.rememberMetronome();
}

void MetronomePanel::timerCallback() {
  // A tab of the settings window that is not the one on show.
  if (!isShowing()) return;
  const int b = proc_.metronome().currentBeat();
  beat_.setText(b > 0 ? TRANS("Beat %1").replace("%1", juce::String(b)) : juce::String(), juce::dontSendNotification);
}

void MetronomePanel::resized() {
  auto r = getLocalBounds().reduced(12);
  enabled_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  auto row = [&r]() { auto x = r.removeFromTop(kRow); r.removeFromTop(4); return x; };
  auto a = row();
  tempoLabel_.setBounds(a.removeFromLeft(120));
  tempo_.setBounds(a.removeFromLeft(320));
  auto b = row();
  beatsLabel_.setBounds(b.removeFromLeft(120));
  beats_.setBounds(b.removeFromLeft(320));
  auto c = row();
  levelLabel_.setBounds(c.removeFromLeft(120));
  level_.setBounds(c.removeFromLeft(320));
  beat_.setBounds(row());
}

// -------------------------------------------------------------- recorder

RecorderPanel::RecorderPanel(MasterpieceProcessor& p) : proc_(p) {
  for (auto* b : {&record_, &play_, &stop_, &save_, &load_, &clear_, &midi_})
    addAndMakeVisible(*b);
  midi_.setTooltip(TRANS("A button on the console for Record and one for Play"));
  midi_.onClick = [this] { showMidiMenu(); };
  addAndMakeVisible(saveForSequencer_);
  addAndMakeVisible(markers_);
  markers_.setToggleState(true, juce::dontSendNotification);
  saveForSequencer_.setTooltip(TRANS("A file for other programs: a track per keyboard, the notes in pairs, the console's "
                                     "own buttons left out. Save MIDI keeps what Masterpiece plays back."));
  saveForSequencer_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Save for a sequencer"), proc_.dialogFolder("midi"), "*.mid");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                            const auto f = fc.getResult();
                            proc_.rememberDialogFolder("midi", f);
                            if (f.getFullPathName().isNotEmpty() &&
                                !proc_.saveTakeForSequencer(f.withFileExtension(".mid"), markers_.getToggleState()))
                              status_.setText(TRANS("Could not write that file"), juce::dontSendNotification);
                          });
  };

  record_.onClick = [this] { askBeforeRecording(); };
  play_.onClick = [this] { proc_.recorder().startPlayback(); };
  stop_.onClick = [this] {
    proc_.recorder().stopRecording();
    proc_.recorder().stopPlayback();
  };
  clear_.onClick = [this] { proc_.recorder().clear(); };

  save_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Save the performance"), proc_.dialogFolder("midi"), "*.mid");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode |
                              juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                            const auto f = fc.getResult();
                            proc_.rememberDialogFolder("midi", f);
                            if (f.getFullPathName().isNotEmpty())
                              proc_.saveTake(f.withFileExtension(".mid"));
                          });
  };
  load_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Load a performance"), proc_.dialogFolder("midi"), "*.mid;*.midi");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                            const auto f = pickedFile(fc);
                            proc_.rememberDialogFolder("midi", f);
                            if (f.existsAsFile()) proc_.loadTake(f);
                          });
  };

  addAndMakeVisible(status_);
  styleLabel(status_, "");

  addAndMakeVisible(audioHeading_);
  styleLabel(audioHeading_, TRANS("Audio"));
  for (auto* b : {&audioRecord_, &audioStop_, &audioFolder_}) addAndMakeVisible(*b);
  addAndMakeVisible(audioStatus_);
  styleLabel(audioStatus_, "");
  addAndMakeVisible(audioNote_);
  styleNote(audioNote_,
            TRANS("Captures the organ's output after the master fader and before "
                  "the metronome, so the click stays out of the file. Written on a "
                  "background thread at 24-bit; the audio callback only hands over "
                  "the block it has already finished."));

  audioRecord_.onClick = [this] {
    audioFailed_ = proc_.startAudioRecording() == juce::File();
    timerCallback();
  };
  audioStop_.onClick = [this] { proc_.audioRecorder().stop(); };
  audioFolder_.onClick = [this] {
    audioChooser_ = std::make_unique<juce::FileChooser>(TRANS("Keep the recordings in"),
                                                        proc_.audioRecordingFolder());
    audioChooser_->launchAsync(juce::FileBrowserComponent::openMode |
                                   juce::FileBrowserComponent::canSelectDirectories,
                               [this](const juce::FileChooser& fc) {
                                 const auto f = fc.getResult();
                                 if (f.isDirectory()) proc_.rememberDialogFolder("recording", f);
                                 audioFailed_ = false;
                                 timerCallback();
                               });
  };

  startTimerHz(8);
}

RecorderPanel::~RecorderPanel() { stopTimer(); }

void RecorderPanel::askBeforeRecording() {
  auto& rec = proc_.recorder();
  // Nothing to lose: there is no take, or the take is on disk and the file
  // still holds it.
  if (!rec.hasUnsavedTake()) {
    rec.startRecording();
    return;
  }
  // The take is not on disk yet. The box is built by hand: the stock box has
  // two buttons, and this question has three answers.
  auto* box = new juce::AlertWindow(
      TRANS("Replace the recording?"),
      TRANS("This recording has not been saved. Replace it, or append to it and "
            "carry on from where it stopped."),
      juce::MessageBoxIconType::QuestionIcon, this);
  box->addButton(TRANS("Replace"), 1);
  box->addButton(TRANS("Append"), 2);
  box->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<RecorderPanel> self(this);
  // The box is deleted when it is dismissed and the answer comes after that.
  // Nothing waits here: the organ keeps playing while the player decides.
  box->enterModalState(
      true, juce::ModalCallbackFunction::create([self](int result) {
        if (self == nullptr) return;
        if (result == 1) self->proc_.recorder().startRecording();
        else if (result == 2)
          self->proc_.recorder().startRecording(MidiRecorder::Take::Append);
      }),
      true);
}

// Learn or clear the console's Record and Play buttons; the mapping is the
// console's, the same on every organ.
void RecorderPanel::showMidiMenu() {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    proc_.cancelLearn();
    return;
  }
  juce::PopupMenu menu;
  const std::pair<MidiTargetKind, juce::String> targets[] = {{MidiTargetKind::RecorderRecord, TRANS("Record")},
                                                             {MidiTargetKind::RecorderPlay, TRANS("Play")}};
  int item = 1;
  for (const auto& [kind, name] : targets) {
    juce::PopupMenu sub;
    const auto existing = map.bindingsFor(kind, 0);
    if (existing.empty()) sub.addSectionHeader(TRANS("Not mapped"));
    for (const MidiBinding* b : existing) sub.addSectionHeader(MidiEventDialog::describeBinding(*b));
    sub.addItem(item, TRANS("Learn"));
    sub.addItem(item + 1, TRANS("Clear"), !existing.empty());
    sub.addItem(item + 2, TRANS("MIDI window, and its lamp..."));
    menu.addSubMenu(name, sub);
    item += 3;
  }
  juce::Component::SafePointer<RecorderPanel> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&midi_), [self](int chosen) {
    if (self == nullptr || chosen <= 0) return;
    const bool record = chosen <= 3;
    const auto kind = record ? MidiTargetKind::RecorderRecord : MidiTargetKind::RecorderPlay;
    switch ((chosen - 1) % 3) {
      case 0: self->proc_.beginLearn(kind, 0, false); break;
      case 1: self->proc_.setObjectBindings(kind, 0, {}); break;
      default: MidiEventDialog::show(self->proc_, kind, 0, record ? TRANS("Record") : TRANS("Play")); break;
    }
  });
}

void RecorderPanel::timerCallback() {
  // A tab of the settings window that is not the one on show.
  if (!isShowing()) return;
  const auto& r = proc_.recorder();
  juce::String s;
  switch (r.state()) {
    case MidiRecorder::State::Recording:
      s = TRANS("Recording - %1 s, %2 events")
              .replace("%1", juce::String(r.positionSeconds(), 1))
              .replace("%2", juce::String(r.eventCount()));
      break;
    case MidiRecorder::State::Playing:
      s = TRANS("Playing - %1 / %2 s")
              .replace("%1", juce::String(r.positionSeconds(), 1))
              .replace("%2", juce::String(r.lengthSeconds(), 1));
      break;
    case MidiRecorder::State::Idle:
      s = r.empty() ? TRANS("Nothing recorded")
                    : TRANS("%1 events, %2 s")
                          .replace("%1", juce::String(r.eventCount()))
                          .replace("%2", juce::String(r.lengthSeconds(), 1));
      break;
  }
  if (const auto kept = proc_.lastButtonSave(); kept != juce::File() && r.state() == MidiRecorder::State::Recording)
    s += "  -  " + TRANS("the last take was saved as %1").replace("%1", kept.getFileName());
  status_.setText(s, juce::dontSendNotification);
  record_.setToggleState(r.isRecording(), juce::dontSendNotification);
  play_.setEnabled(!r.empty());
  save_.setEnabled(!r.empty());
  saveForSequencer_.setEnabled(!r.empty());

  auto& a = proc_.audioRecorder();
  const bool on = a.isRecording();
  if (on) audioFailed_ = false;
  const juce::String folder = proc_.audioRecordingFolder().getFullPathName();
  juce::String audio =
      on ? TRANS("Recording %1 - %2 s")
               .replace("%1", a.file().getFileName())
               .replace("%2", juce::String(a.secondsRecorded(), 1))
      : audioFailed_ ? TRANS("Could not write a file in %1").replace("%1", folder)
                     : TRANS("Recordings go to %1").replace("%1", folder);
  // A disk that holds the writer up past its room leaves gaps in the take
  // (#1128). The player hears them as clicks, so the panel says so, during
  // the take and after it until the next one starts.
  if (a.framesLost() > 0 && !audioFailed_)
    audio += "  -  " + TRANS("%1 s missing: the disk was too slow")
                           .replace("%1", juce::String(a.secondsLost(), 1));
  audioStatus_.setText(audio, juce::dontSendNotification);
  audioStatus_.setTooltip(on ? a.file().getFullPathName() : folder);
  audioRecord_.setEnabled(!on);
  audioStop_.setEnabled(on);
  audioFolder_.setEnabled(!on);
}

// Places a row of buttons, each as wide as its own text. A row that is
// wider than the panel wraps to a second line. Returns the height the row
// took.
int RecorderPanel::placeRow(juce::Rectangle<int> row,
                             std::initializer_list<juce::TextButton*> buttons) {
  auto* first = *buttons.begin();
  const auto font = buttonFont(*first, row.getHeight());
  int x = row.getX();
  int y = row.getY();
  int lines = 1;
  for (auto* b : buttons) {
    const int w = buttonWidth(*b, font);
    if (x > row.getX() && x + w > row.getRight()) {
      x = row.getX();
      y += row.getHeight() + kGap;
      ++lines;
    }
    b->setBounds(x, y, w, row.getHeight());
    x += w + kGap;
  }
  return lines * row.getHeight() + (lines - 1) * kGap;
}

int RecorderPanel::preferredWidth() {
  const auto font = buttonFont(record_, kRow);
  int widest = 0;
  for (auto row : {std::initializer_list<juce::TextButton*>{&record_, &play_, &stop_},
                   std::initializer_list<juce::TextButton*>{&save_, &load_, &clear_, &midi_},
                   std::initializer_list<juce::TextButton*>{&saveForSequencer_},
                   std::initializer_list<juce::TextButton*>{&audioRecord_, &audioStop_, &audioFolder_}})
    widest = juce::jmax(widest, rowWidth(row, font));
  return widest + 2 * kMargin;
}

void RecorderPanel::resized() {
  auto r = getLocalBounds().reduced(kMargin);
  // A row that had to wrap takes a line more, and everything below it moves
  // down: a panel narrower than the buttons needs (a phone in portrait) keeps
  // every button on the screen.
  int h = placeRow(r.removeFromTop(kRow), {&record_, &play_, &stop_});
  r.removeFromTop(h - kRow + kGap);
  h = placeRow(r.removeFromTop(kRow), {&save_, &load_, &clear_, &midi_});
  r.removeFromTop(h - kRow + kGap);
  // The option goes on a line of its own, so it never runs off a narrow panel.
  h = placeRow(r.removeFromTop(kRow), {&saveForSequencer_});
  r.removeFromTop(h - kRow + kGap);
  markers_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  status_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);

  // Audio capture below the MIDI half, with its own heading: they are two
  // recordings of the same performance and mixing the controls would suggest
  // one set of transport buttons drives both.
  r.removeFromTop(kGap * 2);
  audioHeading_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(2);
  h = placeRow(r.removeFromTop(kRow), {&audioRecord_, &audioStop_, &audioFolder_});
  r.removeFromTop(h - kRow + kGap);
  audioStatus_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  audioNote_.setBounds(r);
}

// ------------------------------------------------------------------ midi

MidiPanel::MidiPanel(MasterpieceProcessor& p, juce::AudioDeviceManager& devices, Part part)
    : proc_(p), devices_(devices), part_(part) {
  addAndMakeVisible(inputsLabel_);
  styleLabel(inputsLabel_, TRANS("MIDI inputs"));
  addAndMakeVisible(outputsLabel_);
  styleLabel(outputsLabel_, TRANS("MIDI output"));
  addAndMakeVisible(sharedNote_);
  sharedNote_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  addAndMakeVisible(useAsDefault_);
  useAsDefault_.onClick = [this] {
    proc_.useChannelsAsDefaultConsole();
    showDefaultConsole();
    showDefaultChannels();
  };
  addAndMakeVisible(clearDefault_);
  clearDefault_.onClick = [this] {
    proc_.clearDefaultConsole();
    showDefaultConsole();
    showDefaultChannels();
  };
  addAndMakeVisible(defaultNote_);
  defaultNote_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  showDefaultConsole();
  addAndMakeVisible(defaultChannelsLabel_);
  styleLabel(defaultChannelsLabel_, TRANS("Default channels"));
  defaultChannelsLabel_.setTooltip(TRANS("The channel each keyboard of an organ comes in on, for organs with "
                                         "no channels saved for them. An organ's own choice keeps it."));
  for (int role = 0; role < kDefaultRoles; ++role) {
    auto& label = defaultRoleLabels_[static_cast<size_t>(role)];
    auto& box = defaultRoleChannels_[static_cast<size_t>(role)];
    label.setText(role == 0 ? TRANS("Pedal") : TRANS("Manual %1").replace("%1", juce::String(role)),
                  juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(label);
    box.addItem("-", 1);
    for (int c = 1; c <= 16; ++c) box.addItem(juce::String(c), c + 1);
    addAndMakeVisible(box);
    box.onChange = [this, role, &box] {
      proc_.setDefaultConsoleChannel(role, box.getSelectedId() - 1);
      showDefaultConsole();
      showDefaultChannels();
    };
    auto& device = defaultRoleDevices_[static_cast<size_t>(role)];
    device.setTooltip(TRANS("The MIDI device this keyboard comes from. Any device: every device sending on "
                            "the channel plays it."));
    addAndMakeVisible(device);
    device.onChange = [this, role, &device] {
      proc_.setDefaultConsoleDevice(role, ManualDialog::deviceInBox(device, proc_));
      showDefaultConsole();
    };
  }
  showDefaultChannels();
  addAndMakeVisible(keyboardsLabel_);
  styleLabel(keyboardsLabel_, TRANS("Keyboards"));

  addAndMakeVisible(output_);
  output_.onClick = [this] { showOutputMenu(); };

  addAndMakeVisible(feedback_);
  feedback_.setToggleState(proc_.midiFeedbackEnabled(), juce::dontSendNotification);
  feedback_.onClick = [this] {
    proc_.setMidiFeedbackEnabled(feedback_.getToggleState());
  };
  addAndMakeVisible(holdEcho_);
  holdEcho_.setToggleState(proc_.midiSendsHoldEcho(), juce::dontSendNotification);
  holdEcho_.setTooltip(TRANS("For a console that lights its own lamps: a stop moved from the console leaves out "
                             "the Send event that repeats the same message"));
  holdEcho_.onClick = [this] { proc_.setMidiSendsHoldEcho(holdEcho_.getToggleState()); };
  addAndMakeVisible(cancelResets_);
  cancelResets_.setToggleState(proc_.cancelResetsKeyboards(), juce::dontSendNotification);
  cancelResets_.onClick = [this] {
    proc_.setCancelResetsKeyboards(cancelResets_.getToggleState());
  };
  addAndMakeVisible(leaveHeld_);
  leaveHeld_.setToggleState(proc_.stopsLeaveHeldNotes(), juce::dontSendNotification);
  leaveHeld_.setTooltip(TRANS("Off, a stop or coupler moved while keys are held sounds or stops on those "
                              "notes at once, as on a console with electric action. On, the held notes keep "
                              "what they started with, and only the keys pressed afterwards play the new "
                              "registration."));
  leaveHeld_.onClick = [this] { proc_.setStopsLeaveHeldNotes(leaveHeld_.getToggleState()); };
  addAndMakeVisible(sustain_);
  sustain_.setToggleState(proc_.sustainPedal(), juce::dontSendNotification);
  sustain_.setTooltip(TRANS("For directing a choir: while a sustain pedal is down, the keys let go keep "
                            "sounding until it is lifted. Controller 64 on any channel. A controller 64 "
                            "learned for something else keeps doing that."));
  sustain_.onClick = [this] { proc_.setSustainPedal(sustain_.getToggleState()); };
  addAndMakeVisible(noiseFilter_);
  noiseFilter_.setToggleState(proc_.learnNoiseFilter(), juce::dontSendNotification);
  noiseFilter_.setTooltip(TRANS("For a console whose contacts send stray notes: a shorter note is not learned. "
                                "Controllers and program changes are learned at once. Leave it off for pistons "
                                "that send only a short note."));
  noiseFilter_.onClick = [this] { proc_.setLearnNoiseFilter(noiseFilter_.getToggleState()); };

  // Everything played, kept: a take a file, in Recordings/Archive.
  {
    const auto a = proc_.archiveSettings();
    addAndMakeVisible(archive_);
    archive_.setToggleState(a.on, juce::dontSendNotification);
    archive_.setTooltip(TRANS("Records all the time: a take begins with the first note and ends after the silence "
                              "chosen, each in its own file in Documents/Masterpiece/Recordings/Archive, by date. "
                              "The oldest files go first when the folder passes its size or age."));
    archive_.onClick = [this] { applyArchive(); };
    const auto fill = [](juce::ComboBox& box, std::initializer_list<int> values, int current,
                         const std::function<juce::String(int)>& text) {
      for (const int v : values) box.addItem(text(v), v);
      if (box.indexOfItemId(current) < 0) box.addItem(text(current), current);
      box.setSelectedId(current, juce::dontSendNotification);
    };
    fill(archiveSilence_, {10, 30, 60, 120, 300, 600}, a.silenceSeconds, [](int s) {
      return s < 60 ? TRANS("ends after %1 s of silence").replace("%1", juce::String(s))
                    : TRANS("ends after %1 min of silence").replace("%1", juce::String(s / 60));
    });
    fill(archiveSize_, {100, 250, 500, 1000, 2000, 5000}, a.maxMegabytes, [](int mb) {
      return mb < 1000 ? TRANS("up to %1 MB").replace("%1", juce::String(mb))
                       : TRANS("up to %1 GB").replace("%1", juce::String(mb / 1000));
    });
    fill(archiveAge_, {7, 30, 90, 365}, a.maxDays,
         [](int d) { return TRANS("kept %1 days").replace("%1", juce::String(d)); });
    for (auto* box : {&archiveSilence_, &archiveSize_, &archiveAge_}) {
      addAndMakeVisible(*box);
      box->onChange = [this] { applyArchive(); };
    }
  }

  // A console with fewer keyboards than the organ has manuals: a piston makes
  // a keyboard play another manual (#90). Listed here as the manual's Pistons
  // tab lists them, and learned there, the one way (#850): choose the manual,
  // play a key on the keyboard, press the piston.
  addAndMakeVisible(keyboardsHeading_);
  styleLabel(keyboardsHeading_, TRANS("Manual buttons"));
  addAndMakeVisible(noPistons_);
  styleLabel(noPistons_, TRANS("No pistons yet."));
  addAndMakeVisible(learnManuals_);
  learnManuals_.setTooltip(TRANS("Choose the manual: its MIDI window opens at the Pistons tab"));
  learnManuals_.onClick = [this] {
    juce::PopupMenu menu;
    for (Id kb : proc_.playableKeyboards())
      menu.addItem(static_cast<int>(kb), juce::String(proc_.keyboardName(kb)));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&learnManuals_),
                       [this](int kb) {
                         if (kb > 0) ManualDialog::show(proc_, static_cast<Id>(kb), ManualDialog::Tab::Pistons);
                       });
  };

  addAndMakeVisible(saveMap_);
  saveMap_.onClick = [this] { proc_.saveMidiMap(); };
  addAndMakeVisible(clearMap_);
  clearMap_.onClick = [this] { proc_.editMidiMap([](MidiMap& map) { map.clear(); }); };
  addChildComponent(fromGrandOrgue_);
  fromGrandOrgue_.setTooltip(TRANS("The MIDI settings this organ has in GrandOrgue, from its settings file (.cmb)"));
  fromGrandOrgue_.onClick = [this] { importFromGrandOrgue(); };

  addAndMakeVisible(mapStatus_);
  styleLabel(mapStatus_, "");

  // Shown only where the system can pair one; the Audio tab's device list
  // carried it before (#680).
  addChildComponent(bluetooth_);
  bluetooth_.setTooltip(TRANS("Pairs a Bluetooth MIDI device"));
  bluetooth_.onClick = [] { openBluetoothMidiPairing(); };

  addAndMakeVisible(consoleHeading_);
  styleLabel(consoleHeading_, TRANS("Console"));
  // Every one momentary: a piston is a button, and a latching mapping would
  // act only every other press.
  learnButtons_ = {
      {&learnPagePrev_, MidiTargetKind::ConsolePrevPage, TRANS("Page -")},
      {&learnPageNext_, MidiTargetKind::ConsoleNextPage, TRANS("Page +")},
      {&learnLayout_, MidiTargetKind::ConsoleNextLayout, TRANS("Console size")},
      {&learnStopList_, MidiTargetKind::ConsoleToggleStopList, TRANS("Panel")},
      {&learnKeyboard_, MidiTargetKind::ConsoleToggleKeyboard, TRANS("Keyboard")},
      {&learnCombinations_, MidiTargetKind::ConsoleToggleCombinations, TRANS("Combinations")},
      {&learnQuit_, MidiTargetKind::QuitProgram, TRANS("Quit")},
      {&learnPowerOff_, MidiTargetKind::PowerOffComputer, TRANS("Power off")},
      {&learnScoreBack_, MidiTargetKind::ScorePageBack, TRANS("Score page -")},
      {&learnScoreForward_, MidiTargetKind::ScorePageForward, TRANS("Score page +")},
  };
  for (size_t i = 0; i < learnButtons_.size(); ++i) {
    addAndMakeVisible(*learnButtons_[i].button);
    learnButtons_[i].button->onClick = [this, i] { learnMenu(learnButtons_[i]); };
  }

  addAndMakeVisible(powerHeading_);
  styleLabel(powerHeading_, TRANS("Power"));
  for (auto* b : {&learnQuit_, &learnPowerOff_})
    b->setTooltip(TRANS("For a console with no screen. Obeyed once Masterpiece has run two minutes, and "
                        "only when the button is held three seconds; a button whose message has no "
                        "release is pressed twice within two seconds. Masterpiece saves before it quits."));
  addChildComponent(powerOffNeeds_);
  powerOffNeeds_.setText(TRANS("To shut the computer down with this button, turn on \"Allow quitting Masterpiece "
                               "and shutting the computer down\" in Remote control."),
                         juce::dontSendNotification);
  powerOffNeeds_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  powerOffNeeds_.setMinimumHorizontalScale(0.7f);

  addAndMakeVisible(lampLabel_);
  styleLabel(lampLabel_, TRANS("Ready lamp"));
  lampKind_.addItem(TRANS("Off"), 1);
  lampKind_.addItem(TRANS("Note"), 2);
  lampKind_.addItem(TRANS("Controller"), 3);
  for (int ch = 1; ch <= 16; ++ch) lampChannel_.addItem(TRANS("Channel %1").replace("%1", juce::String(ch)), ch);
  for (int n = 0; n < 128; ++n) lampNumber_.addItem(juce::String(n), n + 1);
  {
    const auto lamp = proc_.readyLamp();
    lampKind_.setSelectedId(lamp.kind == MidiSourceKind::Note             ? 2
                            : lamp.kind == MidiSourceKind::ControlChange ? 3
                                                                          : 1,
                            juce::dontSendNotification);
    lampChannel_.setSelectedId(lamp.channel, juce::dontSendNotification);
    lampNumber_.setSelectedId(lamp.number + 1, juce::dontSendNotification);
  }
  for (auto* box : {&lampKind_, &lampChannel_, &lampNumber_}) {
    addAndMakeVisible(*box);
    box->onChange = [this] { pushReadyLamp(); };
  }
  lampKind_.setTooltip(TRANS("A message on the MIDI output that lights a lamp once the organ is ready, blinks "
                             "while one loads and while a quit or power-off button is held, and goes out "
                             "with no organ"));
  for (auto* e : {&lampLit_, &lampDark_}) {
    addAndMakeVisible(*e);
    e->onReturnKey = e->onFocusLost = [this] { pushLampMessages(); };
  }
  lampLit_.setText(proc_.readyLampLitMessages(), juce::dontSendNotification);
  lampDark_.setText(proc_.readyLampDarkMessages(), juce::dontSendNotification);
  lampLit_.setTextToShowWhenEmpty(TRANS("Lit: hex messages"), juce::Colours::grey);
  lampDark_.setTextToShowWhenEmpty(TRANS("Dark: hex messages"), juce::Colours::grey);
  lampLit_.setTooltip(TRANS("MIDI messages in hex sent when the ready lamp lights, as well as the one above. "
                            "Several are separated by commas: 90 3C 7F, B0 14 7F"));
  lampDark_.setTooltip(TRANS("MIDI messages in hex sent when the ready lamp goes dark. Several are separated "
                             "by commas: 80 3C 00, B0 14 00"));
  addAndMakeVisible(loadMessageLabel_);
  styleLabel(loadMessageLabel_, TRANS("On organ load"));
  addAndMakeVisible(loadMessage_);
  loadMessage_.setText(proc_.loadMessage(), juce::dontSendNotification);
  loadMessage_.setTextToShowWhenEmpty(TRANS("Hex messages, F0 ... F7, B0 07 7F"), juce::Colours::grey);
  loadMessage_.setTooltip(TRANS("MIDI messages in hex, sent on the MIDI output each time an organ loads, before "
                                "any stop moves or lamp lights: for a controller that needs a mode set first, "
                                "such as a pad grid's programmer layout. Several are separated by commas. Empty "
                                "sends nothing."));
  loadMessage_.onReturnKey = loadMessage_.onFocusLost = [this] { pushLoadMessage(); };
  addAndMakeVisible(unloadMessageLabel_);
  styleLabel(unloadMessageLabel_, TRANS("On organ unload"));
  addAndMakeVisible(unloadMessage_);
  unloadMessage_.setText(proc_.unloadMessage(), juce::dontSendNotification);
  unloadMessage_.setTextToShowWhenEmpty(TRANS("Hex messages, F0 ... F7, B0 07 7F"), juce::Colours::grey);
  unloadMessage_.setTooltip(TRANS("MIDI messages in hex, sent on the MIDI output when an organ unloads: before "
                                  "the next organ loads, and when the program quits. For a relay that turns the "
                                  "speakers off. Several are separated by commas. Empty sends nothing."));
  unloadMessage_.onReturnKey = unloadMessage_.onFocusLost = [this] {
    if (proc_.setUnloadMessage(unloadMessage_.getText())) {
      unloadMessage_.removeColour(juce::TextEditor::outlineColourId);
      unloadMessage_.setText(proc_.unloadMessage(), juce::dontSendNotification);
    } else {
      unloadMessage_.setColour(juce::TextEditor::outlineColourId, juce::Colours::red);
    }
    unloadMessage_.repaint();
  };
  for (auto* b : {&sendLoadMessage_, &sendUnloadMessage_}) {
    addAndMakeVisible(*b);
    b->setButtonText(TRANS("Send now"));
    b->setTooltip(TRANS("Sends the messages on the MIDI output now, to try them"));
  }
  sendLoadMessage_.onClick = [this] { sendNow(loadMessage_, sendLoadMessage_); };
  sendUnloadMessage_.onClick = [this] { sendNow(unloadMessage_, sendUnloadMessage_); };
  addAndMakeVisible(scoreHeading_);
  styleLabel(scoreHeading_, TRANS("Score app"));
  addAndMakeVisible(scoreSendsLabel_);
  styleLabel(scoreSendsLabel_, TRANS("Sends"));
  for (auto* b : {&learnScoreBack_, &learnScoreForward_})
    b->setTooltip(TRANS("Turns the page of a score app: sends the message below on the MIDI output"));
  const ScorePageMessage page = proc_.scorePageMessage();
  addAndMakeVisible(scoreKind_);
  scoreKind_.addItem(TRANS("Note"), 1);
  scoreKind_.addItem(TRANS("Controller"), 2);
  scoreKind_.addItem(TRANS("Program change"), 3);
  scoreKind_.setSelectedId(page.kind + 1, juce::dontSendNotification);
  scoreKind_.setTooltip(TRANS("The message a page turn sends on the MIDI output; teach it to the score app"));
  addAndMakeVisible(scoreChannel_);
  for (int ch = 1; ch <= 16; ++ch) scoreChannel_.addItem(TRANS("ch %1").replace("%1", juce::String(ch)), ch);
  scoreChannel_.setSelectedId(page.channel, juce::dontSendNotification);
  for (auto* e : {&scoreForward_, &scoreBack_}) {
    addAndMakeVisible(*e);
    e->setInputRestrictions(3, "0123456789");
    e->onReturnKey = e->onFocusLost = [this] { pushScorePage(); };
  }
  scoreForward_.setText(juce::String(page.forward), juce::dontSendNotification);
  scoreBack_.setText(juce::String(page.back), juce::dontSendNotification);
  scoreForward_.setTooltip(TRANS("The number sent for the next page"));
  scoreBack_.setTooltip(TRANS("The number sent for the page before"));
  scoreKind_.onChange = scoreChannel_.onChange = [this] { pushScorePage(); };

  for (auto& l : learnButtons_) l.tip = l.button->getTooltip();
  refresh();
  startTimerHz(2);
}

void MidiPanel::applyPart() {
  std::vector<juce::Component*> console{&inputsLabel_, &outputsLabel_, &output_, &feedback_, &holdEcho_, &cancelResets_,
                                        &leaveHeld_, &sustain_, &noiseFilter_, &archive_, &archiveSilence_,
                                        &archiveSize_, &archiveAge_, &consoleHeading_, &learnPagePrev_, &learnPageNext_, &learnLayout_,
                                        &learnStopList_, &learnKeyboard_, &learnCombinations_};
  // The tab's whole purpose, so Simple mode shows these too.
  const std::vector<juce::Component*> events{
      &powerHeading_, &learnQuit_, &learnPowerOff_, &lampLabel_, &lampKind_, &lampChannel_, &lampNumber_,
      &lampLit_, &lampDark_, &loadMessageLabel_, &loadMessage_, &unloadMessageLabel_, &unloadMessage_,
      &sendLoadMessage_, &sendUnloadMessage_, &scoreHeading_, &learnScoreBack_, &learnScoreForward_,
      &scoreSendsLabel_, &scoreKind_, &scoreChannel_, &scoreForward_, &scoreBack_};
  for (auto& b : inputs_) console.push_back(b.get());
  console.push_back(&defaultChannelsLabel_);
  for (auto& c : defaultRoleLabels_) console.push_back(&c);
  for (auto& c : defaultRoleChannels_) console.push_back(&c);
  for (auto& c : defaultRoleDevices_) console.push_back(&c);
  std::vector<juce::Component*> organ{&keyboardsLabel_, &sharedNote_, &useAsDefault_, &clearDefault_,
                                      &defaultNote_, &keyboardsHeading_, &learnManuals_, &noPistons_};
  for (auto& v : keyboardLabels_) organ.push_back(v.get());
  for (auto& v : keyboardSummaries_) organ.push_back(v.get());
  for (auto& v : keyboardMore_) organ.push_back(v.get());
  for (auto& v : pistonLines_) organ.push_back(v.get());
  // What Simple mode leaves out: the console builder's settings.
  const std::vector<juce::Component*> professional{
      &cancelResets_, &leaveHeld_, &noiseFilter_, &archive_, &archiveSilence_, &archiveSize_, &archiveAge_,
      &consoleHeading_, &learnPagePrev_, &learnPageNext_,
      &learnLayout_, &learnStopList_, &learnKeyboard_, &learnCombinations_,
      &keyboardsHeading_, &learnManuals_, &noPistons_, &useAsDefault_, &clearDefault_, &defaultNote_};
  shownSimple_ = proc_.simpleMode();
  const auto& mine = part_ == Part::Console ? console : part_ == Part::Organ ? organ : events;
  for (const auto* list : std::initializer_list<const std::vector<juce::Component*>*>{&console, &organ, &events})
    if (list != &mine)
      for (auto* c : *list) c->setVisible(false);
  for (auto* c : mine)
    c->setVisible(!shownSimple_ || std::find(professional.begin(), professional.end(), c) == professional.end());
  // The pistons' lines go with their heading, and "none yet" only when none.
  for (auto& v : pistonLines_) v->setVisible(part_ == Part::Organ && !shownSimple_);
  noPistons_.setVisible(part_ == Part::Organ && !shownSimple_ && pistonLines_.empty());
  powerOffNeeds_.setVisible(part_ == Part::Events && !powerOffAllowed());
  saveMap_.setVisible(!shownSimple_ && part_ != Part::Events);
  clearMap_.setVisible(!shownSimple_ && part_ != Part::Events);
  fromGrandOrgue_.setVisible(part_ == Part::Organ);
  bluetooth_.setVisible(part_ == Part::Console && juce::BluetoothMidiDevicePairingDialogue::isAvailable());
}

// The same menu as a console control's: what it is mapped to, then Learn,
// Clear and the MIDI window. Pressed while learning, it stops learning.
void MidiPanel::learnMenu(const LearnButton& l) {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    proc_.cancelLearn();
    showLearnState();
    return;
  }
  const MidiTargetKind kind = l.kind;
  constexpr Id id = 0;
  const juce::String name = l.name;
  juce::PopupMenu menu;
  const auto existing = map.bindingsFor(kind, id);
  if (existing.empty()) menu.addSectionHeader(TRANS("Not mapped"));
  for (const MidiBinding* b : existing) menu.addSectionHeader(MidiEventDialog::describeBinding(*b));
  menu.addItem(1, TRANS("Learn"));
  menu.addItem(2, TRANS("Clear"), !existing.empty());
  menu.addItem(3, TRANS("MIDI window..."));
  juce::Component::SafePointer<MidiPanel> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(l.button),
                     [self, kind, id, name](int choice) {
                       if (self == nullptr) return;
                       switch (choice) {
                         case 1: self->proc_.beginLearn(kind, id, false); break;
                         case 2: self->proc_.setObjectBindings(kind, id, {}); break;
                         case 3: MidiEventDialog::show(self->proc_, kind, id, name); break;
                         default: return;
                       }
                       self->showLearnState();
                     });
}

// Orange while a button learns, green once it is mapped, its tooltip naming
// the message.
void MidiPanel::showLearnState() {
  const auto& map = proc_.midiMap();
  for (const auto& l : learnButtons_) {
    constexpr Id id = 0;
    const bool learning = map.learning() && map.learningKind() == l.kind && map.learningTarget() == id;
    const auto existing = map.bindingsFor(l.kind, id);
    auto* b = l.button;
    if (learning)
      b->setColour(juce::TextButton::buttonColourId, juce::Colours::darkorange);
    else if (!existing.empty())
      b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2f6b3f));
    else
      b->removeColour(juce::TextButton::buttonColourId);
    juce::String tip = l.tip;
    for (const MidiBinding* m : existing)
      tip += (tip.isEmpty() ? "" : "\n") + TRANS("Mapped: %1").replace("%1", MidiEventDialog::describeBinding(*m));
    if (b->getTooltip() != tip) b->setTooltip(tip);
  }
}

void MidiPanel::pushReadyLamp() {
  MasterpieceProcessor::ReadyLamp lamp;
  const int k = lampKind_.getSelectedId();
  lamp.kind = k == 2 ? MidiSourceKind::Note : k == 3 ? MidiSourceKind::ControlChange : MidiSourceKind::None;
  lamp.channel = juce::jmax(1, lampChannel_.getSelectedId());
  lamp.number = juce::jmax(0, lampNumber_.getSelectedId() - 1);
  proc_.setReadyLamp(lamp);
}

// Messages that do not read are outlined in red; the ones before are kept.
void MidiPanel::pushLampMessages() {
  const bool ok = proc_.setReadyLampMessages(lampLit_.getText(), lampDark_.getText());
  for (auto* e : {&lampLit_, &lampDark_}) {
    if (ok) e->removeColour(juce::TextEditor::outlineColourId);
    else e->setColour(juce::TextEditor::outlineColourId, juce::Colours::red);
    e->repaint();
  }
  if (ok) {
    lampLit_.setText(proc_.readyLampLitMessages(), juce::dontSendNotification);
    lampDark_.setText(proc_.readyLampDarkMessages(), juce::dontSendNotification);
  }
}

void MidiPanel::pushLoadMessage() {
  if (proc_.setLoadMessage(loadMessage_.getText())) {
    loadMessage_.removeColour(juce::TextEditor::outlineColourId);
    loadMessage_.setText(proc_.loadMessage(), juce::dontSendNotification);
  } else {
    loadMessage_.setColour(juce::TextEditor::outlineColourId, juce::Colours::red);
  }
  loadMessage_.repaint();
}

void MidiPanel::sendNow(juce::TextEditor& field, juce::TextButton& button) {
  if (field.isEmpty()) return;
  if (proc_.sendMidiNow(field.getText())) {
    button.setButtonText(TRANS("Sent"));
  } else {
    // Either the text is not whole messages (the field turns red) or there is
    // no output to send on.
    std::vector<uint8_t> bytes;
    const bool whole = parseMidiText(field.getText().toStdString(), bytes);
    if (!whole) {
      field.setColour(juce::TextEditor::outlineColourId, juce::Colours::red);
      field.repaint();
    }
    button.setButtonText(whole ? TRANS("No MIDI output") : TRANS("Not sent"));
  }
  juce::Component::SafePointer<juce::TextButton> safe(&button);
  juce::Timer::callAfterDelay(1500, [safe] {
    if (safe != nullptr) safe->setButtonText(TRANS("Send now"));
  });
}

MidiPanel::~MidiPanel() { stopTimer(); }

// GrandOrgue keeps a set's MIDI settings in its settings file for that set,
// <hash>-0.cmb in its Data folder, compressed or plain (#865). The events this
// program has a binding for replace the mappings of the objects they name;
// the window then says what was taken and what was left out.
void MidiPanel::importFromGrandOrgue() {
  if (proc_.organModel().grandOrgueObjects.empty()) {
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::InfoIcon)
            .withTitle(TRANS("From GrandOrgue"))
            .withMessage(TRANS("Load this organ from its GrandOrgue .organ file to bring its MIDI settings over."))
            .withButton(TRANS("OK"))
            .withAssociatedComponent(this),
        nullptr);
    return;
  }
  grandOrgueChooser_ = std::make_unique<juce::FileChooser>(TRANS("GrandOrgue's settings for this organ"),
                                                           proc_.dialogFolder("grandorgue"), "*.cmb");
  juce::Component::SafePointer<MidiPanel> self(this);
  grandOrgueChooser_->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [self](const juce::FileChooser& fc) {
        if (self == nullptr) return;
        const juce::File f = fc.getResult();
        if (!f.existsAsFile()) return;
        self->proc_.rememberDialogFolder("grandorgue", f);
        juce::MemoryBlock bytes;
        f.loadFileAsData(bytes);
        const auto* b = static_cast<const uint8_t*>(bytes.getData());
        if (bytes.getSize() > 2 && b[0] == 0x1f && b[1] == 0x8b) {
          juce::MemoryInputStream packed(bytes, false);
          juce::GZIPDecompressorInputStream unpacked(&packed, false,
                                                     juce::GZIPDecompressorInputStream::gzipFormat);
          juce::MemoryBlock plain;
          unpacked.readIntoMemoryBlock(plain);
          bytes = std::move(plain);
        }
        const std::string text(static_cast<const char*>(bytes.getData()), bytes.getSize());
        GrandOrgueMidiReport r;
        self->proc_.editMidiMap(
            [&](MidiMap& map) { r = importGrandOrgueMidi(text, self->proc_.organModel(), map); });
        juce::StringArray lines;
        if (!r.ok) {
          lines.add(TRANS("This file holds no MIDI settings from GrandOrgue."));
        } else {
          self->proc_.saveMidiMap();
          // GrandOrgue's On lamp lights the console's ready lamp here.
          const bool lamp = (!r.readyLampLit.empty() || !r.readyLampDark.empty()) &&
                            self->proc_.setReadyLampMessages(juce::String(r.readyLampLit),
                                                             juce::String(r.readyLampDark));
          if (!r.organName.empty())
            lines.add(TRANS("Saved by GrandOrgue for %1.").replace("%1", juce::String::fromUTF8(r.organName.c_str())));
          lines.add(TRANS("Stops, pistons and swells: %1 events").replace("%1", juce::String(r.bindings)));
          lines.add(TRANS("Manuals: %1 events").replace("%1", juce::String(r.keyboards)));
          lines.add(TRANS("Lamps and messages sent: %1").replace("%1", juce::String(r.sends)));
          if (r.leftOut > 0)
            lines.add(TRANS("Events of other kinds, left out: %1").replace("%1", juce::String(r.leftOut)));
          if (r.otherObjects > 0)
            lines.add(TRANS("Objects of another organ, left out: %1").replace("%1", juce::String(r.otherObjects)));
          if (r.shortcuts > 0)
            lines.add(TRANS("Computer key shortcuts: %1").replace("%1", juce::String(r.shortcuts)));
          if (lamp) lines.add(TRANS("Ready lamp: GrandOrgue's On lamp"));
          if (r.anyDevice > 0)
            lines.add(TRANS("Devices not connected now, taken as any console: %1").replace("%1", juce::String(r.anyDevice)));
        }
        self->refresh();
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::InfoIcon)
                                         .withTitle(TRANS("From GrandOrgue"))
                                         .withMessage(lines.joinIntoString("\n"))
                                         .withButton(TRANS("OK"))
                                         .withAssociatedComponent(self.getComponent()),
                                     nullptr);
      });
}

// Manuals that answer the same channel, named. Sharing one is a way of
// playing two divisions from one keyboard; doing it unawares is how a manual
// ends up sounding the wrong division, which is what this line prevents.
void MidiPanel::applyArchive() {
  MasterpieceProcessor::ArchiveSettings a;
  a.on = archive_.getToggleState();
  a.silenceSeconds = archiveSilence_.getSelectedId();
  a.maxMegabytes = archiveSize_.getSelectedId();
  a.maxDays = archiveAge_.getSelectedId();
  proc_.setArchiveSettings(a);
}

void MidiPanel::showDefaultConsole() {
  const auto& def = proc_.defaultConsole();
  if (def.empty()) {
    defaultNote_.setText(TRANS("No default: each organ starts from its own channels"),
                         juce::dontSendNotification);
    return;
  }
  juce::String text = TRANS("Default for organs with no channels saved:");
  const auto& devices = proc_.defaultConsoleDevices();
  for (const auto& [role, channel] : def) {
    juce::String item =
        role == 0 ? TRANS("Pedal %1").replace("%1", juce::String(channel))
                  : TRANS("Manual %1 %2").replace("%1", juce::String(role)).replace("%2", juce::String(channel));
    if (const auto dev = devices.find(role); dev != devices.end()) item << " (" << juce::String(dev->second) << ")";
    text << " " << item << ";";
  }
  defaultNote_.setText(text.dropLastCharacters(1), juce::dontSendNotification);
}

void MidiPanel::showDefaultChannels() {
  const auto& def = proc_.defaultConsole();
  const auto& devices = proc_.defaultConsoleDevices();
  for (int role = 0; role < kDefaultRoles; ++role) {
    const auto it = def.find(role);
    defaultRoleChannels_[static_cast<size_t>(role)].setSelectedId(it == def.end() ? 1 : it->second + 1,
                                                                  juce::dontSendNotification);
    auto& device = defaultRoleDevices_[static_cast<size_t>(role)];
    const auto dev = devices.find(role);
    ManualDialog::fillDeviceBox(device, proc_, dev == devices.end() ? std::string() : dev->second);
    // A device for a keyboard with no channel plays nothing: the box opens
    // once a channel is chosen.
    device.setEnabled(it != def.end());
  }
}

void MidiPanel::showSharedChannels() {
  std::map<int, juce::StringArray> byChannel;
  for (const auto& b : proc_.channelAssignments()) {
    if (b.channel <= 0) continue;
    const auto it = proc_.organModel().keyboards.find(b.keyboardId);
    byChannel[b.channel].add(it != proc_.organModel().keyboards.end() &&
                                     !it->second.name.empty()
                                 ? juce::String(it->second.name)
                                 : TRANS("keyboard %1").replace("%1", juce::String((int) b.keyboardId)));
  }
  juce::String text;
  for (const auto& [channel, names] : byChannel) {
    if (names.size() < 2) continue;
    text << (text.isEmpty() ? "" : "   ")
         << TRANS("Channel %1: %2").replace("%1", juce::String(channel)).replace("%2", names.joinIntoString(", "));
  }
  if (text.isNotEmpty()) text = TRANS("Shared -- %1").replace("%1", text);
  // A manual left on its default channel after another was put there: the
  // channel plays the other one now, and the row still reads "Default" (#637).
  auto nameOf = [this](Id kb) { return juce::String(proc_.keyboardName(kb)); };
  for (Id kb : proc_.playableKeyboards())
    if (const Id other = proc_.defaultChannelTakenBy(kb); other != 0)
      text << (text.isEmpty() ? "" : "   ")
           << TRANS("Channel %1 plays %2; choose Channel %1 for %3 to play both.")
                  .replace("%1", juce::String(proc_.channelForKeyboard(kb)))
                  .replace("%2", nameOf(other))
                  .replace("%3", nameOf(kb));
  sharedNote_.setText(text, juce::dontSendNotification);
}

void MidiPanel::refresh() {
  inputs_.clear();
  for (const auto& in : juce::MidiInput::getAvailableDevices()) {
    // Ticked is what the player wants. A device another program holds cannot
    // be opened (Windows gives a MIDI port to one program at a time): it stays
    // ticked, says so, and is opened when it is free (#305).
    const bool wanted = proc_.midiInputEnabled(in.name);
    const bool open = devices_.isMidiInputDeviceEnabled(in.identifier);
    auto b = std::make_unique<juce::ToggleButton>(
        wanted && !open ? TRANS("%1  -  in use by another program; opened when it is free").replace("%1", in.name)
                        : in.name);
    b->setToggleState(wanted, juce::dontSendNotification);
    const auto id = in.identifier;
    auto* raw = b.get();
    const auto name = in.name;
    b->onClick = [this, id, name, raw] {
      const bool on = raw->getToggleState();
      devices_.setMidiInputDeviceEnabled(id, on);
      proc_.setMidiInputEnabled(name, on);
      const bool failed = on && !devices_.isMidiInputDeviceEnabled(id);
      raw->setButtonText(failed ? TRANS("%1  -  in use by another program; opened when it is free").replace("%1", name)
                                : name);
      if (failed) {
        juce::Logger::writeToLog("midi: " + name + " could not be opened");
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, TRANS("%1 could not be opened").replace("%1", name),
            TRANS("Another program is probably using this MIDI device. Masterpiece keeps it ticked "
                  "and opens it as soon as the other program lets it go."));
      }
    };
    addAndMakeVisible(*b);
    inputs_.push_back(std::move(b));
  }
  // And the inputs Masterpiece opens itself, such as its JACK MIDI port,
  // which the system's list above does not hold (#225).
  for (const auto& name : proc_.ownMidiInputs()) {
    auto b = std::make_unique<juce::ToggleButton>(name);
    b->setToggleState(proc_.midiInputEnabled(name), juce::dontSendNotification);
    auto* raw = b.get();
    b->onClick = [this, name, raw] { proc_.setMidiInputEnabled(name, raw->getToggleState()); };
    addAndMakeVisible(*b);
    inputs_.push_back(std::move(b));
  }
  // The default console's device boxes list the devices there are now.
  showDefaultChannels();

  // Which channel plays which manual. The organ ships a default assignment
  // (Hauptwerk's own: the pedal is channel 1, the manuals follow), and the
  // manual's window is where a player whose console disagrees says so.
  keyboardIds_ = proc_.playableKeyboards();
  keyboardLabels_.clear();
  keyboardSummaries_.clear();
  keyboardMore_.clear();
  for (Id kb : keyboardIds_) {
    auto label = std::make_unique<juce::Label>();
    styleLabel(*label, juce::String(proc_.keyboardName(kb)));
    addAndMakeVisible(*label);
    keyboardLabels_.push_back(std::move(label));

    auto summary = std::make_unique<juce::Label>();
    styleLabel(*summary, {});
    summary->setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(*summary);
    keyboardSummaries_.push_back(std::move(summary));

    auto more = std::make_unique<juce::TextButton>(TRANS("MIDI window..."));
    more->onClick = [this, kb] { ManualDialog::show(proc_, kb); };
    addAndMakeVisible(*more);
    keyboardMore_.push_back(std::move(more));
  }
  pistonLines_.clear();
  showManuals();
  showSharedChannels();

  showOutputs();
  applyPart();
  resized();
}

bool MidiPanel::showManuals() {
  if (part_ != Part::Organ) return false;
  bool relayout = false;
  for (size_t i = 0; i < keyboardIds_.size() && i < keyboardSummaries_.size(); ++i) {
    const auto text = ManualDialog::describeManual(proc_, keyboardIds_[i]).joinIntoString("\n");
    auto& label = *keyboardSummaries_[i];
    if (label.getText() == text) continue;
    // A row grows a line per entry.
    if (label.getText().isEmpty() ||
        juce::StringArray::fromLines(label.getText()).size() != juce::StringArray::fromLines(text).size())
      relayout = true;
    label.setText(text, juce::dontSendNotification);
  }
  juce::StringArray pistons;
  for (Id kb : keyboardIds_) pistons.addArray(ManualDialog::describePistons(proc_, kb));
  if (pistons != pistonRows()) {
    pistonLines_.clear();
    for (const auto& line : pistons) {
      auto label = std::make_unique<juce::Label>();
      styleLabel(*label, line);
      addChildComponent(*label);
      pistonLines_.push_back(std::move(label));
    }
    relayout = true;
  }
  return relayout;
}

juce::StringArray MidiPanel::pistonRows() const {
  juce::StringArray out;
  for (const auto& l : pistonLines_) out.add(l->getText());
  return out;
}

void MidiPanel::showOutputs() {
  const auto& names = proc_.midiOutputNames();
  output_.setButtonText(names.isEmpty() ? TRANS("None") : names.joinIntoString(", "));
}

void MidiPanel::showOutputMenu() {
  // The ports plugged in, then those chosen before and not plugged in now:
  // the choice is kept, and shown as it is.
  // Two ports of one name are two rows, each with its own label (#980).
  auto ports = midiPortLabels(juce::MidiOutput::getAvailableDevices());
  for (const auto& name : proc_.midiOutputNames()) ports.addIfNotAlreadyThere(name);
  const auto& chosen = proc_.midiOutputNames();
  juce::PopupMenu menu;
  menu.addItem(1, TRANS("None"), true, chosen.isEmpty());
  menu.addSeparator();
  for (int i = 0; i < ports.size(); ++i) menu.addItem(i + 2, ports[i], true, chosen.contains(ports[i]));
  juce::Component::SafePointer<MidiPanel> safe(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&output_), [safe, ports](int result) {
    if (safe == nullptr || result == 0) return;
    // The processor holds the ports and keeps the choice: the console's lamps
    // and displays follow it after this window closes, and at the next start.
    juce::StringArray next;
    if (result > 1) {
      next = safe->proc_.midiOutputNames();
      const juce::String port = ports[result - 2];
      if (next.contains(port)) next.removeString(port);
      else next.add(port);
    }
    safe->proc_.openMidiOutputs(next);
    safe->showOutputs();
  });
}

void MidiPanel::timerCallback() {
  // A tab of the settings window that is not the one on show.
  if (!isShowing()) return;
  // The manual windows edit what the rows show: they follow, so the tab never
  // says less than the window holds.
  if (showManuals()) {
    showSharedChannels();
    applyPart();
    resized();
  } else if (part_ == Part::Organ) {
    showSharedChannels();
  }
  // The permission is set in another tab: the line follows it.
  if (proc_.simpleMode() != shownSimple_ ||
      (part_ == Part::Events && powerOffNeeds_.isVisible() == powerOffAllowed())) {
    applyPart();
    resized();
  }
  const auto& map = proc_.midiMap();
  juce::String s = TRANS("MIDI mappings: %1").replace("%1", juce::String(static_cast<int>(map.size())));
  if (map.learning()) s += "  -  " + TRANS("LEARNING: move a control now");
  mapStatus_.setText(s, juce::dontSendNotification);
  showLearnState();
}

bool MidiPanel::powerOffAllowed() const { return proc_.remoteSettings().allowPowerOff; }

void MidiPanel::pushScorePage() {
  ScorePageMessage m;
  m.kind = juce::jmax(0, scoreKind_.getSelectedId() - 1);
  m.channel = juce::jlimit(1, 16, scoreChannel_.getSelectedId());
  m.forward = juce::jlimit(0, 127, scoreForward_.getText().getIntValue());
  m.back = juce::jlimit(0, 127, scoreBack_.getText().getIntValue());
  scoreForward_.setText(juce::String(m.forward), juce::dontSendNotification);
  scoreBack_.setText(juce::String(m.back), juce::dontSendNotification);
  if (!(m == proc_.scorePageMessage())) proc_.setScorePageMessage(m);
}

void MidiPanel::resized() {
  // Every row at its full height from the top: the tab scrolls when the
  // window is shorter than the panel.
  auto r = getLocalBounds().reduced(12).withHeight(1 << 20);
  juce::Rectangle<int> row;
  if (part_ == Part::Console) {
    inputsLabel_.setBounds(r.removeFromTop(kRow));
    for (auto& b : inputs_) {
      b->setBounds(r.removeFromTop(kRow).reduced(12, 0));
      r.removeFromTop(2);
    }
    if (inputs_.empty()) {
      r.removeFromTop(kRow); // leaves room for the "none found" case
    }
    if (bluetooth_.isVisible()) bluetooth_.setBounds(r.removeFromTop(kRow).reduced(12, 0).withWidth(180));
    // The default channels, each with its device, as many to a row as fit.
    r.removeFromTop(4);
    defaultChannelsLabel_.setBounds(r.removeFromTop(kRow));
    const int lineWidth = r.getWidth() - 24;
    const int perRow = juce::jlimit(1, 3, lineWidth / 420);
    const int deviceWidth = juce::jlimit(120, 260, lineWidth / perRow - 90 - 6 - 70 - 6 - 24);
    for (int role = 0; role < kDefaultRoles; role += perRow) {
      auto line = r.removeFromTop(kRow).reduced(12, 0);
      for (int k = role; k < role + perRow && k < kDefaultRoles; ++k) {
        defaultRoleLabels_[static_cast<size_t>(k)].setBounds(line.removeFromLeft(90));
        line.removeFromLeft(6);
        defaultRoleChannels_[static_cast<size_t>(k)].setBounds(line.removeFromLeft(70).reduced(0, 1));
        line.removeFromLeft(6);
        defaultRoleDevices_[static_cast<size_t>(k)].setBounds(line.removeFromLeft(deviceWidth).reduced(0, 1));
        line.removeFromLeft(24);
      }
      r.removeFromTop(2);
    }
  } else if (part_ == Part::Organ) {
    keyboardsLabel_.setBounds(r.removeFromTop(kRow));
    // Name, what plays it (a line per row of its window), and the window.
    constexpr int kLine = 20;
    for (size_t i = 0; i < keyboardSummaries_.size(); ++i) {
      const int lines = juce::jmax(1, juce::StringArray::fromLines(keyboardSummaries_[i]->getText()).size());
      auto kbRow = r.removeFromTop(juce::jmax(kRow, lines * kLine + 6)).reduced(12, 0);
      if (i < keyboardMore_.size())
        keyboardMore_[i]->setBounds(kbRow.removeFromRight(140).withHeight(kRow).reduced(0, 1));
      kbRow.removeFromRight(6);
      keyboardLabels_[i]->setBounds(kbRow.removeFromLeft(150).withHeight(kRow));
      keyboardSummaries_[i]->setBounds(kbRow.withTrimmedTop(3));
      r.removeFromTop(2);
    }

    sharedNote_.setBounds(r.removeFromTop(kRow).reduced(12, 0));
    if (!shownSimple_) {
      auto defRow = r.removeFromTop(kRow).reduced(12, 0);
      useAsDefault_.setBounds(defRow.removeFromLeft(220).reduced(0, 1));
      defRow.removeFromLeft(6);
      clearDefault_.setBounds(defRow.removeFromLeft(110).reduced(0, 1));
      defRow.removeFromLeft(10);
      defaultNote_.setBounds(defRow);
    }
    // The pistons that move a keyboard to another manual (#90).
    if (!shownSimple_) {
      r.removeFromTop(4);
      row = r.removeFromTop(kRow);
      keyboardsHeading_.setBounds(row.removeFromLeft(120));
      learnManuals_.setBounds(row.removeFromLeft(190).reduced(2, 0));
      for (auto& l : pistonLines_) l->setBounds(r.removeFromTop(kRow).reduced(12, 0));
      if (pistonLines_.empty()) noPistons_.setBounds(r.removeFromTop(kRow).reduced(12, 0));
    }
  }

  if (part_ == Part::Console) {
    r.removeFromTop(kGap);
    row = r.removeFromTop(kRow);
    outputsLabel_.setBounds(row.removeFromLeft(120));
    output_.setBounds(row.removeFromLeft(300));
    r.removeFromTop(4);
    feedback_.setBounds(r.removeFromTop(kRow));
    holdEcho_.setBounds(r.removeFromTop(kRow));
    sustain_.setBounds(r.removeFromTop(kRow));
  }
  if (part_ == Part::Console && !shownSimple_) {
    cancelResets_.setBounds(r.removeFromTop(kRow));
    leaveHeld_.setBounds(r.removeFromTop(kRow));
    noiseFilter_.setBounds(r.removeFromTop(kRow));
    row = r.removeFromTop(kRow);
    archive_.setBounds(row.removeFromLeft(200));
    archiveSilence_.setBounds(row.removeFromLeft(200).reduced(2, 1));
    archiveSize_.setBounds(row.removeFromLeft(110).reduced(2, 1));
    archiveAge_.setBounds(row.removeFromLeft(120).reduced(2, 1));
    r.removeFromTop(kGap);
    // Console actions, on their own row under the same idea: things a physical
    // console's thumb pistons do that the organ file never mentions.
    row = r.removeFromTop(kRow);
    consoleHeading_.setBounds(row.removeFromLeft(120));
    for (auto* b : {&learnPagePrev_, &learnPageNext_, &learnLayout_}) {
      b->setBounds(row.removeFromLeft(150).reduced(2, 0));
      row.removeFromLeft(6);
    }
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    row.removeFromLeft(120);
    for (auto* b : {&learnStopList_, &learnKeyboard_, &learnCombinations_}) {
      b->setBounds(row.removeFromLeft(150).reduced(2, 0));
      row.removeFromLeft(6);
    }
  }
  if (part_ == Part::Events) {
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    powerHeading_.setBounds(row.removeFromLeft(120));
    learnQuit_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
    learnPowerOff_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    if (powerOffNeeds_.isVisible()) {
      row = r.removeFromTop(kRow);
      row.removeFromLeft(120);
      powerOffNeeds_.setBounds(row);
    }
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    lampLabel_.setBounds(row.removeFromLeft(120));
    lampKind_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
    lampChannel_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
    lampNumber_.setBounds(row.removeFromLeft(90).reduced(2, 0));
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    row.removeFromLeft(120);
    lampLit_.setBounds(row.removeFromLeft(228).reduced(2, 2));
    row.removeFromLeft(6);
    lampDark_.setBounds(row.removeFromLeft(228).reduced(2, 2));
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    loadMessageLabel_.setBounds(row.removeFromLeft(120));
    loadMessage_.setBounds(row.removeFromLeft(306).reduced(2, 2));
    row.removeFromLeft(6);
    sendLoadMessage_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    unloadMessageLabel_.setBounds(row.removeFromLeft(120));
    unloadMessage_.setBounds(row.removeFromLeft(306).reduced(2, 2));
    row.removeFromLeft(6);
    sendUnloadMessage_.setBounds(row.removeFromLeft(150).reduced(2, 0));

    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    scoreHeading_.setBounds(row.removeFromLeft(120));
    for (auto* b : {&learnScoreBack_, &learnScoreForward_}) {
      b->setBounds(row.removeFromLeft(150).reduced(2, 0));
      row.removeFromLeft(6);
    }
    // The message sent, on a row of its own, as the ready lamp's.
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    scoreSendsLabel_.setBounds(row.removeFromLeft(120));
    scoreKind_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
    scoreChannel_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
    scoreForward_.setBounds(row.removeFromLeft(60).reduced(2, 2));
    row.removeFromLeft(6);
    scoreBack_.setBounds(row.removeFromLeft(60).reduced(2, 2));
  }

  r.removeFromTop(kGap);
  row = r.removeFromTop(kRow);
  if (!shownSimple_) {
    saveMap_.setBounds(row.removeFromLeft(130));
    row.removeFromLeft(kGap);
    clearMap_.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(kGap);
  }
  if (part_ == Part::Organ) {
    fromGrandOrgue_.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(kGap);
  }
  mapStatus_.setBounds(row);
  r.removeFromTop(kGap);
  const int needed = r.getY() + 12;
  if (needed != contentHeight_) {
    contentHeight_ = needed;
    if (auto* host = findParentComponentOfClass<ScrollHost>()) host->fit();
  }
}

// --------------------------------------------------------------- voicing

namespace {

juce::String noteName(int midiNote) {
  return juce::MidiMessage::getMidiNoteName(midiNote, true, true, 4);
}

// The Voicing tab's faders: one width and one height, whatever the window.
constexpr int kVoicingFaderW = 72, kVoicingFaderH = 170;

// Every pipe of one rank at once, a column each, lowest note on the left
// (#849): how a voicer in a church evens out a rank, seeing the whole compass
// and drawing across it. A column shows the pipe's own level or tuning; the
// rank's trim is added on top of it, as everywhere on the Voicing tab.
class RankPipesView final : public juce::Component, private juce::Timer {
public:
  std::function<void()> onChanged;          // a pipe's value was changed here
  std::function<void(int note)> onChosen;   // a pipe was clicked or chosen with the keys

  explicit RankPipesView(MasterpieceProcessor& p) : proc_(p) {
    setWantsKeyboardFocus(true);
    setTitle(TRANS("Every pipe of the rank"));
    startTimerHz(10);
  }

  void setRank(Id rankId) {
    rankId_ = rankId;
    pipes_.clear();
    notes_.clear();
    const auto it = proc_.organModel().ranks.find(rankId);
    if (it != proc_.organModel().ranks.end()) {
      std::vector<std::pair<int, Id>> byNote;
      for (const auto& pipe : it->second.pipes) byNote.push_back({pipe.midiNote, pipe.pipeId});
      std::sort(byNote.begin(), byNote.end());
      for (const auto& [note, id] : byNote) {
        notes_.push_back(note);
        pipes_.push_back(id);
      }
    }
    selected_ = pipes_.empty() ? -1 : juce::jlimit(0, static_cast<int>(pipes_.size()) - 1, selected_);
    repaint();
  }

  void setField(VoicingField f) {
    field_ = f;
    repaint();
  }
  VoicingField field() const { return field_; }
  float range() const { return field_ == VoicingField::Level ? 12.0f : 50.0f; }
  juce::String unit() const { return field_ == VoicingField::Level ? " dB" : " cents"; }

  // The note and value of the pipe chosen, for the line under the view.
  juce::String describeSelected() const {
    if (selected_ < 0) return {};
    const size_t i = static_cast<size_t>(selected_);
    const float v = valueAt(selected_);
    return noteName(notes_[i]) + " (" + juce::String(notes_[i]) + "):  " +
           (v > 0.0f ? "+" : "") + juce::String(v, 1) + unit();
  }

  void selectNote(int note) {
    for (size_t i = 0; i < notes_.size(); ++i)
      if (notes_[i] == note) {
        selected_ = static_cast<int>(i);
        repaint();
        return;
      }
  }

  void resetAll() {
    {
      const MasterpieceProcessor::AudioLock audio(proc_);
      for (const Id pipe : pipes_) {
        PipeVoicing pv = proc_.voicing().live().pipe(pipe);
        setVoicingValue(pv, field_, 0.0f);
        proc_.voicing().live().setPipe(pipe, pv);
      }
    }
    changed(true);
  }

  void paint(juce::Graphics& g) override {
    g.fillAll(juce::Colour(0xff15171c));
    const auto plot = plotArea();
    g.setFont(juce::FontOptions(12.0f));
    // The columns first, under the scale: the naturals' light, the sharps'
    // dark, as on the keyboard.
    for (int i = 0; i < static_cast<int>(pipes_.size()); ++i) {
      const float w = columnWidth();
      const float x = static_cast<float>(plot.getX()) + w * static_cast<float>(i);
      const bool sharp = juce::MidiMessage::isMidiNoteBlack(notes_[static_cast<size_t>(i)]);
      g.setColour(sharp ? juce::Colour(0xff0a0b0e) : juce::Colour(0xff272b34));
      g.fillRect(juce::Rectangle<float>(x + 0.5f, static_cast<float>(plot.getY()), w - 1.0f,
                                        static_cast<float>(plot.getHeight())));
    }
    // The scale: the middle line is the pipe as the set has it.
    const float r = range();
    for (const float v : {-r, -r / 2, 0.0f, r / 2, r}) {
      const float y = yOf(v);
      g.setColour(v == 0.0f ? juce::Colours::grey : juce::Colour(0xff2c3038));
      g.drawHorizontalLine(static_cast<int>(y), static_cast<float>(plot.getX()), static_cast<float>(plot.getRight()));
      g.setColour(juce::Colours::grey);
      g.drawText((v > 0 ? "+" : "") + juce::String(v, 0), 0, static_cast<int>(y) - 7, plot.getX() - 6, 14,
                 juce::Justification::centredRight);
    }
    if (pipes_.empty()) {
      g.setColour(juce::Colours::grey);
      g.drawText(TRANS("This rank has no pipes"), plot, juce::Justification::centred);
      return;
    }
    const int sounding = proc_.testNoteKey();
    const float w = columnWidth();
    const float zero = yOf(0.0f);
    for (int i = 0; i < static_cast<int>(pipes_.size()); ++i) {
      const float x = static_cast<float>(plot.getX()) + w * static_cast<float>(i);
      const auto column = juce::Rectangle<float>(x, static_cast<float>(plot.getY()), w, static_cast<float>(plot.getHeight()));
      const int note = notes_[static_cast<size_t>(i)];
      // Coloured like the keyboard, so a note is found at a glance (#1092):
      // the naturals' columns light, the sharps' dark, and the sharps' bars
      // a deeper shade.
      const bool sharp = juce::MidiMessage::isMidiNoteBlack(note);
      if (note == sounding) {
        g.setColour(juce::Colour(0xff2e4a3a));
        g.fillRect(column);
      }
      const float y = yOf(valueAt(i));
      g.setColour(sharp ? juce::Colour(0xffa86f22) : juce::Colour(0xffe8a33d));
      g.fillRect(juce::Rectangle<float>(x + 1.0f, std::min(y, zero), std::max(1.0f, w - 2.0f),
                                        std::max(1.0f, std::abs(y - zero))));
      if (i == selected_) {
        g.setColour(juce::Colours::white);
        g.drawRect(column.reduced(0.5f), 1.5f);
      }
      // A name under every C, and under every pipe when there is room.
      if (note % 12 == 0 || w >= 30.0f) {
        g.setColour(note % 12 == 0 ? juce::Colours::lightgrey : juce::Colours::grey);
        g.drawText(noteName(note), juce::Rectangle<float>(x - 10.0f, static_cast<float>(plot.getBottom()) + 2.0f,
                                                          w + 20.0f, 14.0f),
                   juce::Justification::centred);
      }
    }
  }

  // A click on another pipe chooses it and leaves its value alone (#1070);
  // a click or a drag from the pipe chosen sets it, and the drag its
  // neighbours.
  void mouseDown(const juce::MouseEvent& e) override {
    grabKeyboardFocus();
    const int i = indexAt(e.x);
    if (i < 0) return;
    if (i != selected_) {
      choose(i);
      return;
    }
    lastIndex_ = i;
    lastValue_ = valueFromY(e.y);
    stroke(i, lastValue_, i, lastValue_);
  }

  void mouseDrag(const juce::MouseEvent& e) override {
    if (lastIndex_ < 0) return;
    const int i = juce::jlimit(0, static_cast<int>(pipes_.size()) - 1, indexAt(e.x, true));
    const float v = valueFromY(e.y);
    stroke(lastIndex_, lastValue_, i, v);
    lastIndex_ = i;
    lastValue_ = v;
    if (i != selected_) choose(i);
  }

  void mouseUp(const juce::MouseEvent&) override {
    if (lastIndex_ >= 0) proc_.restartTestNote();
    lastIndex_ = -1;
  }

  // A double-click puts the pipe back as the set has it.
  void mouseDoubleClick(const juce::MouseEvent& e) override {
    const int i = indexAt(e.x);
    if (i >= 0) setOne(i, 0.0f);
  }

  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
    const int i = indexAt(e.x);
    if (i < 0 || wheel.deltaY == 0.0f) return;
    choose(i);
    setOne(i, valueAt(i) + (wheel.deltaY > 0 ? stepSize() : -stepSize()));
  }

  bool keyPressed(const juce::KeyPress& key) override {
    if (pipes_.empty()) return false;
    const int last = static_cast<int>(pipes_.size()) - 1;
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
      choose(juce::jlimit(0, last, std::max(0, selected_) + (code == juce::KeyPress::leftKey ? -1 : 1)));
      return true;
    }
    if (selected_ < 0) return false;
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
      setOne(selected_, valueAt(selected_) + (code == juce::KeyPress::upKey ? stepSize() : -stepSize()));
      return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
      setOne(selected_, 0.0f);
      return true;
    }
    return false;
  }

private:
  void timerCallback() override {
    // A slider, a console button or the other slot of A/B may have moved a
    // pipe; the test note may have moved to another key.
    std::vector<float> now;
    now.reserve(pipes_.size() + 1);
    for (int i = 0; i < static_cast<int>(pipes_.size()); ++i) now.push_back(valueAt(i));
    now.push_back(static_cast<float>(proc_.testNoteKey()));
    if (now != shown_) {
      shown_ = std::move(now);
      repaint();
    }
  }

  juce::Rectangle<int> plotArea() const { return getLocalBounds().withTrimmedLeft(40).withTrimmedRight(6).withTrimmedTop(8).withTrimmedBottom(20); }
  float columnWidth() const {
    return pipes_.empty() ? 1.0f : static_cast<float>(plotArea().getWidth()) / static_cast<float>(pipes_.size());
  }
  float yOf(float value) const {
    const auto plot = plotArea().toFloat();
    return plot.getCentreY() - juce::jlimit(-1.0f, 1.0f, value / range()) * plot.getHeight() * 0.5f;
  }
  float valueFromY(int y) const {
    const auto plot = plotArea().toFloat();
    float v = (plot.getCentreY() - static_cast<float>(y)) / (plot.getHeight() * 0.5f) * range();
    v = juce::jlimit(-range(), range(), v);
    // Near the middle is the middle: putting a pipe back is the commonest stroke.
    if (std::abs(v) < range() * 0.03f) v = 0.0f;
    return v;
  }
  float stepSize() const { return field_ == VoicingField::Level ? 0.5f : 1.0f; }
  int indexAt(int x, bool clampOutside = false) const {
    const auto plot = plotArea();
    if (pipes_.empty() || (!clampOutside && (x < plot.getX() || x >= plot.getRight()))) return -1;
    return juce::jlimit(0, static_cast<int>(pipes_.size()) - 1,
                        static_cast<int>(static_cast<float>(x - plot.getX()) / columnWidth()));
  }
  float valueAt(int i) const {
    return voicingValue(proc_.voicing().live().pipe(pipes_[static_cast<size_t>(i)]), field_);
  }
  void choose(int i) {
    selected_ = i;
    proc_.setVoicingTarget(rankId_, pipes_[static_cast<size_t>(i)]);
    if (onChosen) onChosen(notes_[static_cast<size_t>(i)]);
    repaint();
  }
  void stroke(int from, float fromValue, int to, float toValue) {
    {
      const MasterpieceProcessor::AudioLock audio(proc_);
      drawPipeStroke(proc_.voicing().live(), pipes_, from, fromValue, to, toValue, field_);
    }
    changed(false);
  }
  void setOne(int i, float value) {
    value = juce::jlimit(-range(), range(), value);
    stroke(i, value, i, value);
    proc_.restartTestNote();
  }
  void changed(bool restart) {
    // The pipes sounding take a new level in a short fade; a new tuning is
    // heard when the test note is struck again.
    proc_.voicingChanged();
    if (restart) proc_.restartTestNote();
    repaint();
    if (onChanged) onChanged();
  }

  MasterpieceProcessor& proc_;
  Id rankId_ = 0;
  std::vector<Id> pipes_;
  std::vector<int> notes_;
  VoicingField field_ = VoicingField::Level;
  int selected_ = -1;
  int lastIndex_ = -1;
  float lastValue_ = 0.0f;
  std::vector<float> shown_;
};

// The window around the view: which rank, level or tuning, and the pipe
// chosen in words.
class RankPipesContent final : public juce::Component, private juce::Timer {
public:
  RankPipesContent(MasterpieceProcessor& p, Id rankId, std::function<void()> changed,
                   std::function<void(Id rank, int note)> chosen)
      : proc_(p), view_(p), changed_(std::move(changed)), chosen_(std::move(chosen)) {
    addAndMakeVisible(rankLabel_);
    styleLabel(rankLabel_, TRANS("Rank"));
    addAndMakeVisible(rank_);
    for (const auto& [id, r] : proc_.organModel().ranks) rankIds_.push_back(id);
    std::sort(rankIds_.begin(), rankIds_.end());
    for (size_t i = 0; i < rankIds_.size(); ++i)
      rank_.addItem(rankTitle(proc_.organModel(), rankIds_[i]), static_cast<int>(i) + 1);
    rank_.onChange = [this] { showRank(); };
    addAndMakeVisible(fieldLabel_);
    styleLabel(fieldLabel_, TRANS("Shows"));
    addAndMakeVisible(field_);
    field_.addItem(TRANS("Level"), 1);
    field_.addItem(TRANS("Tuning"), 2);
    field_.setSelectedId(1, juce::dontSendNotification);
    field_.onChange = [this] {
      view_.setField(field_.getSelectedId() == 2 ? VoicingField::Tuning : VoicingField::Level);
      showSelected();
    };
    addAndMakeVisible(reset_);
    reset_.setTooltip(TRANS("Puts every pipe of this rank back as the set has it, for the value shown"));
    reset_.onClick = [this] { view_.resetAll(); };
    addAndMakeVisible(view_);
    view_.onChanged = [this] {
      showSelected();
      if (changed_) changed_();
    };
    view_.onChosen = [this](int note) {
      showSelected();
      if (chosen_) chosen_(currentRank(), note);
    };
    addAndMakeVisible(selected_);
    styleLabel(selected_, "");
    addAndMakeVisible(help_);
    styleNote(help_, TRANS("A click chooses a pipe. Click or drag from the pipe chosen to set it and its neighbours; "
                           "a double-click puts one back. "
                           "The wheel and the arrow keys move the pipe chosen in steps. "
                           "With Test note on, the key played is marked. The rank's own trim is added to each pipe."));
    selectRank(rankId);
    startTimerHz(10);
  }

  void selectRank(Id rankId) {
    const auto it = std::find(rankIds_.begin(), rankIds_.end(), rankId);
    rank_.setSelectedId(it == rankIds_.end() ? 1 : static_cast<int>(it - rankIds_.begin()) + 1,
                        juce::dontSendNotification);
    showRank();
  }

  void selectNote(int note) {
    view_.selectNote(note);
    showSelected();
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12);
    auto row = r.removeFromTop(kRow);
    rankLabel_.setBounds(row.removeFromLeft(50));
    rank_.setBounds(row.removeFromLeft(320).reduced(0, 1));
    row.removeFromLeft(kGap * 2);
    fieldLabel_.setBounds(row.removeFromLeft(56));
    field_.setBounds(row.removeFromLeft(120).reduced(0, 1));
    reset_.setBounds(row.removeFromRight(170).reduced(0, 1));
    r.removeFromTop(kGap);
    help_.setBounds(r.removeFromBottom(36));
    selected_.setBounds(r.removeFromBottom(kRow));
    view_.setBounds(r);
  }

private:
  Id currentRank() const {
    const int idx = rank_.getSelectedId() - 1;
    return idx >= 0 && idx < static_cast<int>(rankIds_.size()) ? rankIds_[static_cast<size_t>(idx)] : 0;
  }
  void showRank() {
    view_.setRank(currentRank());
    showSelected();
  }
  void showSelected() {
    const auto text = view_.describeSelected();
    selected_.setText(text.isEmpty() ? TRANS("Click a pipe, or play it with Test note on") : text,
                      juce::dontSendNotification);
  }
  // The key played with the test note chooses its pipe here too.
  void timerCallback() override {
    const int key = proc_.testNoteKey();
    if (key == shownKey_) return;
    shownKey_ = key;
    if (key >= 0 && proc_.voicingRank() == currentRank()) {
      view_.selectNote(key);
      showSelected();
    }
  }

  MasterpieceProcessor& proc_;
  RankPipesView view_;
  std::function<void()> changed_;
  std::function<void(Id, int)> chosen_;
  juce::Label rankLabel_, fieldLabel_, selected_, help_;
  juce::ComboBox rank_, field_;
  juce::TextButton reset_{TRANS("Reset these pipes")};
  std::vector<Id> rankIds_;
  int shownKey_ = -1;
};

} // namespace

// A fader per pipe, lowest note on the left, a gap before each C (#1188).
// The panel reads and writes the values; the desk lays the faders out, marks
// the pipe chosen and says which one was touched.
class VoicingPipeDesk final : public juce::Component {
public:
  // kNameH matches the value box under the rank's fader beside the desk.
  static constexpr int kFaderW = 24, kOctaveGap = 12, kNameH = 20;

  std::function<void(int note)> onChoose;              // a pipe's fader was pressed
  std::function<void(int note, float value)> onSet;    // a pipe's fader was moved
  std::function<void()> onSetEnd;                      // the drag ended

  // Said on every pipe's fader: the faders cover the desk.
  void setTooltip(const juce::String& text) {
    tooltip_ = text;
    for (auto& s : faders_) s->setTooltip(text);
  }

  // The notes of the rank's pipes, sorted. The faders are rebuilt only when
  // the notes change.
  void setNotes(const std::vector<int>& notes) {
    if (notes == notes_) return;
    notes_ = notes;
    faders_.clear();
    for (const int note : notes_) {
      auto s = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
      s->setTitle(TRANS("Pipe %1").replace("%1", noteName(note)));
      s->setPopupDisplayEnabled(true, true, this);
      s->setDoubleClickReturnValue(true, 0.0);
      s->setTooltip(tooltip_);
      s->onDragStart = [this, note] {
        if (onChoose) onChoose(note);
      };
      auto* raw = s.get();
      s->onValueChange = [this, note, raw] {
        if (onSet) onSet(note, static_cast<float>(raw->getValue()));
      };
      s->onDragEnd = [this] {
        if (onSetEnd) onSetEnd();
      };
      addAndMakeVisible(*s);
      faders_.push_back(std::move(s));
    }
    applyRange();
    setSize(widthNeeded(), getHeight());
    resized();
  }

  void setField(VoicingField f) {
    field_ = f;
    applyRange();
  }

  // Each pipe's value, without calling back.
  void setValues(const std::function<float(int note)>& valueOf) {
    for (size_t i = 0; i < notes_.size(); ++i) {
      const double v = valueOf(notes_[i]);
      if (faders_[i]->getValue() != v) faders_[i]->setValue(v, juce::dontSendNotification);
    }
  }

  void setChosen(int note) {
    if (note == chosen_) return;
    chosen_ = note;
    repaint();
  }

  // The position of a note's fader, to bring it into view.
  juce::Rectangle<int> faderBounds(int note) const {
    for (size_t i = 0; i < notes_.size(); ++i)
      if (notes_[i] == note) return faders_[i]->getBounds();
    return {};
  }

  int widthNeeded() const {
    int x = 0;
    for (size_t i = 0; i < notes_.size(); ++i) x += kFaderW + (i > 0 && notes_[i] % 12 == 0 ? kOctaveGap : 0);
    return x + 2;
  }

  void resized() override {
    int x = 0;
    const int h = getHeight() - kNameH;
    for (size_t i = 0; i < notes_.size(); ++i) {
      if (i > 0 && notes_[i] % 12 == 0) x += kOctaveGap;
      faders_[i]->setBounds(x, 0, kFaderW, h);
      x += kFaderW;
    }
  }

  void paint(juce::Graphics& g) override {
    g.setFont(juce::FontOptions(11.0f));
    for (size_t i = 0; i < notes_.size(); ++i) {
      const auto b = faders_[i]->getBounds();
      // The sharps a shade darker, as on the keyboard.
      if (juce::MidiMessage::isMidiNoteBlack(notes_[i])) {
        g.setColour(juce::Colour(0xff111317));
        g.fillRect(b);
      }
      if (notes_[i] == chosen_) {
        g.setColour(juce::Colour(0xff2e4a3a));
        g.fillRect(b);
        g.setColour(juce::Colours::white);
        g.drawRect(b, 1);
      }
      // The C's named under their faders, the chosen pipe too.
      if (notes_[i] % 12 == 0 || notes_[i] == chosen_) {
        g.setColour(notes_[i] == chosen_ ? juce::Colours::white : juce::Colours::lightgrey);
        g.drawText(noteName(notes_[i]), b.getX() - 12, b.getBottom() + 1, kFaderW + 24, kNameH - 2,
                   juce::Justification::centred);
      }
    }
  }

private:
  void applyRange() {
    const double r = field_ == VoicingField::Level ? 12.0 : 50.0;
    for (auto& s : faders_) {
      s->setRange(-r, r, 0.1);
      s->setTextValueSuffix(field_ == VoicingField::Level ? " dB" : " cents");
      styleFader(*s, {-r, -r / 2, 0.0, r / 2, r}, 0.0);
    }
  }

  std::vector<int> notes_;
  std::vector<std::unique_ptr<juce::Slider>> faders_;
  VoicingField field_ = VoicingField::Level;
  int chosen_ = -1;
  juce::String tooltip_;
};

VoicingPanel::VoicingPanel(MasterpieceProcessor& p) : proc_(p), desk_(std::make_unique<VoicingPipeDesk>()) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Voicing"));

  addAndMakeVisible(chooseBy_);
  chooseBy_.setTitle(TRANS("Choose by"));
  // What a group does, said plainly (#1055).
  chooseBy_.setTooltip(TRANS("Choose one rank, or every rank of a stop, a division or a windchest at once") + ". " +
                       TRANS("A change reaches each rank chosen. With One pipe it reaches the pipe at that note in "
                             "each, so the perspectives of a stop are voiced together."));
  chooseBy_.onChange = [this] {
    fillChoices();
    loadCurrentIntoSliders();
  };
  addAndMakeVisible(rank_);
  rank_.onChange = [this] { loadCurrentIntoSliders(); };
  addAndMakeVisible(testNote_);
  testNote_.setTooltip(TRANS("A key plays what is chosen here alone; another key moves it there, and the same key "
                             "again stops it. Changes are heard at once."));
  testNote_.onClick = [this] { proc_.setTestNote(testNote_.getToggleState()); };
  addAndMakeVisible(midi_);
  midi_.setTooltip(TRANS("Buttons on the console for level and tuning, acting on the rank or pipe chosen here"));
  midi_.onClick = [this] { midiMenu(); };

  addAndMakeVisible(scopeLabel_);
  styleLabel(scopeLabel_, TRANS("Applies to"));
  addAndMakeVisible(scope_);
  scope_.addItem(TRANS("The whole rank"), 1);
  scope_.addItem(TRANS("One pipe"), 2);
  scope_.addItem(TRANS("Pipes on the C side"), 3);
  scope_.addItem(TRANS("Pipes on the C# side"), 4);
  scope_.setSelectedId(1, juce::dontSendNotification);
  scope_.onChange = [this] {
    const bool perPipe = scope_.getSelectedId() == 2;
    note_.setEnabled(perPipe);
    noteLabel_.setEnabled(perPipe);
    // Choosing pipes by key belongs to one pipe at a time.
    if (!perPipe) keysChoose_.setToggleState(false, juce::dontSendNotification);
    loadCurrentIntoSliders();
  };
  addAndMakeVisible(keysChoose_);
  keysChoose_.setToggleState(false, juce::dontSendNotification);
  keysChoose_.setTooltip(TRANS("A key played chooses its pipe, with the test note or without it. Off, the pipe "
                               "chosen stays, and the test note plays its neighbours beside it."));
  keysChoose_.onClick = [this] {
    shownKeys_ = proc_.keysPressed();
    if (keysChoose_.getToggleState() && scope_.getSelectedId() != 2) scope_.setSelectedId(2, juce::sendNotificationSync);
  };

  addAndMakeVisible(noteLabel_);
  styleLabel(noteLabel_, TRANS("MIDI note"));
  addAndMakeVisible(note_);
  note_.setRange(0, 127, 1);
  // The note's name beside its number; either can be typed as a number.
  note_.textFromValueFunction = [](double v) {
    const int n = static_cast<int>(v);
    return juce::MidiMessage::getMidiNoteName(n, true, true, 4) + " (" + juce::String(n) + ")";
  };
  note_.valueFromTextFunction = [](const juce::String& text) {
    return (text.containsChar('(') ? text.fromFirstOccurrenceOf("(", false, false) : text).getDoubleValue();
  };
  note_.setValue(60, juce::dontSendNotification);
  note_.updateText();
  note_.setEnabled(false);
  note_.onValueChange = [this] { loadCurrentIntoSliders(); };
  // The key played with the test note reaches the note here however the
  // window was opened (#1068); the timer does nothing while the tab is hidden.
  startTimerHz(10);
  addAndMakeVisible(everyPipe_);
  everyPipe_.setTooltip(TRANS("Every pipe of the rank at once, to draw their levels or tunings across the compass"));
  everyPipe_.onClick = [this] { showEveryPipe(); };

  addAndMakeVisible(gainLabel_);
  styleLabel(gainLabel_, TRANS("Level"));
  addAndMakeVisible(gain_);
  // +/-12 dB is the range a voicer works in. Wider would mostly offer new
  // ways to make an organ wrong.
  gain_.setRange(-12.0, 12.0, 0.1);
  gain_.setTooltip(TRANS("The voiced level, saved with the organ: the whole rank, or the pipes chosen in Applies to"));
  gain_.setTextValueSuffix(" dB");
  gain_.setValue(0.0, juce::dontSendNotification);
  styleFader(gain_, {-12.0, -6.0, 0.0, 6.0, 12.0}, 0.0);
  gain_.onValueChange = [this] { pushCurrent(kVoicingLevel); };

  addAndMakeVisible(tuneLabel_);
  styleLabel(tuneLabel_, TRANS("Tuning"));
  // +/-50 cents is half a semitone: past that you are playing a different
  // note, not tuning this one.
  tune_.setRange(-50.0, 50.0, 0.1);
  tune_.setTextValueSuffix(" cents");
  tune_.setValue(0.0, juce::dontSendNotification);
  styleFader(tune_, fader::evenTicks(-50.0, 50.0), 0.0);
  tune_.onValueChange = [this] { pushCurrent(kVoicingTuning); };
  addAndMakeVisible(tune_);
  // A test note is struck again when a drag ends; during it, it would stutter.
  gain_.onDragEnd = tune_.onDragEnd = [this] { proc_.restartTestNote(); };

  // The EQ: shelves of +/-12 dB, the brightness above 3 kHz and the bass
  // below 200 Hz. A rank's and its division's add.
  for (auto* s : {&bright_, &bass_, &divBright_, &divBass_}) {
    addAndMakeVisible(*s);
    s->setRange(-12.0, 12.0, 0.5);
    s->setTextValueSuffix(" dB");
    s->setValue(0.0, juce::dontSendNotification);
    styleFader(*s, {-12.0, -6.0, 0.0, 6.0, 12.0}, 0.0);
  }
  addAndMakeVisible(brightLabel_);
  styleLabel(brightLabel_, TRANS("Brightness"));
  addAndMakeVisible(bassLabel_);
  styleLabel(bassLabel_, TRANS("Bass"));
  bright_.setTooltip(TRANS("The rank's treble, above 3 kHz: heard at once, on the pipes sounding too"));
  bass_.setTooltip(TRANS("The rank's bass, below 200 Hz: heard at once, on the pipes sounding too"));
  bright_.onValueChange = [this] { pushCurrent(kVoicingBrightness); };
  bass_.onValueChange = [this] { pushCurrent(kVoicingBass); };
  addAndMakeVisible(divisionLabel_);
  styleLabel(divisionLabel_, TRANS("Division EQ"));
  addAndMakeVisible(division_);
  division_.onChange = [this] { loadDivision(); };
  addAndMakeVisible(divBrightLabel_);
  styleLabel(divBrightLabel_, TRANS("Brightness"));
  addAndMakeVisible(divBassLabel_);
  styleLabel(divBassLabel_, TRANS("Bass"));
  divBright_.onValueChange = divBass_.onValueChange = [this] { pushDivision(); };
  // Pan from full left to full right, width from mono to twice as wide.
  for (auto* s : {&pan_, &divPan_}) {
    addAndMakeVisible(*s);
    s->setRange(-100.0, 100.0, 1.0);
    s->setValue(0.0, juce::dontSendNotification);
    styleFader(*s, fader::evenTicks(-100.0, 100.0), 0.0);
  }
  for (auto* s : {&width_, &divWidth_}) {
    addAndMakeVisible(*s);
    s->setRange(0.0, 200.0, 1.0);
    s->setTextValueSuffix(" %");
    s->setValue(100.0, juce::dontSendNotification);
    styleFader(*s, fader::evenTicks(0.0, 200.0), 100.0);
  }
  addAndMakeVisible(panLabel_);
  styleLabel(panLabel_, TRANS("Pan"));
  addAndMakeVisible(widthLabel_);
  styleLabel(widthLabel_, TRANS("Width"));
  addAndMakeVisible(divPanLabel_);
  styleLabel(divPanLabel_, TRANS("Pan"));
  addAndMakeVisible(divWidthLabel_);
  styleLabel(divWidthLabel_, TRANS("Width"));
  pan_.setTooltip(TRANS("Where the rank stands, from full left to full right: heard at once"));
  width_.setTooltip(TRANS("The rank's stereo width: 0% mono, 100% as recorded, 200% twice as wide"));
  pan_.onValueChange = [this] { pushCurrent(kVoicingPan); };
  width_.onValueChange = [this] { pushCurrent(kVoicingWidth); };
  divPan_.onValueChange = divWidth_.onValueChange = [this] { pushDivision(); };
  // Upright, the value under each fader and the name over it.
  for (auto* s : {&gain_, &tune_, &bright_, &bass_, &pan_, &width_, &divBright_, &divBass_, &divPan_, &divWidth_,
                  &rankFader_})
    s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, kVoicingFaderW - 4, 20);
  for (auto* l : {&gainLabel_, &tuneLabel_, &brightLabel_, &bassLabel_, &panLabel_, &widthLabel_, &divBrightLabel_,
                  &divBassLabel_, &divPanLabel_, &divWidthLabel_, &rankFaderLabel_})
    l->setJustificationType(juce::Justification::centred);
  // A screen reader names each fader as its label does.
  gain_.setTitle(gainLabel_.getText());
  tune_.setTitle(tuneLabel_.getText());
  bright_.setTitle(brightLabel_.getText());
  bass_.setTitle(bassLabel_.getText());
  pan_.setTitle(panLabel_.getText());
  width_.setTitle(widthLabel_.getText());
  divBright_.setTitle(divisionLabel_.getText() + ": " + divBrightLabel_.getText());
  divBass_.setTitle(divisionLabel_.getText() + ": " + divBassLabel_.getText());
  divPan_.setTitle(divisionLabel_.getText() + ": " + divPanLabel_.getText());
  divWidth_.setTitle(divisionLabel_.getText() + ": " + divWidthLabel_.getText());

  // Every pipe of the rank, on the page (#1188).
  addAndMakeVisible(pipesLabel_);
  styleLabel(pipesLabel_, TRANS("Every pipe of the rank"));
  addAndMakeVisible(deskFieldLabel_);
  styleLabel(deskFieldLabel_, TRANS("Shows"));
  addAndMakeVisible(deskField_);
  deskField_.addItem(TRANS("Level"), 1);
  deskField_.addItem(TRANS("Tuning"), 2);
  deskField_.setSelectedId(1, juce::dontSendNotification);
  deskField_.onChange = [this] {
    setDeskRange();
    refreshDesk();
  };
  addAndMakeVisible(rankFaderLabel_);
  styleLabel(rankFaderLabel_, TRANS("Rank"));
  rankFaderLabel_.setJustificationType(juce::Justification::centred);
  addAndMakeVisible(rankFader_);
  rankFader_.setTitle(TRANS("The whole rank"));
  rankFader_.setTooltip(TRANS("The rank's own level or tuning, which each pipe adds to its own"));
  rankFader_.setDoubleClickReturnValue(true, 0.0);
  rankFader_.onValueChange = [this] {
    VoicingTarget t = target();
    if (t.ranks.empty()) return;
    t.scope = VoicingScope::Rank;
    PipeVoicing v;
    setVoicingValue(v, deskField(), static_cast<float>(rankFader_.getValue()));
    {
      const MasterpieceProcessor::AudioLock audio(proc_);
      setVoicing(proc_.voicing().live(), proc_.organModel(), t, v,
                 deskField() == VoicingField::Level ? kVoicingLevel : kVoicingTuning);
    }
    proc_.voicingChanged();
    loadCurrentIntoSliders();
  };
  rankFader_.onDragEnd = [this] { proc_.restartTestNote(); };
  desk_->setTooltip(TRANS("Each pipe's own level or tuning, lowest note on the left. A click on a pipe's fader "
                          "chooses that pipe for the faders above; a drag sets it; a double-click puts it back."));
  desk_->onChoose = [this](int note) {
    if (scope_.getSelectedId() != 2) scope_.setSelectedId(2, juce::sendNotificationSync);
    if (static_cast<int>(note_.getValue()) != note) note_.setValue(note, juce::dontSendNotification);
    loadCurrentIntoSliders();
  };
  desk_->onSet = [this](int note, float value) {
    VoicingTarget t = target();
    if (t.ranks.empty()) return;
    t.scope = VoicingScope::Pipe;
    t.note = note;
    PipeVoicing v;
    setVoicingValue(v, deskField(), value);
    {
      const MasterpieceProcessor::AudioLock audio(proc_);
      setVoicing(proc_.voicing().live(), proc_.organModel(), t, v,
                 deskField() == VoicingField::Level ? kVoicingLevel : kVoicingTuning);
    }
    proc_.voicingChanged();
    loadCurrentIntoSliders();
  };
  desk_->onSetEnd = [this] { proc_.restartTestNote(); };
  deskView_.setViewedComponent(desk_.get(), false);
  deskView_.setScrollBarsShown(false, true);
  deskView_.setScrollBarThickness(10);
  addAndMakeVisible(deskView_);
  setDeskRange();

  addAndMakeVisible(abSwap_);
  abSwap_.onClick = [this] {
    proc_.voicing().swap();
    proc_.applyVoicingEq();
    loadCurrentIntoSliders();
    loadDivision();
    updateStatus();
  };
  addAndMakeVisible(abCopy_);
  abCopy_.onClick = [this] {
    proc_.voicing().copyToOther();
    updateStatus();
  };
  addAndMakeVisible(resetOne_);
  resetOne_.onClick = [this] {
    for (auto* s : {&gain_, &tune_, &bright_, &bass_, &pan_}) s->setValue(0.0, juce::dontSendNotification);
    width_.setValue(100.0, juce::dontSendNotification);
    pushCurrent(kVoicingLevel | kVoicingTuning | kVoicingBrightness | kVoicingBass | kVoicingPan | kVoicingWidth);
  };
  addAndMakeVisible(resetAll_);
  resetAll_.onClick = [this] {
    proc_.voicing().live().clear();
    proc_.applyVoicingEq();
    loadCurrentIntoSliders();
    loadDivision();
    updateStatus();
  };
  addAndMakeVisible(save_);
  save_.onClick = [this] { proc_.saveSettings(); };

  addAndMakeVisible(status_);
  styleLabel(status_, "");
  addAndMakeVisible(note2_);
  styleNote(note2_,
            TRANS("A rank adjustment and a pipe adjustment ADD. Pulling one sour "
                  "pipe into tune does not throw away the trim you put on the rank "
                  "it belongs to.\n\n"
                  "A and B are two complete sets. Make a change, swap, and hear it "
                  "against what was there before - from memory the comparison "
                  "always flatters whichever you heard last. Copy to other starts "
                  "the far slot from this one, so B is a variation rather than a "
                  "comparison against an unvoiced organ.\n\n"
                  "Level and tuning are a multiply and a ratio taken once when a "
                  "note starts, so they cost nothing while it sounds and work with "
                  "the DSP switched off. Notes already sounding keep what they "
                  "began with, exactly as a pipe does.\n\n"
                  "Saved per organ: a rank number means nothing in a different "
                  "instrument."));
  refresh();
}

void VoicingPanel::refresh() {
  rankIds_.clear();
  for (const auto& [id, r] : proc_.organModel().ranks) rankIds_.push_back(id);
  std::sort(rankIds_.begin(), rankIds_.end());
  // A windchest is offered on an organ that has two or more.
  const int by = chooseBy_.getSelectedId();
  chooseBy_.clear(juce::dontSendNotification);
  chooseBy_.addItem(TRANS("One rank"), 1);
  chooseBy_.addItem(TRANS("Ranks of a stop"), 2);
  chooseBy_.addItem(TRANS("Ranks of a division"), 3);
  if (!windchestRanks(proc_.organModel()).empty()) chooseBy_.addItem(TRANS("Ranks of a windchest"), 4);
  chooseBy_.setSelectedId(by >= 1 && by <= chooseBy_.getNumItems() ? by : 1, juce::dontSendNotification);
  fillChoices();
  divisionIds_.clear();
  division_.clear(juce::dontSendNotification);
  for (const auto& [id, d] : proc_.organModel().divisions) divisionIds_.push_back(id);
  std::sort(divisionIds_.begin(), divisionIds_.end());
  for (size_t i = 0; i < divisionIds_.size(); ++i) {
    const auto& d = proc_.organModel().divisions.at(divisionIds_[i]);
    division_.addItem(d.name.empty() ? juce::String(static_cast<int>(divisionIds_[i])) : juce::String(d.name),
                      static_cast<int>(i) + 1);
  }
  if (!divisionIds_.empty()) division_.setSelectedId(1, juce::dontSendNotification);
  loadCurrentIntoSliders();
  loadDivision();
  updateStatus();
}

void VoicingPanel::loadDivision() {
  const int idx = division_.getSelectedId() - 1;
  const bool any = idx >= 0 && idx < static_cast<int>(divisionIds_.size());
  for (auto* s : {&divBright_, &divBass_, &divPan_, &divWidth_}) s->setEnabled(any);
  const PipeVoicing pv = any ? proc_.voicing().live().division(divisionIds_[static_cast<size_t>(idx)]) : PipeVoicing{};
  divBright_.setValue(pv.brightnessDb, juce::dontSendNotification);
  divBass_.setValue(pv.bassDb, juce::dontSendNotification);
  divPan_.setValue(pv.balance * 100.0, juce::dontSendNotification);
  divWidth_.setValue(pv.width * 100.0, juce::dontSendNotification);
}

void VoicingPanel::pushDivision() {
  const int idx = division_.getSelectedId() - 1;
  if (idx < 0 || idx >= static_cast<int>(divisionIds_.size())) return;
  const Id id = divisionIds_[static_cast<size_t>(idx)];
  PipeVoicing pv = proc_.voicing().live().division(id);
  pv.brightnessDb = static_cast<float>(divBright_.getValue());
  pv.bassDb = static_cast<float>(divBass_.getValue());
  pv.balance = static_cast<float>(divPan_.getValue() / 100.0);
  pv.width = static_cast<float>(divWidth_.getValue() / 100.0);
  proc_.voicing().live().setDivision(id, pv);
  proc_.applyVoicingEq();
  updateStatus();
}

// The items of the second box for what the first has chosen: every rank,
// every stop with its division's name, every division, or every windchest,
// each with the ranks it reaches. An item that reaches no rank is left out.
void VoicingPanel::fillChoices() {
  const auto& m = proc_.organModel();
  rank_.clear(juce::dontSendNotification);
  choiceRanks_.clear();
  const auto add = [this](const juce::String& name, std::vector<Id> ranks) {
    if (ranks.empty()) return;
    choiceRanks_.push_back(std::move(ranks));
    rank_.addItem(name, static_cast<int>(choiceRanks_.size()));
  };
  const auto divisionName = [&m](Id id) {
    const auto it = m.divisions.find(id);
    return it == m.divisions.end() ? juce::String() : juce::String(it->second.name);
  };
  switch (chooseBy_.getSelectedId()) {
    case 2: {
      std::vector<Id> stops;
      for (const auto& [id, s] : m.stops) stops.push_back(id);
      std::sort(stops.begin(), stops.end());
      for (const Id id : stops) {
        const auto& s = m.stops.at(id);
        const auto division = divisionName(s.divisionId);
        add((division.isNotEmpty() ? division + ": " : juce::String()) + juce::String(s.name), ranksOfStop(m, id));
      }
      break;
    }
    case 3: {
      std::vector<Id> divisions;
      for (const auto& [id, d] : m.divisions) divisions.push_back(id);
      std::sort(divisions.begin(), divisions.end());
      for (const Id id : divisions) {
        const auto name = divisionName(id);
        add(name.isNotEmpty() ? name : juce::String(static_cast<int>(id)), ranksOfDivision(m, id));
      }
      break;
    }
    case 4:
      for (auto& [name, ranks] : windchestRanks(m)) add(juce::String(name), ranks);
      break;
    default:
      for (const Id id : rankIds_) add(rankTitle(m, id), {id});
      break;
  }
  if (!choiceRanks_.empty()) rank_.setSelectedId(1, juce::dontSendNotification);
}

VoicingTarget VoicingPanel::target() const {
  VoicingTarget t;
  const int idx = rank_.getSelectedId() - 1;
  if (idx >= 0 && idx < static_cast<int>(choiceRanks_.size())) t.ranks = choiceRanks_[static_cast<size_t>(idx)];
  switch (scope_.getSelectedId()) {
    case 2: t.scope = VoicingScope::Pipe; break;
    case 3: t.scope = VoicingScope::CSide; break;
    case 4: t.scope = VoicingScope::CSharpSide; break;
    default: t.scope = VoicingScope::Rank; break;
  }
  t.note = static_cast<int>(note_.getValue());
  return t;
}

// A rank chosen elsewhere, in the view of every pipe: kept as it is when it
// is one of the ranks chosen here, or else chosen alone.
void VoicingPanel::chooseRank(Id rankId) {
  const int idx = rank_.getSelectedId() - 1;
  if (idx >= 0 && idx < static_cast<int>(choiceRanks_.size())) {
    const auto& ranks = choiceRanks_[static_cast<size_t>(idx)];
    if (std::find(ranks.begin(), ranks.end(), rankId) != ranks.end()) return;
  }
  if (chooseBy_.getSelectedId() != 1) {
    chooseBy_.setSelectedId(1, juce::dontSendNotification);
    fillChoices();
  }
  for (size_t i = 0; i < choiceRanks_.size(); ++i)
    if (choiceRanks_[i].size() == 1 && choiceRanks_[i].front() == rankId)
      rank_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
}

void VoicingPanel::loadCurrentIntoSliders() {
  const auto t = target();
  if (t.ranks.empty()) return;
  const PipeVoicing pv = shownVoicing(proc_.voicing().live(), proc_.organModel(), t);
  pushTarget();
  // dontSendNotification: these are being loaded FROM the model, and letting
  // them call back would write them straight back again, turning a read into
  // a write and a swap into a wipe.
  gain_.setValue(pv.gainDb, juce::dontSendNotification);
  tune_.setValue(pv.tuningCents, juce::dontSendNotification);
  // The shelves are a rank's or a pipe's own (#1188); the pan and the width
  // are a rank's.
  const bool rankScope = t.scope == VoicingScope::Rank;
  bright_.setValue(pv.brightnessDb, juce::dontSendNotification);
  bass_.setValue(pv.bassDb, juce::dontSendNotification);
  bright_.setTooltip(rankScope ? TRANS("The rank's treble, above 3 kHz: heard at once, on the pipes sounding too")
                               : TRANS("The pipe's own treble, above 3 kHz, on top of its rank's: heard from its next note"));
  bass_.setTooltip(rankScope ? TRANS("The rank's bass, below 200 Hz: heard at once, on the pipes sounding too")
                             : TRANS("The pipe's own bass, below 200 Hz, on top of its rank's: heard from its next note"));
  pan_.setValue(rankScope ? pv.balance * 100.0 : 0.0, juce::dontSendNotification);
  width_.setValue(rankScope ? pv.width * 100.0 : 100.0, juce::dontSendNotification);
  for (auto* c : std::initializer_list<juce::Component*>{&pan_, &width_, &panLabel_, &widthLabel_})
    c->setEnabled(rankScope);
  refreshDesk();
  updateStatus();
}

void VoicingPanel::pushCurrent(unsigned which) {
  const auto t = target();
  if (t.ranks.empty()) return;
  const auto& model = proc_.organModel();
  if (t.scope != VoicingScope::Rank && pipesOf(model, t).empty()) {
    status_.setText(t.scope == VoicingScope::Pipe ? TRANS("This rank has no pipe at that note")
                                                  : TRANS("No pipe is chosen"),
                    juce::dontSendNotification);
    return;
  }
  PipeVoicing v;
  v.gainDb = static_cast<float>(gain_.getValue());
  v.tuningCents = static_cast<float>(tune_.getValue());
  v.brightnessDb = static_cast<float>(bright_.getValue());
  v.bassDb = static_cast<float>(bass_.getValue());
  v.balance = static_cast<float>(pan_.getValue() / 100.0);
  v.width = static_cast<float>(width_.getValue() / 100.0);
  {
    const MasterpieceProcessor::AudioLock audio(proc_);
    setVoicing(proc_.voicing().live(), model, t, v, which);
  }
  if (t.scope == VoicingScope::Rank && (which & (kVoicingBrightness | kVoicingBass | kVoicingPan | kVoicingWidth)))
    proc_.applyVoicingEq();
  proc_.voicingChanged();
  if (!juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown()) proc_.restartTestNote();
  refreshDesk();
  updateStatus();
}

void VoicingPanel::pushTarget() {
  const auto t = target();
  if (t == proc_.voicingTarget()) return;
  proc_.setVoicingTarget(t);
  // Another rank chosen: the test note sounds it at the same key.
  proc_.restartTestNote();
}

VoicingField VoicingPanel::deskField() const {
  return deskField_.getSelectedId() == 2 ? VoicingField::Tuning : VoicingField::Level;
}

void VoicingPanel::setDeskRange() {
  const bool level = deskField() == VoicingField::Level;
  const double r = level ? 12.0 : 50.0;
  rankFader_.setRange(-r, r, 0.1);
  rankFader_.setTextValueSuffix(level ? " dB" : " cents");
  styleFader(rankFader_, {-r, -r / 2, 0.0, r / 2, r}, 0.0, fader::kDivision);
  desk_->setField(deskField());
}

// The pipes of the first rank chosen, as the faders above show the first; a
// pipe's fader moves the pipe at that note in every rank chosen.
void VoicingPanel::refreshDesk() {
  const auto t = target();
  const auto& model = proc_.organModel();
  const auto& set = proc_.voicing().live();
  const VoicingField field = deskField();
  std::vector<int> notes;
  std::map<int, Id> pipeAt;
  if (!t.ranks.empty())
    if (const auto it = model.ranks.find(t.ranks.front()); it != model.ranks.end())
      for (const auto& pipe : it->second.pipes)
        if (pipeAt.emplace(pipe.midiNote, pipe.pipeId).second) notes.push_back(pipe.midiNote);
  std::sort(notes.begin(), notes.end());
  desk_->setNotes(notes);
  desk_->setValues([&](int note) { return voicingValue(set.pipe(pipeAt.at(note)), field); });
  const float rankValue = t.ranks.empty() ? 0.0f : voicingValue(set.rank(t.ranks.front()), field);
  if (rankFader_.getValue() != rankValue) rankFader_.setValue(rankValue, juce::dontSendNotification);
  rankFader_.setEnabled(!t.ranks.empty());
  const int chosen = t.scope == VoicingScope::Pipe ? t.note : -1;
  desk_->setChosen(chosen);
  // The pipe chosen with a key or the note box is brought into view.
  if (chosen >= 0) {
    const auto b = desk_->faderBounds(chosen);
    const int x = deskView_.getViewPositionX();
    if (!b.isEmpty() && (b.getX() < x || b.getRight() > x + deskView_.getViewWidth()))
      deskView_.setViewPosition(std::max(0, b.getCentreX() - deskView_.getViewWidth() / 2), 0);
  }
}

void VoicingPanel::showEveryPipe() {
  const auto t = target();
  const Id rankId = t.ranks.empty() ? 0 : t.ranks.front();
  // The pipe these sliders work is the one the view opens on.
  const auto showPipe = [&t](RankPipesContent& content) {
    if (t.scope == VoicingScope::Pipe) content.selectNote(t.note);
  };
  if (everyPipeWindow_ != nullptr) {
    if (auto* content = dynamic_cast<RankPipesContent*>(everyPipeWindow_->getContentComponent())) {
      content->selectRank(rankId);
      showPipe(*content);
    }
    everyPipeWindow_->toFront(true);
    return;
  }
  juce::Component::SafePointer<VoicingPanel> self(this);
  auto content = std::make_unique<RankPipesContent>(
      proc_, rankId,
      [self] {
        if (self != nullptr) self->loadCurrentIntoSliders();
      },
      // A pipe chosen there is the pipe these sliders work.
      [self](Id rank, int note) {
        if (self == nullptr) return;
        self->chooseRank(rank);
        if (self->scope_.getSelectedId() != 2) self->scope_.setSelectedId(2, juce::sendNotificationSync);
        self->note_.setValue(note, juce::dontSendNotification);
        self->loadCurrentIntoSliders();
      });
  showPipe(*content);
  content->setSize(980, 440);
  juce::DialogWindow::LaunchOptions o;
  o.content.setOwned(content.release());
  o.dialogTitle = TRANS("Voicing: every pipe");
  o.dialogBackgroundColour = juce::Colour(0xff1b1e24);
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = true;
  // Beside the console: the keys and the stops stay in reach while voicing.
  everyPipeWindow_ = launchDialog(o, false);
}

// The four steps as console buttons; the mapping is the console's, the same
// on every organ, and acts on whatever this tab has chosen.
void VoicingPanel::midiMenu() {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    proc_.cancelLearn();
    return;
  }
  struct Target {
    MidiTargetKind kind;
    juce::String name;
  };
  const std::vector<Target> targets{{MidiTargetKind::VoicingLevelDown, TRANS("Voicing: level -")},
                                    {MidiTargetKind::VoicingLevelUp, TRANS("Voicing: level +")},
                                    {MidiTargetKind::VoicingTuneDown, TRANS("Voicing: tuning -")},
                                    {MidiTargetKind::VoicingTuneUp, TRANS("Voicing: tuning +")}};
  juce::PopupMenu menu;
  int item = 1;
  for (const auto& t : targets) {
    juce::PopupMenu sub;
    const auto existing = map.bindingsFor(t.kind, 0);
    if (existing.empty()) sub.addSectionHeader(TRANS("Not mapped"));
    for (const MidiBinding* b : existing) sub.addSectionHeader(MidiEventDialog::describeBinding(*b));
    sub.addItem(item, TRANS("Learn"));
    sub.addItem(item + 1, TRANS("MIDI window..."));
    sub.addItem(item + 2, TRANS("Clear mapping"), !existing.empty());
    menu.addSubMenu(t.name, sub);
    item += 10;
  }
  juce::Component::SafePointer<VoicingPanel> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&midi_), [self, targets](int choice) {
    if (self == nullptr || choice <= 0) return;
    const auto& t = targets[static_cast<size_t>((choice - 1) / 10)];
    switch ((choice - 1) % 10) {
      case 0:
        self->proc_.beginLearnAs(t.kind, 0, MidiTrigger::Momentary);
        break;
      case 1:
        MidiEventDialog::show(self->proc_, t.kind, 0, t.name);
        break;
      case 2:
        self->proc_.editMidiMap([&](MidiMap& map) { map.unbindTarget(t.kind, 0); });
        self->proc_.saveMidiMap();
        break;
      default:
        break;
    }
  });
}

// A step from the console shows on the sliders; a test note played in one
// pipe's scope moves the note to the key played, unless the keys are left to
// play its neighbours.
void VoicingPanel::timerCallback() {
  // Runs from construction: a window opened straight on this tab shows the
  // panel before the window reaches the screen, and no visibilityChanged
  // follows when it does (#1068).
  if (!isShowing()) return;
  if (const uint32_t steps = proc_.voicingSteps(); steps != shownSteps_) {
    shownSteps_ = steps;
    loadCurrentIntoSliders();
  }
  // Any key played, with the test note or without it (#1188).
  if (const uint32_t keys = proc_.keysPressed(); keys != shownKeys_) {
    shownKeys_ = keys;
    const int key = proc_.lastKeyPressed();
    if (key >= 0 && scope_.getSelectedId() == 2 && keysChoose_.getToggleState() &&
        static_cast<int>(note_.getValue()) != key)
      note_.setValue(key, juce::sendNotificationSync);
  }
}

void VoicingPanel::visibilityChanged() {
  if (isShowing()) {
    shownSteps_ = proc_.voicingSteps();
    return;
  }
  // Away from the tab, the keys play the organ again.
  if (testNote_.getToggleState()) {
    testNote_.setToggleState(false, juce::dontSendNotification);
    proc_.setTestNote(false);
  }
}

VoicingPanel::~VoicingPanel() {
  stopTimer();
  proc_.setTestNote(false);
}

void VoicingPanel::updateStatus() {
  const auto& v = proc_.voicing().live();
  juce::String s;
  // What a change here reaches, when it is more than one rank or one pipe.
  const auto t = target();
  if (t.scope == VoicingScope::Rank && t.ranks.size() > 1)
    s << TRANS("Changes %1 ranks").replace("%1", juce::String(static_cast<int>(t.ranks.size()))) << "  -  ";
  else if (t.scope == VoicingScope::CSide || t.scope == VoicingScope::CSharpSide)
    s << TRANS("Changes %1 pipes")
             .replace("%1", juce::String(static_cast<int>(pipesOf(proc_.organModel(), t).size())))
      << "  -  ";
  s << TRANS("Slot %1  -  %2 rank(s), %3 pipe(s) adjusted")
           .replace("%1", proc_.voicing().usingB ? "B" : "A")
           .replace("%2", juce::String(static_cast<int>(v.ranks().size())))
           .replace("%3", juce::String(static_cast<int>(v.pipes().size())));
  // Voicing is heard on the NEXT note, like a real pipe being touched while
  // another is sounding. Saying so stops it looking broken.
  s << "  -  " << TRANS("takes effect on the next note");
  status_.setText(s, juce::dontSendNotification);
}

void VoicingPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  const int labelW = 110;
  heading_.setBounds(r.removeFromTop(kRow));

  auto row = r.removeFromTop(kRow);
  chooseBy_.setBounds(row.removeFromLeft(170).reduced(0, 1));
  row.removeFromLeft(kGap);
  rank_.setBounds(row.removeFromLeft(280).reduced(0, 1));
  midi_.setBounds(row.removeFromRight(70).reduced(2, 1));
  everyPipe_.setBounds(row.removeFromRight(120).reduced(2, 1));
  row.removeFromLeft(kGap);
  testNote_.setBounds(row.removeFromLeft(120));
  r.removeFromTop(4);

  row = r.removeFromTop(kRow);
  scopeLabel_.setBounds(row.removeFromLeft(labelW));
  scope_.setBounds(row.removeFromLeft(180).reduced(0, 1));
  row.removeFromLeft(kGap);
  noteLabel_.setBounds(row.removeFromLeft(80));
  note_.setBounds(row.removeFromLeft(110).reduced(0, 1));
  row.removeFromLeft(kGap);
  keysChoose_.setBounds(row.removeFromLeft(190));
  r.removeFromTop(kGap);

  // The desk: the faders for what is chosen above, then the division's, then
  // the buttons in a column. Each fader one size, left to right (#1188).
  const int fw = kVoicingFaderW;
  auto desk = r.removeFromTop(kRow + 18 + kVoicingFaderH);
  const auto place = [&](juce::Rectangle<int>& area, juce::Label& label, juce::Slider& s) {
    auto col = area.removeFromLeft(fw);
    label.setBounds(col.removeFromTop(18));
    s.setBounds(col.reduced(4, 0));
  };
  {
    auto group = desk.removeFromLeft(fw * 6);
    group.removeFromTop(kRow);
    for (auto [label, s] : std::initializer_list<std::pair<juce::Label*, juce::Slider*>>{
             {&gainLabel_, &gain_}, {&tuneLabel_, &tune_}, {&brightLabel_, &bright_},
             {&bassLabel_, &bass_}, {&panLabel_, &pan_}, {&widthLabel_, &width_}})
      place(group, *label, *s);
  }
  desk.removeFromLeft(kGap * 4);
  {
    auto group = desk.removeFromLeft(fw * 4);
    auto head = group.removeFromTop(kRow);
    divisionLabel_.setBounds(head.removeFromLeft(90));
    division_.setBounds(head.reduced(0, 1));
    for (auto [label, s] : std::initializer_list<std::pair<juce::Label*, juce::Slider*>>{
             {&divBrightLabel_, &divBright_}, {&divBassLabel_, &divBass_},
             {&divPanLabel_, &divPan_}, {&divWidthLabel_, &divWidth_}})
      place(group, *label, *s);
  }
  desk.removeFromLeft(kGap * 4);
  {
    auto buttons = desk.removeFromLeft(170);
    buttons.removeFromTop(kRow);
    for (auto* b : {&abSwap_, &abCopy_, &resetOne_, &resetAll_, &save_}) {
      b->setBounds(buttons.removeFromTop(kRow + 4).reduced(2, 2));
    }
  }
  r.removeFromTop(kGap);

  // Every pipe: the rank's fader, then the pipes, scrolled when they outgrow
  // the width.
  row = r.removeFromTop(kRow);
  pipesLabel_.setBounds(row.removeFromLeft(220));
  deskFieldLabel_.setBounds(row.removeFromLeft(56));
  deskField_.setBounds(row.removeFromLeft(120).reduced(0, 1));
  status_.setBounds(r.removeFromBottom(kRow));
  auto pipes = r.removeFromTop(std::min(r.getHeight(), 190));
  // The rank's track and the pipes' tracks level with each other: the
  // rank's value box stands where the pipes' names do.
  const int scrollBar = deskView_.getScrollBarThickness();
  {
    auto col = pipes.removeFromLeft(fw);
    rankFaderLabel_.setBounds(col.removeFromTop(18));
    rankFader_.setBounds(col.withTrimmedBottom(scrollBar).reduced(4, 0));
  }
  pipes.removeFromLeft(kGap);
  deskView_.setBounds(pipes.withTrimmedTop(18));
  desk_->setSize(desk_->widthNeeded(), std::max(60, deskView_.getHeight() - scrollBar));
  r.removeFromTop(kGap);
  note2_.setBounds(r);
}

// ----------------------------------------------------------------- mixer

namespace {
// An output as the player names it, or by its number.
juce::String outputName(const MixerConfig& mixer, int busId) {
  for (const auto& b : mixer.buses)
    if (b.id.value == busId && !b.name.empty()) return juce::String::fromUTF8(b.name.c_str());
  return TRANS("Output %1").replace("%1", juce::String(busId));
}

// One output's EQ (#1188): the ten bands of the organ's voicing EQ, with
// their frequencies and widths, and a gain, for the speaker on that output.
// The curve over the faders is the EQ's, with the output's crossover filters
// in it.
// A fader moved is heard at once and turns the EQ on; off or flat, the
// output is not filtered.
class OutputEqPanel final : public juce::Component {
public:
  OutputEqPanel(MasterpieceProcessor& p, int busId) : proc_(p) {
    addAndMakeVisible(heading_);
    styleLabel(heading_, TRANS("Output EQ"));
    addAndMakeVisible(output_);
    output_.setTooltip(TRANS("The output whose EQ is shown"));
    for (const auto& b : proc_.mixer().buses) output_.addItem(outputName(proc_.mixer(), b.id.value), b.id.value);
    output_.setSelectedId(busId > 0 ? busId : 1, juce::dontSendNotification);
    output_.onChange = [this] { showValues(); };
    addAndMakeVisible(on_);
    on_.setTooltip(TRANS("A correction for the speaker on this output, after what it sends to the other outputs"));
    on_.onClick = [this] { apply(); };
    addAndMakeVisible(flat_);
    flat_.setTooltip(TRANS("Every band back to 0 dB, at the frequency and width it came with"));
    flat_.onClick = [this] {
      const bool on = on_.getToggleState();
      VoicingEqSettings eq;
      eq.on = on;
      show(eq);
      apply();
    };
    auto style = [this](juce::Slider& sl, juce::Colour colour) {
      sl.setSliderStyle(juce::Slider::LinearVertical);
      sl.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 18);
      sl.setRange(-VoicingEqSettings::kMaxDb, VoicingEqSettings::kMaxDb, 0.5);
      sl.setDoubleClickReturnValue(true, 0.0);
      sl.setTextValueSuffix(" dB");
      const double m = VoicingEqSettings::kMaxDb;
      styleFader(sl, {-m, -m * 0.5, 0.0, m * 0.5, m}, 0.0, colour);
      sl.onValueChange = [this] {
        on_.setToggleState(true, juce::dontSendNotification);
        apply();
      };
      addAndMakeVisible(sl);
    };
    auto knob = [this](juce::Slider& k, double lo, double hi, double mid, double home, const juce::String& tip) {
      k.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
      k.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 16);
      k.setLookAndFeel(&FaderLook::shared());
      k.setColour(juce::Slider::thumbColourId, fader::kGroup);
      k.setRange(lo, hi, 0.0);
      k.setSkewFactorFromMidPoint(mid);
      k.setDoubleClickReturnValue(true, home);
      k.setTooltip(tip);
      k.onValueChange = [this] {
        on_.setToggleState(true, juce::dontSendNotification);
        apply();
      };
      addAndMakeVisible(k);
    };
    for (size_t i = 0; i < static_cast<size_t>(VoicingEqSettings::kBands); ++i) {
      style(bands_[i], fader::kChannel);
      auto& f = hz_[i];
      knob(f, VoicingEqSettings::kMinHz, VoicingEqSettings::kMaxHz, 632.0, VoicingEqSettings::kFrequencies[i],
           TRANS("The band's frequency. Double-click for the frequency it came with."));
      f.textFromValueFunction = [](double v) {
        if (v >= 1000.0)
          return juce::String(v / 1000.0, v >= 10000.0 ? 1 : 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + "k";
        return juce::String(v, v < 100.0 ? 1 : 0).upToFirstOccurrenceOf(".0", false, false);
      };
      f.valueFromTextFunction = [](const juce::String& t) {
        const auto v = t.trim().toLowerCase().removeCharacters(" hz");
        return v.endsWithChar('k') ? v.dropLastCharacters(1).getDoubleValue() * 1000.0 : v.getDoubleValue();
      };
      auto& w = q_[i];
      knob(w, VoicingEqSettings::kMinQ, VoicingEqSettings::kMaxQ, VoicingEqSettings::kDefaultQ,
           VoicingEqSettings::kDefaultQ,
           i == 0 || i + 1 == static_cast<size_t>(VoicingEqSettings::kBands)
               ? TRANS("The shelf's slope as a Q: 1.4 the steepest, less a gentler slope. Double-click for 1.4.")
               : TRANS("The band's width as a Q: more is narrower. Double-click for 1.4."));
      w.textFromValueFunction = [](double v) { return "Q " + juce::String(v, 1); };
      w.valueFromTextFunction = [](const juce::String& t) {
        return t.trim().toLowerCase().removeCharacters("q ").getDoubleValue();
      };
    }
    style(gain_, fader::kMaster);
    addAndMakeVisible(gainName_);
    styleLabel(gainName_, TRANS("Gain"));
    gainName_.setJustificationType(juce::Justification::centred);
    showValues();
    setSize(720, 470);
  }
  ~OutputEqPanel() override {
    for (auto& k : hz_) k.setLookAndFeel(nullptr);
    for (auto& k : q_) k.setLookAndFeel(nullptr);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12);
    auto top = r.removeFromTop(kRow);
    heading_.setBounds(top.removeFromLeft(100));
    output_.setBounds(top.removeFromLeft(200).reduced(0, 1));
    top.removeFromLeft(kGap);
    on_.setBounds(top.removeFromLeft(70));
    flat_.setBounds(top.removeFromRight(80).reduced(0, 1));
    r.removeFromTop(kGap);
    curveArea_ = r.removeFromTop(120);
    r.removeFromTop(kGap);
    const int columns = VoicingEqSettings::kBands + 1;
    const int w = r.getWidth() / columns;
    constexpr int kKnob = 40;
    auto knobs = r.removeFromTop(2 * kKnob + 4);
    for (int i = 0; i < columns; ++i) {
      auto col = r.withX(r.getX() + i * w).withWidth(w);
      auto head = knobs.withX(knobs.getX() + i * w).withWidth(w);
      if (i < VoicingEqSettings::kBands) {
        hz_[static_cast<size_t>(i)].setBounds(head.removeFromTop(kKnob).reduced(2, 0));
        head.removeFromTop(4);
        q_[static_cast<size_t>(i)].setBounds(head.removeFromTop(kKnob).reduced(2, 0));
        bands_[static_cast<size_t>(i)].setBounds(col.reduced(2, 0));
      } else {
        gainName_.setBounds(head.removeFromBottom(20));
        gain_.setBounds(col.reduced(2, 0));
      }
    }
  }

  // The EQ's curve on +-18 dB, 20 Hz to 20 kHz on a log scale, with the
  // output's crossover filters; grey while the EQ is off, the filters still
  // drawn.
  void paint(juce::Graphics& g) override {
    g.fillAll(juce::Colour(0xff15171c));
    const auto a = curveArea_.toFloat();
    g.setColour(juce::Colour(0xff1f232b));
    g.fillRoundedRectangle(a, 4.0f);
    const auto xOfHz = [&](double hz) { return a.getX() + a.getWidth() * static_cast<float>(std::log10(hz / 20.0) / 3.0); };
    const auto yOf = [&](double db) {
      return a.getBottom() - static_cast<float>(juce::jlimit(0.0, 1.0, (db + 18.0) / 36.0)) * a.getHeight();
    };
    g.setColour(juce::Colour(0xff2c313b));
    for (double hz : {100.0, 1000.0, 10000.0}) g.drawVerticalLine(juce::roundToInt(xOfHz(hz)), a.getY(), a.getBottom());
    for (double db : {-12.0, 12.0}) g.drawHorizontalLine(juce::roundToInt(yOf(db)), a.getX(), a.getRight());
    g.setColour(juce::Colour(0xff3a4150));
    g.drawHorizontalLine(juce::roundToInt(yOf(0.0)), a.getX(), a.getRight());
    g.setColour(juce::Colour(0xff6a7180));
    g.setFont(juce::FontOptions(11.0f));
    static const std::pair<double, const char*> kMarks[] = {{100.0, "100"}, {1000.0, "1k"}, {10000.0, "10k"}};
    for (const auto& [hz, text] : kMarks)
      g.drawText(text, juce::Rectangle<float>(xOfHz(hz) + 2.0f, a.getBottom() - 14.0f, 30.0f, 12.0f),
                 juce::Justification::left);
    VoicingEqSettings eq = shown();
    const bool on = eq.on;
    eq.on = true;
    const BusFilters* crossover = proc_.mixer().filtersFor(BusId{busId()});
    const int columns = juce::jmax(2, juce::roundToInt(a.getWidth()));
    juce::Path curve;
    for (int c = 0; c < columns; ++c) {
      const double hz = 20.0 * std::pow(1000.0, static_cast<double>(c) / (columns - 1));
      double db = on ? VoicingEq::responseDb(eq, hz) : 0.0;
      if (crossover != nullptr) db += OutputFilters::responseDb(*crossover, hz);
      const float x = a.getX() + a.getWidth() * static_cast<float>(c) / (columns - 1);
      if (c == 0) curve.startNewSubPath(x, yOf(db));
      else curve.lineTo(x, yOf(db));
    }
    g.setColour(on ? juce::Colour(0xffe8eaee) : juce::Colour(0xff8a91a0));
    g.strokePath(curve, juce::PathStrokeType(1.5f));
  }

private:
  int busId() const { return output_.getSelectedId() > 0 ? output_.getSelectedId() : 1; }
  VoicingEqSettings shown() const {
    VoicingEqSettings eq;
    eq.on = on_.getToggleState();
    for (size_t i = 0; i < bands_.size(); ++i) {
      eq.bandsDb[i] = bands_[i].getValue();
      eq.hz[i] = hz_[i].getValue();
      eq.q[i] = q_[i].getValue();
    }
    eq.gainDb = gain_.getValue();
    return eq;
  }
  void show(const VoicingEqSettings& eq) {
    on_.setToggleState(eq.on, juce::dontSendNotification);
    for (size_t i = 0; i < bands_.size(); ++i) {
      bands_[i].setValue(eq.bandsDb[i], juce::dontSendNotification);
      hz_[i].setValue(eq.hz[i], juce::dontSendNotification);
      q_[i].setValue(eq.q[i], juce::dontSendNotification);
    }
    gain_.setValue(eq.gainDb, juce::dontSendNotification);
    repaint();
  }
  void showValues() { show(proc_.busEq(busId())); }
  void apply() {
    proc_.setBusEq(busId(), shown());
    repaint(curveArea_);
  }

  MasterpieceProcessor& proc_;
  juce::Label heading_, gainName_;
  juce::ComboBox output_;
  juce::ToggleButton on_{TRANS("On")};
  juce::TextButton flat_{TRANS("Flat")};
  std::array<juce::Slider, VoicingEqSettings::kBands> bands_, hz_, q_;
  juce::Slider gain_;
  juce::Rectangle<int> curveArea_;
};

// The rows of a box that grows with the pairs, in a viewport that scrolls
// once they pass the height the screen allows.
constexpr int kPanelRowsMax = 470;

// The digital crossover (#1188): per output, a high-pass and a low-pass,
// each Butterworth or Linkwitz-Riley of 12 or 24 dB an octave, a delay and
// a polarity. Nothing decides the bands: the designer gives each output what
// its driver needs, and feeds the outputs of one speaker from the same
// output through its sends (Outputs...). Each change is heard at once.
class CrossoverPanel final : public juce::Component {
public:
  CrossoverPanel(MasterpieceProcessor& p, std::function<void()> changed) : onChanged_(std::move(changed)), proc_(p) {
    for (auto* l : {&heading_, &highHead_, &lowHead_, &delayHead_, &polarityHead_}) addAndMakeVisible(*l);
    styleLabel(heading_, TRANS("Crossover"));
    styleLabel(highHead_, TRANS("High-pass"));
    styleLabel(lowHead_, TRANS("Low-pass"));
    styleLabel(delayHead_, TRANS("Delay"));
    styleLabel(polarityHead_, TRANS("Polarity"));
    addAndMakeVisible(note_);
    styleLabel(note_, TRANS("A high-pass and a low-pass on one output make a band. Feed a speaker's outputs from one "
                            "output through its sends (Outputs...): with Linkwitz-Riley filters that meet at the same "
                            "frequency, the bands add up flat; at 12 dB an octave, invert one of the two."));
    note_.setColour(juce::Label::textColourId, juce::Colours::grey);
    note_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&holder_, false);
    viewport_.setScrollBarsShown(true, false);
    for (const auto& b : proc_.mixer().buses) addRow(b.id.value);
    const int rows = static_cast<int>(rows_.size()) * (kRow + 4);
    holder_.setSize(kWidth - 24, rows);
    setSize(kWidth, 12 + kRow + kGap + kRow + 4 + juce::jmin(kPanelRowsMax, juce::jmax(kRow, rows)) + kGap + 48);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12, 6);
    heading_.setBounds(r.removeFromTop(kRow));
    r.removeFromTop(kGap);
    note_.setBounds(r.removeFromBottom(48));
    r.removeFromBottom(kGap);
    auto head = r.removeFromTop(kRow);
    head.removeFromLeft(kName + kGap);
    highHead_.setBounds(head.removeFromLeft(kHz + kType + kGap));
    head.removeFromLeft(kGap * 2);
    lowHead_.setBounds(head.removeFromLeft(kHz + kType + kGap));
    head.removeFromLeft(kGap * 2);
    delayHead_.setBounds(head.removeFromLeft(kDelay));
    head.removeFromLeft(kGap);
    polarityHead_.setBounds(head);
    r.removeFromTop(4);
    viewport_.setBounds(r);
    holder_.setSize(juce::jmax(10, r.getWidth() - viewport_.getScrollBarThickness()), holder_.getHeight());
    int y = 0;
    for (auto& row : rows_) {
      auto line = juce::Rectangle<int>(0, y, holder_.getWidth(), kRow);
      y += kRow + 4;
      row->name.setBounds(line.removeFromLeft(kName));
      line.removeFromLeft(kGap);
      row->highHz.setBounds(line.removeFromLeft(kHz).reduced(0, 2));
      line.removeFromLeft(kGap);
      row->highType.setBounds(line.removeFromLeft(kType).reduced(0, 1));
      line.removeFromLeft(kGap * 2);
      row->lowHz.setBounds(line.removeFromLeft(kHz).reduced(0, 2));
      line.removeFromLeft(kGap);
      row->lowType.setBounds(line.removeFromLeft(kType).reduced(0, 1));
      line.removeFromLeft(kGap * 2);
      row->delay.setBounds(line.removeFromLeft(kDelay).reduced(0, 2));
      line.removeFromLeft(kGap);
      row->invert.setBounds(line.removeFromLeft(90));
    }
  }

private:
  static constexpr int kName = 120, kHz = 90, kType = 150, kDelay = 90;
  static constexpr int kWidth = 12 + kName + kGap + 2 * (kHz + kGap + kType + 2 * kGap) + kDelay + kGap + 90 + 24;
  struct Row {
    int busId = 0;
    juce::Label name;
    juce::Slider highHz, lowHz, delay;
    juce::ComboBox highType, lowType;
    juce::ToggleButton invert{TRANS("Invert")};
  };

  static void styleHz(juce::Slider& s, float now) {
    s.setSliderStyle(juce::Slider::LinearBar);
    s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, kRow - 4);
    // Under 20 Hz is off; the bar's left end.
    s.setRange(10.0, BusFilters::kMaxHz, 0.0);
    s.setSkewFactorFromMidPoint(632.0);
    s.setDoubleClickReturnValue(true, 10.0);
    s.textFromValueFunction = [](double v) {
      if (v < BusFilters::kMinHz) return TRANS("Off");
      if (v >= 1000.0) return juce::String(v / 1000.0, v >= 10000.0 ? 1 : 2) + " kHz";
      return juce::String(juce::roundToInt(v)) + " Hz";
    };
    s.valueFromTextFunction = [](const juce::String& t) {
      const auto v = t.trim().toLowerCase();
      if (v.isEmpty() || !v.containsAnyOf("0123456789")) return 10.0;
      const double n = v.getDoubleValue();
      return v.contains("k") ? n * 1000.0 : n;
    };
    s.setValue(now > 0.0f ? now : 10.0, juce::dontSendNotification);
    // The value may not have moved from where the range put it, and the box
    // shows its text in the format above only once asked.
    s.updateText();
    s.setChangeNotificationOnlyOnRelease(true);
    s.setTooltip(TRANS("The filter's frequency; at the left end, or a double-click, it is off. A value can be typed."));
  }
  static void styleType(juce::ComboBox& c, int now) {
    c.addItem(TRANS("Butterworth 12 dB"), BusFilters::Butterworth12 + 1);
    c.addItem(TRANS("Butterworth 24 dB"), BusFilters::Butterworth24 + 1);
    c.addItem(TRANS("Linkwitz-Riley 12 dB"), BusFilters::LinkwitzRiley12 + 1);
    c.addItem(TRANS("Linkwitz-Riley 24 dB"), BusFilters::LinkwitzRiley24 + 1);
    c.setSelectedId(now + 1, juce::dontSendNotification);
    c.setTooltip(TRANS("Butterworth: 3 dB down at the frequency, to keep the lowest or highest notes from a speaker. "
                       "Linkwitz-Riley: 6 dB down, so a low-pass and a high-pass at the same frequency add up flat. "
                       "12 or 24 dB less an octave."));
  }

  void addRow(int busId) {
    auto row = std::make_unique<Row>();
    row->busId = busId;
    const BusFilters now = proc_.busFilters(busId);
    styleLabel(row->name, outputName(proc_.mixer(), busId));
    styleHz(row->highHz, now.highPassHz);
    styleHz(row->lowHz, now.lowPassHz);
    styleType(row->highType, now.highPassType);
    styleType(row->lowType, now.lowPassType);
    row->delay.setSliderStyle(juce::Slider::LinearBar);
    row->delay.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 80, kRow - 4);
    row->delay.setRange(0.0, BusFilters::kMaxDelayMs, 0.01);
    row->delay.setSkewFactorFromMidPoint(10.0);
    row->delay.setTextValueSuffix(" ms");
    row->delay.setDoubleClickReturnValue(true, 0.0);
    row->delay.setValue(now.delayMs, juce::dontSendNotification);
    row->delay.updateText();
    row->delay.setChangeNotificationOnlyOnRelease(true);
    row->delay.setTooltip(TRANS("Holds the output back, to set drivers at different depths in time: 1 ms is 34 cm"));
    row->invert.setToggleState(now.invert, juce::dontSendNotification);
    row->invert.setTooltip(TRANS("Turns the output's polarity over"));
    Row* r = row.get();
    const auto apply = [this, r] {
      BusFilters f;
      f.highPassHz = r->highHz.getValue() < BusFilters::kMinHz ? 0.0f : static_cast<float>(r->highHz.getValue());
      f.highPassType = r->highType.getSelectedId() - 1;
      f.lowPassHz = r->lowHz.getValue() < BusFilters::kMinHz ? 0.0f : static_cast<float>(r->lowHz.getValue());
      f.lowPassType = r->lowType.getSelectedId() - 1;
      f.delayMs = static_cast<float>(r->delay.getValue());
      f.invert = r->invert.getToggleState();
      r->highType.setEnabled(f.highPassHz > 0.0f);
      r->lowType.setEnabled(f.lowPassHz > 0.0f);
      proc_.setBusFilters(r->busId, f);
      if (onChanged_) onChanged_();
    };
    r->highType.setEnabled(now.highPassHz > 0.0f);
    r->lowType.setEnabled(now.lowPassHz > 0.0f);
    for (auto* sl : {&r->highHz, &r->lowHz, &r->delay}) sl->onValueChange = apply;
    for (auto* c : {&r->highType, &r->lowType}) c->onChange = apply;
    r->invert.onClick = apply;
    for (juce::Component* c : std::initializer_list<juce::Component*>{&r->name, &r->highHz, &r->highType, &r->lowHz,
                                                                      &r->lowType, &r->delay, &r->invert})
      holder_.addAndMakeVisible(c);
    rows_.push_back(std::move(row));
  }

  std::function<void()> onChanged_;
  MasterpieceProcessor& proc_;
  juce::Label heading_, highHead_, lowHead_, delayHead_, polarityHead_, note_;
  juce::Viewport viewport_;
  juce::Component holder_;
  std::vector<std::unique_ptr<Row>> rows_;
};

// One block per speaker group: its outputs, ticked, and how its notes are
// spread over them. A group with no output ticked plays on output 1.
class SpeakerGroupsPanel final : public juce::Component {
public:
  SpeakerGroupsPanel(MasterpieceProcessor& p, std::function<void()> changed)
      : onChanged(std::move(changed)), proc_(p) {
    addAndMakeVisible(heading_);
    styleLabel(heading_, TRANS("Speaker groups"));
    addAndMakeVisible(note_);
    styleLabel(note_, TRANS("A rank sent to a speaker group sounds on its outputs, note by note."));
    note_.setColour(juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible(add_);
    add_.onClick = [this] {
      auto groups = proc_.mixer().groups;
      BusGroup g;
      for (const auto& other : groups) g.groupId = juce::jmax(g.groupId, other.groupId);
      ++g.groupId;
      groups.push_back(g);
      proc_.setSpeakerGroups(groups);
      rebuild();
      if (onChanged) onChanged();
    };
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&holder_, false);
    viewport_.setScrollBarsShown(true, false);
    rebuild();
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12, 6);
    heading_.setBounds(r.removeFromTop(kRow));
    note_.setBounds(r.removeFromTop(kRow));
    r.removeFromTop(4);
    add_.setBounds(r.removeFromBottom(kRow).removeFromLeft(200).reduced(0, 1));
    r.removeFromBottom(kGap);
    viewport_.setBounds(r);
    const int w = r.getWidth() - (holderHeight() > r.getHeight() ? viewport_.getScrollBarThickness() : 0);
    holder_.setSize(w, holderHeight());
    int y = 0;
    for (auto& g : groups_) {
      auto line = juce::Rectangle<int>(0, y, w, kRow);
      g.name->setBounds(line.removeFromLeft(130));
      g.method->setBounds(line.removeFromLeft(180).reduced(0, 1));
      line.removeFromLeft(kGap);
      g.remove->setBounds(line.removeFromLeft(90).reduced(0, 1));
      y += kRow + 2;
      for (size_t i = 0; i < g.pairs.size(); ++i) {
        const int col = static_cast<int>(i) % kPerLine;
        if (i > 0 && col == 0) y += kRow;
        g.pairs[i]->setBounds(col * kTick, y, kTick, kRow);
      }
      y += kRow + kGap + 6;
    }
  }

  std::function<void()> onChanged;

private:
  static constexpr int kPerLine = 12;
  static constexpr int kTick = 46;
  struct Group {
    int id = 0;
    std::unique_ptr<juce::Label> name;
    std::unique_ptr<juce::ComboBox> method;
    std::unique_ptr<juce::TextButton> remove;
    std::vector<std::unique_ptr<juce::ToggleButton>> pairs;
  };

  int pairCount() const { return juce::jmax(1, static_cast<int>(proc_.mixer().buses.size())); }
  int linesPerGroup() const { return 1 + (pairCount() + kPerLine - 1) / kPerLine; }
  int holderHeight() const {
    return static_cast<int>(groups_.size()) * (linesPerGroup() * kRow + kGap + 8);
  }

  void rebuild() {
    groups_.clear();
    holder_.removeAllChildren();
    for (const auto& bg : proc_.mixer().groups) {
      Group g;
      g.id = bg.groupId;
      g.name = std::make_unique<juce::Label>();
      styleLabel(*g.name, TRANS("Speaker group %1").replace("%1", juce::String(bg.groupId)));
      g.method = std::make_unique<juce::ComboBox>();
      g.method->addItem(TRANS("Across the octave"), 1);
      g.method->addItem(TRANS("Chromatic"), 2);
      g.method->addItem(TRANS("By stop"), 4);
      g.method->setSelectedId(bg.algorithm == AllocationAlgorithm::StaticChromatic ? 2
                              : bg.algorithm == AllocationAlgorithm::StaticByRank ? 4
                                                                                 : 1,
                              juce::dontSendNotification);
      g.method->setTooltip(TRANS("Across the octave: each note of an octave on the next output, each octave and each "
                                 "stop starting one further on. Chromatic: each note on the next output. By stop: each "
                                 "stop whole on one output, the stops taking the outputs in turn. A stop's perspectives, "
                                 "tremulant ranks and mixture ranks sound together on one output."));
      g.method->onChange = [this] { apply(); };
      g.remove = std::make_unique<juce::TextButton>(TRANS("Remove"));
      const int id = bg.groupId;
      g.remove->onClick = [this, id] {
        auto groups = proc_.mixer().groups;
        groups.erase(std::remove_if(groups.begin(), groups.end(), [id](const BusGroup& x) { return x.groupId == id; }),
                     groups.end());
        proc_.setSpeakerGroups(groups);
        // Later: the box holding this button goes in rebuild.
        juce::Component::SafePointer<SpeakerGroupsPanel> self(this);
        juce::MessageManager::callAsync([self] {
          if (self == nullptr) return;
          self->rebuild();
          if (self->onChanged) self->onChanged();
        });
      };
      for (int p = 1; p <= pairCount(); ++p) {
        auto t = std::make_unique<juce::ToggleButton>(juce::String(p));
        t->setTooltip(outputName(proc_.mixer(), p));
        t->setToggleState(std::find(bg.members.begin(), bg.members.end(), BusId{p}) != bg.members.end(),
                          juce::dontSendNotification);
        t->onClick = [this] { apply(); };
        holder_.addAndMakeVisible(*t);
        g.pairs.push_back(std::move(t));
      }
      for (juce::Component* c : {static_cast<juce::Component*>(g.name.get()), static_cast<juce::Component*>(g.method.get()),
                                 static_cast<juce::Component*>(g.remove.get())})
        holder_.addAndMakeVisible(*c);
      groups_.push_back(std::move(g));
    }
    const int rows = juce::jmin(kPanelRowsMax, juce::jmax(kRow, holderHeight()));
    setSize(kPerLine * kTick + 24 + 16, 12 + 2 * kRow + 4 + rows + kGap + kRow + 12);
    resized();
  }

  void apply() {
    std::vector<BusGroup> groups;
    for (const auto& g : groups_) {
      BusGroup bg;
      bg.groupId = g.id;
      const int m = g.method->getSelectedId();
      bg.algorithm = m == 2   ? AllocationAlgorithm::StaticChromatic
                     : m == 4 ? AllocationAlgorithm::StaticByRank
                              : AllocationAlgorithm::StaticOctaveCycled;
      for (size_t i = 0; i < g.pairs.size(); ++i)
        if (g.pairs[i]->getToggleState()) bg.members.push_back(BusId{static_cast<int>(i) + 1});
      groups.push_back(std::move(bg));
    }
    proc_.setSpeakerGroups(groups);
    if (onChanged) onChanged();
  }

  MasterpieceProcessor& proc_;
  juce::Label heading_, note_;
  juce::TextButton add_{TRANS("Add a speaker group")};
  juce::Viewport viewport_;
  juce::Component holder_;
  std::vector<Group> groups_;
};

// Device channels counted from 1, "1-2, 7-8" or "3, 4".
juce::String channelsText(const std::vector<int>& ch, bool mono) {
  juce::StringArray parts;
  for (size_t i = 0; i < ch.size(); ++i) {
    if (!mono && i + 1 < ch.size()) {
      parts.add(juce::String(ch[i] + 1) + (ch[i + 1] == ch[i] + 1 ? "-" : ", ") + juce::String(ch[i + 1] + 1));
      ++i;
    } else {
      parts.add(juce::String(ch[i] + 1));
    }
  }
  return parts.joinIntoString(", ");
}

// A destination box's items: the default, the outputs, then the speaker
// groups.
constexpr int kDestDefault = 900;
constexpr int kDestGroup = 1000;

juce::String destinationName(const MixerConfig& mixer, const std::variant<BusId, int>& dest) {
  if (const int* g = std::get_if<int>(&dest)) return TRANS("Speaker group %1").replace("%1", juce::String(*g));
  return outputName(mixer, std::get<BusId>(dest).value);
}

// compact: the short words a strip's narrow box can show.
void fillDestinationBox(juce::ComboBox& box, const MixerConfig& mixer, bool withDefault, bool compact = false) {
  box.clear(juce::dontSendNotification);
  if (withDefault)
    box.addItem(compact ? TRANS("Default")
                        : TRANS("The default: %1").replace("%1", destinationName(mixer, mixer.defaultDest)),
                kDestDefault);
  const int outputs = juce::jmax(1, static_cast<int>(mixer.buses.size()));
  for (int i = 1; i <= outputs; ++i) box.addItem(outputName(mixer, i), i);
  if (mixer.groups.empty()) return;
  box.addSectionHeading(TRANS("Speaker groups"));
  for (const auto& g : mixer.groups)
    box.addItem((compact ? TRANS("Group %1") : TRANS("Speaker group %1")).replace("%1", juce::String(g.groupId)),
                kDestGroup + g.groupId);
}

int destinationItemOf(const std::variant<BusId, int>& dest) {
  if (const int* g = std::get_if<int>(&dest)) return kDestGroup + *g;
  return juce::jmax(1, std::get<BusId>(dest).value);
}

// One row per output: its name, stereo or mono, its device channels and
// the outputs it also plays into. The output picked shows its channels and
// the other outputs as boxes to tick, under the list.
class OutputsPanel final : public juce::Component {
public:
  OutputsPanel(MasterpieceProcessor& p, std::function<void()> changed)
      : onChanged(std::move(changed)), proc_(p) {
    addAndMakeVisible(heading_);
    styleLabel(heading_, TRANS("Outputs"));
    addAndMakeVisible(note_);
    styleLabel(note_, TRANS("Pick an output, then tick its device channels and the outputs it also plays into. "
                            "Stereo takes the channels two at a time: the first two are the output, the rest copies."));
    note_.setColour(juce::Label::textColourId, juce::Colours::grey);
    note_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(allStereo_);
    allStereo_.onClick = [this] { layOut(false); };
    addAndMakeVisible(allMono_);
    allMono_.onClick = [this] { layOut(true); };
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&holder_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(channelsHeading_);
    addAndMakeVisible(sendsHeading_);
    for (auto* h : {&channelsHeading_, &sendsHeading_}) h->setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));

    for (const auto& b : proc_.mixer().buses) {
      Row row;
      row.busId = b.id.value;
      row.pick = std::make_unique<juce::ToggleButton>(TRANS("Output %1").replace("%1", juce::String(b.id.value)));
      row.pick->setRadioGroupId(1);
      row.pick->setClickingTogglesState(true);
      const int id = b.id.value;
      row.pick->onClick = [this, id] {
        if (selected_ != id) pick(id);
      };
      row.name = std::make_unique<juce::TextEditor>();
      row.name->setText(juce::String::fromUTF8(b.name.c_str()), false);
      row.name->setTextToShowWhenEmpty(TRANS("Name, for example: Gallery left"), juce::Colours::grey);
      row.name->setTooltip(TRANS("The output's name, on its strip and in every list"));
      auto* text = row.name.get();
      auto rename = [this, id, text] {
        proc_.setOutputName(id, text->getText().trim().toStdString());
        showSummaries();
        if (onChanged) onChanged();
      };
      row.name->onReturnKey = rename;
      row.name->onFocusLost = rename;
      row.mode = std::make_unique<juce::ComboBox>();
      row.mode->addItem(TRANS("Stereo"), 1);
      row.mode->addItem(TRANS("Mono"), 2);
      row.mode->setSelectedId(b.mono ? 2 : 1, juce::dontSendNotification);
      auto* mode = row.mode.get();
      row.mode->onChange = [this, id, mode] {
        if (const MixerBus* bus = busOf(id)) proc_.setPairLayout(id, mode->getSelectedId() == 2, bus->deviceChannels);
        showSummaries();
        if (onChanged) onChanged();
      };
      row.summary = std::make_unique<juce::Label>();
      styleLabel(*row.summary, "");
      row.summary->setColour(juce::Label::textColourId, juce::Colours::grey);
      for (juce::Component* c : {static_cast<juce::Component*>(row.pick.get()), static_cast<juce::Component*>(row.name.get()),
                                 static_cast<juce::Component*>(row.mode.get()),
                                 static_cast<juce::Component*>(row.summary.get())})
        holder_.addAndMakeVisible(*c);
      rows_.push_back(std::move(row));
    }

    // Every channel the device gives, and every one in use.
    channelCount_ = juce::jmax(2, proc_.getTotalNumOutputChannels());
    for (const auto& b : proc_.mixer().buses)
      for (int ch : b.deviceChannels) channelCount_ = juce::jmax(channelCount_, ch + 1);
    channelCount_ = juce::jmin(channelCount_, MasterpieceProcessor::kMaxOutputChannels);
    for (int ch = 0; ch < channelCount_; ++ch) {
      auto t = std::make_unique<juce::ToggleButton>(juce::String(ch + 1));
      t->setTooltip(TRANS("Device channel %1").replace("%1", juce::String(ch + 1)));
      t->onClick = [this] { applyChannels(); };
      addAndMakeVisible(*t);
      channels_.push_back(std::move(t));
    }
    for (const auto& b : proc_.mixer().buses) {
      auto t = std::make_unique<juce::ToggleButton>(outputName(proc_.mixer(), b.id.value));
      t->onClick = [this] { applySends(); };
      addAndMakeVisible(*t);
      sends_.push_back(std::move(t));
      // The send's own level, set apart from both outputs' faders (#1023).
      auto level = std::make_unique<juce::Slider>(juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
      level->setRange(MasterpieceProcessor::kSendFloorDb, MasterpieceProcessor::kSendTopDb, 0.5);
      level->setDoubleClickReturnValue(true, 0.0);
      level->textFromValueFunction = [](double v) {
        return v <= MasterpieceProcessor::kSendFloorDb ? juce::String("-inf dB") : juce::String(v, 1) + " dB";
      };
      level->valueFromTextFunction = [](const juce::String& text) {
        return text.retainCharacters("-+.0123456789").getDoubleValue();
      };
      level->setTooltip(TRANS("The level it plays into this output at, in dB; a double-click sets 0 dB"));
      const int target = b.id.value;
      auto* slider = level.get();
      level->onValueChange = [this, target, slider] {
        proc_.setOutputSendLevel(selected_, target, static_cast<float>(slider->getValue()));
        if (onChanged) onChanged();
      };
      addAndMakeVisible(*level);
      sendLevels_.push_back(std::move(level));
    }

    const int list = juce::jmin(kListMax, static_cast<int>(rows_.size()) * (kRow + 4));
    setSize(kWidth, 12 + 3 * kRow + 4 + list + kGap + kRow + kGap + kRow + linesOf(channelCount_, kChannelsPerLine) * kRow +
                        kGap + kRow + linesOf(static_cast<int>(sends_.size()), kSendsPerLine) * kRow + 12);
    pick(rows_.empty() ? 0 : rows_.front().busId);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12, 6);
    heading_.setBounds(r.removeFromTop(kRow));
    note_.setBounds(r.removeFromTop(2 * kRow));
    r.removeFromTop(4);
    // From the bottom: the sends, the channels, the buttons.
    const auto grid = [&r](std::vector<std::unique_ptr<juce::ToggleButton>>& ticks, int perLine, juce::Label& heading) {
      const int lines = linesOf(static_cast<int>(ticks.size()), perLine);
      auto area = r.removeFromBottom(kRow + lines * kRow);
      r.removeFromBottom(kGap);
      heading.setBounds(area.removeFromTop(kRow));
      const int w = area.getWidth() / perLine;
      for (size_t i = 0; i < ticks.size(); ++i)
        ticks[i]->setBounds(area.getX() + static_cast<int>(i) % perLine * w,
                            area.getY() + static_cast<int>(i) / perLine * kRow, w, kRow);
    };
    grid(sends_, kSendsPerLine, sendsHeading_);
    // Each send's level at the right of its box.
    for (size_t i = 0; i < sends_.size() && i < sendLevels_.size(); ++i) {
      auto cell = sends_[i]->getBounds();
      sendLevels_[i]->setBounds(cell.removeFromRight(kSendLevelW).reduced(2, 3));
      sends_[i]->setBounds(cell);
    }
    grid(channels_, kChannelsPerLine, channelsHeading_);
    auto buttons = r.removeFromBottom(kRow);
    allStereo_.setBounds(buttons.removeFromLeft(200).reduced(0, 1));
    buttons.removeFromLeft(kGap);
    allMono_.setBounds(buttons.removeFromLeft(200).reduced(0, 1));
    r.removeFromBottom(kGap);
    viewport_.setBounds(r);
    const int h = static_cast<int>(rows_.size()) * (kRow + 4);
    const int w = r.getWidth() - (h > r.getHeight() ? viewport_.getScrollBarThickness() : 0);
    holder_.setSize(w, h);
    int y = 0;
    for (auto& row : rows_) {
      auto line = juce::Rectangle<int>(0, y, w, kRow);
      row.pick->setBounds(line.removeFromLeft(100));
      row.name->setBounds(line.removeFromLeft(190).reduced(0, 1));
      line.removeFromLeft(kGap);
      row.mode->setBounds(line.removeFromLeft(90).reduced(0, 1));
      line.removeFromLeft(kGap);
      row.summary->setBounds(line);
      y += kRow + 4;
    }
  }

  std::function<void()> onChanged;

private:
  static constexpr int kWidth = 680;
  static constexpr int kListMax = 230;
  static constexpr int kChannelsPerLine = 16;
  static constexpr int kSendsPerLine = 4;
  static constexpr int kSendLevelW = 66;
  static int linesOf(int n, int perLine) { return juce::jmax(1, (n + perLine - 1) / perLine); }

  const MixerBus* busOf(int id) const {
    for (const auto& b : proc_.mixer().buses)
      if (b.id.value == id) return &b;
    return nullptr;
  }

  void pick(int id) {
    selected_ = id;
    const MixerBus* bus = busOf(id);
    for (auto& row : rows_) row.pick->setToggleState(row.busId == id, juce::dontSendNotification);
    const auto name = outputName(proc_.mixer(), id);
    channelsHeading_.setText(TRANS("%1 plays on the device channels").replace("%1", name), juce::dontSendNotification);
    sendsHeading_.setText(TRANS("%1 also plays into").replace("%1", name), juce::dontSendNotification);
    for (int ch = 0; ch < static_cast<int>(channels_.size()); ++ch)
      channels_[static_cast<size_t>(ch)]->setToggleState(
          bus != nullptr && std::find(bus->deviceChannels.begin(), bus->deviceChannels.end(), ch) != bus->deviceChannels.end(),
          juce::dontSendNotification);
    const auto& buses = proc_.mixer().buses;
    for (size_t i = 0; i < sends_.size() && i < buses.size(); ++i) {
      const int other = buses[i].id.value;
      sends_[i]->setEnabled(other != id);
      sends_[i]->setToggleState(bus != nullptr && std::find(bus->sends.begin(), bus->sends.end(), other) != bus->sends.end(),
                                juce::dontSendNotification);
    }
    showSendLevels();
    showSummaries();
  }

  // A level for each output ticked, at the level the send has.
  void showSendLevels() {
    const MixerBus* bus = busOf(selected_);
    for (size_t i = 0; i < sends_.size() && i < sendLevels_.size(); ++i) {
      const bool on = sends_[i]->getToggleState() && sends_[i]->isEnabled();
      const int other = i < proc_.mixer().buses.size() ? proc_.mixer().buses[i].id.value : 0;
      sendLevels_[i]->setEnabled(on);
      sendLevels_[i]->setValue(bus != nullptr ? bus->sendLevelDb(other) : 0.0, juce::dontSendNotification);
      sendLevels_[i]->setAlpha(on ? 1.0f : 0.35f);
    }
  }

  void applyChannels() {
    const MixerBus* bus = busOf(selected_);
    if (bus == nullptr) return;
    std::vector<int> ch;
    for (int i = 0; i < static_cast<int>(channels_.size()); ++i)
      if (channels_[static_cast<size_t>(i)]->getToggleState()) ch.push_back(i);
    // A channel past the device's ticks stays as it was.
    for (int c : bus->deviceChannels)
      if (c >= static_cast<int>(channels_.size())) ch.push_back(c);
    proc_.setPairLayout(selected_, bus->mono, ch);
    showSummaries();
    if (onChanged) onChanged();
  }

  void applySends() {
    std::vector<int> sends;
    const auto& buses = proc_.mixer().buses;
    for (size_t i = 0; i < sends_.size() && i < buses.size(); ++i)
      if (sends_[i]->getToggleState() && buses[i].id.value != selected_) sends.push_back(buses[i].id.value);
    proc_.setOutputSends(selected_, sends);
    showSendLevels();
    showSummaries();
    if (onChanged) onChanged();
  }

  // Each row says its channels and where it also plays; the send boxes take
  // the outputs' names.
  void showSummaries() {
    for (auto& row : rows_) {
      const MixerBus* bus = busOf(row.busId);
      if (bus == nullptr) continue;
      juce::String text = bus->deviceChannels.empty() ? TRANS("no channel")
                                                      : channelsText(bus->deviceChannels, bus->mono);
      if (!bus->sends.empty()) {
        juce::StringArray into;
        for (int s : bus->sends) into.add(outputName(proc_.mixer(), s));
        text += "  " + TRANS("into %1").replace("%1", into.joinIntoString(", "));
      }
      row.summary->setText(text, juce::dontSendNotification);
    }
    const auto& buses = proc_.mixer().buses;
    for (size_t i = 0; i < sends_.size() && i < buses.size(); ++i)
      sends_[i]->setButtonText(outputName(proc_.mixer(), buses[i].id.value));
  }

  // Every output in turn from channel 1: two channels each in stereo, one in
  // mono, without copies.
  void layOut(bool mono) {
    int next = 0;
    for (auto& row : rows_) {
      std::vector<int> ch{next};
      if (!mono) ch.push_back(next + 1);
      next += mono ? 1 : 2;
      proc_.setPairLayout(row.busId, mono, ch);
      row.mode->setSelectedId(mono ? 2 : 1, juce::dontSendNotification);
    }
    pick(selected_);
    if (onChanged) onChanged();
  }

  struct Row {
    int busId = 0;
    std::unique_ptr<juce::ToggleButton> pick;
    std::unique_ptr<juce::TextEditor> name;
    std::unique_ptr<juce::ComboBox> mode;
    std::unique_ptr<juce::Label> summary;
  };
  MasterpieceProcessor& proc_;
  juce::Label heading_, note_, channelsHeading_, sendsHeading_;
  juce::TextButton allStereo_{TRANS("All stereo, in order")};
  juce::TextButton allMono_{TRANS("All mono, in order")};
  juce::Viewport viewport_;
  juce::Component holder_;
  std::vector<Row> rows_;
  std::vector<std::unique_ptr<juce::ToggleButton>> channels_, sends_;
  std::vector<std::unique_ptr<juce::Slider>> sendLevels_;
  int channelCount_ = 2;
  int selected_ = 0;
};

// The ranks, each with a box to tick and where it plays. The ticked ranks go
// to one destination in one move. A division and a text narrow the list.
class RankRoutingPanel final : public juce::Component {
public:
  RankRoutingPanel(MasterpieceProcessor& p, std::function<void()> changed)
      : onChanged(std::move(changed)), proc_(p) {
    addAndMakeVisible(heading_);
    styleLabel(heading_, TRANS("Rank routing"));
    addAndMakeVisible(note_);
    styleLabel(note_, TRANS("A rank with no route of its own plays through the default destination."));
    note_.setColour(juce::Label::textColourId, juce::Colours::grey);

    const auto& model = proc_.organModel();
    std::vector<Id> divisions;
    for (const auto& [id, d] : model.divisions) divisions.push_back(id);
    std::sort(divisions.begin(), divisions.end());
    std::map<int, juce::String> divisionName;
    for (Id id : divisions) {
      const auto& d = model.divisions.at(id);
      divisionName[proc_.liveDivisionIndexOf(id)] = d.name.empty() ? juce::String(static_cast<int>(id)) : juce::String(d.name);
    }
    for (const auto& [id, rank] : model.ranks) {
      Row row;
      row.rankId = id;
      row.division = proc_.liveDivisionOfRank(proc_.liveRankIndexOf(id));
      row.tick = std::make_unique<juce::ToggleButton>(rankTitle(model, id));
      row.tick->setTooltip(TRANS("Rank %1").replace("%1", juce::String(static_cast<int>(id))));
      row.divisionLabel = std::make_unique<juce::Label>();
      const auto name = divisionName.find(row.division);
      styleLabel(*row.divisionLabel, name != divisionName.end() ? name->second : TRANS("Other"));
      row.divisionLabel->setColour(juce::Label::textColourId, juce::Colours::grey);
      row.dest = std::make_unique<juce::Label>();
      styleLabel(*row.dest, "");
      for (juce::Component* c : {static_cast<juce::Component*>(row.tick.get()),
                                 static_cast<juce::Component*>(row.divisionLabel.get()),
                                 static_cast<juce::Component*>(row.dest.get())})
        holder_.addAndMakeVisible(*c);
      rows_.push_back(std::move(row));
    }
    std::stable_sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
      const int da = a.division < 0 ? std::numeric_limits<int>::max() : a.division;
      const int db = b.division < 0 ? std::numeric_limits<int>::max() : b.division;
      return da != db ? da < db : a.rankId < b.rankId;
    });

    addAndMakeVisible(division_);
    division_.addItem(TRANS("Every division"), 1);
    for (const auto& [index, name] : divisionName)
      if (std::any_of(rows_.begin(), rows_.end(), [i = index](const Row& r) { return r.division == i; }))
        division_.addItem(name, kDivisionItem + index);
    division_.setSelectedId(1, juce::dontSendNotification);
    division_.onChange = [this] { resized(); };
    addAndMakeVisible(filter_);
    filter_.setTextToShowWhenEmpty(TRANS("Filter, for example: Rear, tremmed"), juce::Colours::grey);
    filter_.onTextChange = [this] { resized(); };
    addAndMakeVisible(tickShown_);
    tickShown_.onClick = [this] {
      for (auto& row : rows_)
        if (shown(row)) row.tick->setToggleState(true, juce::dontSendNotification);
    };
    addAndMakeVisible(untick_);
    untick_.onClick = [this] {
      for (auto& row : rows_) row.tick->setToggleState(false, juce::dontSendNotification);
    };
    addAndMakeVisible(sendTo_);
    sendTo_.setTextWhenNothingSelected(TRANS("Send the ticked ranks to..."));
    sendTo_.setTitle(TRANS("Send the ticked ranks to..."));  // a screen reader's name for it
    sendTo_.onChange = [this] { send(sendTo_.getSelectedId()); };
    addAndMakeVisible(mixTo_);
    mixTo_.setTitle(TRANS("Put the ticked ranks in a mix..."));
    mixTo_.setTooltip(TRANS("A mix is a set of ranks with a fader of its own on the mixer"));
    mixTo_.onChange = [this] { putInMix(mixTo_.getSelectedId()); };
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&holder_, false);
    viewport_.setScrollBarsShown(true, false);
    showDestinations();
    setSize(660, 590);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12, 6);
    heading_.setBounds(r.removeFromTop(kRow));
    note_.setBounds(r.removeFromTop(kRow));
    r.removeFromTop(4);
    auto line = r.removeFromTop(kRow);
    division_.setBounds(line.removeFromLeft(200).reduced(0, 1));
    line.removeFromLeft(kGap);
    filter_.setBounds(line.reduced(0, 1));
    r.removeFromTop(4);
    auto boxes = r.removeFromBottom(kRow);
    sendTo_.setBounds(boxes.removeFromLeft((boxes.getWidth() - kGap) / 2).reduced(0, 1));
    boxes.removeFromLeft(kGap);
    mixTo_.setBounds(boxes.reduced(0, 1));
    r.removeFromBottom(4);
    auto buttons = r.removeFromBottom(kRow);
    tickShown_.setBounds(buttons.removeFromLeft(170).reduced(0, 1));
    buttons.removeFromLeft(6);
    untick_.setBounds(buttons.removeFromLeft(110).reduced(0, 1));
    r.removeFromBottom(kGap);
    viewport_.setBounds(r);
    int count = 0;
    for (auto& row : rows_) count += shown(row) ? 1 : 0;
    const int h = count * (kRow + 2);
    const int w = r.getWidth() - (h > r.getHeight() ? viewport_.getScrollBarThickness() : 0);
    holder_.setSize(w, h);
    int y = 0;
    for (auto& row : rows_) {
      const bool on = shown(row);
      for (juce::Component* c : {static_cast<juce::Component*>(row.tick.get()),
                                 static_cast<juce::Component*>(row.divisionLabel.get()),
                                 static_cast<juce::Component*>(row.dest.get())})
        c->setVisible(on);
      if (!on) continue;
      auto at = juce::Rectangle<int>(0, y, w, kRow);
      row.tick->setBounds(at.removeFromLeft(w / 2));
      row.divisionLabel->setBounds(at.removeFromLeft(110));
      row.dest->setBounds(at);
      y += kRow + 2;
    }
  }

  std::function<void()> onChanged;

private:
  static constexpr int kDivisionItem = 100;
  static constexpr int kNewMix = 1, kNoMix = 2, kMixItem = 100;
  struct Row {
    Id rankId = 0;
    int division = -1;
    std::unique_ptr<juce::ToggleButton> tick;
    std::unique_ptr<juce::Label> divisionLabel, dest;
  };

  bool shown(const Row& row) const {
    const int d = division_.getSelectedId();
    if (d >= kDivisionItem && row.division != d - kDivisionItem) return false;
    const auto text = filter_.getText().trim();
    return text.isEmpty() || row.tick->getButtonText().containsIgnoreCase(text) ||
           row.divisionLabel->getText().containsIgnoreCase(text);
  }

  void showDestinations() {
    const auto& mixer = proc_.mixer();
    const auto mixes = proc_.rankMixes();
    for (auto& row : rows_) {
      const auto routed = mixer.rankRoutings.find(row.rankId);
      juce::String text = routed != mixer.rankRoutings.end()
                              ? destinationName(mixer, routed->second.perspectives[0].dest)
                              : TRANS("The default: %1").replace("%1", destinationName(mixer, mixer.defaultDest));
      for (const auto& m : mixes)
        if (std::find(m.ranks.begin(), m.ranks.end(), row.rankId) != m.ranks.end())
          text += ", " + mixName(m);
      row.dest->setText(text, juce::dontSendNotification);
      row.dest->setColour(juce::Label::textColourId,
                          routed != mixer.rankRoutings.end() ? juce::Colours::white : juce::Colours::grey);
    }
    fillDestinationBox(sendTo_, mixer, true);
    sendTo_.setSelectedId(0, juce::dontSendNotification);
    mixTo_.clear(juce::dontSendNotification);
    mixTo_.setTextWhenNothingSelected(TRANS("Put the ticked ranks in a mix..."));
    mixTo_.addItem(TRANS("A new mix"), kNewMix);
    mixTo_.setItemEnabled(kNewMix, static_cast<int>(mixes.size()) < LiveLevels::kMaxMixes);
    for (const auto& m : mixes) mixTo_.addItem(mixName(m), kMixItem + m.id);
    mixTo_.addItem(TRANS("Out of their mix"), kNoMix);
  }

  static juce::String mixName(const MasterpieceProcessor::RankMix& m) {
    const auto name = juce::String::fromUTF8(m.name.c_str());
    return name.isEmpty() ? TRANS("Mix %1").replace("%1", juce::String(m.id)) : name;
  }

  // A rank is in one mix at most: the ticked ranks leave the others.
  void putInMix(int item) {
    if (item <= 0) return;
    std::vector<Id> ticked;
    for (auto& row : rows_)
      if (row.tick->getToggleState()) ticked.push_back(row.rankId);
    auto mixes = proc_.rankMixes();
    int target = item >= kMixItem ? item - kMixItem : 0;
    if (item == kNewMix) {
      target = 1;
      while (target <= LiveLevels::kMaxMixes &&
             std::any_of(mixes.begin(), mixes.end(), [target](const auto& m) { return m.id == target; }))
        ++target;
      if (target > LiveLevels::kMaxMixes) return;
      MasterpieceProcessor::RankMix m;
      m.id = target;
      m.name = TRANS("Mix %1").replace("%1", juce::String(target)).toStdString();
      mixes.push_back(std::move(m));
    }
    for (auto& m : mixes) {
      m.ranks.erase(std::remove_if(m.ranks.begin(), m.ranks.end(),
                                   [&ticked](Id r) { return std::find(ticked.begin(), ticked.end(), r) != ticked.end(); }),
                    m.ranks.end());
      if (m.id == target) m.ranks.insert(m.ranks.end(), ticked.begin(), ticked.end());
    }
    proc_.setRankMixes(mixes);
    juce::Component::SafePointer<RankRoutingPanel> self(this);
    juce::MessageManager::callAsync([self] {
      if (self == nullptr) return;
      self->showDestinations();
      if (self->onChanged) self->onChanged();
    });
  }

  void send(int item) {
    if (item <= 0) return;
    std::vector<Id> ticked;
    for (auto& row : rows_)
      if (row.tick->getToggleState()) ticked.push_back(row.rankId);
    if (item == kDestDefault) {
      proc_.followDefault(ticked);
    } else {
      std::vector<std::pair<Id, std::variant<BusId, int>>> routes;
      for (Id id : ticked) {
        if (item >= kDestGroup) routes.emplace_back(id, item - kDestGroup);
        else routes.emplace_back(id, BusId{item});
      }
      proc_.setRankRoutes(routes);
    }
    // Later: the box that asked is filled again.
    juce::Component::SafePointer<RankRoutingPanel> self(this);
    juce::MessageManager::callAsync([self] {
      if (self == nullptr) return;
      self->showDestinations();
      if (self->onChanged) self->onChanged();
    });
  }

  MasterpieceProcessor& proc_;
  juce::Label heading_, note_;
  juce::ComboBox division_;
  juce::TextEditor filter_;
  juce::TextButton tickShown_{TRANS("Tick the ranks on show")};
  juce::TextButton untick_{TRANS("Untick all")};
  juce::ComboBox sendTo_;
  juce::ComboBox mixTo_;
  juce::Viewport viewport_;
  juce::Component holder_;
  std::vector<Row> rows_;
};
}  // namespace

// A mixer tab in a window of its own: the strips side by side, scrolling
// when they outgrow it. Closing it puts the tab back. It floats above the
// Organ settings window it comes from, which stays above the console: opened
// as an ordinary window, it went behind that one, out of sight (#1188).
class MixerPanel::StripWindow final : public juce::DocumentWindow, private juce::Timer {
public:
  StripWindow(const juce::String& title, std::function<void()> closed, std::function<void()> laidOut)
      : juce::DocumentWindow(title, juce::Colour(0xff15171c),
                             juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
        closed_(std::move(closed)),
        laidOut_(std::move(laidOut)) {
    setUsingNativeTitleBar(true);
    setResizable(true, false);
    view.setViewedComponent(&holder, false);
    view.setScrollBarsShown(false, true);
    setContentNonOwned(&view, false);
    // Above the others while this program is in front, as the console's
    // floating windows are; not above anyone else's.
    if (kLiveOnTop) startTimer(250);
  }
  ~StripWindow() override { stopTimer(); }
  void timerCallback() override {
    const bool front = juce::Process::isForegroundProcess();
    if (isVisible() && isAlwaysOnTop() != front) setAlwaysOnTop(front);
  }
  void closeButtonPressed() override {
    // After this call returns: closing takes this window away.
    juce::MessageManager::callAsync([fn = closed_] { fn(); });
  }
  void resized() override {
    juce::DocumentWindow::resized();
    if (laidOut_) laidOut_();
  }
  juce::Viewport view;
  juce::Component holder;

private:
  std::function<void()> closed_, laidOut_;
};

MixerPanel::Source MixerPanel::sourceOfKind(MidiTargetKind kind) {
  switch (kind) {
    case MidiTargetKind::DivisionLevel: return Source::Divisions;
    case MidiTargetKind::GroupLevel: return Source::Groups;
    case MidiTargetKind::MixLevel: return Source::Mixes;
    default: return Source::Outputs;
  }
}

juce::String MixerPanel::sourceName(Source s) const {
  switch (s) {
    case Source::Divisions: return TRANS("Divisions");
    case Source::Groups: return groupedByWindchest(proc_.organModel()) ? TRANS("Windchest groups") : TRANS("Perspectives");
    case Source::Mixes: return TRANS("Mixes");
    case Source::Outputs: break;
  }
  return TRANS("Outputs");
}

bool MixerPanel::isDetached(Source s) const { return stripWindows_[static_cast<size_t>(s)] != nullptr; }

juce::Component& MixerPanel::holderFor(Source s) {
  auto& w = stripWindows_[static_cast<size_t>(s)];
  return w != nullptr ? w->holder : busHolder_;
}

void MixerPanel::setDetached(Source s, bool on) {
  auto& w = stripWindows_[static_cast<size_t>(s)];
  if (on == (w != nullptr) || kMobile) return;
  if (on) {
    juce::Component::SafePointer<MixerPanel> self(this);
    w = std::make_unique<StripWindow>(
        juce::String(proc_.organModel().organName) + " - " + sourceName(s),
        [self, s] {
          if (self != nullptr) self->setDetached(s, false);
        },
        [self, s] {
          if (self == nullptr) return;
          if (auto& win = self->stripWindows_[static_cast<size_t>(s)]; win != nullptr)
            self->layOutStrips(s, win->view, win->holder);
        });
    for (auto& f : sourceFaders(s)) w->holder.addAndMakeVisible(*f.strip);
    // Wide enough for every strip up to most of the screen, as tall as the
    // tab shows them.
    const int stripW = ChannelStrip::widthFor(proc_.narrowStrips());
    const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                          ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                          : juce::Rectangle<int>(0, 0, 1280, 720);
    const int wide = juce::jlimit(2 * stripW, area.getWidth() * 9 / 10,
                                  juce::jmax(1, static_cast<int>(sourceFaders(s).size())) * stripW + 4);
    const int high = juce::jlimit(ChannelStrip::kMinHeight + 40, area.getHeight() * 9 / 10,
                                  juce::jmax(busViewport_.getHeight(), ChannelStrip::kMinHeight) + 40);
    w->centreWithSize(wide, high);
    keepOnScreen(*w);
    showFloating(*w, true, true);
  } else {
    for (auto& f : sourceFaders(s)) busHolder_.addChildComponent(*f.strip);
    w.reset();
  }
  fillSourceTabs();
  resized();
}

// The kinds with strips on this page: those with none, and those in a window
// of their own, are left out.
std::vector<MixerPanel::Source> MixerPanel::sectionsWithStrips() const {
  std::vector<Source> out;
  for (Source s : {Source::Divisions, Source::Groups, Source::Mixes, Source::Outputs})
    if (!sourceFaders(s).empty() && !isDetached(s)) out.push_back(s);
  return out;
}

namespace {
constexpr int kSectionGap = 14;    // between the kinds, side by side
constexpr int kSectionTitle = 18;  // a kind's title above its strips
}  // namespace

int MixerPanel::stripsWidth(const std::vector<Source>& sections) const {
  const int stripW = ChannelStrip::widthFor(proc_.narrowStrips());
  int w = 0;
  for (Source s : sections) {
    if (w > 0) w += kSectionGap;
    w += static_cast<int>(sourceFaders(s).size()) * stripW;
  }
  return w;
}

// Every kind side by side on the page, each under its title (#1188).
void MixerPanel::layOutAll() {
  const int stripW = ChannelStrip::widthFor(proc_.narrowStrips());
  const auto sections = sectionsWithStrips();
  const int h = juce::jmax(ChannelStrip::kMinHeight + kSectionTitle, busViewport_.getHeight());
  busHolder_.setSize(juce::jmax(1, stripsWidth(sections)), h);
  int x = 0;
  for (Source s : sections) {
    auto& faders = sourceFaders(s);
    const int w = static_cast<int>(faders.size()) * stripW;
    auto& title = sectionTitles_[static_cast<size_t>(s)];
    title.setText(sourceName(s), juce::dontSendNotification);
    title.setBounds(x, 0, w, kSectionTitle);
    title.setVisible(true);
    int at = x;
    for (auto& f : faders) {
      f.strip->setBounds(at, kSectionTitle, stripW, h - kSectionTitle);
      at += stripW;
    }
    x += w + kSectionGap;
  }
}

void MixerPanel::layOutStrips(Source s, juce::Viewport& view, juce::Component& holder) {
  const int stripW = ChannelStrip::widthFor(proc_.narrowStrips());
  auto& faders = sourceFaders(s);
  const int needed = static_cast<int>(faders.size()) * stripW;
  const int h = juce::jmax(ChannelStrip::kMinHeight,
                           view.getHeight() - (needed > view.getWidth() ? view.getScrollBarThickness() : 0));
  holder.setSize(juce::jmax(needed, 1), h);
  int x = 0;
  for (auto& f : faders) {
    f.strip->setBounds(x, 0, stripW, h);
    x += stripW;
  }
}

MixerPanel::MixerPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Outputs"));
  addAndMakeVisible(busesLabel_);
  styleLabel(busesLabel_, TRANS("Outputs"));
  addAndMakeVisible(busCount_);
  for (int i = 1; i <= MasterpieceProcessor::kMaxOutputs; ++i)
    busCount_.addItem(i == 1 ? TRANS("1 output") : TRANS("%1 outputs").replace("%1", juce::String(i)), i);
  busCount_.setSelectedId(juce::jmax(1, proc_.mixBusCount()),
                          juce::dontSendNotification);
  busCount_.onChange = [this] { setBusCount(busCount_.getSelectedId()); };
  addAndMakeVisible(defaultLabel_);
  styleLabel(defaultLabel_, TRANS("Ranks play through"));
  defaultLabel_.setJustificationType(juce::Justification::centredRight);
  addAndMakeVisible(defaultDest_);
  defaultDest_.setTooltip(TRANS("Every rank without a route of its own plays here, on every organ"));
  defaultDest_.onChange = [this] {
    const int item = defaultDest_.getSelectedId();
    if (item <= 0) return;
    if (item > kGroupItem) proc_.setDefaultDestination(item - kGroupItem);
    else proc_.setDefaultDestination(BusId{item});
    outputsChanged();
  };

  addAndMakeVisible(spread_);
  spread_.setTooltip(TRANS("Each rank to the next output in turn"));
  spread_.onClick = [this] {
    // Round-robin: the routing audible at once, for the player to adjust.
    const int buses = juce::jmax(1, proc_.mixBusCount());
    for (size_t i = 0; i < rankDest_.size(); ++i) rankDest_[i] = static_cast<int>(i) % buses + 1;
    pushRouting();
  };
  addAndMakeVisible(reset_);
  reset_.setTooltip(TRANS("Every rank loses its own route and plays through the default"));
  reset_.onClick = [this] {
    std::fill(rankDest_.begin(), rankDest_.end(), kDefaultItem);
    pushRouting();
  };
  addAndMakeVisible(routing_);
  routing_.setTooltip(TRANS("The ranks in a list, each to an output or a speaker group"));
  routing_.onClick = [this] {
    juce::Component::SafePointer<MixerPanel> self(this);
    juce::CallOutBox::launchAsynchronously(std::make_unique<RankRoutingPanel>(proc_,
                                                                              [self] {
                                                                                if (self == nullptr) return;
                                                                                self->mixesChanged();
                                                                                self->routesChanged();
                                                                              }),
                                           routing_.getScreenBounds(), nullptr);
  };
  addAndMakeVisible(narrow_);
  narrow_.setToggleState(proc_.narrowStrips(), juce::dontSendNotification);
  narrow_.setTooltip(TRANS("More strips in view: the scales keep their ticks, without numbers"));
  narrow_.onClick = [this] {
    proc_.setNarrowStrips(narrow_.getToggleState());
    for (auto* list : {&pairFaders_, &divisionFaders_, &groupFaders_, &mixFaders_})
      for (auto& f : *list) f.strip->setNarrow(narrow_.getToggleState());
    resized();
  };
  addAndMakeVisible(sources_);
  sources_.onChange = [this](int index) {
    if (index >= 0 && index < static_cast<int>(sourceOfTab_.size())) shownSource_ = sourceOfTab_[static_cast<size_t>(index)];
    resized();
  };
  // A tab to a window of its own: the button for the open tab, or a
  // right-click on any tab. Not on phones and tablets, where every panel is
  // a window already.
  sources_.onPopup = [this](int index) {
    if (kMobile || index < 0 || index >= static_cast<int>(sourceOfTab_.size())) return;
    const Source s = sourceOfTab_[static_cast<size_t>(index)];
    juce::PopupMenu m;
    m.addItem(TRANS("Open %1 in a window of its own").replace("%1", sourceName(s)), [this, s] { setDetached(s, true); });
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
  };
  if (!kMobile) addAndMakeVisible(detach_);
  detach_.setTooltip(TRANS("Opens this tab's strips in a window of their own, to keep beside the console; closing it "
                           "puts them back"));
  detach_.onClick = [this] {
    if (!allInView_) {
      if (!sourceOfTab_.empty()) setDetached(shownSource_, true);
      return;
    }
    // Every kind in view: the player picks which one.
    juce::PopupMenu m;
    for (Source s : sectionsWithStrips())
      m.addItem(TRANS("Open %1 in a window of its own").replace("%1", sourceName(s)), [this, s] { setDetached(s, true); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&detach_));
  };
  addAndMakeVisible(emptySource_);
  styleLabel(emptySource_, "");
  emptySource_.setColour(juce::Label::textColourId, juce::Colours::grey);
  emptySource_.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(save_);
  save_.onClick = [this] { proc_.saveSettings(); };
  addAndMakeVisible(outputEq_);
  outputEq_.setTooltip(TRANS("Each output's own EQ, for the speaker on it"));
  outputEq_.onClick = [this] {
    juce::CallOutBox::launchAsynchronously(std::make_unique<OutputEqPanel>(proc_, 1), outputEq_.getScreenBounds(),
                                           nullptr);
  };
  addAndMakeVisible(crossover_);
  crossover_.setTooltip(TRANS("A digital crossover: each output's high-pass, low-pass, delay and polarity, so an "
                              "installation needs no crossover of its own"));
  crossover_.onClick = [this] {
    juce::Component::SafePointer<MixerPanel> self(this);
    juce::CallOutBox::launchAsynchronously(std::make_unique<CrossoverPanel>(proc_,
                                                                            [self] {
                                                                              if (self != nullptr) self->showOutputs();
                                                                            }),
                                           crossover_.getScreenBounds(), nullptr);
  };
  addAndMakeVisible(speakerGroups_);
  speakerGroups_.setTooltip(TRANS("Outputs that share the pipes sent to them, note by note"));
  speakerGroups_.onClick = [this] {
    juce::Component::SafePointer<MixerPanel> self(this);
    juce::CallOutBox::launchAsynchronously(std::make_unique<SpeakerGroupsPanel>(proc_,
                                                                                [self] {
                                                                                  if (self != nullptr)
                                                                                    self->outputsChanged();
                                                                                }),
                                           speakerGroups_.getScreenBounds(), nullptr);
  };
  addAndMakeVisible(channels_);
  channels_.setTooltip(TRANS("Each output's name, stereo or mono, its device channels and the outputs it also plays into"));
  channels_.onClick = [this] {
    juce::Component::SafePointer<MixerPanel> self(this);
    juce::CallOutBox::launchAsynchronously(std::make_unique<OutputsPanel>(proc_,
                                                                               [self] {
                                                                                 if (self != nullptr)
                                                                                   self->outputsChanged();
                                                                               }),
                                           channels_.getScreenBounds(), nullptr);
  };
  for (auto& title : sectionTitles_) {
    styleLabel(title, "");
    title.setColour(juce::Label::textColourId, juce::Colours::grey);
    title.setMinimumHorizontalScale(0.6f);
    busHolder_.addChildComponent(title);
  }

  addAndMakeVisible(status_);
  styleLabel(status_, "");
  addAndMakeVisible(outputs_);
  styleLabel(outputs_, "");
  addAndMakeVisible(busViewport_);
  busViewport_.setViewedComponent(&busHolder_, false);
  busViewport_.setScrollBarsShown(false, true);

  refresh();
  // Four looks a second, as often as the engine measures.
  startTimerHz(4);
}

MixerPanel::~MixerPanel() { proc_.setStripMeters(false); }

LiveLevels::Strip MixerPanel::stripOf(MidiTargetKind kind) {
  switch (kind) {
    case MidiTargetKind::PairLevel: return LiveLevels::Strip::Pair;
    case MidiTargetKind::DivisionLevel: return LiveLevels::Strip::Division;
    case MidiTargetKind::GroupLevel: return LiveLevels::Strip::Group;
    case MidiTargetKind::MixLevel: return LiveLevels::Strip::Mix;
    default: return LiveLevels::Strip::Rank;
  }
}

void MixerPanel::addFader(std::vector<Fader>& into, MidiTargetKind kind, Id id, int index,
                          const juce::String& name, const juce::String& tooltip) {
  Fader f;
  f.kind = kind;
  f.id = id;
  f.index = index;
  const auto stripKind = kind == MidiTargetKind::PairLevel       ? ChannelStrip::Kind::Pair
                         : kind == MidiTargetKind::DivisionLevel ? ChannelStrip::Kind::Division
                         : kind == MidiTargetKind::GroupLevel || kind == MidiTargetKind::MixLevel
                             ? ChannelStrip::Kind::Group
                             : ChannelStrip::Kind::Rank;
  f.strip = std::make_unique<ChannelStrip>(stripKind, name, tooltip);
  f.strip->setNarrow(proc_.narrowStrips());
  f.strip->setLevel(faderLevel(f));
  const auto s = stripOf(kind);
  if (isMaster(f)) {
    f.strip->setMaster();
  } else {
    f.strip->setMuted(proc_.liveMuted(s, index));
    f.strip->setSoloed(proc_.liveSoloed(s, index));
  }
  // Shown when its tab is open, or in its tab's own window.
  holderFor(sourceOfKind(kind)).addChildComponent(*f.strip);
  into.push_back(std::move(f));
  Fader& made = into.back();
  ChannelStrip& strip = *made.strip;
  strip.onLevel = [this, &into, i = into.size() - 1](int value) { setFaderLevel(into[i], value); };
  strip.onMute = [this, s, &into, i = into.size() - 1](bool on) {
    proc_.setLiveMuted(s, into[i].index, on);
    for (int m : into[i].more) proc_.setLiveMuted(s, m, on);
  };
  strip.onSolo = [this, s, &into, i = into.size() - 1](bool on) {
    proc_.setLiveSoloed(s, into[i].index, on);
    for (int m : into[i].more) proc_.setLiveSoloed(s, m, on);
  };
  strip.midi.onClick = [this, &into, i = into.size() - 1] { faderMidiMenu(into[i]); };
}

int MixerPanel::faderLevel(const Fader& f) const {
  switch (f.kind) {
    case MidiTargetKind::PairLevel: return proc_.livePairLevel(f.index);
    case MidiTargetKind::DivisionLevel: return proc_.liveDivisionLevel(f.index);
    case MidiTargetKind::GroupLevel: return proc_.liveGroupLevel(f.index);
    case MidiTargetKind::MixLevel: return proc_.liveMixLevel(f.index);
    case MidiTargetKind::RankLevel: return proc_.liveRankLevel(f.index);
    case MidiTargetKind::MasterVolume: {
      // The top bar's law and range: -40 to +24 dB, the bottom silence.
      const auto* gain = proc_.apvts().getRawParameterValue("masterGain");
      const float g = gain != nullptr ? gain->load(std::memory_order_relaxed) : 1.0f;
      return g <= 0.0f ? 0 : LiveLevels::valueOf(juce::Decibels::gainToDecibels(g, -100.0f));
    }
    default: return LiveLevels::kFull;
  }
}

void MixerPanel::addMasterFader() {
  addFader(pairFaders_, MidiTargetKind::MasterVolume, 0, 0, TRANS("Master"),
           TRANS("The master volume, the same as the top bar's"));
}

void MixerPanel::setFaderLevel(const Fader& f, int value) {
  switch (f.kind) {
    case MidiTargetKind::PairLevel: proc_.setLivePairLevel(f.index, value); break;
    case MidiTargetKind::DivisionLevel:
      proc_.setLiveDivisionLevel(f.index, value);
      for (int m : f.more) proc_.setLiveDivisionLevel(m, value);
      break;
    case MidiTargetKind::GroupLevel: proc_.setLiveGroupLevel(f.index, value); break;
    case MidiTargetKind::MixLevel: proc_.setLiveMixLevel(f.index, value); break;
    case MidiTargetKind::RankLevel: proc_.setLiveRankLevel(f.index, value); break;
    case MidiTargetKind::MasterVolume:
      if (auto* p = proc_.apvts().getParameter("masterGain")) {
        p->setValueNotifyingHost(p->convertTo0to1(LiveLevels::gainOf(value)));
        // Written alone, as the top bar's fader is.
        proc_.markMasterGainDirty();
      }
      break;
    default: break;
  }
}

// What the knob or pedal for this level is, and Learn and Clear. Learning
// waits for a controller: a key pressed meanwhile plays.
void MixerPanel::faderMidiMenu(Fader& f) {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    proc_.cancelLearn();
    return;
  }
  // The level from a knob or a pedal; mute and solo from a button each.
  const Id buttonId = stripTarget(static_cast<int>(stripOf(f.kind)), f.id);
  const std::array<std::pair<MidiTargetKind, Id>, 3> targets{
      {{f.kind, f.id}, {MidiTargetKind::StripMute, buttonId}, {MidiTargetKind::StripSolo, buttonId}}};
  const juce::String names[] = {TRANS("Level"), TRANS("Mute"), TRANS("Solo")};
  juce::PopupMenu menu;
  // The master has its level alone.
  for (size_t t = 0; t < (isMaster(f) ? size_t{1} : targets.size()); ++t) {
    menu.addSectionHeader(names[t]);
    const auto existing = map.bindingsFor(targets[t].first, targets[t].second);
    if (existing.empty()) menu.addItem(-1, TRANS("Not mapped"), false);
    for (const MidiBinding* b : existing) menu.addItem(-1, MidiEventDialog::describeBinding(*b), false);
    const int base = 1 + static_cast<int>(t) * 2;
    menu.addItem(base, t == 0 ? TRANS("Learn: move the knob or pedal") : TRANS("Learn: press the button"));
    menu.addItem(base + 1, TRANS("Clear mapping"), !existing.empty());
  }
  juce::Component::SafePointer<MixerPanel> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&f.strip->midi),
                     [self, targets](int choice) {
                       if (self == nullptr || choice <= 0) return;
                       const auto& [kind, id] = targets[static_cast<size_t>((choice - 1) / 2)];
                       if ((choice - 1) % 2 == 0) {
                         self->proc_.beginLearn(kind, id, false);
                       } else {
                         self->proc_.editMidiMap([k = kind, i = id](MidiMap& map) { map.unbindTarget(k, i); });
                         self->proc_.saveMidiMap();
                       }
                     });
}

void MixerPanel::timerCallback() {
  // The engine measures only while the strips are on screen.
  const bool showing = isShowing();
  proc_.setStripMeters(showing);
  if (!showing || proc_.organLoading()) return;
  for (auto* list : {&pairFaders_, &divisionFaders_, &groupFaders_, &mixFaders_})
    for (auto& f : *list) {
      ChannelStrip& strip = *f.strip;
      if (!strip.isVisible()) continue;
      if (isMaster(f)) {
        // The loudest output after the master volume, and the top bar's
        // fader where it stands.
        float loudest = 0.0f;
        for (const auto& pair : pairFaders_)
          if (!isMaster(pair)) loudest = std::max(loudest, proc_.stripLevel(LiveLevels::Strip::Pair, pair.index));
        strip.setMeter(loudest * LiveLevels::gainOf(faderLevel(f)));
        if (!strip.fader.isMouseButtonDown()) strip.setLevel(faderLevel(f));
        const auto& map = proc_.midiMap();
        const bool learning = map.learning() && map.learningKind() == f.kind;
        const auto text = learning ? TRANS("Move it...") : TRANS("MIDI");
        if (strip.midi.getButtonText() != text) strip.midi.setButtonText(text);
        continue;
      }
      const auto s = stripOf(f.kind);
      // A strip over several: its first leads, so a knob learnt on it moves
      // them all; the meter shows the loudest.
      float meter = proc_.stripLevel(s, f.index);
      for (int m : f.more) {
        meter = std::max(meter, proc_.stripLevel(s, m));
        if (f.kind == MidiTargetKind::DivisionLevel && proc_.liveDivisionLevel(m) != proc_.liveDivisionLevel(f.index))
          proc_.setLiveDivisionLevel(m, proc_.liveDivisionLevel(f.index));
        if (proc_.liveMuted(s, m) != proc_.liveMuted(s, f.index)) proc_.setLiveMuted(s, m, proc_.liveMuted(s, f.index));
        if (proc_.liveSoloed(s, m) != proc_.liveSoloed(s, f.index))
          proc_.setLiveSoloed(s, m, proc_.liveSoloed(s, f.index));
      }
      strip.setMeter(meter);
      if (!strip.fader.isMouseButtonDown()) strip.setLevel(faderLevel(f));
      strip.setMuted(proc_.liveMuted(s, f.index));
      strip.setSoloed(proc_.liveSoloed(s, f.index));
      // A learning button says so, as the console's controls do.
      const auto& map = proc_.midiMap();
      const Id buttonId = stripTarget(static_cast<int>(s), f.id);
      const bool learningLevel = map.learning() && map.learningKind() == f.kind && map.learningTarget() == f.id;
      const bool learningButton = map.learning() && map.learningTarget() == buttonId &&
                                  (map.learningKind() == MidiTargetKind::StripMute ||
                                   map.learningKind() == MidiTargetKind::StripSolo);
      const auto text = learningLevel ? TRANS("Move it...") : learningButton ? TRANS("Press it...") : TRANS("MIDI");
      if (strip.midi.getButtonText() != text) strip.midi.setButtonText(text);
    }
}

void MixerPanel::setBusCount(int buses) {
  // A rank sent to a pair that no longer exists would go quiet for a reason
  // nowhere on screen: the processor folds it back to the first one.
  proc_.setPairCount(buses);
  outputsChanged();
}

void MixerPanel::outputsChanged() {
  const int buses = juce::jmax(1, proc_.mixBusCount());
  busCount_.setSelectedId(buses, juce::dontSendNotification);
  // The output faders follow the count and the names.
  pairFaders_.clear();
  for (int i = 0; i < juce::jmin(buses, LiveLevels::kMaxPairs); ++i)
    addFader(pairFaders_, MidiTargetKind::PairLevel, i + 1, i, outputName(proc_.mixer(), i + 1));
  addMasterFader();
  fillDestinationBox(defaultDest_, proc_.mixer(), false);
  defaultDest_.setSelectedId(destinationItemOf(proc_.mixer().defaultDest), juce::dontSendNotification);
  routesChanged();
  resized();
}

void MixerPanel::routesChanged() {
  rankDest_.resize(rankIds_.size());
  for (size_t i = 0; i < rankIds_.size(); ++i) rankDest_[i] = destinationItem(rankIds_[i]);
  fillGroupPairs();
  pushRouting();
}

void MixerPanel::fillDestinations(juce::ComboBox& box) const {
  static_assert(kDefaultItem == kDestDefault && kGroupItem == kDestGroup);
  fillDestinationBox(box, proc_.mixer(), true, true);
  box.setTooltip(box.getTooltip().upToFirstOccurrenceOf("\n", false, false) + "\n" + TRANS("Default: %1").replace(
                     "%1", destinationName(proc_.mixer(), proc_.mixer().defaultDest)));
}

int MixerPanel::destinationItem(Id rankId) const {
  const auto& mixer = proc_.mixer();
  const auto routed = mixer.rankRoutings.find(rankId);
  if (routed == mixer.rankRoutings.end()) return kDefaultItem;
  const auto& dest = routed->second.perspectives[0].dest;
  if (const int* g = std::get_if<int>(&dest))
    return mixer.group(*g) != nullptr ? kGroupItem + *g : 1;
  const int pair = std::get<BusId>(dest).value;
  return pair >= 1 && pair <= juce::jmax(1, proc_.mixBusCount()) ? pair : 1;
}

void MixerPanel::sendRanks(const std::function<bool(size_t)>& which, int item) {
  if (item <= 0) return;
  for (size_t i = 0; i < rankIds_.size() && i < rankDest_.size(); ++i)
    if (which(i)) rankDest_[i] = item;
  pushRouting();
}

void MixerPanel::fillGroupPairs() {
  for (auto* list : {&groupPairs_, &divisionDests_, &mixDests_})
    for (auto& box : *list) fillDestinations(*box);
  showGroupPairs();
}

bool MixerPanel::rankInMix(size_t row, int mixId) const {
  if (row >= rankIds_.size()) return false;
  for (const auto& m : proc_.rankMixes())
    if (m.id == mixId) return std::find(m.ranks.begin(), m.ranks.end(), rankIds_[row]) != m.ranks.end();
  return false;
}

void MixerPanel::mixesChanged() {
  mixFaders_.clear();
  mixDests_.clear();
  mixIdOf_.clear();
  for (const auto& m : proc_.rankMixes()) {
    const auto name = juce::String::fromUTF8(m.name.c_str());
    addFader(mixFaders_, MidiTargetKind::MixLevel, m.id, m.id - 1,
             name.isEmpty() ? TRANS("Mix %1").replace("%1", juce::String(m.id)) : name,
             TRANS("%1 ranks").replace("%1", juce::String(static_cast<int>(m.ranks.size()))) + ". " +
                 TRANS("To rename the mix, double-click its name; to remove it, right-click its strip."));
    // Named and removed here, on its strip: one place for mixes (#1188).
    const int mixId = m.id;
    mixFaders_.back().strip->setRenamable([this, mixId](const juce::String& text) {
      auto mixes = proc_.rankMixes();
      for (auto& x : mixes)
        if (x.id == mixId) x.name = text.toStdString();
      proc_.setRankMixes(mixes);
    });
    mixFaders_.back().strip->onMenu = [this, mixId] { mixMenu(mixId); };
    auto box = std::make_unique<juce::ComboBox>();
    box->setTextWhenNothingSelected(TRANS("Several"));
    box->setTooltip(TRANS("Sends every rank of this mix to one output or speaker group"));
    const size_t at = mixDests_.size();
    box->onChange = [this, at] {
      const int id = mixIdOf_[at];
      sendRanks([this, id](size_t i) { return rankInMix(i, id); }, mixDests_[at]->getSelectedId());
    };
    mixFaders_.back().strip->setTopControl(box.get());
    mixDests_.push_back(std::move(box));
    mixIdOf_.push_back(m.id);
  }
  fillGroupPairs();
  resized();
}

void MixerPanel::mixMenu(int mixId) {
  juce::PopupMenu m;
  juce::Component::SafePointer<MixerPanel> self(this);
  m.addItem(TRANS("Rename"), [self, mixId] {
    if (self == nullptr) return;
    for (size_t i = 0; i < self->mixIdOf_.size() && i < self->mixFaders_.size(); ++i)
      if (self->mixIdOf_[i] == mixId) self->mixFaders_[i].strip->editName();
  });
  m.addItem(TRANS("Remove the mix"), [self, mixId] {
    if (self == nullptr) return;
    auto mixes = self->proc_.rankMixes();
    mixes.erase(std::remove_if(mixes.begin(), mixes.end(), [mixId](const auto& x) { return x.id == mixId; }),
                mixes.end());
    self->proc_.setRankMixes(mixes);
    // After the menu's own call returns: the strip it came from goes.
    juce::MessageManager::callAsync([self] {
      if (self != nullptr) self->mixesChanged();
    });
  });
  m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

// Each perspective's and division's box shows the destination its ranks
// share, or nothing while they play through several.
void MixerPanel::showGroupPairs() {
  const auto show = [this](juce::ComboBox& box, const std::function<bool(size_t)>& member) {
    int shared = 0;
    bool mixed = false;
    for (size_t i = 0; i < rankIds_.size() && i < rankDest_.size() && !mixed; ++i) {
      if (!member(i)) continue;
      const int item = rankDest_[i];
      if (shared == 0) shared = item;
      else if (item != shared) mixed = true;
    }
    box.setSelectedId(mixed ? 0 : shared, juce::dontSendNotification);
  };
  for (size_t g = 0; g < groupPairs_.size(); ++g)
    show(*groupPairs_[g], [this, g](size_t i) {
      return proc_.liveGroupOfRank(proc_.liveRankIndexOf(rankIds_[i])) == static_cast<int>(g);
    });
  for (size_t d = 0; d < divisionDests_.size(); ++d)
    show(*divisionDests_[d], [this, d](size_t i) {
      const int division = proc_.liveDivisionOfRank(proc_.liveRankIndexOf(rankIds_[i]));
      const auto& of = divisionIndexOf_[d];
      return std::find(of.begin(), of.end(), division) != of.end();
    });
  for (size_t m = 0; m < mixDests_.size(); ++m)
    show(*mixDests_[m], [this, m](size_t i) { return rankInMix(i, mixIdOf_[m]); });
}

void MixerPanel::pushRouting() {
  std::vector<std::pair<Id, std::variant<BusId, int>>> routes;
  std::vector<Id> toDefault;
  routes.reserve(rankIds_.size());
  for (size_t i = 0; i < rankIds_.size() && i < rankDest_.size(); ++i) {
    const int item = rankDest_[i];
    if (item == kDefaultItem || item <= 0) toDefault.push_back(rankIds_[i]);
    else if (item > kGroupItem) routes.emplace_back(rankIds_[i], item - kGroupItem);
    else routes.emplace_back(rankIds_[i], BusId{item});
  }
  proc_.setRankRoutes(routes);
  proc_.followDefault(toDefault);
  const auto& mixer = proc_.mixer();
  const auto d = validateMixer(proc_.organModel(), mixer);
  juce::String text = TRANS("%1 ranks, %2 routed by hand")
                          .replace("%1", juce::String(static_cast<int>(rankIds_.size())))
                          .replace("%2", juce::String(static_cast<int>(routes.size())));
  if (!d.ranksRoutedToMissingBus.empty())
    text += "  -  " + TRANS("%1 stale").replace("%1", juce::String((int)d.ranksRoutedToMissingBus.size()));
  status_.setText(text, juce::dontSendNotification);
  showOutputs();
  showGroupPairs();
}

void MixerPanel::showOutputs() {
  // Each pair's device channels, counted from 1 as a device's own panel
  // counts them, and what the device gives the program.
  // A run of pairs on consecutive channels is one entry, "Pairs 1-24: 1-48",
  // so that a 24-pair interface fits on the line (#1007).
  const int outs = proc_.getTotalNumOutputChannels();
  juce::StringArray pairs;
  bool beyond = false;
  int runFirst = 0, runLast = 0, runFrom = 0, runTo = 0;
  const auto endRun = [&] {
    if (runFirst == 0) return;
    pairs.add(runLast == runFirst ? TRANS("Output %1: %2-%3")
                                        .replace("%1", juce::String(runFirst))
                                        .replace("%2", juce::String(runFrom + 1))
                                        .replace("%3", juce::String(runTo + 1))
                                  : TRANS("Outputs %1-%2: %3-%4")
                                        .replace("%1", juce::String(runFirst))
                                        .replace("%2", juce::String(runLast))
                                        .replace("%3", juce::String(runFrom + 1))
                                        .replace("%4", juce::String(runTo + 1)));
    runFirst = 0;
  };
  for (const auto& b : proc_.mixer().buses) {
    if (b.deviceChannels.empty()) continue;
    const int left = b.deviceChannels[0];
    const int right = b.deviceChannels.size() > 1 ? b.deviceChannels[1] : left;
    if (left >= outs || right >= outs) beyond = true;
    // A named output, a mono one or one with copies says all its channels.
    if (b.mono || b.deviceChannels.size() > 2 || !b.name.empty()) {
      endRun();
      pairs.add(outputName(proc_.mixer(), b.id.value) + ": " + channelsText(b.deviceChannels, b.mono) +
                (b.mono ? " (" + TRANS("mono") + ")" : juce::String()));
      continue;
    }
    if (runFirst != 0 && b.id.value == runLast + 1 && left == runTo + 1 && right == left + 1 &&
        runTo - runFrom == 2 * (runLast - runFirst) + 1) {
      runLast = b.id.value;
      runTo = right;
      continue;
    }
    endRun();
    runFirst = runLast = b.id.value;
    runFrom = left;
    runTo = right;
  }
  endRun();
  juce::String text = pairs.joinIntoString(", ");
  if (beyond)
    text += "  -  " + TRANS("the device gives %1 channels: the outputs past them play on 1-2")
                          .replace("%1", juce::String(outs));
  outputs_.setText(text, juce::dontSendNotification);
}

void MixerPanel::refresh() {
  rankIds_.clear();
  rankDest_.clear();
  pairFaders_.clear();
  divisionFaders_.clear();
  groupFaders_.clear();
  groupPairs_.clear();
  divisionDests_.clear();
  divisionIndexOf_.clear();
  busHolder_.removeAllChildren();
  for (auto& title : sectionTitles_) busHolder_.addChildComponent(title);
  mixFaders_.clear();
  mixDests_.clear();
  mixIdOf_.clear();

  for (int i = 0; i < juce::jmin(juce::jmax(1, proc_.mixBusCount()), LiveLevels::kMaxPairs); ++i)
    addFader(pairFaders_, MidiTargetKind::PairLevel, i + 1, i, outputName(proc_.mixer(), i + 1));
  addMasterFader();
  fillDestinationBox(defaultDest_, proc_.mixer(), false);
  defaultDest_.setSelectedId(destinationItemOf(proc_.mixer().defaultDest), juce::dontSendNotification);
  std::vector<juce::String> divisionNames;
  {
    std::vector<Id> divisions;
    for (const auto& [id, d] : proc_.organModel().divisions) divisions.push_back(id);
    std::sort(divisions.begin(), divisions.end());
    // A strip for each division with pipes. A division of noises alone has
    // none: a noise's level is its rank's, on the Voicing tab (#1023).
    std::map<int, std::pair<int, int>> ranksIn;  // live division index: ranks, noises
    for (const auto& [rankId, rank] : proc_.organModel().ranks) {
      auto& n = ranksIn[proc_.liveDivisionOfRank(proc_.liveRankIndexOf(rankId))];
      ++n.first;
      if (rank.isNoise) ++n.second;
    }
    const auto addDivision = [this](Id id, int index, const juce::String& name, std::vector<int> more) {
      addFader(divisionFaders_, MidiTargetKind::DivisionLevel, id, index, name);
      divisionFaders_.back().more = more;
      // The division's destination: every rank of it to one output or group.
      auto box = std::make_unique<juce::ComboBox>();
      box->setTextWhenNothingSelected(TRANS("Several"));
      box->setTooltip(TRANS("Sends every rank of this division to one output or speaker group"));
      const size_t at = divisionDests_.size();
      box->onChange = [this, at] {
        sendRanks(
            [this, at](size_t i) {
              const int division = proc_.liveDivisionOfRank(proc_.liveRankIndexOf(rankIds_[i]));
              const auto& of = divisionIndexOf_[at];
              return std::find(of.begin(), of.end(), division) != of.end();
            },
            divisionDests_[at]->getSelectedId());
      };
      divisionFaders_.back().strip->setTopControl(box.get());
      divisionDests_.push_back(std::move(box));
      more.insert(more.begin(), index);
      divisionIndexOf_.push_back(std::move(more));
    };
    for (Id id : divisions) {
      const auto& d = proc_.organModel().divisions.at(id);
      divisionNames.push_back(d.name.empty() ? juce::String(static_cast<int>(id)) : juce::String(d.name));
      const int index = proc_.liveDivisionIndexOf(id);
      const auto n = ranksIn.find(index);
      if (n == ranksIn.end() || n->second.first == 0) continue;
      if (n->second.second == n->second.first) continue;
      addDivision(id, index, divisionNames.back(), {});
    }
  }
  // The perspectives, front against rear in one move (#930), or a
  // GrandOrgue set's windchest groups, named as the set names them.
  {
    const auto& names = proc_.liveGroupNames();
    for (size_t g = 0; g < names.size(); ++g) {
      juce::String name(juce::CharPointer_UTF8(names[g].c_str()));
      name = name.substring(0, 1).toUpperCase() + name.substring(1);
      addFader(groupFaders_, MidiTargetKind::GroupLevel, static_cast<Id>(g + 1), static_cast<int>(g), name);
      auto box = std::make_unique<juce::ComboBox>();
      box->setTextWhenNothingSelected(TRANS("Several"));
      box->setTooltip(TRANS("Sends every rank of this group to one output or speaker group"));
      box->onChange = [this, g] {
        sendRanks(
            [this, g](size_t i) {
              return proc_.liveGroupOfRank(proc_.liveRankIndexOf(rankIds_[i])) == static_cast<int>(g);
            },
            groupPairs_[g]->getSelectedId());
      };
      groupFaders_.back().strip->setTopControl(box.get());
      groupPairs_.push_back(std::move(box));
    }
    fillGroupPairs();
  }

  // By division, a stop's tremmed ranks with its others, then by id.
  const auto divisionOf = [this](Id rankId) {
    const int d = proc_.liveDivisionOfRank(proc_.liveRankIndexOf(rankId));
    return d < 0 ? std::numeric_limits<int>::max() : d;
  };
  for (const auto& [id, rank] : proc_.organModel().ranks) rankIds_.push_back(id);
  std::sort(rankIds_.begin(), rankIds_.end(), [&divisionOf](Id a, Id b) {
    const int da = divisionOf(a), db = divisionOf(b);
    return da != db ? da < db : a < b;
  });
  for (Id rankId : rankIds_) rankDest_.push_back(destinationItem(rankId));

  const int buses = juce::jmax(1, proc_.mixBusCount());
  busCount_.setSelectedId(buses, juce::dontSendNotification);

  mixesChanged();
  pushRouting();
  fillSourceTabs();
  resized();
}

std::vector<MixerPanel::Fader>& MixerPanel::sourceFaders(Source s) {
  switch (s) {
    case Source::Divisions: return divisionFaders_;
    case Source::Groups: return groupFaders_;
    case Source::Mixes: return mixFaders_;
    case Source::Outputs: break;
  }
  return pairFaders_;
}

void MixerPanel::fillSourceTabs() {
  // A tab for each kind the organ has; Mixes always, where mixes are made;
  // the outputs last, as the desk's masters.
  const Source was = shownSource_;
  sources_.clearTabs();
  sourceOfTab_.clear();
  // A tab in a window of its own is left out until the window closes.
  const auto colour = findColour(juce::ResizableWindow::backgroundColourId);
  const auto add = [&](Source s, bool has) {
    if (!has || isDetached(s)) return;
    sources_.addTab(sourceName(s), colour, -1);
    sourceOfTab_.push_back(s);
  };
  add(Source::Divisions, !divisionFaders_.empty());
  add(Source::Groups, !groupFaders_.empty());
  add(Source::Mixes, true);
  add(Source::Outputs, true);
  detach_.setEnabled(!sourceOfTab_.empty());
  if (sourceOfTab_.empty()) return;
  int index = 0;
  for (size_t t = 0; t < sourceOfTab_.size(); ++t)
    if (sourceOfTab_[t] == was) index = static_cast<int>(t);
  shownSource_ = sourceOfTab_[static_cast<size_t>(index)];
  sources_.setCurrentTabIndex(index, false);
}

void MixerPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  heading_.setBounds(r.removeFromTop(kRow));
  // Two rows. One row of five controls overflowed the dialog and squeezed the
  // last button until its label wrapped inside it.
  auto row = r.removeFromTop(kRow);
  busesLabel_.setBounds(row.removeFromLeft(110));
  busCount_.setBounds(row.removeFromLeft(130).reduced(0, 1));
  row.removeFromLeft(kGap);
  defaultLabel_.setBounds(row.removeFromLeft(140));
  row.removeFromLeft(6);
  defaultDest_.setBounds(row.removeFromLeft(220).reduced(0, 1));
  row.removeFromLeft(kGap);
  status_.setBounds(row);
  r.removeFromTop(4);

  row = r.removeFromTop(kRow);
  for (auto [b, w] : std::initializer_list<std::pair<juce::Button*, int>>{
           {&channels_, 110}, {&speakerGroups_, 140}, {&routing_, 130}, {&outputEq_, 110}, {&crossover_, 110},
           {&spread_, 170}, {&reset_, 150},
           {&save_, 160}}) {
    b->setBounds(row.removeFromLeft(w).reduced(0, 1));
    row.removeFromLeft(6);
  }
  r.removeFromTop(4);
  outputs_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);

  // The strips of the open tab, the full width.
  constexpr int band = 22;  // the tabs

  auto tabRow = r.removeFromTop(band + 4);
  narrow_.setBounds(tabRow.removeFromRight(130));
  tabRow.removeFromRight(6);
  if (!kMobile) detach_.setBounds(tabRow.removeFromRight(110).reduced(0, 2));
  tabRow.removeFromRight(kGap);
  sources_.setBounds(tabRow);
  busViewport_.setBounds(r);

  // Every kind side by side while they all fit the width, and more than one
  // is here; the tabs otherwise.
  {
    const auto sections = sectionsWithStrips();
    allInView_ = sections.size() > 1 && stripsWidth(sections) <= r.getWidth();
    sources_.setVisible(!allInView_);
  }

  // The open tab's strips, or all of them, and each tab's in its own window;
  // the others hidden.
  {
    const bool anyTab = !sourceOfTab_.empty();
    for (Source s : {Source::Divisions, Source::Groups, Source::Mixes, Source::Outputs}) {
      const bool detached = isDetached(s);
      for (auto& f : sourceFaders(s)) f.strip->setVisible(detached || allInView_ || (anyTab && s == shownSource_));
      if (detached) {
        auto& w = *stripWindows_[static_cast<size_t>(s)];
        layOutStrips(s, w.view, w.holder);
      }
    }
    for (auto& title : sectionTitles_) title.setVisible(false);
    if (allInView_) {
      emptySource_.setVisible(false);
      layOutAll();
      return;
    }
    if (!anyTab) {
      busHolder_.setSize(1, 1);
      emptySource_.setVisible(true);
      emptySource_.setText(TRANS("Every tab is open in a window of its own."), juce::dontSendNotification);
      emptySource_.setBounds(r.withHeight(3 * kRow));
      emptySource_.toFront(false);
      return;
    }
    layOutStrips(shownSource_, busViewport_, busHolder_);
    auto& faders = sourceFaders(shownSource_);
    emptySource_.setVisible(faders.empty());
    emptySource_.setText(shownSource_ == Source::Mixes
                             ? TRANS("A mix is a set of ranks with a fader of its own. To make one, open Rank "
                                     "routing, tick the ranks and choose a mix for them.")
                             : juce::String(),
                         juce::dontSendNotification);
    emptySource_.setBounds(r.withHeight(3 * kRow));
    if (faders.empty()) emptySource_.toFront(false);
  }
}

// ------------------------------------------------------- console display

namespace {
juce::File logFile() {
  if (auto* logger = dynamic_cast<juce::FileLogger*>(juce::Logger::getCurrentLogger()))
    return logger->getLogFile();
  return {};
}
}  // namespace

juce::String diagnosticsReport(MasterpieceProcessor& p) {
  juce::StringArray out;
  // 1 passed, -1 failed, 0 a fact with nothing to pass.
  const auto row = [&out](int state, const juce::String& what) {
    out.add(juce::String(state > 0 ? "ok      " : state < 0 ? "FAILED  " : "        ") + what);
  };
  // A folder Masterpiece has to be able to write to.
  const auto folder = [&row](const juce::String& name, const juce::File& dir) {
    if (dir == juce::File()) {
      row(-1, name + ": none");
      return;
    }
    dir.createDirectory();
    const auto probe = dir.getChildFile(".masterpiece-check");
    const bool wrote = probe.replaceWithText("check") && probe.loadFileAsString() == "check";
    probe.deleteFile();
    const double freeGb = static_cast<double>(dir.getBytesFreeOnVolume()) / (1024.0 * 1024.0 * 1024.0);
    row(wrote ? 1 : -1, name + ": " + dir.getFullPathName() +
                            (wrote ? ", can be written, " : ", CANNOT be written, ") +
                            juce::String(freeGb, 1) + " GB free");
  };
  const auto readable = [](const juce::File& f) {
    if (f.isDirectory()) return true;
    juce::FileInputStream in(f);
    return in.openedOk();
  };

  juce::String version = "plug-in";
  if (juce::JUCEApplicationBase::getInstance() != nullptr) version = buildLabel();
  out.add("Masterpiece " + version + ", " + juce::SystemStats::getOperatingSystemName() + ", " +
          juce::SystemStats::getDeviceDescription());
  out.add(juce::String(juce::SystemStats::getNumCpus()) + " processors, " +
          juce::String(juce::SystemStats::getMemorySizeInMegabytes()) + " MB of memory");
  out.add("Checked " + juce::Time::getCurrentTime().toISO8601(true));
  out.add({});

  const auto data = mp::dataFolder();
  folder("Settings folder", data);
  folder("Sample cache", p.cacheDirectory());
  if (p.cacheDirectorySetting() != juce::File() && p.cacheDirectorySetting() != p.cacheDirectory())
    row(-1, "The cache folder chosen in Loading cannot be used, so the usual one is: " +
                p.cacheDirectorySetting().getFullPathName());
  folder("Temporary folder", juce::File::getSpecialLocation(juce::File::tempDirectory));
  {
    juce::int64 bytes = 0;
    const auto caches = p.cacheDirectory().findChildFiles(juce::File::findFiles, false, "*.mpcache");
    for (const auto& f : caches) bytes += f.getSize();
    row(0, "Cache files: " + juce::String(caches.size()) + ", " +
               juce::String(static_cast<double>(bytes) / (1024.0 * 1024.0), 0) + " MB");
    row(0, "Organs unpacked from their archives: " +
               juce::String(data.getChildFile("Packaged").getNumberOfChildFiles(juce::File::findDirectories)));
  }
  {
    const auto log = logFile();
    if (log == juce::File()) row(-1, "No log is being written");
    else row(log.existsAsFile() ? 1 : -1, "Log: " + log.getFullPathName() + ", " +
                                             juce::String(log.getSize() / 1024) + " KB");
  }

  const double rate = p.getSampleRate();
  row(rate > 0.0 ? 1 : -1,
      rate > 0.0 ? "Audio: " + juce::String(rate, 0) + " Hz, blocks of " + juce::String(p.getBlockSize()) + " frames"
                 : juce::String("Audio: not running"));
  {
    juce::StringArray names;
    for (const auto& d : juce::MidiInput::getAvailableDevices()) names.add(d.name);
    row(0, "MIDI inputs: " + (names.isEmpty() ? juce::String("none") : names.joinIntoString(", ")));
  }

  const auto odf = p.loadedOrganFile();
  if (odf == juce::File()) {
    row(0, "No organ is loaded");
  } else {
    row(odf.existsAsFile() ? 1 : -1, "Organ: " + odf.getFullPathName() +
                                         (odf.existsAsFile() ? "" : " (the file is no longer there)"));
    row(readable(odf) ? 1 : -1, readable(odf) ? "The organ's definition can be read"
                                              : "The organ's definition CANNOT be read");
    const juce::File organRoot(p.organRootDir());
    row(organRoot.isDirectory() ? 1 : -1, "Organ folder: " + organRoot.getFullPathName());
    const auto& model = p.organModel();
    row(0, juce::String(static_cast<int>(model.ranks.size())) + " ranks, " +
               juce::String(static_cast<int>(model.stops.size())) + " stops, " +
               juce::String(static_cast<int>(model.displayPages.size())) + " console pages");
  }
  const auto storage = p.sampleStorage();
  row(0, juce::String("Samples kept as ") +
             (storage == SampleStorage::Int16   ? "16-bit"
              : storage == SampleStorage::Int20 ? "20-bit"
              : storage == SampleStorage::Int24 ? "as recorded (16 or 24-bit)"
                                                : "32-bit float") +
             ", memory limit " +
             (p.memoryLimitSettingMB() > 0 ? juce::String(p.memoryLimitSettingMB()) + " MB" : juce::String("not set")) +
             ", " + juce::String(p.streamUnderruns()) + " stream underruns");
  // Where the system ends a program at a share of its own (iOS), that share:
  // what a report from a device needs first (#650).
  if (const int64_t ceiling = processMemoryCeilingBytes(); ceiling > 0)
    row(0, "The system allows this program " + juce::String(ceiling / (1024 * 1024)) + " MB; loads stop at " +
               juce::String(p.memoryLimitBytes() / (1024 * 1024)) + " MB");
  row(0, "Impulse response: " + (p.convolver().hasImpulseResponse() ? p.convolver().impulseResponseName()
                                                                    : juce::String("none")));
 #if JUCE_IOS
  const auto held = heldDocuments();
  row(0, "Documents lent by the Files app: " + juce::String(held.size()));
  for (const auto& path : held) {
    const juce::File f(path);
    const bool there = f.exists() && readable(f);
    row(there ? 1 : -1, "  " + path + (there ? "" : " (cannot be read now)"));
  }
 #endif
  // The organ's wind system and tremulants as the program read them: what a
  // report of a set that sounds wrong with the wind model on needs, by id and
  // name, with nothing of the definition's own file.
  if (odf != juce::File()) {
    const auto& sw = p.engineSwitch();
    out.add({});
    out.add(juce::String("Wind model ") + (sw.enableWindModel && !sw.simpleWavOnly ? "on" : "off") +
            ", moving reservoir boards " + (sw.enableBellowsBoard ? "on" : "off") + ", tremulants " +
            (sw.enableTremulant && !sw.simpleWavOnly ? "on" : "off") + ", wind depth " +
            juce::String(p.windDepth(), 2));
    const auto& wind = p.wind();
    const auto pressures = [&wind](mp::Id id) { return std::make_pair(wind.nominalFor(id), wind.pressureFor(id)); };
    out.addLines(juce::String::fromUTF8(mp::windAndTremulantReport(p.organModel(), pressures).c_str()).trimEnd());
  }
  return out.joinIntoString("\n");
}

void LogPanel::runChecks() { report_.setText(diagnosticsReport(proc_), false); }

juce::String LogPanel::reportWithLog() {
  if (report_.isEmpty()) runChecks();
  juce::StringArray lines;
  lines.addLines(logFile().loadFileAsString());
  const int from = juce::jmax(0, lines.size() - 200);
  lines.removeRange(0, from);
  return report_.getText() + "\n\n---- the end of the log ----\n" + lines.joinIntoString("\n");
}

LogPanel::LogPanel(MasterpieceProcessor& p) : proc_(p) {
  const juce::File file = logFile();
  check_.setTooltip(TRANS("The folders Masterpiece writes to, the audio, the organ and its files"));
  check_.onClick = [this] { runChecks(); };
  addAndMakeVisible(check_);
  copy_.setTooltip(TRANS("The checks and the last 200 lines of the log, to paste into a bug report"));
  copy_.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(reportWithLog()); };
  addAndMakeVisible(copy_);
  send_.onClick = [this] {
    const auto report = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("masterpiece-report.txt");
    mp::writeTextFile(report, reportWithLog());
    juce::Array<juce::URL> files;
    files.add(juce::URL(report));
    const auto log = logFile();
    if (log.existsAsFile()) files.add(juce::URL(log));
    const auto previous = log.getSiblingFile("masterpiece.previous.log");
    if (previous.existsAsFile()) files.add(juce::URL(previous));
    sending_ = juce::ContentSharer::shareFilesScoped(files, [](bool, const juce::String&) {}, this);
  };
  // Only a phone or a tablet has a share sheet; a desktop opens the folder.
  addChildComponent(send_);
  send_.setVisible(kMobile);
  report_.setMultiLine(true);
  report_.setReadOnly(true);
  report_.setScrollbarsShown(true);
  report_.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
  report_.setTextToShowWhenEmpty(TRANS("Run the checks to see what Masterpiece finds on this device."),
                                 juce::Colours::grey);
  addAndMakeVisible(report_);
  about_.setText(TRANS("Masterpiece always keeps a log of this session, and the previous one beside "
                       "it. Attach it to a bug report.") +
                     "\n\n" +
                     (file == juce::File() ? TRANS("No log is being written.")
                                           : file.getFullPathName()),
                 juce::dontSendNotification);
  about_.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(about_);
  open_.setEnabled(file != juce::File());
  open_.onClick = [file] { file.revealToUser(); };
  addChildComponent(open_);
  open_.setVisible(!kMobile);
  midi_.setToggleState(proc_.midiLogging(), juce::dontSendNotification);
  midi_.setTooltip(TRANS("For keys or pedals that do nothing, or the wrong thing. Until Masterpiece is closed."));
  midi_.onClick = [this] { proc_.setMidiLogging(midi_.getToggleState()); };
  addAndMakeVisible(midi_);
  releases_.setToggleState(proc_.releaseLogging(), juce::dontSendNotification);
  releases_.setTooltip(TRANS("For a click, a noise or a note that goes on sounding when keys are let go. "
                             "Until Masterpiece is closed."));
  releases_.onClick = [this] { proc_.setReleaseLogging(releases_.getToggleState()); };
  addAndMakeVisible(releases_);
  updates_.setToggleState(proc_.updateCheckEnabled(), juce::dontSendNotification);
  updates_.setTooltip(TRANS("Reads the newest version's number from GitHub and offers its download page "
                            "when it is newer. Nothing about you is sent."));
  updates_.onClick = [this] { proc_.setUpdateCheckEnabled(updates_.getToggleState()); };
  addAndMakeVisible(updates_);
}

void LogPanel::resized() {
  auto r = getLocalBounds().reduced(16);
  about_.setBounds(r.removeFromTop(80));
  auto buttons = r.removeFromTop(kMobile ? 40 : 30);
  if (open_.isVisible()) open_.setBounds(buttons.removeFromLeft(180).reduced(0, 1));
  buttons.removeFromLeft(open_.isVisible() ? 8 : 0);
  check_.setBounds(buttons.removeFromLeft(130).reduced(0, 1));
  buttons.removeFromLeft(8);
  copy_.setBounds(buttons.removeFromLeft(140).reduced(0, 1));
  buttons.removeFromLeft(8);
  if (send_.isVisible()) send_.setBounds(buttons.removeFromLeft(220).reduced(0, 1));
  r.removeFromTop(12);
  midi_.setBounds(r.removeFromTop(28));
  releases_.setBounds(r.removeFromTop(28));
  updates_.setBounds(r.removeFromTop(28));
  r.removeFromTop(8);
  report_.setBounds(r);
}

RemotePanel::RemotePanel(MasterpieceProcessor& p) : proc_(p) {
  const auto& s = proc_.remoteSettings();
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Remote control"));
  addAndMakeVisible(enable_);
  enable_.setToggleState(s.enabled, juce::dontSendNotification);
  enable_.setTooltip(TRANS("A page any browser on this network can open to draw stops, press pistons and move "
                           "the shoes, and the API that consoles and scripts use (HTTP, JSON). Off by default. "
                           "While it is on, anyone on this network can open the page; without a token, "
                           "anyone can also change the registration from it."));
  enable_.onClick = [this] { apply(); };
  addAndMakeVisible(write_);
  write_.setToggleState(s.allowChanges, juce::dontSendNotification);
  write_.setTooltip(TRANS("Off, the network can only read: the stops drawn, the levels, the organ loaded."));
  write_.onClick = [this] { apply(); };
  addAndMakeVisible(power_);
  power_.setToggleState(s.allowPowerOff, juce::dontSendNotification);
  power_.setTooltip(TRANS("The quit and power-off commands, for a console with no screen. Refused in the first "
                          "two minutes after Masterpiece starts, so a faulty button cannot shut the computer down "
                          "at every start. Masterpiece saves before it quits."));
  power_.onClick = [this] { apply(); };
  addAndMakeVisible(local_);
  local_.setToggleState(s.thisComputerOnly, juce::dontSendNotification);
  local_.setTooltip(TRANS("The page, the API and OSC answer on this computer only, at 127.0.0.1. No other "
                          "device on the network reaches them, and Windows asks for no firewall rule."));
  local_.onClick = [this] { apply(); };
  addAndMakeVisible(sysex_);
  sysex_.setToggleState(s.sysex, juce::dontSendNotification);
  sysex_.setTooltip(TRANS("For a console with a MIDI cable and no network: requests read on the MIDI inputs, "
                          "replies and changes sent on the console's MIDI output."));
  sysex_.onClick = [this] { apply(); };
  addAndMakeVisible(osc_);
  osc_.setToggleState(s.osc, juce::dontSendNotification);
  osc_.setTooltip(TRANS("For control surfaces and apps that speak OSC, such as TouchOSC, on the UDP port "
                        "beside it."));
  osc_.onClick = [this] { apply(); };
  addAndMakeVisible(oscPortLabel_);
  oscPortLabel_.setText(TRANS("Port"), juce::dontSendNotification);
  addAndMakeVisible(oscPort_);
  oscPort_.setInputRestrictions(5, "0123456789");
  oscPort_.setText(juce::String(s.oscPort), juce::dontSendNotification);
  oscPort_.onReturnKey = oscPort_.onFocusLost = [this] { apply(); };
  addAndMakeVisible(portLabel_);
  portLabel_.setText(TRANS("Port"), juce::dontSendNotification);
  addAndMakeVisible(port_);
  port_.setInputRestrictions(5, "0123456789");
  port_.setText(juce::String(s.port), juce::dontSendNotification);
  port_.onReturnKey = port_.onFocusLost = [this] { apply(); };
  addAndMakeVisible(tokenLabel_);
  tokenLabel_.setText(TRANS("Token"), juce::dontSendNotification);
  addAndMakeVisible(token_);
  token_.setPasswordCharacter(0x2022);
  token_.setText(juce::String::fromUTF8(s.token.c_str()), juce::dontSendNotification);
  token_.setTooltip(TRANS("When set, a change needs it: the web page asks for it under Access, a console "
                          "sends it as Authorization: Bearer and the token. Reading needs none."));
  token_.onReturnKey = token_.onFocusLost = [this] { apply(); };
  addAndMakeVisible(status_);
  status_.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(open_);
  open_.onClick = [this] {
    juce::URL("http://localhost:" + juce::String(proc_.remoteSettings().port) + "/").launchInDefaultBrowser();
  };
  timerCallback();
  startTimerHz(1);
}

void RemotePanel::apply() {
  MasterpieceProcessor::RemoteSettings s;
  s.enabled = enable_.getToggleState();
  s.allowChanges = write_.getToggleState();
  s.allowPowerOff = power_.getToggleState();
  s.thisComputerOnly = local_.getToggleState();
  s.port = juce::jlimit(1, 65535, port_.getText().getIntValue() > 0 ? port_.getText().getIntValue() : 8080);
  port_.setText(juce::String(s.port), juce::dontSendNotification);
  s.token = token_.getText().trim().toStdString();
  const auto& now = proc_.remoteSettings();
  s.sysex = sysex_.getToggleState();
  s.sysexUnit = now.sysexUnit;
  s.osc = osc_.getToggleState();
  s.oscPort = juce::jlimit(1, 65535, oscPort_.getText().getIntValue() > 0 ? oscPort_.getText().getIntValue() : 9000);
  oscPort_.setText(juce::String(s.oscPort), juce::dontSendNotification);
  if (s.enabled == now.enabled && s.allowChanges == now.allowChanges && s.port == now.port && s.token == now.token &&
      s.allowPowerOff == now.allowPowerOff && s.thisComputerOnly == now.thisComputerOnly && s.sysex == now.sysex &&
      s.osc == now.osc && s.oscPort == now.oscPort)
    return;
  proc_.setRemoteSettings(s);
  timerCallback();
}

void RemotePanel::timerCallback() {
  status_.setText(proc_.remoteStatus ? proc_.remoteStatus() : juce::String(), juce::dontSendNotification);
  open_.setEnabled(proc_.remoteSettings().enabled);
  if (proc_.simpleMode() != shownSimple_) resized();
}

void RemotePanel::resized() {
  // Simple mode keeps the web console in the browser; quitting from the
  // network, MIDI and OSC are for Advanced (#634).
  shownSimple_ = proc_.simpleMode();
  for (juce::Component* c : std::initializer_list<juce::Component*>{&power_, &local_, &sysex_, &osc_, &oscPort_,
                                                                     &oscPortLabel_})
    c->setVisible(!shownSimple_);
  auto r = getLocalBounds().reduced(16);
  heading_.setBounds(r.removeFromTop(28));
  r.removeFromTop(6);
  enable_.setBounds(r.removeFromTop(28));
  write_.setBounds(r.removeFromTop(28));
  if (!shownSimple_) {
    power_.setBounds(r.removeFromTop(28));
    local_.setBounds(r.removeFromTop(28));
    sysex_.setBounds(r.removeFromTop(28));
  }
  if (!shownSimple_) {
    auto oscRow = r.removeFromTop(28);
    oscPort_.setBounds(oscRow.removeFromRight(90).reduced(0, 2));
    oscPortLabel_.setBounds(oscRow.removeFromRight(50));
    osc_.setBounds(oscRow);
  }
  r.removeFromTop(6);
  auto row = r.removeFromTop(28);
  portLabel_.setBounds(row.removeFromLeft(60));
  port_.setBounds(row.removeFromLeft(90).reduced(0, 2));
  row.removeFromLeft(20);
  tokenLabel_.setBounds(row.removeFromLeft(60));
  token_.setBounds(row.removeFromLeft(220).reduced(0, 2));
  r.removeFromTop(10);
  status_.setBounds(r.removeFromTop(70));
  open_.setBounds(r.removeFromTop(30).removeFromLeft(200).reduced(0, 1));
  r.removeFromTop(12);
}

DisplayPanel::DisplayPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Console display"));
  addAndMakeVisible(note_);
  styleNote(note_, TRANS("For a text display fitted to the console: it shows the lines chosen below, sent as "
                         "system exclusive messages to the MIDI output. The display's own documentation gives "
                         "its number, its line length and the prefix it expects."));
  // Wrapped at full size: a Label squeezes a long line before it wraps.
  note_.setMinimumHorizontalScale(1.0f);
  addAndMakeVisible(enable_);
  enable_.onClick = [this] { rebuild(); };

  addAndMakeVisible(idLabel_);
  styleLabel(idLabel_, TRANS("Display number"));
  addAndMakeVisible(id_);
  id_.setRange(0, 15, 1);
  id_.setValue(0, juce::dontSendNotification);
  id_.onValueChange = [this] { rebuild(); };

  addAndMakeVisible(widthLabel_);
  styleLabel(widthLabel_, TRANS("Characters per line"));
  addAndMakeVisible(width_);
  int wid = 1;
  for (int w : {16, 20, 32, 40}) width_.addItem(juce::String(w), wid++);
  width_.setSelectedId(3, juce::dontSendNotification); // 32, the usual one
  width_.onChange = [this] { rebuild(); };

  addAndMakeVisible(headerLabel_);
  styleLabel(headerLabel_, TRANS("Sys-ex prefix (hex)"));
  addAndMakeVisible(header_);
  header_.setText("7D", juce::dontSendNotification);
  header_.setInputRestrictions(23, "0123456789abcdefABCDEF ");
  header_.onReturnKey = [this] { rebuild(); };
  header_.onFocusLost = [this] { rebuild(); };

  addAndMakeVisible(linesLabel_);
  styleLabel(linesLabel_, TRANS("Lines"));
  // The fields a jamb display is worth having for. Deliberately short: a
  // player reads this at a glance between pieces, not as a report.
  const juce::String fields[] = {TRANS("(blank)"),   TRANS("Organ"),     TRANS("Temperament"),
                                 TRANS("Pitch"),     TRANS("Transpose"), TRANS("Stops drawn"),
                                 TRANS("Crescendo"), TRANS("Combination set")};
  for (size_t i = 0; i < lines_.size(); ++i) {
    lines_[i] = std::make_unique<juce::ComboBox>();
    addAndMakeVisible(*lines_[i]);
    int id = 1;
    for (const auto& f : fields) lines_[i]->addItem(f, id++);
    lines_[i]->onChange = [this] { rebuild(); };
  }
  // The setup as it was left, kept by the processor; out of the box the organ,
  // temperament, pitch and crescendo, so the first send shows it working.
  const auto& setup = proc_.lcdSetup();
  enable_.setToggleState(setup.on, juce::dontSendNotification);
  id_.setValue(setup.displayNumber, juce::dontSendNotification);
  for (int w = 0, item = 1; w < 4; ++w, ++item)
    if (const int widths[] = {16, 20, 32, 40}; widths[w] == setup.width)
      width_.setSelectedId(item, juce::dontSendNotification);
  header_.setText(setup.prefix, juce::dontSendNotification);
  for (size_t i = 0; i < lines_.size(); ++i)
    lines_[i]->setSelectedId(setup.lines[i], juce::dontSendNotification);

  addAndMakeVisible(previewLabel_);
  styleLabel(previewLabel_, TRANS("What the display reads"));
  addAndMakeVisible(preview_);
  // Monospaced and top-left: the preview is fixed-width lines, and the whole
  // point is that the columns line up the way the glass does. The bars mark
  // where the panel ends, which is what makes a truncation visible.
  preview_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                     13.0f, juce::Font::plain));
  preview_.setJustificationType(juce::Justification::topLeft);

  addAndMakeVisible(send_);
  send_.onClick = [this] { proc_.refreshLcdPanels(); };


  startTimerHz(2);
}

DisplayPanel::~DisplayPanel() { stopTimer(); }

// The setup belongs to the processor, which owns the state a line shows and
// keeps the setup; this only reads it off the boxes.
void DisplayPanel::rebuild() {
  MasterpieceProcessor::LcdSetup s;
  s.on = enable_.getToggleState();
  s.displayNumber = static_cast<int>(id_.getValue());
  const int widths[] = {16, 20, 32, 40};
  s.width = widths[juce::jlimit(0, 3, width_.getSelectedId() - 1)];
  s.prefix = header_.getText().trim();
  for (size_t i = 0; i < lines_.size(); ++i) s.lines[i] = juce::jlimit(1, 8, lines_[i]->getSelectedId());
  proc_.setLcdSetup(s);
}

void DisplayPanel::timerCallback() {
  // The preview. The display itself is sent from the main window, four times
  // a second, whether or not this tab is open.
  if (!enable_.getToggleState()) {
    preview_.setText("", juce::dontSendNotification);
    return;
  }
  juce::String text;
  const LcdState st = proc_.lcdState();
  for (const auto& panel : proc_.lcdPanels().panels())
    for (const auto& line : panel.lines)
      text << "|" << LcdPanels::renderLine(line, st, panel.lineWidth) << "|\n";
  preview_.setText(text, juce::dontSendNotification);
}

void DisplayPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  const int labelW = 170;

  heading_.setBounds(r.removeFromTop(kRow));
  note_.setBounds(r.removeFromTop(wrappedHeight(note_, r.getWidth())));
  enable_.setBounds(r.removeFromTop(kRow).withTrimmedLeft(12));
  r.removeFromTop(kGap);

  auto row = r.removeFromTop(kRow);
  idLabel_.setBounds(row.removeFromLeft(labelW));
  id_.setBounds(row.removeFromLeft(110).reduced(0, 1));
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  widthLabel_.setBounds(row.removeFromLeft(labelW));
  width_.setBounds(row.removeFromLeft(110).reduced(0, 1));
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  headerLabel_.setBounds(row.removeFromLeft(labelW));
  header_.setBounds(row.removeFromLeft(160).reduced(0, 2));
  row.removeFromLeft(kGap);
  send_.setBounds(row.removeFromLeft(150).reduced(0, 1));

  // Four line pickers, two to a row: four across does not fit the dialog and
  // wrapping them is better than clipping the last one.
  r.removeFromTop(kGap);
  linesLabel_.setBounds(r.removeFromTop(kRow));
  for (size_t i = 0; i < lines_.size(); i += 2) {
    row = r.removeFromTop(kRow).withTrimmedLeft(12);
    lines_[i]->setBounds(row.removeFromLeft(200).reduced(0, 1));
    row.removeFromLeft(kGap);
    lines_[i + 1]->setBounds(row.removeFromLeft(200).reduced(0, 1));
    r.removeFromTop(2);
  }

  r.removeFromTop(kGap);
  previewLabel_.setBounds(r.removeFromTop(kRow));
  preview_.setBounds(r.removeFromTop(kRow * 3).withTrimmedLeft(12));
  r.removeFromTop(kGap);
}

// -------------------------------------------------------------- language

LanguagePanel::LanguagePanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Language"));
  addAndMakeVisible(language_);
  language_.setTooltip(TRANS("The language changes the next time Masterpiece starts. Names of "
                              "organs, stops and pages stay as the organ gives them."));
  addAndMakeVisible(note_);
  note_.setText(TRANS("Language and font apply at the next start."), juce::dontSendNotification);
  note_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  addAndMakeVisible(sizeLabel_);
  styleLabel(sizeLabel_, TRANS("Text size"));
  addAndMakeVisible(size_);
  for (int percent : {100, 115, 130, 150, 175, 200}) size_.addItem(juce::String(percent) + " %", percent);
  size_.setSelectedId(proc_.textSizeSetting(), juce::dontSendNotification);
  size_.setTooltip(TRANS("Scales the text and the controls of every window"));
  size_.onChange = [this] {
    proc_.setTextSizeSetting(size_.getSelectedId());
#if !JUCE_IOS && !JUCE_ANDROID
    juce::Desktop::getInstance().setGlobalScaleFactor(static_cast<float>(size_.getSelectedId()) / 100.0f);
#endif
  };
  addAndMakeVisible(fontLabel_);
  styleLabel(fontLabel_, TRANS("Font"));
  addAndMakeVisible(font_);
  font_.addItem(TRANS("The program's own"), 1);
  const auto fonts = juce::Font::findAllTypefaceNames();
  for (int i = 0; i < fonts.size(); ++i) font_.addItem(fonts[i], i + 2);
  font_.setSelectedId(fonts.indexOf(proc_.textFontSetting()) + 2, juce::dontSendNotification);
  font_.onChange = [this] { proc_.setTextFontSetting(font_.getSelectedId() == 1 ? juce::String() : font_.getText()); };
  language_.addItem(TRANS("System default"), 1);
  language_.addItem("English", 2);
  // Each language is named in itself, so a player who cannot read the current
  // one can still find theirs.
  int id = 3;
  for (const auto& l : availableLanguages()) {
    codes_.push_back(l.code);
    language_.addItem(l.name, id++);
  }
  const auto chosen = proc_.languageSetting();
  int selected = chosen.isEmpty() ? 1 : chosen == "en" ? 2 : 0;
  for (size_t i = 0; i < codes_.size(); ++i)
    if (codes_[i] == chosen) selected = static_cast<int>(i) + 3;
  language_.setSelectedId(selected != 0 ? selected : 1, juce::dontSendNotification);
  language_.onChange = [this] {
    const int sel = language_.getSelectedId();
    proc_.setLanguageSetting(sel == 1 ? juce::String() : sel == 2 ? juce::String("en")
                                                                  : codes_[static_cast<size_t>(sel - 3)]);
  };
}

void LanguagePanel::resized() {
  auto r = getLocalBounds().reduced(16);
  heading_.setBounds(r.removeFromTop(kRow));
  language_.setBounds(r.removeFromTop(kRow).removeFromLeft(260).reduced(0, 1));
  r.removeFromTop(kGap);
  auto row = r.removeFromTop(kRow);
  sizeLabel_.setBounds(row.removeFromLeft(100));
  size_.setBounds(row.removeFromLeft(160).reduced(0, 1));
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  fontLabel_.setBounds(row.removeFromLeft(100));
  font_.setBounds(row.removeFromLeft(260).reduced(0, 1));
  r.removeFromTop(kGap);
  note_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
}

// --------------------------------------------------------------- window

namespace {
// The tab General settings was last closed on: it opens there again (#591).
juce::String& lastSettingsTab() {
  static juce::String tab;
  return tab;
}
}  // namespace

SettingsWindow::~SettingsWindow() {
  lastSettingsTab() = tabs_.getCurrentTabName();
  // Posted, not called: the window is still being torn down here.
  if (onClosed) juce::MessageManager::callAsync(std::move(onClosed));
}

int TabNamesLook::longestWordWidth(juce::TabBarButton& button, int tabDepth) {
  // The font the tab draws its name in (LookAndFeel_V3::createTabTextLayout).
  const juce::Font font{button.withDefaultMetrics(juce::FontOptions(static_cast<float>(tabDepth) * 0.5f))};
  int longest = 0;
  for (const auto& word : juce::StringArray::fromTokens(button.getButtonText().trim(), " ", ""))
    longest = std::max(longest, juce::GlyphArrangement::getStringWidthInt(font, word));
  return longest + 8;
}

int TabNamesLook::getTabButtonBestWidth(juce::TabBarButton& button, int tabDepth) {
  const int own = juce::LookAndFeel_V4::getTabButtonBestWidth(button, tabDepth);
  auto& bar = button.getTabbedButtonBar();
  // As TabbedButtonBar::updateTabPositions counts the bar's length.
  const int overlap = getTabButtonOverlap(tabDepth) + getTabButtonSpaceAroundImage() * 2;
  int wanted = std::max(0, overlap), least = wanted, room = bar.isVertical() ? bar.getHeight() : bar.getWidth();
  for (int i = 0; i < bar.getNumTabs(); ++i)
    if (auto* tab = bar.getTabButton(i)) {
      const int w = juce::LookAndFeel_V4::getTabButtonBestWidth(*tab, tabDepth);
      wanted += w - overlap;
      least += std::min(w, longestWordWidth(*tab, tabDepth)) - overlap;
    }
  if (wanted <= room) return own;
  const int floor = std::min(own, longestWordWidth(button, tabDepth));
  if (least >= room || wanted == least) return floor;
  // The bar's length past the words, shared as each name wants more.
  return floor + (own - floor) * (room - least) / (wanted - least);
}

SettingsWindow::SettingsWindow(MasterpieceProcessor& p,
                               juce::AudioDeviceManager* devices)
    : metronome_(p), performance_(p, EnginePanel::Part::Performance),
      general_(p, EnginePanel::Part::General), display_(p), language_(p), log_(p) {
  const auto bg = juce::Colour(0xff1b1e24);
  addAndMakeVisible(tabs_);
  tabs_.getTabbedButtonBar().setLookAndFeel(&tabNamesLook_);
  // General first: its display mode decides what every other tab shows.
  tabs_.addTab(TRANS("General"), bg, &generalScroll_, false);
  // Without a device manager -- a plugin, whose host owns the audio device
  // and the MIDI ports -- the Audio and MIDI tabs are left out.
  if (devices != nullptr) {
    auto panel = std::make_unique<AudioSettingsPanel>(*devices, p);
    auto* audio = panel.get();
    audio_ = std::move(panel);
    // The compressor and the limiter make the tab taller than the window.
    audioScroll_ = std::make_unique<ScrollHost>(*audio_, 0, [audio] { return audio->contentHeight(); });
    tabs_.addTab(TRANS("Audio"), bg, audioScroll_.get(), false);
  }
  if (devices != nullptr) {
    midi_ = std::make_unique<MidiPanel>(p, *devices, MidiPanel::Part::Console);
    midiScroll_ = std::make_unique<ScrollHost>(*midi_, 0, [this] { return midi_->contentHeight(); });
    tabs_.addTab(TRANS("MIDI"), bg, midiScroll_.get(), false);
  }
  tabs_.addTab(TRANS("Performance"), bg, &performanceScroll_, false);
  tabs_.addTab(TRANS("Metronome"), bg, &metronome_, false);
  tabs_.addTab(TRANS("Console display"), bg, &display_, false);
  if (devices != nullptr) {
    events_ = std::make_unique<MidiPanel>(p, *devices, MidiPanel::Part::Events);
    eventsScroll_ = std::make_unique<ScrollHost>(*events_, 0, [this] { return events_->contentHeight(); });
    tabs_.addTab(TRANS("Console events"), bg, eventsScroll_.get(), false);
  }
  tabs_.addTab(TRANS("Preferences"), bg, &language_, false);
  // Only where the program runs the server: the standalone program sets
  // remoteStatus; a plugin does not.
  if (p.remoteStatus) {
    remote_ = std::make_unique<RemotePanel>(p);
    tabs_.addTab(TRANS("Remote control"), bg, remote_.get(), false);
  }
  tabs_.addTab(TRANS("Diagnostics"), bg, &log_, false);
  if (lastSettingsTab().isNotEmpty()) showTab(lastSettingsTab());
  // Wide enough for every tab's name in full; the tabs scroll when taller.
  setSize(960, 560);
}

void SettingsWindow::showTab(const juce::String& name) {
  const auto names = tabs_.getTabNames();
  if (const int i = names.indexOf(name); i >= 0) tabs_.setCurrentTabIndex(i);
}

void SettingsWindow::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff15171c)); }
void SettingsWindow::resized() { tabs_.setBounds(getLocalBounds()); }

} // namespace mp::ui
