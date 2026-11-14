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
// The board as a moving mass (#812) is an option, off by default: "Moving
// reservoir boards". While it is off none of the below runs and the solver
// is the swallowed-volume model above, bit for bit.
//
// While it is on, each reservoir whose board the file describes in full has
// that board moving. The file states everything it needs: the weight under
// gravity, whose force over the frame's area IS the regulated pressure (a
// round figure on every set: Klais 6.00 inches, Erfurt 5.12), the moving
// mass, the damping, the travel, and for a hinged board its centre of mass.
// No set in the corpus uses springs. The board is solved in the volume it
// displaces, u: M u'' = p - pReg - R u', between its bottom and its full
// travel, and the air in the reservoir follows its volume, V0 + u. The board
// reports its height to the extension output control, 0 to 127, and every set
// in the corpus drives its inlet valve from that through one control linkage
// -- Klais closes it between 39 and 84 percent of the travel. That loop is
// solved here, so the reservoir regulates itself as the real one does.
//
// The earlier objection to a board still holds and is met: a spring toward an
// invented "regulated pressure" pulled against the pipework forever. The
// pressure is the file's own, and the working pressures are re-solved with
// the boards in place, so an organ with nobody playing reads no sag.
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
    // The share of ampMul that is the chest too empty to speak (below half
    // its working pressure), 1 above that. A pipe's own depth (#821) trims
    // the pressure's movement and leaves this whole.
    double supply = 1.0;
  };

  // Moving reservoir boards (#812). Takes effect at the next reset(): the
  // working pressures are solved with the boards in place.
  void setBellowsBoard(bool on) { boardWanted_ = on; }

  void reset(const OrganModel& model);

  // Re-solve the working pressures with the organ in the state it is actually
  // in. Call once the blower is running and the valves are where they belong:
  // a compartment cut off by a shut valve settles somewhere else entirely, and
  // measuring its sag against the all-valves-open figure reads as a permanent
  // 48% drop that has nothing to do with anything being played.
  void settleWith(const std::unordered_set<Id>& engagedSwitches);

  // The valves a continuous control opens (ValveControlTypeCode 3), other than
  // a reservoir's own regulator, found at reset. The caller reads each control
  // once a block and passes its value in; a valve reads open until it does,
  // and always while settling.
  struct ControlValve {
    size_t link = 0;  // position among the resolved links
    Id controlId = 0;
    int low = 0, high = 127;
  };
  const std::vector<ControlValve>& controlValves() const { return controlValves_; }
  void setControlValve(size_t index, int value);

  // Tell the solver what is sounding, before advancing. Call once per block:
  // clear, then add the demand of each compartment from a table the caller
  // already has. The engine knows what is sounding; walking its voice pool
  // here would mean the solver knowing about voices.
  void clearDemand();
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

  // A moving board's height, 0 at the bottom to 127 at full travel: what the
  // compartment reports to its extension output control. -1 when the
  // compartment has no moving board.
  double extensionFor(Id compartmentId) const;
  // The moving boards' compartments and their extension output controls, for
  // the caller to publish; empty while the option is off.
  std::vector<std::pair<Id, Id>> extensionOutputs() const;

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
  void settleSolve(const std::unordered_set<Id>* engagedSwitches);
  // Every control valve reads open while this is set.
  bool settling_ = false;
  std::vector<ControlValve> controlValves_;
  // Per resolved link, how far its control valve is open: 1 for every other link.
  std::vector<double> linkOpening_;
  void resetControlValves(const OrganModel& model);
  void settleSwitches(const std::unordered_set<Id>& engagedSwitches);
  void settleControlValves(const std::unordered_set<Id>& engagedSwitches);
  // The working pressures solved for directly: where every compartment's
  // inflow equals its outflow. Milliseconds, against seconds for integrating
  // up to the same point. False when it does not converge, and settle() then
  // integrates as before.
  bool solveSteady(const std::unordered_set<Id>* engagedSwitches);
  // Work out how finely this particular organ has to be integrated.
  void chooseStep();

public:
  // How the last settle went, for the load log: sweeps of the direct solve,
  // or integration steps when it fell back to those.
  int lastSettleSweeps() const { return lastSweeps_; }
  long long lastSettleSteps() const { return lastSteps_; }
  // Integrate to the working pressures instead of solving for them: the way
  // every version before 0.7.2 did it, kept for comparison.
  void setDirectSettle(bool on) { directSettle_ = on; }

private:
  int lastSweeps_ = 0;
  long long lastSteps_ = 0;
  bool directSettle_ = true;

  const OrganModel* model_ = nullptr;
  std::vector<State> order_;
  std::unordered_map<Id, int> index_;
  std::unordered_map<Id, Id> pipeCompartment_; // pipe id -> source compartment
  std::vector<WindCompartmentLink> links_;
  // Each link with its two ends resolved once, at reset: a position in
  // order_, or -1 and the fixed pressure of a compartment that is not
  // modelled. Looking them up by id on every step was most of a large
  // organ's load time -- the settle runs millions of steps (#133).
  struct ResolvedLink {
    size_t link = 0;  // index in links_, which survives a copy
    int a = -1, b = -1;
    double fixedA = 0.0, fixedB = 0.0;
  };
  std::vector<ResolvedLink> resolved_;
  // Whether each resolved link's valve is open, worked out once for a run of
  // steps: the switches do not move inside a block, and looking each valve's
  // switch up again on every step of it cost a hash lookup per link per step.
  std::vector<char> valveOpen_;
  // The drift of each resolved link's flow (#807), stepped once a block while
  // the organ plays; the factor stays 1 for a link whose set states none, and
  // for settling, which finds the working pressures of the steady organ.
  std::vector<RandomWalkState> linkWalks_;
  std::vector<double> linkFlowMul_;
  bool anyLinkWalk_ = false;
  bool driftFlow_ = false;
  void readValves(const std::unordered_set<Id>* engagedSwitches);
  void resolveLinks();
  // The largest step this system can be integrated at without ringing.
  // Derived from the organ's own numbers — a small chest fed by a fat pipe has
  // a time constant of milliseconds — rather than picked and hoped for.
  double maxStep_ = 0.002;
  // Time owed to the solver but not yet integrated. The wind is stepped at a
  // FIXED rate and the leftover is carried, so the pressure a chord produces
  // does not depend on the host's buffer size.
  double debtSeconds_ = 0.0;
  bool running_ = false;

  // Moving reservoir boards (#812). Each in the volume it displaces, u, with
  // its figures turned into that coordinate at reset: pressure in inches,
  // u in cubic metres.
  struct Board {
    int at = -1;              // position in order_
    Id outputControlId = 0;
    double fixedVolumeM3 = 0.0;
    double travelM3 = 0.0;    // u at full travel
    double regulatedInches = 0.0;
    double massPerInch = 0.0; // inches per (m3/s^2) of acceleration
    double dampRate = 0.0;    // per second
    double u = 0.0, uDot = 0.0;
  };
  std::vector<Board> boards_;
  // Per resolved link, the board whose height drives its valve, or -1; and
  // the control linkage between the two. Empty while the option is off.
  std::vector<int> boardValve_;
  std::vector<ContinuousControlLinkage> boardValveLink_;
  bool boardWanted_ = false;
  // While settling, each board is damped to critical: the balance is the
  // same, and an organ whose boards would ring for minutes settles at once.
  bool settlingBoards_ = false;
  void resetBoards(const OrganModel& model);
  void settleBoards(const std::unordered_set<Id>* engagedSwitches);
  void stepBoards(double dt);
  double boardValveOpening(size_t link) const;
};

} // namespace mp
