#include "Settings.h"

#include <map>

#include "ManualDialog.h"

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
  openMono_ = proc_.loadMono();
  openRate_ = proc_.loadSampleRate();
  openCache_ = proc_.cacheMode();
  openLoadTicks_ = proc_.loadTicks();

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
  styleLabel(profileLabel_, "Memory profile");
  addAndMakeVisible(profile_);
  // Named for what they do, not for a machine they suit. The figures that
  // actually decide whether one fits are in the readout below the controls,
  // measured on the organ in front of you rather than assumed.
  profile_.addItem("Best quality - 24-bit, hold everything", 1);
  profile_.addItem("Recommended - 16-bit, stream releases", 2);
  profile_.addItem("Smallest - 16-bit mono, stream releases", 3);
  profile_.addItem("Custom", 4);
  profile_.onChange = [this] {
    if (applyingProfile_) return;
    if (profile_.getSelectedId() != 4) applyProfile(profile_.getSelectedId());
  };

  addAndMakeVisible(preloadLabel_);
  styleLabel(preloadLabel_, "Preloaded per sample");
  addAndMakeVisible(preload_);
  // The head is a minimum: it is always extended to cover the sustain loop,
  // so these are "how much MORE than the loop", not a hard cap.
  preload_.addItem("Whole samples (most memory)", 1);
  preload_.addItem("Loop + 2 s", 2);
  preload_.addItem("Loop + 1 s", 3);
  preload_.addItem("Loop only (least memory)", 4);
  preload_.setSelectedId(1, juce::dontSendNotification);
  preload_.onChange = [this] {
    const int64_t rate = 48000;
    switch (preload_.getSelectedId()) {
      case 2: proc_.setPreloadHeadFrames(2 * rate); break;
      case 3: proc_.setPreloadHeadFrames(rate); break;
      case 4: proc_.setPreloadHeadFrames(1); break;
      default: proc_.setPreloadHeadFrames(0); break;
    }
    syncProfile();
  };

  addAndMakeVisible(storageLabel_);
  styleLabel(storageLabel_, "Resident format");
  addAndMakeVisible(storage_);
  storage_.addItem("24-bit - what the samples are", 1);
  storage_.addItem("16-bit - two thirds of that, inaudible", 2);
  storage_.addItem("32-bit float - larger than the source", 3);
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
  styleLabel(rateLabel_, "Sample rate");
  addAndMakeVisible(rate_);
  rate_.addItem("As recorded", 1);
  rate_.addItem("48 kHz", 2);
  rate_.addItem("44.1 kHz", 3);
  const auto hz = proc_.loadSampleRate();
  rate_.setSelectedId(hz >= 47000.0 && hz <= 49000.0   ? 2
                      : hz >= 43000.0 && hz <= 45000.0 ? 3
                                                       : 1,
                      juce::dontSendNotification);
  rate_.onChange = [this] {
    const int id = rate_.getSelectedId();
    proc_.setLoadSampleRate(id == 2 ? 48000.0 : id == 3 ? 44100.0 : 0.0);
  };

  // Keeping the decoded samples, so the next load of the same organ is a read
  // rather than twelve thousand decodes. One file by default: these run to
  // gigabytes, and a cache per organ per settings combination would fill a
  // disk without ever being asked to. Loading something else replaces it.
  addAndMakeVisible(cacheLabel_);
  styleLabel(cacheLabel_, "Sample cache");
  addAndMakeVisible(cache_);
  cache_.addItem("One cache, replaced as organs change", 1);
  cache_.addItem("One cache per organ (uses more disk)", 2);
  cache_.addItem("Off", 3);
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

  // Where that cache is written. It is as large as the organs played through
  // it, so a machine with a small fast disk and a large slow one needs to be
  // able to say which holds it.
  addAndMakeVisible(cacheDirLabel_);
  styleLabel(cacheDirLabel_, "Cache folder");
  addAndMakeVisible(cacheDirValue_);
  cacheDirValue_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  showCacheDir();
  addAndMakeVisible(cacheDirChoose_);
  cacheDirChoose_.onClick = [this] {
    cacheDirChooser_ = std::make_unique<juce::FileChooser>(
        "Where should the sample cache be written?", proc_.cacheDirectory());
    cacheDirChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc) {
          const auto dir = fc.getResult();
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

  // Where this organ's OrganInstallationPackages is. Normally worked out
  // from the definition's path; a set whose folders are linked in from
  // another tree can leave that path pointing nowhere near the audio, and
  // then only the player knows. Takes effect the next time it loads.
  addAndMakeVisible(organRootLabel_);
  styleLabel(organRootLabel_, "Organ folder");
  addAndMakeVisible(organRootValue_);
  organRootValue_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  showOrganRoot();
  addAndMakeVisible(organRootChoose_);
  organRootChoose_.onClick = [this] {
    organRootChooser_ = std::make_unique<juce::FileChooser>(
        "Which folder holds OrganInstallationPackages?",
        juce::File(proc_.organRootDir()));
    organRootChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc) {
          const auto dir = fc.getResult();
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
    syncProfile();
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
      "Play a short tap at each 10% of an organ load, so a large set can be "
      "followed by ear.");
  loadTicks_.onClick = [this] { proc_.setLoadTicks(loadTicks_.getToggleState()); };

  syncProfile();

  // Nothing on this panel writes to disk on its own. Changes are live the
  // moment they are made, and what happens to them afterwards is the
  // player's to say: forget them, keep them for this session, give them to
  // this organ, or make them where every organ starts.
  for (auto* b : {&revert_, &keep_, &saveOrgan_, &saveGlobal_})
    addAndMakeVisible(*b);
  revert_.onClick = [this] { revert(); };
  keep_.onClick = [this] { closeDialog(); };
  saveOrgan_.onClick = [this] {
    proc_.saveSettings();
    closeDialog();
  };
  saveGlobal_.onClick = [this] {
    proc_.saveGlobalDefaults();
    closeDialog();
  };

  addAndMakeVisible(memory_);
  styleLabel(memory_, "");
  addAndMakeVisible(note_);
  styleNote(note_,
            "These settings take effect on the next organ load.\n\n"
            "The preload head is a minimum, never a cap: it is always extended "
            "to cover the sustain loop, because a sample whose loop is missing "
            "does not sustain - the note simply stops when the audio runs "
            "out.\n\n"
            "The resident format decides what a held frame costs. 16-bit halves "
            "the memory and is what most players load by default; each sample "
            "is scaled by its own peak first, so a quiet stop keeps the full "
            "sixteen bits instead of only the top few.\n\n"
            "Streaming holds only the first second of each release and fetches "
            "the rest from disk while it plays. Releases are the only samples "
            "in an organ worth streaming - several seconds each, played once, "
            "straight through, never looped - and an attack cannot be treated "
            "the same way because its sustain loop has to be resident. On the "
            "Nancy demo this is 12.2 GB down to 5.6 GB, with the rendered "
            "audio identical sample for sample. Needs a disk that keeps up; "
            "the Engine readout says if it did not.");
  startTimerHz(2);
}

