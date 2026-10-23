#include "SimdRun.h"

#include "VoiceEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#if defined(_M_X64) || defined(__x86_64__)
#define MP_SIMD_X86 1
#include <immintrin.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define MP_AVX2
#else
#define MP_AVX2 __attribute__((target("avx2")))
#endif
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#define MP_SIMD_NEON 1
#include <arm_neon.h>
#endif

namespace mp::simd {

namespace {

// The engine's interpolation, operation for operation (VoiceEngine.cpp).
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b = w + a;
  return ((((a * t) - b) * t + c) * t + x0);
}

// One frame the way the per-frame reader does it, for the few frames at the
// end of a run that do not fill a vector.
template <typename T>
inline void scalarFrame(const T* data, int channels, int64_t k, float t, float env,
                        float* const* out, int numChannels, int i) {
  for (int ch = 0; ch < numChannels; ++ch) {
    const int srcCh = ch < channels ? ch : channels - 1;
    const T* p = data + (k - 1) * channels + srcCh;
    out[ch][i] += hermite(static_cast<float>(p[0]), static_cast<float>(p[channels]),
                          static_cast<float>(p[2 * channels]),
                          static_cast<float>(p[3 * channels]), t) *
                  env;
  }
}

// Twenty-bit samples are packed, so a frame's four taps are four decoded
// values rather than four neighbours in an array.
inline void scalarFrame(const Pcm20* data, int channels, int64_t k, float t, float env,
                        float* const* out, int numChannels, int i) {
  for (int ch = 0; ch < numChannels; ++ch) {
    const int srcCh = ch < channels ? ch : channels - 1;
    const size_t base = static_cast<size_t>((k - 1) * channels + srcCh);
    const size_t step = static_cast<size_t>(channels);
    out[ch][i] += hermite(static_cast<float>(pcm20At(data, base)),
                          static_cast<float>(pcm20At(data, base + step)),
                          static_cast<float>(pcm20At(data, base + 2 * step)),
                          static_cast<float>(pcm20At(data, base + 3 * step)), t) *
                  env;
  }
}

// A run whose pitch and level move from frame to frame -- a tremulant -- is
// given as two arrays, one step and one level per frame; without them every
// frame takes `ratio` and `env`.
struct PerFrame {
  const double* ratios = nullptr;
  const float* envs = nullptr;
};

// Every run takes `base`, the frame `data` starts at (SimdRun.h). The frame a
// tap is read from is counted from there; its fraction comes from the whole
// position, as it does for a sample held whole.
template <typename T>
double runScalar(const T* data, int channels, double cursor, double ratio, float env,
                 float* const* out, int numChannels, int start, int frames, PerFrame pf = {},
                 int first = 0, int64_t base = 0) {
  double p = cursor;
  for (int j = 0; j < frames; ++j) {
    const auto k = static_cast<int64_t>(p);
    scalarFrame(data, channels, k - base, static_cast<float>(p - static_cast<double>(k)),
                pf.envs != nullptr ? pf.envs[first + j] : env, out, numChannels, start + j);
    p += pf.ratios != nullptr ? pf.ratios[first + j] : ratio;
  }
  return p;
}

#if defined(MP_SIMD_X86)
// ---------------------------------------------------------------- AVX2

MP_AVX2 inline __m256 hermite8(__m256 xm1, __m256 x0, __m256 x1, __m256 x2, __m256 t) {
  const __m256 half = _mm256_set1_ps(0.5f);
  const __m256 c = _mm256_mul_ps(_mm256_sub_ps(x1, xm1), half);
  const __m256 v = _mm256_sub_ps(x0, x1);
  const __m256 w = _mm256_add_ps(c, v);
  const __m256 a = _mm256_add_ps(_mm256_add_ps(w, v), _mm256_mul_ps(_mm256_sub_ps(x2, x0), half));
  const __m256 b = _mm256_add_ps(w, a);
  __m256 r = _mm256_sub_ps(_mm256_mul_ps(a, t), b);
  r = _mm256_add_ps(_mm256_mul_ps(r, t), c);
  return _mm256_add_ps(_mm256_mul_ps(r, t), x0);
}

// Eight taps, one per frame, at element indices `e` (frame * channels +
// channel) past `data`. The integer formats fetch 32 bits and sign-extend,
// reading up to two bytes beyond the tap: kRunMargin keeps that inside the
// resident storage.
MP_AVX2 inline __m256 gather8(const float* data, __m256i e) {
  return _mm256_i32gather_ps(data, e, 4);
}
MP_AVX2 inline __m256 gather8(const int16_t* data, __m256i e) {
  const __m256i g = _mm256_i32gather_epi32(reinterpret_cast<const int*>(data), e, 2);
  return _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(g, 16), 16));
}
MP_AVX2 inline __m256 gather8(const Pcm24* data, __m256i e) {
  const __m256i bytes = _mm256_add_epi32(_mm256_add_epi32(e, e), e);
  const __m256i g = _mm256_i32gather_epi32(reinterpret_cast<const int*>(data), bytes, 1);
  return _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(g, 8), 8));
}
// Packed twenty-bit samples: sample i lives at byte (i*5)>>1, at bit
// (i&1)*4 of the little-endian word there. The gather reads four bytes and
// the shift brings the sample to the bottom of the word; twenty bits are
// then sign-extended, which is what the two shifts do.
//
// The byte offset and the shift come out of one shift of the index: i*5 is
// (i<<2)+i, and the bit the sample starts at is the low bit of i, which the
// same (i<<2) carries.
MP_AVX2 inline __m256 gather8(const Pcm20* data, __m256i e) {
  const __m256i four = _mm256_slli_epi32(e, 2);
  const __m256i bytes = _mm256_srli_epi32(_mm256_add_epi32(four, e), 1);
  const __m256i shift = _mm256_and_si256(four, _mm256_set1_epi32(4));
  const __m256i g = _mm256_i32gather_epi32(reinterpret_cast<const int*>(data), bytes, 1);
  const __m256i v = _mm256_srlv_epi32(g, shift);
  return _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_slli_epi32(v, 12), 12));
}

