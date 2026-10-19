#include "Settings.h"

#include <map>

#include "ManualDialog.h"
#include "Ui.h"
#include "Mobile.h"
#include "../mp_audio/Translations.h"

namespace mp::ui {
namespace {

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

constexpr int kRow = 28;
constexpr int kGap = 6;

} // namespace

// ---------------------------------------------------------------- engine

EnginePanel::EnginePanel(MasterpieceProcessor& p) : proc_(p) {
  // Before a single control is wired: what Revert goes back to.
  openSwitch_ = proc_.engineSwitch();
  openPreload_ = proc_.preloadHeadFrames();
  openStorage_ = proc_.sampleStorage();
  openStream_ = proc_.streamReleases();
  openStreamPercent_ = proc_.streamHeadPercent();
  openMono_ = proc_.loadMono();
  openRate_ = proc_.loadSampleRate();
  openReleases_ = proc_.releaseChoice();
  openReleaseMs_ = proc_.releaseMaxMs();
  openCache_ = proc_.cacheMode();
  openLoadTicks_ = proc_.loadTicks();
  openReopen_ = proc_.reopenLastOrgan();
  openFaster_ = proc_.fasterEngine();
  openRealtime_ = proc_.realtimeThreads();
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
  styleLabel(profileLabel_, TRANS("Memory profile"));
  addAndMakeVisible(profile_);
  // Named for what they do, not for a machine they suit. The figures that
  // actually decide whether one fits are in the readout below the controls,
  // measured on the organ in front of you rather than assumed.
  profile_.addItem(TRANS("Best quality - 24-bit, hold everything"), 1);
  profile_.addItem(TRANS("Recommended - 16-bit, stream releases"), 2);
  profile_.addItem(TRANS("Smallest - 16-bit mono, stream releases"), 3);
  profile_.addItem(TRANS("Custom"), 4);
  profile_.onChange = [this] {
    if (applyingProfile_) return;
    if (profile_.getSelectedId() != 4) applyProfile(profile_.getSelectedId());
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
  storage_.addItem(TRANS("16-bit - two thirds of that, inaudible"), 2);
  storage_.addItem(TRANS("32-bit float - larger than the source"), 3);
  storage_.setSelectedId(proc_.sampleStorage() == SampleStorage::Int16     ? 2
                         : proc_.sampleStorage() == SampleStorage::Float32 ? 3
                                                                           : 1,
                         juce::dontSendNotification);
  storage_.onChange = [this] {
    const int id = storage_.getSelectedId();
    proc_.setSampleStorage(id == 2   ? SampleStorage::Int16
                           : id == 3 ? SampleStorage::Float32
                                     : SampleStorage::Int24);
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
  };
  addAndMakeVisible(releaseLengthLabel_);
  styleLabel(releaseLengthLabel_, TRANS("Release length"));
  addAndMakeVisible(releaseLength_);
  showReleaseLength(proc_.releaseMaxMs());
  releaseLength_.setTooltip(TRANS("Each release is kept up to this length and fades out over its "
                                  "last quarter. Shorter releases use less memory: for an organ "
                                  "played in a room with its own reverberation, or under an "
                                  "impulse response."));
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
    static const int kMs[] = {0, 500, 1000, 1500, 2000, 3000, 4000};
    const int id = releaseLength_.getSelectedId();
    if (id >= 1 && id <= 7) proc_.setReleaseMaxMs(kMs[id - 1]);
  };

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
    streamHead_.setEnabled(stream_.getToggleState());
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
  streamHead_.setEnabled(proc_.streamReleases());
  streamHead_.onChange = [this] {
    const int id = streamHead_.getSelectedId();
    proc_.setStreamHeadPercent(id == 2 ? 25 : id == 3 ? 50 : id == 4 ? 75 : 0);
  };

  addAndMakeVisible(mono_);
  mono_.setToggleState(proc_.loadMono(), juce::dontSendNotification);
  mono_.onClick = [this] {
    proc_.setLoadMono(mono_.getToggleState());
    syncProfile();
  };

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
  };
  showFasterUnit();
  // Real-time render threads, off until players have tried it. Android gives
  // an app no way to have them.
  if (MasterpieceProcessor::realtimeThreadsSupported())
    addAndMakeVisible(realtime_);
  else
    addChildComponent(realtime_);
  realtime_.setToggleState(proc_.realtimeThreads(), juce::dontSendNotification);
  realtime_.setTooltip(TRANS("Makes the threads that render the organ real-time threads, so the system runs "
                             "them first, on the fastest cores: MMCSS on Windows, SCHED_FIFO or rtkit on Linux, "
                             "the audio workgroup on macOS. Off by default while it is tried. If the late blocks "
                             "in the status bar go down or up with it on, please tell us in the discussion on "
                             "GitHub."));
  realtime_.onClick = [this] {
    proc_.setRealtimeThreads(realtime_.getToggleState());
    showRealtimeUnit();
  };
  showRealtimeUnit();
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
  polyphony_.onChange = [this] {
    const int id = polyphony_.getSelectedId();
    // Ids above 1000 are a number of voices plus 1000; 1 is every voice.
    proc_.setPolyphonyLimit(id == 1 ? 0 : id - 1000);
  };

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
  addAndMakeVisible(note_);
  styleNote(note_,
            TRANS("These settings take effect on the next organ load.\n\n"
                  "\"Up to the loop\" keeps each attack from its start to the end of "
                  "its sustain loop, which is all a held note plays; a sample whose "
                  "release is in the same file is always kept whole.\n\n"
                  "The resident format decides what a held frame costs. 16-bit halves "
                  "the memory and is what most players load by default; each sample "
                  "is scaled by its own peak first, so a quiet stop keeps the full "
                  "sixteen bits instead of only the top few.\n\n"
                  "Streaming holds only the start of each release - the first second, "
                  "or the share chosen beside it - and fetches the rest from disk while "
                  "it plays. A larger share gives a slow disk longer to catch up. Releases are the only samples "
                  "in an organ worth streaming - several seconds each, played once, "
                  "straight through, never looped - and an attack cannot be treated "
                  "the same way because its sustain loop has to be resident. On the "
                  "Nancy demo this is 12.2 GB down to 5.6 GB, with the rendered "
                  "audio identical sample for sample. Needs a disk that keeps up; "
                  "the Engine readout says if it did not.\n\n"
                  "\"Keep this organ playable without its installation files\" gives "
                  "the organ a sample cache of its own, whatever the Sample cache "
                  "setting says, and keeps a copy of its definition and console "
                  "pictures beside it. It is made at the next complete load. Released "
                  "notes streamed from disk still need the installation files, so "
                  "turn streaming off for an organ that has to play without them."));
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
  proc_.setLoadMono(openMono_);
  mono_.setToggleState(openMono_, juce::dontSendNotification);
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
  proc_.setMemoryLimitMB(openMemLimit_);
  fillMemLimit();
  proc_.setPolyphonyLimit(openPolyphony_);
  fillPolyphony();
  proc_.setLoadSampleRate(openRate_);
  proc_.setReleaseChoice(openReleases_);
  releases_.setSelectedId(static_cast<int>(openReleases_) + 1, juce::dontSendNotification);
  proc_.setReleaseMaxMs(openReleaseMs_);
  showReleaseLength(openReleaseMs_);
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
  storage_.setSelectedId(openStorage_ == SampleStorage::Int16 ? 2 : 1,
                         juce::dontSendNotification);
  stream_.setToggleState(openStream_, juce::dontSendNotification);
  showStreamPercent(openStreamPercent_);
  streamHead_.setEnabled(openStream_);
  showPreload(openPreload_);

