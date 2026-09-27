// Organ settings: what belongs to the organ loaded now, as against the
// program and the player's console (General settings, SettingsWindow).
//
//   Loading  how the organ's samples are held -- format, mono, streaming,
//            cache. The Engine page, unchanged.
//   Stops    which stops to load at all. A stop left out costs no memory and
//            makes no sound; everything else about it -- its drawstop, its
//            place in pistons -- stays, dimmed, so it is not taken for broken.
//
// It opens by itself before an organ's first load, once the console is up
// without its audio, because this is the moment those choices decide whether
// the organ fits.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../mp_audio/MasterpieceProcessor.h"
#include "Settings.h"

#include <functional>
#include <atomic>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace mp::ui {

class StopsLoadPanel : public juce::Component, private juce::Timer {
public:
  // `reload` loads the organ again with the choice made here.
  StopsLoadPanel(MasterpieceProcessor& p, std::function<void()> reload);
  ~StopsLoadPanel() override;
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  struct Row {
    Id stopId = 0;
    bool effect = false;  // no drawstop: a noise or an effect the set keeps in a stop
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::Label> size;
    std::vector<Id> samples;
  };
  struct Heading {
    int beforeRow = 0;
    std::unique_ptr<juce::Label> label;
  };

  void build();
  void choose(std::set<Id> excluded);
  void refreshFigures();
  void startEstimate();
  // The Loading tab changes the figures (16-bit, mono, streaming) without a
  // word to this one; a slow tick follows it.
  void timerCallback() override { refreshFigures(); }

  MasterpieceProcessor& proc_;
  std::function<void()> reload_;
  juce::Viewport viewport_;
  juce::Component list_;
  std::vector<Row> rows_;
  std::vector<Heading> headings_;

  juce::TextButton all_{"All"}, none_{"None"}, drawn_{"Only those drawn now"};
  juce::TextButton reloadNow_{"Load the organ again"};
  juce::Label total_;
  juce::Label note_;

  // Each sample's shape, read off the message thread from its file header;
  // turned into bytes at whatever the Loading tab says now.
  std::unordered_map<Id, SampleLibrary::SampleShape> shapes_;
  bool ready_ = false;
  // Tells the worker to stop when the panel goes before it finishes.
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

class OrganSettingsWindow : public juce::Component {
public:
  OrganSettingsWindow(MasterpieceProcessor& p, std::function<void()> reload);
  ~OrganSettingsWindow() override;
  void resized() override;
  void paint(juce::Graphics& g) override;

  // Run once the window has closed, on the message thread: an organ's first
  // load waits here for these choices.
  std::function<void()> onClosed;

private:
  // "Load the organ again" closes the window; the load follows the close, so
  // on a first load -- where closing loads anyway -- it happens once.
  void reloadOnClose();
  std::function<void()> reload_;
  bool reloadOnClose_ = false;
  juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
  EnginePanel engine_;
  ScrollHost engineScroll_{engine_, 720};
  StopsLoadPanel stops_;
};

}  // namespace mp::ui
