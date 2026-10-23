// Masterpiece -- split Rice coding. See SplitRice.h.
#include "SplitRice.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#define MP_SR_X86 1
#include <immintrin.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define MP_SSE41
#else
#define MP_SSE41 __attribute__((target("sse4.1")))
#endif
#endif

namespace mp {
namespace {

constexpr int kBlock = static_cast<int>(kSplitRiceBlockFrames);
constexpr int kGroup = 256;                    // values that share a k
constexpr int kGroupsPerBlock = kBlock / kGroup;
constexpr size_t kTail = 16;                   // zero bytes after the last block
constexpr int kMaxK = 31;
constexpr int kVectorK = 25;                   // the widest k the vector unpack takes
constexpr int kMaxOrder = 8;                   // what the decoder takes
constexpr int kEncodeOrder = 3;                // what the encoder tries
// One channel's positions of the unary stream's ones: the -1 before the
// first, one per value, and room for the eight a byte may add past the end.
constexpr size_t kMarkStride = 1 + kBlock + 128;

uint32_t ld32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
uint64_t ld64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
void st32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t zigzag(int32_t v) {
  return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}
int bitLength(uint32_t v) { return v ? 32 - std::countl_zero(v) : 0; }

// The carries of the cascaded sums that undo an order-`order` difference,
// from the samples before the block, newest first.
void initCarry(int order, const int32_t* hist, int32_t* carry) {
  int32_t tmp[kMaxOrder + 1];
  for (int i = 0; i < order; ++i) tmp[i] = hist[i];
  for (int m = 0; m < order; ++m) {
    carry[m] = tmp[0];
    for (int i = 0; i + 1 < order - m; ++i)
      tmp[i] = static_cast<int32_t>(static_cast<uint32_t>(tmp[i]) - static_cast<uint32_t>(tmp[i + 1]));
  }
}

// ------------------------------------------------------------------ encoder

struct BitWriter {
  std::vector<uint8_t>& out;
  uint64_t acc = 0;
  int n = 0;
  explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
  void put(uint32_t v, int bits) {  // bits <= 32
    acc |= static_cast<uint64_t>(v) << n;
    n += bits;
    while (n >= 8) {
      out.push_back(static_cast<uint8_t>(acc));
      acc >>= 8;
      n -= 8;
    }
  }
  void flush() {
    if (n > 0) out.push_back(static_cast<uint8_t>(acc));
    acc = 0;
    n = 0;
  }
};

// One signal of a block at one predictor order.
struct Candidate {
  uint64_t bits = ~0ull;
  int order = 0;
  std::vector<uint32_t> u;  // the residuals, zigzagged, padded to whole groups
  uint8_t k[kGroupsPerBlock] = {};
  int groups = 0;
  int32_t hist[kMaxOrder] = {};
};

// The best k for one group, and its cost in bits.
int pickK(const uint32_t* u, uint64_t& bits) {
  uint64_t sum = 0;
  for (int i = 0; i < kGroup; ++i) sum += u[i];
  const uint64_t mean = sum / kGroup;
  const int guess = mean ? bitLength(static_cast<uint32_t>(std::min<uint64_t>(mean, 0xFFFFFFFFu))) - 1 : 0;
  int best = 0;
  uint64_t bestBits = ~0ull;
  for (int k = std::max(0, guess - 2); k <= std::min(kMaxK, guess + 2); ++k) {
    uint64_t q = 0;
    for (int i = 0; i < kGroup; ++i) q += u[i] >> k;
    const uint64_t b = static_cast<uint64_t>(kGroup) * static_cast<uint64_t>(k + 1) + q;
    if (b < bestBits) {
      bestBits = b;
      best = k;
    }
  }
  bits = bestBits;
  return best;
}

// `t` is frame 0 of the block; t[-1] to t[-kMaxOrder] are the frames before.
void analyse(const int32_t* t, int n, int padded, int order, Candidate& c) {
  std::vector<uint32_t> tmp(static_cast<size_t>(order + n));
  for (int i = 0; i < order; ++i) tmp[static_cast<size_t>(i)] = static_cast<uint32_t>(t[i - order]);
  for (int i = 0; i < n; ++i) tmp[static_cast<size_t>(order + i)] = static_cast<uint32_t>(t[i]);
  for (int m = 0; m < order; ++m)
    for (size_t i = tmp.size() - 1; i >= 1; --i) tmp[i] -= tmp[i - 1];
  c.order = order;
  c.u.assign(static_cast<size_t>(padded), 0);
  for (int i = 0; i < n; ++i)
    c.u[static_cast<size_t>(i)] = zigzag(static_cast<int32_t>(tmp[static_cast<size_t>(order + i)]));
  for (int i = 0; i < kMaxOrder; ++i) c.hist[i] = i < order ? t[-1 - i] : 0;
  c.groups = padded / kGroup;
  uint64_t total = static_cast<uint64_t>(order) * 32 + static_cast<uint64_t>(c.groups) * 5;
  for (int g = 0; g < c.groups; ++g) {
    uint64_t b = 0;
    c.k[g] = static_cast<uint8_t>(pickK(&c.u[static_cast<size_t>(g) * kGroup], b));
    total += b;
  }
  c.bits = total;
}

void writeBody(const Candidate& c, std::vector<uint8_t>& out) {
  {
    BitWriter w(out);
    for (int g = 0; g < c.groups; ++g) w.put(c.k[g], 5);
    w.flush();
  }
  {
    BitWriter w(out);
    for (int g = 0; g < c.groups; ++g) {
      const int k = c.k[g];
      if (k == 0) continue;
      const uint32_t mask = k >= 32 ? ~0u : (1u << k) - 1;
      for (int i = 0; i < kGroup; ++i) w.put(c.u[static_cast<size_t>(g) * kGroup + static_cast<size_t>(i)] & mask, k);
    }
    w.flush();
  }
  const size_t start = out.size();
  {
    BitWriter w(out);
    for (int g = 0; g < c.groups; ++g) {
      const int k = c.k[g];
      for (int i = 0; i < kGroup; ++i) {
        uint32_t q = c.u[static_cast<size_t>(g) * kGroup + static_cast<size_t>(i)] >> k;
        while (q >= 32) {
          w.put(0, 32);
          q -= 32;
        }
        w.put(1u << q, static_cast<int>(q) + 1);
      }
    }
    w.flush();
  }
  while ((out.size() - start) % 8) out.push_back(0);
}

// ------------------------------------------------------------------ kernels

// The tables the vector unpack and the byte-at-a-time mark parse read.
struct Tables {
  alignas(16) uint8_t shuffle[kVectorK + 1][8][16];
  alignas(16) uint32_t multiply[kVectorK + 1][8][4];
  uint16_t start[kVectorK + 1][8];
  alignas(16) uint8_t markPos[256][8];
  uint8_t markCount[256];
  Tables() {
    std::memset(this, 0, sizeof *this);
    for (int b = 0; b < 256; ++b) {
      int c = 0;
      for (int i = 0; i < 8; ++i)
        if ((b >> i) & 1) markPos[b][c++] = static_cast<uint8_t>(i);
      markCount[b] = static_cast<uint8_t>(c);
    }
    // Eight vectors of four values each take 4 * k bytes. Vector j's values
    // start at bit 4 * j * k; each lane loads the four bytes its value starts
    // in and shifts it into place with a multiply and a right shift.
    for (int k = 0; k <= kVectorK; ++k)
      for (int j = 0; j < 8; ++j) {
        const int byteStart = (4 * j * k) >> 3;
        start[k][j] = static_cast<uint16_t>(byteStart);
        for (int l = 0; l < 4; ++l) {
          const int bit = (4 * j + l) * k;
          const int off = (bit >> 3) - byteStart;
          for (int b = 0; b < 4; ++b) shuffle[k][j][4 * l + b] = static_cast<uint8_t>(off + b);
          multiply[k][j][l] = 1u << (7 - (bit & 7));
        }
      }
  }
};
const Tables& tables() {
  static const Tables t;
  return t;
}

// The plain kernels: every machine, and the reference the vector ones match.
struct Plain {
  // The positions of the ones of the unary stream (`words` 8-byte words):
  // marks[1 + i] is where value i's code ends; marks[0] is -1.
  static void marks(const uint8_t* unary, size_t words, int n, int32_t* marks) {
    uint32_t* p = reinterpret_cast<uint32_t*>(marks) + 1;
    marks[0] = -1;
    size_t count = 0;
    for (size_t wi = 0; wi < words && count < static_cast<size_t>(n); ++wi) {
      uint64_t w = ld64(unary + 8 * wi);
      while (w) {
        p[count++] = static_cast<uint32_t>(wi) * 64 + static_cast<uint32_t>(std::countr_zero(w));
        w &= w - 1;
      }
    }
    // A stream with too few ones is damaged; what it decodes to fails the
    // width check.
    for (size_t i = count; i < static_cast<size_t>(n); ++i) p[i] = count ? p[count - 1] : 0;
  }