template <typename T>
MP_AVX2 double runAvx2(const T* data, int channels, double cursor, double ratio, float env,
                       float* const* out, int numChannels, int start, int frames, PerFrame pf,
                       int64_t origin) {
  // `origin` is the run's base frame; `base` the element each lane reads from.
  alignas(32) int32_t base[8];
  alignas(32) float frac[8];
  const __m256 envv = _mm256_set1_ps(env);
  const __m256i step = _mm256_set1_epi32(channels);
  double p = cursor;
  int j = 0;
  for (; j + 8 <= frames; j += 8) {
    // Positions advance one frame at a time, as in the per-frame reader, so
    // every frame reads the same taps at the same fraction.
    for (int l = 0; l < 8; ++l) {
      const auto k = static_cast<int64_t>(p);
      frac[l] = static_cast<float>(p - static_cast<double>(k));
      base[l] = static_cast<int32_t>((k - 1 - origin) * channels);
      p += pf.ratios != nullptr ? pf.ratios[j + l] : ratio;
    }
    const __m256i b = _mm256_load_si256(reinterpret_cast<const __m256i*>(base));
    const __m256 t = _mm256_load_ps(frac);
    const __m256 level = pf.envs != nullptr ? _mm256_loadu_ps(pf.envs + j) : envv;
    const int sources = channels == 1 ? 1 : numChannels;
    for (int s = 0; s < sources; ++s) {
      const int srcCh = s < channels ? s : channels - 1;
      const __m256i e0 = _mm256_add_epi32(b, _mm256_set1_epi32(srcCh));
      const __m256i e1 = _mm256_add_epi32(e0, step);
      const __m256i e2 = _mm256_add_epi32(e1, step);
      const __m256i e3 = _mm256_add_epi32(e2, step);
      const __m256 y = _mm256_mul_ps(
          hermite8(gather8(data, e0), gather8(data, e1), gather8(data, e2), gather8(data, e3), t),
          level);
      // A mono sample sounds the same in every output channel.
      const int from = channels == 1 ? 0 : s;
      const int to = channels == 1 ? numChannels : s + 1;
      for (int ch = from; ch < to; ++ch) {
        float* o = out[ch] + start + j;
        _mm256_storeu_ps(o, _mm256_add_ps(_mm256_loadu_ps(o), y));
      }
    }
  }
  const double rest = runScalar(data, channels, p, ratio, env, out, numChannels, start + j,
                                frames - j, pf, j, origin);
  return rest;
}

