// Masterpiece -- split Rice coding. See SplitRice.h.
#include "SplitRice.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
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
#define MP_SR_AVX2
#else
#define MP_SSE41 __attribute__((target("sse4.1")))
#define MP_SR_AVX2 __attribute__((target("avx2")))
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
constexpr int kLightOrder = 2;                 // what it tries for a packed block: order 3
                                               // costs a sixth more decode for 1.5 % of size
constexpr int kRun = 64;                       // values that share a width, packed
constexpr int kRunsPerGroup = kGroup / kRun;
constexpr int kRunsPerBlock = kBlock / kRun;
constexpr int kPackedMode = 4;                 // the mode bit of a packed block
constexpr int kMinLag = 8;                     // the shortest cycle: eight values at a time
constexpr int kLagCost = 16;                   // a cycle's lag in the channel's header, bits
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
  uint8_t w[kRunsPerBlock] = {};  // packed: each run's width
  int groups = 0;
  int32_t hist[kMaxOrder] = {};
  int lag = 0, gain = 0;  // the cycle stage; gain 0 for none
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

// The residuals the order-`order` fixed predictor leaves for the block's n
// frames, into e. `t` is frame 0 of the block; t[-1] to t[-kMaxOrder] are
// the frames before.
void differences(const int32_t* t, int n, int order, int32_t* e) {
  uint32_t tmp[kBlock + kMaxOrder];
  const int size = order + n;
  for (int i = 0; i < size; ++i) tmp[i] = static_cast<uint32_t>(t[i - order]);
  for (int m = 0; m < order; ++m)
    for (int i = size - 1; i >= 1; --i) tmp[i] -= tmp[i - 1];
  for (int i = 0; i < n; ++i) e[i] = static_cast<int32_t>(tmp[order + i]);
}

// The residual less a share of the one a period back: r[i] = e[i] -
// (e[i - lag] * g >> 2) from value lag on, in arithmetic that wraps.
void cycleResiduals(const int32_t* e, int n, int lag, int g, int32_t* r) {
  for (int i = 0; i < n; ++i) {
    uint32_t v = static_cast<uint32_t>(e[i]);
    if (i >= lag)
      v -= static_cast<uint32_t>(
          static_cast<int32_t>(static_cast<uint32_t>(e[i - lag]) * static_cast<uint32_t>(g)) >> 2);
    r[i] = static_cast<int32_t>(v);
  }
}

// About what n residuals cost split Rice: each group at the k its mean
// gives. Close enough to rank candidates before one is coded in full.
uint64_t estimateBits(const int32_t* r, int n) {
  uint64_t total = 0;
  for (int g0 = 0; g0 < n; g0 += kGroup) {
    const int count = std::min(kGroup, n - g0);
    uint64_t sum = 0;
    for (int i = 0; i < count; ++i) sum += zigzag(r[g0 + i]);
    const uint64_t mean = sum / static_cast<uint64_t>(count);
    const int k = mean ? bitLength(static_cast<uint32_t>(std::min<uint64_t>(mean, 0xFFFFFFFFu))) - 1 : 0;
    total += static_cast<uint64_t>(count) * static_cast<uint64_t>(k + 1) + (sum >> k);
  }
  return total;
}

void costResiduals(const int32_t* e, int n, int padded, bool packed, Candidate& c);

// `t` is frame 0 of the block; t[-1] to t[-kMaxOrder] are the frames before.
// Packed, a run costs its width in every value; otherwise a group costs its
// Rice code.
void analyse(const int32_t* t, int n, int padded, int order, bool packed, Candidate& c) {
  std::vector<int32_t> e(static_cast<size_t>(n));
  differences(t, n, order, e.data());
  c.order = order;
  c.lag = 0;
  c.gain = 0;
  for (int i = 0; i < kMaxOrder; ++i) c.hist[i] = i < order ? t[-1 - i] : 0;
  costResiduals(e.data(), n, padded, packed, c);
}