  // simpleWavOnly greys the rest out; restoring the states has to restore
  // that too, or the panel lies about what is reachable.
  const bool detailed = !openSwitch_.simpleWavOnly;
  for (auto* b : {&wind_, &tremulant_, &enclosure_, &voicing_})
    b->setEnabled(detailed);
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
  const juce::String base = TRANS("Real-time render threads (trial)");
  const juce::String how = MasterpieceProcessor::realtimeMethod();
  realtime_.setButtonText(!proc_.realtimeThreads() ? base
                          : how.isNotEmpty()       ? TRANS("Real-time render threads (trial): using %1").replace("%1", how)
                                                   : TRANS("Real-time render threads (trial): no real-time rights "
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
  static const int kMs[] = {0, 500, 1000, 1500, 2000, 3000, 4000};
  releaseLength_.clear(juce::dontSendNotification);
  releaseLength_.addItem(TRANS("Whole"), 1);
  for (int i = 1; i < 7; ++i)
    releaseLength_.addItem(TRANS("Up to %1 s").replace("%1", juce::String(kMs[i] / 1000.0, kMs[i] % 1000 ? 1 : 0)), i + 1);
  int id = 1;
  for (int i = 0; i < 7; ++i)
    if (kMs[i] == ms) id = i + 1;
  // A length set in the file and not in the list is shown as it is.
  if (ms > 0 && id == 1) {
    releaseLength_.addItem(TRANS("Up to %1 s").replace("%1", juce::String(ms / 1000.0, 2)), 8);
    id = 8;
  }
  releaseLength_.setSelectedId(id, juce::dontSendNotification);
}

bool EnginePanel::loadingChanged() const {
  const auto sw = proc_.engineSwitch();
  return proc_.sampleStorage() != openStorage_ || proc_.streamReleases() != openStream_ ||
         proc_.releaseChoice() != openReleases_ || proc_.releaseMaxMs() != openReleaseMs_ ||
         proc_.streamHeadPercent() != openStreamPercent_ ||
         proc_.loadMono() != openMono_ || proc_.loadSampleRate() != openRate_ ||
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
  const bool offer = onReload != nullptr && proc_.loadedOrganFile().getFullPathName().isNotEmpty();
  if (reload_.isVisible() != offer) reload_.setVisible(offer);
  const bool pending = offer && loadingChanged();
  if (rebuildNote_.isVisible() != cacheRebuildPending()) rebuildNote_.setVisible(cacheRebuildPending());
  const juce::String reloadText = pending ? TRANS("Load the organ again to apply") : TRANS("Load the organ again");
  if (reload_.getButtonText() != reloadText) {
    reload_.setButtonText(reloadText);
    reload_.setColour(juce::TextButton::buttonColourId,
                      pending ? juce::Colour(0xff8a5a12) : juce::Colour(0xff2a2f38));
  }

  const auto bytes = proc_.sampleLibrary().residentBytes();
  const auto fmt = proc_.sampleStorage();
  const juce::String format = fmt == SampleStorage::Int16     ? juce::String("16-bit")
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
void EnginePanel::applyProfile(int id) {
  applyingProfile_ = true;
  struct Profile {
    SampleStorage storage;
    bool mono;
    bool stream;
    int64_t head;
  };
  const Profile p = id == 1   ? Profile{SampleStorage::Int24, false, false, 0}
                    : id == 2 ? Profile{SampleStorage::Int16, false, true, 0}
                              : Profile{SampleStorage::Int16, true, true, 0};

  proc_.setSampleStorage(p.storage);
  proc_.setLoadMono(p.mono);
  proc_.setStreamReleases(p.stream);
  proc_.setPreloadHeadFrames(p.head);

  storage_.setSelectedId(p.storage == SampleStorage::Int16     ? 2
                         : p.storage == SampleStorage::Float32 ? 3
                                                               : 1,
                         juce::dontSendNotification);
  mono_.setToggleState(p.mono, juce::dontSendNotification);
  stream_.setToggleState(p.stream, juce::dontSendNotification);
  streamHead_.setEnabled(p.stream);
  preload_.setSelectedId(1, juce::dontSendNotification);
  applyingProfile_ = false;
}

void EnginePanel::syncProfile() {
  const auto st = proc_.sampleStorage();
  const bool mono = proc_.loadMono();
  const bool stream = proc_.streamReleases();
  const bool wholeHead = proc_.preloadHeadFrames() == 0;

  int id = 4; // Custom, until the settings match one of the three exactly
  if (wholeHead) {
    if (st == SampleStorage::Int24 && !mono && !stream) id = 1;
    else if (st == SampleStorage::Int16 && !mono && stream) id = 2;
    else if (st == SampleStorage::Int16 && mono && stream) id = 3;
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

void EnginePanel::resized() {
  auto r = getLocalBounds().reduced(12);
  // The buttons first, at their full height, whatever the rows above need:
  // when a row was added in 0.7.2 the rows ran past the panel's height and
  // the footer took the shortfall, which left the buttons too short to tap
  // (#149). The note between them takes what is left instead.
  auto footer = r.removeFromBottom(kRow + 4);
  r.removeFromBottom(kGap);
  for (auto* b : {&simpleWav_, &wind_, &tremulant_, &enclosure_, &voicing_,
                  &originalPitch_}) {
    b->setBounds(r.removeFromTop(kRow));
    r.removeFromTop(2);
  }
  r.removeFromTop(kGap);
  auto profileRow = r.removeFromTop(kRow);
  profileLabel_.setBounds(profileRow.removeFromLeft(180));
  profile_.setBounds(profileRow.removeFromLeft(320));
  r.removeFromTop(10);
  auto row = r.removeFromTop(kRow);
  preloadLabel_.setBounds(row.removeFromLeft(180));
  preload_.setBounds(row.removeFromLeft(260));
  r.removeFromTop(6);
  auto storageRow = r.removeFromTop(kRow);
  storageLabel_.setBounds(storageRow.removeFromLeft(180));
  storage_.setBounds(storageRow.removeFromLeft(260));
  r.removeFromTop(6);
  auto rateRow = r.removeFromTop(kRow);
  rateLabel_.setBounds(rateRow.removeFromLeft(180));
  rate_.setBounds(rateRow.removeFromLeft(260));
  r.removeFromTop(6);
  auto releasesRow = r.removeFromTop(kRow);
  releasesLabel_.setBounds(releasesRow.removeFromLeft(180));
  releases_.setBounds(releasesRow.removeFromLeft(300));
  r.removeFromTop(6);
  auto releaseLengthRow = r.removeFromTop(kRow);
  releaseLengthLabel_.setBounds(releaseLengthRow.removeFromLeft(180));
  releaseLength_.setBounds(releaseLengthRow.removeFromLeft(300));
  r.removeFromTop(6);
  auto releaseCutoffRow = r.removeFromTop(kRow);
  releaseCutoffLabel_.setBounds(releaseCutoffRow.removeFromLeft(180));
  releaseCutoff_.setBounds(releaseCutoffRow.removeFromLeft(300));
  r.removeFromTop(6);
  auto cacheRow = r.removeFromTop(kRow);
  cacheLabel_.setBounds(cacheRow.removeFromLeft(180));
  cache_.setBounds(cacheRow.removeFromLeft(300));
  r.removeFromTop(6);
  auto cacheWhenRow = r.removeFromTop(kRow);
  cacheWhenLabel_.setBounds(cacheWhenRow.removeFromLeft(180));
  cacheWhen_.setBounds(cacheWhenRow.removeFromLeft(300));
  rebuildNote_.setBounds(r.removeFromTop(kRow).withTrimmedLeft(180));
  r.removeFromTop(6);
  auto cacheDirRow = r.removeFromTop(kRow);
  cacheDirLabel_.setBounds(cacheDirRow.removeFromLeft(180));
  cacheDirChoose_.setBounds(cacheDirRow.removeFromRight(90).reduced(0, 2));
  cacheDirRow.removeFromRight(6);
  cacheDirDefault_.setBounds(cacheDirRow.removeFromRight(80).reduced(0, 2));
  cacheDirRow.removeFromRight(8);
  cacheDirValue_.setBounds(cacheDirRow);
  r.removeFromTop(6);
  auto packageDirRow = r.removeFromTop(kRow);
  packageDirLabel_.setBounds(packageDirRow.removeFromLeft(180));
  packageDirChoose_.setBounds(packageDirRow.removeFromRight(90).reduced(0, 2));
  packageDirRow.removeFromRight(6);
  packageDirClear_.setBounds(packageDirRow.removeFromRight(80).reduced(0, 2));
  packageDirRow.removeFromRight(8);
  packageDirValue_.setBounds(packageDirRow);
  r.removeFromTop(6);
  auto organRootRow = r.removeFromTop(kRow);
  organRootLabel_.setBounds(organRootRow.removeFromLeft(180));
  organRootChoose_.setBounds(organRootRow.removeFromRight(90).reduced(0, 2));
  organRootRow.removeFromRight(6);
  organRootDefault_.setBounds(organRootRow.removeFromRight(80).reduced(0, 2));
  organRootRow.removeFromRight(8);
  organRootValue_.setBounds(organRootRow);
  r.removeFromTop(6);
  {
    auto streamRow = r.removeFromTop(kRow);
    streamHead_.setBounds(streamRow.removeFromRight(230).reduced(0, 2));
    stream_.setBounds(streamRow);
  }
  r.removeFromTop(2);
  mono_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(2);
  loadTicks_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(2);
  reopen_.setBounds(r.removeFromTop(kRow));
  portable_.setBounds(r.removeFromTop(kRow));
  {
    auto fasterRow = r.removeFromTop(kRow);
    if (realtime_.isVisible()) realtime_.setBounds(fasterRow.removeFromRight(fasterRow.getWidth() / 2));
    faster_.setBounds(fasterRow);
  }
  r.removeFromTop(6);
  auto memRow = r.removeFromTop(kRow);
  memLimitLabel_.setBounds(memRow.removeFromLeft(180));
  memLimit_.setBounds(memRow.removeFromLeft(300));
  r.removeFromTop(4);
  auto polyRow = r.removeFromTop(kRow);
  polyphonyLabel_.setBounds(polyRow.removeFromLeft(180));
  polyphony_.setBounds(polyRow.removeFromLeft(300));
  r.removeFromTop(kGap);
  memory_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);

  // The footer, taken above. Read left to right it goes from discarding to
  // committing hardest, so the consequence grows with the distance from "put
  // it back". The buttons share the whole width, each in proportion to its
  // label, with the same gap between every pair: two groups pushed to either
  // side left a hole in the middle as wide as the panel was spare (#120).
  // "Load the organ again" is laid out even while it is hidden, so the others
  // do not jump when it appears.
  layoutFooter(footer);

  note_.setBounds(r);
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

  addAndMakeVisible(note_);
  styleNote(note_,
            TRANS("Most sample sets are recorded in the room they live in, and "
                  "several ship close / far / rear perspectives which ARE the room. "
                  "Convolution is for a dry set, or for headphones, and usually "
                  "wants far less wet signal than a reverb plugin would suggest.") +
                "\n\n" +
                TRANS("The room chosen here is kept with the organ that is loaded, and each "
                      "organ opens with its own."));
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
  note_.setBounds(r);
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
  for (auto* b : {&record_, &play_, &stop_, &save_, &load_, &clear_})
    addAndMakeVisible(*b);

  record_.onClick = [this] { proc_.recorder().startRecording(); };
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
                              proc_.recorder().saveToFile(
                                  f.withFileExtension(".mid"));
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
                            if (f.existsAsFile()) proc_.recorder().loadFromFile(f);
                          });
  };

  addAndMakeVisible(status_);
  styleLabel(status_, "");
  addAndMakeVisible(note_);
  styleNote(note_,
            TRANS("Records everything that reaches the engine, not just notes: stop "
                  "changes, couplers and shoe movements that arrive as MIDI are "
                  "captured too, so a recording plays back as the same performance "
                  "rather than as notes on whatever registration happens to be "
                  "drawn at the time."));

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
  status_.setText(s, juce::dontSendNotification);
  record_.setToggleState(r.isRecording(), juce::dontSendNotification);
  play_.setEnabled(!r.empty());
  save_.setEnabled(!r.empty());

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

void RecorderPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  auto row = r.removeFromTop(kRow);
  for (auto* b : {&record_, &play_, &stop_}) {
    b->setBounds(row.removeFromLeft(90));
    row.removeFromLeft(kGap);
  }
  r.removeFromTop(kGap);
  row = r.removeFromTop(kRow);
  for (auto* b : {&save_, &load_, &clear_}) {
    b->setBounds(row.removeFromLeft(110));
    row.removeFromLeft(kGap);
  }
  r.removeFromTop(kGap);
  status_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  note_.setBounds(r.removeFromTop(72));

  // Audio capture below the MIDI half, with its own heading: they are two
  // recordings of the same performance and mixing the controls would suggest
  // one set of transport buttons drives both.
  r.removeFromTop(kGap * 2);
  audioHeading_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(2);
  row = r.removeFromTop(kRow);
  audioRecord_.setBounds(row.removeFromLeft(140));
  row.removeFromLeft(kGap);
  audioStop_.setBounds(row.removeFromLeft(90));
  r.removeFromTop(kGap);
  audioStatus_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  audioNote_.setBounds(r);
}

// ------------------------------------------------------------------ midi

MidiPanel::MidiPanel(MasterpieceProcessor& p, juce::AudioDeviceManager& devices)
    : proc_(p), devices_(devices) {
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
  addAndMakeVisible(noiseFilter_);
  noiseFilter_.setToggleState(proc_.learnNoiseFilter(), juce::dontSendNotification);
  noiseFilter_.setTooltip(TRANS("For a console whose contacts send stray notes: a shorter note is not learned. "
                                "Controllers and program changes are learned at once. Leave it off for pistons "
                                "that send only a short note."));
  noiseFilter_.onClick = [this] { proc_.setLearnNoiseFilter(noiseFilter_.getToggleState()); };

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
  addAndMakeVisible(note_);
  styleNote(note_,
            TRANS("To map a control: right-click a drawstop on the console, then "
                  "move the control on your console. Unmapped messages keep working "
                  "as they are, so an organ is playable the moment it loads rather "
                  "than only after a mapping session.\n\n"
                  "The mapping is saved per organ, in your own application data - "
                  "never inside the sample set.\n\n"
                  "Keyboards: the channel decides which manual your console plays, "
                  "and therefore which division sounds. Couplers work off that, so "
                  "getting it wrong makes a manual sound like the wrong one."
                  "\n\nSequencer and console actions: the organ declares none of "
                  "these, so there is nothing on the console to right-click. Press "
                  "a Learn button here, then the piston you want to use."));

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
  };
  for (auto& e : consoleLearn) {
    addAndMakeVisible(*e.b);
    const auto kind = e.k;
    e.b->onClick = [this, kind] {
      proc_.midiMap().beginLearn(kind, 0, false);
    };
  }

