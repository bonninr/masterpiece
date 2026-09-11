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
    cancel_.setButtonText("Cancelling...");
    detail_.setText("Finishing the file it is reading",
                    juce::dontSendNotification);
    if (onCancel) onCancel();
  };

  startTimerHz(10);
}

LoadingDialog::~LoadingDialog() { stopTimer(); }

// Whole seconds are noise on a multi-minute estimate; "about 4 minutes" is
// both truer and easier to read than "4:07 remaining" that is wrong by a
// minute either way.
static juce::String humanise(double secs) {
  if (secs < 45.0) return "less than a minute";
  const int mins = static_cast<int>(secs / 60.0 + 0.5);
  if (mins <= 1) return "about a minute";
  return "about " + juce::String(mins) + " minutes";
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
    phase_.setText(LoadProgress::phaseName(phase), juce::dontSendNotification);

  const double f = p.fraction();
  barValue_ = f;  // negative keeps the bar indeterminate rather than faking 0%

  if (phase == LoadProgress::Phase::LoadingSamples) {
    const int done = p.done.load(std::memory_order_relaxed);
    const int total = p.total.load(std::memory_order_relaxed);
    if (!cancelling_ && total > 0) {
      juce::String text = juce::String(done) + " of " + juce::String(total) +
                          " samples";
      const auto eta = estimateRemaining();
      if (eta.isNotEmpty()) text += "   -   " + eta + " left";
      detail_.setText(text, juce::dontSendNotification);
    }
  } else if (!cancelling_ && phase == LoadProgress::Phase::ReadingDefinition) {
    // No item count exists yet, and inventing one would be a lie that the
    // next phase immediately contradicts.
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
