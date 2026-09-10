// The wind system: why an organ sags when a big chord lands.
//
// An organ is a pneumatic machine. Every sounding pipe draws air out of the
// chest it stands on; the reservoir and the blower put it back, but not
// instantly. Drop the full tutti onto the keys and the pressure dips before it
// recovers, and the whole instrument dips with it — a little flatter, a little
// softer, and settling with the wobble the reservoir's own mass gives it.
// Draw one flute and nothing happens at all. That difference is the point.
//
// This lives in mp_control rather than mp_dsp because it is control-rate
// STATE, not an audio process: pressures advance once per block, never per
// sample, and nothing here touches a sample. That also keeps it JUCE-free, so
// the fast loop tests it in seconds. Nothing here allocates after reset().
//
// What is solved, and what is not
// -------------------------------
// Solved from the organ's own numbers: compartment volumes, pipe mass-flow
// rates at their reference pressures, the links between compartments and their
// valves. Flow through an opening goes as the square root of the pressure
// difference across it, and a compartment's pressure follows from how much air
// is in it -- for small excess pressures, dp/dt = 339 * netFlow / volume, with
// pressure in inches of water and flow in kilograms per second. (339 is
// P_atm/rho_atm converted into those units; it is not a tuning knob.)
//
// A reservoir is its SWALLOWED VOLUME. Its board rises and falls, so it can
// take in air without its pressure moving at all, and the volume it can take
// is the frame area times the travel — width, length and maximum extension,
// all of which the organ declares. Nancy's main reservoir is 1.5 m3 of fixed
// volume plus 1.35 m3 of board travel, so it very nearly doubles. That is why
// an organ with a big reservoir holds its wind under a chord.
//
// NOT solved: the board as a moving mass. Hauptwerk carries its weight, its
// damping, spring lengths and tension curves, and a real bellows solver works
// out where the board sits and what force is on it. That would add the
// overshoot and the slow wobble a reservoir has when a big chord lands and
// leaves. It is deliberately absent rather than approximated: an earlier draft
// modelled the board as a spring pulling toward a "regulated pressure", and
// since nothing in the file says what that pressure IS, the spring pulled
// against the pipework forever — a steady two percent sag on an organ with
// nobody playing it. A missing effect is better than an invented one.
#pragma once
#include "../mp_core/OrganModel.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mp {

class WindSolver {
public:
  // Modulation one compartment currently imposes on the pipes standing on it.
  struct Mod {
    double ampMul = 1.0;
    double pitchRatio = 1.0;
  };

  void reset(const OrganModel& model);

  // Re-solve the working pressures with the organ in the state it is actually
  // in. Call once the blower is running and the valves are where they belong:
  // a compartment cut off by a shut valve settles somewhere else entirely, and
  // measuring its sag against the all-valves-open figure reads as a permanent
  // 48% drop that has nothing to do with anything being played.
  void settleWith(const std::unordered_set<Id>& engagedSwitches);

  // Tell the solver what is sounding, before advancing. Call once per block:
  // clear, then add each sounding pipe.
  void clearDemand();
  void addDemand(const Pipe& pipe);
  // Set every compartment's demand at once, from a table the caller already
  // has. This is the path the engine uses: it knows what is sounding, and
  // walking its voice pool here would mean the solver knowing about voices.
  void addDemandDirect(const std::vector<Id>& compartments,
                       const std::vector<float>& kgPerSec);

  // Advance by `dtSeconds`. Returns false when the model is off, in which case
  // every compartment reads nominal and nothing modulates.
  bool advance(double dtSeconds, const EngineSwitch& sw,
               const std::unordered_set<Id>& engagedSwitches);

  // Pressure in inches of water. Nominal for a compartment that is not modelled.
  double pressureFor(Id compartmentId) const;
  // How far off nominal, as a fraction: 0 is steady, -0.05 is five percent down.
  double sagFor(Id compartmentId) const;
  // The working pressure this compartment settles to with the organ running
  // and nothing playing. Solved, not declared — see settle().
  double nominalFor(Id compartmentId) const;
  // What the pipes on this compartment should do about it.
  Mod modFor(Id compartmentId) const;

  // Which compartment a pipe stands on, resolved once at reset. 0 when the
  // organ says nothing, and then the pipe is unaffected.
  Id compartmentForPipe(const Pipe& pipe) const;

  size_t modelledCompartments() const { return order_.size(); }
  // The integration step this organ needs, in seconds. Small means a chest
  // somewhere is tiny and its supply pipe is fat.
  double stepSeconds() const { return maxStep_; }
  // What this compartment is currently being asked for, in kg/s. Reported so a
  // wind model that does nothing can be told from an organ that is asking for
  // nothing.
  double demandFor(Id compartmentId) const;
  bool active() const { return !order_.empty(); }

private:
  struct State {
    Id id = 0;
    double volumeM3 = 1.0;
    double nominalInches = 0.0;
    double pressure = 0.0;
    double velocity = 0.0; // unused by the current model; see the header
    double demandKgPerSec = 0.0;
  };

  int indexOf(Id compartmentId) const;
  // One integration step. `engagedSwitches` null means every valve is open,
  // which is what settling uses.
  void integrate(double dt, const std::unordered_set<Id>* engagedSwitches);
  // Find each compartment's working pressure by running the system empty. A
  // windchest does not declare one — see the .cpp.
  void settle(const std::unordered_set<Id>* engagedSwitches = nullptr);
  // Work out how finely this particular organ has to be integrated.
  void chooseStep();

  const OrganModel* model_ = nullptr;
  std::vector<State> order_;
  std::unordered_map<Id, int> index_;
  std::unordered_map<Id, Id> pipeCompartment_; // pipe id -> source compartment
  std::vector<WindCompartmentLink> links_;
  // The largest step this system can be integrated at without ringing.
  // Derived from the organ's own numbers — a small chest fed by a fat pipe has
  // a time constant of milliseconds — rather than picked and hoped for.
  double maxStep_ = 0.002;
  // Time owed to the solver but not yet integrated. The wind is stepped at a
  // FIXED rate and the leftover is carried, so the pressure a chord produces
  // does not depend on the host's buffer size.
  double debtSeconds_ = 0.0;
  bool running_ = false;
};

} // namespace mp
