// Masterpiece -- a voice's decoded frames of a sample held coded. See
// DecodedWindow.h.
#include "DecodedWindow.h"

#include <algorithm>

namespace mp {
namespace {

constexpr int64_t kBlock = static_cast<int64_t>(kSplitRiceBlockFrames);

// A block's values, decoded: from one of the scratch's blocks when it holds
// that block already, decoded into the scratch's own buffer otherwise.
const int32_t* blockValues(const SplitRiceStream& s, int64_t block, DecodeScratch& sc) {
  for (const auto& b : sc.blocks)
    if (b.serial == s.serial && b.block == block) return b.values.data();
  if (!decodeSplitRiceBlock(s, static_cast<size_t>(block), sc.decoded.data(), sc.rice))
    return nullptr;
  sc.decodes.fetch_add(1, std::memory_order_relaxed);
  return sc.decoded.data();
}

// The block's frames into the window, from frame `at` of the window on, in
// the window's storage, with the zeroed pad after them.
bool putBlock(DecodedWindow& w, const SplitRiceStream& s, int64_t block, int64_t at,
              DecodeScratch& sc) {
  const int32_t* v = blockValues(s, block, sc);
  if (v == nullptr) return false;
  const size_t n = static_cast<size_t>(s.framesInBlock(static_cast<size_t>(block))) *
                   static_cast<size_t>(s.channels);
  const size_t first = static_cast<size_t>(at) * static_cast<size_t>(s.channels);
  const size_t pad = static_cast<size_t>(kWindowPad) * static_cast<size_t>(s.channels);
  if (w.wide) {
    // Four bytes stored a value, three kept: each store's top byte is
    // written over by the next value, the last one's by the zeroed pad.
    unsigned char* p = w.data + first * 3;
    for (size_t i = 0; i < n; ++i, p += 3) {
      const auto x = static_cast<uint32_t>(v[i]);
      std::memcpy(p, &x, 4);
    }
    std::memset(p, 0, pad * 3);
  } else {
    auto* p = reinterpret_cast<int16_t*>(w.data) + first;
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<int16_t>(v[i]);
    std::memset(p + n, 0, pad * 2);
  }
  return true;
}

}  // namespace

DecodeScratch::DecodeScratch()
    : decoded(static_cast<size_t>(kBlock) * kSplitRiceMaxChannels, 0) {
  for (auto& b : blocks) b.values.assign(static_cast<size_t>(kBlock) * kSplitRiceMaxChannels, 0);
}

bool holdFrames(DecodedWindow& w, const SplitRiceStream& s, int64_t first, int64_t last,
                DecodeScratch& sc) {
  const auto frames = static_cast<int64_t>(s.numFrames);
  if (frames <= 0 || s.channels < 1 || s.channels > kSplitRiceMaxChannels) return false;
  first = std::clamp<int64_t>(first, 0, frames - 1);
  last = std::clamp<int64_t>(last, first, frames - 1);
  if (w.holds(&s, first, last)) return true;

  // On from the block it holds, when the frames wanted begin in its last
  // few: those are kept, and only the next block is decoded.
  const bool onward = w.serial == s.serial && w.block >= 0 && last / kBlock == w.block + 1 &&
                      w.start + w.count == (w.block + 1) * kBlock &&
                      first >= w.start + w.count - kWindowMargin;
  if (!onward) {
    // Anywhere else: the block the first frame is in.
    w.stream = &s;
    w.serial = s.serial;
    w.wide = s.bitDepth > 16;
    const int64_t b = first / kBlock;
    if (!putBlock(w, s, b, 0, sc)) {
      w.clear();
      return false;
    }
    w.start = b * kBlock;
    w.count = s.framesInBlock(static_cast<size_t>(b));
    w.block = b;
    if (last < w.start + w.count) return true;
    // The frames run on into the next block: moved on below.
  }
  const size_t bytes = static_cast<size_t>(s.channels) * (w.wide ? 3 : 2);
  const int64_t keep = std::min(kWindowMargin, w.count);
  std::memmove(w.data, w.data + static_cast<size_t>(w.count - keep) * bytes,
               static_cast<size_t>(keep) * bytes);
  const int64_t b = w.block + 1;
  if (!putBlock(w, s, b, keep, sc)) {
    w.clear();
    return false;
  }
  w.start += w.count - keep;
  w.count = keep + s.framesInBlock(static_cast<size_t>(b));
  w.block = b;
  return w.holds(&s, first, last);
}

int32_t codedValue(const SplitRiceStream& s, const DecodedWindow* a, const DecodedWindow* b,
                   int64_t frame, int channel, DecodeScratch& sc) {
  if (frame < 0 || frame >= static_cast<int64_t>(s.numFrames)) return 0;
  const int ch = channel < s.channels ? channel : s.channels - 1;
  if (a != nullptr && a->holdsFrame(&s, frame)) return a->value(frame, ch);
  if (b != nullptr && b->holdsFrame(&s, frame)) return b->value(frame, ch);
  const int64_t block = frame / kBlock;
  DecodeScratch::Block* hit = nullptr;
  for (auto& k : sc.blocks)
    if (k.serial == s.serial && k.block == block) hit = &k;
  if (hit == nullptr) {
    hit = &sc.blocks[sc.next];
    sc.next ^= 1;
    if (!decodeSplitRiceBlock(s, static_cast<size_t>(block), hit->values.data(), sc.rice)) {
      hit->serial = 0;
      hit->block = -1;
      return 0;
    }
    sc.decodes.fetch_add(1, std::memory_order_relaxed);
    hit->serial = s.serial;
    hit->block = block;
  }
  return hit->values[static_cast<size_t>((frame - block * kBlock) * s.channels + ch)];
}

}  // namespace mp
