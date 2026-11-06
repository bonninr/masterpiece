#include "Translations.h"

#include "MpTranslations.h"

#include <algorithm>

namespace mp {

namespace {
bool hasLanguage(const juce::String& code) {
  for (const auto& l : availableLanguages())
    if (l.code == code) return true;
  return false;
}
}  // namespace

juce::String languageOfFile(const juce::String& fileName) {
  return juce::File::createFileWithoutCheckingPath(fileName).getFileName().upToFirstOccurrenceOf(
      ".", false, false);
}

bool isMainLanguageFile(const juce::String& fileName) {
  const auto name = juce::File::createFileWithoutCheckingPath(fileName).getFileName();
  return name == languageOfFile(name) + ".txt";
}

juce::String joinLanguageFiles(const juce::String& code,
                               std::vector<std::pair<juce::String, juce::String>> files) {
  files.erase(std::remove_if(files.begin(), files.end(),
                             [&](const auto& f) { return languageOfFile(f.first) != code; }),
              files.end());
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    const bool am = isMainLanguageFile(a.first), bm = isMainLanguageFile(b.first);
    if (am != bm) return am;
    return a.first < b.first;
  });
  juce::String out;
  for (const auto& f : files) {
    out << f.second;
    if (!out.endsWithChar('\n')) out << "\n";
  }
  return out;
}

juce::String translationText(const juce::String& code) {
  std::vector<std::pair<juce::String, juce::String>> files;
  for (int i = 0; i < MpTranslations::namedResourceListSize; ++i) {
    const juce::String name = MpTranslations::originalFilenames[i];
    if (languageOfFile(name) != code) continue;
    int size = 0;
    const char* data = MpTranslations::getNamedResource(MpTranslations::namedResourceList[i], size);
    files.emplace_back(name, juce::String::fromUTF8(data, size));
  }
  return joinLanguageFiles(code, std::move(files));
}

std::vector<Language> availableLanguages() {
  std::vector<Language> out;
  for (int i = 0; i < MpTranslations::namedResourceListSize; ++i) {
    if (!isMainLanguageFile(MpTranslations::originalFilenames[i])) continue;
    const auto code = languageOfFile(MpTranslations::originalFilenames[i]);
    juce::LocalisedStrings file(translationText(code), false);
    out.push_back({code, file.getLanguageName().isNotEmpty() ? file.getLanguageName() : code});
  }
  std::sort(out.begin(), out.end(),
            [](const Language& a, const Language& b) { return a.code < b.code; });
  return out;
}

juce::String systemLanguageCode() {
  // "es" on most systems; "es-AR" or "es_AR" where the system adds the region.
  const auto system = juce::SystemStats::getUserLanguage()
                          .toLowerCase()
                          .upToFirstOccurrenceOf("-", false, false)
                          .upToFirstOccurrenceOf("_", false, false);
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