// ---------------------------------------------------------------- SSE2
// Every x86-64 processor has it, so this is the vector path for those
// without AVX2: budget Celeron and Pentium Silver mini PCs, older Core
// chips, and the Intel build under Rosetta. Four frames at a time, built the
// way the NEON path builds them: a frame's taps are contiguous, so each is
// one load, then a transpose puts frames across and taps down.

inline __m128 hermite4(__m128 xm1, __m128 x0, __m128 x1, __m128 x2, __m128 t) {
  const __m128 half = _mm_set1_ps(0.5f);
  const __m128 c = _mm_mul_ps(_mm_sub_ps(x1, xm1), half);
  const __m128 v = _mm_sub_ps(x0, x1);
  const __m128 w = _mm_add_ps(c, v);
  const __m128 a = _mm_add_ps(_mm_add_ps(w, v), _mm_mul_ps(_mm_sub_ps(x2, x0), half));
  const __m128 b = _mm_add_ps(w, a);
  __m128 r = _mm_sub_ps(_mm_mul_ps(a, t), b);
  r = _mm_add_ps(_mm_mul_ps(r, t), c);
  return _mm_add_ps(_mm_mul_ps(r, t), x0);
}

// One frame's four taps for up to two channels.
inline void tapsSse(const float* data, int channels, int64_t k, __m128* ch) {
  if (channels == 1) {
    ch[0] = _mm_loadu_ps(data + (k - 1));
  } else {
    const __m128 lo = _mm_loadu_ps(data + (k - 1) * 2);      // L0 R0 L1 R1
    const __m128 hi = _mm_loadu_ps(data + (k - 1) * 2 + 4);  // L2 R2 L3 R3
    ch[0] = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));
    ch[1] = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
  }
}
// 16-bit samples, the resident format most players use: a frame's four taps
// are 64 bits mono or 128 bits stereo, so one load takes them all, and each
// count is widened in place (paired with itself, then shifted down, which
// keeps its sign). Widening them one value at a time cost the SSE2 path as
// much as its arithmetic.
inline void tapsSse(const int16_t* data, int channels, int64_t k, __m128* ch) {
  if (channels == 1) {
    const __m128i v = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(data + (k - 1)));
    ch[0] = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(v, v), 16));
  } else {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + (k - 1) * 2));
    const __m128 lo = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(v, v), 16));  // L0 R0 L1 R1
    const __m128 hi = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpackhi_epi16(v, v), 16));  // L2 R2 L3 R3
    ch[0] = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));
    ch[1] = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
  }
}
// Four 24-bit counts from twelve bytes, sign-extended, from one 16-byte load.
// Each lane is moved to the bytes its count starts at, the top byte shifted
// out and the sign brought back. The load reads four bytes past the twelve;
// kRunMargin leaves at least two frames there.
inline __m128i widen24(const Pcm24* p) {
  const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
  const __m128i a = _mm_unpacklo_epi32(v, _mm_srli_si128(v, 3));                     // 0 3 4 7
  const __m128i b = _mm_unpacklo_epi32(_mm_srli_si128(v, 6), _mm_srli_si128(v, 9));  // 6 9 ...
  const __m128i w = _mm_unpacklo_epi64(a, b);                                         // 0 3 6 9
  return _mm_srai_epi32(_mm_slli_epi32(w, 8), 8);
}
// 24-bit samples, widened four at a time: a frame's taps are twelve bytes
// mono, twenty-four stereo.
inline void tapsSse(const Pcm24* data, int channels, int64_t k, __m128* ch) {
  if (channels == 1) {
    ch[0] = _mm_cvtepi32_ps(widen24(data + (k - 1)));
  } else {
    const __m128 lo = _mm_cvtepi32_ps(widen24(data + (k - 1) * 2));      // L0 R0 L1 R1
    const __m128 hi = _mm_cvtepi32_ps(widen24(data + (k - 1) * 2 + 4));  // L2 R2 L3 R3
    ch[0] = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));
    ch[1] = _mm_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
  }
}
template <typename T>
inline void tapsSse(const T* data, int channels, int64_t k, __m128* ch) {
  // The integer formats are widened one value at a time; the arithmetic
  // still runs four frames wide.
  for (int c = 0; c < channels; ++c) {
    alignas(16) float f[4];
    for (int m = 0; m < 4; ++m) f[m] = static_cast<float>(data[(k - 1 + m) * channels + c]);
    ch[c] = _mm_load_ps(f);
  }
}
// Packed twenty-bit samples have no vector load either: each tap is decoded
// on its own, and the arithmetic still runs four frames wide.
inline void tapsSse(const Pcm20* data, int channels, int64_t k, __m128* ch) {
  for (int c = 0; c < channels; ++c) {
    alignas(16) float f[4];
    for (int m = 0; m < 4; ++m)
      f[m] = static_cast<float>(
          pcm20At(data, static_cast<size_t>((k - 1 + m) * channels + c)));
    ch[c] = _mm_load_ps(f);
  }
}

