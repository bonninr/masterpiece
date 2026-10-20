// Masterpiece tests -- the lossless codec measurement (#447, phase 1).
//
// Perf category. It measures the codec over the samples of an installed
// organ: the compression ratio at 16, 20 and 24 bits, the decode speed on
// one core, and the longest loop the organ holds. The samples are not in the
// repository, so the measurement is given a directory to read and does
// nothing when there is none.
//
//   MP_CODEC_SAMPLES   a directory of WAV files, or several separated by ';'
//   MP_CODEC_ODF       the organ definition, for the voice count
//
// The storage rules are the ones the player uses (SampleLibrary): 16 and 20
// bits scale each file by its own peak, so a quiet stop keeps every bit;
// 24 bits stores what the file holds.
//
// Run: mp_tests --perf-only
#include "MpTest.h"

#ifdef MP_TEST_HAS_SAMPLER
#include "../src/mp_sampler/LosslessBlocks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#endif

using mp::test::Category;

#ifdef MP_TEST_HAS_SAMPLER

namespace {

// ---------------------------------------------------------------- the WAVs

struct WavData {
  int channels = 0;
  int bitDepth = 0;
  double sampleRate = 0.0;
  std::vector<int32_t> samples; // interleaved, at the file's own width
  int64_t loopStart = -1;       // the file's own frames
  int64_t loopEnd = -1;         // inclusive, as the file says
  int64_t loopLength = 0;       // the frames of the longest loop
};

uint32_t readU32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readU16(const unsigned char* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                (static_cast<uint16_t>(p[1]) << 8));
}

// Reads a WAV file: the samples at the file's own width, and the loop
// points out of the smpl chunk.
bool readWav(const std::filesystem::path& path, WavData& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return false;
  unsigned char header[12];
  if (!file.read(reinterpret_cast<char*>(header), 12))
    return false;
  if (std::memcmp(header, "RIFF", 4) != 0 ||
      std::memcmp(header + 8, "WAVE", 4) != 0)
    return false;

  int format = 0;
  std::vector<unsigned char> data;
  bool haveFormat = false;
  for (;;) {
    unsigned char chunk[8];
    if (!file.read(reinterpret_cast<char*>(chunk), 8))
      break;
    const uint32_t size = readU32(chunk + 4);
    const std::string id(reinterpret_cast<char*>(chunk), 4);
    if (id == "fmt ") {
      std::vector<unsigned char> body(size < 16 ? 16 : size);
      if (!file.read(reinterpret_cast<char*>(body.data()),
                     static_cast<std::streamsize>(body.size())))
        break;
      format = readU16(body.data());
      if (format == 0xFFFE && body.size() >= 26)
        format = readU16(body.data() + 24);
      out.channels = readU16(body.data() + 2);
      out.sampleRate = static_cast<double>(readU32(body.data() + 4));
      out.bitDepth = readU16(body.data() + 14);
      haveFormat = true;
    } else if (id == "data") {
      data.resize(size);
      if (size > 0 &&
          !file.read(reinterpret_cast<char*>(data.data()),
                     static_cast<std::streamsize>(size)))
        break;
    } else if (id == "smpl") {
      std::vector<unsigned char> body(size);
      if (size >= 36 &&
          file.read(reinterpret_cast<char*>(body.data()),
                    static_cast<std::streamsize>(size))) {
        const uint32_t loops = readU32(body.data() + 28);
        for (uint32_t l = 0; l < loops; ++l) {
          const size_t at = 36 + static_cast<size_t>(l) * 24;
          if (at + 24 > body.size())
            break;
          const uint32_t start = readU32(body.data() + at + 8);
          const uint32_t end = readU32(body.data() + at + 12);
          if (end < start)
            continue;
          // The end of a loop is the last sample of it, inclusive.
          const int64_t length = static_cast<int64_t>(end - start) + 1;
          if (length > out.loopLength) {
            out.loopLength = length;
            out.loopStart = start;
            out.loopEnd = end;
          }
        }
      }
    } else {
      file.seekg(static_cast<std::streamoff>(size) + (size & 1u),
                 std::ios::cur);
      continue;
    }
    if ((size & 1u) != 0)
      file.seekg(1, std::ios::cur);
  }
  if (!haveFormat || out.channels <= 0)
    return false;

  const size_t frameBytes =
      static_cast<size_t>(out.bitDepth / 8) * static_cast<size_t>(out.channels);
  if (frameBytes == 0)
    return false;
  const size_t frames = data.size() / frameBytes;
  out.samples.resize(frames * static_cast<size_t>(out.channels));
  for (size_t i = 0; i < out.samples.size(); ++i) {
    const unsigned char* p = data.data() + i * (out.bitDepth / 8);
    int32_t value = 0;
    if (format == 3 && out.bitDepth == 32) {
      float f = 0.0f;
      std::memcpy(&f, p, 4);
      const double scaled = static_cast<double>(f) * 2147483648.0;
      value = static_cast<int32_t>(
          scaled > 2147483647.0 ? 2147483647.0
                                 : (scaled < -2147483648.0 ? -2147483648.0 : scaled));
    } else if (out.bitDepth == 16) {
      value = static_cast<int16_t>(readU16(p));
    } else if (out.bitDepth == 24) {
      const uint32_t raw = static_cast<uint32_t>(p[0]) |
                           (static_cast<uint32_t>(p[1]) << 8) |
                           (static_cast<uint32_t>(p[2]) << 16);
      value = static_cast<int32_t>((raw & 0x800000u) ? raw | 0xFF000000u
                                                      : raw);
    } else if (out.bitDepth == 32) {
      value = static_cast<int32_t>(readU32(p));
    } else if (out.bitDepth == 8) {
      value = (static_cast<int32_t>(p[0]) - 128) * 256;
    } else {
      return false;
    }
    out.samples[i] = value;
  }
  return true;
}