// The same with the cycle stage after the predictor.
void analyseCycle(const int32_t* t, int n, int padded, int order, int lag, int g, Candidate& c) {
  std::vector<int32_t> e(static_cast<size_t>(n)), r(static_cast<size_t>(n));
  differences(t, n, order, e.data());
  cycleResiduals(e.data(), n, lag, g, r.data());
  c.order = order;
  c.lag = lag;
  c.gain = g;
  for (int i = 0; i < kMaxOrder; ++i) c.hist[i] = i < order ? t[-1 - i] : 0;
  costResiduals(r.data(), n, padded, false, c);
  if (c.bits != ~0ull) c.bits += kLagCost;
}

// The cheapest cycle for one signal of a block, coded into c: lags a frame
// either side of the pipe's period, gains a half, three quarters and one,
// predictor orders 0 to 3, ranked by estimate. False when the period gives
// the block no lag to try.
bool bestCycle(const int32_t* t, int n, int padded, int period, std::vector<int32_t>& e,
               std::vector<int32_t>& r, Candidate& c) {
  e.resize(static_cast<size_t>(n));
  r.resize(static_cast<size_t>(n));
  uint64_t least = ~0ull;
  int bestOrder = 0, bestLag = 0, bestGain = 0;
  for (int order = 0; order <= kEncodeOrder; ++order) {
    differences(t, n, order, e.data());
    for (int lag = period - 1; lag <= period + 1; ++lag) {
      if (lag < kMinLag || lag >= n - kGroup / 4) continue;
      for (int g = 2; g <= 4; ++g) {
        cycleResiduals(e.data(), n, lag, g, r.data());
        const uint64_t bits = estimateBits(r.data(), n) + static_cast<uint64_t>(order) * 32;
        if (bits < least) {
          least = bits;
          bestOrder = order;
          bestLag = lag;
          bestGain = g;
        }
      }
    }
  }
  if (bestLag == 0) return false;
  analyseCycle(t, n, padded, bestOrder, bestLag, bestGain, c);
  return true;
}

// In place, a power of two long.
void fft(std::vector<std::complex<double>>& a, bool inverse) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  constexpr double kTwoPi = 6.283185307179586;
  for (size_t len = 2; len <= n; len <<= 1) {
    const double angle = (inverse ? kTwoPi : -kTwoPi) / static_cast<double>(len);
    const std::complex<double> step(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= step;
      }
    }
  }
}

// The pipe's period in frames: the strongest peak of the autocorrelation of
// up to 8192 frames of the first channel from the sample's middle, past its
// first fall below zero, when it holds at least 0.3 of the energy. 0 when
// the sample has none.
int samplePeriod(const int32_t* samples, uint64_t frames, int channels) {
  constexpr size_t kWindow = 8192;
  const size_t len = static_cast<size_t>(std::min<uint64_t>(frames, kWindow));
  if (len < 4 * kMinLag) return 0;
  const size_t first = static_cast<size_t>((frames - len) / 2);
  double mean = 0.0;
  for (size_t i = 0; i < len; ++i) mean += samples[(first + i) * static_cast<size_t>(channels)];
  mean /= static_cast<double>(len);
  std::vector<std::complex<double>> a(std::bit_ceil(2 * len));
  for (size_t i = 0; i < len; ++i) a[i] = samples[(first + i) * static_cast<size_t>(channels)] - mean;
  fft(a, false);
  for (auto& v : a) v = std::norm(v);
  fft(a, true);
  const size_t top = std::min(len / 2, static_cast<size_t>(kBlock));
  const double energy = a[0].real();
  if (energy <= 0.0) return 0;
  size_t lo = 1;
  while (lo < top && a[lo].real() >= 0.0) ++lo;
  if (lo >= top) return 0;
  size_t peak = lo;
  for (size_t i = lo; i < top; ++i)
    if (a[i].real() > a[peak].real()) peak = i;
  if (a[peak].real() < 0.3 * energy || peak + 1 >= top) return 0;
  const double y0 = a[peak - 1].real(), y1 = a[peak].real(), y2 = a[peak + 1].real();
  const double den = y0 - 2.0 * y1 + y2;
  const double at = static_cast<double>(peak) + (den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0);
  return static_cast<int>(std::lround(at));
}

