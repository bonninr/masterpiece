// A MusicXML score, engraved by Verovio and drawn to fit its panel.
//
// Experimental: the page is engraved to SVG at the panel's size and drawn by
// JUCE's SVG renderer. The panel is re-engraved a moment after it stops being
// resized, so the staves keep their size and the lines break again to the new
// width.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_score/ScoreEngraver.h"

#include <memory>

namespace mp::ui {

// The font and glyph data Verovio reads, written once to the app-data folder
// from the copy built into the program. An empty File when it cannot be written.
juce::File scoreResourceFolder();
// The same into any folder, which is emptied first. False when it fails.
bool unpackScoreFonts(const juce::File& folder);

class ScorePanel : public juce::Component, private juce::Timer {
public:
  // The fonts come from `fonts` when given, otherwise from scoreResourceFolder().
  explicit ScorePanel(const juce::File& fonts = {});
  ~ScorePanel() override;

  // Loads a MusicXML (.musicxml, .xml) or compressed (.mxl) file. False, with
  // the reason shown in the panel, when it cannot be read.
  bool openFile(const juce::File& file);
  // The same for the bytes of such a file.
  bool openData(const juce::MemoryBlock& bytes, const juce::String& name);

  int page() const { return page_; }
  int pageCount() const;
  // The panel as drawn now, for tests: engraves at once if a re-engraving is
  // pending.
  juce::Image render();

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void timerCallback() override;
  void engrave();
  void showPage(int page);
  void chooseFile();
  juce::Rectangle<int> pageArea() const;

  std::unique_ptr<mp::ScoreEngraver> engraver_;
  std::unique_ptr<juce::Drawable> drawable_;
  juce::TextButton open_{"Open a score..."};
  juce::TextButton prev_{"<"};
  juce::TextButton next_{">"};
  juce::Label status_;
  std::unique_ptr<juce::FileChooser> chooser_;
  int page_ = 1;
  bool loaded_ = false;
  juce::String message_;
};

// Shows the panel in a window of its own: made on the first call, hidden or
// shown after that.
void toggleScoreWindow(std::unique_ptr<juce::DocumentWindow>& slot,
                       juce::Rectangle<int> besideThis);

}  // namespace mp::ui