  refresh();
  startTimerHz(2);
}

MidiPanel::~MidiPanel() { stopTimer(); }

// Manuals that answer the same channel, named. Sharing one is a way of
// playing two divisions from one keyboard; doing it unawares is how a manual
// ends up sounding the wrong division, which is what this line prevents.
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
  sharedNote_.setText(text.isEmpty() ? juce::String() : TRANS("Shared -- %1").replace("%1", text),
                      juce::dontSendNotification);
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
    dev->onChange = [this, kb, rawBox, rawDev] {
      const int channel = rawBox->getSelectedId() > 1
                              ? rawBox->getSelectedId() - 1
                              : proc_.channelForKeyboard(kb);
      proc_.keepRouting();
      proc_.setKeyboardForChannel(channel, kb, rawDev->getSelectedId() - 1);
      proc_.saveMidiMap();
    };
    addAndMakeVisible(*dev);
    keyboardDevices_.push_back(std::move(dev));

    auto more = std::make_unique<juce::TextButton>(TRANS("Range, transpose..."));
    more->onClick = [this, kb] { ManualDialog::show(proc_, kb); };
    addAndMakeVisible(*more);
    keyboardMore_.push_back(std::move(more));
  }

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
  resized();
}

void MidiPanel::timerCallback() {
  // A tab of the settings window that is not the one on show.
  if (!isShowing()) return;
  const auto& map = proc_.midiMap();
  juce::String s = TRANS("%1 binding(s)").replace("%1", juce::String(static_cast<int>(map.size())));
  if (map.learning()) s += "  -  " + TRANS("LEARNING: move a control now");
  mapStatus_.setText(s, juce::dontSendNotification);

}

void MidiPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  inputsLabel_.setBounds(r.removeFromTop(kRow));
  for (auto& b : inputs_) {
    b->setBounds(r.removeFromTop(kRow).reduced(12, 0));
    r.removeFromTop(2);
  }
  if (inputs_.empty()) {
    r.removeFromTop(kRow); // leaves room for the "none found" case
  }
  r.removeFromTop(kGap);
  keyboardsLabel_.setBounds(r.removeFromTop(kRow));
  for (size_t i = 0; i < keyboardChannels_.size(); ++i) {
    auto kbRow = r.removeFromTop(kRow).reduced(12, 0);
    keyboardLabels_[i]->setBounds(kbRow.removeFromLeft(150));
    keyboardChannels_[i]->setBounds(kbRow.removeFromLeft(170));
    kbRow.removeFromLeft(6);
    if (i < keyboardDevices_.size())
      keyboardDevices_[i]->setBounds(kbRow.removeFromLeft(200));
    kbRow.removeFromLeft(6);
    if (i < keyboardMore_.size())
      keyboardMore_[i]->setBounds(kbRow.removeFromLeft(160).reduced(0, 1));
    r.removeFromTop(2);
  }

  sharedNote_.setBounds(r.removeFromTop(kRow).reduced(12, 0));
  {
    auto defRow = r.removeFromTop(kRow).reduced(12, 0);
    useAsDefault_.setBounds(defRow.removeFromLeft(220).reduced(0, 1));
    defRow.removeFromLeft(6);
    clearDefault_.setBounds(defRow.removeFromLeft(110).reduced(0, 1));
    defRow.removeFromLeft(10);
    defaultNote_.setBounds(defRow);
  }

  r.removeFromTop(kGap);
  auto row = r.removeFromTop(kRow);
  outputsLabel_.setBounds(row.removeFromLeft(120));
  output_.setBounds(row.removeFromLeft(300));
  r.removeFromTop(4);
  feedback_.setBounds(r.removeFromTop(kRow));
  cancelResets_.setBounds(r.removeFromTop(kRow));
  noiseFilter_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  row = r.removeFromTop(kRow);
  stepperLabel_.setBounds(row.removeFromLeft(120));
  learnPrev_.setBounds(row.removeFromLeft(150).reduced(2, 0));
  row.removeFromLeft(6);
  learnNext_.setBounds(row.removeFromLeft(150).reduced(2, 0));

  // The manual buttons on a row of their own: beside the sequencer they read
  // as part of it, and a dialog at its default size cut them off (#90).
  r.removeFromTop(4);
  row = r.removeFromTop(kRow);
  keyboardsHeading_.setBounds(row.removeFromLeft(120));
  learnManuals_.setBounds(row.removeFromLeft(190).reduced(2, 0));

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
  for (auto* b : {&learnTransposeDown_, &learnTransposeUp_, &learnTemperamentPrev_,
                  &learnTemperamentNext_}) {
    b->setBounds(row.removeFromLeft(150).reduced(2, 0));
    row.removeFromLeft(6);
  }

  r.removeFromTop(kGap);
  row = r.removeFromTop(kRow);
  saveMap_.setBounds(row.removeFromLeft(120));
  row.removeFromLeft(kGap);
  clearMap_.setBounds(row.removeFromLeft(120));
  row.removeFromLeft(kGap);
  mapStatus_.setBounds(row);
  r.removeFromTop(kGap);
  note_.setBounds(r);
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

  addAndMakeVisible(note_);
  styleNote(note_,
            TRANS("A combination set is a whole registration book: one for a "
                  "recital, another for a service. Changing set saves the one you "
                  "are leaving first, so nothing you captured is lost by switching.\n\n"
                  "Favourite organs are in Open: search them, put them in your own "
                  "order, rename them, and add the organ now loaded."));
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
  note_.setBounds(r.removeFromTop(120));
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

  addAndMakeVisible(abSwap_);
  abSwap_.onClick = [this] {
    proc_.voicing().swap();
    loadCurrentIntoSliders();
    updateStatus();
  };
  addAndMakeVisible(abCopy_);
  abCopy_.onClick = [this] {
    proc_.voicing().copyToOther();
    updateStatus();
  };
  addAndMakeVisible(resetOne_);
  resetOne_.onClick = [this] {
    gain_.setValue(0.0, juce::dontSendNotification);
    tune_.setValue(0.0, juce::dontSendNotification);
    pushCurrent();
  };
  addAndMakeVisible(resetAll_);
  resetAll_.onClick = [this] {
    proc_.voicing().live().clear();
    loadCurrentIntoSliders();
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
  loadCurrentIntoSliders();
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
    proc_.voicing().live().setRank(rankId, pv);
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

MixerPanel::MixerPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Mixer"));
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

  addAndMakeVisible(status_);
  styleLabel(status_, "");
  addAndMakeVisible(viewport_);
  viewport_.setViewedComponent(&rankHolder_, false);
  viewport_.setScrollBarsShown(true, false);

  addAndMakeVisible(note_);
  styleNote(note_,
            TRANS("No sample set says anything about audio routing - not one - so "
                  "this is yours to decide, like the MIDI mapping. Output pairs and "
                  "their device channels carry across organs; which rank goes where "
                  "is saved per organ, because a rank number means nothing in a "
                  "different instrument.\n\n"
                  "A rank you never touch plays through the first pair, so an organ "
                  "is audible before you open this page. If you are listening in "
                  "stereo the pairs are summed, so nothing disappears when you "
                  "split them up."));

  refresh();
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
}