// ------------------------------------------------------- the measurement

struct Totals {
  uint64_t files = 0;
  uint64_t frames = 0;
  uint64_t rawBytes = 0;
  uint64_t codedBytes = 0;
  double decodeSeconds = 0.0;
  uint64_t decodedFrames = 0;
};

// The samples of one file at one width, in the rule the player uses.
std::vector<int32_t> scaleTo(const std::vector<int32_t>& source, int width,
                             bool scaleByPeak) {
  const int64_t full = (static_cast<int64_t>(1) << (width - 1)) - 1;
  std::vector<int32_t> out(source.size());
  if (!scaleByPeak) {
    for (size_t i = 0; i < source.size(); ++i) {
      int64_t v = source[i];
      if (v > full)
        v = full;
      if (v < -full)
        v = -full;
      out[i] = static_cast<int32_t>(v);
    }
    return out;
  }
  int64_t peak = 0;
  for (int32_t v : source)
    peak = std::max<int64_t>(peak, v < 0 ? -static_cast<int64_t>(v) : v);
  if (peak == 0)
    return out;
  const double toCounts = static_cast<double>(full) / static_cast<double>(peak);
  for (size_t i = 0; i < source.size(); ++i) {
    const double scaled = std::round(static_cast<double>(source[i]) * toCounts);
    const int64_t v = scaled > static_cast<double>(full)
                          ? full
                          : (scaled < -static_cast<double>(full) ? -full
                                                                 : static_cast<int64_t>(scaled));
    out[i] = static_cast<int32_t>(v);
  }
  return out;
}

// The organ definition: how many ranks and pipes, and so how many voices a
// full chord of ten keys sounds. The ranks the stops draw are the ones with
// a StopRank entry; the pipes of a rank are what sound.
struct OrganVoices {
  uint64_t ranks = 0;
  uint64_t pipes = 0;
  uint64_t voices = 0; // the pipes that sound with ten keys down
};

