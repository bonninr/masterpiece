// Whether a newer Masterpiece is out, asked once a day.
//
// The application reads GitHub's record of the project's newest release,
// over HTTPS from api.github.com, and takes the number from its tag. It
// sends nothing about the player beyond what any request for a web page
// carries. When the number is newer than this program's, a notice at the
// right of the status bar offers the download page until 30 seconds after
// the organ has loaded, and goes:
// nothing takes the focus or waits for an answer while the player plays. The
// notice can turn the check off, as General settings > Log can. A check that
// fails -- no network, a slow one, no curl on Linux -- says nothing and is
// tried again the next day.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace mp {
class MasterpieceProcessor;

namespace ui {

// Whether `latest` is a newer version than `current`: dotted numbers,
// compared part by part ("0.10.0" is newer than "0.9.2").
bool isNewerVersion(const juce::String& latest, const juce::String& current);

// The version an answer names: GitHub's release record by its tag ("v0.9.0"
// gives "0.9.0"), or a plain number. Empty for anything else.
juce::String versionFromAnswer(const juce::String& answer);

// The check, when the player has it on and a day has passed since the last.
// The file is read on a thread of its own; `onNewer` is called on the message
// thread with the newer version's number.
void checkForUpdate(MasterpieceProcessor& proc, const juce::String& currentVersion,
                    std::function<void(const juce::String& latest)> onNewer);

// Help > Check for updates: asked now, whatever the day. `onAnswer` is called
// on the message thread with the newest version's number, empty when there
// was no answer.
void checkForUpdateNow(MasterpieceProcessor& proc, std::function<void(const juce::String& latest)> onAnswer);

// This program's version, "0.9.0".
juce::String programVersion();

// What the program calls itself: its version on a tagged release,
// "main 3fa91" on a build of main, "dev 3fa91" on any other build.
juce::String buildLabel();

// The pages Help opens.
extern const char* const kDocumentationUrl;
extern const char* const kReleaseNotesUrl;
extern const char* const kDownloadPageUrl;

// "Masterpiece 0.8.1 is out", with Download, Never check again and a close
// button, at the right of the status bar. It stays while an organ loads and
// goes 30 seconds after the load ends; with no load, 30 seconds after it
// appears.
class UpdateNotice : public juce::Component, private juce::Timer {
public:
  explicit UpdateNotice(MasterpieceProcessor& p);
  void show(const juce::String& latest);
  // Whether an organ is loading, asked when the notice appears.
  std::function<bool()> isLoading;
  // An organ's load has ended: the 30 seconds start now.
  void loadFinished();
  // The width it wants in the status bar; the bar's height is its own.
  int preferredWidth() const;
  void resized() override;
  void paint(juce::Graphics& g) override;
  // Called when it appears or goes, for the bar to lay itself out again.
  std::function<void()> onShownOrHidden;

private:
  void timerCallback() override;
  void hide();
  MasterpieceProcessor& proc_;
  juce::Label text_;
  juce::TextButton download_{TRANS("Download")};
  juce::TextButton never_{TRANS("Never check again")};
  juce::TextButton close_{juce::String::fromUTF8("\xc3\x97")};
};

} // namespace ui
} // namespace mp
