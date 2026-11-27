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
#include <map>
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
    // A bass or a melody coupler: a key action that carries the lowest or
    // the highest key held.
    bool bassOrMelody = false;
    // What a cancel does to it, from its switch: 0 off, 1 drawn, -1 left.
    int cancelState = 0;
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
  //
  // `onConsole` says whether a player can reach an element. On a drawn
  // console only what has a drawstop is registration: sets keep machinery in
  // stops and switches nobody draws -- Vasvar sounds its key action through
  // five stops called DivisionKeyAction_01..05, always on -- and a general
  // cancel must not reach them. Null takes everything, which is right for an
  // organ with no console artwork, where the stop list is the console.
  static std::vector<Element> collect(const OrganModel& model,
                                      const std::function<Id(Id)>& playerSwitch,
                                      const std::function<bool(const Element&)>& onConsole = nullptr);
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

  // Whether pistons and sequencer steps leave the bass and melody couplers
  // as they are: not stored, not recalled, as on consoles where these belong
  // to the player's hands. The cancels still push them in. Off by default.
  void setLeaveBassAndMelody(bool on) { leaveBassAndMelody_ = on; }
  bool leaveBassAndMelody() const { return leaveBassAndMelody_; }

  int generalCount() const { return generalCount_; }
  int divisionalCount() const { return divisionalCount_; }
  void setGeneralCount(int n);
  void setDivisionalCount(int n);
  // The general count one step up (`direction` 1) or down (-1): by one up
  // to ten, so a console with six or eight generals has exactly those, and
  // by whole rows of ten above that.
  static int steppedGeneralCount(int count, int direction);

  // --- pistons ----------------------------------------------------------
  // `isEngaged` reads the live console for an element. Captures return false
  // for a piston that does not exist.
  using Reader = std::function<bool(const Element&)>;
  bool captureGeneral(int n, const Reader& isEngaged);
  bool captureDivisional(Id divisionId, int n, const Reader& isEngaged);
  // The same, from elsewhere (a GrandOrgue settings file): stored without
  // lighting the piston or moving the sequencer. A general beyond the count
  // shown raises the count.
  bool storeGeneral(int n, const Reader& isEngaged);
  bool storeDivisional(Id divisionId, int n, const Reader& isEngaged);
  bool storeFrame(int oneBased, const Reader& isEngaged);
  void recallGeneral(int n, std::vector<Change>& out) const;
  void recallDivisional(Id divisionId, int n, std::vector<Change>& out) const;
  void generalCancel(std::vector<Change>& out) const;
  void divisionalCancel(Id divisionId, std::vector<Change>& out) const;

  bool generalSet(int n) const;
  // A general's name, chosen by the player (#1214): "Tutti", "Plenum",
  // "Adagio". Empty for none. Kept with the registrations, so a set made
  // for a piece keeps its own names. Read and written on the message thread.
  const std::string& generalName(int n) const;
  void setGeneralName(int n, std::string name);
  // A general back to unused (#1221): nothing stored, no name, unlit.
  void clearGeneral(int n);
  bool divisionalSet(Id divisionId, int n) const;
  // A divisional's name, as a general's (JPIL04, #1221): "Cornet", "Plein
  // jeu". Empty for none. Kept with the registrations.
  const std::string& divisionalName(Id divisionId, int n) const;
  void setDivisionalName(Id divisionId, int n, std::string name);
  // A divisional back to unused, as clearGeneral.
  void clearDivisional(Id divisionId, int n);
  // Whether any divisional shown has a name, which widens all their pistons.
  bool anyDivisionalNamed() const {
    for (const auto& named : divisionalNames_)
      if (named.first.second <= divisionalCount_) return true;
    return false;
  }
  // The piston whose registration was recalled or captured last, which is the
  // one a console lights. 0 when the registration has since moved on.
  int litGeneral() const { return litGeneral_; }
  int litDivisional(Id divisionId) const;
  // A hand-drawn change in one division: the general no longer describes the
  // console, and that division's divisional no longer describes the division.
  // The other divisions' pistons still describe theirs and stay lit (#979).
  // kEveryDivision unlights them all; 0 is an element only generals reach.
  static constexpr Id kEveryDivision = -1;
  void registrationMoved(Id divisionId = kEveryDivision);
  // Whether a lit piston stays lit through hand-drawn changes, until another
  // piston, a cancel or the sequencer moves the registration (#1117). Off by
  // default: a hand-drawn change unlights it, as above.
  void setKeepLit(bool on) { keepLit_ = on; }
  bool keepLit() const { return keepLit_; }
  // Every lamp out, whatever keepLit says: a cancel was pressed.
  void unlight() { unlightDivisions(kEveryDivision); }
  // Light the piston just recalled.
  void lightGeneral(int n);
  void lightDivisional(Id divisionId, int n);

  // --- stepper ----------------------------------------------------------
  // Frames 1..kMaxFrames, every one of them there from the start, set or
  // not, as on GrandOrgue's setter (#1245): stepping goes to the next frame
  // whatever it holds, and an empty frame recalls nothing, so the stops stay
  // as they are. Its place is kept: storing into frame 010 with 000 to 004
  // set needs no frames inserted, and moves none.
  int frame() const { return frame_; }
  int lastUsedFrame() const;
  bool frameSet(int oneBased) const;
  bool stepNext(bool capturing, const Reader& isEngaged, std::vector<Change>& out);
  bool stepPrev(bool capturing, const Reader& isEngaged, std::vector<Change>& out);
  bool gotoFrame(int oneBased, bool capturing, const Reader& isEngaged,
                 std::vector<Change>& out);
  // A frame as a player reads it: numbered from 000, as GrandOrgue's setter
  // numbers them, so frame n here is shown as n - 1.
  static int shownNumber(int frame) { return frame - 1; }
  // As GrandOrgue's setter: Insert copies the current frame into a new one
  // after it, and Delete takes the current one out; later frames move with
  // them. The current frame stays where it is.
  bool insertFrame();
  bool deleteFrame();

  // --- crescendo --------------------------------------------------------
  // The player's crescendo, for an organ that has none of its own (JPIL04,
  // #1221): steps 1 to kCrescendoSteps, as on GrandOrgue's setter, each a
  // registration. It adds to what the hands drew, as GrandOrgue's crescendo
  // does: a step draws what it holds and the hands have not, and going back
  // retires only what the crescendo drew. A step never set takes the nearest
  // set step below it, so steps 1, 8, 16, 24 and 32 are enough to start.
  static constexpr int kCrescendoSteps = 32;
  int crescendoStep() const { return crescendoStep_; }
  bool crescendoSet(int step) const;
  int crescendoSetCount() const;
  // Move to a step, 0 being closed, and hand back the changes.
  void moveCrescendo(int step, const Reader& isEngaged, std::vector<Change>& out);
  // Store what is drawn into a step. What it holds then counts as the
  // crescendo's own, so closing the pedal retires it.
  bool captureCrescendo(int step, const Reader& isEngaged);
  bool storeCrescendo(int step, const Reader& isEngaged);
  void clearCrescendo(int step);
  // The pedal's value (0 to 127) as a step: 0 closed, and 1 to 32 in equal
  // parts of the travel above it.
  static int crescendoStepOf(int value);

  // --- persistence ------------------------------------------------------
  std::string toText() const;
  bool fromText(const std::string& text);
  void clearAll();

  static std::string keyOf(const Element& e);

  // --- ready-made registrations (#1221) ------------------------------------
  // Graded registrations built from the stop names, as the PP to Tutti
  // buttons of a digital organ: PP, P, MF, F, Plein jeu, Tutti. Each stop is
  // read through the stop-name dictionary (family and footage, in several
  // languages); the main manual is the one with the most stops, the pedal the
  // division `keyboardOf` gives 0. A level holds the one before it:
  //   PP         a soft stopped flute 8' on a manual, a soft 16' in the pedal
  //   P          a flute 8' on the main manual too, a flute 8' in the pedal
  //   MF         the main manual's principal 8' and a flute 4', a 4' on the
  //              others, the pedal's 8' principal
  //   F          the principal chorus to 2' on every manual, the pedal's 16'
  //   Plein jeu  the mixtures and the principals' mutations
  //   Tutti      every stop but celestes and percussion, every coupler; no
  //              tremulant
  // `isTremulant` says which switch elements are tremulants; the others are
  // couplers. Nothing here stores anything: the processor fills only the
  // generals not yet set.
  struct Suggestion {
    std::string name;
    std::vector<char> drawn;  // one flag per element, in elements() order
    // The elements the suggestion leaves as they are (#1221): stops whose
    // name says no family (a blower, a motor, bells). A fixed combination
    // keeps what they are when it is filled, so PP does not stop the wind.
    std::vector<char> kept;
  };
  static std::vector<Suggestion> suggestRegistrations(const std::vector<Element>& elements,
                                                      const std::function<int(Id)>& keyboardOf,
                                                      const std::function<bool(Id)>& isTremulant);
  // Fills every crescendo step from the ready-made registrations, in order,
  // each step adding a few stops, from PP up to Tutti. Only when no step is
  // set yet; false otherwise.
  bool fillCrescendo(const std::vector<Suggestion>& graded);

private:
  std::vector<Element> elements_;
  std::vector<Division> divisions_;
  int generalCount_ = kDefaultGenerals;
  int divisionalCount_ = kDefaultDivisionals;
  // Index 0 unused, so a piston's number is its index.
  std::vector<Registration> generals_;
  std::vector<std::string> generalNames_;  // by number, as generals_
  std::map<std::pair<Id, int>, std::string> divisionalNames_;  // by division and number
  std::vector<Registration> frames_;
  std::vector<Registration> crescendo_;  // by step, index 0 unused
  std::vector<char> crescendoDrew_;      // by element: drawn by the crescendo
  int crescendoStep_ = 0;
  // divisionIndex * (kMaxDivisionals + 1) + piston
  std::vector<Registration> divisionals_;
  std::vector<int> litDivisional_;  // by division index
  int frame_ = 0;
  int litGeneral_ = 0;
  bool leaveBassAndMelody_ = false;
  bool keepLit_ = false;
  void unlightDivisions(Id divisionId);
  bool left(const Element& e) const { return leaveBassAndMelody_ && e.bassOrMelody; }

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
