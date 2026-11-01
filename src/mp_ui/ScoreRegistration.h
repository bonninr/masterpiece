// A registration mark's content: the organ's state as it was set, kept for
// each organ the mark was set on, so one score serves several organs.
//
// An organ's state is what the player sets for a passage: the stops, the
// couplers and the tremulants (as the player's own pistons hold them), the
// volume, the voicing EQ, and the drawn shoes and sliders. The organ is
// named by its key (MasterpieceProcessor::organKey), which follows the set
// across updates and moves.
//
// In the mark, one line per organ: the key, a tab, and the state as JSON.
#pragma once

#include <juce_core/juce_core.h>

#include "../mp_audio/VoicingEq.h"
#include "../mp_core/OrganModel.h"

#include <map>
#include <string>
#include <vector>

namespace mp {
class MasterpieceProcessor;
}

namespace mp::ui {

struct ScoreOrganState {
  std::string organ;            // the organ's key
  juce::String organName;       // for reading, on another organ
  std::vector<std::string> drawn;  // PlayerCombinations::keyOf, of what is on
  float gainDb = 0.0f;
  VoicingEqSettings eq;
  std::map<Id, int> controls;   // the drawn shoes and sliders, by control
};

std::vector<ScoreOrganState> parseScoreStates(const std::string& text);
std::string writeScoreStates(const std::vector<ScoreOrganState>& states);

// The state for the organ loaded now, or null.
const ScoreOrganState* stateForOrgan(const std::vector<ScoreOrganState>& states, const std::string& organ);

// What the organ loaded now has set.
ScoreOrganState captureScoreState(MasterpieceProcessor& proc);
// Sets it on the organ, which must be the one it was taken from.
void applyScoreState(MasterpieceProcessor& proc, const ScoreOrganState& state);
// Its content, a line each, for the mark's window.
juce::StringArray describeScoreState(const MasterpieceProcessor& proc, const ScoreOrganState& state);

}  // namespace mp::ui
