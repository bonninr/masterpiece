#include "ScoreEngraver.h"

#include <vrv/toolkit.h>

#include <algorithm>
#include <cstdio>

namespace mp {

struct ScoreEngraver::Impl {
  vrv::Toolkit tk{false};
  int widthPx = 0, heightPx = 0, scale = 0;
};

ScoreEngraver::ScoreEngraver(const std::string& resourceDir) : impl_(new Impl) {
  ready_ = impl_->tk.SetResourcePath(resourceDir);
  if (!ready_) error_ = "The score fonts could not be loaded from " + resourceDir;
}

ScoreEngraver::~ScoreEngraver() = default;

void ScoreEngraver::setPage(int widthPx, int heightPx, int scalePercent) {
  widthPx = std::max(widthPx, 100);
  heightPx = std::max(heightPx, 100);
  scalePercent = std::clamp(scalePercent, 10, 400);
  Impl& i = *impl_;
  if (i.widthPx == widthPx && i.heightPx == heightPx && i.scale == scalePercent) return;
  i.widthPx = widthPx;
  i.heightPx = heightPx;
  i.scale = scalePercent;
  // The page is given in Verovio units, which the scale shrinks or grows to
  // pixels: pixels = units * scale / 100. The first page opens with the
  // score's title, composer and other credits. Music symbols inside text are
  // drawn from the Bravura the program registers, so no page carries its own
  // copy of the font: a page is half the size without it.
  char opts[384];
  std::snprintf(opts, sizeof opts,
                "{\"pageWidth\":%d,\"pageHeight\":%d,\"scale\":%d,\"breaks\":\"auto\","
                "\"adjustPageHeight\":false,\"header\":\"auto\",\"footer\":\"none\","
                "\"font\":\"Bravura\",\"smuflTextFont\":\"none\",\"svgViewBox\":false,\"svgRemoveXlink\":false}",
                widthPx * 100 / scalePercent, heightPx * 100 / scalePercent, scalePercent);
  i.tk.SetOptions(opts);
  if (loaded_) i.tk.RedoLayout();
}

bool ScoreEngraver::load(const std::string& musicXml) {
  loaded_ = false;
  error_.clear();
  if (!ready_) {
    error_ = "The score fonts are not available";
    return false;
  }
  if (musicXml.empty()) {
    error_ = "The file is empty";
    return false;
  }
  // Verovio would take any text and engrave an empty page.
  if (musicXml.find("<score-partwise") == std::string::npos &&
      musicXml.find("<score-timewise") == std::string::npos) {
    error_ = "This is not a MusicXML score";
    return false;
  }
  vrv::Toolkit& tk = impl_->tk;
  tk.SetOptions("{\"inputFrom\":\"musicxml\"}");
  if (!tk.LoadData(musicXml) || tk.GetPageCount() < 1) {
    error_ = "This MusicXML score could not be read";
    return false;
  }
  loaded_ = true;
  return true;
}

int ScoreEngraver::pageCount() const { return loaded_ ? impl_->tk.GetPageCount() : 0; }

std::string ScoreEngraver::renderPage(int page) {
  if (!loaded_ || page < 1 || page > pageCount()) return {};
  return impl_->tk.RenderToSVG(page, true);
}

std::string ScoreEngraver::midiBase64() { return loaded_ ? impl_->tk.RenderToMIDI() : std::string(); }

std::string ScoreEngraver::timemapJson() { return loaded_ ? impl_->tk.RenderToTimemap() : std::string(); }

int ScoreEngraver::pageWithElement(const std::string& id) {
  return loaded_ ? impl_->tk.GetPageWithElement(id) : 0;
}

std::string ScoreEngraver::elementsAtTime(int milliseconds) {
  return loaded_ ? impl_->tk.GetElementsAtTime(milliseconds) : std::string();
}

} // namespace mp