EnginePanel::~EnginePanel() { stopTimer(); }

void EnginePanel::revert() {
  proc_.setEngineSwitch(openSwitch_);
  proc_.setPreloadHeadFrames(openPreload_);
  proc_.setSampleStorage(openStorage_);
  proc_.setStreamReleases(openStream_);
  proc_.setLoadMono(openMono_);
  mono_.setToggleState(openMono_, juce::dontSendNotification);
  proc_.setLoadTicks(openLoadTicks_);
  loadTicks_.setToggleState(openLoadTicks_, juce::dontSendNotification);
  proc_.setLoadSampleRate(openRate_);
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
  // The preload combo is a coarse choice over a frame count, so it is matched
  // back rather than stored twice.
  const int64_t rate = 48000;
  preload_.setSelectedId(openPreload_ == 0          ? 1
                         : openPreload_ == 2 * rate ? 2
                         : openPreload_ == rate     ? 3
                                                    : 4,
                         juce::dontSendNotification);

  // simpleWavOnly greys the rest out; restoring the states has to restore
  // that too, or the panel lies about what is reachable.
  const bool detailed = !openSwitch_.simpleWavOnly;
  for (auto* b : {&wind_, &tremulant_, &enclosure_, &voicing_})
    b->setEnabled(detailed);
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

void EnginePanel::timerCallback() {
  const auto bytes = proc_.sampleLibrary().residentBytes();
  const auto fmt = proc_.sampleStorage();
  juce::String text = "Resident samples: " +
                      juce::String(bytes / (1024 * 1024)) + " MB (" +
                      juce::String(proc_.sampleLibrary().residentCount()) +
                      " samples, " +
                      (fmt == SampleStorage::Int16     ? "16-bit"
                       : fmt == SampleStorage::Float32 ? "32-bit float"
                                                       : "24-bit") +
                      ", " + (proc_.loadMono() ? "mono" : "stereo") +
                      (proc_.loadSampleRate() > 0.0
                           ? ", " + juce::String(proc_.loadSampleRate() / 1000.0, 1) +
                                 " kHz"
                           : juce::String()) +
                      ")";
  const auto streamed = proc_.sampleLibrary().streamedCount();
  if (streamed > 0)
    text += "  -  " + juce::String(static_cast<int>(streamed)) +
            " streamed, " +
            juce::String(proc_.sampleLibrary().streamedBytesSaved() /
                         (1024 * 1024)) +
            " MB left on disk";
  if (const auto under = proc_.streamUnderruns(); under > 0)
    text += "  -  DISK TOO SLOW: " + juce::String(static_cast<int>(under)) +
            " underrun(s)";
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
                             ? "Default (" + proc_.cacheDirectory().getFullPathName() + ")"
                             : chosen.getFullPathName(),
                         juce::dontSendNotification);
  cacheDirValue_.setTooltip(proc_.cacheDirectory().getFullPathName());
}

// The folder as the player sees it: the one they named, or the one derived
// from the definition's path, marked as derived so the two cannot be
// confused.
void EnginePanel::showOrganRoot() {
  const auto chosen = proc_.organRootOverride();
  organRootValue_.setText(chosen.getFullPathName().isEmpty()
                              ? "From the definition (" + juce::String(proc_.organRootDir()) + ")"
                              : chosen.getFullPathName() + "  -- loads next time",
                          juce::dontSendNotification);
}

