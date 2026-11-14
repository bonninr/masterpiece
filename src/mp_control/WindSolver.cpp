#include "WindSolver.h"

#include <algorithm>
#include <cmath>

namespace mp {
namespace {

// Pressure in a fixed volume, per kilogram of air added. P_atm / rho_atm is
// 84437 J/kg; one inch of water is 249.09 Pa. This is physics, not a knob.
constexpr double kInchesPerKgPerM3 = 84437.0 / 249.09;

// Orifice flow: what goes through an opening at a pressure difference, given
// what goes through it at a reference difference. Square-root, and signed, so
// that a reversed difference blows the other way instead of going imaginary.
inline double orificeFlow(double flowAtRef, double refDiff, double diff) {
  if (flowAtRef <= 0.0 || refDiff <= 0.0) return 0.0;
  const double ratio = diff / refDiff;
  const double mag = std::sqrt(std::abs(ratio));
  return flowAtRef * (ratio < 0.0 ? -mag : mag);
}

// How far a flue pipe's pitch moves when its wind does. A pipe's frequency
// goes with the square root of pressure only in the limit; a real flue is
// held far closer than that by its own resonator, and organ builders quote a
// few cents for a few percent. This is the conservative end of that: about
// four cents for a five percent sag.
constexpr double kPitchPerFractionalSag = 0.25;

// And how much quieter it gets. Loudness follows the wind much more directly
// than pitch does.
constexpr double kAmpPerFractionalSag = 0.7;

// The fraction of its working pressure below which a pipe fades to silence.
constexpr double kSpeechFraction = 0.5;

// A moving board (#812). One inch of water in pascals, and the atmosphere in
// inches: a compartment whose volume grows by dV at a fixed amount of air
// loses p_atm * dV / V of pressure, the same isothermal law as the 339 above.
constexpr double kPaPerInch = 249.09;
constexpr double kAtmInches = 101325.0 / kPaPerInch;
constexpr double kGravity = 9.81;

} // namespace

void WindSolver::reset(const OrganModel& model) {
  model_ = &model;
  order_.clear();
  index_.clear();
  pipeCompartment_.clear();
  links_ = model.windLinks;
  running_ = false;

  // Only the compartments that can actually move. An infinite one is a
  // boundary condition — the room, the blower's intake — and modelling it
  // would be modelling nothing.
  for (const auto& [id, wc] : model.wind) {
    if (wc.infiniteVolume) continue;
    State st;
    st.id = id;
    st.volumeM3 = wc.volumeM3 > 0.0 ? wc.volumeM3 : 1.0;
    st.nominalInches = wc.defaultPressureInches;
    st.pressure = st.nominalInches;
    // A reservoir's board can rise and fall, so the compartment can take in
    // air without its pressure moving — which is the entire point of having
    // one. The volume it can swallow is the frame area times the travel, and
    // the organ declares all three. Nancy's main reservoir is 1.5 m3 of fixed
    // volume plus 1.35 m3 of travel: it very nearly doubles.
    if (wc.hasBellows) st.volumeM3 += wc.sweptVolumeM3();
    index_[id] = static_cast<int>(order_.size());
    order_.push_back(st);
  }

  // Where each pipe draws from. Resolved once: a note-on must not search.
  for (const auto& [rankId, rank] : model.ranks) {
    (void)rankId;
    for (const Pipe& pipe : rank.pipes)
      if (pipe.windSourceCompartmentId != 0)
        pipeCompartment_[pipe.pipeId] = pipe.windSourceCompartmentId;
  }

  resolveLinks();
  boards_.clear();
  boardValve_.clear();
  boardValveLink_.clear();
  if (boardWanted_) resetBoards(model);
  resetControlValves(model);
  chooseStep();
  settle();
}

void WindSolver::resetControlValves(const OrganModel& model) {
  // A valve a continuous control opens (#1028): Alessandria's blower fills its
  // reservoir through one, on the control the blower motor drives. A
  // reservoir's own regulator is left out: its control is the board's height,
  // which is solved only with the boards on, and read anywhere else it would
  // be a number nothing sets. Everything the board's height reaches counts,
  // through any number of linkages and double linkages: St Mary Portsea's
  // regulator valve is a double linkage of a "Regulator Base" the height
  // feeds and the wind supply's level (#1051).
  std::unordered_set<Id> regulators;
  for (const auto& [id, wc] : model.wind)
    if (wc.bellowsExtensionOutputControlId != 0) regulators.insert(wc.bellowsExtensionOutputControlId);
  for (bool grew = !regulators.empty(); grew;) {
    grew = false;
    for (const ContinuousControlLinkage& l : model.controlLinkages)
      if (regulators.count(l.sourceControlId) != 0 && regulators.insert(l.destControlId).second) grew = true;
    for (const ContinuousControlDoubleLinkage& l : model.controlDoubleLinkages)
      if ((regulators.count(l.firstControlId) != 0 || regulators.count(l.secondControlId) != 0) &&
          regulators.insert(l.destControlId).second)
        grew = true;
  }

  controlValves_.clear();
  linkOpening_.assign(resolved_.size(), 1.0);
  for (size_t k = 0; k < resolved_.size(); ++k) {
    const WindCompartmentLink& link = links_[resolved_[k].link];
    if (link.valveControlTypeCode != 3 || link.valveControlId == 0) continue;
    if (regulators.count(link.valveControlId) != 0) continue;
    if (!boardValve_.empty() && boardValve_[k] >= 0) continue;
    const auto c = model.continuousControls.find(link.valveControlId);
    if (c == model.continuousControls.end()) continue;
    ControlValve v;
    v.link = k;
    v.controlId = link.valveControlId;
    v.low = std::min(c->second.minValue, c->second.maxValue);
    v.high = std::max(c->second.minValue, c->second.maxValue);
    if (v.high > v.low) controlValves_.push_back(v);
  }
}

void WindSolver::setControlValve(size_t index, int value) {
  const ControlValve& v = controlValves_[index];
  linkOpening_[v.link] =
      std::clamp(static_cast<double>(value - v.low) / static_cast<double>(v.high - v.low), 0.0, 1.0);
}

void WindSolver::resetBoards(const OrganModel& model) {
  for (const auto& [id, wc] : model.wind) {
    const int at = indexOf(id);
    if (at < 0 || !wc.hasBellows) continue;
    const double area = wc.bellowsWidthM * wc.bellowsLengthM;
    const double inertia = wc.bellowsMassKg > 0.0 ? wc.bellowsMassKg : wc.bellowsGravityMassKg;
    // A board the file leaves without a weight, a frame or a travel stays
    // the swallowed volume it is with the option off.
    if (area <= 0.0 || wc.bellowsExtensionM <= 0.0 || wc.bellowsGravityMassKg <= 0.0 || inertia <= 0.0)
      continue;
    Board b;
    b.at = at;
    b.outputControlId = wc.bellowsExtensionOutputControlId;
    b.fixedVolumeM3 = wc.volumeM3 > 0.0 ? wc.volumeM3 : 1.0;
    if (wc.bellowsShapeCode == 2 && wc.bellowsHingeDistanceM > 0.0) {
      // Hinged at one edge, opening by the frame's length: the board turns by
      // theta and displaces a * theta, a = area * length / 2. The pressure
      // turns it with p * a, its weight with m g q, and its mass acts at its
      // centre of mass, q from the hinge.
      const double a = area * wc.bellowsLengthM * 0.5;
      const double q = wc.bellowsHingeDistanceM;
      b.travelM3 = area * wc.bellowsExtensionM * 0.5;
      b.regulatedInches = wc.bellowsGravityMassKg * kGravity * q / a / kPaPerInch;
      b.massPerInch = inertia * q * q / (kPaPerInch * a * a);
    } else {
      // Rising flat: it displaces area * height.
      b.travelM3 = area * wc.bellowsExtensionM;
      b.regulatedInches = wc.bellowsGravityMassKg * kGravity / area / kPaPerInch;
      b.massPerInch = inertia / (kPaPerInch * area * area);
    }
    // The damping is a rate per second, the speed it takes off the board,
    // as with a control linkage's inertia model: Klais's 10 settles its board
    // in a few swings. Read as a force it left every set's boards ringing for
    // tens of seconds.
    b.dampRate = std::max(0.0, wc.bellowsDamping);
    b.u = 0.5 * b.travelM3;
    order_[static_cast<size_t>(at)].volumeM3 = b.fixedVolumeM3 + b.u;
    boards_.push_back(b);
  }
  if (boards_.empty()) return;

  // The valves the boards drive: a link whose valve control is a board's
  // extension output, or is fed from one by an unconditional control linkage.
  boardValve_.assign(resolved_.size(), -1);
  boardValveLink_.assign(resolved_.size(), ContinuousControlLinkage{});
  for (size_t k = 0; k < resolved_.size(); ++k) {
    const Id valve = links_[resolved_[k].link].valveControlId;
    if (valve == 0) continue;
    for (size_t j = 0; j < boards_.size() && boardValve_[k] < 0; ++j) {
      const Id out = boards_[j].outputControlId;
      if (out == 0) continue;
      if (out == valve) {
        boardValve_[k] = static_cast<int>(j);
        continue;
      }
      for (const ContinuousControlLinkage& l : model.controlLinkages)
        if (l.sourceControlId == out && l.destControlId == valve && l.conditionSwitchId == 0) {
          boardValve_[k] = static_cast<int>(j);
          boardValveLink_[k] = l;
          break;
        }
    }
  }
}

double WindSolver::boardValveOpening(size_t link) const {
  const Board& b = boards_[static_cast<size_t>(boardValve_[link])];
  const double extension = b.travelM3 > 0.0 ? 127.0 * b.u / b.travelM3 : 0.0;
  return std::clamp(boardValveLink_[link].apply(extension), 0.0, 127.0) / 127.0;
}

void WindSolver::stepBoards(double dt) {
  for (Board& b : boards_) {
    State& st = order_[static_cast<size_t>(b.at)];
    double damp = b.dampRate;
    if (settlingBoards_) {
      // Critical damping on the board's bounce against its own air.
      const double stiffness = kAtmInches / st.volumeM3;
      damp = std::max(damp, 2.0 * std::sqrt(stiffness / b.massPerInch));
    }
    // Damped implicitly, so a heavy damping cannot overshoot.
    b.uDot = (b.uDot + (st.pressure - b.regulatedInches) / b.massPerInch * dt) / (1.0 + damp * dt);
    double u = b.u + b.uDot * dt;
    if (u <= 0.0) {
      u = 0.0;
      b.uDot = std::max(b.uDot, 0.0);
    } else if (u >= b.travelM3) {
      u = b.travelM3;
      b.uDot = std::min(b.uDot, 0.0);
    }
    if (!std::isfinite(u) || !std::isfinite(b.uDot)) {
      u = b.u;
      b.uDot = 0.0;
    }
    const double du = u - b.u;
    b.u = u;
    st.volumeM3 = b.fixedVolumeM3 + u;
    st.pressure -= kAtmInches * du / st.volumeM3;
  }
}

double WindSolver::extensionFor(Id compartmentId) const {
  const int i = indexOf(compartmentId);
  for (const Board& b : boards_)
    if (b.at == i && i >= 0) return b.travelM3 > 0.0 ? 127.0 * b.u / b.travelM3 : 0.0;
  return -1.0;
}

std::vector<std::pair<Id, Id>> WindSolver::extensionOutputs() const {
  std::vector<std::pair<Id, Id>> out;
  for (const Board& b : boards_)
    if (b.outputControlId != 0) out.emplace_back(order_[static_cast<size_t>(b.at)].id, b.outputControlId);
  return out;
}

void WindSolver::resolveLinks() {
  // A compartment that is not modelled sits at whatever it declares: that is
  // what makes it a boundary condition. The blower's intake and the open air
  // are both this.
  const auto fixedPressure = [this](Id id) {
    if (model_ != nullptr) {
      const auto it = model_->wind.find(id);
      if (it != model_->wind.end()) return it->second.defaultPressureInches;
    }
    return 0.0;
  };
  resolved_.clear();
  resolved_.reserve(links_.size());
  for (size_t i = 0; i < links_.size(); ++i) {
    const WindCompartmentLink& link = links_[i];
    ResolvedLink r;
    r.link = i;
    r.a = indexOf(link.firstCompartmentId);
    r.b = indexOf(link.secondCompartmentId);
    if (r.a < 0) r.fixedA = fixedPressure(link.firstCompartmentId);
    if (r.b < 0) r.fixedB = fixedPressure(link.secondCompartmentId);
    resolved_.push_back(r);
  }
  linkWalks_.assign(resolved_.size(), RandomWalkState{});
  linkFlowMul_.assign(resolved_.size(), 1.0);
  anyLinkWalk_ = false;
  for (size_t k = 0; k < resolved_.size(); ++k) {
    linkWalks_[k].start(links_[resolved_[k].link].flowRandom, 0x57494E44ull + k);
    anyLinkWalk_ = anyLinkWalk_ || linkWalks_[k].on();
  }
}

void WindSolver::chooseStep() {
  // How fast can a compartment's pressure change? For a link carrying F kg/s
  // at a reference difference of R inches, the flow near that difference
  // changes by about F/(2R) per inch, so the compartment's own time constant
  // is 2*R*V/(k*F). Integrate at a quarter of the fastest of those and the
  // system tracks instead of ringing.
  //
  // This is not a comfort margin. Nancy has 0.03 m3 compartments fed by the
  // same pipework as her 1.5 m3 reservoir, and at a fixed 5 ms step those
  // small ones overshot so hard that a chest ended up at a HIGHER pressure
  // than the blower feeding it — which then read as a permanent 25% sag on
  // chests that nothing was playing on.
  double smallest = 0.002;
  for (const WindCompartmentLink& link : links_) {
    if (link.massFlowKgPerSec <= 0.0) continue;
    for (Id id : {link.firstCompartmentId, link.secondCompartmentId}) {
      const int i = indexOf(id);
      if (i < 0) continue;
      const double v = order_[static_cast<size_t>(i)].volumeM3;
      const double tau = 2.0 * link.refPressureInches * v /
                         (kInchesPerKgPerM3 * link.massFlowKgPerSec);
      smallest = std::min(smallest, tau * 0.02);
    }
  }
  // A moving board (#812): its reservoir is smallest with the board down, and
  // the board bounces on its own air at sqrt(stiffness / mass). A twentieth
  // of a radian a step keeps both.
  for (const Board& b : boards_) {
    for (const WindCompartmentLink& link : links_) {
      if (link.massFlowKgPerSec <= 0.0) continue;
      if (indexOf(link.firstCompartmentId) != b.at && indexOf(link.secondCompartmentId) != b.at) continue;
      const double tau = 2.0 * link.refPressureInches * b.fixedVolumeM3 /
                         (kInchesPerKgPerM3 * link.massFlowKgPerSec);
      smallest = std::min(smallest, tau * 0.02);
    }
    const double omega = std::sqrt(kAtmInches / b.fixedVolumeM3 / b.massPerInch);
    smallest = std::min(smallest, 0.05 / omega);
  }
  // A floor, because an organ that needs a microsecond step cannot be
  // integrated under the player's hands and the honest answer is a coarser
  // model, not a hung audio thread.
  maxStep_ = std::clamp(smallest, 5.0e-6, 0.002);
}

void WindSolver::settleWith(const std::unordered_set<Id>& engagedSwitches) {
  settleSwitches(engagedSwitches);
  settleControlValves(engagedSwitches);
}

void WindSolver::settleControlValves(const std::unordered_set<Id>& engagedSwitches) {
  // The valves a continuous control opens, where their controls stand at load
  // (#1028). Alessandria's blower fills its reservoir through one; with the
  // blower off at load that valve is shut and the dump valve open, and the
  // reservoir loads empty. Air with a way out is gone; air the valves seal in
  // stays: with the blower on, the same valve is still shut at load, its
  // control not yet run up, and the sealed reservoir keeps its wind.
  const bool anyShut = std::any_of(controlValves_.begin(), controlValves_.end(),
                                   [this](const ControlValve& v) { return linkOpening_[v.link] < 1.0; });
  if (!anyShut) return;

  // Which compartments the valves as they stand join to a source or to the
  // open air.
  std::vector<char> reaches(order_.size(), 0);
  std::vector<std::vector<int>> next(order_.size());
  for (size_t k = 0; k < resolved_.size(); ++k) {
    const ResolvedLink& r = resolved_[k];
    const WindCompartmentLink& link = links_[r.link];
    if (link.valveSwitchId != 0 &&
        (engagedSwitches.count(link.valveSwitchId) != 0) != link.valveOpenWhenEngaged)
      continue;
    if (link.massFlowKgPerSec <= 0.0 || linkOpening_[k] <= 0.0) continue;
    if (r.a >= 0 && r.b >= 0) {
      next[static_cast<size_t>(r.a)].push_back(r.b);
      next[static_cast<size_t>(r.b)].push_back(r.a);
    } else if (r.a >= 0) {
      reaches[static_cast<size_t>(r.a)] = 1;
    } else if (r.b >= 0) {
      reaches[static_cast<size_t>(r.b)] = 1;
    }
  }
  std::vector<int> todo;
  for (size_t j = 0; j < order_.size(); ++j)
    if (reaches[j]) todo.push_back(static_cast<int>(j));
  while (!todo.empty()) {
    const int j = todo.back();
    todo.pop_back();
    for (const int o : next[static_cast<size_t>(j)])
      if (!reaches[static_cast<size_t>(o)]) {
        reaches[static_cast<size_t>(o)] = 1;
        todo.push_back(o);
      }
  }

  const std::vector<State> asLoaded = order_;
  const std::vector<Board> boardsAsLoaded = boards_;
  const int sweeps = lastSweeps_;
  const long long steps = lastSteps_;
  settleSolve(&engagedSwitches);  // settling_ is clear: the control valves as they stand
  const std::vector<State> standing = std::move(order_);
  order_ = asLoaded;
  boards_ = boardsAsLoaded;
  for (size_t j = 0; j < order_.size(); ++j)
    if (reaches[j]) order_[j].pressure = standing[j].pressure;
  lastSweeps_ += sweeps;
  lastSteps_ += steps;
}

void WindSolver::settleSwitches(const std::unordered_set<Id>& engagedSwitches) {
  settle(&engagedSwitches);

  // A chest the settle left without wind has its blower off at load (#1028):
  // Alessandria starts its blower from a switch the player can leave off, and
  // its Tuba chest has a motor of its own. Its working pressure is then the
  // one it has with the blower running, which the settle with every valve
  // open finds; the chest itself stays empty, so its pipes are silent until
  // the blower fills it. A chest that has wind keeps the figure it has now.
  constexpr double kNoWindInches = 1.0e-3;
  const bool anyEmpty = std::any_of(order_.begin(), order_.end(), [](const State& st) {
    return st.nominalInches < kNoWindInches;
  });
  if (!anyEmpty) return;
  const std::vector<State> asLoaded = order_;
  const std::vector<Board> boardsAsLoaded = boards_;
  const int sweeps = lastSweeps_;
  const long long steps = lastSteps_;
  settle(nullptr);
  std::vector<State> running = std::move(order_);
  order_ = asLoaded;
  boards_ = boardsAsLoaded;
  for (size_t i = 0; i < order_.size(); ++i)
    if (order_[i].nominalInches < kNoWindInches) order_[i].nominalInches = running[i].nominalInches;
  lastSweeps_ += sweeps;
  lastSteps_ += steps;
}

void WindSolver::settle(const std::unordered_set<Id>* engagedSwitches) {
  settling_ = true;
  settleSolve(engagedSwitches);
  settling_ = false;
}

void WindSolver::settleSolve(const std::unordered_set<Id>* engagedSwitches) {
  // A windchest does not declare its own pressure, and it would be wrong if it
  // did: the pressure in a chest is whatever the supply holds it at against
  // its own leaks. Nancy declares five inches at the blower and nothing
  // anywhere else, and gives every chest a deliberate leak to the open air, so
  // the working pressure is a balance and has to be solved for.
  //
  // Integrated rather than flooded. A flood — "no flow means no pressure
  // difference, so everything sits at the source" — is exact only for a
  // network with no leaks, and every real organ leaks on purpose. Assuming
  // otherwise put every chest at the blower's five inches and then read the
  // difference as a sixty percent sag.
  if (order_.empty()) return;

  for (State& st : order_) {
    st.demandKgPerSec = 0.0;
    st.velocity = 0.0;
    // The nominal is what is being solved for; it must not feed back into the
    // solve through the pipe-draw term, which is zero here anyway.
    st.nominalInches = 0.0;
  }

  lastSweeps_ = 0;
  lastSteps_ = 0;
  if (directSettle_ && solveSteady(engagedSwitches)) {
    settleBoards(engagedSwitches);
    for (State& st : order_) {
      st.nominalInches = st.pressure;
      st.velocity = 0.0;
    }
    debtSeconds_ = 0.0;
    return;
  }

  // Long enough for the largest reservoir to fill through the narrowest valve,
  // and it stops as soon as it stops moving.
  constexpr double kSettleSeconds = 120.0;
  const int steps = static_cast<int>(kSettleSeconds / maxStep_);
  // Converged when nothing SWINGS, not when the endpoints of a window happen
  // to agree. Two tanks joined by an orifice can ring, and a window that only
  // compares its ends reads a full cycle as perfect stillness — which then
  // showed up as a four percent "sag" the moment the organ started playing,
  // because the ripple was still going and the measurement caught its trough.
  std::vector<double> lo(order_.size(), 0.0), hi(order_.size(), 0.0);
  constexpr int kWindow = 1024;
  readValves(engagedSwitches);
  for (int i = 0; i < steps; ++i) {
    const int phase = i % kWindow;
    for (size_t j = 0; j < order_.size(); ++j) {
      const double p = order_[j].pressure;
      if (phase == 0) {
        lo[j] = hi[j] = p;
      } else {
        lo[j] = std::min(lo[j], p);
        hi[j] = std::max(hi[j], p);
      }
    }
    integrate(maxStep_, engagedSwitches);
    ++lastSteps_;
    if (phase == kWindow - 1) {
      double swing = 0.0;
      for (size_t j = 0; j < order_.size(); ++j)
        swing = std::max(swing, hi[j] - lo[j]);
      // A ten-thousandth of an inch is far below anything audible and far
      // below the sag a single pipe causes.
      if (swing < 1.0e-4) break;
    }
  }
  settleBoards(engagedSwitches);

  for (State& st : order_) {
    st.nominalInches = st.pressure;
    st.velocity = 0.0;
  }
  debtSeconds_ = 0.0;
}

void WindSolver::settleBoards(const std::unordered_set<Id>* engagedSwitches) {
  // The balance with the boards in place (#812): the steady solve above
  // leaves every valve open and every board where it was, and the boards
  // then find where their weight holds the air. Integrated, each board
  // damped to critical, until neither a pressure nor a board moves.
  if (boards_.empty()) return;
  settlingBoards_ = true;
  readValves(engagedSwitches);
  constexpr double kSettleSeconds = 60.0;
  const long long steps = static_cast<long long>(kSettleSeconds / maxStep_);
  constexpr int kWindow = 1024;
  std::vector<double> lo(order_.size() + boards_.size()), hi(lo.size());
  // The integrator has a floor of its own: at the smallest step a small
  // compartment on a wide link chatters around its balance by a few
  // ten-thousandths of an inch for ever, at rest as in play. Pinerolo's blower
  // outlet does, and waiting for it to fall below the limit below ran the
  // settle to its end, twelve million steps and seconds of load time. Settled
  // also when no window's middle has moved over the last few windows and the
  // swing has stopped shrinking: the boards are damped to critical here, so
  // what is left is the step's own chatter.
  constexpr int kLookBack = 8;
  std::vector<std::vector<double>> mids;
  std::vector<double> swings;
  for (long long i = 0; i < steps; ++i) {
    const int phase = static_cast<int>(i % kWindow);
    for (size_t j = 0; j < lo.size(); ++j) {
      // Pressures in inches; a board's height scaled so that the limit below
      // is a hundred-thousandth of its travel.
      const double v = j < order_.size()
                           ? order_[j].pressure
                           : boards_[j - order_.size()].u / boards_[j - order_.size()].travelM3 * 10.0;
      if (phase == 0) lo[j] = hi[j] = v;
      lo[j] = std::min(lo[j], v);
      hi[j] = std::max(hi[j], v);
    }
    integrate(maxStep_, engagedSwitches);
    ++lastSteps_;
    if (phase == kWindow - 1) {
      double swing = 0.0;
      for (size_t j = 0; j < lo.size(); ++j) swing = std::max(swing, hi[j] - lo[j]);
      if (swing < 1.0e-4) break;
      std::vector<double> mid(lo.size());
      for (size_t j = 0; j < lo.size(); ++j) mid[j] = 0.5 * (lo[j] + hi[j]);
      mids.push_back(std::move(mid));
      swings.push_back(swing);
      const size_t n = swings.size();
      if (n > kLookBack && swing < 1.0e-2 && swing >= 0.95 * swings[n - 1 - kLookBack]) {
        double drift = 0.0;
        for (size_t j = 0; j < lo.size(); ++j)
          for (size_t k = n - 1 - kLookBack; k < n - 1; ++k)
            drift = std::max(drift, std::abs(mids[n - 1][j] - mids[k][j]));
        if (drift < 1.0e-5) break;
      }
    }
  }
  for (Board& b : boards_) b.uDot = 0.0;
  settlingBoards_ = false;
}

bool WindSolver::solveSteady(const std::unordered_set<Id>* engagedSwitches) {
  // At the working pressure nothing flows on balance: what a compartment takes
  // in through its links equals what it lets out. The flow through a link
  // only ever grows with the pressure difference across it, so a
  // compartment's net inflow only ever falls as its own pressure rises, and
  // its balance point is unique and lies between the lowest and highest of
  // its neighbours. Found one compartment at a time, by bisection -- which
  // cannot overshoot -- and swept until nothing moves. A Newton step would be
  // faster per compartment, but the square-root flow law has an infinite
  // slope at zero difference, which is exactly where a settled leak-free
  // chest sits.
  struct End {
    int other = -1;       // position in order_, or -1 for a fixed pressure
    double fixed = 0.0;
    double flowAtRef = 0.0, refDiff = 0.0;
  };
  std::vector<std::vector<End>> ends(order_.size());
  for (size_t k = 0; k < resolved_.size(); ++k) {
    const ResolvedLink& r = resolved_[k];
    const WindCompartmentLink& link = links_[r.link];
    if (engagedSwitches != nullptr && link.valveSwitchId != 0) {
      const bool on = engagedSwitches->count(link.valveSwitchId) != 0;
      if (on != link.valveOpenWhenEngaged) continue;  // valve shut
    }
    // A control valve where its control stands, outside a settle (#1028).
    const double opening = !settling_ && k < linkOpening_.size() ? linkOpening_[k] : 1.0;
    const double flow = link.massFlowKgPerSec * opening;
    if (flow <= 0.0 || link.refPressureInches <= 0.0) continue;
    if (r.a >= 0)
      ends[static_cast<size_t>(r.a)].push_back({r.b, r.fixedB, flow, link.refPressureInches});
    if (r.b >= 0)
      ends[static_cast<size_t>(r.b)].push_back({r.a, r.fixedA, flow, link.refPressureInches});
  }

  std::vector<double> start(order_.size());
  for (size_t j = 0; j < order_.size(); ++j) start[j] = order_[j].pressure;
  const auto pressureAt = [this](const End& e) {
    return e.other >= 0 ? order_[static_cast<size_t>(e.other)].pressure : e.fixed;
  };

  constexpr int kMaxSweeps = 20000;
  constexpr double kStill = 1.0e-7;  // inches: far below anything the model resolves
  int sweeps = kMaxSweeps;
  bool finite = true;
  for (int sweep = 1; sweep <= kMaxSweeps; ++sweep) {
    double moved = 0.0;
    for (size_t j = 0; j < order_.size(); ++j) {
      const auto& mine = ends[j];
      if (mine.empty()) continue;  // nothing reaches it: it stays as it is
      double lo = pressureAt(mine.front()), hi = lo;
      for (const End& e : mine) {
        lo = std::min(lo, pressureAt(e));
        hi = std::max(hi, pressureAt(e));
      }
      double p = lo;
      if (hi - lo > 1.0e-12) {
        const auto inflow = [&](double at) {
          double sum = 0.0;
          for (const End& e : mine) sum += orificeFlow(e.flowAtRef, e.refDiff, pressureAt(e) - at);
          return sum;
        };
        for (int it = 0; it < 64 && hi - lo > 1.0e-12; ++it) {
          const double mid = 0.5 * (lo + hi);
          if (inflow(mid) > 0.0) lo = mid;  // still filling: the balance is higher
          else hi = mid;
        }
        p = 0.5 * (lo + hi);
      }
      moved = std::max(moved, std::abs(p - order_[j].pressure));
      order_[j].pressure = p;
    }
    if (!std::isfinite(moved)) {
      finite = false;
      break;
    }
    if (moved < kStill) {
      sweeps = sweep;
      break;
    }
  }

  // A small step per sweep is not always the balance. A block of compartments
  // joined by wide links, fed and drained only through narrow ones, moves as
  // one body a hair per sweep while still far from where it settles. Newton's
  // method on the whole network finishes it. The balance is the lowest point
  // of a convex energy -- each link's flow only grows with its pressure
  // difference -- so a Newton step taken to the lowest point along it always
  // makes progress. Where the sweeps have settled, the first Newton step is
  // below kStill and their answer stands unchanged.
  std::vector<int> var(order_.size(), -1);
  std::vector<size_t> at;
  for (size_t j = 0; j < order_.size(); ++j)
    if (!ends[j].empty()) {
      var[j] = static_cast<int>(at.size());
      at.push_back(j);
    }
  const size_t n = at.size();
  constexpr size_t kMaxNewton = 1000;  // a dense solve; organs have tens to a few hundred
  if (finite && n > 0 && n <= kMaxNewton) {
    std::vector<double> h(n * n), r(n), step(n), base(n);
    bool settled = false;
    for (int it = 0; it < 100; ++it) {
      std::fill(h.begin(), h.end(), 0.0);
      for (size_t v = 0; v < n; ++v) {
        const size_t j = at[v];
        double net = 0.0;
        for (const End& e : ends[j]) {
          const double d = pressureAt(e) - order_[j].pressure;
          net += orificeFlow(e.flowAtRef, e.refDiff, d);
          // The flow's slope, held finite where the difference vanishes.
          const double slope =
              e.flowAtRef / (2.0 * std::sqrt(e.refDiff * std::max(std::abs(d), 1.0e-12)));
          h[v * n + v] += slope;
          if (e.other >= 0) h[v * n + static_cast<size_t>(var[static_cast<size_t>(e.other)])] -= slope;
        }
        r[v] = net;
      }
      // A block reached by no fixed pressure has no single balance; a touch
      // on the diagonal keeps the solve defined.
      double top = 0.0;
      for (size_t v = 0; v < n; ++v) top = std::max(top, h[v * n + v]);
      for (size_t v = 0; v < n; ++v) h[v * n + v] += 1.0e-12 * top;
      bool factored = true;  // Cholesky, in place in the lower triangle
      for (size_t c = 0; c < n && factored; ++c) {
        double d = h[c * n + c];
        for (size_t k = 0; k < c; ++k) d -= h[c * n + k] * h[c * n + k];
        if (!(d > 0.0)) {
          factored = false;
          break;
        }
        d = std::sqrt(d);
        h[c * n + c] = d;
        for (size_t row = c + 1; row < n; ++row) {
          double s = h[row * n + c];
          for (size_t k = 0; k < c; ++k) s -= h[row * n + k] * h[c * n + k];
          h[row * n + c] = s / d;
        }
      }
      if (!factored) break;
      for (size_t row = 0; row < n; ++row) {
        double s = r[row];
        for (size_t k = 0; k < row; ++k) s -= h[row * n + k] * step[k];
        step[row] = s / h[row * n + row];
      }
      for (size_t row = n; row-- > 0;) {
        double s = step[row];
        for (size_t k = row + 1; k < n; ++k) s -= h[k * n + row] * step[k];
        step[row] = s / h[row * n + row];
      }
      double longest = 0.0;
      for (size_t v = 0; v < n; ++v) longest = std::max(longest, std::abs(step[v]));
      if (!std::isfinite(longest)) break;
      if (longest < kStill) {
        settled = true;
        break;
      }
      // The step's length: the lowest point of the energy along it, where the
      // net inflow no longer has a part along the step. That part only falls
      // as the step lengthens, so bisection finds it. Near a vanishing
      // difference the full step lands twice as far as the balance, and this
      // halves it.
      for (size_t v = 0; v < n; ++v) base[v] = order_[at[v]].pressure;
      const auto along = [&](double t) {
        for (size_t v = 0; v < n; ++v) order_[at[v]].pressure = base[v] + t * step[v];
        double sum = 0.0;
        for (size_t v = 0; v < n; ++v) {
          const size_t j = at[v];
          double net = 0.0;
          for (const End& e : ends[j]) net += orificeFlow(e.flowAtRef, e.refDiff, pressureAt(e) - order_[j].pressure);
          sum += net * step[v];
        }
        return sum;
      };
      double lo = 0.0, hi = 1.0;
      while (along(hi) > 0.0 && hi < 64.0) {
        lo = hi;
        hi *= 2.0;
      }
      for (int b = 0; b < 60 && (hi - lo) * longest > 1.0e-13; ++b) {
        const double mid = 0.5 * (lo + hi);
        if (along(mid) > 0.0) lo = mid;
        else hi = mid;
      }
      const double t = 0.5 * (lo + hi);
      for (size_t v = 0; v < n; ++v) order_[at[v]].pressure = base[v] + t * step[v];
    }
    if (settled) {
      lastSweeps_ = sweeps;
      return true;
    }
  }
  // Did not settle: put everything back and let the integration do it.
  for (size_t j = 0; j < order_.size(); ++j) order_[j].pressure = start[j];
  return false;
}

int WindSolver::indexOf(Id compartmentId) const {
  const auto it = index_.find(compartmentId);
  return it == index_.end() ? -1 : it->second;
}

Id WindSolver::compartmentForPipe(const Pipe& pipe) const {
  if (pipe.windSourceCompartmentId != 0) return pipe.windSourceCompartmentId;
  const auto it = pipeCompartment_.find(pipe.pipeId);
  return it == pipeCompartment_.end() ? 0 : it->second;
}

void WindSolver::clearDemand() {
  for (State& st : order_) st.demandKgPerSec = 0.0;
}

void WindSolver::addDemandDirect(const std::vector<Id>& compartments,
                                 const std::vector<float>& kgPerSec) {
  const size_t n = std::min(compartments.size(), kgPerSec.size());
  for (size_t i = 0; i < n; ++i) {
    const int idx = indexOf(compartments[i]);
    if (idx < 0) continue;
    order_[static_cast<size_t>(idx)].demandKgPerSec += kgPerSec[i];
  }
}

bool WindSolver::advance(double dtSeconds, const EngineSwitch& sw,
                         const std::unordered_set<Id>& engagedSwitches) {
  running_ = !sw.simpleWavOnly && sw.enableWindModel && !order_.empty();
  if (!running_) {
    for (State& st : order_) {
      st.pressure = st.nominalInches;
      st.velocity = 0.0;
    }
    return false;
  }
  if (dtSeconds <= 0.0) return true;

  // The links' flow drifts once a block (#807).
  if (anyLinkWalk_)
    for (size_t k = 0; k < linkWalks_.size(); ++k)
      if (linkWalks_[k].on()) linkFlowMul_[k] = linkWalks_[k].step(dtSeconds);

  // Step at a FIXED rate, carrying whatever is left over to the next block.
  //
  // The obvious alternative — divide the block into equal sub-steps — makes
  // the step size depend on the host's buffer, and this system is oscillatory
  // enough that the ripple's amplitude goes with it. Settled at one step size
  // and run at another, the same organ with nobody playing showed a four
  // percent sag that appeared and disappeared with the buffer setting. The
  // physics must not know what the buffer size is.
  debtSeconds_ += dtSeconds;
  int steps = static_cast<int>(debtSeconds_ / maxStep_);
  // Bounded: the audio thread cannot spend an unbounded time here whatever the
  // file or a stalled host says. Dropping the excess loses a little simulated
  // time, which is the right thing to lose.
  if (steps > 256) {
    steps = 256;
    debtSeconds_ = 0.0;
  } else {
    debtSeconds_ -= steps * maxStep_;
  }
  if (steps <= 0) return true;
  const double dt = maxStep_;

  readValves(&engagedSwitches);
  driftFlow_ = anyLinkWalk_;
  for (int step = 0; step < steps; ++step) integrate(dt, &engagedSwitches);
  driftFlow_ = false;
  return true;
}

void WindSolver::readValves(const std::unordered_set<Id>* engagedSwitches) {
  valveOpen_.assign(resolved_.size(), 1);
  if (engagedSwitches == nullptr) return;
  for (size_t k = 0; k < resolved_.size(); ++k) {
    const WindCompartmentLink& link = links_[resolved_[k].link];
    if (link.valveSwitchId == 0) continue;
    const bool on = engagedSwitches->count(link.valveSwitchId) != 0;
    valveOpen_[k] = on == link.valveOpenWhenEngaged ? 1 : 0;
  }
}

void WindSolver::integrate(double dt,
                           const std::unordered_set<Id>* engagedSwitches) {
  // Out through the pipes standing on each chest. A chest that has already
  // sagged feeds its pipes less, which is the feedback that keeps this stable.
  for (State& st : order_) {
    if (st.demandKgPerSec <= 0.0) continue;
    const double ratio = st.nominalInches > 0.0
                             ? std::max(0.0, st.pressure / st.nominalInches)
                             : 1.0;
    const double draw = st.demandKgPerSec * std::sqrt(ratio);
    st.pressure -= kInchesPerKgPerM3 * draw * dt / st.volumeM3;
  }

  // In through the links from other compartments. A null switch set means
  // "every valve open", which is how the system is settled at load: the
  // working pressure is the organ RUNNING, not the organ switched off.
  (void)engagedSwitches;  // read once for the run of steps: readValves()
  for (size_t k = 0; k < resolved_.size(); ++k) {
    const ResolvedLink& r = resolved_[k];
    const WindCompartmentLink& link = links_[r.link];
    if (k < valveOpen_.size() && !valveOpen_[k]) continue; // valve shut
    // A valve driven by a continuous control is left OPEN. On a real organ
    // that control is the reservoir's own board extension closing the intake
    // as it fills — the mechanism by which a reservoir regulates itself — and
    // we do not solve the board's position, so we do not know where the valve
    // is. Open is the answer that leaves the organ winded; shut would make it
    // silent, and inventing a position would invent the regulation with it.
    // See the header on what is and is not modelled.
    const int ia = r.a;
    const int ib = r.b;
    const double pa = ia >= 0 ? order_[static_cast<size_t>(ia)].pressure : r.fixedA;
    const double pb = ib >= 0 ? order_[static_cast<size_t>(ib)].pressure : r.fixedB;
    double mul = driftFlow_ && k < linkFlowMul_.size() ? linkFlowMul_[k] : 1.0;
    // A valve its reservoir's board drives (#812), open as far as the board's
    // height and the set's linkage say.
    if (!boardValve_.empty() && boardValve_[k] >= 0) mul *= boardValveOpening(k);
    // A valve its continuous control opens, as far as the control reads. Open
    // while settling: the working pressure is the organ's with its blower on.
    if (!settling_ && k < linkOpening_.size()) mul *= linkOpening_[k];
    double flow =
        orificeFlow(link.massFlowKgPerSec * mul, link.refPressureInches, pa - pb);
    // No more air in one step than brings the two sides to one pressure. The
    // square-root law's slope has no bound as the difference closes, so an
    // explicit step near balance carries the pair past it, and a small chest
    // fed hard chattered about its supply: under a chord a 1-litre chest read
    // 35% above its working pressure (#1121). Where the step is short enough
    // this never binds.
    const double ga = ia >= 0 ? kInchesPerKgPerM3 * dt / order_[static_cast<size_t>(ia)].volumeM3 : 0.0;
    const double gb = ib >= 0 ? kInchesPerKgPerM3 * dt / order_[static_cast<size_t>(ib)].volumeM3 : 0.0;
    if (ga + gb > 0.0) {
      const double most = std::abs(pa - pb) / (ga + gb);
      flow = std::clamp(flow, -most, most);
    }
    if (ia >= 0) {
      State& a = order_[static_cast<size_t>(ia)];
      a.pressure -= kInchesPerKgPerM3 * flow * dt / a.volumeM3;
    }
    if (ib >= 0) {
      State& b = order_[static_cast<size_t>(ib)];
      b.pressure += kInchesPerKgPerM3 * flow * dt / b.volumeM3;
    }
  }
  if (!boards_.empty()) stepBoards(dt);

  // A wind system cannot go negative, and a number that has gone non-finite
  // must not reach the audio path — it would silence the organ permanently
  // rather than for one block.
  for (State& st : order_) {
    if (!std::isfinite(st.pressure) || !std::isfinite(st.velocity)) {
      st.pressure = st.nominalInches;
      st.velocity = 0.0;
    }
    // A guard against a runaway, not a model. The ceiling has to be absolute
    // rather than a multiple of the nominal, because while settling the
    // nominal is deliberately zero — deriving the ceiling from it pinned the
    // biggest reservoir at the ceiling and left it reading a permanent 48%
    // sag afterwards. No organ runs above about twenty inches.
    constexpr double kAbsurdInches = 100.0;
    st.pressure = std::clamp(st.pressure, 0.0,
                             std::max(st.nominalInches * 4.0, kAbsurdInches));
  }
}

double WindSolver::pressureFor(Id compartmentId) const {
  const int i = indexOf(compartmentId);
  if (i >= 0) return order_[static_cast<size_t>(i)].pressure;
  if (model_ != nullptr) {
    const auto it = model_->wind.find(compartmentId);
    if (it != model_->wind.end()) return it->second.defaultPressureInches;
  }
  return 0.0;
}

double WindSolver::demandFor(Id compartmentId) const {
  const int i = indexOf(compartmentId);
  return i < 0 ? 0.0 : order_[static_cast<size_t>(i)].demandKgPerSec;
}

double WindSolver::nominalFor(Id compartmentId) const {
  const int i = indexOf(compartmentId);
  if (i >= 0) return order_[static_cast<size_t>(i)].nominalInches;
  if (model_ != nullptr) {
    const auto it = model_->wind.find(compartmentId);
    if (it != model_->wind.end()) return it->second.defaultPressureInches;
  }
  return 0.0;
}

double WindSolver::sagFor(Id compartmentId) const {
  const int i = indexOf(compartmentId);
  if (i < 0) return 0.0;
  const State& st = order_[static_cast<size_t>(i)];
  if (st.nominalInches <= 0.0) return 0.0;
  return (st.pressure - st.nominalInches) / st.nominalInches;
}

WindSolver::Mod WindSolver::modFor(Id compartmentId) const {
  Mod m;
  if (!running_) return m;
  const double sag = sagFor(compartmentId);
  if (sag == 0.0) return m;
  m.ampMul = std::clamp(1.0 + kAmpPerFractionalSag * sag, 0.25, 2.0);
  // Below half its working pressure a pipe runs out of wind to speak with,
  // down to silence in an empty chest (#1028): a blower switched off leaves
  // the organ mute once the reservoir drains. The line meets the curve above
  // at half pressure, so an ordinary sag of a few percent is untouched.
  const double fraction = 1.0 + sag;
  if (fraction < kSpeechFraction) {
    m.supply = std::max(0.0, fraction) / kSpeechFraction;
    m.ampMul = m.supply * (1.0 + kAmpPerFractionalSag * (kSpeechFraction - 1.0));
  }
  // Semitones, then a ratio: a sag flattens the organ, and the amount is
  // deliberately conservative — see the header.
  const double semitones = kPitchPerFractionalSag * sag;
  m.pitchRatio = std::pow(2.0, semitones / 12.0);
  return m;
}

} // namespace mp