OrganVoices readVoices(const std::filesystem::path& odf) {
  OrganVoices voices;
  std::ifstream file(odf, std::ios::binary);
  if (!file)
    return voices;
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());

  // The ranks the stops draw.
  std::vector<bool> drawn(1, false);
  size_t at = 0;
  while ((at = text.find("<StopRank>", at)) != std::string::npos) {
    const size_t idAt = text.find("<RankID>", at);
    const size_t idEnd = text.find("</RankID>", at);
    if (idAt != std::string::npos && idEnd != std::string::npos &&
        idEnd > idAt) {
      const uint64_t id = static_cast<uint64_t>(
          std::strtoull(text.c_str() + idAt + 8, nullptr, 10));
      if (id >= drawn.size())
        drawn.resize(id + 1, false);
      drawn[id] = true;
    }
    at += 9;
  }

  // The pipes of each rank.
  std::vector<uint64_t> perRank(1, 0);
  at = 0;
  while ((at = text.find("<Pipe_SoundEngine01>", at)) != std::string::npos) {
    ++voices.pipes;
    const size_t idAt = text.find("<RankID>", at);
    const size_t idEnd = text.find("</RankID>", at);
    if (idAt != std::string::npos && idEnd != std::string::npos &&
        idEnd > idAt) {
      const uint64_t id = static_cast<uint64_t>(
          std::strtoull(text.c_str() + idAt + 8, nullptr, 10));
      if (id >= perRank.size())
        perRank.resize(id + 1, 0);
      ++perRank[id];
    }
    at += 18;
  }
  at = 0;
  while ((at = text.find("<Rank>", at)) != std::string::npos) {
    ++voices.ranks;
    at += 6;
  }
  for (size_t r = 0; r < perRank.size(); ++r)
    if (r < drawn.size() && drawn[r])
      voices.voices += std::min<uint64_t>(10, perRank[r]);
  return voices;
}