  // One group's values, the low bits from `low` and q from the marks.
  static void group(const uint8_t* low, unsigned k, const int32_t* marks, int32_t* r) {
    const uint32_t mask = k >= 32 ? ~0u : (1u << k) - 1;
    for (int i = 0; i < kGroup; ++i) {
      const uint64_t bit = static_cast<uint64_t>(i) * k;
      uint32_t v = k ? static_cast<uint32_t>(ld64(low + (bit >> 3)) >> (bit & 7)) & mask : 0;
      v |= static_cast<uint32_t>(marks[i] - marks[i - 1] - 1) << k;
      r[i] = static_cast<int32_t>((v >> 1) ^ (0u - (v & 1u)));
    }
  }

  template <int P>
  static void prefix(const int32_t* r, int32_t* x, int32_t* carry) {
    for (int i = 0; i < kGroup; ++i) {
      uint32_t v = static_cast<uint32_t>(r[i]);
      for (int m = P - 1; m >= 0; --m) {
        v += static_cast<uint32_t>(carry[m]);
        carry[m] = static_cast<int32_t>(v);
      }
      x[i] = static_cast<int32_t>(v);
    }
  }

  static void stereo(int mode, const int32_t* a, const int32_t* b, int32_t* out) {
    for (int i = 0; i < kGroup; ++i) {
      int32_t A = a[i], B = b[i];
      if (mode == 1) {
        B = static_cast<int32_t>(static_cast<uint32_t>(A) - static_cast<uint32_t>(B));
      } else if (mode == 2) {
        A = static_cast<int32_t>(static_cast<uint32_t>(A) + static_cast<uint32_t>(B));
      } else if (mode == 3) {
        const uint32_t sum = (static_cast<uint32_t>(A) << 1) | static_cast<uint32_t>(B & 1);
        A = static_cast<int32_t>(sum + static_cast<uint32_t>(B)) >> 1;
        B = static_cast<int32_t>(sum - static_cast<uint32_t>(B)) >> 1;
      }
      out[2 * i] = A;
      out[2 * i + 1] = B;
    }
  }
};

#if defined(MP_SR_X86)
// The SSE4.1 kernels: the same numbers, four values at a time.
struct Vector {
  MP_SSE41 static void marks(const uint8_t* unary, size_t words, int n, int32_t* marks) {
    const Tables& T = tables();
    uint32_t* p = reinterpret_cast<uint32_t*>(marks) + 1;
    marks[0] = -1;
    size_t count = 0;
    // A byte at a time: the table gives the positions of its ones, eight
    // stored whatever the count, and the count moves the output on.
    for (size_t wi = 0; wi < words && count < static_cast<size_t>(n); ++wi) {
      const uint8_t* bytes = unary + 8 * wi;
      for (int j = 0; j < 8; ++j) {
        const unsigned by = bytes[j];
        const __m128i v8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(T.markPos[by]));
        const __m128i base = _mm_set1_epi32(static_cast<int>(wi * 64 + static_cast<unsigned>(j) * 8));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + count), _mm_add_epi32(_mm_cvtepu8_epi32(v8), base));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + count + 4),
                         _mm_add_epi32(_mm_cvtepu8_epi32(_mm_srli_si128(v8, 4)), base));
        count += T.markCount[by];
      }
    }
    for (size_t i = count; i < static_cast<size_t>(n); ++i) p[i] = count ? p[count - 1] : 0;
  }

  MP_SSE41 static void group(const uint8_t* low, unsigned k, const int32_t* marks, int32_t* r) {
    if (k > static_cast<unsigned>(kVectorK)) {
      Plain::group(low, k, marks, r);
      return;
    }
    const Tables& T = tables();
    const __m128i one = _mm_set1_epi32(1);
    const __m128i zero = _mm_setzero_si128();
    const __m128i mask = _mm_set1_epi32(static_cast<int>((1u << k) - 1));
    const __m128i shift = _mm_cvtsi32_si128(static_cast<int>(k));
    for (int b = 0; b < kGroup / 4; b += 8) {
      const uint8_t* blk = low + static_cast<size_t>(b >> 1) * k;  // 32 values: 4 * k bytes
      for (int jj = 0; jj < 8; ++jj) {
        const uint8_t* s = blk + T.start[k][jj];
        __m128i v = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s)),
                                     _mm_load_si128(reinterpret_cast<const __m128i*>(T.shuffle[k][jj])));
        v = _mm_mullo_epi32(v, _mm_load_si128(reinterpret_cast<const __m128i*>(T.multiply[k][jj])));
        v = _mm_and_si128(_mm_srli_epi32(v, 7), mask);
        const int j = b + jj;
        const __m128i q = _mm_sub_epi32(
            _mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(marks + 4 * j)),
                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(marks + 4 * j - 1))),
            one);
        v = _mm_or_si128(_mm_sll_epi32(q, shift), v);
        const __m128i d = _mm_xor_si128(_mm_srli_epi32(v, 1), _mm_sub_epi32(zero, _mm_and_si128(v, one)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(r + 4 * j), d);
      }
    }
  }

  template <int P>
  MP_SSE41 static void prefix(const int32_t* r, int32_t* x, int32_t* carry) {
    if constexpr (P == 0) {
      std::memcpy(x, r, sizeof(int32_t) * kGroup);
    } else {
      __m128i c[P];
      for (int m = 0; m < P; ++m) c[m] = _mm_set1_epi32(carry[m]);
      for (int i = 0; i < kGroup; i += 4) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r + i));
        for (int m = P - 1; m >= 0; --m) {
          v = _mm_add_epi32(v, _mm_slli_si128(v, 4));
          v = _mm_add_epi32(v, _mm_slli_si128(v, 8));
          v = _mm_add_epi32(v, c[m]);
          c[m] = _mm_shuffle_epi32(v, 0xFF);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(x + i), v);
      }
      for (int m = 0; m < P; ++m) carry[m] = _mm_cvtsi128_si32(c[m]);
    }
  }

  MP_SSE41 static void stereo(int mode, const int32_t* a, const int32_t* b, int32_t* out) {
    const __m128i one = _mm_set1_epi32(1);
    for (int i = 0; i < kGroup; i += 4) {
      __m128i A = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
      __m128i B = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
      if (mode == 1) {
        B = _mm_sub_epi32(A, B);
      } else if (mode == 2) {
        A = _mm_add_epi32(A, B);
      } else if (mode == 3) {
        const __m128i sum = _mm_or_si128(_mm_slli_epi32(A, 1), _mm_and_si128(B, one));
        A = _mm_srai_epi32(_mm_add_epi32(sum, B), 1);
        B = _mm_srai_epi32(_mm_sub_epi32(sum, B), 1);
      }
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2 * i), _mm_unpacklo_epi32(A, B));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2 * i + 4), _mm_unpackhi_epi32(A, B));
    }
  }
};

