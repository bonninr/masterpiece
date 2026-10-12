#include "Translations.h"

#include "MpTranslations.h"

#include <algorithm>

namespace mp {

namespace {
// The name binary data gives a file is the file name with its dot turned into
// an underscore, so the code is what is left of "es.txt".
juce::String codeOf(const char* originalFilename) {
  return juce::File::createFileWithoutCheckingPath(originalFilename)
      .getFileNameWithoutExtension();
}

bool hasLanguage(const juce::String& code) {
  for (const auto& l : availableLanguages())
    if (l.code == code) return true;
  return false;
}
}  // namespace

juce::String translationText(const juce::String& code) {
  for (int i = 0; i < MpTranslations::namedResourceListSize; ++i) {
    if (codeOf(MpTranslations::originalFilenames[i]) != code) continue;
    int size = 0;
    const char* data = MpTranslations::getNamedResource(MpTranslations::namedResourceList[i], size);
    return juce::String::fromUTF8(data, size);
  }
  return {};
}

std::vector<Language> availableLanguages() {
  std::vector<Language> out;
  for (int i = 0; i < MpTranslations::namedResourceListSize; ++i) {
    const auto code = codeOf(MpTranslations::originalFilenames[i]);
    juce::LocalisedStrings file(translationText(code), false);
    out.push_back({code, file.getLanguageName().isNotEmpty() ? file.getLanguageName() : code});
  }
  std::sort(out.begin(), out.end(),
            [](const Language& a, const Language& b) { return a.code < b.code; });
  return out;
}

juce::String systemLanguageCode() {
  const auto system = juce::SystemStats::getUserLanguage().toLowerCase();
  return hasLanguage(system) ? system : juce::String("en");
}

juce::String applyLanguage(const juce::String& choice) {
  const auto want = choice.isEmpty() ? systemLanguageCode() : choice.toLowerCase();
  if (want == "en" || !hasLanguage(want)) {
    juce::LocalisedStrings::setCurrentMappings(nullptr);
    return "en";
  }
  juce::LocalisedStrings::setCurrentMappings(new juce::LocalisedStrings(translationText(want), false));
  return want;
}

}  // namespace mp