class CodecBenchTest final : public mp::test::Test {
public:
  CodecBenchTest() : Test("perf.codec.organ-samples", Category::Perf) {}
  void run() override {
    // The organ definition first: it is a small file and it answers what a
    // full chord asks of the decoder.
    const char* odfEnv = std::getenv("MP_CODEC_ODF");
    if (odfEnv != nullptr && *odfEnv != 0) {
      const OrganVoices voices = readVoices(std::filesystem::path(odfEnv));
      if (voices.ranks == 0) {
        std::printf("        organ definition: not in the 4.0 format, the "
                    "voice count was not read\n");
      } else {
        std::printf("        organ definition: %llu ranks, %llu pipes, %llu "
                    "voices with ten keys down\n",
                    static_cast<unsigned long long>(voices.ranks),
                    static_cast<unsigned long long>(voices.pipes),
                    static_cast<unsigned long long>(voices.voices));
        std::printf("        a full chord at 48 kHz needs %llu decoded frames "
                    "a second\n",
                    static_cast<unsigned long long>(voices.voices) * 48000ull);
      }
    }

    const char* env = std::getenv("MP_CODEC_SAMPLES");
    if (env == nullptr || *env == 0) {
      std::printf("        no MP_CODEC_SAMPLES: the organ samples are not "
                  "here, the measurement did not run\n");
      return;
    }
    std::string roots(env);
    std::vector<std::filesystem::path> directories;
    size_t at = 0;
    while (at <= roots.size()) {
      const size_t cut = roots.find(';', at);
      const std::string one = roots.substr(
          at, cut == std::string::npos ? std::string::npos : cut - at);
      if (!one.empty())
        directories.emplace_back(one);
      if (cut == std::string::npos)
        break;
      at = cut + 1;
    }
    if (directories.empty()) {
      std::printf("        MP_CODEC_SAMPLES names no directory\n");
      return;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& directory : directories) {
      std::error_code error;
      if (!std::filesystem::is_directory(directory, error)) {
        std::printf("        %s is not a directory\n",
                    directory.string().c_str());
        continue;
      }
      for (const auto& entry :
           std::filesystem::recursive_directory_iterator(directory, error)) {
        if (!entry.is_regular_file())
          continue;
        const std::string name = entry.path().extension().string();
        if (name.size() >= 4 &&
            (name.compare(name.size() - 4, 4, ".wav") == 0 ||
             name.compare(name.size() - 4, 4, ".WAV") == 0))
          files.push_back(entry.path());
      }
    }
    if (files.empty()) {
      std::printf("        no WAV files under %s\n",
                  directories.front().string().c_str());
      return;
    }
    std::sort(files.begin(), files.end());

    const int widths[] = {16, 20, 24};
    Totals totals[3];
    int64_t longestLoop = 0;
    std::string longestLoopFile;
    uint64_t skipped = 0;
    uint64_t failures = 0;

    for (const auto& path : files) {
      WavData wav;
      if (!readWav(path, wav) || wav.samples.empty()) {
        ++skipped;
        continue;
      }
      if (wav.loopLength > longestLoop) {
        longestLoop = wav.loopLength;
        longestLoopFile = path.filename().string();
      }
      const uint64_t frames = wav.samples.size() /
                              static_cast<size_t>(wav.channels);
      for (int w = 0; w < 3; ++w) {
        const int width = widths[w];
        // 24 bits stores what the file holds; the narrower widths scale by
        // the file's own peak, as the player does.
        const std::vector<int32_t> samples =
            scaleTo(wav.samples, width, width != 24);
        mp::LosslessStream stream;
        mp::LosslessError error = mp::LosslessError::None;
        if (!mp::encodeLosslessBlocks(samples.data(), frames, wav.channels,
                                       width, stream, &error)) {
          std::printf("        %s: encode failed: %s\n",
                      path.filename().string().c_str(),
                      mp::losslessErrorText(error));
          ++skipped;
          continue;
        }
        std::vector<int32_t> output(samples.size(), 0);
        const auto start = std::chrono::steady_clock::now();
        const bool ok = mp::decodeLosslessFrames(stream, 0, frames,
                                                 output.data(), &error);
        const auto stop = std::chrono::steady_clock::now();
        if (!ok) {
          std::printf("        %s: %d bits, %llu frames: decode failed: %s\n",
                      path.filename().string().c_str(), width,
                      static_cast<unsigned long long>(frames),
                      mp::losslessErrorText(error));
          ++failures;
          continue;
        }
        if (output != samples) {
          size_t at = 0;
          while (at < output.size() && output[at] == samples[at])
            ++at;
          std::printf("        %s: %d bits: not lossless at sample %zu "
                      "(in %d, out %d)\n",
                      path.filename().string().c_str(), width, at,
                      samples[at], output[at]);
          ++failures;
          continue;
        }
        Totals& total = totals[w];
        ++total.files;
        total.frames += frames;
        total.rawBytes += frames * static_cast<uint64_t>(wav.channels) *
                          static_cast<uint64_t>(width) / 8u;
        total.codedBytes += stream.data.size();
        total.decodeSeconds +=
            std::chrono::duration<double>(stop - start).count();
        total.decodedFrames += frames;
      }
    }

    std::printf("\n");
    std::printf("        organ samples: %zu files, %llu skipped, %llu failed\n",
                files.size(), static_cast<unsigned long long>(skipped),
                static_cast<unsigned long long>(failures));
    std::printf("        %-6s %10s %14s %14s %8s %12s\n", "width", "frames",
                "raw bytes", "coded bytes", "ratio", "decode fps");
    for (int w = 0; w < 3; ++w) {
      const Totals& total = totals[w];
      const double ratio =
          total.codedBytes > 0
              ? static_cast<double>(total.rawBytes) /
                    static_cast<double>(total.codedBytes)
              : 0.0;
      const double fps =
          total.decodeSeconds > 0
              ? static_cast<double>(total.decodedFrames) / total.decodeSeconds
              : 0.0;
      std::printf("        %-6d %10llu %14llu %14llu %8.2f %12.0f\n",
                  widths[w], static_cast<unsigned long long>(total.frames),
                  static_cast<unsigned long long>(total.rawBytes),
                  static_cast<unsigned long long>(total.codedBytes), ratio, fps);
    }
    if (longestLoop > 0) {
      const double seconds = static_cast<double>(longestLoop) / 48000.0;
      std::printf("        longest loop: %lld frames (%.2f s at 48 kHz, %s)\n",
                  static_cast<long long>(longestLoop), seconds,
                  longestLoopFile.c_str());
    }

    MP_CHECK(failures == 0,
             std::to_string(failures) + " sample(s) did not survive the codec");
  }
};

} // namespace

static CodecBenchTest g_codecBench;

#endif // MP_TEST_HAS_SAMPLER
