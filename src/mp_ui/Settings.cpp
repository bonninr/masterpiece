#include "Settings.h"
#include "../mp_audio/DataFolder.h"

#include <cmath>
#include <map>
#include <iterator>

#include "ManualDialog.h"
#include "MidiEventDialog.h"
#include "Ui.h"
#include "UpdateCheck.h"
#include "Mobile.h"
#include "../mp_audio/Translations.h"

namespace mp::ui {
namespace {

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
  openProgramCouplers_ = proc_.programCouplers();
  addAndMakeVisible(programCouplers_);
  programCouplers_.setToggleState(openProgramCouplers_, juce::dontSendNotification);
  programCouplers_.onClick = [this] {
    proc_.setProgramCouplers(programCouplers_.getToggleState());
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

  for (auto* b : {&simpleWav_, &wind_, &tremulant_, &enclosure_, &voicing_,
                  &originalPitch_}) {
    addAndMakeVisible(*b);
    b->onClick = [this] { pushSwitches(); };
  }

  const auto sw = proc_.engineSwitch();
  simpleWav_.setToggleState(sw.simpleWavOnly, juce::dontSendNotification);
  wind_.setToggleState(sw.enableWindModel, juce::dontSendNotification);
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
  profile_.addItem(TRANS("Best: 24-bit, all in memory"), 1);
  profile_.addItem(TRANS("High: 24-bit, compressed, releases streamed"), 2);
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
  storage_.addItem(TRANS("24-bit - what the samples are"), 1);
  storage_.addItem(TRANS("20-bit - a sixth less than the source"), 4);
  storage_.addItem(TRANS("16-bit - two thirds of that, inaudible"), 2);
  storage_.addItem(TRANS("32-bit float - larger than the source"), 3);
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
  sparse_.setSelectedId(proc_.sparseEvery(), juce::dontSendNotification);
  sparse_.setTooltip(TRANS("Saves memory: a note that is not loaded plays its nearest loaded "
                            "neighbour's samples, retuned to its own pitch. Applies when the "
                            "organ is loaded again."));
  sparse_.onChange = [this] { proc_.setSparseEvery(sparse_.getSelectedId()); };

  // Keeping the decoded samples, so the next load of the same organ is a read
  // rather than twelve thousand decodes. One file by default: these run to
  // gigabytes, and a cache per organ per settings combination would fill a
  // disk without ever being asked to. Loading something else replaces it.
  addAndMakeVisible(cacheLabel_);
  styleLabel(cacheLabel_, TRANS("Sample cache"));
  addAndMakeVisible(cache_);
  cache_.addItem(TRANS("One cache, replaced as organs change"), 1);
  cache_.addItem(TRANS("One cache per organ (uses more disk)"), 2);
  cache_.addItem(TRANS("Off"), 3);
  cache_.setSelectedId(proc_.cacheMode() == SampleLibrary::CacheMode::Off        ? 3
                       : proc_.cacheMode() == SampleLibrary::CacheMode::PerOrgan ? 2
                                                                                 : 1,
                       juce::dontSendNotification);
  cache_.onChange = [this] {
    const int id = cache_.getSelectedId();
    proc_.setCacheMode(id == 3   ? SampleLibrary::CacheMode::Off
                       : id == 2 ? SampleLibrary::CacheMode::PerOrgan
                                 : SampleLibrary::CacheMode::Single);
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
  addAndMakeVisible(organRootLabel_);
  styleLabel(organRootLabel_, TRANS("Organ folder"));
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
    syncProfile();
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
  performance_.addItem(TRANS("Most polyphony: every processor, 8192 voices"), 1);
  performance_.addItem(TRANS("Balanced (default)"), 2);
  performance_.addItem(TRANS("Steady: one processor kept for the system"), 3);
  performance_.addItem(TRANS("Light computer: 1024 voices"), 4);
  performance_.addItem(TRANS("Small computer, Raspberry Pi, tablet: 512 voices"), 5);
  performance_.addItem(TRANS("Custom"), 6);
  performance_.setTooltip(TRANS("Sets the audio engine, its threads and the polyphony together. Applies at once."));
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
  proc_.setProgramCouplers(openProgramCouplers_);
  programCouplers_.setToggleState(openProgramCouplers_, juce::dontSendNotification);
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
  cache_.setSelectedId(openCache_ == SampleLibrary::CacheMode::Off        ? 3
                       : openCache_ == SampleLibrary::CacheMode::PerOrgan ? 2
                                                                          : 1,
                       juce::dontSendNotification);
  rate_.setSelectedId(openRate_ >= 47000.0 && openRate_ <= 49000.0   ? 2
                      : openRate_ >= 43000.0 && openRate_ <= 45000.0 ? 3
                                                                     : 1,
                      juce::dontSendNotification);
  syncProfile();

  simpleWav_.setToggleState(openSwitch_.simpleWavOnly, juce::dontSendNotification);
  wind_.setToggleState(openSwitch_.enableWindModel, juce::dontSendNotification);
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
         proc_.codeSamples() != openCompress_ ||
         proc_.programCouplers() != openProgramCouplers_ ||
         proc_.preloadHeadFrames() != openPreload_ ||
         proc_.polyphonyLimit() != openPolyphony_ ||
         sw.simpleWavOnly != openSwitch_.simpleWavOnly ||
         sw.enableWindModel != openSwitch_.enableWindModel ||
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
                                                              : juce::String("24-bit");
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
  compress_.setEnabled(st == SampleStorage::Int16 || st == SampleStorage::Int24);
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
  const List organ{&simpleWav_, &wind_, &tremulant_, &enclosure_, &voicing_, &originalPitch_,
                   &profileLabel_, &profile_, &preloadLabel_, &preload_, &storageLabel_, &storage_,
                   &rateLabel_, &rate_, &sparseLabel_, &sparse_, &releasesLabel_, &releases_,
                   &releaseLengthLabel_, &releaseLength_, &releaseCutoffLabel_, &releaseCutoff_,
                   &stream_, &streamHead_, &releaseBufferLabel_, &releaseBuffer_, &mono_, &compress_,
                   &programCouplers_, &portable_, &memory_, &revert_, &keep_, &saveOrgan_, &saveGlobal_};
  const List organSimple{&profileLabel_, &profile_, &memory_, &revert_, &keep_, &saveOrgan_, &saveGlobal_};
  const List performance{&performanceLabel_, &performance_, &faster_, &realtime_, &everyProcessor_,
                         &memLimitLabel_, &memLimit_, &polyphonyLabel_, &polyphony_};
  const List performanceSimple{&performanceLabel_, &performance_};
  const List general{&reopen_, &loadTicks_, &organRootLabel_, &organRootValue_, &organRootChoose_,
                     &organRootDefault_, &cacheDirLabel_, &cacheDirValue_, &cacheDirChoose_,
                     &cacheDirDefault_, &cacheDelete_, &packageDirLabel_, &packageDirValue_,
                     &packageDirChoose_, &packageDirClear_, &cacheLabel_, &cache_, &cacheWhenLabel_,
                     &cacheWhen_};
  // The cache's choices too: the first load of a large organ waits on it (#565).
  const List generalSimple{&reopen_, &organRootLabel_, &organRootValue_, &organRootChoose_,
                           &organRootDefault_, &cacheLabel_, &cache_, &cacheWhenLabel_,
                           &cacheWhen_};
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
  for (auto* b : {&simpleWav_, &wind_, &tremulant_, &enclosure_, &voicing_, &originalPitch_}) line(*b);
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
  line(compress_, kIndent);
  if (profile_.isVisible()) r.removeFromTop(kGap);
  pair(rateLabel_, rate_, 260);
  pair(sparseLabel_, sparse_, 300);
  pair(releaseLengthLabel_, releaseLength_, 300);
  pair(releaseCutoffLabel_, releaseCutoff_, 300);
  pair(releaseBufferLabel_, releaseBuffer_, 300);
  line(programCouplers_);
  line(portable_);
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

  // General: start, folders and the cache, for every organ.
  line(reopen_);
  line(loadTicks_);
  if (reopen_.isVisible()) r.removeFromTop(4);
  folder(organRootLabel_, organRootValue_, {{&organRootChoose_, 90}, {&organRootDefault_, 80}});
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
  mix_.setValue(proc_.convolver().mix(), juce::dontSendNotification);
  mix_.onValueChange = [this] {
    proc_.convolver().setMix(static_cast<float>(mix_.getValue()));
    proc_.markSettingsDirty();
  };

}

void ReverbPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  enabled_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  auto row = r.removeFromTop(kRow);
  load_.setBounds(row.removeFromLeft(110));
  row.removeFromLeft(kGap);
  clear_.setBounds(row.removeFromLeft(80));
  row.removeFromLeft(kGap);
  irName_.setBounds(row);
  r.removeFromTop(kGap);
  row = r.removeFromTop(kRow);
  mixLabel_.setBounds(row.removeFromLeft(60));
  mix_.setBounds(row.removeFromLeft(320));
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
  for (auto* b : {&audioRecord_, &audioStop_}) addAndMakeVisible(*b);
  addAndMakeVisible(audioStatus_);
  styleLabel(audioStatus_, "");
  addAndMakeVisible(audioNote_);
  styleNote(audioNote_,
            TRANS("Captures the organ's output after the master fader and before "
                  "the metronome, so the click stays out of the file. Written on a "
                  "background thread at 24-bit; the audio callback only hands over "
                  "the block it has already finished."));

  audioRecord_.onClick = [this] {
    audioChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Record the organ to"), proc_.dialogFolder("recording"), "*.wav");
    audioChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& fc) {
          const auto f = fc.getResult();
          proc_.rememberDialogFolder("recording", f);
          if (f.getFullPathName().isEmpty()) return;
          // The device's rate, not the organ's: this is what is leaving.
          if (!proc_.audioRecorder().start(f.withFileExtension(".wav"),
                                           proc_.getSampleRate(),
                                           proc_.getTotalNumOutputChannels()))
            audioStatus_.setText(TRANS("Could not open that file for writing"),
                                 juce::dontSendNotification);
        });
  };
  audioStop_.onClick = [this] { proc_.audioRecorder().stop(); };

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
    map.cancelLearn();
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
      case 0: self->proc_.midiMap().beginLearn(kind, 0, false); break;
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
  audioStatus_.setText(
      on ? TRANS("Recording %1 - %2 s")
               .replace("%1", a.file().getFileName())
               .replace("%2", juce::String(a.secondsRecorded(), 1))
         : TRANS("Not recording"),
      juce::dontSendNotification);
  audioRecord_.setEnabled(!on);
  audioStop_.setEnabled(on);
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
                   std::initializer_list<juce::TextButton*>{&audioRecord_, &audioStop_}})
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
  placeRow(r.removeFromTop(kRow), {&audioRecord_, &audioStop_});
  r.removeFromTop(kGap);
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
  };
  addAndMakeVisible(clearDefault_);
  clearDefault_.onClick = [this] {
    proc_.clearDefaultConsole();
    showDefaultConsole();
  };
  addAndMakeVisible(defaultNote_);
  defaultNote_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  showDefaultConsole();
  addAndMakeVisible(keyboardsLabel_);
  styleLabel(keyboardsLabel_, TRANS("Keyboards"));

  addAndMakeVisible(output_);
  output_.onChange = [this] {
    // The processor holds the port and keeps the choice: the console's lamps
    // and displays follow it after this window closes, and at the next start.
    const int id = output_.getSelectedId();
    proc_.openMidiOutput(id > 1 ? output_.getItemText(output_.indexOfItemId(id)) : juce::String());
  };

  addAndMakeVisible(feedback_);
  feedback_.setToggleState(proc_.midiFeedbackEnabled(), juce::dontSendNotification);
  feedback_.onClick = [this] {
    proc_.setMidiFeedbackEnabled(feedback_.getToggleState());
  };
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

  addAndMakeVisible(stepperLabel_);
  styleLabel(stepperLabel_, TRANS("Sequencer"));
  addAndMakeVisible(learnNext_);
  learnNext_.onClick = [this] {
    // Momentary: a sequencer piston is a button, and a latching binding would
    // advance a frame only every other press.
    proc_.midiMap().beginLearn(MidiTargetKind::StepperNext, 0, false);
  };
  addAndMakeVisible(learnPrev_);
  learnPrev_.onClick = [this] {
    proc_.midiMap().beginLearn(MidiTargetKind::StepperPrev, 0, false);
  };
  // A console with fewer keyboards than the organ has manuals: a piston makes
  // the keyboard on a channel play another manual (#90). Pick the channel,
  // then the manual, then press the piston.
  addAndMakeVisible(keyboardsHeading_);
  styleLabel(keyboardsHeading_, TRANS("Manual buttons"));
  addAndMakeVisible(learnManuals_);
  learnManuals_.setTooltip(TRANS("Pistons that switch which manual a keyboard plays: choose the keyboard's "
                                 "channel and the manual, then press the piston"));
  learnManuals_.onClick = [this] {
    juce::PopupMenu menu;
    const auto manuals = proc_.playableKeyboards();
    for (int channel = 1; channel <= 16; ++channel) {
      juce::PopupMenu sub;
      for (Id kb : manuals) {
        const bool now = proc_.keyboardForChannel(channel) == kb;
        sub.addItem(static_cast<int>(routeKeyboardTarget(kb, channel)),
                    juce::String(proc_.keyboardName(kb)), true, now);
      }
      const Id playing = proc_.keyboardForChannel(channel);
      menu.addSubMenu(playing != 0 ? TRANS("Keyboard on channel %1 (plays %2)")
                                           .replace("%1", juce::String(channel))
                                           .replace("%2", juce::String(proc_.keyboardName(playing)))
                                   : TRANS("Keyboard on channel %1").replace("%1", juce::String(channel)),
                      sub);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&learnManuals_),
                       [this](int target) {
                         if (target <= 0) return;
                         proc_.midiMap().beginLearn(MidiTargetKind::RouteKeyboard, target, false);
                       });
  };

  addAndMakeVisible(saveMap_);
  saveMap_.onClick = [this] { proc_.saveMidiMap(); };
  addAndMakeVisible(clearMap_);
  clearMap_.onClick = [this] { proc_.midiMap().clear(); };

  addAndMakeVisible(mapStatus_);
  styleLabel(mapStatus_, "");

  addAndMakeVisible(consoleHeading_);
  styleLabel(consoleHeading_, TRANS("Console"));
  struct { juce::TextButton* b; MidiTargetKind k; } consoleLearn[] = {
      {&learnPageNext_, MidiTargetKind::ConsoleNextPage},
      {&learnPagePrev_, MidiTargetKind::ConsolePrevPage},
      {&learnLayout_, MidiTargetKind::ConsoleNextLayout},
      {&learnStopList_, MidiTargetKind::ConsoleToggleStopList},
      {&learnKeyboard_, MidiTargetKind::ConsoleToggleKeyboard},
      {&learnCombinations_, MidiTargetKind::ConsoleToggleCombinations},
      {&learnTransposeDown_, MidiTargetKind::TransposeDown},
      {&learnTransposeUp_, MidiTargetKind::TransposeUp},
      {&learnTemperamentPrev_, MidiTargetKind::TemperamentPrev},
      {&learnTemperamentNext_, MidiTargetKind::TemperamentNext},
      {&learnScoreBack_, MidiTargetKind::ScorePageBack},
      {&learnScoreForward_, MidiTargetKind::ScorePageForward},
      {&learnRecord_, MidiTargetKind::RecorderRecord},
      {&learnPlay_, MidiTargetKind::RecorderPlay},
  };
  for (auto& e : consoleLearn) {
    addAndMakeVisible(*e.b);
    const auto kind = e.k;
    e.b->onClick = [this, kind] {
      proc_.midiMap().beginLearn(kind, 0, false);
    };
  }

  addAndMakeVisible(temperamentLabel_);
  styleLabel(temperamentLabel_, TRANS("Temperament"));
  addAndMakeVisible(temperamentPick_);
  temperamentNames_.push_back("");
  temperamentPick_.addItem(TRANS("The organ's own"), 1);
  for (const auto& t : temperamentLibrary()) {
    temperamentNames_.push_back(t.name);
    temperamentPick_.addItem(juce::String(t.name), static_cast<int>(temperamentNames_.size()));
  }
  temperamentPick_.setSelectedId(1, juce::dontSendNotification);
  addAndMakeVisible(learnTemperament_);
  learnTemperament_.setTooltip(TRANS("A button that chooses this temperament: press Learn, then the button"));
  learnTemperament_.onClick = [this] {
    const int i = temperamentPick_.getSelectedId() - 1;
    if (i < 0 || i >= static_cast<int>(temperamentNames_.size())) return;
    proc_.midiMap().beginLearn(MidiTargetKind::Temperament,
                               temperamentTarget(temperamentNames_[static_cast<size_t>(i)]), false);
  };

  addAndMakeVisible(powerHeading_);
  styleLabel(powerHeading_, TRANS("Power"));
  for (auto* b : {&learnQuit_, &learnPowerOff_}) {
    addAndMakeVisible(*b);
    b->setTooltip(TRANS("For a console with no screen. Obeyed once Masterpiece has run two minutes, and "
                        "only when the button is held three seconds; a button whose message has no "
                        "release is pressed twice within two seconds. Masterpiece saves before it quits."));
  }
  learnQuit_.onClick = [this] { proc_.midiMap().beginLearn(MidiTargetKind::QuitProgram, 0, false); };
  learnPowerOff_.onClick = [this] { proc_.midiMap().beginLearn(MidiTargetKind::PowerOffComputer, 0, false); };

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
  addAndMakeVisible(scoreHeading_);
  styleLabel(scoreHeading_, TRANS("Score app"));
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

  refresh();
  startTimerHz(2);
}

