#include "BmpImage.h"

#include <cstring>
#include <vector>

namespace mp {
namespace {

struct Reader {
  const uint8_t* p = nullptr;
  size_t size = 0;
  bool ok = true;

  uint32_t u32(size_t at) {
    if (at + 4 > size) { ok = false; return 0; }
    return static_cast<uint32_t>(p[at]) | (static_cast<uint32_t>(p[at + 1]) << 8) |
           (static_cast<uint32_t>(p[at + 2]) << 16) |
           (static_cast<uint32_t>(p[at + 3]) << 24);
  }
  uint16_t u16(size_t at) {
    if (at + 2 > size) { ok = false; return 0; }
    return static_cast<uint16_t>(p[at] | (p[at + 1] << 8));
  }
};

// Where a colour mask puts its bits, as a shift and a scale back to 0..255.
struct Channel {
  int shift = 0;
  uint32_t mask = 0;
  int bits = 0;

  static Channel from(uint32_t mask) {
    Channel c;
    c.mask = mask;
    if (mask == 0) return c;
    while (((mask >> c.shift) & 1u) == 0) ++c.shift;
    uint32_t m = mask >> c.shift;
    while (m & 1u) { ++c.bits; m >>= 1; }
    return c;
  }
  uint8_t value(uint32_t pixel) const {
    if (mask == 0 || bits == 0) return 0;
    const uint32_t v = (pixel & mask) >> shift;
    if (bits == 8) return static_cast<uint8_t>(v);
    // Spread the range rather than shifting: 5 bits of 31 is white, not 248.
    return static_cast<uint8_t>((v * 255u) / ((1u << bits) - 1u));
  }
};

// RLE4 and RLE8, which is how the older sets store their smaller artwork.
// Rows are written bottom-up; absolute runs are word-aligned.
bool decodeRle(const uint8_t* src, size_t len, int width, int height, int bpp,
               std::vector<uint8_t>& indices) {
  indices.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  int x = 0, y = 0;
  size_t i = 0;
  auto put = [&](int value) {
    if (x < width && y < height)
      indices[static_cast<size_t>(y) * static_cast<size_t>(width) +
              static_cast<size_t>(x)] = static_cast<uint8_t>(value);
    ++x;
  };
  while (i + 1 < len) {
    const uint8_t count = src[i], val = src[i + 1];
    i += 2;
    if (count > 0) {
      for (int n = 0; n < count; ++n)
        put(bpp == 8 ? val : ((n % 2 == 0) ? (val >> 4) : (val & 0x0f)));
      continue;
    }
    if (val == 0) { x = 0; ++y; continue; }        // end of line
    if (val == 1) return true;                     // end of bitmap
    if (val == 2) {                                // delta
      if (i + 1 >= len) return false;
      x += src[i]; y += src[i + 1]; i += 2;
      continue;
    }
    // Absolute mode: `val` pixels follow, padded to a word boundary.
    const int n = val;
    if (bpp == 8) {
      if (i + static_cast<size_t>(n) > len) return false;
      for (int k = 0; k < n; ++k) put(src[i + static_cast<size_t>(k)]);
      i += static_cast<size_t>(n);
      if (n & 1) ++i;
    } else {
      const size_t bytes = static_cast<size_t>((n + 1) / 2);
      if (i + bytes > len) return false;
      for (int k = 0; k < n; ++k) {
        const uint8_t b = src[i + static_cast<size_t>(k / 2)];
        put((k % 2 == 0) ? (b >> 4) : (b & 0x0f));
      }
      i += bytes;
      if (bytes & 1) ++i;
    }
  }
  return true;
}

} // namespace

juce::Image loadBmpImage(const juce::File& file) {
  juce::MemoryBlock data;
  if (!file.loadFileAsData(data)) return {};
  Reader r{static_cast<const uint8_t*>(data.getData()), data.getSize(), true};
  if (r.size < 54) return {};
  if (r.p[0] != 'B' || r.p[1] != 'M') return {};

  const uint32_t pixelOffset = r.u32(10);
  const uint32_t headerSize = r.u32(14);
  if (!r.ok || headerSize < 12) return {};

  int width = 0, height = 0, bpp = 0;
  uint32_t compression = 0, paletteCount = 0;
  if (headerSize == 12) { // BITMAPCOREHEADER
    width = static_cast<int16_t>(r.u16(18));
    height = static_cast<int16_t>(r.u16(20));
    bpp = r.u16(24);
  } else {
    width = static_cast<int32_t>(r.u32(18));
    height = static_cast<int32_t>(r.u32(22));
    bpp = r.u16(28);
    compression = r.u32(30);
    paletteCount = r.u32(46);
  }
  if (!r.ok || width <= 0 || height == 0) return {};
  const bool topDown = height < 0;
  if (topDown) height = -height;
  if (width > 20000 || height > 20000) return {};

  Channel cr, cg, cb, ca;
  if (compression == 3 || compression == 6) { // BI_BITFIELDS / BI_ALPHABITFIELDS
    const size_t at = 14 + headerSize;
    if (headerSize >= 52) {
      cr = Channel::from(r.u32(54)); cg = Channel::from(r.u32(58));
      cb = Channel::from(r.u32(62));
      if (headerSize >= 56) ca = Channel::from(r.u32(66));
    } else {
      cr = Channel::from(r.u32(at)); cg = Channel::from(r.u32(at + 4));
      cb = Channel::from(r.u32(at + 8));
    }
  } else if (bpp == 16) {
    cr = Channel::from(0x7c00); cg = Channel::from(0x03e0); cb = Channel::from(0x001f);
  } else if (bpp == 32) {
    cr = Channel::from(0x00ff0000); cg = Channel::from(0x0000ff00);
    cb = Channel::from(0x000000ff); ca = Channel::from(0xff000000);
  }
  if (!r.ok) return {};

  // The palette, for the indexed depths.
  std::vector<juce::Colour> palette;
  if (bpp <= 8) {
    const size_t entrySize = (headerSize == 12) ? 3 : 4;
    size_t count = paletteCount != 0 ? paletteCount : (size_t{1} << bpp);
    count = juce::jmin<size_t>(count, 256);
    const size_t at = 14 + headerSize;
    palette.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const size_t e = at + i * entrySize;
      if (e + 2 >= r.size) break;
      palette.push_back(juce::Colour(r.p[e + 2], r.p[e + 1], r.p[e]));
    }
    if (palette.empty()) return {};
  }