bool sse41Present() {
#if defined(__APPLE__)
  int v = 0;
  size_t size = sizeof v;
  return sysctlbyname("hw.optional.sse4_1", &v, &size, nullptr, 0) == 0 && v != 0;
#elif defined(_MSC_VER) && !defined(__clang__)
  int r[4];
  __cpuid(r, 1);
  return (r[2] & (1 << 19)) != 0;
#else
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.1");
#endif
}
#endif  // MP_SR_X86

bool machineHasVector() {
#if defined(MP_SR_X86)
  static const bool present = sse41Present();
  return present;
#else
  return false;
#endif
}

std::atomic<bool>& vectorWanted() {
  static std::atomic<bool> wanted{true};
  return wanted;
}

template <class K>
void prefixOrder(int order, const int32_t* r, int32_t* x, int32_t* carry) {
  switch (order) {
  case 0: K::template prefix<0>(r, x, carry); break;
  case 1: K::template prefix<1>(r, x, carry); break;
  case 2: K::template prefix<2>(r, x, carry); break;
  case 3: K::template prefix<3>(r, x, carry); break;
  case 4: K::template prefix<4>(r, x, carry); break;
  case 5: K::template prefix<5>(r, x, carry); break;
  case 6: K::template prefix<6>(r, x, carry); break;
  case 7: K::template prefix<7>(r, x, carry); break;
  default: K::template prefix<8>(r, x, carry); break;
  }
}