void MidiPanel::applyPart() {
  std::vector<juce::Component*> console{&inputsLabel_, &outputsLabel_, &output_, &feedback_, &cancelResets_,
                                        &leaveHeld_, &sustain_, &noiseFilter_, &archive_, &archiveSilence_,
                                        &archiveSize_, &archiveAge_, &stepperLabel_, &learnPrev_, &learnNext_,
                                        &consoleHeading_, &learnPagePrev_, &learnPageNext_, &learnLayout_,
                                        &learnStopList_, &learnKeyboard_, &learnCombinations_,
                                        &learnTransposeDown_, &learnTransposeUp_, &temperamentLabel_,
                                        &temperamentPick_, &learnTemperament_, &learnTemperamentPrev_,
                                        &learnTemperamentNext_, &learnRecord_, &learnPlay_, &powerHeading_,
                                        &learnQuit_, &learnPowerOff_, &lampLabel_, &lampKind_, &lampChannel_,
                                        &lampNumber_, &lampLit_, &lampDark_, &loadMessageLabel_,
                                        &loadMessage_, &unloadMessageLabel_, &unloadMessage_,
                                        &scoreHeading_, &learnScoreBack_, &learnScoreForward_,
                                        &scoreKind_, &scoreChannel_, &scoreForward_, &scoreBack_};
  for (auto& b : inputs_) console.push_back(b.get());
  std::vector<juce::Component*> organ{&keyboardsLabel_, &sharedNote_, &useAsDefault_, &clearDefault_,
                                      &defaultNote_, &keyboardsHeading_, &learnManuals_};
  for (auto& v : keyboardLabels_) organ.push_back(v.get());
  for (auto& v : keyboardChannels_) organ.push_back(v.get());
  for (auto& v : keyboardDevices_) organ.push_back(v.get());
  for (auto& v : keyboardMore_) organ.push_back(v.get());
  // What Simple mode leaves out: the console builder's settings.
  const std::vector<juce::Component*> professional{
      &cancelResets_, &leaveHeld_, &noiseFilter_, &archive_, &archiveSilence_, &archiveSize_, &archiveAge_,
      &stepperLabel_, &learnPrev_, &learnNext_, &consoleHeading_, &learnPagePrev_, &learnPageNext_,
      &learnLayout_, &learnStopList_, &learnKeyboard_, &learnCombinations_, &learnTransposeDown_,
      &learnTransposeUp_, &temperamentLabel_, &temperamentPick_, &learnTemperament_, &learnTemperamentPrev_,
      &learnTemperamentNext_, &learnRecord_, &learnPlay_, &powerHeading_, &learnQuit_, &learnPowerOff_,
      &lampLabel_, &lampKind_, &lampChannel_, &lampNumber_, &lampLit_, &lampDark_, &loadMessageLabel_,
      &loadMessage_, &unloadMessageLabel_, &unloadMessage_, &scoreHeading_, &learnScoreBack_,
      &learnScoreForward_, &scoreKind_, &scoreChannel_, &scoreForward_, &scoreBack_, &keyboardsHeading_,
      &learnManuals_, &useAsDefault_, &clearDefault_, &defaultNote_};
  shownSimple_ = proc_.simpleMode();
  for (auto* c : part_ == Part::Console ? console : organ)
    c->setVisible(!shownSimple_ || std::find(professional.begin(), professional.end(), c) == professional.end());
  for (auto* c : part_ == Part::Console ? organ : console) c->setVisible(false);
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

MidiPanel::~MidiPanel() { stopTimer(); }

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
  for (const auto& [role, channel] : def) {
    const juce::String item =
        role == 0 ? TRANS("Pedal %1").replace("%1", juce::String(channel))
                  : TRANS("Manual %1 %2").replace("%1", juce::String(role)).replace("%2", juce::String(channel));
    text << " " << item << ";";
  }
  defaultNote_.setText(text.dropLastCharacters(1), juce::dontSendNotification);
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

  // Which channel plays which manual. The organ ships a default assignment
  // (Hauptwerk's own: the pedal is channel 1, the manuals follow), and this is
  // where a player whose console disagrees says so.
  keyboardLabels_.clear();
  keyboardChannels_.clear();
  keyboardDevices_.clear();
  keyboardMore_.clear();
  for (Id kb : proc_.playableKeyboards()) {
    auto label = std::make_unique<juce::Label>();
    styleLabel(*label, juce::String(proc_.keyboardName(kb)));
    addAndMakeVisible(*label);
    keyboardLabels_.push_back(std::move(label));

    auto box = std::make_unique<juce::ComboBox>();
    box->addItem(TRANS("Default (channel %1)").replace("%1", juce::String(proc_.channelForKeyboard(kb))), 1);
    for (int ch = 1; ch <= 16; ++ch)
      box->addItem(TRANS("Channel %1").replace("%1", juce::String(ch)), ch + 1);
    const auto& assigned = proc_.channelAssignments();
    int selected = 1;
    for (const auto& b : assigned)
      if (b.keyboardId == kb && b.channel > 0) selected = b.channel + 1;
    box->setSelectedId(selected, juce::dontSendNotification);
    auto* raw = box.get();
    box->onChange = [this, kb, raw] {
      // Shared, not taken: a channel chosen here may already belong to another
      // manual, and one keyboard playing two divisions is a coupler a player
      // can build for themselves. The line under the row says who else is on
      // it, so nothing is hidden.
      // A channel chosen keeps the player's routing. Default is channel 0,
      // which removes this manual's own binding and leaves the organ's.
      if (raw->getSelectedId() > 1) proc_.keepRouting();
      proc_.setKeyboardForChannel(raw->getSelectedId() - 1, kb, 0, false);
      proc_.saveMidiMap();
      showSharedChannels();
    };
    addAndMakeVisible(*box);
    keyboardChannels_.push_back(std::move(box));

    // And which console. Named by the device itself, because a saved mapping
    // refers to it by name and an id means nothing across runs.
    auto dev = std::make_unique<juce::ComboBox>();
    dev->addItem(TRANS("Any device"), 1);
    const auto& known = proc_.midiDevices();
    for (size_t i = 0; i < known.names().size(); ++i)
      dev->addItem(juce::String(known.displayName(static_cast<int>(i) + 1)), static_cast<int>(i) + 2);
    int selectedDev = 1;
    for (const auto& b : proc_.channelAssignments())
      if (b.keyboardId == kb && b.deviceId != 0) selectedDev = b.deviceId + 1;
    dev->setSelectedId(selectedDev, juce::dontSendNotification);
    auto* rawDev = dev.get();
    auto* rawBox = keyboardChannels_.back().get();
    dev->onChange = [this, kb, rawDev] {
      // On the channel the row shows; the manuals sharing it keep it (#637).
      proc_.setKeyboardDevice(kb, rawDev->getSelectedId() - 1);
      proc_.saveMidiMap();
      showSharedChannels();
    };
    addAndMakeVisible(*dev);
    keyboardDevices_.push_back(std::move(dev));

    auto more = std::make_unique<juce::TextButton>(TRANS("Range, transpose..."));
    more->onClick = [this, kb] { ManualDialog::show(proc_, kb); };
    addAndMakeVisible(*more);
    keyboardMore_.push_back(std::move(more));
  }
  showSharedChannels();

  output_.clear(juce::dontSendNotification);
  output_.addItem(TRANS("None"), 1);
  int id = 2, chosen = 1;
  for (const auto& out : juce::MidiOutput::getAvailableDevices()) {
    if (out.name == proc_.midiOutputName()) chosen = id;
    output_.addItem(out.name, id++);
  }
  // The port chosen before, though it is not plugged in now: the choice is
  // kept, and shown as it is.
  if (chosen == 1 && proc_.midiOutputName().isNotEmpty()) {
    chosen = id;
    output_.addItem(proc_.midiOutputName(), id++);
  }
  output_.setSelectedId(chosen, juce::dontSendNotification);
  applyPart();
  resized();
}

