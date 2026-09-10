// Where each key of a drawn manual goes, and which picture it is drawn from.
//
// Hauptwerk does not ship a keyboard as one picture. It ships one image per
// KEY SHAPE and the horizontal advances between them, and the console
// assembles the manual key by key. This header is the assembly rule, kept free
// of JUCE so it can be reasoned about and tested without a console.
//
// Two things make it more than a loop over a constant key width:
//
//   * A key is not a rectangle. It is notched wherever a sharp sits beside it,
//     so a C is cut differently from a D, which is cut differently from an E.
//   * The advance is not one number. The gap from a C to its C# is not the gap
//     from that C# to the D, which is why Hauptwerk records seven of them.
//
// The rule below matches the one OdfEdit uses to convert Hauptwerk consoles
// for GrandOrgue (specs/odfedit), which is the only written-down account of it
// there is.
//
// The advances do NOT have to be self-consistent, and in real sets they are
// not. Lemmer makes C-to-C#-to-D add up to 17 pixels while E-to-F is 16, so an
// octave comes out 117 wide rather than 112; its pedal set is further out
// still. Keys are authored to overlap, and normalising the numbers would draw
// a tidier keyboard than the organ has. Walk them as given.
#pragma once
#include "OrganModel.h"

namespace mp {

// Pitch classes, as a reminder of what the numbers below mean:
// 0=C 1=C# 2=D 3=D# 4=E 5=F 6=F# 7=G 8=G# 9=A 10=A# 11=B
constexpr bool isSharpPitchClass(int pc) {
  return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// Pitch class of a MIDI note, correct for negative inputs.
constexpr int pitchClassOf(int midiNote) {
  return ((midiNote % 12) + 12) % 12;
}

// How far right the NEXT key's left edge sits, given this key's pitch class.
inline int keyAdvance(const KeyImageSet& ks, int pc) {
  switch (pc) {
    case 0: case 5:   return ks.spacingCFToSharp;        // C, F -> its sharp
    case 2: case 9:   return ks.spacingDAToSharp;        // D, A -> its sharp
    case 7:           return ks.spacingGToSharp;         // G -> G#
    case 4: case 11:  return ks.spacingNaturalToNatural; // E->F, B->C
    case 1: case 6:   return ks.spacingSharpToDG;        // C#->D, F#->G
    case 3: case 10:  return ks.spacingSharpToEB;        // D#->E, A#->B
    case 8:           return ks.spacingSharpToA;         // G#->A
    default:          return ks.spacingNaturalToNatural;
  }
}

// Which image set this key is drawn from. `first` and `last` mark the ends of
// the compass, where the neighbouring sharp does not exist and the key is
// therefore cut square on that side. Returns 0 when the set defines no usable
// shape, which means the key is not drawn at all.
inline Id keyShapeFor(const KeyImageSet& ks, int pc, bool first, bool last) {
  if (isSharpPitchClass(pc)) return ks.shapeSharp;

  if (first) {
    // A D or an A at the bottom of the compass has no sharp below it, and a G
    // none either; Hauptwerk ships a separate cut for exactly those. An E or a
    // B at the bottom is an uncut natural. A C or an F needs nothing special:
    // it is never notched on its left anyway.
    if ((pc == 2 || pc == 9) && ks.shapeFirstKeyDA) return ks.shapeFirstKeyDA;
    if (pc == 7 && ks.shapeFirstKeyG) return ks.shapeFirstKeyG;
    if ((pc == 4 || pc == 11) && ks.shapeWholeNatural)
      return ks.shapeWholeNatural;
  }
  if (last) {
    if ((pc == 2 || pc == 7) && ks.shapeLastKeyDG) return ks.shapeLastKeyDG;
    if (pc == 9 && ks.shapeLastKeyA) return ks.shapeLastKeyA;
    // A C or an F at the top has no sharp above it, so it is a plain natural.
    if ((pc == 0 || pc == 5) && ks.shapeWholeNatural) return ks.shapeWholeNatural;
  }

  switch (pc) {
    case 0: case 5:  return ks.shapeCF ? ks.shapeCF : ks.shapeWholeNatural;
    case 2:          return ks.shapeD ? ks.shapeD : ks.shapeWholeNatural;
    case 4: case 11: return ks.shapeEB ? ks.shapeEB : ks.shapeWholeNatural;
    case 7:          return ks.shapeG ? ks.shapeG : ks.shapeWholeNatural;
    case 9:          return ks.shapeA ? ks.shapeA : ks.shapeWholeNatural;
    default:         return ks.shapeWholeNatural;
  }
}

} // namespace mp
