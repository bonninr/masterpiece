#include "JOrganSkin.h"

#include "mp_core/Utf8Path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#if MP_JORGAN_SKINS
#include <juce_graphics/juce_graphics.h>
#include <pugixml.hpp>
#endif

namespace mp {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// The skin a console names, beside the disposition, its name in any case.
std::filesystem::path skinPath(const std::string& dispositionDir, const std::string& name) {
  std::string rel = name;
  std::replace(rel.begin(), rel.end(), '\\', '/');
  rel = rel.substr(rel.find_last_of('/') + 1);
  const std::filesystem::path dir = pathOf(dispositionDir);
  std::error_code ec;
  if (std::filesystem::exists(dir / pathOf(rel), ec)) return dir / pathOf(rel);
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    if (lower(utf8Of(entry.path().filename())) == lower(rel)) return entry.path();
  return {};
}

}  // namespace

std::vector<std::string> jorganSkinFiles(const JOrganDisposition& d, const std::string& dispositionDir) {
  std::vector<std::string> out;
  for (const JOrganElement& e : d.elements) {
    if (e.type != "console") continue;
    const std::string skin = e.prop("skin");
    if (skin.empty()) continue;
    const std::filesystem::path p = skinPath(dispositionDir, skin);
    if (p.empty()) continue;
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
      for (const auto& f : std::filesystem::recursive_directory_iterator(p, ec)) out.push_back(utf8Of(f.path()));
    } else {
      out.push_back(utf8Of(p));
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

#if !MP_JORGAN_SKINS

std::vector<JOrganOrgan::Console> drawJOrganConsoles(const JOrganDisposition&, const std::string&, const std::string&,
                                                     std::vector<std::string>& notes) {
  notes.push_back("skins are left out in this build; the consoles use the stock layout");
  return {};
}

#else

namespace {

// A skin's files: a folder's, or a ZIP file's entries.
struct SkinFiles {
  std::filesystem::path folder;
  std::unique_ptr<juce::ZipFile> zip;

  bool open(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
      folder = p;
      return true;
    }
    zip = std::make_unique<juce::ZipFile>(juce::File(juce::String::fromUTF8(utf8Of(p).c_str())));
    return zip->getNumEntries() > 0;
  }
  bool read(std::string name, juce::MemoryBlock& out) const {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (zip) {
      int i = zip->getIndexOfFileName(juce::String::fromUTF8(name.c_str()), true);
      if (i < 0) return false;
      std::unique_ptr<juce::InputStream> in(zip->createStreamForEntry(i));
      if (!in) return false;
      out.reset();
      in->readIntoMemoryBlock(out);
      return true;
    }
    std::filesystem::path p = folder / pathOf(name);
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
      p.clear();
      for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        if (lower(utf8Of(entry.path().filename())) == lower(name)) p = entry.path();
      if (p.empty()) return false;
    }
    return juce::File(juce::String::fromUTF8(utf8Of(p).c_str())).loadFileAsData(out);
  }
};

// jOrgan's own rounding of a float to an int: half up.
int roundHalfUp(double v) { return static_cast<int>(std::floor(v + 0.5)); }

struct Insets {
  int top = 0, left = 0, bottom = 0, right = 0;
  bool zero() const { return top == 0 && left == 0 && bottom == 0 && right == 0; }
};

enum class Kind { Plain, Composite, Button, Slider, Image, Text };

// A layer, as skin.xml describes it.
struct Layer {
  Kind kind = Kind::Plain;
  std::string anchor = "CENTER", fill = "NONE", binding;
  int width = 0, height = 0;
  Insets padding;
  std::vector<Layer> children;
  // An image.
  std::string file;
  Insets border;
  // A text.
  std::string alignment = "CENTER", fontName = "Arial";
  int fontStyle = 0, fontSize = 12;
  juce::Colour colour = juce::Colours::black;
};

Insets insetsOf(pugi::xml_node n) {
  Insets i;
  i.top = n.child("top").text().as_int();
  i.left = n.child("left").text().as_int();
  i.bottom = n.child("bottom").text().as_int();
  i.right = n.child("right").text().as_int();
  return i;
}

Layer layerOf(pugi::xml_node n) {
  Layer l;
  const std::string tag = n.name();
  l.kind = tag == "image"       ? Kind::Image
           : tag == "text"      ? Kind::Text
           : tag == "button"    ? Kind::Button
           : tag == "slider"    ? Kind::Slider
           : tag == "composite" || tag == "style" ? Kind::Composite
                                : Kind::Plain;
  if (auto a = n.child("anchor")) l.anchor = a.text().as_string();
  if (auto f = n.child("fill")) l.fill = f.text().as_string();
  l.binding = n.child("binding").text().as_string();
  l.width = n.child("width").text().as_int();
  l.height = n.child("height").text().as_int();
  l.padding = insetsOf(n.child("padding"));
  for (pugi::xml_node c : n.child("layers").children())
    if (c.type() == pugi::node_element) l.children.push_back(layerOf(c));
  l.file = n.child("file").text().as_string();
  l.border = insetsOf(n.child("border"));
  if (auto a = n.child("alignment")) l.alignment = a.text().as_string();
  if (auto f = n.child("font")) {
    l.fontName = f.child("name").text().as_string();
    l.fontStyle = f.child("style").text().as_int();
    l.fontSize = std::max(1, f.child("size").text().as_int(12));
  }
  if (auto c = n.child("color")) {
    const std::string hex = c.text().as_string();
    if (c.child("red")) {
      l.colour = juce::Colour(static_cast<juce::uint8>(c.child("red").text().as_int()),
                              static_cast<juce::uint8>(c.child("green").text().as_int()),
                              static_cast<juce::uint8>(c.child("blue").text().as_int()));
    } else if (hex.size() >= 6) {
      l.colour = juce::Colour(static_cast<juce::uint32>(0xff000000u | std::stoul(hex.substr(0, 6), nullptr, 16)));
    }
  }
  return l;
}

// A skin: its styles and files, with the pictures and fonts it has read.
struct Skin {
  SkinFiles files;
  std::map<std::string, Layer> styles;
  std::map<std::string, juce::Image> images;        // by file
  std::map<std::string, juce::Image> scaled;        // by scale and file
  std::map<std::string, juce::Typeface::Ptr> fonts; // by file; null when not one
  std::vector<std::string> missing;

  const juce::Image& image(const std::string& file, float scale) {
    char key[32];
    std::snprintf(key, sizeof key, "%.4f:", scale);
    const std::string k = key + file;
    auto hit = scaled.find(k);
    if (hit != scaled.end()) return hit->second;
    auto base = images.find(file);
    if (base == images.end()) {
      juce::Image img;
      juce::MemoryBlock data;
      if (files.read(file, data)) img = juce::ImageFileFormat::loadFrom(data.getData(), data.getSize());
      if (img.isValid()) img = juce::SoftwareImageType().convert(img).convertedToFormat(juce::Image::ARGB);
      else missing.push_back(file);
      base = images.emplace(file, img).first;
    }
    juce::Image img = base->second;
    // Scaled as jOrgan's image cache does: to the rounded size, at least a pixel.
    if (img.isValid() && scale != 1.0f) {
      const int w = std::max(1, roundHalfUp(img.getWidth() * static_cast<double>(scale)));
      const int h = std::max(1, roundHalfUp(img.getHeight() * static_cast<double>(scale)));
      img = juce::SoftwareImageType().convert(img.rescaled(w, h, juce::Graphics::highResamplingQuality));
    }
    return scaled.emplace(k, img).first->second;
  }

  // A text's font: the skin's own TrueType file, or a font of the system by
  // name, Java's logical names as the system's defaults.
  juce::Font font(const Layer& l) {
    juce::Typeface::Ptr face;
    auto hit = fonts.find(l.fontName);
    if (hit != fonts.end()) {
      face = hit->second;
    } else {
      juce::MemoryBlock data;
      if (files.read(l.fontName, data) && data.getSize() > 12)
        face = juce::Typeface::createSystemTypefaceFor(data.getData(), data.getSize());
      fonts[l.fontName] = face;
    }
    const float size = static_cast<float>(l.fontSize);
    const bool bold = (l.fontStyle & 1) != 0, italic = (l.fontStyle & 2) != 0;
    if (face != nullptr) {
      juce::Font f(juce::FontOptions(face).withPointHeight(size));
      if (bold) f = f.boldened();
      if (italic) f = f.italicised();
      return f;
    }
    const std::string n = lower(l.fontName);
    juce::String family = juce::String::fromUTF8(l.fontName.c_str());
    if (n == "serif" || n == "timesroman") family = juce::Font::getDefaultSerifFontName();
    else if (n == "sansserif" || n == "dialog" || n == "dialoginput" || n == "helvetica") family = juce::Font::getDefaultSansSerifFontName();
    else if (n == "monospaced" || n == "courier") family = juce::Font::getDefaultMonospacedFontName();
    const int flags = (bold ? juce::Font::bold : 0) | (italic ? juce::Font::italic : 0);
    return juce::Font(juce::FontOptions(family, size, flags).withPointHeight(size));
  }
};

// An element as a view draws it: its texts and its state.
struct ViewState {
  float scale = 1.0f;
  std::string name;
  std::map<std::string, std::string> texts;
  bool active = false;
  float position = 0.0f;
};

// A paragraph of a text layer: the bound text in lines, as jOrgan breaks it.
struct Paragraph {
  juce::Font font{juce::FontOptions()};
  int ascent = 0, descent = 0, leading = 0, width = 0, height = 0;
  std::vector<std::pair<std::string, int>> lines;  // text and width
};

std::string textOf(const Layer& l, const ViewState& v) {
  if (l.binding == "name") return v.name;
  const auto it = v.texts.find(l.binding);
  return it == v.texts.end() ? std::string() : it->second;
}

std::string trim(const std::string& s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

struct Renderer {
  Skin& skin;
  const ViewState& v;

  int scale(int value) const { return roundHalfUp(value * static_cast<double>(v.scale)); }

  Paragraph paragraph(const Layer& l) const {
    Paragraph p;
    p.font = skin.font(l);
    // Java's metrics: whole pixels, rounded up from just below.
    p.ascent = static_cast<int>(p.font.getAscent() + 0.95f);
    p.descent = static_cast<int>(p.font.getDescent() + 0.95f);
    const juce::String text = juce::String::fromUTF8(trim(textOf(l, v)).c_str());
    auto widthOf = [&](int start, int length) {
      return roundHalfUp(juce::GlyphArrangement::getStringWidth(p.font, text.substring(start, start + length)));
    };
    const int maxWidth = l.width, maxHeight = l.height;
    int start = 0;
    const int end = text.length();
    while (start != end) {
      int length = end - start;
      const int lineBreak = text.indexOf(start, "  ");
      if (lineBreak != -1) length = lineBreak - start;
      if (maxWidth > 0) {
        while (widthOf(start, length) > maxWidth) {
          int wordBreak = text.substring(0, start + length).lastIndexOfChar(' ');
          if (length > 1) wordBreak = std::max(wordBreak, text.substring(0, start + length - 1).lastIndexOfChar('-') + 1);
          if (wordBreak <= start) {
            while (widthOf(start, length) > maxWidth) {
              if (--length == 0) return p;
            }
          } else {
            length = wordBreak - start;
          }
        }
      }
      int newHeight = p.height + p.ascent + p.descent;
      if (!p.lines.empty()) newHeight += p.leading;
      if (maxHeight > 0 && newHeight > maxHeight) return p;
      const int w = widthOf(start, length);
      p.lines.emplace_back(text.substring(start, start + length).toStdString(), w);
      p.width = std::max(p.width, w);
      p.height = newHeight;
      start += length;
      while (start < end && text[start] == ' ') ++start;
    }
    return p;
  }

  int calcWidth(const Layer& l) const {
    if (l.kind == Kind::Image) return skin.image(l.file, v.scale).getWidth();
    if (l.kind == Kind::Text) return scale(paragraph(l).width);
    return 0;
  }
  int calcHeight(const Layer& l) const {
    if (l.kind == Kind::Image) return skin.image(l.file, v.scale).getHeight();
    if (l.kind == Kind::Text) return scale(paragraph(l).height);
    return 0;
  }
  // jOrgan reads the given height only when a width is given.
  int unpaddedWidth(const Layer& l) const { return l.width == 0 ? calcWidth(l) : scale(l.width); }
  int unpaddedHeight(const Layer& l) const { return l.width == 0 ? calcHeight(l) : scale(l.height); }

  juce::Point<int> size(const Layer& l) const {
    juce::Point<int> d(unpaddedWidth(l) + scale(l.padding.left + l.padding.right),
                       unpaddedHeight(l) + scale(l.padding.top + l.padding.bottom));
    for (const Layer& c : l.children) {
      const auto cd = size(c);
      d.x = std::max(d.x, cd.x);
      d.y = std::max(d.y, cd.y);
    }
    return d;
  }

  juce::Rectangle<int> bounds(const Layer& l, juce::Point<int> d) const {
    juce::Rectangle<int> r(0, 0, unpaddedWidth(l), unpaddedHeight(l));
    const Insets& p = l.padding;
    if (l.fill == "BOTH" || l.fill == "HORIZONTAL") r.setWidth(d.x - scale(p.left + p.right));
    if (l.fill == "BOTH" || l.fill == "VERTICAL") r.setHeight(d.y - scale(p.top + p.bottom));
    const std::string& a = l.anchor;
    if (a == "TOP_LEFT" || a == "LEFT" || a == "BOTTOM_LEFT") r.setX(scale(p.left));
    else if (a == "TOP_RIGHT" || a == "RIGHT" || a == "BOTTOM_RIGHT") r.setX(d.x - scale(p.right) - r.getWidth());
    else r.setX(scale(p.left) + (d.x - scale(p.left + p.right)) / 2 - r.getWidth() / 2);
    if (a == "TOP_LEFT" || a == "TOP" || a == "TOP_RIGHT") r.setY(scale(p.top));
    else if (a == "BOTTOM_LEFT" || a == "BOTTOM" || a == "BOTTOM_RIGHT") r.setY(d.y - scale(p.bottom) - r.getHeight());
    else r.setY(scale(p.top) + (d.y - scale(p.top + p.bottom)) / 2 - r.getHeight() / 2);
    return r;
  }

  static void piece(juce::Graphics& g, const juce::Image& img, int dx1, int dy1, int dx2, int dy2, int sx1, int sy1,
                    int sx2, int sy2) {
    if (dx2 <= dx1 || dy2 <= dy1 || sx2 <= sx1 || sy2 <= sy1) return;
    g.drawImage(img, dx1, dy1, dx2 - dx1, dy2 - dy1, sx1, sy1, sx2 - sx1, sy2 - sy1);
  }

  void drawImage(juce::Graphics& g, const Layer& l, juce::Rectangle<int> r) const {
    const juce::Image& img = skin.image(l.file, v.scale);
    if (!img.isValid()) return;
    const int x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    const int iw = img.getWidth(), ih = img.getHeight();
    if ((w == iw && h == ih) || l.border.zero()) {
      // Unstretched: the picture's own pixels, cut to the layer.
      piece(g, img, x, y, x + std::min(w, iw), y + std::min(h, ih), 0, 0, std::min(w, iw), std::min(h, ih));
      return;
    }
    const Insets& b = l.border;
    piece(g, img, x, y, x + b.left, y + b.top, 0, 0, b.left, b.top);
    piece(g, img, x, y + h - b.bottom, x + b.left, y + h, 0, ih - b.bottom, b.left, ih);
    piece(g, img, x + w - b.right, y, x + w, y + b.top, iw - b.right, 0, iw, b.top);
    piece(g, img, x + w - b.right, y + h - b.bottom, x + w, y + h, iw - b.right, ih - b.bottom, iw, ih);
    piece(g, img, x + b.left, y, x + w - b.right, y + b.top, b.left, 0, iw - b.right, b.top);
    piece(g, img, x + b.left, y + h - b.bottom, x + w - b.right, y + h, b.left, ih - b.bottom, iw - b.right, ih);
    piece(g, img, x, y + b.top, x + b.left, y + h - b.bottom, 0, b.top, b.left, ih - b.bottom);
    piece(g, img, x + w - b.right, y + b.top, x + w, y + h - b.bottom, iw - b.right, b.top, iw, ih - b.bottom);
    piece(g, img, x + b.left, y + b.top, x + w - b.right, y + h - b.bottom, b.left, b.top, iw - b.right,
          ih - b.bottom);
  }

  void drawText(juce::Graphics& g, const Layer& l, juce::Rectangle<int> r) const {
    const Paragraph p = paragraph(l);
    if (p.lines.empty()) return;
    // Drawn unscaled, in a scaled frame, as jOrgan draws it.
    const int width = roundHalfUp(r.getWidth() / static_cast<double>(v.scale));
    const int height = roundHalfUp(r.getHeight() / static_cast<double>(v.scale));
    juce::Graphics::ScopedSaveState saved(g);
    g.addTransform(juce::AffineTransform::scale(v.scale).translated(static_cast<float>(r.getX()),
                                                                     static_cast<float>(r.getY())));
    g.setColour(l.colour);
    g.setFont(p.font);
    const std::string& a = l.alignment;
    int y = 0;
    if (a == "CENTER" || a == "RIGHT" || a == "LEFT") y += height / 2 - p.height / 2;
    else if (a == "BOTTOM" || a == "BOTTOM_RIGHT" || a == "BOTTOM_LEFT") y += height - p.height;
    for (size_t i = 0; i < p.lines.size(); ++i) {
      if (i > 0) y += p.leading;
      int x = 0;
      const int lw = p.lines[i].second;
      if (a == "TOP" || a == "CENTER" || a == "BOTTOM") x = width / 2 - lw / 2;
      else if (a == "RIGHT" || a == "TOP_RIGHT" || a == "BOTTOM_RIGHT") x = width - lw;
      g.drawSingleLineText(juce::String::fromUTF8(p.lines[i].first.c_str()), x, y + p.ascent);
      y += p.ascent + p.descent;
    }
  }

  void draw(juce::Graphics& g, const Layer& l, juce::Point<int> d) const {
    const juce::Rectangle<int> r = bounds(l, d);
    if (l.kind == Kind::Image) drawImage(g, l, r);
    else if (l.kind == Kind::Text) drawText(g, l, r);
    if (l.children.empty()) return;
    // Every child of a stack draws over the whole view; a button draws the
    // child of its state, a slider the child of its position.
    if (l.kind == Kind::Button) {
      const bool pressed = (l.binding == "active" || l.binding == "engaged") && v.active;
      draw(g, l.children[std::min(l.children.size() - 1, static_cast<size_t>(pressed ? 1 : 0))], d);
    } else if (l.kind == Kind::Slider) {
      const float pos = l.binding == "value" ? v.position : 0.0f;
      const size_t n = l.children.size();
      const size_t index = n == 2 ? (pos == 0.0f ? 0 : 1) : static_cast<size_t>(roundHalfUp((n - 1) * static_cast<double>(pos)));
      draw(g, l.children[std::min(index, n - 1)], d);
    } else {
      for (const Layer& c : l.children) draw(g, c, d);
    }
  }
};

// What a style shows of an element's state: on and off, positions, or nothing.
void statesOf(const Layer& l, bool& button, size_t& positions) {
  if (l.kind == Kind::Button && (l.binding == "active" || l.binding == "engaged")) button = true;
  if (l.kind == Kind::Slider && l.binding == "value") positions = std::max(positions, l.children.size());
  for (const Layer& c : l.children) statesOf(c, button, positions);
}

std::map<std::string, std::string> textsOf(const std::string& description) {
  std::map<std::string, std::string> texts;
  std::istringstream lines(description);
  for (std::string line; std::getline(lines, line);) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    if (!key.empty()) texts[key] = trim(line.substr(eq + 1));
  }
  return texts;
}

bool writePng(const juce::Image& img, const std::filesystem::path& p) {
  juce::File f(juce::String::fromUTF8(utf8Of(p).c_str()));
  f.deleteFile();
  juce::FileOutputStream out(f);
  if (!out.openedOk()) return false;
  juce::PNGImageFormat png;
  return png.writeImageToStream(img, out);
}

constexpr float kMinZoom = 0.25f;

}  // namespace