// ----------------------------------------------------------------- decoder

struct Channel {
  int order = 0;
  uint8_t k[kGroupsPerBlock] = {};
  const uint8_t* low = nullptr;
  int32_t carry[kMaxOrder] = {};
  int32_t* marks = nullptr;
  int group = 0;
};

// One block: the bytes [begin, end) of `data`, which runs on for at least
// kTail bytes after `end`. `n` frames come back in `out`, interleaved.
template <class K>
CodecError decodeBlockWith(const uint8_t* data, size_t begin, size_t end, int n, int channels,
                           int bitDepth, int32_t* out, SplitRiceScratch& scratch) {
  const uint8_t* p = data + begin;
  const uint8_t* const e = data + end;
  if (p >= e) return CodecError::Truncated;
  const int mode = *p++;
  if (mode > 3 || (channels == 1 && mode != 0)) return CodecError::CodecFailed;

  Channel ch[kSplitRiceMaxChannels];
  int32_t hist[kSplitRiceMaxChannels][kMaxOrder] = {};
  for (int c = 0; c < channels; ++c) {
    if (p >= e) return CodecError::Truncated;
    ch[c].order = *p++;
    if (ch[c].order > kMaxOrder) return CodecError::CodecFailed;
    if (e - p < 4 * ch[c].order) return CodecError::Truncated;
    for (int i = 0; i < ch[c].order; ++i) hist[c][i] = static_cast<int32_t>(ld32(p + 4 * i));
    p += 4 * ch[c].order;
  }
  size_t length[kSplitRiceMaxChannels] = {};
  size_t used = 0;
  for (int c = 0; c + 1 < channels; ++c) {
    if (e - p < 4) return CodecError::Truncated;
    length[c] = ld32(p);
    p += 4;
    used += length[c];
  }
  if (static_cast<size_t>(e - p) < used) return CodecError::Truncated;
  length[channels - 1] = static_cast<size_t>(e - p) - used;

  const int padded = (n + kGroup - 1) / kGroup * kGroup;
  const int groups = padded / kGroup;
  const size_t kBytes = (static_cast<size_t>(groups) * 5 + 7) / 8;
  for (int c = 0; c < channels; ++c) {
    Channel& h = ch[c];
    const size_t len = length[c];
    if (len < kBytes) return CodecError::Truncated;
    size_t lowBytes = 0;
    for (int g = 0; g < groups; ++g) {
      const unsigned bit = static_cast<unsigned>(g) * 5;
      h.k[g] = static_cast<uint8_t>((ld32(p + (bit >> 3)) >> (bit & 7)) & 31);
      lowBytes += static_cast<size_t>(h.k[g]) * (kGroup / 8);
    }
    if (len - kBytes < lowBytes) return CodecError::Truncated;
    h.low = p + kBytes;
    h.marks = scratch.marks.data() + static_cast<size_t>(c) * kMarkStride;
    K::marks(h.low + lowBytes, (len - kBytes - lowBytes) / 8, padded, h.marks);
    initCarry(h.order, hist[c], h.carry);
    p += len;
  }

  int32_t* r = scratch.work.data();
  int32_t* x0 = r + kGroup;
  int32_t* x1 = x0 + kGroup;
  auto decodeGroup = [&](Channel& h, int32_t* dst) {
    const unsigned k = h.k[h.group];
    K::group(h.low, k, h.marks + 1 + static_cast<size_t>(h.group) * kGroup, r);
    h.low += static_cast<size_t>(k) * (kGroup / 8);
    ++h.group;
    prefixOrder<K>(h.order, r, dst, h.carry);
  };
  for (int g = 0; g < groups; ++g) {
    if (channels == 1) {
      decodeGroup(ch[0], out + static_cast<size_t>(g) * kGroup);
    } else {
      decodeGroup(ch[0], x0);
      decodeGroup(ch[1], x1);
      K::stereo(mode, x0, x1, out + static_cast<size_t>(g) * kGroup * 2);
    }
  }

  // Every sample within the width, or the block is not one this codec wrote.
  const int32_t low = static_cast<int32_t>(codecSampleMin(bitDepth));
  const int32_t high = static_cast<int32_t>(codecSampleMax(bitDepth));
  int32_t lo = 0, hi = 0;
  const size_t count = static_cast<size_t>(n) * static_cast<size_t>(channels);
  for (size_t i = 0; i < count; ++i) {
    lo = std::min(lo, out[i]);
    hi = std::max(hi, out[i]);
  }
  if (lo < low || hi > high) return CodecError::CodecFailed;
  return CodecError::None;
}