void MidiPanel::timerCallback() {
  // A tab of the settings window that is not the one on show.
  if (!isShowing()) return;
  if (proc_.simpleMode() != shownSimple_) {
    applyPart();
    resized();
  }
  const auto& map = proc_.midiMap();
  juce::String s = TRANS("%1 binding(s)").replace("%1", juce::String(static_cast<int>(map.size())));
  if (map.learning()) s += "  -  " + TRANS("LEARNING: move a control now");
  mapStatus_.setText(s, juce::dontSendNotification);

}

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
  } else {
    keyboardsLabel_.setBounds(r.removeFromTop(kRow));
    for (size_t i = 0; i < keyboardChannels_.size(); ++i) {
      auto kbRow = r.removeFromTop(kRow).reduced(12, 0);
      keyboardLabels_[i]->setBounds(kbRow.removeFromLeft(150));
      keyboardChannels_[i]->setBounds(kbRow.removeFromLeft(170));
      kbRow.removeFromLeft(6);
      // In a narrow window the device box gives way first: the range button's
      // text shrank to a few pixels when it took the remainder (#493).
      const int deviceWidth = juce::jlimit(140, 200, kbRow.getWidth() - 6 - 160);
      if (i < keyboardDevices_.size())
        keyboardDevices_[i]->setBounds(kbRow.removeFromLeft(deviceWidth));
      kbRow.removeFromLeft(6);
      if (i < keyboardMore_.size())
        keyboardMore_[i]->setBounds(kbRow.removeFromLeft(160).reduced(0, 1));
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
    }
  }

  if (part_ == Part::Console) {
    r.removeFromTop(kGap);
    row = r.removeFromTop(kRow);
    outputsLabel_.setBounds(row.removeFromLeft(120));
    output_.setBounds(row.removeFromLeft(300));
    r.removeFromTop(4);
    feedback_.setBounds(r.removeFromTop(kRow));
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
    row = r.removeFromTop(kRow);
    stepperLabel_.setBounds(row.removeFromLeft(120));
    learnPrev_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
    learnNext_.setBounds(row.removeFromLeft(150).reduced(2, 0));

    // Console actions, on their own row under the same idea: things a physical
    // console's thumb pistons do that the organ file never mentions.
    r.removeFromTop(4);
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
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    row.removeFromLeft(120);
    for (auto* b : {&learnTransposeDown_, &learnTransposeUp_}) {
      b->setBounds(row.removeFromLeft(150).reduced(2, 0));
      row.removeFromLeft(6);
    }
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    temperamentLabel_.setBounds(row.removeFromLeft(120));
    temperamentPick_.setBounds(row.removeFromLeft(306).reduced(2, 0));
    row.removeFromLeft(6);
    learnTemperament_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    row.removeFromLeft(120);
    for (auto* b : {&learnTemperamentPrev_, &learnTemperamentNext_}) {
      b->setBounds(row.removeFromLeft(150).reduced(2, 0));
      row.removeFromLeft(6);
    }
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    row.removeFromLeft(120);
    for (auto* b : {&learnRecord_, &learnPlay_}) {
      b->setBounds(row.removeFromLeft(150).reduced(2, 0));
      row.removeFromLeft(6);
    }

    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    powerHeading_.setBounds(row.removeFromLeft(120));
    learnQuit_.setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
    learnPowerOff_.setBounds(row.removeFromLeft(150).reduced(2, 0));
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
    r.removeFromTop(4);
    row = r.removeFromTop(kRow);
    unloadMessageLabel_.setBounds(row.removeFromLeft(120));
    unloadMessage_.setBounds(row.removeFromLeft(306).reduced(2, 2));

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
    row.removeFromLeft(120);
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
  saveMap_.setBounds(row.removeFromLeft(120));
  row.removeFromLeft(kGap);
  clearMap_.setBounds(row.removeFromLeft(120));
  row.removeFromLeft(kGap);
  mapStatus_.setBounds(row);
  r.removeFromTop(kGap);
  const int needed = r.getY() + 12;
  if (needed != contentHeight_) {
    contentHeight_ = needed;
    if (auto* host = findParentComponentOfClass<ScrollHost>()) host->fit();
  }
}

