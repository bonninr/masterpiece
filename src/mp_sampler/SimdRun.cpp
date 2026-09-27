#include "SimdRun.h"

#include "VoiceEngine.h"

#include <algorithm>
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

template <typename T>
double runScalar(const T* data, int channels, double cursor, double ratio, float env,
                 float* const* out, int numChannels, int start, int frames) {
  double p = cursor;
  for (int j = 0; j < frames; ++j) {
    const auto k = static_cast<int64_t>(p);
    scalarFrame(data, channels, k, static_cast<float>(p - static_cast<double>(k)), env, out,
                numChannels, start + j);
    p += ratio;
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

template <typename T>
MP_AVX2 double runAvx2(const T* data, int channels, double cursor, double ratio, float env,
                       float* const* out, int numChannels, int start, int frames) {
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
      base[l] = static_cast<int32_t>((k - 1) * channels);
      p += ratio;
    }
    const __m256i b = _mm256_load_si256(reinterpret_cast<const __m256i*>(base));
    const __m256 t = _mm256_load_ps(frac);
    const int sources = channels == 1 ? 1 : numChannels;
    for (int s = 0; s < sources; ++s) {
      const int srcCh = s < channels ? s : channels - 1;
      const __m256i e0 = _mm256_add_epi32(b, _mm256_set1_epi32(srcCh));
      const __m256i e1 = _mm256_add_epi32(e0, step);
      const __m256i e2 = _mm256_add_epi32(e1, step);
      const __m256i e3 = _mm256_add_epi32(e2, step);
      const __m256 y = _mm256_mul_ps(
          hermite8(gather8(data, e0), gather8(data, e1), gather8(data, e2), gather8(data, e3), t),
          envv);
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
                                frames - j);
  return rest;
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

template <typename T>
double runNeon(const T* data, int channels, double cursor, double ratio, float env,
               float* const* out, int numChannels, int start, int frames) {
  const float32x4_t envv = vdupq_n_f32(env);
  double p = cursor;
  int j = 0;
  for (; j + 4 <= frames; j += 4) {
    float32x4_t rows[4][2];
    float frac[4];
    for (int l = 0; l < 4; ++l) {
      const auto k = static_cast<int64_t>(p);
      frac[l] = static_cast<float>(p - static_cast<double>(k));
      taps(data, channels, k, rows[l]);
      p += ratio;
    }
    const float32x4_t t = vld1q_f32(frac);
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
      const float32x4_t y = vmulq_f32(hermite4(xm1, x0, x1, x2, t), envv);
      const int from = channels == 1 ? 0 : s;
      const int to = channels == 1 ? numChannels : s + 1;
      for (int ch = from; ch < to; ++ch) {
        float* o = out[ch] + start + j;
        vst1q_f32(o, vaddq_f32(vld1q_f32(o), y));
      }
    }
  }
  return runScalar(data, channels, p, ratio, env, out, numChannels, start + j, frames - j);
}
#endif  // MP_SIMD_NEON

template <typename T>
double run(Isa isa, const T* data, int channels, double cursor, double ratio, float env,
           float* const* out, int numChannels, int start, int frames) {
#if defined(MP_SIMD_X86)
  if (isa == Isa::Avx2)
    return runAvx2(data, channels, cursor, ratio, env, out, numChannels, start, frames);
#endif
#if defined(MP_SIMD_NEON)
  if (isa == Isa::Neon)
    return runNeon(data, channels, cursor, ratio, env, out, numChannels, start, frames);
#endif
  (void)isa;
  return runScalar(data, channels, cursor, ratio, env, out, numChannels, start, frames);
}

}  // namespace

Isa detect() {
  const char* forced = std::getenv("MASTERPIECE_SIMD");
  if (forced != nullptr && forced[0] == '0') return Isa::None;
#if defined(MP_SIMD_X86)
#if defined(__APPLE__)
  // The Intel build on an Apple Silicon Mac runs under Rosetta, which does
  // not normally report AVX2 -- but has been seen to fault on it when it
  // does. The per-frame path there, whatever the CPU check says.
  int translated = 0;
  size_t size = sizeof(translated);
  if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 &&
      translated == 1)
    return Isa::None;
#endif
  return avx2Present() ? Isa::Avx2 : Isa::None;
#elif defined(MP_SIMD_NEON)
  return Isa::Neon;
#else
  return Isa::None;
#endif
}

const char* isaName(Isa isa) {
  switch (isa) {
    case Isa::Avx2: return "AVX2";
    case Isa::Neon: return "NEON";
    case Isa::None: break;
  }
  return "none";
}

bool runSupports(Isa isa, int bufChannels) {
  if (bufChannels < 1) return false;
  if (isa == Isa::Avx2) return true;
  if (isa == Isa::Neon) return bufChannels <= 2;
  return false;
}

double renderRun(Isa isa, const float* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames) {
  return run(isa, data, bufChannels, cursor, ratio, env, out, numChannels, start, frames);
}
double renderRun(Isa isa, const int16_t* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames) {
  return run(isa, data, bufChannels, cursor, ratio, env, out, numChannels, start, frames);
}
double renderRun(Isa isa, const Pcm24* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames) {
  return run(isa, data, bufChannels, cursor, ratio, env, out, numChannels, start, frames);
}

}  // namespace mp::simd
