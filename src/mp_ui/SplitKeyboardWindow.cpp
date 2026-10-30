#include "SplitKeyboardWindow.h"

#include <array>
#include <functional>

#include "Mobile.h"

namespace mp::ui {

namespace {

using split::Layout;
using split::Role;

constexpr int kRow = 26;
constexpr int kLowKey = 21, kHighKey = 108;  // a piano's 88 keys, whatever the keyboard has

const juce::Colour kZoneColour[split::kMaxZones] = {juce::Colour(0xff4a90d9), juce::Colour(0xff5cb85c),
                                                     juce::Colour(0xffe0a030), juce::Colour(0xffa06cd5)};

juce::String noteName(int n) {
  static const char* kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (n < 0 || n > 127) return "-";
  return juce::String(kNames[n % 12]) + juce::String(n / 12 - 1);
}

bool isBlack(int n) {
  const int pc = n % 12;
  return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

juce::String roleText(Role r) {
  switch (r) {
    case Role::Lower: return TRANS("Lower");
    case Role::Upper: return TRANS("Upper");
    case Role::Solo: return TRANS("Solo");
    case Role::Pedal: return TRANS("Pedal");
    default: return TRANS("Nothing");
  }
}

juce::String factoryText(int i) {
  switch (i) {
    case 0: return TRANS("Two manuals and pedal");
    case 1: return TRANS("Two manuals and bass");
    default: return TRANS("Two manuals");
  }
}

juce::String controlText(int c) {
  switch (c) {
    case 0: return TRANS("On and off");
    case 1: return TRANS("Next preset");
    case 2: return TRANS("Previous preset");
    default: return TRANS("Preset %1").replace("%1", juce::String(c - 2));
  }
}

juce::String triggerText(const split::Trigger& t) {
  switch (t.kind) {
    case split::Trigger::Note:
      return TRANS("Note %1, channel %2").replace("%1", noteName(t.number)).replace("%2", juce::String(t.channel));
    case split::Trigger::Controller:
      return TRANS("Controller %1, channel %2")
          .replace("%1", juce::String(t.number))
          .replace("%2", juce::String(t.channel));
    case split::Trigger::Program:
      return TRANS("Program %1, channel %2")
          .replace("%1", juce::String(t.number + 1))
          .replace("%2", juce::String(t.channel));
    default: return TRANS("Learn");
  }
}

void styleLabel(juce::Label& l, const juce::String& text) {
  l.setText(text, juce::dontSendNotification);
  l.setColour(juce::Label::textColourId, juce::Colour(0xffb9c2d0));
}

void styleHeading(juce::Label& l, const juce::String& text) {
  styleLabel(l, text);
  l.setFont(juce::FontOptions(15.0f, juce::Font::bold));
  l.setColour(juce::Label::textColourId, juce::Colours::white);
}

void noteSlider(juce::Slider& s) {
  s.setSliderStyle(juce::Slider::IncDecButtons);
  s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 52, kRow - 4);
  s.setRange(1, 127, 1);
  s.textFromValueFunction = [](double v) { return noteName(static_cast<int>(v)); };
  s.valueFromTextFunction = [](const juce::String& text) {
    for (int n = 0; n < 128; ++n)
      if (noteName(n).equalsIgnoreCase(text.trim())) return static_cast<double>(n);
    return text.getDoubleValue();
  };
}

void stepSlider(juce::Slider& s, int lo, int hi) {
  s.setSliderStyle(juce::Slider::IncDecButtons);
  s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 40, kRow - 4);
  s.setRange(lo, hi, 1);
}

void roleBox(juce::ComboBox& c) {
  for (int r = 0; r <= split::kRoleCount; ++r) c.addItem(roleText(static_cast<Role>(r)), r + 1);
}

// The keyboard drawn: the zones in colour, the split points as handles, the
// keys lighting as they are played.
class KeysView : public juce::Component {
public:
  std::function<void(int index, int note)> onSplitMoved;

  void show(const Layout& l, const split::State& s, bool live) {
    layout_ = l;
    state_ = s;
    live_ = live;
    repaint();
  }