std::vector<JOrganOrgan::Console> drawJOrganConsoles(const JOrganDisposition& d, const std::string& dispositionDir,
                                                     const std::string& workFolder, std::vector<std::string>& notes) {
  std::vector<JOrganOrgan::Console> out;
  std::map<std::string, std::unique_ptr<Skin>> skins;
  const std::filesystem::path skinDir = pathOf(workFolder) / "skin";
  std::error_code ec;
  std::filesystem::remove_all(skinDir, ec);
  int consoleNo = 0;
  for (const JOrganElement& console : d.elements) {
    if (console.type != "console") continue;
    ++consoleNo;
    const std::string skinName = console.prop("skin");
    if (skinName.empty()) continue;
    auto& slot = skins[lower(skinName)];
    if (!slot) {
      auto skin = std::make_unique<Skin>();
      const std::filesystem::path p = skinPath(dispositionDir, skinName);
      juce::MemoryBlock xml;
      pugi::xml_document doc;
      if (p.empty() || !skin->files.open(p) || !skin->files.read("skin.xml", xml) ||
          !doc.load_buffer(xml.getData(), xml.getSize())) {
        notes.push_back("skin \"" + skinName + "\" is not found or not read; its consoles use the stock layout");
        continue;
      }
      for (pugi::xml_node s : doc.child("skin").child("styles").children("style"))
        skin->styles[s.child("name").text().as_string()] = layerOf(s);
      slot = std::move(skin);
    }
    Skin& skin = *slot;
    std::filesystem::create_directories(skinDir, ec);
    const float consoleZoom = std::max(kMinZoom, static_cast<float>(console.zoom));

    JOrganOrgan::Console c;
    c.name = console.name;
    auto render = [&](const Layer& style, const ViewState& v, const std::string& file) -> JOrganOrgan::Picture {
      Renderer r{skin, v};
      const auto sz = r.size(style);
      if (sz.x <= 0 || sz.y <= 0 || sz.x > 8192 || sz.y > 8192) return {};
      juce::Image img(juce::Image::ARGB, sz.x, sz.y, true, juce::SoftwareImageType());
      {
        juce::Graphics g(img);
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        r.draw(g, style, sz);
      }
      if (!writePng(img, skinDir / pathOf(file))) return {};
      return {"skin/" + file, sz.x, sz.y};
    };
    const std::string stem = "c" + std::to_string(consoleNo);

    // The console's own style, tiled behind its elements.
    const auto bg = skin.styles.find(console.style);
    if (bg != skin.styles.end()) {
      ViewState v;
      v.scale = consoleZoom;
      v.name = console.name;
      v.texts = textsOf(console.description);
      c.background = render(bg->second, v, stem + "-background.png");
    }

    for (const JOrganReference& ref : console.references) {
      if (!ref.located) continue;
      const JOrganElement* e = d.find(ref.id);
      if (!e) continue;
      const auto st = skin.styles.find(e->style);
      if (st == skin.styles.end()) {
        if (!e->style.empty()) notes.push_back("a style the skin does not have is left out: \"" + e->style + "\"");
        continue;
      }
      ViewState v;
      v.scale = std::max(kMinZoom, static_cast<float>(e->zoom)) * consoleZoom;
      v.name = e->name;
      v.texts = textsOf(e->description);
      v.active = e->prop("active") == "true";
      v.position = std::clamp(static_cast<float>(e->real("value", 0.0)), 0.0f, 1.0f);
      bool button = false;
      size_t positions = 0;
      statesOf(st->second, button, positions);
      JOrganOrgan::ConsoleView view;
      view.element = e->id;
      view.x = static_cast<int>(ref.x * consoleZoom);
      view.y = static_cast<int>(ref.y * consoleZoom);
      const std::string base = stem + "-e" + std::to_string(e->id);
      if (positions >= 2) {
        view.slider = true;
        for (size_t f = 0; f < positions; ++f) {
          ViewState at = v;
          at.position = static_cast<float>(f) / static_cast<float>(positions - 1);
          view.frames.push_back(render(st->second, at, base + "-" + std::to_string(f) + ".png"));
        }
        view.initial = positions == 2 ? (v.position == 0.0f ? 0 : 1)
                                      : roundHalfUp((positions - 1) * static_cast<double>(v.position));
      } else if (button) {
        for (int on = 0; on < 2; ++on) {
          ViewState at = v;
          at.active = on != 0;
          view.frames.push_back(render(st->second, at, base + "-" + std::to_string(on) + ".png"));
        }
        view.initial = v.active ? 1 : 0;
      } else {
        view.frames.push_back(render(st->second, v, base + ".png"));
      }
      if (view.frames.empty() || view.frames.front().file.empty()) continue;
      bool whole = true;
      for (const auto& f : view.frames) whole = whole && f.file.size() > 0;
      if (!whole) continue;
      c.width = std::max(c.width, view.x + view.frames.front().width);
      c.height = std::max(c.height, view.y + view.frames.front().height);
      c.views.push_back(std::move(view));
    }
    if (c.views.empty()) continue;
    out.push_back(std::move(c));
  }
  for (const auto& [name, skin] : skins)
    if (skin)
      for (const auto& f : skin->missing) notes.push_back("a picture the skin names is not in it: \"" + f + "\"");
  return out;
}

#endif

}  // namespace mp