void EnginePanel::resized() {
  auto r = getLocalBounds().reduced(12);
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
  auto cacheRow = r.removeFromTop(kRow);
  cacheLabel_.setBounds(cacheRow.removeFromLeft(180));
  cache_.setBounds(cacheRow.removeFromLeft(300));
  r.removeFromTop(6);
  auto cacheDirRow = r.removeFromTop(kRow);
  cacheDirLabel_.setBounds(cacheDirRow.removeFromLeft(180));
  cacheDirChoose_.setBounds(cacheDirRow.removeFromRight(90).reduced(0, 2));
  cacheDirRow.removeFromRight(6);
  cacheDirDefault_.setBounds(cacheDirRow.removeFromRight(80).reduced(0, 2));
  cacheDirRow.removeFromRight(8);
  cacheDirValue_.setBounds(cacheDirRow);
  r.removeFromTop(6);
  auto organRootRow = r.removeFromTop(kRow);
  organRootLabel_.setBounds(organRootRow.removeFromLeft(180));
  organRootChoose_.setBounds(organRootRow.removeFromRight(90).reduced(0, 2));
  organRootRow.removeFromRight(6);
  organRootDefault_.setBounds(organRootRow.removeFromRight(80).reduced(0, 2));
  organRootRow.removeFromRight(8);
  organRootValue_.setBounds(organRootRow);
  r.removeFromTop(6);
  stream_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(2);
  mono_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(2);
  loadTicks_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);
  memory_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(kGap);

  // Footer, taken from the bottom before the note gets what is left. Read
  // left to right it goes from discarding to committing hardest, so the
  // consequence grows with the distance from "put it back".
  auto footer = r.removeFromBottom(kRow + 4);
  revert_.setBounds(footer.removeFromLeft(130).reduced(0, 2));
  footer.removeFromLeft(kGap);
  keep_.setBounds(footer.removeFromLeft(120).reduced(0, 2));
  saveGlobal_.setBounds(footer.removeFromRight(150).reduced(0, 2));
  footer.removeFromRight(kGap);
  saveOrgan_.setBounds(footer.removeFromRight(160).reduced(0, 2));
  r.removeFromBottom(kGap);

  note_.setBounds(r);
}

// ---------------------------------------------------------------- reverb

ReverbPanel::ReverbPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(enabled_);
  enabled_.setToggleState(proc_.convolver().enabled(), juce::dontSendNotification);
  enabled_.onClick = [this] {
    proc_.convolver().setEnabled(enabled_.getToggleState());
  };

  addAndMakeVisible(load_);
  load_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Choose an impulse response", juce::File(), "*.wav;*.aiff;*.aif;*.flac");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                            const auto f = fc.getResult();
                            if (!f.existsAsFile()) return;
                            if (proc_.convolver().loadImpulseResponse(f)) {
                              irName_.setText("IR: " + f.getFileName(),
                                              juce::dontSendNotification);
                              enabled_.setToggleState(true, juce::sendNotification);
                            }
                          });
  };

  addAndMakeVisible(clear_);
  clear_.onClick = [this] {
    proc_.convolver().clear();
    irName_.setText("No IR loaded", juce::dontSendNotification);
  };

  addAndMakeVisible(irName_);
  styleLabel(irName_, proc_.convolver().hasImpulseResponse()
                          ? "IR: " + proc_.convolver().impulseResponseName()
                          : "No IR loaded");

  addAndMakeVisible(mixLabel_);
  styleLabel(mixLabel_, "Wet");
  addAndMakeVisible(mix_);
  mix_.setRange(0.0, 1.0, 0.01);
  mix_.setValue(proc_.convolver().mix(), juce::dontSendNotification);
  mix_.onValueChange = [this] {
    proc_.convolver().setMix(static_cast<float>(mix_.getValue()));
  };

  addAndMakeVisible(note_);
  styleNote(note_,
            "Most sample sets are recorded in the room they live in, and "
            "several ship close / far / rear perspectives which ARE the room. "
            "Convolution is for a dry set, or for headphones, and usually "
            "wants far less wet signal than a reverb plugin would suggest.");
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
  enabled_.onClick = [this] {
    proc_.metronome().setEnabled(enabled_.getToggleState());
  };

  addAndMakeVisible(tempoLabel_);
  styleLabel(tempoLabel_, "Tempo");
  addAndMakeVisible(tempo_);
  tempo_.setRange(20.0, 300.0, 1.0);
  tempo_.setValue(proc_.metronome().tempo(), juce::dontSendNotification);
  tempo_.setTextValueSuffix(" bpm");
  tempo_.onValueChange = [this] { proc_.metronome().setTempo(tempo_.getValue()); };

  addAndMakeVisible(beatsLabel_);
  styleLabel(beatsLabel_, "Beats per bar");
  addAndMakeVisible(beats_);
  beats_.setRange(0.0, 12.0, 1.0);
  beats_.setValue(proc_.metronome().beatsPerBar(), juce::dontSendNotification);
  beats_.onValueChange = [this] {
    proc_.metronome().setBeatsPerBar(static_cast<int>(beats_.getValue()));
  };

  addAndMakeVisible(levelLabel_);
  styleLabel(levelLabel_, "Level");
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

MetronomePanel::~MetronomePanel() { stopTimer(); }