  void paint(juce::Graphics& g) override {
    const auto area = getLocalBounds().toFloat();
    const float labelH = 16.0f;
    const auto keys = area.withTrimmedTop(labelH);
    const int whites = whiteCount();
    const float w = keys.getWidth() / static_cast<float>(whites);
    const int first = live_ ? state_.firstSplit : layout_.splits[0];

    // White keys, then black over them.
    for (int pass = 0; pass < 2; ++pass)
      for (int n = kLowKey; n <= kHighKey; ++n) {
        if (isBlack(n) != (pass == 1)) continue;
        const auto r = keyRect(n, keys, w);
        const int zone = layout_.zoneOf(n, first);
        const bool silent = layout_.zones[zone].role == Role::None && layout_.zones[zone].layer == Role::None;
        juce::Colour c = silent ? juce::Colour(0xff555a63) : kZoneColour[zone];
        juce::Colour base = isBlack(n) ? juce::Colour(0xff1b1d22) : juce::Colours::white;
        base = base.interpolatedWith(c, isBlack(n) ? 0.35f : 0.28f);
        const bool down = live_ && (state_.held[n / 64] >> (n % 64) & 1);
        if (down) {
          const int pressed = state_.zoneOfKey[n] > 0 ? state_.zoneOfKey[n] - 1 : zone;
          base = kZoneColour[pressed].brighter(0.2f);
        }
        g.setColour(base);
        g.fillRect(r);
        g.setColour(juce::Colour(0xff2a2d33));
        g.drawRect(r, 1.0f);
        if (live_ && state_.pedalNote == n) {
          g.setColour(juce::Colours::black);
          g.fillEllipse(r.getCentreX() - 3.0f, r.getBottom() - 10.0f, 6.0f, 6.0f);
        }
      }

    // The bass range, under the keys.
    if (layout_.bass.on) {
      const float right = keyRect(std::min(layout_.bass.topKey, kHighKey), keys, w).getRight();
      g.setColour(juce::Colour(0xffe0a030).withAlpha(0.9f));
      g.fillRect(keys.getX(), keys.getBottom() - 3.0f, right - keys.getX(), 3.0f);
    }

    // Split points: handles to drag, and the zones' roles above them.
    g.setFont(juce::FontOptions(12.0f));
    int from = kLowKey;
    for (int z = 0; z <= layout_.splitCount; ++z) {
      const int to = z < layout_.splitCount ? (z == 0 ? first : layout_.splits[z]) - 1 : kHighKey;
      const float x0 = keyRect(std::max(from, kLowKey), keys, w).getX();
      const float x1 = keyRect(std::min(std::max(to, kLowKey), kHighKey), keys, w).getRight();
      juce::String text = roleText(layout_.zones[z].role);
      if (layout_.zones[z].layer != Role::None) text << " + " << roleText(layout_.zones[z].layer);
      g.setColour(kZoneColour[z]);
      g.drawFittedText(text, juce::Rectangle<float>(x0, 0.0f, x1 - x0, labelH).toNearestInt(),
                       juce::Justification::centred, 1);
      from = to + 1;
    }
    for (int i = 0; i < layout_.splitCount; ++i) {
      const int at = i == 0 ? first : layout_.splits[i];
      const float x = keyRect(std::clamp(at, kLowKey, kHighKey), keys, w).getX();
      g.setColour(juce::Colours::white);
      g.fillRect(x - 1.5f, keys.getY(), 3.0f, keys.getHeight());
      g.fillEllipse(x - 5.0f, keys.getY() - 5.0f, 10.0f, 10.0f);
    }
  }

  void mouseDown(const juce::MouseEvent& e) override {
    dragging_ = -1;
    float best = 8.0f;
    const auto keys = getLocalBounds().toFloat().withTrimmedTop(16.0f);
    const float w = keys.getWidth() / static_cast<float>(whiteCount());
    for (int i = 0; i < layout_.splitCount; ++i) {
      const float x = keyRect(std::clamp(layout_.splits[i], kLowKey, kHighKey), keys, w).getX();
      if (std::abs(x - static_cast<float>(e.x)) < best) {
        best = std::abs(x - static_cast<float>(e.x));
        dragging_ = i;
      }
    }
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (dragging_ < 0) return;
    const int note = noteAt(static_cast<float>(e.x));
    if (note != layout_.splits[dragging_] && onSplitMoved) onSplitMoved(dragging_, note);
  }
  void mouseUp(const juce::MouseEvent&) override { dragging_ = -1; }

private:
  static int whiteCount() {
    int c = 0;
    for (int n = kLowKey; n <= kHighKey; ++n)
      if (!isBlack(n)) ++c;
    return c;
  }
  static juce::Rectangle<float> keyRect(int n, juce::Rectangle<float> keys, float w) {
    int white = 0;
    for (int k = kLowKey; k < n; ++k)
      if (!isBlack(k)) ++white;
    if (!isBlack(n)) return {keys.getX() + static_cast<float>(white) * w, keys.getY(), w, keys.getHeight()};
    return {keys.getX() + static_cast<float>(white) * w - w * 0.3f, keys.getY(), w * 0.6f, keys.getHeight() * 0.6f};
  }
  // The key whose left edge is nearest: a split point is the first key of a zone.
  int noteAt(float x) const {
    const auto keys = getLocalBounds().toFloat().withTrimmedTop(16.0f);
    const float w = keys.getWidth() / static_cast<float>(whiteCount());
    int best = kLowKey;
    float bestDist = 1e9f;
    for (int n = kLowKey; n <= kHighKey; ++n) {
      const float d = std::abs(keyRect(n, keys, w).getX() - x);
      if (d < bestDist) {
        bestDist = d;
        best = n;
      }
    }
    return best;
  }