void MixerPanel::refresh() {
  rankIds_.clear();
  rankLabels_.clear();
  rankBuses_.clear();
  rankHolder_.removeAllChildren();

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
  r.removeFromTop(kGap);

  // The note keeps a little space; the rank list takes the rest, which is what
  // makes the page work on an organ with fifty ranks and on one with six.
  auto noteArea = r.removeFromBottom(juce::jmin(76, r.getHeight() / 3));
  note_.setBounds(noteArea);
  r.removeFromBottom(kGap);
  viewport_.setBounds(r);

  const int rowH = kRow + 2;
  rankHolder_.setSize(juce::jmax(0, viewport_.getWidth() - 12),
                      static_cast<int>(rankIds_.size()) * rowH);
  for (size_t i = 0; i < rankIds_.size(); ++i) {
    juce::Rectangle<int> line(0, static_cast<int>(i) * rowH,
                              rankHolder_.getWidth(), kRow);
    rankBuses_[i]->setBounds(line.removeFromRight(140).reduced(2, 1));
    rankLabels_[i]->setBounds(line);
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
  if (auto* app = juce::JUCEApplicationBase::getInstance()) version = app->getApplicationVersion();
  out.add("Masterpiece " + version + ", " + juce::SystemStats::getOperatingSystemName() + ", " +
          juce::SystemStats::getDeviceDescription());
  out.add(juce::String(juce::SystemStats::getNumCpus()) + " processors, " +
          juce::String(juce::SystemStats::getMemorySizeInMegabytes()) + " MB of memory");
  out.add("Checked " + juce::Time::getCurrentTime().toISO8601(true));
  out.add({});

  const auto data = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                        .getChildFile("Masterpiece");
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
             (storage == SampleStorage::Int16 ? "16-bit" : storage == SampleStorage::Int24 ? "24-bit" : "32-bit float") +
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
    report.replaceWithText(reportWithLog());
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
  updates_.setTooltip(TRANS("Reads the newest version's number from masterpieceorgan.com, a few bytes, "
                            "and offers the download page when it is newer. Nothing about you is sent."));
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
                           "the shoes, and the API that consoles and scripts use (HTTP, JSON). Off by default."));
  enable_.onClick = [this] { apply(); };
  addAndMakeVisible(write_);
  write_.setToggleState(s.allowChanges, juce::dontSendNotification);
  write_.setTooltip(TRANS("Off, the network can only read: the stops drawn, the levels, the organ loaded."));
  write_.onClick = [this] { apply(); };
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
  addAndMakeVisible(note_);
  styleNote(note_, TRANS("While it is on, anyone on this network can open the page. Without a token, anyone can "
                          "also change the registration from it; with one, only those who know it."));
  timerCallback();
  startTimerHz(1);
}