void MetronomePanel::timerCallback() {
  const int b = proc_.metronome().currentBeat();
  beat_.setText(b > 0 ? "Beat " + juce::String(b) : "", juce::dontSendNotification);
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
        "Save the performance", juce::File(), "*.mid");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode |
                              juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                            const auto f = fc.getResult();
                            if (f.getFullPathName().isNotEmpty())
                              proc_.recorder().saveToFile(
                                  f.withFileExtension(".mid"));
                          });
  };
  load_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>(
        "Load a performance", juce::File(), "*.mid;*.midi");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                            const auto f = fc.getResult();
                            if (f.existsAsFile()) proc_.recorder().loadFromFile(f);
                          });
  };

  addAndMakeVisible(status_);
  styleLabel(status_, "");
  addAndMakeVisible(note_);
  styleNote(note_,
            "Records everything that reaches the engine, not just notes: stop "
            "changes, couplers and shoe movements that arrive as MIDI are "
            "captured too, so a recording plays back as the same performance "
            "rather than as notes on whatever registration happens to be "
            "drawn at the time.");

  addAndMakeVisible(audioHeading_);
  styleLabel(audioHeading_, "Audio");
  for (auto* b : {&audioRecord_, &audioStop_}) addAndMakeVisible(*b);
  addAndMakeVisible(audioStatus_);
  styleLabel(audioStatus_, "");
  addAndMakeVisible(audioNote_);
  styleNote(audioNote_,
            "Captures the organ's output after the master fader and before "
            "the metronome, so the click stays out of the file. Written on a "
            "background thread at 24-bit; the audio callback only hands over "
            "the block it has already finished.");

  audioRecord_.onClick = [this] {
    audioChooser_ = std::make_unique<juce::FileChooser>(
        "Record the organ to", juce::File(), "*.wav");
    audioChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& fc) {
          const auto f = fc.getResult();
          if (f.getFullPathName().isEmpty()) return;
          // The device's rate, not the organ's: this is what is leaving.
          if (!proc_.audioRecorder().start(f.withFileExtension(".wav"),
                                           proc_.getSampleRate(),
                                           proc_.getTotalNumOutputChannels()))
            audioStatus_.setText("Could not open that file for writing",
                                 juce::dontSendNotification);
        });
  };
  audioStop_.onClick = [this] { proc_.audioRecorder().stop(); };

  startTimerHz(8);
}

RecorderPanel::~RecorderPanel() { stopTimer(); }

