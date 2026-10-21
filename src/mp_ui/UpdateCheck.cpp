#include "UpdateCheck.h"

#include "../mp_audio/MasterpieceProcessor.h"

namespace mp::ui {

namespace {

// GitHub's own record of the newest release: drafts and pre-releases are
// left out of "latest". Asked once a day, well inside the hourly allowance
// GitHub gives a request that carries no account.
const char* const kVersionUrl = "https://api.github.com/repos/bonninr/masterpiece/releases/latest";
const char* const kRequestHeaders = "User-Agent: Masterpiece\r\nAccept: application/vnd.github+json";
// The newest release's page, with its downloads, on the same host.
const char* const kDownloadUrl = "https://github.com/bonninr/masterpiece/releases/latest";
constexpr int kShownMs = 30000;

// The newest version's number, or empty. JUCE fetches it everywhere but on
// Linux, where it is built without curl; there the system's own curl does,
// which every distribution and the Flatpak runtime carry.
juce::String fetchLatest() {
  // MASTERPIECE_UPDATE_URL reads another address: a test server, or a copy.
  const juce::String url = juce::SystemStats::getEnvironmentVariable("MASTERPIECE_UPDATE_URL", kVersionUrl);
  juce::String text;
#if JUCE_LINUX
  juce::ChildProcess curl;
  if (curl.start(juce::StringArray{"curl", "-fsSL", "--max-time", "5", "-H", "User-Agent: Masterpiece", "-H",
                                   "Accept: application/vnd.github+json", url},
                 juce::ChildProcess::wantStdOut)) {
    text = curl.readAllProcessOutput();  // returns when curl ends, in 5 s at most
    if (curl.getExitCode() != 0) text.clear();
  }
#else
  const auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs(5000)
                           .withNumRedirectsToFollow(3)
                           .withExtraHeaders(kRequestHeaders);
  if (auto in = juce::URL(url).createInputStream(options)) text = in->readEntireStreamAsString();
#endif
  return versionFromAnswer(text);
}

std::vector<int> parts(const juce::String& v) {
  std::vector<int> out;
  for (const auto& p : juce::StringArray::fromTokens(v.trim(), ".", "")) out.push_back(p.getIntValue());
  return out;
}

} // namespace

juce::String versionFromAnswer(const juce::String& answer) {
  auto text = answer.trim();
  // GitHub's release record names the release by its tag, "v0.9.0". A plain
  // number is read as it is: a test server, or a copy given by
  // MASTERPIECE_UPDATE_URL.
  if (text.startsWithChar('{')) text = juce::JSON::parse(text)["tag_name"].toString().trim();
  if (text.startsWithIgnoreCase("v")) text = text.substring(1);
  // A version, and nothing else: a captive portal's page is not one.
  return text.containsOnly("0123456789.") && text.contains(".") && text.length() <= 20 ? text : juce::String();
}

bool isNewerVersion(const juce::String& latest, const juce::String& current) {
  const auto a = parts(latest), b = parts(current);
  for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
    const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
    if (x != y) return x > y;
  }
  return false;
}

void checkForUpdate(MasterpieceProcessor& proc, const juce::String& currentVersion,
                    std::function<void(const juce::String& latest)> onNewer) {
  if (!proc.updateCheckDue()) return;
  proc.markUpdateChecked();
  const auto alive = proc.aliveToken();
  juce::Thread::launch([alive, currentVersion, onNewer = std::move(onNewer)] {
    const juce::String latest = fetchLatest();
    if (latest.isEmpty() || !isNewerVersion(latest, currentVersion)) {
      juce::Logger::writeToLog("update check: " +
                               (latest.isEmpty() ? juce::String("no answer") : "this is the newest, " + latest));
      return;
    }
    juce::Logger::writeToLog("update check: " + latest + " is out, this is " + currentVersion);
    juce::MessageManager::callAsync([alive, latest, onNewer] {
      if (const auto token = alive.lock(); token != nullptr && token->load() && onNewer) onNewer(latest);
    });
  });
}

UpdateNotice::UpdateNotice(MasterpieceProcessor& p) : proc_(p) {
  text_.setColour(juce::Label::textColourId, juce::Colour(0xffe8c98a));
  text_.setFont(juce::FontOptions(12.0f));
  text_.setJustificationType(juce::Justification::centredRight);
  addAndMakeVisible(text_);
  download_.setTooltip(TRANS("Opens the download page in the browser"));
  download_.onClick = [this] {
    juce::URL(kDownloadUrl).launchInDefaultBrowser();
    hide();
  };
  addAndMakeVisible(download_);
  never_.setTooltip(TRANS("No more checks; General settings, Log turns them on again"));
  never_.onClick = [this] {
    proc_.setUpdateCheckEnabled(false);
    hide();
  };
  addAndMakeVisible(never_);
  close_.setTooltip(TRANS("Close"));
  close_.onClick = [this] { hide(); };
  addAndMakeVisible(close_);
  setVisible(false);
}

void UpdateNotice::show(const juce::String& latest) {
  text_.setText(TRANS("Masterpiece %1 is out").replace("%1", latest), juce::dontSendNotification);
  setVisible(true);
  // Kept through a load, which can take minutes: the countdown starts when
  // it ends (loadFinished).
  if (isLoading && isLoading()) stopTimer();
  else startTimer(kShownMs);
  if (onShownOrHidden) onShownOrHidden();
}

void UpdateNotice::loadFinished() {
  if (isVisible()) startTimer(kShownMs);
}

void UpdateNotice::hide() {
  stopTimer();
  setVisible(false);
  if (onShownOrHidden) onShownOrHidden();
}

void UpdateNotice::timerCallback() {
  // A load that began meanwhile: stay, and count again from its end.
  if (isLoading && isLoading()) {
    stopTimer();
    return;
  }
  hide();
}

int UpdateNotice::preferredWidth() const { return 170 + 80 + 130 + 24; }

void UpdateNotice::resized() {
  auto r = getLocalBounds();
  close_.setBounds(r.removeFromRight(24).reduced(2, 1));
  never_.setBounds(r.removeFromRight(130).reduced(2, 1));
  download_.setBounds(r.removeFromRight(80).reduced(2, 1));
  text_.setBounds(r);
}

void UpdateNotice::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff1f2329)); }

} // namespace mp::ui