  Layout layout_;
  split::State state_;
  bool live_ = false;
  int dragging_ = -1;
};

class SplitKeyboardPanel : public juce::Component, private juce::Timer {
public:
  explicit SplitKeyboardPanel(MasterpieceProcessor& p) : proc_(p), host_(p.splitKeyboard()) {
    addAndMakeVisible(onOff_);
    onOff_.setButtonText(TRANS("Split keyboard on"));
    onOff_.onClick = [this] { host_.setEnabled(onOff_.getToggleState()); };

    addAndMakeVisible(sourceLabel_);
    styleLabel(sourceLabel_, TRANS("Keyboard"));
    addAndMakeVisible(source_);
    source_.onChange = [this] {
      const int i = source_.getSelectedItemIndex();
      host_.setSource(i <= 0 || i > sourceNames_.size() ? std::string() : sourceNames_[i - 1].toStdString());
    };

    addAndMakeVisible(presetsHeading_);
    styleHeading(presetsHeading_, TRANS("Presets"));
    for (int i = 0; i < split::kPresetCount; ++i) {
      auto& b = presets_[static_cast<size_t>(i)];
      addAndMakeVisible(b);
      b.setButtonText(juce::String(i + 1));
      b.setClickingTogglesState(false);
      b.onClick = [this, i] { host_.recall(i); };
    }
    addAndMakeVisible(saveAs_);
    saveAs_.setButtonText(TRANS("Save as..."));
    saveAs_.onClick = [this] { askSave(); };
    addAndMakeVisible(delete_);
    delete_.setButtonText(TRANS("Delete"));
    delete_.onClick = [this] {
      if (host_.preset() >= split::kFactoryPresets) host_.deletePreset(host_.preset());
    };

    addAndMakeVisible(keys_);
    keys_.onSplitMoved = [this](int i, int note) {
      Layout l = host_.layout();
      l.splits[i] = note;
      if (i > 0 && note <= l.splits[i - 1]) return;
      if (i + 1 < l.splitCount && note >= l.splits[i + 1]) return;
      apply(l);
    };

    addAndMakeVisible(layoutHeading_);
    styleHeading(layoutHeading_, TRANS("Zones"));
    addAndMakeVisible(splitCountLabel_);
    styleLabel(splitCountLabel_, TRANS("Split points"));
    addAndMakeVisible(splitCount_);
    for (int n = 0; n <= split::kMaxSplits; ++n) splitCount_.addItem(juce::String(n), n + 1);
    splitCount_.onChange = [this] {
      Layout l = host_.layout();
      l.splitCount = splitCount_.getSelectedId() - 1;
      apply(l);
    };
    for (int i = 0; i < split::kMaxSplits; ++i) {
      auto& s = splits_[static_cast<size_t>(i)];
      addAndMakeVisible(s);
      noteSlider(s);
      s.onValueChange = [this, i] {
        Layout l = host_.layout();
        l.splits[i] = static_cast<int>(splits_[static_cast<size_t>(i)].getValue());
        apply(l);
      };
      auto& b = learnSplit_[static_cast<size_t>(i)];
      addAndMakeVisible(b);
      b.setButtonText(TRANS("Press a key"));
      b.setTooltip(TRANS("The next key pressed on the keyboard becomes this split point. The split has to be on."));
      b.onClick = [this, i] {
        learningSplit_ = learningSplit_ == i ? -1 : i;
        learnBaseline_[0] = host_.state().held[0];
        learnBaseline_[1] = host_.state().held[1];
        refreshLearnButtons();
      };
    }
    addAndMakeVisible(transposeLabel_);
    styleLabel(transposeLabel_, TRANS("Transpose all"));
    addAndMakeVisible(transpose_);
    stepSlider(transpose_, -12, 12);
    transpose_.onValueChange = [this] {
      Layout l = host_.layout();
      l.transpose = static_cast<int>(transpose_.getValue());
      apply(l);
    };
    addAndMakeVisible(follow_);
    follow_.setButtonText(TRANS("The first split follows the hands"));
    follow_.setTooltip(TRANS("With keys held more than an octave apart, the gap between the hands becomes the "
                             "first split point, and it stays there a few seconds after the keys are released."));
    follow_.onClick = [this] {
      Layout l = host_.layout();
      l.followHands = follow_.getToggleState();
      apply(l);
    };

    for (int z = 0; z < split::kMaxZones; ++z) {
      const auto zi = static_cast<size_t>(z);
      addAndMakeVisible(zoneLabel_[zi]);
      styleLabel(zoneLabel_[zi], {});
      zoneLabel_[zi].setColour(juce::Label::textColourId, kZoneColour[z]);
      addAndMakeVisible(role_[zi]);
      roleBox(role_[zi]);
      role_[zi].onChange = [this, z] {
        Layout l = host_.layout();
        l.zones[z].role = static_cast<Role>(role_[static_cast<size_t>(z)].getSelectedId() - 1);
        apply(l);
      };
      addAndMakeVisible(layer_[zi]);
      roleBox(layer_[zi]);
      layer_[zi].setTooltip(TRANS("A second role played by the same keys"));
      layer_[zi].onChange = [this, z] {
        Layout l = host_.layout();
        l.zones[z].layer = static_cast<Role>(layer_[static_cast<size_t>(z)].getSelectedId() - 1);
        apply(l);
      };
      addAndMakeVisible(octave_[zi]);
      stepSlider(octave_[zi], -3, 3);
      octave_[zi].setTooltip(TRANS("Octaves"));
      octave_[zi].onValueChange = [this, z] {
        Layout l = host_.layout();
        l.zones[z].octave = static_cast<int>(octave_[static_cast<size_t>(z)].getValue());
        apply(l);
      };
      addAndMakeVisible(zoneTranspose_[zi]);
      stepSlider(zoneTranspose_[zi], -12, 12);
      zoneTranspose_[zi].setTooltip(TRANS("Semitones"));
      zoneTranspose_[zi].onValueChange = [this, z] {
        Layout l = host_.layout();
        l.zones[z].transpose = static_cast<int>(zoneTranspose_[static_cast<size_t>(z)].getValue());
        apply(l);
      };
    }
    for (auto* l : {&roleHeader_, &layerHeader_, &octaveHeader_, &semitoneHeader_}) addAndMakeVisible(*l);
    styleLabel(roleHeader_, TRANS("Plays"));
    styleLabel(layerHeader_, TRANS("And also"));
    styleLabel(octaveHeader_, TRANS("Octaves"));
    styleLabel(semitoneHeader_, TRANS("Semitones"));

    addAndMakeVisible(bassHeading_);
    styleHeading(bassHeading_, TRANS("Bass from the hands"));
    addAndMakeVisible(bassOn_);
    bassOn_.setButtonText(TRANS("The lowest key held plays the pedal"));
    bassOn_.onClick = [this] {
      Layout l = host_.layout();
      l.bass.on = bassOn_.getToggleState();
      apply(l);
    };
    addAndMakeVisible(bassTopLabel_);
    styleLabel(bassTopLabel_, TRANS("From the bottom up to"));
    addAndMakeVisible(bassTop_);
    noteSlider(bassTop_);
    bassTop_.setRange(0, 127, 1);
    bassTop_.onValueChange = [this] {
      Layout l = host_.layout();
      l.bass.topKey = static_cast<int>(bassTop_.getValue());
      apply(l);
    };
    addAndMakeVisible(bassManual_);
    bassManual_.setButtonText(TRANS("The bass key also plays its manual"));
    bassManual_.onClick = [this] {
      Layout l = host_.layout();
      l.bass.alsoOnManual = bassManual_.getToggleState();
      apply(l);
    };
    addAndMakeVisible(sustainLabel_);
    styleLabel(sustainLabel_, TRANS("After the hand lifts"));
    addAndMakeVisible(sustain_);
    sustain_.addItem(TRANS("The pedal note ends"), 1);
    sustain_.addItem(TRANS("It holds until the next bass note"), 2);
    sustain_.addItem(TRANS("It holds for a time"), 3);
    sustain_.onChange = [this] {
      Layout l = host_.layout();
      l.bass.sustain = static_cast<split::Sustain>(sustain_.getSelectedId() - 1);
      apply(l);
    };
    addAndMakeVisible(sustainMs_);
    sustainMs_.setSliderStyle(juce::Slider::IncDecButtons);
    sustainMs_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, kRow - 4);
    sustainMs_.setRange(50, 10000, 50);
    sustainMs_.setTextValueSuffix(" ms");
    sustainMs_.onValueChange = [this] {
      Layout l = host_.layout();
      l.bass.sustainMs = static_cast<int>(sustainMs_.getValue());
      apply(l);
    };

