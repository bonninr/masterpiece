// The player's settings as one file: an organ's, or everything, exported to a
// zip and restored from one. And one organ's MIDI map, combinations, mixer
// and voicing copied to another.
//
// Everything works on the settings folder given (`root`, which is
// %APPDATA%\Masterpiece and its equivalents), so the tests run on a folder of
// their own. An organ's files are named by its key (its name and its id,
// MasterpieceProcessor::organKey()):
//
//   organs/<key>.mporgan       loading, voicing, mixer, tuning, state
//   organs/<key>.mppanels      the panels' layout
//   midi/<key>.mpmidi          the organ's MIDI map
//   combinations/<key>[.<set>].mpcomb   the combination sets
//   split/<key>.mpsplit        the split keyboard
//
// and the program's own are settings.mpglobal, midi/console.mpmidi and
// split-keyboard.txt. The audio device is left out: it belongs to the computer.
//
// An archive holds a manifest, masterpiece-settings.txt, and the files as
// entries f1, f2...; the manifest gives each one's path. Every path is checked
// against the names above before anything is written, so an archive cannot
// reach outside the settings folder, and one that is not ours is refused
// whole. Restoring first saves the files it replaces, and those of the organ
// it does not hold, to backups/ in the settings folder.
#pragma once

#include <juce_core/juce_core.h>

namespace mp::settings {

enum class Scope { Organ, All };

struct Contents {
  Scope scope = Scope::Organ;
  juce::String organKey;    // Scope::Organ: whose settings
  juce::String organName;   // as the program showed it, for the player
  juce::String program;     // the version that wrote it
  juce::StringArray paths;  // relative to the settings folder, with '/'
};

// The files of one organ that exist, relative to `root`, in a stable order.
juce::StringArray organFiles(const juce::File& root, const juce::String& key);
// The program's own settings files that exist.
juce::StringArray globalFiles(const juce::File& root);
// Every organ that has settings, by key, sorted.
juce::StringArray organKeys(const juce::File& root);

bool exportOrgan(const juce::File& root, const juce::String& key, const juce::String& organName,
                 const juce::String& program, const juce::File& zip, juce::String& error);
bool exportAll(const juce::File& root, const juce::String& program, const juce::File& zip, juce::String& error);

// Reads and checks an archive; changes nothing.
bool inspect(const juce::File& zip, Contents& out, juce::String& error);

// Writes an archive's files into `root`: an organ's archive replaces that
// organ's settings, an archive of everything replaces every setting it
// covers. What is replaced or removed is saved first; `backup` names that
// file. Nothing is changed when this returns false.
bool restore(const juce::File& zip, const juce::File& root, const juce::String& program, juce::File& backup,
             juce::String& error);

// What copy() can take from one organ to another.
enum Part { MidiMap = 1, Combinations = 2, MixerAndVoicing = 4 };
// Copies those parts of organ `from`'s settings to organ `to`, replacing
// `to`'s own; `to`'s files are saved to backups/ first. The mixer and
// voicing are the route and voicing lines of the .mporgan file; the rest of
// `to`'s file is kept.
bool copy(const juce::File& root, const juce::String& from, const juce::String& to, int parts,
          const juce::String& program, juce::File& backup, juce::String& error);
// The rank and pipe ids the mixer and voicing lines of an organ's settings
// name, for checking they mean something in another organ.
struct VoicingIds {
  juce::Array<juce::int64> ranks, pipes;
};
VoicingIds voicingIds(const juce::File& root, const juce::String& key);

// This program's version, as an archive records it.
juce::String programVersion();

// Whether a path may be part of an archive of this scope (and, for an
// organ's, of this organ).
bool allowedPath(const juce::String& path, Scope scope, const juce::String& key);

}  // namespace mp::settings
