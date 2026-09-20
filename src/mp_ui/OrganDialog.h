#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "mp_audio/SampleLibrary.h"
#include <functional>
#include <memory>
#include <vector>
#include <atomic>

namespace mp {
class MasterpieceProcessor;
}

namespace mp::ui {

class MasterpieceEditor;

struct OrganPackageInfo {
  uint32_t packageId = 0;
  juce::String name;
  juce::String supplierName;
  juce::File directory;
  bool isInstalled = false;
  juce::int64 diskSizeBytes = 0;
};

struct OrganEntry {
  juce::File file;
  juce::String name;
  juce::String uniqueOrganId;
  juce::File organRootDir;
  bool isCurrent = false;
  bool exists = false;
  bool isInstalled = false;
  juce::int64 odfSizeBytes = 0;
  juce::int64 diskSpaceBytes = 0;
  std::vector<OrganPackageInfo> packages;
};

struct OrganAudioConfig {
  SampleStorage storage = SampleStorage::Int24;
  bool mono = false;
  double sampleRate = 0.0;
  SampleLibrary::CacheMode cacheMode = SampleLibrary::CacheMode::Single;
  bool streamReleases = false;
  juce::int64 streamHeadFrames = 44100;
  juce::int64 preloadHeadFrames = 0;
  juce::File organRootOverride;
  EngineSwitch engineSwitch;
};

struct OrganAudioStat {
  juce::int64 totalAudioFrames = 0;
  juce::int64 attackFrames = 0;
  juce::int64 releaseFrames = 0;
  juce::int64 attackLoopFrames = 0;
  int attackCount = 0;
  int releaseCount = 0;
  juce::int64 rawPcmBytes = 0;
  bool hasStats = false;
};

// Utilities
juce::File findUnrarBinary();
juce::Array<juce::File> filterArchivesForExtraction(const juce::Array<juce::File>& files);
juce::String readOrganNameFromOdf(const juce::File& file);
std::vector<OrganEntry> discoverOrgans(const MasterpieceProcessor& proc);
OrganEntry getOrganDetails(const juce::File& odfFile, const MasterpieceProcessor& proc);
juce::String formatByteSize(juce::int64 bytes);
juce::int64 computeDirectorySize(const juce::File& dir);
juce::File getOrganAudioStatCacheFile(const OrganEntry& entry);
bool hasCachedOrganAudioStat(const OrganEntry& entry);
OrganAudioStat computeOrganAudioStat(
    const OrganEntry& entry,
    const MasterpieceProcessor& proc,
    std::function<void(double progress, int current, int total)> progressCallback = nullptr,
    std::atomic<bool>* cancelFlag = nullptr);
juce::int64 estimateRamFootprintBytes(const OrganAudioStat& stat, const OrganAudioConfig& cfg);
OrganAudioConfig loadOrganAudioConfig(const MasterpieceProcessor& proc, const juce::File& odf);
bool saveOrganAudioConfig(MasterpieceProcessor& proc, const juce::File& odf, const OrganAudioConfig& cfg);
void triggerBackgroundAudioStatPrecomputation(const juce::File& odfFile);
void triggerDirectoryAudioStatPrecomputation(const juce::File& dir);
juce::String humaniseEta(double secs);

class OrganDetailsDialog : public juce::Component,
                           public juce::ListBoxModel {
public:
  OrganDetailsDialog(const OrganEntry& entry, MasterpieceProcessor& proc,
                     std::function<void()> onOpenAudioSettings);
  ~OrganDetailsDialog() override = default;

  void resized() override;
  void paint(juce::Graphics& g) override;

  int getNumRows() override;
  void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                        bool rowIsSelected) override;

private:
  OrganEntry entry_;
  MasterpieceProcessor& proc_;
  std::function<void()> onOpenAudioSettings_;

  juce::Label titleLabel_;
  juce::Label pathLabel_;
  juce::Label rootLabel_;
  juce::Label sizeLabel_;
  juce::Label settingsLabel_;
  juce::Label packagesHeaderLabel_;

  juce::ListBox packageList_;

  juce::TextButton adjustAudioBtn_{"Adjust Audio Settings..."};
  juce::TextButton revealBtn_{"Reveal in Finder"};
  juce::TextButton closeBtn_{"Close"};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OrganDetailsDialog)
};

// Visual graph showing system RAM distribution and organ footprint
class RamGraphMeterComponent : public juce::Component {
public:
  RamGraphMeterComponent();
  ~RamGraphMeterComponent() override = default;

  void setValues(juce::int64 totalSystemRamBytes,
                 juce::int64 osUsedRamBytes,
                 juce::int64 organFootprintBytes);

  void paint(juce::Graphics& g) override;

private:
  juce::int64 totalSystemRam_ = 0;
  juce::int64 osUsedRam_ = 0;
  juce::int64 organFootprint_ = 0;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RamGraphMeterComponent)
};

class OrganAudioSettingsDialog : public juce::Component {
public:
  OrganAudioSettingsDialog(const OrganEntry& entry, MasterpieceProcessor& proc);
  ~OrganAudioSettingsDialog() override;

  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  class StatScanThread;
  std::unique_ptr<StatScanThread> statThread_;

  void onScanProgress(double progress, int current, int total);
  void onScanCompleted(const OrganAudioStat& stat);