CodecError decodeBlockRaw(const uint8_t* data, size_t dataBytes, const uint32_t* offsets,
                          size_t blocks, uint64_t frames, int channels, int bitDepth, size_t index,
                          int32_t* out, SplitRiceScratch& scratch) {
  if (channels < 1 || channels > kSplitRiceMaxChannels) return CodecError::BadHeader;
  if (bitDepth < kSplitRiceMinBitDepth || bitDepth > kSplitRiceMaxBitDepth) return CodecError::BadWidth;
  if (index >= blocks) return CodecError::BadRange;
  if (dataBytes < kTail) return CodecError::Truncated;
  const size_t limit = dataBytes - kTail;
  const size_t begin = offsets[index];
  const size_t end = index + 1 < blocks ? offsets[index + 1] : limit;
  if (begin > end || end > limit) return CodecError::BadHeader;
  const uint64_t first = static_cast<uint64_t>(index) * kBlock;
  if (first >= frames) return CodecError::BadRange;
  const int n = static_cast<int>(std::min<uint64_t>(kBlock, frames - first));
  if (machineHasVector() && vectorWanted().load(std::memory_order_relaxed)) {
#if defined(MP_SR_X86)
    return decodeBlockWith<Vector>(data, begin, end, n, channels, bitDepth, out, scratch);
#endif
  }
  return decodeBlockWith<Plain>(data, begin, end, n, channels, bitDepth, out, scratch);
}