void RecorderPanel::timerCallback() {
  const auto& r = proc_.recorder();
  juce::String s;
  switch (r.state()) {
    case MidiRecorder::State::Recording:
      s = "Recording - " + juce::String(r.positionSeconds(), 1) + " s, " +
          juce::String(r.eventCount()) + " events";
      break;
    case MidiRecorder::State::Playing:
      s = "Playing - " + juce::String(r.positionSeconds(), 1) + " / " +
          juce::String(r.lengthSeconds(), 1) + " s";
      break;
    case MidiRecorder::State::Idle:
      s = r.empty() ? "Nothing recorded"
                    : juce::String(r.eventCount()) + " events, " +
                          juce::String(r.lengthSeconds(), 1) + " s";
      break;
  }
  status_.setText(s, juce::dontSendNotification);
  record_.setToggleState(r.isRecording(), juce::dontSendNotification);
  play_.setEnabled(!r.empty());
  save_.setEnabled(!r.empty());

  auto& a = proc_.audioRecorder();
  const bool on = a.isRecording();
  audioStatus_.setText(
      on ? "Recording " + a.file().getFileName() + " - " +
               juce::String(a.secondsRecorded(), 1) + " s"
         : "Not recording",
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
  styleLabel(inputsLabel_, "MIDI inputs");
  addAndMakeVisible(outputsLabel_);
  styleLabel(outputsLabel_, "MIDI output");
  addAndMakeVisible(sharedNote_);
  sharedNote_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  addAndMakeVisible(keyboardsLabel_);
  styleLabel(keyboardsLabel_, "Keyboards");

  addAndMakeVisible(output_);
  output_.onChange = [this] {
    // Opening the port here rather than in the engine: the device belongs to
    // the application, and the engine only ever borrows a pointer to it.
    proc_.setMidiOutput(nullptr);
    openedOutput_.reset();
    const int idx = output_.getSelectedId() - 2; // id 1 is "None"
    const auto devs = juce::MidiOutput::getAvailableDevices();
    if (idx >= 0 && idx < devs.size()) {
      openedOutput_ = juce::MidiOutput::openDevice(devs[idx].identifier);
      proc_.setMidiOutput(openedOutput_.get());
    }
  };

  addAndMakeVisible(feedback_);
  feedback_.setToggleState(proc_.midiFeedbackEnabled(), juce::dontSendNotification);
  feedback_.onClick = [this] {
    proc_.setMidiFeedbackEnabled(feedback_.getToggleState());
  };

  addAndMakeVisible(stepperLabel_);
  styleLabel(stepperLabel_, "Sequencer");
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

  addAndMakeVisible(saveMap_);
  saveMap_.onClick = [this] { proc_.saveMidiMap(); };
  addAndMakeVisible(clearMap_);
  clearMap_.onClick = [this] { proc_.midiMap().clear(); };

  addAndMakeVisible(mapStatus_);
  styleLabel(mapStatus_, "");
  addAndMakeVisible(note_);
  styleNote(note_,
            "To map a control: right-click a drawstop on the console, then "
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
            "a Learn button here, then the piston you want to use.");

  addAndMakeVisible(consoleHeading_);
  styleLabel(consoleHeading_, "Console");
  struct { juce::TextButton* b; MidiTargetKind k; } consoleLearn[] = {
      {&learnPageNext_, MidiTargetKind::ConsoleNextPage},
      {&learnPagePrev_, MidiTargetKind::ConsolePrevPage},
      {&learnLayout_, MidiTargetKind::ConsoleNextLayout},
      {&learnStopList_, MidiTargetKind::ConsoleToggleStopList},
      {&learnKeyboard_, MidiTargetKind::ConsoleToggleKeyboard},
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

MidiPanel::~MidiPanel() {
  stopTimer();
  proc_.setMidiOutput(nullptr);
}

// Manuals that answer the same channel, named. Sharing one is a way of
// playing two divisions from one keyboard; doing it unawares is how a manual
// ends up sounding the wrong division, which is what this line prevents.
void MidiPanel::showSharedChannels() {
  std::map<int, juce::StringArray> byChannel;
  for (const auto& b : proc_.channelAssignments()) {
    if (b.channel <= 0) continue;
    const auto it = proc_.organModel().keyboards.find(b.keyboardId);
    byChannel[b.channel].add(it != proc_.organModel().keyboards.end() &&
                                     !it->second.name.empty()
                                 ? juce::String(it->second.name)
                                 : "keyboard " + juce::String((int) b.keyboardId));
  }
  juce::String text;
  for (const auto& [channel, names] : byChannel) {
    if (names.size() < 2) continue;
    text << (text.isEmpty() ? "" : "   ") << "Channel " << channel << ": "
         << names.joinIntoString(", ");
  }
  sharedNote_.setText(text.isEmpty() ? "" : "Shared -- " + text,
                      juce::dontSendNotification);
}

void MidiPanel::refresh() {
  inputs_.clear();
  for (const auto& in : juce::MidiInput::getAvailableDevices()) {
    auto b = std::make_unique<juce::ToggleButton>(in.name);
    b->setToggleState(devices_.isMidiInputDeviceEnabled(in.identifier),
                      juce::dontSendNotification);
    const auto id = in.identifier;
    auto* raw = b.get();
    b->onClick = [this, id, raw] {
      devices_.setMidiInputDeviceEnabled(id, raw->getToggleState());
    };
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
    box->addItem("Default (channel " +
                     juce::String(proc_.channelForKeyboard(kb)) + ")", 1);
    for (int ch = 1; ch <= 16; ++ch)
      box->addItem("Channel " + juce::String(ch), ch + 1);
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
      if (raw->getSelectedId() > 1)
        proc_.setKeyboardForChannel(raw->getSelectedId() - 1, kb, 0, false);
      proc_.saveMidiMap();
      showSharedChannels();
    };
    addAndMakeVisible(*box);
    keyboardChannels_.push_back(std::move(box));

    // And which console. Named by the device itself, because a saved mapping
    // refers to it by name and an id means nothing across runs.
    auto dev = std::make_unique<juce::ComboBox>();
    dev->addItem("Any device", 1);
    const auto& known = proc_.midiDevices();
    for (size_t i = 0; i < known.names().size(); ++i)
      dev->addItem(juce::String(known.names()[i]), static_cast<int>(i) + 2);
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
      proc_.setKeyboardForChannel(channel, kb, rawDev->getSelectedId() - 1);
      proc_.saveMidiMap();
    };
    addAndMakeVisible(*dev);
    keyboardDevices_.push_back(std::move(dev));

    auto more = std::make_unique<juce::TextButton>("Range, transpose...");
    more->onClick = [this, kb] { ManualDialog::show(proc_, kb); };
    addAndMakeVisible(*more);
    keyboardMore_.push_back(std::move(more));
  }

  output_.clear(juce::dontSendNotification);
  output_.addItem("None", 1);
  int id = 2;
  for (const auto& out : juce::MidiOutput::getAvailableDevices())
    output_.addItem(out.name, id++);
  output_.setSelectedId(1, juce::dontSendNotification);
  resized();
}

void MidiPanel::timerCallback() {
  const auto& map = proc_.midiMap();
  juce::String s = juce::String(static_cast<int>(map.size())) + " binding(s)";
  if (map.learning()) s += "  -  LEARNING: move a control now";
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

  r.removeFromTop(kGap);
  auto row = r.removeFromTop(kRow);
  outputsLabel_.setBounds(row.removeFromLeft(120));
  output_.setBounds(row.removeFromLeft(300));
  r.removeFromTop(4);
  feedback_.setBounds(r.removeFromTop(kRow));
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
  for (auto* b : {&learnStopList_, &learnKeyboard_}) {
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
  addAndMakeVisible(heading_);
  styleLabel(heading_, "Favourite organs");
  addAndMakeVisible(addCurrent_);
  addCurrent_.onClick = [this] {
    const int slot = proc_.addCurrentOrganToFavourites();
    if (slot == 0)
      status_.setText("Nothing to add - load an organ first, or all 64 slots "
                      "are taken",
                      juce::dontSendNotification);
    else
      status_.setText("On slot " + juce::String(slot),
                      juce::dontSendNotification);
    refresh();
  };
  addAndMakeVisible(status_);
  styleLabel(status_, "");
  addAndMakeVisible(viewport_);
  viewport_.setViewedComponent(&rows_, false);
  viewport_.setScrollBarsShown(true, false);
  // --- combination sets --------------------------------------------------
  addAndMakeVisible(setsHeading_);
  styleLabel(setsHeading_, "Combination sets");
  addAndMakeVisible(setLabel_);
  styleLabel(setLabel_, "In use");
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
    setStatus_.setText("Now using " +
                           (want.empty() ? juce::String("the default set")
                                         : juce::String(want)),
                       juce::dontSendNotification);
  };

  addAndMakeVisible(setNew_);
  setNew_.onClick = [this] {
    setPrompt_ = std::make_unique<juce::AlertWindow>(
        "Save as new set", "A name for this registration set:",
        juce::MessageBoxIconType::NoIcon);
    setPrompt_->addTextEditor("name", "", "Name");
    setPrompt_->addButton("Save", 1);
    setPrompt_->addButton("Cancel", 0);
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
          setStatus_.setText("Now using " + name, juce::dontSendNotification);
        }));
  };

  addAndMakeVisible(setDelete_);
  setDelete_.onClick = [this] {
    const std::string live = proc_.combinationSetName();
    if (live.empty()) {
      setStatus_.setText("The default set cannot be deleted",
                         juce::dontSendNotification);
      return;
    }
    // Move off it first: deleting the file under the live set would leave the
    // organ pointing at something that is no longer there.
    proc_.switchCombinationSet("");
    proc_.deleteCombinationSet(live);
    proc_.saveSettings();
    refreshSets();
    setStatus_.setText("Deleted " + juce::String(live) + ", back on the default",
                       juce::dontSendNotification);
  };
  addAndMakeVisible(setStatus_);
  styleLabel(setStatus_, "");

  addAndMakeVisible(note_);
  styleNote(note_,
            "A slot number is something a thumb piston can be mapped to; a "
            "file path is not. That is what these are for: on a console you "
            "are standing at a keyboard with both hands busy, and finding a "
            "19 GB set in a file browser is not something that happens "
            "between two pieces.\n\n"
            "Gaps are kept. Removing slot 1 does not renumber slot 5, because "
            "the numbers are the thing you learned.\n\n"
            "Loading starts in the background - a large set takes a while, and "
            "the organ you are playing keeps sounding until the new one is "
            "ready.\n\n"
            "A combination set is a whole registration book: one for a "
            "recital, another for a service. Changing set saves the one you "
            "are leaving first, so nothing you captured is lost by switching.");
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
    setBox_.addItem(n.empty() ? "(default)" : juce::String(n),
                    static_cast<int>(i) + 1);
    if (n == proc_.combinationSetName()) selected = static_cast<int>(i) + 1;
  }
  setBox_.setSelectedId(selected, juce::dontSendNotification);
}