// ------------------------------------------------------------ favourites

FavouritesPanel::FavouritesPanel(MasterpieceProcessor& p) : proc_(p) {
  // The favourite organs are in Open, with everything else about getting an
  // organ on the console (#276). What stays here is the registration books.
  // --- combination sets --------------------------------------------------
  addAndMakeVisible(setsHeading_);
  styleLabel(setsHeading_, TRANS("Combination sets"));
  addAndMakeVisible(setLabel_);
  styleLabel(setLabel_, TRANS("In use"));
  addAndMakeVisible(setBox_);
  setBox_.onChange = [this] {
    const int idx = setBox_.getSelectedId() - 1;
    if (idx < 0 || idx >= static_cast<int>(setNames_.size())) return;
    const std::string want = setNames_[static_cast<size_t>(idx)];
    if (want == proc_.combinationSetName()) return;
    // Switching saves the live registrations first, so changing set never
    // silently discards what was just captured.
    proc_.switchCombinationSet(want);
    proc_.saveSettings();
    setStatus_.setText(TRANS("Now using %1").replace("%1", want.empty() ? TRANS("the default set")
                                                                       : juce::String(want)),
                       juce::dontSendNotification);
  };

  addAndMakeVisible(setNew_);
  setNew_.onClick = [this] {
    setPrompt_ = std::make_unique<juce::AlertWindow>(
        TRANS("Save as new set"), TRANS("A name for this registration set:"),
        juce::MessageBoxIconType::NoIcon);
    setPrompt_->addTextEditor("name", "", TRANS("Name"));
    setPrompt_->addButton(TRANS("Save"), 1);
    setPrompt_->addButton(TRANS("Cancel"), 0);
    setPrompt_->enterModalState(
        true, juce::ModalCallbackFunction::create([this](int result) {
          const juce::String name =
              result == 1 && setPrompt_ != nullptr
                  ? setPrompt_->getTextEditorContents("name").trim()
                  : juce::String();
          setPrompt_.reset();
          if (name.isEmpty()) return;
          // Copy, then switch onto it: "save as" means the player carries on
          // in the new set, not that they made a backup and kept editing the
          // old one.
          proc_.copyCombinationSetTo(name.toStdString());
          proc_.switchCombinationSet(name.toStdString());
          proc_.saveSettings();
          refreshSets();
          setStatus_.setText(TRANS("Now using %1").replace("%1", name), juce::dontSendNotification);
        }));
  };

  addAndMakeVisible(setDelete_);
  setDelete_.onClick = [this] {
    const std::string live = proc_.combinationSetName();
    if (live.empty()) {
      setStatus_.setText(TRANS("The default set cannot be deleted"),
                         juce::dontSendNotification);
      return;
    }
    // Move off it first: deleting the file under the live set would leave the
    // organ pointing at something that is no longer there.
    proc_.switchCombinationSet("");
    proc_.deleteCombinationSet(live);
    proc_.saveSettings();
    refreshSets();
    setStatus_.setText(TRANS("Deleted %1, back on the default").replace("%1", juce::String(live)),
                       juce::dontSendNotification);
  };
  addAndMakeVisible(setStatus_);
  styleLabel(setStatus_, "");

  refresh();
}

// The sets this organ has on disk, plus the default. Rebuilt rather than
// cached: another window, or another copy of the program, may have made one.
void FavouritesPanel::refreshSets() {
  setNames_ = proc_.combinationSets();
  // The default set always exists, whether or not a file has been written for
  // it yet, so it is offered even when findChildFiles saw nothing.
  if (std::find(setNames_.begin(), setNames_.end(), std::string()) ==
      setNames_.end())
    setNames_.insert(setNames_.begin(), std::string());

  setBox_.clear(juce::dontSendNotification);
  int selected = 1;
  for (size_t i = 0; i < setNames_.size(); ++i) {
    const auto& n = setNames_[i];
    setBox_.addItem(n.empty() ? TRANS("(default)") : juce::String(n),
                    static_cast<int>(i) + 1);
    if (n == proc_.combinationSetName()) selected = static_cast<int>(i) + 1;
  }
  setBox_.setSelectedId(selected, juce::dontSendNotification);
}

void FavouritesPanel::refresh() {
  refreshSets();
  resized();
}

void FavouritesPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  setsHeading_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(4);
  auto setRow = r.removeFromTop(kRow);
  setLabel_.setBounds(setRow.removeFromLeft(60));
  setBox_.setBounds(setRow.removeFromLeft(200).reduced(0, 1));
  setRow.removeFromLeft(kGap);
  setNew_.setBounds(setRow.removeFromLeft(160).reduced(0, 1));
  setRow.removeFromLeft(6);
  setDelete_.setBounds(setRow.removeFromLeft(120).reduced(0, 1));
  r.removeFromTop(4);
  setStatus_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap * 2);
}

