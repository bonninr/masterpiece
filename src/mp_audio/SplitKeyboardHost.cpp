#include "SplitKeyboardHost.h"

#include <algorithm>

namespace mp {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

juce::File SplitKeyboardHost::sharedFile() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("split-keyboard.txt");
}

void SplitKeyboardHost::attach(int ownDevice, std::function<void()> mapChanged) {
  ownDevice_ = ownDevice;
  mapChanged_ = std::move(mapChanged);
  map_.setExclusiveDevice(ownDevice);
  scratch_.reserve(1024);
  scratchOffsets_.reserve(1024);
  const auto f = sharedFile();
  if (f.existsAsFile())
    shared_ = split::sharedFromText(f.loadFileAsString().toStdString(),
                                    [this](const std::string& n) { return map_.devices().idFor(n); });
  pushAll();
}

void SplitKeyboardHost::pushAll() {
  engine_.setPresets(shared_.presets);
  engine_.setTriggers(shared_.triggers);
  const int source = shared_.source.empty() ? 0 : map_.devices().idFor(shared_.source);
  engine_.setDevices(source, ownDevice_);
  engine_.setLayout(organLayout_, organPreset_);
  engine_.setEnabled(shared_.on);
}

// --- audio thread ------------------------------------------------------------

void SplitKeyboardHost::drainTo(int offset) {
  int n = 0;
  const auto* m = engine_.output(n);
  for (int i = 0; i < n; ++i) {
    scratch_.emplace_back(ownDevice_, juce::MidiMessage(m[i].status, m[i].data1, m[i].data2));
    scratchOffsets_.push_back(offset);
  }
  engine_.clearOutput();
}

void SplitKeyboardHost::process(Tagged& messages, std::vector<int>& offsets, double nowMs) {
  if (ownDevice_ == 0) return;
  scratch_.clear();
  scratchOffsets_.clear();
  engine_.beginBlock(nowMs);
  drainTo(0);
  for (size_t i = 0; i < messages.size(); ++i) {
    const auto& [device, msg] = messages[i];
    const int offset = i < offsets.size() ? offsets[i] : 0;
    const int size = msg.getRawDataSize();
    const bool taken = size >= 1 && size <= 3 && engine_.feed(device, msg.getRawData(), size);
    if (!taken) {
      scratch_.emplace_back(device, msg);
      scratchOffsets_.push_back(offset);
    }
    drainTo(offset);
  }
  engine_.endBlock();
  drainTo(0);
  messages.swap(scratch_);
  offsets.swap(scratchOffsets_);
}

void SplitKeyboardHost::retagReplay(Tagged& messages) const {
  if (ownDevice_ == 0) return;
  for (auto& [device, msg] : messages)
    if (device == MidiDeviceMap::kAnyDevice && msg.isNoteOnOrOff() &&
        split::roleOfChannel(msg.getChannel()) != split::Role::None)
      device = ownDevice_;
}

// --- message thread ----------------------------------------------------------

bool SplitKeyboardHost::organLoaded(const juce::File& layoutFile, const OrganModel* model) {
  organFile_ = layoutFile;
  model_ = model;
  proposed_ = false;
  organLayout_ = split::factoryLayout(1);
  organPreset_ = -1;
  if (organFile_.existsAsFile())
    split::organFromText(organFile_.loadFileAsString().toStdString(), organLayout_, organPreset_);
  engine_.setLayout(organLayout_, organPreset_);
  return shared_.on && proposeMapping();
}

bool SplitKeyboardHost::service() {
  split::State s;
  if (!engine_.state(s)) return false;
  state_ = s;
  bool mapChanged = false;
  if (s.enabled != shared_.on) {
    shared_.on = s.enabled;
    saveShared();
    if (s.enabled) mapChanged = proposeMapping();
  }
  if (s.layoutChanges != seenLayoutChanges_) {
    seenLayoutChanges_ = s.layoutChanges;
    organLayout_ = s.layout;
    organPreset_ = s.preset;
    saveOrgan();
  }
  if (s.triggerChanges != seenTriggerChanges_) {
    seenTriggerChanges_ = s.triggerChanges;
    std::copy(std::begin(s.triggers), std::end(s.triggers), shared_.triggers.begin());
    saveShared();
  }
  return mapChanged;
}

bool SplitKeyboardHost::setEnabled(bool on) {
  if (on == shared_.on) return false;
  shared_.on = on;
  engine_.setEnabled(on);
  saveShared();
  return on && proposeMapping();
}

void SplitKeyboardHost::setLayout(const split::Layout& l) {
  organLayout_ = l;
  organLayout_.normalize();
  organPreset_ = -1;
  engine_.setLayout(organLayout_, -1);
  saveOrgan();
}

void SplitKeyboardHost::recall(int slot) {
  if (slot < 0 || slot >= split::kPresetCount || !shared_.presets.used[slot]) return;
  organLayout_ = shared_.presets.layouts[slot];
  organPreset_ = slot;
  engine_.setLayout(organLayout_, slot);
  saveOrgan();
}

void SplitKeyboardHost::saveAsPreset(int slot, const std::string& name) {
  if (slot < split::kFactoryPresets || slot >= split::kPresetCount) return;
  shared_.presets.layouts[slot] = organLayout_;
  shared_.presets.used[slot] = true;
  shared_.names[slot] = name;
  organPreset_ = slot;
  engine_.setPresets(shared_.presets);
  engine_.setLayout(organLayout_, slot);
  saveShared();
  saveOrgan();
}