CodecError decodeFramesRaw(const uint8_t* data, size_t dataBytes, const uint32_t* offsets,
                           size_t blocks, uint64_t frames, int channels, int bitDepth,
                           uint64_t firstFrame, uint64_t count, int32_t* out) {
  if (count == 0 || firstFrame >= frames || count > frames - firstFrame) return CodecError::BadRange;
  if (channels < 1 || channels > kSplitRiceMaxChannels) return CodecError::BadHeader;
  SplitRiceScratch scratch;
  std::vector<int32_t> block(static_cast<size_t>(kBlock) * static_cast<size_t>(channels));
  const size_t ch = static_cast<size_t>(channels);
  uint64_t done = 0;
  while (done < count) {
    const uint64_t frame = firstFrame + done;
    const size_t index = static_cast<size_t>(frame / kBlock);
    const CodecError why = decodeBlockRaw(data, dataBytes, offsets, blocks, frames, channels,
                                          bitDepth, index, block.data(), scratch);
    if (why != CodecError::None) return why;
    const uint64_t at = frame - static_cast<uint64_t>(index) * kBlock;
    const uint64_t blockEnd = std::min<uint64_t>(kBlock, frames - static_cast<uint64_t>(index) * kBlock);
    const uint64_t take = std::min<uint64_t>(blockEnd - at, count - done);
    std::memcpy(out + done * ch, block.data() + at * ch, static_cast<size_t>(take * ch) * sizeof(int32_t));
    done += take;
  }
  return CodecError::None;
}

// ------------------------------------------------------------- the codec

const CodecInfo& splitRiceInfo() {
  static const CodecInfo info = [] {
    CodecInfo i;
    i.id = codecId("SRIC");
    i.name = "split Rice";
    i.access = CodecAccess::RandomAccess;
    i.minBitDepth = kSplitRiceMinBitDepth;
    i.maxBitDepth = kSplitRiceMaxBitDepth;
    i.blockFrames = kSplitRiceBlockFrames;
    i.lossless = true;
    return i;
  }();
  return info;
}

class SplitRiceCodec : public SampleCodec {
public:
  const CodecInfo& info() const override { return splitRiceInfo(); }