    addAndMakeVisible(rolesHeading_);
    styleHeading(rolesHeading_, TRANS("Manuals on this organ"));
    for (int r = 0; r < split::kRoleCount; ++r) {
      const auto ri = static_cast<size_t>(r);
      addAndMakeVisible(roleKbLabel_[ri]);
      styleLabel(roleKbLabel_[ri], roleText(static_cast<Role>(r + 1)));
      addAndMakeVisible(roleKb_[ri]);
      roleKb_[ri].onChange = [this, r] {
        const int i = roleKb_[static_cast<size_t>(r)].getSelectedItemIndex();
        host_.setKeyboardFor(static_cast<Role>(r + 1),
                             i <= 0 || i > static_cast<int>(keyboardIds_.size()) ? Id(0)
                                                                                 : keyboardIds_[static_cast<size_t>(i - 1)]);
      };
    }

    addAndMakeVisible(controlsHeading_);
    styleHeading(controlsHeading_, TRANS("MIDI controls"));
    for (int c = 0; c < split::kControlCount; ++c) {
      const auto ci = static_cast<size_t>(c);
      addAndMakeVisible(controlLabel_[ci]);
      styleLabel(controlLabel_[ci], controlText(c));
      addAndMakeVisible(controlLearn_[ci]);
      controlLearn_[ci].setTooltip(TRANS("Click, then press a key, a button or a pedal on any MIDI input"));
      controlLearn_[ci].onClick = [this, c] { host_.learn(host_.state().learning == c ? -1 : c); };
      addAndMakeVisible(controlClear_[ci]);
      controlClear_[ci].setButtonText("x");
      controlClear_[ci].setTooltip(TRANS("Forget this control"));
      controlClear_[ci].onClick = [this, c] { host_.clearControl(c); };
    }