template <typename T>
double runSse2(const T* data, int channels, double cursor, double ratio, float env,
               float* const* out, int numChannels, int start, int frames, PerFrame pf,
               int64_t base) {
  const __m128 envv = _mm_set1_ps(env);
  double p = cursor;
  int j = 0;
  for (; j + 4 <= frames; j += 4) {
    __m128 rows[4][2];
    alignas(16) float frac[4];
    for (int l = 0; l < 4; ++l) {
      const auto k = static_cast<int64_t>(p);
      frac[l] = static_cast<float>(p - static_cast<double>(k));
      tapsSse(data, channels, k - base, rows[l]);
      p += pf.ratios != nullptr ? pf.ratios[j + l] : ratio;
    }
    const __m128 t = _mm_load_ps(frac);
    const __m128 level = pf.envs != nullptr ? _mm_loadu_ps(pf.envs + j) : envv;
    const int sources = channels == 1 ? 1 : numChannels;
    for (int s = 0; s < sources; ++s) {
      const int c = s < channels ? s : channels - 1;
      // Frames across, taps down.
      __m128 xm1 = rows[0][c], x0 = rows[1][c], x1 = rows[2][c], x2 = rows[3][c];
      _MM_TRANSPOSE4_PS(xm1, x0, x1, x2);
      const __m128 y = _mm_mul_ps(hermite4(xm1, x0, x1, x2, t), level);
      const int from = channels == 1 ? 0 : s;
      const int to = channels == 1 ? numChannels : s + 1;
      for (int ch = from; ch < to; ++ch) {
        float* o = out[ch] + start + j;
        _mm_storeu_ps(o, _mm_add_ps(_mm_loadu_ps(o), y));
      }
    }
  }
  return runScalar(data, channels, p, ratio, env, out, numChannels, start + j, frames - j, pf,
                   j, base);
}