  if (pixelOffset >= r.size) return {};
  const uint8_t* bits = r.p + pixelOffset;
  const size_t bitsLen = r.size - pixelOffset;

  // Decoded into a buffer first: whether the file's alpha channel was ever
  // written can only be known once every pixel has been read, and an image
  // being written to must not be read back from.
  std::vector<juce::Colour> pixels(static_cast<size_t>(width) * static_cast<size_t>(height),
                                   juce::Colours::black);
  bool anyVisible = false;
  bool sawAlpha = false;

  auto setPixel = [&](int x, int yStored, juce::Colour c) {
    const int y = topDown ? yStored : (height - 1 - yStored);
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = c;
    if (c.getAlpha() != 0) anyVisible = true;
  };

  auto finish = [&]() {
    // A 32-bit file whose alpha bytes were never written reads as fully
    // transparent, which would draw nothing at all. Such a file means opaque.
    const bool dropAlpha = sawAlpha && !anyVisible;
    juce::Image img(juce::Image::ARGB, width, height, true);
    juce::Image::BitmapData out(img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        const juce::Colour c =
            pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        out.setPixelColour(x, y, dropAlpha ? c.withAlpha((juce::uint8) 255) : c);
      }
    return img;
  };

  if (compression == 1 || compression == 2) { // RLE8 / RLE4
    std::vector<uint8_t> indices;
    if (!decodeRle(bits, bitsLen, width, height, compression == 1 ? 8 : 4, indices))
      return {};
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        const uint8_t idx = indices[static_cast<size_t>(y) * static_cast<size_t>(width) +
                                    static_cast<size_t>(x)];
        setPixel(x, y, idx < palette.size() ? palette[idx] : juce::Colours::black);
      }
    return finish();
  }

  // Uncompressed: rows are padded to four bytes.
  const size_t rowBytes = ((static_cast<size_t>(width) * static_cast<size_t>(bpp) + 31) / 32) * 4;
  if (rowBytes == 0 || bitsLen < rowBytes) return {};
  const int rows = juce::jmin<int>(height, static_cast<int>(bitsLen / rowBytes));

  for (int y = 0; y < rows; ++y) {
    const uint8_t* row = bits + static_cast<size_t>(y) * rowBytes;
    for (int x = 0; x < width; ++x) {
      juce::Colour c;
      switch (bpp) {
        case 1: case 4: case 8: {
          const int perByte = 8 / bpp;
          const uint8_t b = row[static_cast<size_t>(x / perByte)];
          const int shift = 8 - bpp * ((x % perByte) + 1);
          const size_t idx = static_cast<size_t>((b >> shift) & ((1 << bpp) - 1));
          c = idx < palette.size() ? palette[idx] : juce::Colours::black;
          break;
        }
        case 16: {
          const uint32_t v = static_cast<uint32_t>(row[x * 2] | (row[x * 2 + 1] << 8));
          c = juce::Colour(cr.value(v), cg.value(v), cb.value(v));
          break;
        }
        case 24: {
          const uint8_t* q = row + static_cast<size_t>(x) * 3;
          c = juce::Colour(q[2], q[1], q[0]);
          break;
        }
        case 32: {
          const uint8_t* q = row + static_cast<size_t>(x) * 4;
          const uint32_t v = static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
                             (static_cast<uint32_t>(q[2]) << 16) |
                             (static_cast<uint32_t>(q[3]) << 24);
          sawAlpha = ca.mask != 0;
          const uint8_t a = ca.mask != 0 ? ca.value(v) : 255;
          c = juce::Colour(cr.value(v), cg.value(v), cb.value(v)).withAlpha(a);
          break;
        }
        default:
          return {};
      }
      setPixel(x, y, c);
    }
  }

  return finish();
}

juce::Image loadConsoleImage(const juce::File& file) {
  juce::Image img = juce::ImageFileFormat::loadFrom(file);
  if (img.isValid()) return img;
  return loadBmpImage(file);
}

} // namespace mp