void FavouritesPanel::refresh() {
  refreshSets();
  slots_.clear();
  labels_.clear();
  loads_.clear();
  removes_.clear();
  rows_.removeAllChildren();

  const auto& bank = proc_.favourites().organs;
  slots_ = bank.used();
  for (int slot : slots_) {
    const auto& fav = bank.at(slot);
    auto label = std::make_unique<juce::Label>();
    styleLabel(*label, juce::String(slot) + ".  " + juce::String(fav.name));
    // The path as a tooltip: two sets can share a name, and then the only
    // thing that tells them apart is where they live.
    label->setTooltip(juce::String(fav.target));
    rows_.addAndMakeVisible(*label);
    labels_.push_back(std::move(label));

    auto load = std::make_unique<juce::TextButton>("Load");
    const juce::String path(fav.target);
    load->onClick = [this, path] {
      const juce::File f(path);
      if (!f.existsAsFile()) {
        // A moved or unplugged set. Saying so beats a silent no-op, and the
        // favourite is left alone: the drive may come back.
        status_.setText("Not found: " + f.getFullPathName(),
                        juce::dontSendNotification);
        return;
      }
      status_.setText("Loading " + f.getFileName() + "...",
                      juce::dontSendNotification);
      proc_.loadOrganAsync(f);
    };
    rows_.addAndMakeVisible(*load);
    loads_.push_back(std::move(load));

    auto rm = std::make_unique<juce::TextButton>("Remove");
    rm->onClick = [this, slot] {
      proc_.favourites().organs.clear(slot);
      proc_.saveGlobalDefaults();
      refresh();
    };
    rows_.addAndMakeVisible(*rm);
    removes_.push_back(std::move(rm));
  }
  if (slots_.empty())
    status_.setText("No favourites yet", juce::dontSendNotification);
  resized();
}

void FavouritesPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  heading_.setBounds(r.removeFromTop(kRow));
  auto row = r.removeFromTop(kRow);
  addCurrent_.setBounds(row.removeFromLeft(220).reduced(0, 1));
  row.removeFromLeft(kGap);
  status_.setBounds(row);
  r.removeFromTop(kGap);

  auto noteArea = r.removeFromBottom(juce::jmin(110, r.getHeight() / 3));
  note_.setBounds(noteArea);
  r.removeFromBottom(kGap);

  // Sets sit under the list, where a player looks after choosing an organ.
  // Heading on its own line: the heading, a label, a box and two buttons on
  // one row overflowed the dialog and squeezed the last button's label.
  auto setsArea = r.removeFromBottom(kRow * 3 + 12);
  setsHeading_.setBounds(setsArea.removeFromTop(kRow));
  setsArea.removeFromTop(4);
  auto setRow = setsArea.removeFromTop(kRow);
  setLabel_.setBounds(setRow.removeFromLeft(60));
  setBox_.setBounds(setRow.removeFromLeft(200).reduced(0, 1));
  setRow.removeFromLeft(kGap);
  setNew_.setBounds(setRow.removeFromLeft(160).reduced(0, 1));
  setRow.removeFromLeft(6);
  setDelete_.setBounds(setRow.removeFromLeft(120).reduced(0, 1));
  setsArea.removeFromTop(4);
  setStatus_.setBounds(setsArea.removeFromTop(kRow));
  r.removeFromBottom(kGap);

  viewport_.setBounds(r);

  const int rowH = kRow + 2;
  rows_.setSize(juce::jmax(0, viewport_.getWidth() - 12),
                static_cast<int>(slots_.size()) * rowH);
  for (size_t i = 0; i < slots_.size(); ++i) {
    juce::Rectangle<int> line(0, static_cast<int>(i) * rowH, rows_.getWidth(),
                              kRow);
    removes_[i]->setBounds(line.removeFromRight(90).reduced(2, 1));
    loads_[i]->setBounds(line.removeFromRight(80).reduced(2, 1));
    labels_[i]->setBounds(line);
  }
}