bool avx2Present() {
#if defined(_MSC_VER) && !defined(__clang__)
  int r[4];
  __cpuid(r, 0);
  if (r[0] < 7) return false;
  __cpuid(r, 1);
  const bool osxsave = (r[2] & (1 << 27)) != 0;
  const bool avx = (r[2] & (1 << 28)) != 0;
  if (!osxsave || !avx) return false;
  if ((_xgetbv(0) & 6) != 6) return false;  // the OS saves the AVX registers
  __cpuidex(r, 7, 0);
  return (r[1] & (1 << 5)) != 0;
#else
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2");
#endif
}
#endif  // MP_SIMD_X86

#if defined(MP_SIMD_NEON)
// ---------------------------------------------------------------- NEON

inline float32x4_t hermite4(float32x4_t xm1, float32x4_t x0, float32x4_t x1, float32x4_t x2,
                            float32x4_t t) {
  const float32x4_t half = vdupq_n_f32(0.5f);
  const float32x4_t c = vmulq_f32(vsubq_f32(x1, xm1), half);
  const float32x4_t v = vsubq_f32(x0, x1);
  const float32x4_t w = vaddq_f32(c, v);
  const float32x4_t a = vaddq_f32(vaddq_f32(w, v), vmulq_f32(vsubq_f32(x2, x0), half));
  const float32x4_t b = vaddq_f32(w, a);
  float32x4_t r = vsubq_f32(vmulq_f32(a, t), b);
  r = vaddq_f32(vmulq_f32(r, t), c);
  return vaddq_f32(vmulq_f32(r, t), x0);
}

// One frame's four taps for up to two channels. NEON has no gather, but a
// frame's taps are contiguous (or interleaved with the other channel), so
// each is a single load.
inline void taps(const float* data, int channels, int64_t k, float32x4_t* ch) {
  if (channels == 1) {
    ch[0] = vld1q_f32(data + (k - 1));
  } else {
    const float32x4x2_t v = vld2q_f32(data + (k - 1) * 2);
    ch[0] = v.val[0];
    ch[1] = v.val[1];
  }
}
inline void taps(const int16_t* data, int channels, int64_t k, float32x4_t* ch) {
  if (channels == 1) {
    ch[0] = vcvtq_f32_s32(vmovl_s16(vld1_s16(data + (k - 1))));
  } else {
    const int16x4x2_t v = vld2_s16(data + (k - 1) * 2);
    ch[0] = vcvtq_f32_s32(vmovl_s16(v.val[0]));
    ch[1] = vcvtq_f32_s32(vmovl_s16(v.val[1]));
  }
}
inline void taps(const Pcm24* data, int channels, int64_t k, float32x4_t* ch) {
  // Three-byte samples have no vector load; the arithmetic still runs wide.
  for (int c = 0; c < channels; ++c) {
    float f[4];
    for (int m = 0; m < 4; ++m) f[m] = static_cast<float>(data[(k - 1 + m) * channels + c]);
    ch[c] = vld1q_f32(f);
  }
}
// Packed twenty-bit samples: each tap is decoded on its own.
inline void taps(const Pcm20* data, int channels, int64_t k, float32x4_t* ch) {
  for (int c = 0; c < channels; ++c) {
    float f[4];
    for (int m = 0; m < 4; ++m)
      f[m] = static_cast<float>(
          pcm20At(data, static_cast<size_t>((k - 1 + m) * channels + c)));
    ch[c] = vld1q_f32(f);
  }
}

