// The A/B harness at the engine's level: one TraceScript played through a
// whole MasterpieceProcessor, headless, with the legacy or the event network,
// and a trace of what changed block by block: switches, controls, and the
// pipes speaking (a voice in its attack or loop; a releasing voice is not
// speaking). Two such traces diffed with diffTraces() compare what the
// player hears and sees, whichever paths inside produced it.
//
// Keys go in as MIDI note messages, one channel per keyboard the script
// plays, through the processor's own channel mapping. Every script step runs
// one block after it; a wait runs blocks until the time has passed.
#pragma once
#include "../mp_control/TraceSimulator.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace mp {

struct ProcessorTraceOptions {
  double sampleRate = 48000.0;
  int blockSize = 256;
  // With standIns off: the preload head per sample, 0 for whole files. A
  // minimum, not a cap: the read always covers the sustain loop.
  int64_t maxFramesPerSample = 0;
  // Blocks run after the load before the "init" state is taken, so start-up
  // pulses and ramps are part of the trace's first lines when they are short.
  int settleBlocks = 1;
  // Every sample a silent stand-in (SampleLibrary::setStandIns): the trace
  // reads the voice pool, and a large set held 13.6 GB loaded for real.
  bool standIns = true;
};

struct ProcessorTraceResult {
  bool loaded = false;
  std::string error;
  std::vector<std::string> lines;
};

ProcessorTraceResult runProcessorTrace(const juce::File& definition, OrganNetwork::Kind kind,
                                       const TraceScript& script, const ProcessorTraceOptions& options = {});

} // namespace mp