  bool encode(const int32_t* samples, uint64_t frames, int channels, int bitDepth,
              uint32_t blockFrames, std::vector<uint8_t>& payload, CodecError* error) override {
    if (blockFrames != 0 && blockFrames != kSplitRiceBlockFrames) {
      if (error) *error = CodecError::NotSupported;
      return false;
    }
    SplitRiceStream stream;
    if (!encodeSplitRice(samples, frames, channels, bitDepth, stream, error)) return false;
    payload.clear();
    payload.reserve(4 + stream.blockOffsets.size() * 4 + stream.data.size());
    st32(payload, static_cast<uint32_t>(stream.blockOffsets.size()));
    for (uint32_t o : stream.blockOffsets) st32(payload, o);
    payload.insert(payload.end(), stream.data.begin(), stream.data.end());
    return true;
  }

  bool decodeRange(const uint8_t* payload, size_t payloadBytes, const CodedHeader& header,
                   uint64_t firstFrame, uint64_t count, int32_t* out, CodecError* error) override {
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (payload == nullptr || out == nullptr || payloadBytes < 4) return fail(CodecError::BadHeader);
    if (header.blockFrames != kSplitRiceBlockFrames) return fail(CodecError::BadHeader);
    const uint64_t blocks = ld32(payload);
    if (blocks != (header.frames + kBlock - 1) / kBlock) return fail(CodecError::BadHeader);
    if ((payloadBytes - 4) / 4 < blocks) return fail(CodecError::Truncated);
    std::vector<uint32_t> offsets(static_cast<size_t>(blocks));
    for (size_t i = 0; i < offsets.size(); ++i) offsets[i] = ld32(payload + 4 + 4 * i);
    const size_t head = 4 + 4 * static_cast<size_t>(blocks);
    const CodecError why =
        decodeFramesRaw(payload + head, payloadBytes - head, offsets.data(), offsets.size(),
                        header.frames, header.channels, header.bitDepth, firstFrame, count, out);
    if (why != CodecError::None) return fail(why);
    if (error) *error = CodecError::None;
    return true;
  }
};

} // namespace

uint32_t SplitRiceStream::framesInBlock(size_t index) const {
  const uint64_t first = static_cast<uint64_t>(index) * kBlock;
  if (index >= blockOffsets.size() || first >= numFrames) return 0;
  return static_cast<uint32_t>(std::min<uint64_t>(kBlock, numFrames - first));
}

SplitRiceScratch::SplitRiceScratch()
    : marks(kMarkStride * kSplitRiceMaxChannels, 0), work(static_cast<size_t>(kGroup) * 3, 0) {}