void RemotePanel::apply() {
  MasterpieceProcessor::RemoteSettings s;
  s.enabled = enable_.getToggleState();
  s.allowChanges = write_.getToggleState();
  s.port = juce::jlimit(1, 65535, port_.getText().getIntValue() > 0 ? port_.getText().getIntValue() : 8080);
  port_.setText(juce::String(s.port), juce::dontSendNotification);
  s.token = token_.getText().trim().toStdString();
  const auto& now = proc_.remoteSettings();
  if (s.enabled == now.enabled && s.allowChanges == now.allowChanges && s.port == now.port && s.token == now.token)
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
  note_.setBounds(r.removeFromTop(60));
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

  addAndMakeVisible(note_);
  styleNote(note_,
            TRANS("A console display is driven by system exclusive messages, and the "
                  "bytes that introduce one belong to the display hardware rather "
                  "than to the organ - so they are typed in here rather than "
                  "guessed. 7D is the id the MIDI specification reserves for "
                  "non-commercial use, which is the honest default when your "
                  "manufacturer's prefix is unknown.\n\n"
                  "Choose the MIDI output on the MIDI page first: this sends "
                  "through the same port that lights your drawstops.\n\n"
                  "Only lines whose text actually changed are sent, so a display "
                  "showing a steady temperament costs nothing. Accented letters are "
                  "transliterated, because system exclusive carries seven bits and a "
                  "high byte would end the message rather than draw a character."));

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
  note_.setBounds(r);
}