template <typename T>
double runNeon(const T* data, int channels, double cursor, double ratio, float env,
               float* const* out, int numChannels, int start, int frames, PerFrame pf,
               int64_t base) {
  const float32x4_t envv = vdupq_n_f32(env);
  double p = cursor;
  int j = 0;
  for (; j + 4 <= frames; j += 4) {
    float32x4_t rows[4][2];
    float frac[4];
    for (int l = 0; l < 4; ++l) {
      const auto k = static_cast<int64_t>(p);
      frac[l] = static_cast<float>(p - static_cast<double>(k));
      taps(data, channels, k - base, rows[l]);
      p += pf.ratios != nullptr ? pf.ratios[j + l] : ratio;
    }
    const float32x4_t t = vld1q_f32(frac);
    const float32x4_t level = pf.envs != nullptr ? vld1q_f32(pf.envs + j) : envv;
    const int sources = channels == 1 ? 1 : numChannels;
    for (int s = 0; s < sources; ++s) {
      const int c = s < channels ? s : channels - 1;
      // Frames across, taps down.
      const float32x4x2_t t01 = vtrnq_f32(rows[0][c], rows[1][c]);
      const float32x4x2_t t23 = vtrnq_f32(rows[2][c], rows[3][c]);
      const float32x4_t xm1 = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
      const float32x4_t x0 = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
      const float32x4_t x1 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
      const float32x4_t x2 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
      const float32x4_t y = vmulq_f32(hermite4(xm1, x0, x1, x2, t), level);
      const int from = channels == 1 ? 0 : s;
      const int to = channels == 1 ? numChannels : s + 1;
      for (int ch = from; ch < to; ++ch) {
        float* o = out[ch] + start + j;
        vst1q_f32(o, vaddq_f32(vld1q_f32(o), y));
      }
    }
  }
  return runScalar(data, channels, p, ratio, env, out, numChannels, start + j, frames - j, pf,
                   j, base);
}
#endif  // MP_SIMD_NEON

template <typename T>
double run(Isa isa, const T* data, int channels, double cursor, double ratio, float env,
           float* const* out, int numChannels, int start, int frames, PerFrame pf,
           int64_t base) {
#if defined(MP_SIMD_X86)
  if (isa == Isa::Avx2)
    return runAvx2(data, channels, cursor, ratio, env, out, numChannels, start, frames, pf,
                   base);
  if (isa == Isa::Sse2)
    return runSse2(data, channels, cursor, ratio, env, out, numChannels, start, frames, pf,
                   base);
#endif
#if defined(MP_SIMD_NEON)
  if (isa == Isa::Neon)
    return runNeon(data, channels, cursor, ratio, env, out, numChannels, start, frames, pf,
                   base);
#endif
  (void)isa;
  return runScalar(data, channels, cursor, ratio, env, out, numChannels, start, frames, pf, 0,
                   base);
}

// How far the twenty-bit index may run: the byte offset of a sample is
// (i*5)>>1, so the product i*5 has to fit in an int. Past this the vector
// path would wrap round, and the scalar path takes over.
constexpr int64_t kPcm20IndexLimit = 400000000;

// The twenty-bit run. As every other width's, except that the index range is
// checked first: the packed layout costs an address computation per sample,
// and that computation has to fit.
double runPcm20(Isa isa, const Pcm20* data, int channels, double cursor, double ratio,
                float env, float* const* out, int numChannels, int start, int frames,
                PerFrame pf, int64_t base) {
  // How far the run reads: the cursor plus the steps it takes.
  double span = std::abs(ratio) * static_cast<double>(frames);
  if (pf.ratios != nullptr) {
    double worst = 0.0;
    for (int j = 0; j < frames; ++j) worst = std::max(worst, std::abs(pf.ratios[j]));
    span = worst * static_cast<double>(frames);
  }
  const double last = cursor - static_cast<double>(base) + span + 4.0;
  if (last * static_cast<double>(channels) > static_cast<double>(kPcm20IndexLimit))
    return runScalar(data, channels, cursor, ratio, env, out, numChannels, start, frames, pf, 0,
                     base);
  return run(isa, data, channels, cursor, ratio, env, out, numChannels, start, frames, pf, base);
}

}  // namespace