    fillSources();
    fillKeyboards();
    refresh(true);
    setSize(760, 980);
    startTimerHz(30);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(12);
    auto row = [&](int h = kRow) {
      auto x = r.removeFromTop(h);
      r.removeFromTop(4);
      return x;
    };
    {
      auto x = row();
      onOff_.setBounds(x.removeFromLeft(220));
      sourceLabel_.setBounds(x.removeFromLeft(80));
      source_.setBounds(x.removeFromLeft(280));
    }
    presetsHeading_.setBounds(row(22));
    {
      auto x = row();
      for (auto& b : presets_) {
        b.setBounds(x.removeFromLeft(40));
        x.removeFromLeft(4);
      }
      x.removeFromLeft(12);
      saveAs_.setBounds(x.removeFromLeft(100));
      x.removeFromLeft(6);
      delete_.setBounds(x.removeFromLeft(80));
    }
    r.removeFromTop(6);
    keys_.setBounds(row(110));
    layoutHeading_.setBounds(row(22));
    {
      auto x = row();
      splitCountLabel_.setBounds(x.removeFromLeft(110));
      splitCount_.setBounds(x.removeFromLeft(60));
      x.removeFromLeft(20);
      transposeLabel_.setBounds(x.removeFromLeft(100));
      transpose_.setBounds(x.removeFromLeft(110));
      x.removeFromLeft(20);
      follow_.setBounds(x);
    }
    // Only the split points and zones in use take a row.
    const int splits = host_.layout().splitCount;
    laidOutSplits_ = splits;
    for (int i = 0; i < splits; ++i) {
      auto x = row();
      x.removeFromLeft(110);
      splits_[static_cast<size_t>(i)].setBounds(x.removeFromLeft(130));
      x.removeFromLeft(8);
      learnSplit_[static_cast<size_t>(i)].setBounds(x.removeFromLeft(110));
    }
    {
      auto x = row(18);
      x.removeFromLeft(150);
      roleHeader_.setBounds(x.removeFromLeft(130));
      layerHeader_.setBounds(x.removeFromLeft(130));
      octaveHeader_.setBounds(x.removeFromLeft(110));
      semitoneHeader_.setBounds(x.removeFromLeft(110));
    }
    for (int z = 0; z <= splits; ++z) {
      const auto zi = static_cast<size_t>(z);
      auto x = row();
      zoneLabel_[zi].setBounds(x.removeFromLeft(150));
      role_[zi].setBounds(x.removeFromLeft(124));
      x.removeFromLeft(6);
      layer_[zi].setBounds(x.removeFromLeft(124));
      x.removeFromLeft(6);
      octave_[zi].setBounds(x.removeFromLeft(104));
      x.removeFromLeft(6);
      zoneTranspose_[zi].setBounds(x.removeFromLeft(104));
    }
    r.removeFromTop(6);
    bassHeading_.setBounds(row(22));
    bassOn_.setBounds(row());
    {
      auto x = row();
      bassTopLabel_.setBounds(x.removeFromLeft(170));
      bassTop_.setBounds(x.removeFromLeft(130));
      x.removeFromLeft(20);
      bassManual_.setBounds(x);
    }
    {
      auto x = row();
      sustainLabel_.setBounds(x.removeFromLeft(170));
      sustain_.setBounds(x.removeFromLeft(280));
      x.removeFromLeft(8);
      sustainMs_.setBounds(x.removeFromLeft(150));
    }
    r.removeFromTop(6);
    rolesHeading_.setBounds(row(22));
    for (int i = 0; i < split::kRoleCount; i += 2) {
      auto x = row();
      for (int k = i; k < i + 2; ++k) {
        roleKbLabel_[static_cast<size_t>(k)].setBounds(x.removeFromLeft(70));
        roleKb_[static_cast<size_t>(k)].setBounds(x.removeFromLeft(280));
        x.removeFromLeft(16);
      }
    }
    r.removeFromTop(6);
    controlsHeading_.setBounds(row(22));
    for (int c = 0; c < split::kControlCount; c += 2) {
      auto x = row();
      for (int k = c; k < c + 2 && k < split::kControlCount; ++k) {
        const auto ki = static_cast<size_t>(k);
        controlLabel_[ki].setBounds(x.removeFromLeft(120));
        controlLearn_[ki].setBounds(x.removeFromLeft(200));
        x.removeFromLeft(4);
        controlClear_[ki].setBounds(x.removeFromLeft(26));
        x.removeFromLeft(16);
      }
    }
    r.removeFromTop(6);
  }

