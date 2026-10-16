// The split keyboard (#358) in the program: the module's engine, its files,
// and the few places it meets the rest. Everything the split decides is in
// mp_split; this only carries messages in and out, keeps the files, and
// proposes which manual each of the split's channels plays.
//
// Where it meets the rest:
// - The processor's MIDI handling calls process() once per block, after the
//   inputs are drained and before anything matches a message, and
//   retagReplay() after the recorder has taken its copy.
// - The split's input, "Split keyboard", is a MIDI device like any console,
//   marked exclusive in the MIDI map: only mappings that name it take its
//   notes. Its channels are mapped to manuals in Settings > MIDI, and a first
//   mapping is proposed for each organ when the split is first used with it.
// - Files: the organ's layout beside its other settings (split/<organ>.mpsplit),
//   the shared presets and controls in split-keyboard.txt.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../mp_control/MidiMap.h"
#include "../mp_core/OrganModel.h"
#include "../mp_split/SplitKeyboard.h"
#include "../mp_split/SplitText.h"

namespace mp {

class SplitKeyboardHost {
public:
  static constexpr const char* kInputName = "Split keyboard";

  explicit SplitKeyboardHost(MidiMap& map) : map_(map) {}

  // Message thread, once, with the id the split's input was given, and what
  // to call when the split changed the MIDI map (it is saved by its owner).
  void attach(int ownDevice, std::function<void()> mapChanged);

  // --- audio thread -------------------------------------------------------
  using Tagged = std::vector<std::pair<int, juce::MidiMessage>>;
  // The block's messages, rewritten in place: the source keyboard's notes
  // become the split's, on its input.
  void process(Tagged& messages, std::vector<int>& offsets, double nowMs);
  // A recording of the split played back comes from no input: its notes on
  // the split's channels are the split's again.
  void retagReplay(Tagged& messages) const;

  // --- message thread -----------------------------------------------------
  // An organ came up (or went: an empty file). Its layout is read, and if the
  // split is on, its channels get a first mapping. True if the MIDI map
  // changed and wants saving.
  bool organLoaded(const juce::File& layoutFile, const OrganModel* model);
  // From a timer: what the audio thread changed is saved, and a split
  // switched on from MIDI proposes its mapping. True if the MIDI map changed.
  bool service();

  const split::State& state() const { return state_; }
  // The organ's layout as it stands, and the preset it came from (-1: none).
  const split::Layout& layout() const { return organLayout_; }
  int preset() const { return organPreset_; }
  const OrganModel* model() const { return model_; }
  const split::Shared& shared() const { return shared_; }
  bool enabled() const { return shared_.on; }
  int ownDevice() const { return ownDevice_; }

  bool setEnabled(bool on);  // true if the MIDI map changed
  void setLayout(const split::Layout& l);
  // Copies the preset into the organ's layout.
  void recall(int slot);
  // The organ's layout into a player's slot (3..8), under this name.
  void saveAsPreset(int slot, const std::string& name);
  void deletePreset(int slot);
  std::string presetName(int slot) const;
  // The keyboard read, by input name; empty: any input.
  void setSource(const std::string& inputName);
  void learn(int control) { engine_.learn(control); }
  void clearControl(int control);

  // The manual a role plays on this organ, from the MIDI map; 0: none.
  Id keyboardFor(split::Role role) const;
  void setKeyboardFor(split::Role role, Id keyboardId);
  // A first mapping of the roles to this organ's manuals.
  static Id proposedKeyboard(const OrganModel& model, split::Role role);

  static juce::File sharedFile();

private:
  void pushAll();
  void saveShared() const;
  void saveOrgan() const;
  bool proposeMapping();
  void drainTo(int offset);

  MidiMap& map_;
  std::function<void()> mapChanged_;
  split::SplitKeyboard engine_;
  split::Shared shared_;
  split::State state_;
  int ownDevice_ = 0;

  juce::File organFile_;
  const OrganModel* model_ = nullptr;
  split::Layout organLayout_ = split::factoryLayout(1);
  int organPreset_ = -1;
  uint32_t seenLayoutChanges_ = 0, seenTriggerChanges_ = 0;
  bool proposed_ = false;  // for this organ, in this run

  Tagged scratch_;
  std::vector<int> scratchOffsets_;
};

}  // namespace mp
