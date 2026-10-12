#include "LoadingDialog.h"

namespace mp::ui {

LoadingDialog::LoadingDialog(MasterpieceProcessor& proc,
                             const juce::String& organName)
    : proc_(proc), bar_(barValue_) {
  addAndMakeVisible(title_);
  title_.setText(organName, juce::dontSendNotification);
  title_.setFont(juce::Font(juce::FontOptions(18.0f)));
  title_.setColour(juce::Label::textColourId, juce::Colours::white);

  addAndMakeVisible(phase_);
  phase_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

  addAndMakeVisible(detail_);
  detail_.setFont(juce::Font(juce::FontOptions(12.0f)));
  detail_.setColour(juce::Label::textColourId, juce::Colours::grey);

  addAndMakeVisible(bar_);
  // JUCE draws a moving barber-pole for a negative value, which is the honest
  // display while a phase has nothing countable to report.
  barValue_ = -1.0;

  addAndMakeVisible(cancel_);
  cancel_.onClick = [this] {
    if (cancelling_) return;
    cancelling_ = true;
    // The button stays visible but stops responding: pressing Cancel twice
    // cannot make the load stop sooner, and a dead-looking button invites a
    // second press.
    cancel_.setEnabled(false);
    cancel_.setButtonText(TRANS("Cancelling..."));
    detail_.setText(TRANS("Finishing the file it is reading"),
                    juce::dontSendNotification);
    if (onCancel) onCancel();
  };

  timerCallback();
  startTimerHz(10);
}

LoadingDialog::~LoadingDialog() { stopTimer(); }

// LoadProgress is below the interface and has no translations of its own, so
// the words for each phase are given here.
static juce::String phaseText(LoadProgress::Phase p) {
  switch (p) {
    case LoadProgress::Phase::OpeningArchive:    return TRANS("Opening the archive");
    case LoadProgress::Phase::ReadingArchive:    return TRANS("Reading the archive");
    case LoadProgress::Phase::ExtractingArchive: return TRANS("Extracting definitions and artwork");
    case LoadProgress::Phase::ReadingDefinition: return TRANS("Reading the organ definition");
    case LoadProgress::Phase::BuildingWind:      return TRANS("Building the wind model");
    case LoadProgress::Phase::WiringConsole:     return TRANS("Wiring the console");
    case LoadProgress::Phase::FindingSamples:    return TRANS("Finding the samples");
    case LoadProgress::Phase::LoadingSamples:    return TRANS("Loading samples");
    case LoadProgress::Phase::Preparing:         return TRANS("Preparing to play");
    case LoadProgress::Phase::BuildingConsole:   return TRANS("Building the console");
    case LoadProgress::Phase::Done:              return TRANS("Ready");
    case LoadProgress::Phase::Cancelled:         return TRANS("Cancelled");
    case LoadProgress::Phase::Failed:            return TRANS("Failed");
    case LoadProgress::Phase::Idle:              break;
  }
  return {};
}

// Whole seconds are noise on a multi-minute estimate; "about 4 minutes" is
// both truer and easier to read than "4:07 remaining" that is wrong by a
// minute either way.
static juce::String humanise(double secs) {
  if (secs < 45.0) return TRANS("less than a minute");
  const int mins = static_cast<int>(secs / 60.0 + 0.5);
  if (mins <= 1) return TRANS("about a minute");
  return TRANS("about %1 minutes").replace("%1", juce::String(mins));
}

juce::String LoadingDialog::estimateRemaining() {
  const auto& p = proc_.loadProgress();
  const int done = p.done.load(std::memory_order_relaxed);
  const int total = p.total.load(std::memory_order_relaxed);
  if (total <= 0 || done <= 0) return {};

  const double elapsed =
      (juce::Time::getMillisecondCounterHiRes() - phaseStartMs_) / 1000.0;
  // Below a couple of percent the rate is dominated by whatever the OS had
  // cached, and the estimate swings by minutes. Saying nothing is better than
  // saying something that will be contradicted in five seconds.
  if (elapsed < 3.0 || done < total / 50) return {};

  const double perItem = elapsed / static_cast<double>(done);
  const double raw = perItem * static_cast<double>(total - done);

  // Exponential smoothing, and only ever toward the new value: a load that
  // hits a slow patch should let the estimate rise rather than pretend.
  etaSeconds_ = etaSeconds_ < 0.0 ? raw : etaSeconds_ * 0.85 + raw * 0.15;
  return humanise(etaSeconds_);
}

void LoadingDialog::timerCallback() {
  const auto& p = proc_.loadProgress();
  const auto phase = p.phase.load(std::memory_order_acquire);

  // Each phase is timed from when it starts, because the rate of one says
  // nothing about the rate of the next.
  if (phase != lastPhase_) {
    lastPhase_ = phase;
    phaseStartMs_ = juce::Time::getMillisecondCounterHiRes();
    etaSeconds_ = -1.0;
  }
  if (!cancelling_)
    phase_.setText(phaseText(phase), juce::dontSendNotification);

  const double f = p.fraction();
  barValue_ = f;  // negative keeps the bar indeterminate rather than faking 0%

  if (phase == LoadProgress::Phase::LoadingSamples) {
    const int done = p.done.load(std::memory_order_relaxed);
    const int total = p.total.load(std::memory_order_relaxed);
    if (!cancelling_ && total > 0) {
      juce::String text = TRANS("%1 of %2 samples")
                              .replace("%1", juce::String(done))
                              .replace("%2", juce::String(total));
      const auto eta = estimateRemaining();
      if (eta.isNotEmpty()) text += "   -   " + TRANS("%1 left").replace("%1", eta);
      detail_.setText(text, juce::dontSendNotification);
    }
  } else if (!cancelling_ && (phase == LoadProgress::Phase::ReadingArchive ||
                              phase == LoadProgress::Phase::ExtractingArchive)) {
    const juce::String percent = juce::String(static_cast<int>(f * 100.0));
    detail_.setText(f >= 0.0 ? (phase == LoadProgress::Phase::ReadingArchive
                                    ? TRANS("%1% of archive scanned")
                                    : TRANS("%1% of files processed")).replace("%1", percent)
                            : juce::String(TRANS("This may take a few minutes")),
                    juce::dontSendNotification);
  } else if (!cancelling_) {
    // Every other phase counts nothing, and inventing a number would be a lie
    // the next phase immediately contradicts. The phase line carries the
    // meaning; this one stays empty rather than holding the last phase's
    // sample count while something else entirely is happening.
    detail_.setText("", juce::dontSendNotification);
  }
}

void LoadingDialog::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff15171c));
}

void LoadingDialog::resized() {
  auto r = getLocalBounds().reduced(18);
  title_.setBounds(r.removeFromTop(28));
  r.removeFromTop(2);
  phase_.setBounds(r.removeFromTop(22));
  r.removeFromTop(8);
  bar_.setBounds(r.removeFromTop(22));
  r.removeFromTop(6);
  detail_.setBounds(r.removeFromTop(20));
  auto foot = r.removeFromBottom(30);
  cancel_.setBounds(foot.removeFromRight(120).reduced(2, 2));
}

}  // namespace mp::ui