// Residuals into the candidate's body and its cost, with its header: the
// order's samples before the block and the k or widths.
void costResiduals(const int32_t* e, int n, int padded, bool packed, Candidate& c) {
  const int order = c.order;
  c.u.assign(static_cast<size_t>(padded), 0);
  for (int i = 0; i < n; ++i) c.u[static_cast<size_t>(i)] = zigzag(e[i]);
  c.groups = padded / kGroup;
  if (packed) {
    const int runs = padded / kRun;
    uint64_t total = static_cast<uint64_t>(order) * 32 + static_cast<uint64_t>(runs) * 5;
    for (int j = 0; j < runs; ++j) {
      uint32_t all = 0;  // or'ed: the same bit length as the largest
      for (int i = 0; i < kRun; ++i) all |= c.u[static_cast<size_t>(j) * kRun + static_cast<size_t>(i)];
      const int w = bitLength(all);
      c.w[j] = static_cast<uint8_t>(w);
      total += static_cast<uint64_t>(kRun) * static_cast<uint64_t>(w);
      if (w > kMaxK) total = ~0ull;  // wider than a run's width can say
      if (total == ~0ull) break;
    }
    c.bits = total;
    return;
  }
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

// A run of 64 values at width w is 8 * w bytes, so the runs stay whole bytes.
void writePackedBody(const Candidate& c, std::vector<uint8_t>& out) {
  const int runs = c.groups * kRunsPerGroup;
  {
    BitWriter w(out);
    for (int j = 0; j < runs; ++j) w.put(c.w[j], 5);
    w.flush();
  }
  BitWriter w(out);
  for (int j = 0; j < runs; ++j) {
    const int width = c.w[j];
    if (width == 0) continue;
    for (int i = 0; i < kRun; ++i) w.put(c.u[static_cast<size_t>(j) * kRun + static_cast<size_t>(i)], width);
  }
  w.flush();
}

// ------------------------------------------------------------------ kernels

// The tables the vector unpack and the byte-at-a-time mark parse read.
struct Tables {
  alignas(16) uint8_t shuffle[kVectorK + 1][8][16];
  alignas(16) uint32_t multiply[kVectorK + 1][8][4];
  uint16_t start[kVectorK + 1][8];
  alignas(16) uint8_t markPos[256][8];
  uint8_t markCount[256];
  // The AVX2 unpack of a packed run: eight values of w bits are w bytes, so
  // every eight start on a byte. The low half's four values start at its
  // first byte, the high half's at byte packedHigh; each lane takes the four
  // bytes its value starts in and shifts right by packedShift.
  alignas(32) uint8_t packedShuffle[kVectorK + 1][32];
  alignas(32) uint32_t packedShift[kVectorK + 1][8];
  uint8_t packedHigh[kVectorK + 1];
  Tables() {
    std::memset(this, 0, sizeof *this);
    for (int w = 0; w <= kVectorK; ++w) {
      packedHigh[w] = static_cast<uint8_t>((4 * w) >> 3);
      for (int l = 0; l < 8; ++l) {
        const int bit = l * w;
        const int base = l < 4 ? 0 : 8 * packedHigh[w];
        const int off = (bit - base) >> 3;
        for (int b = 0; b < 4; ++b) packedShuffle[w][4 * l + b] = static_cast<uint8_t>(off + b);
        packedShift[w][l] = static_cast<uint32_t>((bit - base) & 7);
      }
    }
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

  // One packed run: kRun values of w bits each.
  static void packed(const uint8_t* low, unsigned w, int32_t* r) {
    const uint32_t mask = w >= 32 ? ~0u : (1u << w) - 1;
    for (int i = 0; i < kRun; ++i) {
      const uint64_t bit = static_cast<uint64_t>(i) * w;
      const uint32_t v = w ? static_cast<uint32_t>(ld64(low + (bit >> 3)) >> (bit & 7)) & mask : 0;
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

  // Undoes the cycle stage over `count` residuals from value `from` of the
  // block: e[i] = r[i] + (e[i - lag] * g >> 2) from value lag on, in 32-bit
  // arithmetic that wraps. `e` holds the block's values before `from`.
  static void cycle(const int32_t* r, int32_t* e, int from, int count, int lag, int g) {
    for (int i = 0; i < count; ++i) {
      const int at = from + i;
      uint32_t v = static_cast<uint32_t>(r[i]);
      if (at >= lag)
        v += static_cast<uint32_t>(
            static_cast<int32_t>(static_cast<uint32_t>(e[at - lag]) * static_cast<uint32_t>(g)) >> 2);
      e[at] = static_cast<int32_t>(v);
    }
  }

  // The smallest and largest of `count` values, folded into lo and hi.
  static void range(const int32_t* v, size_t count, int32_t& lo, int32_t& hi) {
    for (size_t i = 0; i < count; ++i) {
      lo = std::min(lo, v[i]);
      hi = std::max(hi, v[i]);
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

  MP_SSE41 static void packed(const uint8_t* low, unsigned w, int32_t* r) {
    if (w > static_cast<unsigned>(kVectorK)) {
      Plain::packed(low, w, r);
      return;
    }
    if (w == 0) {
      std::memset(r, 0, sizeof(int32_t) * kRun);
      return;
    }
    const Tables& T = tables();
    const __m128i one = _mm_set1_epi32(1);
    const __m128i zero = _mm_setzero_si128();
    const __m128i mask = _mm_set1_epi32(static_cast<int>((1u << w) - 1));
    for (int b = 0; b < kRun / 4; b += 8) {
      const uint8_t* blk = low + static_cast<size_t>(b >> 1) * w;  // 32 values: 4 * w bytes
      for (int jj = 0; jj < 8; ++jj) {
        const uint8_t* s = blk + T.start[w][jj];
        __m128i v = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s)),
                                     _mm_load_si128(reinterpret_cast<const __m128i*>(T.shuffle[w][jj])));
        v = _mm_mullo_epi32(v, _mm_load_si128(reinterpret_cast<const __m128i*>(T.multiply[w][jj])));
        v = _mm_and_si128(_mm_srli_epi32(v, 7), mask);
        const __m128i d = _mm_xor_si128(_mm_srli_epi32(v, 1), _mm_sub_epi32(zero, _mm_and_si128(v, one)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(r + 4 * (b + jj)), d);
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

  // Four at a time: a lag of 8 or more means the four values one lag back
  // are all done.
  MP_SSE41 static void cycle(const int32_t* r, int32_t* e, int from, int count, int lag, int g) {
    int i = std::clamp(lag - from, 0, count);
    std::memcpy(e + from, r, sizeof(int32_t) * static_cast<size_t>(i));
    const __m128i gain = _mm_set1_epi32(g);
    for (; i + 4 <= count; i += 4) {
      const int at = from + i;
      const __m128i back = _mm_loadu_si128(reinterpret_cast<const __m128i*>(e + at - lag));
      const __m128i v = _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(r + i)),
                                      _mm_srai_epi32(_mm_mullo_epi32(back, gain), 2));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(e + at), v);
    }
    Plain::cycle(r + i, e, from + i, count - i, lag, g);
  }

  MP_SSE41 static void range(const int32_t* v, size_t count, int32_t& lo, int32_t& hi) {
    __m128i l = _mm_set1_epi32(lo), h = _mm_set1_epi32(hi);
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
      const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v + i));
      l = _mm_min_epi32(l, x);
      h = _mm_max_epi32(h, x);
    }
    l = _mm_min_epi32(l, _mm_shuffle_epi32(l, 0x4E));
    l = _mm_min_epi32(l, _mm_shuffle_epi32(l, 0xB1));
    h = _mm_max_epi32(h, _mm_shuffle_epi32(h, 0x4E));
    h = _mm_max_epi32(h, _mm_shuffle_epi32(h, 0xB1));
    lo = _mm_cvtsi128_si32(l);
    hi = _mm_cvtsi128_si32(h);
    Plain::range(v + i, count - i, lo, hi);
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

// The AVX2 kernels: the vector ones, with the packed unpack, the cascaded
// sums and the width check eight values at a time.
struct Avx2 : Vector {
  MP_SR_AVX2 static void packed(const uint8_t* low, unsigned w, int32_t* r) {
    if (w > static_cast<unsigned>(kVectorK)) {
      Plain::packed(low, w, r);
      return;
    }
    if (w == 0) {
      std::memset(r, 0, sizeof(int32_t) * kRun);
      return;
    }
    const Tables& T = tables();
    const __m256i shuffle = _mm256_load_si256(reinterpret_cast<const __m256i*>(T.packedShuffle[w]));
    const __m256i shift = _mm256_load_si256(reinterpret_cast<const __m256i*>(T.packedShift[w]));
    const __m256i mask = _mm256_set1_epi32(static_cast<int>((1u << w) - 1));
    const __m256i one = _mm256_set1_epi32(1);
    const __m256i zero = _mm256_setzero_si256();
    const size_t high = T.packedHigh[w];
    for (int j = 0; j < kRun / 8; ++j) {
      const uint8_t* s = low + static_cast<size_t>(j) * w;
      __m256i v = _mm256_inserti128_si256(
          _mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s))),
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + high)), 1);
      v = _mm256_shuffle_epi8(v, shuffle);
      v = _mm256_and_si256(_mm256_srlv_epi32(v, shift), mask);
      v = _mm256_xor_si256(_mm256_srli_epi32(v, 1), _mm256_sub_epi32(zero, _mm256_and_si256(v, one)));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(r + 8 * j), v);
    }
  }

  template <int P>
  MP_SR_AVX2 static void prefix(const int32_t* r, int32_t* x, int32_t* carry) {
    if constexpr (P == 0) {
      std::memcpy(x, r, sizeof(int32_t) * kGroup);
    } else {
      const __m256i last = _mm256_set1_epi32(7);
      __m256i c[P];
      for (int m = 0; m < P; ++m) c[m] = _mm256_set1_epi32(carry[m]);
      for (int i = 0; i < kGroup; i += 8) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r + i));
        for (int m = P - 1; m >= 0; --m) {
          v = _mm256_add_epi32(v, _mm256_slli_si256(v, 4));
          v = _mm256_add_epi32(v, _mm256_slli_si256(v, 8));
          // The low half's sum into every lane of the high half.
          const __m256i low = _mm256_shuffle_epi32(v, 0xFF);
          v = _mm256_add_epi32(v, _mm256_permute2x128_si256(low, low, 0x08));
          v = _mm256_add_epi32(v, c[m]);
          c[m] = _mm256_permutevar8x32_epi32(v, last);
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(x + i), v);
      }
      for (int m = 0; m < P; ++m) carry[m] = _mm256_cvtsi256_si32(c[m]);
    }
  }

  MP_SR_AVX2 static void cycle(const int32_t* r, int32_t* e, int from, int count, int lag, int g) {
    int i = std::clamp(lag - from, 0, count);
    std::memcpy(e + from, r, sizeof(int32_t) * static_cast<size_t>(i));
    const __m256i gain = _mm256_set1_epi32(g);
    for (; i + 8 <= count; i += 8) {
      const int at = from + i;
      const __m256i back = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(e + at - lag));
      const __m256i v = _mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(r + i)),
                                         _mm256_srai_epi32(_mm256_mullo_epi32(back, gain), 2));
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(e + at), v);
    }
    Plain::cycle(r + i, e, from + i, count - i, lag, g);
  }

  MP_SR_AVX2 static void range(const int32_t* v, size_t count, int32_t& lo, int32_t& hi) {
    __m256i l = _mm256_set1_epi32(lo), h = _mm256_set1_epi32(hi);
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
      const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + i));
      l = _mm256_min_epi32(l, x);
      h = _mm256_max_epi32(h, x);
    }
    __m128i l4 = _mm_min_epi32(_mm256_castsi256_si128(l), _mm256_extracti128_si256(l, 1));
    __m128i h4 = _mm_max_epi32(_mm256_castsi256_si128(h), _mm256_extracti128_si256(h, 1));
    l4 = _mm_min_epi32(l4, _mm_shuffle_epi32(l4, 0x4E));
    l4 = _mm_min_epi32(l4, _mm_shuffle_epi32(l4, 0xB1));
    h4 = _mm_max_epi32(h4, _mm_shuffle_epi32(h4, 0x4E));
    h4 = _mm_max_epi32(h4, _mm_shuffle_epi32(h4, 0xB1));
    lo = _mm_cvtsi128_si32(l4);
    hi = _mm_cvtsi128_si32(h4);
    Plain::range(v + i, count - i, lo, hi);
  }
};