// --------------------------------------------------------------- voicing

VoicingPanel::VoicingPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, "Voicing");

  addAndMakeVisible(rankLabel_);
  styleLabel(rankLabel_, "Rank");
  addAndMakeVisible(rank_);
  rank_.onChange = [this] { loadCurrentIntoSliders(); };

  addAndMakeVisible(scopeLabel_);
  styleLabel(scopeLabel_, "Applies to");
  addAndMakeVisible(scope_);
  scope_.addItem("The whole rank", 1);
  scope_.addItem("One pipe", 2);
  scope_.setSelectedId(1, juce::dontSendNotification);
  scope_.onChange = [this] {
    const bool perPipe = scope_.getSelectedId() == 2;
    note_.setEnabled(perPipe);
    noteLabel_.setEnabled(perPipe);
    loadCurrentIntoSliders();
  };

  addAndMakeVisible(noteLabel_);
  styleLabel(noteLabel_, "MIDI note");
  addAndMakeVisible(note_);
  note_.setRange(0, 127, 1);
  note_.setValue(60, juce::dontSendNotification);
  note_.setEnabled(false);
  note_.onValueChange = [this] { loadCurrentIntoSliders(); };

  addAndMakeVisible(gainLabel_);
  styleLabel(gainLabel_, "Level");
  addAndMakeVisible(gain_);
  // +/-12 dB is the range a voicer works in. Wider would mostly offer new
  // ways to make an organ wrong.
  gain_.setRange(-12.0, 12.0, 0.1);
  gain_.setTextValueSuffix(" dB");
  gain_.setValue(0.0, juce::dontSendNotification);
  gain_.onValueChange = [this] { pushCurrent(); };

  addAndMakeVisible(tuneLabel_);
  styleLabel(tuneLabel_, "Tuning");
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
            "A rank adjustment and a pipe adjustment ADD. Pulling one sour "
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
            "instrument.");
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
      status_.setText("This rank has no pipe at that note",
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
  s << "Slot " << (proc_.voicing().usingB ? "B" : "A") << "  -  "
    << static_cast<int>(v.ranks().size()) << " rank(s), "
    << static_cast<int>(v.pipes().size()) << " pipe(s) adjusted";
  // Voicing is heard on the NEXT note, like a real pipe being touched while
  // another is sounding. Saying so stops it looking broken.
  s << "  -  takes effect on the next note";
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
  styleLabel(heading_, "Mixer");
  addAndMakeVisible(busesLabel_);
  styleLabel(busesLabel_, "Output pairs");
  addAndMakeVisible(busCount_);
  for (int i = 1; i <= 8; ++i)
    busCount_.addItem(juce::String(i) + (i == 1 ? " (stereo)" : " pairs"), i);
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
            "No sample set says anything about audio routing - not one - so "
            "this is yours to decide, like the MIDI mapping. Output pairs and "
            "their device channels carry across organs; which rank goes where "
            "is saved per organ, because a rank number means nothing in a "
            "different instrument.\n\n"
            "A rank you never touch plays through the first pair, so an organ "
            "is audible before you open this page. If you are listening in "
            "stereo the pairs are summed, so nothing disappears when you "
            "split them up.");

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
    for (int i = 1; i <= buses; ++i) box->addItem("Pair " + juce::String(i), i);
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
  status_.setText(juce::String(static_cast<int>(rankIds_.size())) +
                      " rank(s), " + juce::String(proc_.mixBusCount()) +
                      " pair(s)" +
                      (d.clean() ? "" : "  -  " +
                                            juce::String((int)d.unroutedRanks.size()) +
                                            " unrouted, " +
                                            juce::String((int)d.ranksRoutedToMissingBus.size()) +
                                            " stale"),
                  juce::dontSendNotification);
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
    for (int i = 1; i <= buses; ++i) box->addItem("Pair " + juce::String(i), i);
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

