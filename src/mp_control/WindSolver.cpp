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

  chooseStep();
  settle();
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
  // A floor, because an organ that needs a microsecond step cannot be
  // integrated under the player's hands and the honest answer is a coarser
  // model, not a hung audio thread.
  maxStep_ = std::clamp(smallest, 5.0e-6, 0.002);
}

void WindSolver::settleWith(const std::unordered_set<Id>& engagedSwitches) {
  settle(&engagedSwitches);
}

void WindSolver::settle(const std::unordered_set<Id>* engagedSwitches) {
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
    if (phase == kWindow - 1) {
      double swing = 0.0;
      for (size_t j = 0; j < order_.size(); ++j)
        swing = std::max(swing, hi[j] - lo[j]);
      // A ten-thousandth of an inch is far below anything audible and far
      // below the sag a single pipe causes.
      if (swing < 1.0e-4) break;
    }
  }

  for (State& st : order_) {
    st.nominalInches = st.pressure;
    st.velocity = 0.0;
  }
  debtSeconds_ = 0.0;
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

void WindSolver::addDemand(const Pipe& pipe) {
  const int i = indexOf(compartmentForPipe(pipe));
  if (i < 0) return;
  State& st = order_[static_cast<size_t>(i)];
  // What the pipe would draw at its reference pressure. The pressure it is
  // actually seeing is applied in advance(), so that a chest already sagging
  // draws less — which is the feedback that stops the model running away.
  st.demandKgPerSec += pipe.windMassFlowKgPerSec;
  // Nancy's flow rates are per pipe and honest; an organ that declares none
  // still has to sag, or the model is decorative. Fall back to a rate that
  // makes a full tutti matter and a single stop not.
  if (pipe.windMassFlowKgPerSec <= 0.0) st.demandKgPerSec += 0.0005;
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

  for (int step = 0; step < steps; ++step) integrate(dt, &engagedSwitches);
  return true;
}

void WindSolver::integrate(double dt,
                           const std::unordered_set<Id>* engagedSwitches) {
  const auto pressureOf = [this](Id id) {
    const int i = indexOf(id);
    if (i >= 0) return order_[static_cast<size_t>(i)].pressure;
    // Not modelled: an infinite compartment sits at whatever it declares, and
    // that is exactly what makes it a boundary condition. The blower's intake
    // and the open air are both this.
    if (model_ != nullptr) {
      const auto it = model_->wind.find(id);
      if (it != model_->wind.end()) return it->second.defaultPressureInches;
    }
    return 0.0;
  };

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
  for (const WindCompartmentLink& link : links_) {
    if (engagedSwitches != nullptr && link.valveSwitchId != 0) {
      const bool on = engagedSwitches->count(link.valveSwitchId) != 0;
      if (on != link.valveOpenWhenEngaged) continue; // valve shut
    }
    // A valve driven by a continuous control is left OPEN. On a real organ
    // that control is the reservoir's own board extension closing the intake
    // as it fills — the mechanism by which a reservoir regulates itself — and
    // we do not solve the board's position, so we do not know where the valve
    // is. Open is the answer that leaves the organ winded; shut would make it
    // silent, and inventing a position would invent the regulation with it.
    // See the header on what is and is not modelled.
    const double pa = pressureOf(link.firstCompartmentId);
    const double pb = pressureOf(link.secondCompartmentId);
    const double flow =
        orificeFlow(link.massFlowKgPerSec, link.refPressureInches, pa - pb);
    const int ia = indexOf(link.firstCompartmentId);
    const int ib = indexOf(link.secondCompartmentId);
    if (ia >= 0) {
      State& a = order_[static_cast<size_t>(ia)];
      a.pressure -= kInchesPerKgPerM3 * flow * dt / a.volumeM3;
    }
    if (ib >= 0) {
      State& b = order_[static_cast<size_t>(ib)];
      b.pressure += kInchesPerKgPerM3 * flow * dt / b.volumeM3;
    }
  }

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
  // Semitones, then a ratio: a sag flattens the organ, and the amount is
  // deliberately conservative — see the header.
  const double semitones = kPitchPerFractionalSag * sag;
  m.pitchRatio = std::pow(2.0, semitones / 12.0);
  return m;
}

} // namespace mp
