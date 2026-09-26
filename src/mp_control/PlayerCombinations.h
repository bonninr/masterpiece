// The player's own pistons: generals, divisionals, cancels and a stepper that
// exist on every organ, whatever its file declares.
//
// An organ file declares the pistons its builder put on the console, and
// CombinationSystem runs those. Plenty of organs declare few or none, or
// declare them and never draw them, and then a player has nowhere to keep a
// registration. These are the program's, the way a console's setter system
// belongs to the console rather than to the pipework: the same on every organ,
// captured with the same setter, saved per organ in the player's own data.
//
// What they capture is the registration and nothing else -- the stops, the
// couplers and the tremulants. Not the blower, not a noise, not a page switch:
// a general cancel that stopped the blower would be a very bad piston.
//
// A divisional covers one division: its stops, the couplers that feed it, and
// the tremulant that shakes it. A coupler or tremulant whose division the file
// does not make clear belongs to the generals only.
//
// JUCE-free and engine-free: it decides what a piston means and hands back the
// changes, the way CombinationSystem does, and the processor applies them.
#pragma once
#include "../mp_core/OrganModel.h"

#include <functional>
#include <string>
#include <vector>

namespace mp {

class PlayerCombinations {
public:
  // One thing a registration can hold. A stop is recalled as a stop, so it
  // moves its knob the way the stop list does; a coupler or tremulant is its
  // player-facing switch.
  enum class ElementKind { Stop, Switch };
  struct Element {
    ElementKind kind = ElementKind::Stop;
    Id id = 0;
    Id divisionId = 0;  // 0: reached by generals only
    std::string name;
  };
  struct Division {
    Id divisionId = 0;
    std::string name;
  };
  struct Change {
    ElementKind kind = ElementKind::Stop;
    Id id = 0;
    bool engage = false;
  };

  // A stored registration: one flag per element, in elements() order.
  // Nothing drawn with `set` true is a registration of nothing, which recalls
  // as a cancel; `set` false is a piston nobody has captured, which does
  // nothing at all rather than silently clearing the organ.
  //
  // Every registration is sized once, at reset. A piston pressed on a console
  // captures on the audio thread while the window reads the same pistons on
  // the message thread, so nothing here may reallocate after load.
  struct Registration {
    bool set = false;
    std::vector<char> drawn;
  };

  static constexpr int kDefaultGenerals = 10;
  static constexpr int kDefaultDivisionals = 5;
  static constexpr int kMaxGenerals = 100;
  static constexpr int kMaxDivisionals = 20;
  static constexpr int kMaxFrames = 999;

  // What the organ offers to register. `playerSwitch` maps an internal switch
  // to the one a player moves (identity when there is none).
  static std::vector<Element> collect(const OrganModel& model,
                                      const std::function<Id(Id)>& playerSwitch);
  // The divisions that get divisionals: those holding at least one element,
  // in the organ's order (pedal first, then by manual number).
  static std::vector<Division> divisionsOf(const OrganModel& model,
                                           const std::vector<Element>& elements);

  // Sized from the start, so the window can ask about pistons before any
  // organ has loaded.
  PlayerCombinations() { reset({}, {}); }
  void reset(std::vector<Element> elements, std::vector<Division> divisions);
  const std::vector<Element>& elements() const { return elements_; }
  const std::vector<Division>& divisions() const { return divisions_; }

  int generalCount() const { return generalCount_; }
  int divisionalCount() const { return divisionalCount_; }
  void setGeneralCount(int n);
  void setDivisionalCount(int n);

  // --- pistons ----------------------------------------------------------
  // `isEngaged` reads the live console for an element. Captures return false
  // for a piston that does not exist.
  using Reader = std::function<bool(const Element&)>;
  bool captureGeneral(int n, const Reader& isEngaged);
  bool captureDivisional(Id divisionId, int n, const Reader& isEngaged);
  void recallGeneral(int n, std::vector<Change>& out) const;
  void recallDivisional(Id divisionId, int n, std::vector<Change>& out) const;
  void generalCancel(std::vector<Change>& out) const;
  void divisionalCancel(Id divisionId, std::vector<Change>& out) const;

  bool generalSet(int n) const;
  bool divisionalSet(Id divisionId, int n) const;
  // The piston whose registration was recalled or captured last, which is the
  // one a console lights. 0 when the registration has since moved on.
  int litGeneral() const { return litGeneral_; }
  int litDivisional(Id divisionId) const;
  // Any hand-drawn change unlights everything: the pistons no longer describe
  // what is drawn.
  void registrationMoved();
  // Light the piston just recalled.
  void lightGeneral(int n);
  void lightDivisional(Id divisionId, int n);

  // --- stepper ----------------------------------------------------------
  // Frames 1..kMaxFrames. Stepping forward stops after the last frame that
  // holds anything, so a piece's sequence does not run on into empties --
  // except while capturing, when stepping on is how a sequence gets longer.
  int frame() const { return frame_; }
  int lastUsedFrame() const;
  bool frameSet(int oneBased) const;
  bool stepNext(bool capturing, const Reader& isEngaged, std::vector<Change>& out);
  bool stepPrev(bool capturing, const Reader& isEngaged, std::vector<Change>& out);
  bool gotoFrame(int oneBased, bool capturing, const Reader& isEngaged,
                 std::vector<Change>& out);
  // Make room for a frame before the current one, or take the current one
  // out; later frames move with it, as they do on a sequencer.
  bool insertFrame();
  bool deleteFrame();
  void rewind() { frame_ = 0; }

  // --- persistence ------------------------------------------------------
  std::string toText() const;
  bool fromText(const std::string& text);
  void clearAll();

  static std::string keyOf(const Element& e);

private:
  std::vector<Element> elements_;
  std::vector<Division> divisions_;
  int generalCount_ = kDefaultGenerals;
  int divisionalCount_ = kDefaultDivisionals;
  // Index 0 unused, so a piston's number is its index.
  std::vector<Registration> generals_;
  std::vector<Registration> frames_;
  // divisionIndex * (kMaxDivisionals + 1) + piston
  std::vector<Registration> divisionals_;
  std::vector<int> litDivisional_;  // by division index
  int frame_ = 0;
  int litGeneral_ = 0;

  int divisionIndex(Id divisionId) const;
  Registration* divisional(Id divisionId, int n);
  const Registration* divisional(Id divisionId, int n) const;
  void captureInto(Registration& r, Id divisionScope, bool all,
                   const Reader& isEngaged) const;
  void recallInto(const Registration& r, Id divisionScope, bool all,
                  std::vector<Change>& out) const;
  bool fireFrame(int oneBased, bool capturing, const Reader& isEngaged,
                 std::vector<Change>& out);
};

} // namespace mp
