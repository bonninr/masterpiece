// Masterpiece -- the codecs that ship with the program.
//
// Two families: the lossless block codec, whose blocks are independent and
// can be decoded one at a time, and plain storage, which is the samples as
// they are. The other codecs are measured in the streaming lab and stay
// there; the interface is the same for all of them.
#pragma once

#include "SampleCodec.h"

#include <memory>

namespace mp {

// The lossless block codec: independent blocks, each with its own header
// and its own check. The block size and the width are given per sample,
// so one codec carries blocks of any size at any width it supports.
std::unique_ptr<SampleCodec> makeBlockCodec();

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