bool avx2Present() {
#if defined(__APPLE__)
  int v = 0;
  size_t size = sizeof v;
  return sysctlbyname("hw.optional.avx2_0", &v, &size, nullptr, 0) == 0 && v != 0;
#elif defined(_MSC_VER) && !defined(__clang__)
  int r[4];
  __cpuid(r, 1);
  const bool osYmm = (r[2] & (1 << 27)) != 0 && (_xgetbv(0) & 6) == 6;
  __cpuidex(r, 7, 0);
  return osYmm && (r[1] & (1 << 5)) != 0;
#else
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2");
#endif
}

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

bool machineHasAvx2() {
#if defined(MP_SR_X86)
  static const bool present = sse41Present() && avx2Present();
  return present;
#else
  return false;
#endif
}

std::atomic<bool>& vectorWanted() {
  static std::atomic<bool> wanted{true};
  return wanted;
}

std::atomic<bool>& avx2Wanted() {
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
  uint8_t w[kRunsPerBlock] = {};  // packed: each run's width
  const uint8_t* low = nullptr;
  int32_t carry[kMaxOrder] = {};
  int32_t* marks = nullptr;
  int group = 0;
  int lag = 0, gain = 0;     // the cycle stage; gain 0 when the channel has none
  int32_t* cycle = nullptr;  // with one: the block's residuals so far
};