// --------------------------------------------------------------- voicing

VoicingPanel::VoicingPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Voicing"));

  addAndMakeVisible(rankLabel_);
  styleLabel(rankLabel_, TRANS("Rank"));
  addAndMakeVisible(rank_);
  rank_.onChange = [this] { loadCurrentIntoSliders(); };

  addAndMakeVisible(scopeLabel_);
  styleLabel(scopeLabel_, TRANS("Applies to"));
  addAndMakeVisible(scope_);
  scope_.addItem(TRANS("The whole rank"), 1);
  scope_.addItem(TRANS("One pipe"), 2);
  scope_.setSelectedId(1, juce::dontSendNotification);
  scope_.onChange = [this] {
    const bool perPipe = scope_.getSelectedId() == 2;
    note_.setEnabled(perPipe);
    noteLabel_.setEnabled(perPipe);
    loadCurrentIntoSliders();
  };

  addAndMakeVisible(noteLabel_);
  styleLabel(noteLabel_, TRANS("MIDI note"));
  addAndMakeVisible(note_);
  note_.setRange(0, 127, 1);
  note_.setValue(60, juce::dontSendNotification);
  note_.setEnabled(false);
  note_.onValueChange = [this] { loadCurrentIntoSliders(); };

  addAndMakeVisible(gainLabel_);
  styleLabel(gainLabel_, TRANS("Level"));
  addAndMakeVisible(gain_);
  // +/-12 dB is the range a voicer works in. Wider would mostly offer new
  // ways to make an organ wrong.
  gain_.setRange(-12.0, 12.0, 0.1);
  gain_.setTextValueSuffix(" dB");
  gain_.setValue(0.0, juce::dontSendNotification);
  gain_.onValueChange = [this] { pushCurrent(); };

  addAndMakeVisible(tuneLabel_);
  styleLabel(tuneLabel_, TRANS("Tuning"));
  // +/-50 cents is half a semitone: past that you are playing a different
  // note, not tuning this one.
  tune_.setRange(-50.0, 50.0, 0.1);
  tune_.setTextValueSuffix(" cents");
  tune_.setValue(0.0, juce::dontSendNotification);
  tune_.onValueChange = [this] { pushCurrent(); };
  addAndMakeVisible(tune_);

  // The EQ: shelves of +/-12 dB, the brightness above 3 kHz and the bass
  // below 200 Hz. A rank's and its division's add.
  for (auto* s : {&bright_, &bass_, &divBright_, &divBass_}) {
    addAndMakeVisible(*s);
    s->setRange(-12.0, 12.0, 0.5);
    s->setTextValueSuffix(" dB");
    s->setValue(0.0, juce::dontSendNotification);
  }
  addAndMakeVisible(brightLabel_);
  styleLabel(brightLabel_, TRANS("Brightness"));
  addAndMakeVisible(bassLabel_);
  styleLabel(bassLabel_, TRANS("Bass"));
  bright_.setTooltip(TRANS("The rank's treble, above 3 kHz: heard at once, on the pipes sounding too"));
  bass_.setTooltip(TRANS("The rank's bass, below 200 Hz: heard at once, on the pipes sounding too"));
  bright_.onValueChange = bass_.onValueChange = [this] { pushCurrent(); };
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
  }
  for (auto* s : {&width_, &divWidth_}) {
    addAndMakeVisible(*s);
    s->setRange(0.0, 200.0, 1.0);
    s->setTextValueSuffix(" %");
    s->setValue(100.0, juce::dontSendNotification);
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
  pan_.onValueChange = width_.onValueChange = [this] { pushCurrent(); };
  divPan_.onValueChange = divWidth_.onValueChange = [this] { pushDivision(); };

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
    pushCurrent();
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
  rank_.clear(juce::dontSendNotification);
  for (const auto& [id, r] : proc_.organModel().ranks) rankIds_.push_back(id);
  std::sort(rankIds_.begin(), rankIds_.end());
  for (size_t i = 0; i < rankIds_.size(); ++i) {
    const auto it = proc_.organModel().ranks.find(rankIds_[i]);
    juce::String name = juce::String(static_cast<int>(rankIds_[i]));
    if (it != proc_.organModel().ranks.end() && !it->second.name.empty())
      name << "  " << juce::String(it->second.name);
    rank_.addItem(name, static_cast<int>(i) + 1);
  }
  if (!rankIds_.empty()) rank_.setSelectedId(1, juce::dontSendNotification);
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

// The pipe under the current rank and note, or 0 when the rank has no pipe
// there. A rank does not necessarily span the whole compass.
static Id pipeAt(const OrganModel& model, Id rankId, int midiNote) {
  const auto it = model.ranks.find(rankId);
  if (it == model.ranks.end()) return 0;
  for (const auto& pipe : it->second.pipes)
    if (pipe.midiNote == midiNote) return pipe.pipeId;
  return 0;
}

void VoicingPanel::loadCurrentIntoSliders() {
  if (rankIds_.empty()) return;
  const int idx = juce::jlimit(0, static_cast<int>(rankIds_.size()) - 1,
                               rank_.getSelectedId() - 1);
  const Id rankId = rankIds_[static_cast<size_t>(idx)];
  PipeVoicing pv;
  if (scope_.getSelectedId() == 2) {
    const Id pipeId = pipeAt(proc_.organModel(), rankId,
                             static_cast<int>(note_.getValue()));
    pv = proc_.voicing().live().pipe(pipeId);
  } else {
    pv = proc_.voicing().live().rank(rankId);
  }
  // dontSendNotification: these are being loaded FROM the model, and letting
  // them call back would write them straight back again, turning a read into
  // a write and a swap into a wipe.
  gain_.setValue(pv.gainDb, juce::dontSendNotification);
  tune_.setValue(pv.tuningCents, juce::dontSendNotification);
  // The EQ is a rank's: a pipe of its own would need a filter of its own.
  const bool rankScope = scope_.getSelectedId() != 2;
  bright_.setValue(rankScope ? pv.brightnessDb : 0.0, juce::dontSendNotification);
  bass_.setValue(rankScope ? pv.bassDb : 0.0, juce::dontSendNotification);
  pan_.setValue(rankScope ? pv.balance * 100.0 : 0.0, juce::dontSendNotification);
  width_.setValue(rankScope ? pv.width * 100.0 : 100.0, juce::dontSendNotification);
  for (auto* c : std::initializer_list<juce::Component*>{&bright_, &bass_, &brightLabel_, &bassLabel_, &pan_,
                                                         &width_, &panLabel_, &widthLabel_})
    c->setEnabled(rankScope);
  updateStatus();
}

void VoicingPanel::pushCurrent() {
  if (rankIds_.empty()) return;
  const int idx = juce::jlimit(0, static_cast<int>(rankIds_.size()) - 1,
                               rank_.getSelectedId() - 1);
  const Id rankId = rankIds_[static_cast<size_t>(idx)];

  if (scope_.getSelectedId() == 2) {
    const Id pipeId = pipeAt(proc_.organModel(), rankId,
                             static_cast<int>(note_.getValue()));
    if (pipeId == 0) {
      status_.setText(TRANS("This rank has no pipe at that note"),
                      juce::dontSendNotification);
      return;
    }
    PipeVoicing pv = proc_.voicing().live().pipe(pipeId);
    pv.gainDb = static_cast<float>(gain_.getValue());
    pv.tuningCents = static_cast<float>(tune_.getValue());
    proc_.voicing().live().setPipe(pipeId, pv);
  } else {
    PipeVoicing pv = proc_.voicing().live().rank(rankId);
    pv.gainDb = static_cast<float>(gain_.getValue());
    pv.tuningCents = static_cast<float>(tune_.getValue());
    const float pan = static_cast<float>(pan_.getValue() / 100.0);
    const float width = static_cast<float>(width_.getValue() / 100.0);
    const bool eqChanged = pv.brightnessDb != static_cast<float>(bright_.getValue()) ||
                           pv.bassDb != static_cast<float>(bass_.getValue()) || pv.balance != pan ||
                           pv.width != width;
    pv.brightnessDb = static_cast<float>(bright_.getValue());
    pv.bassDb = static_cast<float>(bass_.getValue());
    pv.balance = pan;
    pv.width = width;
    proc_.voicing().live().setRank(rankId, pv);
    if (eqChanged) proc_.applyVoicingEq();
  }
  updateStatus();
}

void VoicingPanel::updateStatus() {
  const auto& v = proc_.voicing().live();
  juce::String s;
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
  rankLabel_.setBounds(row.removeFromLeft(labelW));
  rank_.setBounds(row.removeFromLeft(320).reduced(0, 1));
  r.removeFromTop(4);

  row = r.removeFromTop(kRow);
  scopeLabel_.setBounds(row.removeFromLeft(labelW));
  scope_.setBounds(row.removeFromLeft(180).reduced(0, 1));
  row.removeFromLeft(kGap);
  noteLabel_.setBounds(row.removeFromLeft(80));
  note_.setBounds(row.removeFromLeft(110).reduced(0, 1));
  r.removeFromTop(kGap);

  row = r.removeFromTop(kRow);
  gainLabel_.setBounds(row.removeFromLeft(labelW));
  gain_.setBounds(row.reduced(0, 2));
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  tuneLabel_.setBounds(row.removeFromLeft(labelW));
  tune_.setBounds(row.reduced(0, 2));
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  {
    auto left = row.removeFromLeft(row.getWidth() / 2);
    brightLabel_.setBounds(left.removeFromLeft(labelW));
    bright_.setBounds(left.reduced(0, 2));
    row.removeFromLeft(kGap);
    bassLabel_.setBounds(row.removeFromLeft(60));
    bass_.setBounds(row.reduced(0, 2));
  }
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  {
    auto left = row.removeFromLeft(row.getWidth() / 2);
    panLabel_.setBounds(left.removeFromLeft(labelW));
    pan_.setBounds(left.reduced(0, 2));
    row.removeFromLeft(kGap);
    widthLabel_.setBounds(row.removeFromLeft(60));
    width_.setBounds(row.reduced(0, 2));
  }
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  divisionLabel_.setBounds(row.removeFromLeft(labelW));
  division_.setBounds(row.removeFromLeft(220).reduced(0, 1));
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  {
    row.removeFromLeft(labelW);
    auto left = row.removeFromLeft(row.getWidth() / 2);
    divBrightLabel_.setBounds(left.removeFromLeft(90));
    divBright_.setBounds(left.reduced(0, 2));
    row.removeFromLeft(kGap);
    divBassLabel_.setBounds(row.removeFromLeft(60));
    divBass_.setBounds(row.reduced(0, 2));
  }
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  {
    row.removeFromLeft(labelW);
    auto left = row.removeFromLeft(row.getWidth() / 2);
    divPanLabel_.setBounds(left.removeFromLeft(90));
    divPan_.setBounds(left.reduced(0, 2));
    row.removeFromLeft(kGap);
    divWidthLabel_.setBounds(row.removeFromLeft(60));
    divWidth_.setBounds(row.reduced(0, 2));
  }
  r.removeFromTop(kGap);

  // Four then one. Five across overflowed the dialog and squeezed the last
  // button until its label no longer fitted inside it.
  row = r.removeFromTop(kRow);
  for (auto* b : {&abSwap_, &abCopy_, &resetOne_, &resetAll_}) {
    b->setBounds(row.removeFromLeft(140).reduced(2, 1));
    row.removeFromLeft(4);
  }
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  save_.setBounds(row.removeFromLeft(180).reduced(2, 1));
  r.removeFromTop(4);
  status_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  note2_.setBounds(r);
}

// ----------------------------------------------------------------- mixer

namespace {
// One row per pair: the crossover under which its lows go to the subwoofer,
// and the subwoofer's device channel. Off leaves the pair as it was.
class BassPanel final : public juce::Component {
public:
  std::function<void()> onChanged;
  explicit BassPanel(MasterpieceProcessor& p, std::function<void()> changed)
      : onChanged(std::move(changed)), proc_(p) {
    addAndMakeVisible(heading_);
    styleLabel(heading_, TRANS("Bass management"));
    const int channels = juce::jmax(8, proc_.getTotalNumOutputChannels());
    for (const auto& b : proc_.mixer().buses) {
      Row row;
      row.busId = b.id.value;
      row.label = std::make_unique<juce::Label>();
      styleLabel(*row.label, TRANS("Pair %1").replace("%1", juce::String(b.id.value)));
      row.crossover = std::make_unique<juce::ComboBox>();
      row.crossover->addItem(TRANS("Off"), 1);
      for (int hz : kFrequencies) row.crossover->addItem(juce::String(hz) + " Hz", hz);
      row.sub = std::make_unique<juce::ComboBox>();
      for (int ch = 0; ch < channels; ++ch)
        row.sub->addItem(TRANS("Subwoofer on %1").replace("%1", juce::String(ch + 1)), ch + 1);
      const BassSetting now = settingOf(b.id.value);
      row.crossover->setSelectedId(now.hz > 0 ? now.hz : 1, juce::dontSendNotification);
      row.sub->setSelectedId(now.sub >= 0 ? now.sub + 1 : juce::jmin(channels, 3), juce::dontSendNotification);
      row.sub->setEnabled(now.hz > 0);
      const int id = b.id.value;
      auto* crossover = row.crossover.get();
      auto* subBox = row.sub.get();
      auto apply = [this, id, crossover, subBox] {
        const int hz = crossover->getSelectedId() > 1 ? crossover->getSelectedId() : 0;
        subBox->setEnabled(hz > 0);
        proc_.setBusBass(id, static_cast<float>(hz), subBox->getSelectedId() - 1);
        if (onChanged) onChanged();
      };
      row.crossover->onChange = apply;
      row.sub->onChange = apply;
      addAndMakeVisible(*row.label);
      addAndMakeVisible(*row.crossover);
      addAndMakeVisible(*row.sub);
      rows_.push_back(std::move(row));
    }
    setSize(460, 12 + kRow + kGap + static_cast<int>(rows_.size()) * (kRow + 4) + 64);
  }
  void resized() override {
    auto r = getLocalBounds().reduced(12, 6);
    heading_.setBounds(r.removeFromTop(kRow));
    r.removeFromTop(kGap);
    for (auto& row : rows_) {
      auto line = r.removeFromTop(kRow);
      r.removeFromTop(4);
      row.label->setBounds(line.removeFromLeft(80));
      row.crossover->setBounds(line.removeFromLeft(110).reduced(0, 1));
      line.removeFromLeft(kGap);
      row.sub->setBounds(line.removeFromLeft(220).reduced(0, 1));
    }
  }

private:
  static constexpr int kFrequencies[] = {40, 60, 80, 100, 120, 150, 200};
  struct BassSetting {
    int hz = 0;
    int sub = -1;
  };
  BassSetting settingOf(int busId) const {
    const auto* b = proc_.mixer().bassFor(BusId{busId});
    if (b == nullptr || !b->active()) return {};
    return {juce::roundToInt(b->crossoverHz), b->subChannel};
  }
  struct Row {
    int busId = 0;
    std::unique_ptr<juce::Label> label;
    std::unique_ptr<juce::ComboBox> crossover;
    std::unique_ptr<juce::ComboBox> sub;
  };
  MasterpieceProcessor& proc_;
  juce::Label heading_;
  std::vector<Row> rows_;
};
}  // namespace

MixerPanel::MixerPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Outputs"));
  addAndMakeVisible(busesLabel_);
  styleLabel(busesLabel_, TRANS("Output pairs"));
  addAndMakeVisible(busCount_);
  for (int i = 1; i <= 8; ++i)
    busCount_.addItem(i == 1 ? TRANS("1 (stereo)") : TRANS("%1 pairs").replace("%1", juce::String(i)), i);
  busCount_.setSelectedId(juce::jmax(1, proc_.mixBusCount()),
                          juce::dontSendNotification);
  busCount_.onChange = [this] { setBusCount(busCount_.getSelectedId()); };

  addAndMakeVisible(spread_);
  spread_.onClick = [this] {
    // Round-robin, which is the useful starting point rather than a
    // suggestion about how this organ should be mixed: it makes the routing
    // audible immediately so the player can hear what they are adjusting.
    const int buses = juce::jmax(1, proc_.mixBusCount());
    for (size_t i = 0; i < rankIds_.size(); ++i)
      rankBuses_[i]->setSelectedId(static_cast<int>(i % buses) + 1,
                                   juce::dontSendNotification);
    pushRouting();
  };
  addAndMakeVisible(reset_);
  reset_.onClick = [this] {
    for (auto& box : rankBuses_)
      box->setSelectedId(1, juce::dontSendNotification);
    pushRouting();
  };
  addAndMakeVisible(save_);
  save_.onClick = [this] { proc_.saveSettings(); };
  addAndMakeVisible(bass_);
  bass_.onClick = [this] {
    juce::Component::SafePointer<MixerPanel> self(this);
    juce::CallOutBox::launchAsynchronously(std::make_unique<BassPanel>(proc_,
                                                                       [self] {
                                                                         if (self != nullptr) self->showOutputs();
                                                                       }),
                                           bass_.getScreenBounds(), nullptr);
  };

  addAndMakeVisible(status_);
  styleLabel(status_, "");
  addAndMakeVisible(outputs_);
  styleLabel(outputs_, "");
  addAndMakeVisible(viewport_);
  viewport_.setViewedComponent(&rankHolder_, false);
  viewport_.setScrollBarsShown(true, false);


  for (auto* h : {&pairsHeading_, &divisionsHeading_, &ranksHeading_}) rankHolder_.addAndMakeVisible(*h);
  styleLabel(pairsHeading_, TRANS("Output pairs"));
  styleLabel(divisionsHeading_, TRANS("Divisions"));
  styleLabel(ranksHeading_, TRANS("Ranks"));

  refresh();
  startTimerHz(10);
}