Isa detect() {
  const char* forced = std::getenv("MASTERPIECE_SIMD");
  if (forced != nullptr && forced[0] == '0') return Isa::None;
#if defined(MP_SIMD_X86)
#if defined(__APPLE__)
  // The Intel build on an Apple Silicon Mac runs under Rosetta, which does
  // not normally report AVX2 -- but has been seen to fault on it when it
  // does. SSE2 there, whatever the CPU check says: Rosetta translates SSE.
  int translated = 0;
  size_t size = sizeof(translated);
  if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 &&
      translated == 1)
    return Isa::Sse2;
#endif
  return avx2Present() ? Isa::Avx2 : Isa::Sse2;
#elif defined(MP_SIMD_NEON)
  return Isa::Neon;
#else
  return Isa::None;
#endif
}

std::vector<Isa> available() {
  std::vector<Isa> out;
#if defined(MP_SIMD_X86)
  out.push_back(Isa::Sse2);
  if (avx2Present()) out.push_back(Isa::Avx2);
#elif defined(MP_SIMD_NEON)
  out.push_back(Isa::Neon);
#endif
  return out;
}

const char* isaName(Isa isa) {
  switch (isa) {
    case Isa::Avx2: return "AVX2";
    case Isa::Sse2: return "SSE2";
    case Isa::Neon: return "NEON";
    case Isa::None: break;
  }
  return "none";
}

bool runSupports(Isa isa, int bufChannels) {
  if (bufChannels < 1) return false;
  if (isa == Isa::Avx2) return true;
  if (isa == Isa::Sse2) return bufChannels <= 2;
  if (isa == Isa::Neon) return bufChannels <= 2;
  return false;
}

double renderRun(Isa isa, const float* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames,
                 int64_t base) {
  return run(isa, data, bufChannels, cursor, ratio, env, out, numChannels, start, frames, {},
             base);
}
double renderRun(Isa isa, const int16_t* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames,
                 int64_t base) {
  return run(isa, data, bufChannels, cursor, ratio, env, out, numChannels, start, frames, {},
             base);
}
double renderRun(Isa isa, const Pcm24* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames,
                 int64_t base) {
  return run(isa, data, bufChannels, cursor, ratio, env, out, numChannels, start, frames, {},
             base);
}
double renderRun(Isa isa, const Pcm20* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames,
                 int64_t base) {
  return runPcm20(isa, data, bufChannels, cursor, ratio, env, out, numChannels, start, frames,
                  {}, base);
}

double renderRunPerFrame(Isa isa, const float* data, int bufChannels, double cursor,
                         const double* ratios, const float* envs, float* const* out,
                         int numChannels, int start, int frames, int64_t base) {
  return run(isa, data, bufChannels, cursor, 0.0, 0.0f, out, numChannels, start, frames,
             PerFrame{ratios, envs}, base);
}
double renderRunPerFrame(Isa isa, const int16_t* data, int bufChannels, double cursor,
                         const double* ratios, const float* envs, float* const* out,
                         int numChannels, int start, int frames, int64_t base) {
  return run(isa, data, bufChannels, cursor, 0.0, 0.0f, out, numChannels, start, frames,
             PerFrame{ratios, envs}, base);
}
double renderRunPerFrame(Isa isa, const Pcm24* data, int bufChannels, double cursor,
                         const double* ratios, const float* envs, float* const* out,
                         int numChannels, int start, int frames, int64_t base) {
  return run(isa, data, bufChannels, cursor, 0.0, 0.0f, out, numChannels, start, frames,
             PerFrame{ratios, envs}, base);
}
double renderRunPerFrame(Isa isa, const Pcm20* data, int bufChannels, double cursor,
                         const double* ratios, const float* envs, float* const* out,
                         int numChannels, int start, int frames, int64_t base) {
  return runPcm20(isa, data, bufChannels, cursor, 0.0, 0.0f, out, numChannels, start, frames,
                  PerFrame{ratios, envs}, base);
}

}  // namespace mp::simd