// One block: the bytes [begin, end) of `data`, which runs on for at least
// kTail bytes after `end`. `n` frames come back in `out`, interleaved.
template <class K>
CodecError decodeBlockWith(const uint8_t* data, size_t begin, size_t end, int n, int channels,
                           int bitDepth, int32_t* out, SplitRiceScratch& scratch) {
  const uint8_t* p = data + begin;
  const uint8_t* const e = data + end;
  if (p >= e) return CodecError::Truncated;
  const int modeByte = *p++;
  const bool packed = (modeByte & kPackedMode) != 0;
  const int mode = modeByte & 3;
  if (modeByte > (kPackedMode | 3) || (channels == 1 && mode != 0)) return CodecError::CodecFailed;

  Channel ch[kSplitRiceMaxChannels];
  int32_t hist[kSplitRiceMaxChannels][kMaxOrder] = {};
  for (int c = 0; c < channels; ++c) {
    if (p >= e) return CodecError::Truncated;
    const int predictor = *p++;
    ch[c].order = predictor & 15;
    const int gain = (predictor >> 4) & 3;
    if ((predictor & 0xC0) != 0 || ch[c].order > kMaxOrder || (gain != 0 && packed))
      return CodecError::CodecFailed;
    if (gain != 0) {
      if (e - p < 2) return CodecError::Truncated;
      ch[c].lag = p[0] | (p[1] << 8);
      ch[c].gain = gain + 1;
      p += 2;
      if (ch[c].lag < kMinLag || ch[c].lag >= kBlock) return CodecError::CodecFailed;
      ch[c].cycle = scratch.cycle.data() + static_cast<size_t>(c) * kBlock;
    }
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
  const int runs = groups * kRunsPerGroup;
  const size_t wBytes = (static_cast<size_t>(runs) * 5 + 7) / 8;
  for (int c = 0; c < channels; ++c) {
    Channel& h = ch[c];
    const size_t len = length[c];
    if (packed) {
      if (len < wBytes) return CodecError::Truncated;
      size_t body = 0;
      for (int j = 0; j < runs; ++j) {
        const unsigned bit = static_cast<unsigned>(j) * 5;
        h.w[j] = static_cast<uint8_t>((ld32(p + (bit >> 3)) >> (bit & 7)) & 31);
        body += static_cast<size_t>(h.w[j]) * (kRun / 8);
      }
      if (len - wBytes < body) return CodecError::Truncated;
      h.low = p + wBytes;
      initCarry(h.order, hist[c], h.carry);
      p += len;
      continue;
    }
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
    if (packed) {
      for (int j = 0; j < kRunsPerGroup; ++j) {
        const unsigned w = h.w[h.group * kRunsPerGroup + j];
        K::packed(h.low, w, r + j * kRun);
        h.low += static_cast<size_t>(w) * (kRun / 8);
      }
    } else {
      const unsigned k = h.k[h.group];
      K::group(h.low, k, h.marks + 1 + static_cast<size_t>(h.group) * kGroup, r);
      h.low += static_cast<size_t>(k) * (kGroup / 8);
    }
    const int32_t* e = r;
    if (h.gain != 0) {
      const int from = h.group * kGroup;
      K::cycle(r, h.cycle, from, kGroup, h.lag, h.gain);
      e = h.cycle + from;
    }
    ++h.group;
    prefixOrder<K>(h.order, e, dst, h.carry);
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
  K::range(out, static_cast<size_t>(n) * static_cast<size_t>(channels), lo, hi);
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
    if (machineHasAvx2() && avx2Wanted().load(std::memory_order_relaxed))
      return decodeBlockWith<Avx2>(data, begin, end, n, channels, bitDepth, out, scratch);
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
    : marks(kMarkStride * kSplitRiceMaxChannels, 0),
      work(static_cast<size_t>(kGroup) * 3, 0),
      cycle(static_cast<size_t>(kBlock) * kSplitRiceMaxChannels, 0) {}

uint64_t newSplitRiceSerial() {
  static std::atomic<uint64_t> last{0};
  return last.fetch_add(1, std::memory_order_relaxed) + 1;
}

namespace {
constexpr const char* kLevelNames[] = {"light", "medium", "high", "max"};
}  // namespace

const char* splitRiceLevelName(SplitRiceLevel level) {
  const auto i = static_cast<size_t>(level);
  return i < std::size(kLevelNames) ? kLevelNames[i] : "medium";
}

bool splitRiceLevelFromName(const char* name, SplitRiceLevel& level) {
  if (name == nullptr) return false;
  for (size_t i = 0; i < std::size(kLevelNames); ++i)
    if (std::strcmp(name, kLevelNames[i]) == 0) {
      level = static_cast<SplitRiceLevel>(i);
      return true;
    }
  return false;
}

bool encodeSplitRice(const int32_t* samples, uint64_t frames, int channels, int bitDepth,
                     SplitRiceStream& out, CodecError* error, SplitRiceLevel level) {
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

  // High and Max pack 16-bit samples. Above 16 bits, the recording's noise
  // floor leaves them little to find for their extra work, so those samples
  // are coded medium.
  if (level > SplitRiceLevel::Medium && bitDepth > 16) level = SplitRiceLevel::Medium;

  out = SplitRiceStream{};  // with a serial of its own: other frames from here on
  out.numFrames = frames;
  out.channels = channels;
  out.bitDepth = bitDepth;
  out.data.reserve(total * 3 / 2);

  // The signals a block may be held as: left (or the only channel), right,
  // side and mid.
  const int signals = channels == 1 ? 1 : 4;
  const bool packed = level == SplitRiceLevel::Light;
  // High: the pipe's period, once for the sample; every block tries the
  // cycle stage at it.
  const int period = level >= SplitRiceLevel::High ? samplePeriod(samples, frames, channels) : 0;
  std::vector<int32_t> sig[4];
  Candidate cand[4][kEncodeOrder + 1];
  Candidate cyc[4];
  std::vector<int32_t> cycleE, cycleR;
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
        sig[3][at] = static_cast<int32_t>((static_cast<int64_t>(L) + R) >> 1);
      }
    }
    const Candidate* chosen[4] = {};
    for (int s = 0; s < signals; ++s) {
      uint64_t bits = ~0ull;
      for (int order = 0; order <= (packed ? kLightOrder : kEncodeOrder); ++order) {
        analyse(sig[s].data() + kMaxOrder, n, padded, order, packed, cand[s][order]);
        if (cand[s][order].bits < bits) {
          bits = cand[s][order].bits;
          chosen[s] = &cand[s][order];
        }
      }
      if (period > 0 && bestCycle(sig[s].data() + kMaxOrder, n, padded, period, cycleE, cycleR, cyc[s]) &&
          cyc[s].bits < bits)
        chosen[s] = &cyc[s];
    }
    // The smallest pair: left and right, left and side, side and right, or
    // mid and side, the stereo modes in that order.
    int mode = 0;
    int pick[2] = {0, 1};
    if (channels == 2) {
      static constexpr int kPairs[4][2] = {{0, 1}, {0, 2}, {2, 1}, {3, 2}};
      uint64_t least = ~0ull;
      for (int m = 0; m < 4; ++m) {
        const uint64_t bits = chosen[kPairs[m][0]]->bits + chosen[kPairs[m][1]]->bits;
        if (bits < least) {
          least = bits;
          mode = m;
          pick[0] = kPairs[m][0];
          pick[1] = kPairs[m][1];
        }
      }
    }
    out.data.push_back(static_cast<uint8_t>(mode | (packed ? kPackedMode : 0)));
    for (int c = 0; c < channels; ++c) {
      const Candidate& k = *chosen[pick[c]];
      // The gain bits: 1, 2 or 3 for a gain of 2, 3 or 4 quarters.
      const int gainBits = k.gain != 0 ? k.gain - 1 : 0;
      out.data.push_back(static_cast<uint8_t>(k.order | (gainBits << 4)));
      if (k.gain != 0) {
        out.data.push_back(static_cast<uint8_t>(k.lag));
        out.data.push_back(static_cast<uint8_t>(k.lag >> 8));
      }
      for (int i = 0; i < k.order; ++i) st32(out.data, static_cast<uint32_t>(k.hist[i]));
    }
    const size_t lengthAt = out.data.size();
    if (channels == 2) st32(out.data, 0);
    const size_t bodyAt = out.data.size();
    for (int c = 0; c < channels; ++c) {
      if (packed)
        writePackedBody(*chosen[pick[c]], out.data);
      else
        writeBody(*chosen[pick[c]], out.data);
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

namespace {
void storeValuesPlain(const int32_t* v, size_t count, bool wide, unsigned char* out, size_t from) {
  if (wide) {
    unsigned char* p = out + from * 3;
    for (size_t i = from; i < count; ++i, p += 3) {
      const auto x = static_cast<uint32_t>(v[i]);
      std::memcpy(p, &x, 4);
    }
  } else {
    auto* p = reinterpret_cast<int16_t*>(out);
    for (size_t i = from; i < count; ++i) {
      const auto x = static_cast<int16_t>(v[i]);
      std::memcpy(p + i, &x, 2);
    }
  }
}

#if defined(MP_SR_X86)
// Four values to twelve bytes with one shuffle; eight to sixteen bytes with
// one pack, which saturates nothing: every value fits 16 bits.
MP_SSE41 void storeValuesVector(const int32_t* v, size_t count, bool wide, unsigned char* out) {
  size_t i = 0;
  if (wide) {
    const __m128i pick = _mm_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
    for (; i + 4 <= count; i += 4)
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i * 3),
                       _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(v + i)), pick));
  } else {
    for (; i + 8 <= count; i += 8)
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i * 2),
                       _mm_packs_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(v + i)),
                                       _mm_loadu_si128(reinterpret_cast<const __m128i*>(v + i + 4))));
  }
  storeValuesPlain(v, count, wide, out, i);
}
#endif
}  // namespace

void storeSplitRiceValues(const int32_t* values, size_t count, bool wide, unsigned char* out) {
#if defined(MP_SR_X86)
  if (machineHasVector() && vectorWanted().load(std::memory_order_relaxed)) {
    storeValuesVector(values, count, wide, out);
    return;
  }
#endif
  storeValuesPlain(values, count, wide, out, 0);
}

std::unique_ptr<SampleCodec> makeSplitRiceCodec() { return std::make_unique<SplitRiceCodec>(); }

bool splitRiceVectorKernels() {
  return machineHasVector() && vectorWanted().load(std::memory_order_relaxed);
}

void setSplitRiceVectorKernels(bool on) { vectorWanted().store(on, std::memory_order_relaxed); }

bool splitRiceAvx2Kernels() {
  return splitRiceVectorKernels() && machineHasAvx2() && avx2Wanted().load(std::memory_order_relaxed);
}

void setSplitRiceAvx2Kernels(bool on) { avx2Wanted().store(on, std::memory_order_relaxed); }

} // namespace mp