  void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff15171c)); }

private:
  void timerCallback() override {
    host_.service();
    const auto& s = host_.state();
    // A split point set by pressing a key.
    if (learningSplit_ >= 0) {
      for (int n = 0; n < 128; ++n) {
        const bool now = s.held[n / 64] >> (n % 64) & 1;
        const bool before = learnBaseline_[n / 64] >> (n % 64) & 1;
        if (now && !before) {
          Layout l = host_.layout();
          const int i = learningSplit_;
          learningSplit_ = -1;
          const bool fits = (i == 0 || n > l.splits[i - 1]) && (i + 1 >= l.splitCount || n < l.splits[i + 1]);
          if (fits) {
            l.splits[i] = n;
            apply(l);
          }
          refreshLearnButtons();
          break;
        }
      }
      learnBaseline_[0] = s.held[0];
      learnBaseline_[1] = s.held[1];
    }
    refresh(false);
    keys_.show(host_.layout(), s, s.enabled);
  }

  void apply(const Layout& l) {
    host_.setLayout(l);
    refresh(true);
  }

  void fillSources() {
    sourceNames_.clear();
    source_.clear(juce::dontSendNotification);
    source_.addItem(TRANS("Any input"), 1);
    const auto& devices = proc_.midiDevices();
    for (const auto& n : devices.names())
      if (n != SplitKeyboardHost::kInputName) sourceNames_.add(n);
    for (int i = 0; i < sourceNames_.size(); ++i) source_.addItem(sourceNames_[i], i + 2);
    const auto chosen = juce::String(host_.shared().source);
    const int at = sourceNames_.indexOf(chosen);
    source_.setSelectedItemIndex(at >= 0 ? at + 1 : 0, juce::dontSendNotification);
  }

  void fillKeyboards() {
    keyboardIds_.clear();
    const OrganModel* model = host_.model();
    for (auto& box : roleKb_) {
      box.clear(juce::dontSendNotification);
      box.addItem(TRANS("Nothing"), 1);
    }
    if (model == nullptr) return;
    std::vector<std::pair<Id, std::string>> kbs;
    for (const Id id : host_.keyboards())
      if (const auto it = model->keyboards.find(id); it != model->keyboards.end()) kbs.emplace_back(id, it->second.name);
    std::sort(kbs.begin(), kbs.end());
    for (const auto& [id, name] : kbs) {
      keyboardIds_.push_back(id);
      for (auto& box : roleKb_) box.addItem(juce::String::fromUTF8(name.c_str()), static_cast<int>(keyboardIds_.size()) + 1);
    }
    shownLoads_ = host_.organLoads();
  }

  void refreshLearnButtons() {
    for (int i = 0; i < split::kMaxSplits; ++i)
      learnSplit_[static_cast<size_t>(i)].setButtonText(learningSplit_ == i ? TRANS("Press a key now...")
                                                                            : TRANS("Press a key"));
  }

  // The widgets from the host, when something changed under them.
  void refresh(bool force) {
    const Layout& l = host_.layout();
    const auto& s = host_.state();
    if (host_.organLoads() != shownLoads_) {
      fillKeyboards();
      force = true;
    }
    if (proc_.midiDevices().names().size() != static_cast<size_t>(sourceNames_.size()) + 1) fillSources();

    onOff_.setToggleState(host_.enabled(), juce::dontSendNotification);
    for (int i = 0; i < split::kPresetCount; ++i) {
      auto& b = presets_[static_cast<size_t>(i)];
      const auto name = juce::String(host_.presetName(i));
      b.setEnabled(name.isNotEmpty());
      b.setTooltip(i < split::kFactoryPresets ? factoryText(i) : name);
      b.setColour(juce::TextButton::buttonColourId,
                  host_.preset() == i ? juce::Colour(0xff4a90d9) : juce::Colour(0xff2a2d33));
    }
    delete_.setEnabled(host_.preset() >= split::kFactoryPresets);

    for (int c = 0; c < split::kControlCount; ++c) {
      const auto ci = static_cast<size_t>(c);
      const auto& t = host_.shared().triggers[ci];
      controlLearn_[ci].setButtonText(s.learning == c ? TRANS("Press it now...") : triggerText(t));
      controlClear_[ci].setEnabled(t.kind != split::Trigger::None);
    }
    for (int r = 0; r < split::kRoleCount; ++r) {
      const Id kb = host_.keyboardFor(static_cast<Role>(r + 1));
      const auto it = std::find(keyboardIds_.begin(), keyboardIds_.end(), kb);
      roleKb_[static_cast<size_t>(r)].setSelectedItemIndex(
          it == keyboardIds_.end() ? 0 : static_cast<int>(it - keyboardIds_.begin()) + 1, juce::dontSendNotification);
    }

    if (!force && l == shown_) return;
    shown_ = l;
    if (l.splitCount != laidOutSplits_) resized();
    splitCount_.setSelectedId(l.splitCount + 1, juce::dontSendNotification);
    for (int i = 0; i < split::kMaxSplits; ++i) {
      auto& sl = splits_[static_cast<size_t>(i)];
      sl.setValue(l.splits[i], juce::dontSendNotification);
      sl.setVisible(i < l.splitCount);
      learnSplit_[static_cast<size_t>(i)].setVisible(i < l.splitCount);
    }
    transpose_.setValue(l.transpose, juce::dontSendNotification);
    follow_.setToggleState(l.followHands, juce::dontSendNotification);
    follow_.setEnabled(l.splitCount > 0);
    int from = 0;
    for (int z = 0; z < split::kMaxZones; ++z) {
      const auto zi = static_cast<size_t>(z);
      const bool used = z <= l.splitCount;
      const int to = z < l.splitCount ? l.splits[z] - 1 : 127;
      zoneLabel_[zi].setText(TRANS("Zone %1: %2 to %3")
                                 .replace("%1", juce::String(z + 1))
                                 .replace("%2", noteName(from))
                                 .replace("%3", noteName(to)),
                             juce::dontSendNotification);
      from = to + 1;
      for (juce::Component* c : std::initializer_list<juce::Component*>{&zoneLabel_[zi], &role_[zi], &layer_[zi],
                                                                       &octave_[zi], &zoneTranspose_[zi]})
        c->setVisible(used);
      role_[zi].setSelectedId(static_cast<int>(l.zones[z].role) + 1, juce::dontSendNotification);
      layer_[zi].setSelectedId(static_cast<int>(l.zones[z].layer) + 1, juce::dontSendNotification);
      octave_[zi].setValue(l.zones[z].octave, juce::dontSendNotification);
      zoneTranspose_[zi].setValue(l.zones[z].transpose, juce::dontSendNotification);
    }
    bassOn_.setToggleState(l.bass.on, juce::dontSendNotification);
    bassTop_.setValue(l.bass.topKey, juce::dontSendNotification);
    bassManual_.setToggleState(l.bass.alsoOnManual, juce::dontSendNotification);
    sustain_.setSelectedId(static_cast<int>(l.bass.sustain) + 1, juce::dontSendNotification);
    sustainMs_.setValue(l.bass.sustainMs, juce::dontSendNotification);
    sustainMs_.setEnabled(l.bass.sustain == split::Sustain::Timed);
    for (juce::Component* c :
         std::initializer_list<juce::Component*>{&bassTop_, &bassManual_, &sustain_, &bassTopLabel_, &sustainLabel_})
      c->setEnabled(l.bass.on);
  }

  void askSave() {
    // The first free slot, else the one recalled, else the first of the player's.
    int slot = -1;
    for (int i = split::kFactoryPresets; i < split::kPresetCount && slot < 0; ++i)
      if (host_.presetName(i).empty() && !host_.shared().presets.used[i]) slot = i;
    if (slot < 0) slot = host_.preset() >= split::kFactoryPresets ? host_.preset() : split::kFactoryPresets;
    prompt_ = std::make_unique<juce::AlertWindow>(TRANS("Save as a preset"),
                                                  TRANS("The layout as it is now, in a slot every organ can use."),
                                                  juce::MessageBoxIconType::NoIcon);
    prompt_->addTextEditor("name", host_.presetName(slot).empty() ? TRANS("My layout") : juce::String(host_.presetName(slot)),
                           TRANS("Name"));
    juce::StringArray slots;
    for (int i = split::kFactoryPresets; i < split::kPresetCount; ++i) {
      const auto name = juce::String(host_.presetName(i));
      slots.add(juce::String(i + 1) + (name.isEmpty() ? juce::String() : " - " + name));
    }
    prompt_->addComboBox("slot", slots, TRANS("Slot"));
    if (auto* c = prompt_->getComboBoxComponent("slot")) c->setSelectedItemIndex(slot - split::kFactoryPresets);
    prompt_->addButton(TRANS("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    prompt_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    prompt_->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
      if (result == 1 && prompt_ != nullptr) {
        const auto name = prompt_->getTextEditorContents("name").trim();
        const int chosen = prompt_->getComboBoxComponent("slot")->getSelectedItemIndex() + split::kFactoryPresets;
        host_.saveAsPreset(chosen, name.toStdString());
        refresh(true);
      }
      prompt_.reset();
    }), false);
  }

  MasterpieceProcessor& proc_;
  SplitKeyboardHost& host_;
  Layout shown_;
  uint32_t shownLoads_ = 0;
  int learningSplit_ = -1;
  int laidOutSplits_ = -1;
  uint64_t learnBaseline_[2] = {};
  juce::StringArray sourceNames_;
  std::vector<Id> keyboardIds_;
  std::unique_ptr<juce::AlertWindow> prompt_;

  juce::ToggleButton onOff_;
  juce::Label sourceLabel_;
  juce::ComboBox source_;
  juce::Label presetsHeading_;
  std::array<juce::TextButton, split::kPresetCount> presets_;
  juce::TextButton saveAs_, delete_;
  KeysView keys_;
  juce::Label layoutHeading_, splitCountLabel_, transposeLabel_;
  juce::ComboBox splitCount_;
  std::array<juce::Slider, split::kMaxSplits> splits_;
  std::array<juce::TextButton, split::kMaxSplits> learnSplit_;
  juce::Slider transpose_;
  juce::ToggleButton follow_;
  juce::Label roleHeader_, layerHeader_, octaveHeader_, semitoneHeader_;
  std::array<juce::Label, split::kMaxZones> zoneLabel_;
  std::array<juce::ComboBox, split::kMaxZones> role_, layer_;
  std::array<juce::Slider, split::kMaxZones> octave_, zoneTranspose_;
  juce::Label bassHeading_, bassTopLabel_, sustainLabel_;
  juce::ToggleButton bassOn_, bassManual_;
  juce::Slider bassTop_, sustainMs_;
  juce::ComboBox sustain_;
  juce::Label rolesHeading_;
  std::array<juce::Label, split::kRoleCount> roleKbLabel_;
  std::array<juce::ComboBox, split::kRoleCount> roleKb_;
  juce::Label controlsHeading_;
  std::array<juce::Label, split::kControlCount> controlLabel_;
  std::array<juce::TextButton, split::kControlCount> controlLearn_, controlClear_;
};

// The panel in a viewport: on a small screen it scrolls.
class SplitKeyboardContent : public juce::Component {
public:
  explicit SplitKeyboardContent(MasterpieceProcessor& p) : panel_(p) {
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&panel_, false);
    viewport_.setScrollBarsShown(true, false);
    setSize(panel_.getWidth() + viewport_.getScrollBarThickness(), 720);
  }
  void resized() override {
    viewport_.setBounds(getLocalBounds());
    panel_.setSize(std::max(getWidth() - viewport_.getScrollBarThickness(), 600), panel_.getHeight());
  }

private:
  SplitKeyboardPanel panel_;
  juce::Viewport viewport_;
};

}  // namespace

SplitKeyboardWindow::SplitKeyboardWindow(MasterpieceProcessor& p)
    : juce::DocumentWindow(TRANS("Split keyboard"), juce::Colour(0xff15171c), juce::DocumentWindow::closeButton) {
  setUsingNativeTitleBar(true);
  setContentOwned(new SplitKeyboardContent(p), true);
  setResizable(true, false);
  fitToScreen(*this);
}

void SplitKeyboardWindow::closeButtonPressed() { showFloating(*this, false, false); }

}  // namespace mp::ui