void MixerPanel::addFader(std::vector<Fader>& into, MidiTargetKind kind, Id id, int index,
                          const juce::String& name) {
  Fader f;
  f.kind = kind;
  f.id = id;
  f.index = index;
  if (name.isNotEmpty()) {
    f.label = std::make_unique<juce::Label>();
    styleLabel(*f.label, name);
    rankHolder_.addAndMakeVisible(*f.label);
  }
  f.slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
  f.slider->setRange(0.0, LiveLevels::kFull, 1.0);
  f.slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, kRow - 6);
  f.slider->textFromValueFunction = [](double v) {
    const int n = juce::roundToInt(v);
    return n <= 0 ? TRANS("Off") : juce::String(LiveLevels::dbOf(n), 1) + " dB";
  };
  f.slider->setValue(faderLevel(f), juce::dontSendNotification);
  f.slider->setTooltip(TRANS("A live level, heard at once: full is the level as voiced"));
  f.midi = std::make_unique<juce::TextButton>(TRANS("MIDI"));
  f.midi->setTooltip(TRANS("Learn a knob or a pedal for this level"));
  rankHolder_.addAndMakeVisible(*f.slider);
  rankHolder_.addAndMakeVisible(*f.midi);
  into.push_back(std::move(f));
  Fader& made = into.back();
  made.slider->onValueChange = [this, &into, i = into.size() - 1] {
    setFaderLevel(into[i], juce::roundToInt(into[i].slider->getValue()));
  };
  made.midi->onClick = [this, &into, i = into.size() - 1] { faderMidiMenu(into[i]); };
}