DisplayPanel::DisplayPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(heading_);
  styleLabel(heading_, "Console display");
  addAndMakeVisible(enable_);
  enable_.onClick = [this] { rebuild(); };

  addAndMakeVisible(idLabel_);
  styleLabel(idLabel_, "Display number");
  addAndMakeVisible(id_);
  id_.setRange(0, 15, 1);
  id_.setValue(0, juce::dontSendNotification);
  id_.onValueChange = [this] { rebuild(); };

  addAndMakeVisible(widthLabel_);
  styleLabel(widthLabel_, "Characters per line");
  addAndMakeVisible(width_);
  int wid = 1;
  for (int w : {16, 20, 32, 40}) width_.addItem(juce::String(w), wid++);
  width_.setSelectedId(3, juce::dontSendNotification); // 32, the usual one
  width_.onChange = [this] { rebuild(); };

  addAndMakeVisible(headerLabel_);
  styleLabel(headerLabel_, "Sys-ex prefix (hex)");
  addAndMakeVisible(header_);
  header_.setText("7D", juce::dontSendNotification);
  header_.setInputRestrictions(23, "0123456789abcdefABCDEF ");
  header_.onReturnKey = [this] { rebuild(); };
  header_.onFocusLost = [this] { rebuild(); };

  addAndMakeVisible(linesLabel_);
  styleLabel(linesLabel_, "Lines");
  // The fields a jamb display is worth having for. Deliberately short: a
  // player reads this at a glance between pieces, not as a report.
  const char* fields[] = {"(blank)",   "Organ",       "Temperament",
                          "Pitch",     "Transpose",   "Stops drawn",
                          "Crescendo", "Combination set"};
  for (size_t i = 0; i < lines_.size(); ++i) {
    lines_[i] = std::make_unique<juce::ComboBox>();
    addAndMakeVisible(*lines_[i]);
    int id = 1;
    for (const char* f : fields) lines_[i]->addItem(f, id++);
    lines_[i]->onChange = [this] { rebuild(); };
  }
  // Something useful out of the box, so the first send shows the player it is
  // working rather than four blank lines.
  lines_[0]->setSelectedId(2, juce::dontSendNotification); // Organ
  lines_[1]->setSelectedId(3, juce::dontSendNotification); // Temperament
  lines_[2]->setSelectedId(4, juce::dontSendNotification); // Pitch
  lines_[3]->setSelectedId(7, juce::dontSendNotification); // Crescendo

  addAndMakeVisible(previewLabel_);
  styleLabel(previewLabel_, "What the display reads");
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
            "A console display is driven by system exclusive messages, and the "
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
            "high byte would end the message rather than draw a character.");

  rebuild();
  startTimerHz(2);
}

DisplayPanel::~DisplayPanel() { stopTimer(); }

// The panel description belongs to the engine, which owns the state a line
// shows; this only assembles it from the boxes.
void DisplayPanel::rebuild() {
  auto& panels = proc_.lcdPanels();
  panels.clear();
  if (!enable_.getToggleState()) return;

  // The prefix, as hex bytes. Anything unparseable falls back to the reserved
  // non-commercial id rather than to silence, so a typo leaves something on
  // the wire to see instead of looking like a dead feature.
  std::vector<uint8_t> header;
  for (const auto& tok :
       juce::StringArray::fromTokens(header_.getText(), " ", "")) {
    if (tok.isEmpty()) continue;
    header.push_back(static_cast<uint8_t>(tok.getHexValue32() & 0xFF));
  }
  if (!panels.setHeader(header)) panels.setHeader({0x7D});

  LcdPanel p;
  p.hardwareId = static_cast<int>(id_.getValue());
  const int widths[] = {16, 20, 32, 40};
  p.lineWidth = widths[juce::jlimit(0, 3, width_.getSelectedId() - 1)];
  const LcdField byId[] = {LcdField::Literal,       LcdField::OrganName,
                           LcdField::Temperament,   LcdField::PitchHz,
                           LcdField::Transpose,     LcdField::StopsDrawn,
                           LcdField::CrescendoStep, LcdField::CombinationSet};
  for (auto& box : lines_) {
    const int sel = juce::jlimit(1, 8, box->getSelectedId());
    p.lines.push_back({byId[sel - 1], ""});
  }
  panels.addPanel(std::move(p));
  proc_.refreshLcdPanels();
}

void DisplayPanel::timerCallback() {
  // The display's own refresh. Twice a second is far faster than anyone reads
  // a jamb panel and far slower than the wire, and nothing goes out unless a
  // line's text actually changed.
  proc_.pumpLcdPanels();

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

// --------------------------------------------------------------- window

SettingsWindow::SettingsWindow(MasterpieceProcessor& p,
                               juce::AudioDeviceManager& devices)
    : engine_(p), reverb_(p), metronome_(p), recorder_(p), midi_(p, devices), mixer_(p), voicing_(p), favourites_(p), display_(p) {
  const auto bg = juce::Colour(0xff1b1e24);
  addAndMakeVisible(tabs_);
  tabs_.addTab("Engine", bg, &engineScroll_, false);
  tabs_.addTab("Room", bg, &reverb_, false);
  tabs_.addTab("Metronome", bg, &metronome_, false);
  tabs_.addTab("Recorder", bg, &recorder_, false);
  tabs_.addTab("MIDI", bg, &midi_, false);
  tabs_.addTab("Mixer", bg, &mixer_, false);
  tabs_.addTab("Voicing", bg, &voicing_, false);
  tabs_.addTab("Favourites", bg, &favourites_, false);
  tabs_.addTab("Display", bg, &display_, false);
  // The Engine tab is the tallest: six switches, five memory controls, a
  // readout and a footer. Adding a row without adding height pushes the
  // Save buttons off the bottom, where they cannot be pressed at all.
  setSize(660, 560);
}

void SettingsWindow::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff15171c)); }
void SettingsWindow::resized() { tabs_.setBounds(getLocalBounds()); }

} // namespace mp::ui
