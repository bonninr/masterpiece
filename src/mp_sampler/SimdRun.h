// The engine's inner loop for the frames where nothing happens but playing.
//
// Most of a voice's life is a straight run: the key is held, the cursor is
// well inside the resident sample and short of the loop end, no crossfade is
// running and no tremulant is moving the pitch. For those frames the engine
// only interpolates and mixes, and that is the part a CPU's vector unit can
// do several frames at a time. Everything else -- loop wraps, crossfades,
// release fades, streamed tails, the tremulant -- stays on the ordinary
// per-frame path, which is also the whole engine on a CPU without them.
//
// The vector code gives the same answer as the per-frame reader: positions
// advance one frame at a time exactly as there, and the interpolation uses
// the same operations in the same order, without fused multiply-adds. So the
// choice changes the speed and not the sound.
//
// JUCE-free, like the rest of mp_sampler.
#pragma once

#include <vector>
#include <cstdint>

namespace mp {

struct Pcm24;

namespace simd {

enum class Isa {
  None,  // the per-frame path only
  Sse2,  // every x86-64 processor: four frames at a time, for those without AVX2
  Avx2,  // x86-64 with AVX2, checked when the program starts
  Neon,  // ARM: always present on 64-bit, and required by the 32-bit build
};

// What this machine offers, unless MASTERPIECE_SIMD=0 is set in the
// environment, which forces the per-frame path for comparison.
Isa detect();
const char* isaName(Isa isa);
// Every unit this machine can run, whatever detect() would choose: what the
// equivalence test compares against the per-frame path.
std::vector<Isa> available();

// The frames the vector code needs on either side of a tap: a run may use
// the positions from 1 to resident - kRunMargin.
constexpr int64_t kRunMargin = 4;

// Render `frames` frames starting at out[..][start], reading from the
// resident storage at `cursor` and stepping by `ratio`, each output added at
// `env`. Returns the cursor after the run. The caller guarantees every
// position in the run satisfies 1 <= pos < resident - kRunMargin. Channel
// counts above two fall back to the per-frame path on NEON; the caller asks
// runSupports() first.
bool runSupports(Isa isa, int bufChannels);
double renderRun(Isa isa, const float* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames);
double renderRun(Isa isa, const int16_t* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames);
double renderRun(Isa isa, const Pcm24* data, int bufChannels, double cursor, double ratio,
                 float env, float* const* out, int numChannels, int start, int frames);

}  // namespace simd
}  // namespace mp