void SplitKeyboardHost::deletePreset(int slot) {
  if (slot < split::kFactoryPresets || slot >= split::kPresetCount) return;
  shared_.presets.used[slot] = false;
  shared_.names[slot].clear();
  if (organPreset_ == slot) {
    organPreset_ = -1;
    engine_.setLayout(organLayout_, -1);
    saveOrgan();
  }
  engine_.setPresets(shared_.presets);
  saveShared();
}

std::string SplitKeyboardHost::presetName(int slot) const {
  if (slot < 0 || slot >= split::kPresetCount) return {};
  if (slot < split::kFactoryPresets) return split::factoryName(slot);
  return shared_.presets.used[slot] ? shared_.names[slot] : std::string();
}

void SplitKeyboardHost::setSource(const std::string& inputName) {
  shared_.source = inputName;
  const int source = inputName.empty() ? 0 : map_.devices().idFor(inputName);
  engine_.setDevices(source, ownDevice_);
  saveShared();
}

void SplitKeyboardHost::clearControl(int control) {
  if (control < 0 || control >= split::kControlCount) return;
  shared_.triggers[static_cast<size_t>(control)] = split::Trigger{};
  engine_.setTriggers(shared_.triggers);
  saveShared();
}

Id SplitKeyboardHost::keyboardFor(split::Role role) const {
  const int channel = split::channelOf(role);
  for (const auto& b : map_.keyboardBindings())
    if (b.deviceId == ownDevice_ && b.channel == channel) return b.keyboardId;
  return 0;
}

void SplitKeyboardHost::setKeyboardFor(split::Role role, Id keyboardId) {
  const int channel = split::channelOf(role);
  if (channel == 0 || ownDevice_ == 0) return;
  // The split's own lines only: what it maps, it maps alone.
  std::vector<MidiMap::KeyboardBinding> keep;
  for (const auto& b : map_.keyboardBindings())
    if (!(b.deviceId == ownDevice_ && b.channel == channel)) keep.push_back(b);
  if (keep.size() != map_.keyboardBindings().size()) {
    std::vector<Id> ids;
    for (const auto& b : map_.keyboardBindings())
      if (std::find(ids.begin(), ids.end(), b.keyboardId) == ids.end()) ids.push_back(b.keyboardId);
    for (Id id : ids) map_.removeKeyboardBindingsFor(id);
    for (const auto& b : keep) map_.addKeyboardBinding(b);
  }
  if (keyboardId != 0) {
    MidiMap::KeyboardBinding b;
    b.deviceId = ownDevice_;
    b.channel = channel;
    b.keyboardId = keyboardId;
    map_.addKeyboardBinding(b);
  }
  if (mapChanged_) mapChanged_();
}

Id SplitKeyboardHost::proposedKeyboard(const OrganModel& model, split::Role role) {
  struct Candidate {
    Id id;
    int code;
    bool pedal;
  };
  std::vector<Candidate> all;
  for (const auto& [id, kb] : model.keyboards) {
    if (!kb.accessibleForInput || kb.numKeys <= 0) continue;
    const bool pedal = kb.assignmentCode == 1 || lower(kb.name).find("pedal") != std::string::npos;
    all.push_back({id, kb.assignmentCode > 0 ? kb.assignmentCode : 1000, pedal});
  }
  std::sort(all.begin(), all.end(),
            [](const Candidate& a, const Candidate& b) { return a.code != b.code ? a.code < b.code : a.id < b.id; });
  std::vector<Id> manuals;
  Id pedal = 0;
  for (const auto& c : all) {
    if (c.pedal) {
      if (pedal == 0) pedal = c.id;
    } else {
      manuals.push_back(c.id);
    }
  }
  const auto manual = [&](size_t i) { return manuals.empty() ? Id(0) : manuals[std::min(i, manuals.size() - 1)]; };
  switch (role) {
    case split::Role::Lower: return manual(0);
    case split::Role::Upper: return manual(1);
    case split::Role::Solo: return manual(2);
    case split::Role::Pedal: return pedal;
    default: return 0;
  }
}

bool SplitKeyboardHost::proposeMapping() {
  if (proposed_ || model_ == nullptr || ownDevice_ == 0) return false;
  proposed_ = true;
  bool changed = false;
  for (int r = 1; r <= split::kRoleCount; ++r) {
    const auto role = static_cast<split::Role>(r);
    if (map_.hasChannelBinding(ownDevice_, split::channelOf(role))) continue;
    const Id kb = proposedKeyboard(*model_, role);
    if (kb == 0) continue;
    setKeyboardFor(role, kb);
    changed = true;
  }
  return changed;
}

void SplitKeyboardHost::saveShared() const {
  const auto f = sharedFile();
  f.getParentDirectory().createDirectory();
  f.replaceWithText(split::sharedToText(shared_, [this](int id) { return map_.devices().nameFor(id); }));
}

void SplitKeyboardHost::saveOrgan() const {
  if (organFile_.getFullPathName().isEmpty()) return;
  organFile_.getParentDirectory().createDirectory();
  organFile_.replaceWithText(split::organToText(organLayout_, organPreset_));
}

}  // namespace mp