  void syncProfile();
  void updateControlsFromConfig();
  void updateRamFootprintDisplay();
  void rebuildDropdownItemTexts();
  void showOrganRoot();
  void save();
  void resetToDefaults();
  void closeDialog();

  OrganEntry entry_;
  MasterpieceProcessor& proc_;
  OrganAudioConfig config_;
  OrganAudioStat audioStat_;

  bool isScanning_ = false;
  double scanProgress_ = 0.0;
  juce::ProgressBar scanProgressBar_{scanProgress_};
  juce::Label scanStatusLabel_;

  juce::Label titleLabel_;
  juce::Label subtitleLabel_;

  // RAM Usage Graph & breakdown
  juce::Label ramHeading_;
  RamGraphMeterComponent ramMeter_;
  juce::Label ramDetailsLabel_;

  // Memory profile
  juce::Label profileLabel_;
  juce::ComboBox profileCombo_;

  // Resident sample format
  juce::Label storageLabel_;
  juce::ComboBox storageCombo_;

  // Channels
  juce::ToggleButton monoToggle_{"Load in mono (halves RAM, sums stereo to mono)"};

  // Sample rate
  juce::Label rateLabel_;
  juce::ComboBox rateCombo_;

  // Streaming & preload
  juce::ToggleButton streamToggle_{"Stream release tails from disk"};
  juce::Label preloadLabel_;
  juce::ComboBox preloadCombo_;

  // Cache
  juce::Label cacheLabel_;
  juce::ComboBox cacheCombo_;

  // Organ root override
  juce::Label rootLabel_;
  juce::Label rootValue_;
  juce::TextButton rootChooseBtn_{"Choose..."};
  juce::TextButton rootDefaultBtn_{"Default"};
  std::unique_ptr<juce::FileChooser> rootChooser_;

  // Engine switches
  juce::Label dspHeading_;
  juce::ToggleButton simpleWav_{"Simple WAV only (bypass all DSP)"};
  juce::ToggleButton wind_{"Wind model"};
  juce::ToggleButton tremulant_{"Tremulants"};
  juce::ToggleButton enclosure_{"Enclosures (swell shades)"};
  juce::ToggleButton voicing_{"Voicing adjustments"};
  juce::ToggleButton originalPitch_{"Play at original organ's pitch"};

  // Bottom buttons
  juce::TextButton saveBtn_{"Save Settings"};
  juce::TextButton defaultsBtn_{"Reset to Defaults"};
  juce::TextButton cancelBtn_{"Cancel"};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OrganAudioSettingsDialog)
};

class OrganDialog : public juce::Component,
                    public juce::ListBoxModel,
                    public juce::Timer {
public:
  OrganDialog(MasterpieceEditor& editor, MasterpieceProcessor& proc);
  ~OrganDialog() override;

  static void show(MasterpieceEditor& editor, MasterpieceProcessor& proc);

  void resized() override;
  void paint(juce::Graphics& g) override;

  int getNumRows() override;
  void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                        bool rowIsSelected) override;
  void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
  void selectedRowsChanged(int lastRowSelected) override;
  void deleteKeyPressed(int lastRowSelected) override;
  void returnKeyPressed(int lastRowSelected) override;

  void timerCallback() override;

private:
  class InstallThread;
  std::unique_ptr<InstallThread> installThread_;

  void refreshList();
  void updateFilter();
  void loadSelected();
  void adjustAudioSettings();
  void showDetails();
  void removeSelected();
  void performDeleteOrgan(const OrganEntry& entry);
  void openOdf();
  void installPackages();
  void startExtraction(const juce::Array<juce::File>& archives);
  void cancelInstallation();
  void updateInstallProgress(int currentArchIdx, int totalArchs, const juce::String& archName,
                             double overallProgress, double subProgress, const juce::String& currentFile);
  void installFinished(int succeeded, int total, bool aborted, const juce::String& error);
  void closeDialog();

  MasterpieceEditor& editor_;
  MasterpieceProcessor& proc_;
  std::vector<OrganEntry> allOrgans_;
  std::vector<OrganEntry> filteredOrgans_;

  juce::Label titleLabel_;
  juce::Label subtitleLabel_;
  juce::TextEditor filterBox_;
  juce::ListBox listBox_;

  // Selected organ action buttons
  juce::TextButton loadBtn_{"Load"};
  juce::TextButton adjustAudioBtn_{"Adjust Audio Settings..."};
  juce::TextButton detailsBtn_{"Details..."};
  juce::TextButton removeBtn_{"Remove..."};

  // General action buttons
  juce::TextButton openOdfBtn_{"Open ODF..."};
  juce::TextButton installBtn_{"Install Organ Packages..."};

  std::unique_ptr<juce::FileChooser> chooser_;

  // Progress overlay panel
  struct OverlayPanel : public juce::Component {
    OverlayPanel(double& progressRef);
    void paint(juce::Graphics& g) override;
    void resized() override;

    juce::Label titleLabel;
    juce::Label archiveLabel;
    juce::Label etaLabel;
    juce::ProgressBar progressBar;
    juce::Label fileLabel;
    juce::TextButton cancelBtn{"Cancel"};
    std::function<void()> onCancel;
  };

  double installProgress_ = 0.0;
  double installStartTimeMs_ = 0.0;
  double installEtaSeconds_ = -1.0;
  OverlayPanel installPanel_{installProgress_};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OrganDialog)
};

} // namespace mp::ui