int MixerPanel::faderLevel(const Fader& f) const {
  switch (f.kind) {
    case MidiTargetKind::PairLevel: return proc_.livePairLevel(f.index);
    case MidiTargetKind::DivisionLevel: return proc_.liveDivisionLevel(f.index);
    case MidiTargetKind::RankLevel: return proc_.liveRankLevel(f.index);
    default: return LiveLevels::kFull;
  }
}

void MixerPanel::setFaderLevel(const Fader& f, int value) {
  switch (f.kind) {
    case MidiTargetKind::PairLevel: proc_.setLivePairLevel(f.index, value); break;
    case MidiTargetKind::DivisionLevel: proc_.setLiveDivisionLevel(f.index, value); break;
    case MidiTargetKind::RankLevel: proc_.setLiveRankLevel(f.index, value); break;
    default: break;
  }
}

// What the knob or pedal for this level is, and Learn and Clear. Learning
// waits for a controller: a key pressed meanwhile plays.
void MixerPanel::faderMidiMenu(Fader& f) {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    map.cancelLearn();
    return;
  }
  juce::PopupMenu menu;
  const auto existing = map.bindingsFor(f.kind, f.id);
  if (existing.empty()) menu.addSectionHeader(TRANS("Not mapped"));
  for (const MidiBinding* b : existing) menu.addSectionHeader(MidiEventDialog::describeBinding(*b));
  menu.addItem(1, TRANS("Learn: move the knob or pedal"));
  menu.addItem(2, TRANS("Clear mapping"), !existing.empty());
  juce::Component::SafePointer<MixerPanel> self(this);
  const MidiTargetKind kind = f.kind;
  const Id id = f.id;
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(f.midi.get()),
                     [self, kind, id](int choice) {
                       if (self == nullptr) return;
                       auto& m = self->proc_.midiMap();
                       if (choice == 1) m.beginLearn(kind, id, false);
                       if (choice == 2) {
                         m.unbindTarget(kind, id);
                         self->proc_.saveMidiMap();
                       }
                     });
}

void MixerPanel::timerCallback() {
  for (auto* list : {&pairFaders_, &divisionFaders_, &rankFaders_})
    for (auto& f : *list) {
      if (f.slider->isMouseButtonDown()) continue;
      const int now = faderLevel(f);
      if (juce::roundToInt(f.slider->getValue()) != now) f.slider->setValue(now, juce::dontSendNotification);
      // A learning button says so, as the console's controls do.
      const bool learning = proc_.midiMap().learning() && proc_.midiMap().learningKind() == f.kind &&
                            proc_.midiMap().learningTarget() == f.id;
      f.midi->setButtonText(learning ? TRANS("Move it...") : TRANS("MIDI"));
    }
}

void MixerPanel::setBusCount(int buses) {
  auto& mixer = proc_.mixer();
  mixer.buses.clear();
  for (int i = 0; i < buses; ++i) {
    MixerBus b;
    b.id = BusId{i + 1};
    // Consecutive pairs on the device. A player with a different layout can
    // say so in the settings file; guessing anything cleverer here would be
    // inventing a convention nobody asked for.
    b.deviceChannels = {2 * i, 2 * i + 1};
    mixer.buses.push_back(b);
  }
  proc_.refreshMixerBuses();

  // The pair faders follow the count.
  pairFaders_.clear();
  for (int i = 0; i < juce::jmin(buses, LiveLevels::kMaxPairs); ++i)
    addFader(pairFaders_, MidiTargetKind::PairLevel, i + 1, i, TRANS("Pair %1").replace("%1", juce::String(i + 1)));
  resized();

  // A rank pointing at a pair that no longer exists would go quiet for a
  // reason nowhere on screen, so fold it back to the first one.
  for (auto& box : rankBuses_) {
    box->clear(juce::dontSendNotification);
    for (int i = 1; i <= buses; ++i) box->addItem(TRANS("Pair %1").replace("%1", juce::String(i)), i);
  }
  for (size_t i = 0; i < rankIds_.size(); ++i) {
    const auto& dest = proc_.mixer().routingFor(rankIds_[i]).perspectives[0].dest;
    int sel = 1;
    if (std::holds_alternative<BusId>(dest)) sel = std::get<BusId>(dest).value;
    rankBuses_[i]->setSelectedId(sel >= 1 && sel <= buses ? sel : 1,
                                 juce::dontSendNotification);
  }
  pushRouting();
}

void MixerPanel::pushRouting() {
  auto& mixer = proc_.mixer();
  for (size_t i = 0; i < rankIds_.size(); ++i) {
    RankRouting r;
    r.rankId = rankIds_[i];
    r.perspectives[0].dest = BusId{rankBuses_[i]->getSelectedId()};
    mixer.rankRoutings[rankIds_[i]] = r;
  }
  const auto d = validateMixer(proc_.organModel(), mixer);
  juce::String text = TRANS("%1 rank(s), %2 pair(s)")
                          .replace("%1", juce::String(static_cast<int>(rankIds_.size())))
                          .replace("%2", juce::String(proc_.mixBusCount()));
  if (!d.clean())
    text += "  -  " + TRANS("%1 unrouted, %2 stale")
                          .replace("%1", juce::String((int)d.unroutedRanks.size()))
                          .replace("%2", juce::String((int)d.ranksRoutedToMissingBus.size()));
  status_.setText(text, juce::dontSendNotification);
  showOutputs();
}

void MixerPanel::showOutputs() {
  // Each pair's device channels, counted from 1 as a device's own panel
  // counts them, and what the device gives the program.
  const int outs = proc_.getTotalNumOutputChannels();
  juce::StringArray pairs;
  bool beyond = false;
  for (const auto& b : proc_.mixer().buses) {
    if (b.deviceChannels.empty()) continue;
    const int left = b.deviceChannels[0];
    const int right = b.deviceChannels.size() > 1 ? b.deviceChannels[1] : left;
    juce::String pair = TRANS("Pair %1: %2-%3")
                            .replace("%1", juce::String(b.id.value))
                            .replace("%2", juce::String(left + 1))
                            .replace("%3", juce::String(right + 1));
    if (const auto* bass = proc_.mixer().bassFor(b.id); bass != nullptr && bass->active()) {
      pair += " (" + TRANS("bass to %1").replace("%1", juce::String(bass->subChannel + 1)) + ")";
      if (bass->subChannel >= outs) beyond = true;
    }
    pairs.add(pair);
    if (left >= outs || right >= outs) beyond = true;
  }
  juce::String text = pairs.joinIntoString(", ");
  if (beyond)
    text += "  -  " + TRANS("the device gives %1 outputs: the pairs past them play on 1-2")
                          .replace("%1", juce::String(outs));
  outputs_.setText(text, juce::dontSendNotification);
}