uint64_t newSplitRiceSerial() {
  static std::atomic<uint64_t> last{0};
  return last.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool encodeSplitRice(const int32_t* samples, uint64_t frames, int channels, int bitDepth,
                     SplitRiceStream& out, CodecError* error) {
  auto fail = [&](CodecError why) {
    if (error) *error = why;
    return false;
  };
  if (samples == nullptr || frames == 0) return fail(CodecError::BadHeader);
  if (channels < 1 || channels > kSplitRiceMaxChannels) return fail(CodecError::NotSupported);
  if (bitDepth < kSplitRiceMinBitDepth || bitDepth > kSplitRiceMaxBitDepth) return fail(CodecError::BadWidth);
  const int64_t low = codecSampleMin(bitDepth);
  const int64_t high = codecSampleMax(bitDepth);
  const size_t total = static_cast<size_t>(frames) * static_cast<size_t>(channels);
  for (size_t i = 0; i < total; ++i)
    if (samples[i] < low || samples[i] > high) return fail(CodecError::BadWidth);

  out = SplitRiceStream{};  // with a serial of its own: other frames from here on
  out.numFrames = frames;
  out.channels = channels;
  out.bitDepth = bitDepth;
  out.data.reserve(total * 3 / 2);

  // The signals a block may be held as: left (or the only channel), right,
  // and side.
  const int signals = channels == 1 ? 1 : 3;
  std::vector<int32_t> sig[3];
  Candidate cand[3][kEncodeOrder + 1];
  for (uint64_t f0 = 0; f0 < frames; f0 += kBlock) {
    if (out.data.size() > 0xFFFFFFFFu) return fail(CodecError::NotSupported);
    out.blockOffsets.push_back(static_cast<uint32_t>(out.data.size()));
    const int n = static_cast<int>(std::min<uint64_t>(kBlock, frames - f0));
    const int padded = (n + kGroup - 1) / kGroup * kGroup;
    for (int s = 0; s < signals; ++s) sig[s].assign(static_cast<size_t>(n + kMaxOrder), 0);
    for (int f = -kMaxOrder; f < n; ++f) {
      const int64_t fr = static_cast<int64_t>(f0) + f;
      if (fr < 0) continue;
      const size_t at = static_cast<size_t>(f + kMaxOrder);
      const int32_t L = samples[static_cast<size_t>(fr) * static_cast<size_t>(channels)];
      sig[0][at] = L;
      if (channels == 2) {
        const int32_t R = samples[static_cast<size_t>(fr) * 2 + 1];
        sig[1][at] = R;
        sig[2][at] = L - R;
      }
    }
    int best[3] = {};
    for (int s = 0; s < signals; ++s) {
      uint64_t bits = ~0ull;
      for (int order = 0; order <= kEncodeOrder; ++order) {
        analyse(sig[s].data() + kMaxOrder, n, padded, order, cand[s][order]);
        if (cand[s][order].bits < bits) {
          bits = cand[s][order].bits;
          best[s] = order;
        }
      }
    }
    // Left and right, or left and side.
    int mode = 0;
    int pick[2] = {0, 1};
    if (channels == 2 && cand[2][best[2]].bits < cand[1][best[1]].bits) {
      mode = 1;
      pick[1] = 2;
    }
    out.data.push_back(static_cast<uint8_t>(mode));
    for (int c = 0; c < channels; ++c) {
      const Candidate& k = cand[pick[c]][best[pick[c]]];
      out.data.push_back(static_cast<uint8_t>(k.order));
      for (int i = 0; i < k.order; ++i) st32(out.data, static_cast<uint32_t>(k.hist[i]));
    }
    const size_t lengthAt = out.data.size();
    if (channels == 2) st32(out.data, 0);
    const size_t bodyAt = out.data.size();
    for (int c = 0; c < channels; ++c) {
      writeBody(cand[pick[c]][best[pick[c]]], out.data);
      if (c == 0 && channels == 2) {
        const uint32_t length = static_cast<uint32_t>(out.data.size() - bodyAt);
        for (int i = 0; i < 4; ++i) out.data[lengthAt + static_cast<size_t>(i)] = static_cast<uint8_t>(length >> (8 * i));
      }
    }
  }
  out.data.insert(out.data.end(), kTail, 0);
  // The reserve above is a guess at the worst case, about twice what a
  // sample codes to. A stream lives as long as the organ, so it keeps only
  // what it uses; otherwise every sample holds its surplus resident.
  out.data.shrink_to_fit();
  out.blockOffsets.shrink_to_fit();
  if (error) *error = CodecError::None;
  return true;
}

bool decodeSplitRiceBlock(const SplitRiceStream& stream, size_t index, int32_t* out,
                          SplitRiceScratch& scratch, CodecError* error) {
  const CodecError why = out == nullptr ? CodecError::BadHeader
                                        : decodeBlockRaw(stream.data.data(), stream.data.size(),
                                                         stream.blockOffsets.data(), stream.blockOffsets.size(),
                                                         stream.numFrames, stream.channels, stream.bitDepth,
                                                         index, out, scratch);
  if (error) *error = why;
  return why == CodecError::None;
}

bool decodeSplitRiceFrames(const SplitRiceStream& stream, uint64_t firstFrame, uint64_t count,
                           int32_t* out, CodecError* error) {
  const CodecError why =
      out == nullptr ? CodecError::BadHeader
                     : decodeFramesRaw(stream.data.data(), stream.data.size(), stream.blockOffsets.data(),
                                       stream.blockOffsets.size(), stream.numFrames, stream.channels,
                                       stream.bitDepth, firstFrame, count, out);
  if (error) *error = why;
  return why == CodecError::None;
}

std::unique_ptr<SampleCodec> makeSplitRiceCodec() { return std::make_unique<SplitRiceCodec>(); }

bool splitRiceVectorKernels() {
  return machineHasVector() && vectorWanted().load(std::memory_order_relaxed);
}

void setSplitRiceVectorKernels(bool on) { vectorWanted().store(on, std::memory_order_relaxed); }

} // namespace mp
