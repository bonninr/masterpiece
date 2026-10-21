// Masterpiece -- the codecs that ship with the program.
//
// One codec that compresses, the predictive delta codec, and plain storage,
// which is the samples as they are. Another codec is added the same way:
// a SampleCodec, registered under a four-character name of its own.
#pragma once

#include "SampleCodec.h"

#include <memory>

namespace mp {

// The predictive delta codec, "PDLT": lossless, sequential.
//
// Each sample is predicted from the two before it on its channel,
//
//   prediction = prev + (prev - last) / 2      (integer division, towards 0)
//
// and only the difference from the prediction is kept, in as few units as
// it needs: one to four bytes for samples of up to 16 bits, two to four
// bytes for wider ones. A slow, smooth wave leaves small differences. On
// two real sets, 60 samples each, the payload is 70 % of plain storage at
// 24 bits and at a mix of 16 and 24 bits.
//
// Decoding goes forward from the first frame. The state that carries it on
// is two numbers per channel and a position, so a voice keeps it, and a
// saved state at a loop's start lets the voice jump back there.
std::unique_ptr<SampleCodec> makeDeltaCodec();

// Plain storage.
//
//   bitDepth 32   the samples as 32-bit floats
//   bitDepth 24   the samples as 24-bit integers
//   bitDepth 16   the samples as 16-bit integers
//
// The width is fixed by the codec, and its name says which: "PCMF", "PCM3"
// and "PCM2".
std::unique_ptr<SampleCodec> makePlainCodec(int bitDepth);

// Adds the codecs above to a registry.
void registerBuiltinCodecs(CodecRegistry& registry);

} // namespace mp
