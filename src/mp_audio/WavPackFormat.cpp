#include "WavPackFormat.h"

#include <wavpack.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mp {
namespace {

// libwavpack reads through callbacks; these serve it from a JUCE stream, so
// a file on disk and a sample decompressed from an archive are read alike.
struct StreamIo {
  static juce::InputStream& in(void* id) { return *static_cast<juce::InputStream*>(id); }
  static int32_t read(void* id, void* data, int32_t count) {
    return count <= 0 ? 0 : in(id).read(data, count);
  }
  static int32_t write(void*, void*, int32_t) { return 0; }
  static int64_t getPos(void* id) { return in(id).getPosition(); }
  static int setAbs(void* id, int64_t pos) { return in(id).setPosition(pos) ? 0 : -1; }
  static int setRel(void* id, int64_t delta, int mode) {
    auto& s = in(id);
    int64_t base = 0;
    if (mode == SEEK_CUR) base = s.getPosition();
    else if (mode == SEEK_END) base = s.getTotalLength();
    return s.setPosition(base + delta) ? 0 : -1;
  }
  static int pushBack(void* id, int c) {
    auto& s = in(id);
    const int64_t pos = s.getPosition();
    return pos > 0 && s.setPosition(pos - 1) ? c : EOF;
  }
  static int64_t length(void* id) { return in(id).getTotalLength(); }
  static int canSeek(void*) { return 1; }
  static int truncate(void*) { return -1; }
  static int close(void*) { return 0; }
};

WavpackStreamReader64 streamReader = {StreamIo::read,   StreamIo::write,    StreamIo::getPos,
                                      StreamIo::setAbs, StreamIo::setRel,   StreamIo::pushBack,
                                      StreamIo::length, StreamIo::canSeek,  StreamIo::truncate,
                                      StreamIo::close};

uint32_t le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// The chunks of the WAV the file was made from, as JUCE's WAV reader reports
// them. The wrapper holds the header and whatever followed the audio; the
// data chunk's own header is there but its bytes are not, so the walk steps
// over the header alone, as GrandOrgue's does.
void readWrapperChunks(const uint8_t* p, size_t n, juce::StringPairArray& md) {
  if (n < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return;
  size_t pos = 12;
  while (pos + 8 <= n) {
    const uint8_t* id = p + pos;
    const uint32_t size = le32(p + pos + 4);
    const uint8_t* body = p + pos + 8;
    const size_t avail = n - (pos + 8);
    if (std::memcmp(id, "data", 4) == 0) {
      pos += 8;
      continue;
    }
    if (std::memcmp(id, "smpl", 4) == 0 && size >= 36 && avail >= 36) {
      md.set("MidiUnityNote", juce::String(le32(body + 12)));
      md.set("MidiPitchFraction", juce::String(static_cast<juce::int64>(le32(body + 16))));
      const uint32_t loops = le32(body + 28);
      uint32_t kept = 0;
      for (uint32_t i = 0; i < loops && 36 + (i + 1) * 24 <= std::min<size_t>(size, avail); ++i) {
        const uint8_t* l = body + 36 + i * 24;
        const juce::String prefix = "Loop" + juce::String(kept);
        md.set(prefix + "Identifier", juce::String(le32(l)));
        md.set(prefix + "Type", juce::String(le32(l + 4)));
        md.set(prefix + "Start", juce::String(static_cast<juce::int64>(le32(l + 8))));
        md.set(prefix + "End", juce::String(static_cast<juce::int64>(le32(l + 12))));
        ++kept;
      }
      md.set("NumSampleLoops", juce::String(kept));
    } else if (std::memcmp(id, "cue ", 4) == 0 && size >= 4 && avail >= 4) {
      const uint32_t cues = le32(body);
      uint32_t kept = 0;
      for (uint32_t i = 0; i < cues && 4 + (i + 1) * 24 <= std::min<size_t>(size, avail); ++i) {
        const uint8_t* c = body + 4 + i * 24;
        const juce::String prefix = "Cue" + juce::String(kept);
        md.set(prefix + "Identifier", juce::String(le32(c)));
        md.set(prefix + "Offset", juce::String(static_cast<juce::int64>(le32(c + 20))));
        ++kept;
      }
      md.set("NumCuePoints", juce::String(kept));
    }
    pos += 8 + size + (size & 1);
  }
}

class WavPackReader : public juce::AudioFormatReader {
public:
  WavPackReader(juce::InputStream* in, WavpackContext* ctx)
    : juce::AudioFormatReader(in, "WavPack"), ctx_(ctx) {
    sampleRate = WavpackGetSampleRate(ctx_);
    numChannels = static_cast<unsigned int>(WavpackGetNumChannels(ctx_));
    lengthInSamples = WavpackGetNumSamples64(ctx_);
    bitsPerSample = static_cast<unsigned int>(WavpackGetBitsPerSample(ctx_));
    bytesPerSample_ = WavpackGetBytesPerSample(ctx_);
    usesFloatingPointData = (WavpackGetMode(ctx_) & MODE_FLOAT) != 0;

    // The chunks after the audio are only reachable by seeking there; the
    // decoder is put back at the start afterwards.
    WavpackSeekTrailingWrapper(ctx_);
    readWrapperChunks(WavpackGetWrapperData(ctx_), WavpackGetWrapperBytes(ctx_), metadataValues);
    WavpackFreeWrapper(ctx_);
    WavpackSeekSample64(ctx_, 0);
    position_ = 0;
  }

  ~WavPackReader() override { WavpackCloseFile(ctx_); }

  bool readSamples(int* const* dest, int numDest, int destOffset, juce::int64 start,
                   int numSamples) override {
    const int channels = static_cast<int>(numChannels);
    // Past the end reads as silence, as every JUCE reader's does.
    if (start >= lengthInSamples) {
      for (int c = 0; c < numDest; ++c)
        if (dest[c] != nullptr) std::memset(dest[c] + destOffset, 0, sizeof(int) * static_cast<size_t>(numSamples));
      return true;
    }
    if (start != position_) {
      if (!WavpackSeekSample64(ctx_, start)) return false;
      position_ = start;
    }
    const int shift = usesFloatingPointData ? 0 : 32 - 8 * bytesPerSample_;
    int done = 0;
    while (done < numSamples) {
      const int want = std::min(numSamples - done, 4096);
      scratch_.resize(static_cast<size_t>(want * channels));
      const auto got = static_cast<int>(WavpackUnpackSamples(ctx_, scratch_.data(), static_cast<uint32_t>(want)));
      for (int c = 0; c < numDest; ++c) {
        if (dest[c] == nullptr) continue;
        int* out = dest[c] + destOffset + done;
        if (c >= channels) {
          std::memset(out, 0, sizeof(int) * static_cast<size_t>(want));
          continue;
        }
        for (int i = 0; i < got; ++i) {
          const int32_t v = scratch_[static_cast<size_t>(i * channels + c)];
          // Integer samples come right-justified in their byte width; JUCE
          // wants them left-justified. Floats pass through as their bits.
          out[i] = shift > 0 ? static_cast<int>(static_cast<uint32_t>(v) << shift) : v;
        }
        if (got < want) std::memset(out + got, 0, sizeof(int) * static_cast<size_t>(want - got));
      }
      position_ += got;
      done += want;
      if (got < want) break;
    }
    return true;
  }

private:
  WavpackContext* ctx_;
  int bytesPerSample_ = 2;
  juce::int64 position_ = 0;
  std::vector<int32_t> scratch_;
};

}  // namespace

WavPackAudioFormat::WavPackAudioFormat()
  : juce::AudioFormat("WavPack", juce::StringArray{".wv", ".wav", ".wave"}) {}

juce::AudioFormatReader* WavPackAudioFormat::createReaderFor(juce::InputStream* source,
                                                             bool deleteStreamIfOpeningFails) {
  // By content: a WavPack file named .wav is still WavPack, and a real WAV is
  // left for the WAV reader behind this one.
  char magic[4] = {};
  const auto start = source->getPosition();
  const bool isWavPack = source->read(magic, 4) == 4 && std::memcmp(magic, "wvpk", 4) == 0;
  source->setPosition(start);
  WavpackContext* ctx = nullptr;
  if (isWavPack) {
    char error[128] = {};
    // Whatever the file holds -- any version, lossless or hybrid -- the
    // library decides; the wrapper is asked for to recover the WAV chunks,
    // and floats are normalised to the usual +-1.
    ctx = WavpackOpenFileInputEx64(&streamReader, source, nullptr, error,
                                   OPEN_WRAPPER | OPEN_NORMALIZE, 0);
  }
  if (ctx == nullptr) {
    if (deleteStreamIfOpeningFails) delete source;
    return nullptr;
  }
  return new WavPackReader(source, ctx);
}

void registerSampleFormats(juce::AudioFormatManager& formats) {
  formats.registerFormat(new WavPackAudioFormat(), false);
  formats.registerBasicFormats();
}

}  // namespace mp
