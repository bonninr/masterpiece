// The program's words in the player's language.
//
// One plain-text file per language under resources/translations/, compiled
// into the program so there is nothing to install beside it. JUCE's
// LocalisedStrings reads them; every string a player sees goes through
// TRANS(), and one that a file does not translate stays English. English
// itself has no file: it is the text in the code.
#pragma once
#include <juce_core/juce_core.h>

#include <utility>
#include <vector>

namespace mp {

struct Language {
  juce::String code;  // "es": the name of the file, without its extension
  juce::String name;  // as the language writes it, for the setting's list
};

// The languages that have a file, in code order. English is not among them.
std::vector<Language> availableLanguages();

// The text of one language, or an empty string when there is none: its file,
// "es.txt", then each of its part files, "es.<anything>.txt", in name order.
// A change that adds strings puts them in a part file of its own, so two
// changes never edit the same lines; tools/i18n.py --fold folds the parts
// into the main file.
juce::String translationText(const juce::String& code);

// The language a file belongs to, "es" for "es.txt" and "es.reverb.txt", and
// whether it is the language's main file.
juce::String languageOfFile(const juce::String& fileName);
bool isMainLanguageFile(const juce::String& fileName);

// One language's text from the files given as (file name, text): the main
// file first, then its parts in name order. Files of other languages are
// left out.
juce::String joinLanguageFiles(const juce::String& code,
                               std::vector<std::pair<juce::String, juce::String>> files);

// The language the system is set to, as a code that has a file, or "en".
juce::String systemLanguageCode();

// Makes a language the one TRANS() speaks. `choice` is "" for the system's
// language, "en" for English, or a code from availableLanguages(). Anything
// else is English. Returns the code actually in force.
juce::String applyLanguage(const juce::String& choice);

}  // namespace mp