void MixerPanel::refresh() {
  rankIds_.clear();
  rankLabels_.clear();
  rankBuses_.clear();
  pairFaders_.clear();
  divisionFaders_.clear();
  rankFaders_.clear();
  rankHolder_.removeAllChildren();
  for (auto* h : {&pairsHeading_, &divisionsHeading_, &ranksHeading_}) rankHolder_.addAndMakeVisible(*h);

  for (int i = 0; i < juce::jmin(juce::jmax(1, proc_.mixBusCount()), LiveLevels::kMaxPairs); ++i)
    addFader(pairFaders_, MidiTargetKind::PairLevel, i + 1, i, TRANS("Pair %1").replace("%1", juce::String(i + 1)));
  {
    std::vector<Id> divisions;
    for (const auto& [id, d] : proc_.organModel().divisions) divisions.push_back(id);
    std::sort(divisions.begin(), divisions.end());
    for (Id id : divisions) {
      const auto& d = proc_.organModel().divisions.at(id);
      addFader(divisionFaders_, MidiTargetKind::DivisionLevel, id, proc_.liveDivisionIndexOf(id),
               d.name.empty() ? juce::String(static_cast<int>(id)) : juce::String(d.name));
    }
  }

  for (const auto& [id, rank] : proc_.organModel().ranks) rankIds_.push_back(id);
  std::sort(rankIds_.begin(), rankIds_.end());

  const int buses = juce::jmax(1, proc_.mixBusCount());
  busCount_.setSelectedId(buses, juce::dontSendNotification);

  for (Id rankId : rankIds_) {
    auto label = std::make_unique<juce::Label>();
    const auto it = proc_.organModel().ranks.find(rankId);
    juce::String name = juce::String(static_cast<int>(rankId));
    if (it != proc_.organModel().ranks.end() && !it->second.name.empty())
      name << "  " << juce::String(it->second.name);
    styleLabel(*label, name);
    rankHolder_.addAndMakeVisible(*label);
    rankLabels_.push_back(std::move(label));

    auto box = std::make_unique<juce::ComboBox>();
    for (int i = 1; i <= buses; ++i) box->addItem(TRANS("Pair %1").replace("%1", juce::String(i)), i);
    const auto& dest = proc_.mixer().routingFor(rankId).perspectives[0].dest;
    int sel = 1;
    if (std::holds_alternative<BusId>(dest)) sel = std::get<BusId>(dest).value;
    box->setSelectedId(sel >= 1 && sel <= buses ? sel : 1,
                       juce::dontSendNotification);
    box->onChange = [this] { pushRouting(); };
    rankHolder_.addAndMakeVisible(*box);
    rankBuses_.push_back(std::move(box));
    addFader(rankFaders_, MidiTargetKind::RankLevel, rankId, proc_.liveRankIndexOf(rankId), {});
  }

  pushRouting();
  resized();
}

void MixerPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  heading_.setBounds(r.removeFromTop(kRow));
  // Two rows. One row of five controls overflowed the dialog and squeezed the
  // last button until its label wrapped inside it.
  auto row = r.removeFromTop(kRow);
  busesLabel_.setBounds(row.removeFromLeft(110));
  busCount_.setBounds(row.removeFromLeft(150).reduced(0, 1));
  row.removeFromLeft(kGap);
  status_.setBounds(row);
  r.removeFromTop(4);

  row = r.removeFromTop(kRow);
  spread_.setBounds(row.removeFromLeft(160).reduced(0, 1));
  row.removeFromLeft(6);
  reset_.setBounds(row.removeFromLeft(120).reduced(0, 1));
  row.removeFromLeft(6);
  save_.setBounds(row.removeFromLeft(160).reduced(0, 1));
  row.removeFromLeft(6);
  bass_.setBounds(row.removeFromLeft(170).reduced(0, 1));
  r.removeFromTop(4);
  outputs_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);

  // The note keeps a little space; the rank list takes the rest, which is what
  // makes the page work on an organ with fifty ranks and on one with six.
  auto noteArea = r.removeFromBottom(juce::jmin(76, r.getHeight() / 3));
  r.removeFromBottom(kGap);
  viewport_.setBounds(r);

  // The pairs and divisions first, then a row per rank: its name, its pair,
  // its fader. The fader and its MIDI button sit at the right of each row.
  const int rowH = kRow + 2;
  const int rows = 3 + static_cast<int>(pairFaders_.size() + divisionFaders_.size() + rankIds_.size());
  rankHolder_.setSize(juce::jmax(0, viewport_.getWidth() - 12), rows * rowH);
  int y = 0;
  const auto line = [&] {
    juce::Rectangle<int> l(0, y, rankHolder_.getWidth(), kRow);
    y += rowH;
    return l;
  };
  const auto place = [](Fader& f, juce::Rectangle<int> l) {
    f.midi->setBounds(l.removeFromRight(70).reduced(2, 1));
    f.slider->setBounds(l.removeFromRight(260).reduced(2, 1));
    if (f.label != nullptr) f.label->setBounds(l);
    return l;
  };
  pairsHeading_.setBounds(line());
  for (auto& f : pairFaders_) place(f, line());
  divisionsHeading_.setBounds(line());
  for (auto& f : divisionFaders_) place(f, line());
  ranksHeading_.setBounds(line());
  for (size_t i = 0; i < rankIds_.size(); ++i) {
    auto l = line();
    if (i < rankFaders_.size()) l = place(rankFaders_[i], l);
    rankBuses_[i]->setBounds(l.removeFromRight(120).reduced(2, 1));
    rankLabels_[i]->setBounds(l);
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
              : storage == SampleStorage::Int24 ? "24-bit"
                                                : "32-bit float") +
             ", memory limit " +
             (p.memoryLimitSettingMB() > 0 ? juce::String(p.memoryLimitSettingMB()) + " MB" : juce::String("not set")) +
             ", " + juce::String(p.streamUnderruns()) + " stream underruns");
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
}

void RemotePanel::resized() {
  auto r = getLocalBounds().reduced(16);
  heading_.setBounds(r.removeFromTop(28));
  r.removeFromTop(6);
  enable_.setBounds(r.removeFromTop(28));
  write_.setBounds(r.removeFromTop(28));
  power_.setBounds(r.removeFromTop(28));
  local_.setBounds(r.removeFromTop(28));
  sysex_.setBounds(r.removeFromTop(28));
  {
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

SettingsWindow::SettingsWindow(MasterpieceProcessor& p,
                               juce::AudioDeviceManager* devices)
    : reverb_(p), metronome_(p), performance_(p, EnginePanel::Part::Performance),
      general_(p, EnginePanel::Part::General), favourites_(p), display_(p), language_(p), log_(p) {
  const auto bg = juce::Colour(0xff1b1e24);
  addAndMakeVisible(tabs_);
  // Without a device manager -- a plugin, whose host owns the audio device
  // and the MIDI ports -- the Audio and MIDI tabs are left out.
  if (devices != nullptr) {
    audio_ = std::make_unique<AudioSettingsPanel>(*devices, p);
    tabs_.addTab(TRANS("Audio"), bg, audio_.get(), false);
  }
  if (devices != nullptr) {
    midi_ = std::make_unique<MidiPanel>(p, *devices, MidiPanel::Part::Console);
    midiScroll_ = std::make_unique<ScrollHost>(*midi_, 0, [this] { return midi_->contentHeight(); });
    tabs_.addTab(TRANS("MIDI"), bg, midiScroll_.get(), false);
  }
  tabs_.addTab(TRANS("Performance"), bg, &performanceScroll_, false);
  tabs_.addTab(TRANS("General"), bg, &generalScroll_, false);
  tabs_.addTab(TRANS("Reverb"), bg, &reverb_, false);
  tabs_.addTab(TRANS("Metronome"), bg, &metronome_, false);
  tabs_.addTab(TRANS("Combination sets"), bg, &favourites_, false);
  tabs_.addTab(TRANS("Console display"), bg, &display_, false);
  tabs_.addTab(TRANS("Language"), bg, &language_, false);
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
