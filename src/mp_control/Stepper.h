// The registration sequencer: one button that walks the generals in order.
//
// Unlike couplers, pistons and the crescendo, this is NOT wired in the organ
// file. Lemmer declares a combination called "Registration sequencer template"
// and stops there; nothing on its console advances anything. The sequencer is
// the PLAYER's, provided by the software, and a player maps it to whatever
// their console sends — which is exactly why it belongs here rather than in
// the loader.
//
// What it walks is the organ's own general combinations, in playing order.
// Hauptwerk numbers them in the 1xx family (101 is "General 01"), and the x00
// member of a family is its cancel, so that one is skipped: stepping onto a
// cancel would wipe the registration rather than change it.
//
// The stepper does not touch switches. It answers "which combination now", and
// the caller fires it through the ordinary piston path, so a frame recalled by
// the sequencer behaves exactly like the same piston pressed by hand.
#pragma once
#include "../mp_core/OrganModel.h"

#include <cstddef>
#include <vector>

namespace mp {

class Stepper {
public:
  void reset(const OrganModel& model);

  // Frames are 1-based, the way an organist counts them. 0 means "before the
  // first", which is where the sequencer sits until it is used.
  int frame() const { return frame_; }
  size_t frameCount() const { return frames_.size(); }
  bool empty() const { return frames_.empty(); }

  // Move, and return the combination to fire. 0 when there is nothing to fire:
  // an organ with no generals, or an attempt to step past either end.
  //
  // Deliberately does NOT wrap. A sequencer that rolls from the last frame
  // back to the first will do it in performance, at the loudest possible
  // moment, and no organist wants that.
  Id next();
  Id prev();
  // Jump straight to a frame, for a console that sends a number.
  Id gotoFrame(int oneBased);

  // The combination at the current frame, without moving — this is what a
  // capture writes into.
  Id current() const;

  // Back to the beginning, without firing anything.
  void rewind() { frame_ = 0; }

private:
  std::vector<Id> frames_; // general combinations, in playing order
  int frame_ = 0;
};

} // namespace mp
