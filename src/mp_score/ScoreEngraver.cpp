#include "ScoreEngraver.h"

#include <vrv/toolkit.h>

#include <algorithm>
#include <cstdio>

namespace mp {

struct ScoreEngraver::Impl {
  vrv::Toolkit tk{false};
  int widthPx = 0, heightPx = 0, scale = 0;
};

static bool looksZipped(const std::string& b) {
  return b.size() > 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4;
}

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
  // pixels: pixels = units * scale / 100.
  char opts[320];
  std::snprintf(opts, sizeof opts,
                "{\"pageWidth\":%d,\"pageHeight\":%d,\"scale\":%d,\"breaks\":\"auto\","
                "\"adjustPageHeight\":false,\"header\":\"none\",\"footer\":\"none\","
                "\"font\":\"Bravura\",\"svgViewBox\":false,\"svgRemoveXlink\":false}",
                widthPx * 100 / scalePercent, heightPx * 100 / scalePercent, scalePercent);
  i.tk.SetOptions(opts);
  if (loaded_) i.tk.RedoLayout();
}

bool ScoreEngraver::load(const std::string& bytes) {
  loaded_ = false;
  error_.clear();
  if (!ready_) {
    error_ = "The score fonts are not available";
    return false;
  }
  if (bytes.empty()) {
    error_ = "The file is empty";
    return false;
  }
  // Verovio would take any text and engrave an empty page.
  if (!looksZipped(bytes) && bytes.find("<score-partwise") == std::string::npos &&
      bytes.find("<score-timewise") == std::string::npos) {
    error_ = "This is not a MusicXML score";
    return false;
  }
  vrv::Toolkit& tk = impl_->tk;
  tk.SetOptions("{\"inputFrom\":\"musicxml\"}");
  const bool ok = looksZipped(bytes)
                      ? tk.LoadZipDataBuffer(reinterpret_cast<const unsigned char*>(bytes.data()),
                                             static_cast<int>(bytes.size()))
                      : tk.LoadData(bytes);
  if (!ok || tk.GetPageCount() < 1) {
    error_ = "This is not a MusicXML score Verovio can read";
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

} // namespace mp