// -------------------------------------------------------------- language

LanguagePanel::LanguagePanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, TRANS("Language"));
  addAndMakeVisible(language_);
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
  addAndMakeVisible(note_);
  styleNote(note_, TRANS("The language changes the next time Masterpiece starts. Names of organs, stops and "
                         "pages stay as the organ gives them."));
}

void LanguagePanel::resized() {
  auto r = getLocalBounds().reduced(16);
  heading_.setBounds(r.removeFromTop(kRow));
  language_.setBounds(r.removeFromTop(kRow).removeFromLeft(260).reduced(0, 1));
  r.removeFromTop(kGap);
  note_.setBounds(r.removeFromTop(48));
}

// --------------------------------------------------------------- window

SettingsWindow::~SettingsWindow() {
  // Posted, not called: the window is still being torn down here.
  if (onClosed) juce::MessageManager::callAsync(std::move(onClosed));
}

SettingsWindow::SettingsWindow(MasterpieceProcessor& p,
                               juce::AudioDeviceManager* devices)
    : reverb_(p), metronome_(p), mixer_(p), voicing_(p), favourites_(p), display_(p), language_(p), log_(p) {
  const auto bg = juce::Colour(0xff1b1e24);
  addAndMakeVisible(tabs_);
  // Without a device manager -- a plugin, whose host owns the audio device
  // and the MIDI ports -- the Audio and MIDI tabs are left out.
  if (devices != nullptr) {
    audio_ = std::make_unique<AudioSettingsPanel>(*devices, p);
    tabs_.addTab(TRANS("Audio"), bg, audio_.get(), false);
  }
  tabs_.addTab(TRANS("Room"), bg, &reverb_, false);
  tabs_.addTab(TRANS("Metronome"), bg, &metronome_, false);
  if (devices != nullptr) {
    midi_ = std::make_unique<MidiPanel>(p, *devices);
    tabs_.addTab(TRANS("MIDI"), bg, midi_.get(), false);
  }
  tabs_.addTab(TRANS("Mixer"), bg, &mixer_, false);
  tabs_.addTab(TRANS("Voicing"), bg, &voicing_, false);
  tabs_.addTab(TRANS("Combination sets"), bg, &favourites_, false);
  tabs_.addTab(TRANS("Display"), bg, &display_, false);
  tabs_.addTab(TRANS("Language"), bg, &language_, false);
  // Only where the program runs the server: the standalone program sets
  // remoteStatus; a plugin does not.
  if (p.remoteStatus) {
    remote_ = std::make_unique<RemotePanel>(p);
    tabs_.addTab(TRANS("Remote"), bg, remote_.get(), false);
  }
  tabs_.addTab(TRANS("Log"), bg, &log_, false);
  // The Engine tab is the tallest: six switches, five memory controls, a
  // readout and a footer. Adding a row without adding height pushes the
  // Save buttons off the bottom, where they cannot be pressed at all.
  setSize(660, 560);
}

void SettingsWindow::showTab(const juce::String& name) {
  const auto names = tabs_.getTabNames();
  if (const int i = names.indexOf(name); i >= 0) tabs_.setCurrentTabIndex(i);
}

void SettingsWindow::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff15171c)); }
void SettingsWindow::resized() { tabs_.setBounds(getLocalBounds()); }

} // namespace mp::ui
