#include "SampleLibrary.h"
#include "DataFolder.h"
#include "../mp_core/Utf8Path.h"
#include "PathCase.h"
#include "WavPackFormat.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <thread>
#include <unordered_set>
#include <vector>
#include <functional>
#include <list>
#include <mutex>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif
#if !defined(_WIN32)
#include <sys/resource.h>
#if defined(__APPLE__)
#include <sys/syslimits.h>
#include <pthread/qos.h>
#endif
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(_WIN32)
// The two calls it needs, declared here: windows.h beside JUCE's headers
// brings in more names than this file wants.
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void* thread, int priority);
#endif

namespace mp {
namespace {

// The thread that saves the sample cache while the organ plays runs only on
// time nothing else wants. It reads and checksums every sample it has:
// gigabytes, for half a minute or more. On a two-core computer it took a core
// from the first notes played, and the audio fell behind until it was done.
// The system's own lowest class for work in the background: idle scheduling
// on Linux, the background class on macOS and iOS, background mode on
// Windows, which lowers its disk use as well.
void runInTheBackground() {
#if defined(__linux__)
  sched_param none{};
  pthread_setschedparam(pthread_self(), SCHED_IDLE, &none);
#elif defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#elif defined(_WIN32)
  constexpr int kThreadModeBackgroundBegin = 0x00010000;
  SetThreadPriority(GetCurrentThread(), kThreadModeBackgroundBegin);
#endif
}

// A file decoded into the organ's own memory has no further use for its pages
// in the system's file cache. Left there, a load fills that cache with
// gigabytes of samples and the kernel evicts what it holds of everything else
// to make room, the program's own code included: on a machine running from an
// SD card, the window then stopped painting through every cold load while
// that code was read back. Linux and Android only; elsewhere a no-op.
void dropFromFileCache(const std::string& path) {
#if defined(__linux__)
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  ::close(fd);
#else
  (void)path;
#endif
}

bool hasSuffixInsensitive(const std::string& s, const char* suffix) {
  const std::string suf(suffix);
  if (s.size() < suf.size()) return false;
  return std::equal(suf.rbegin(), suf.rend(), s.rbegin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

bool isEncrypted(const std::string& file) {
  return hasSuffixInsensitive(file, ".hbw") || hasSuffixInsensitive(file, ".hbx");
}

// Resolve a sample's path under the organ root.
//
// Two things have to be right or nothing loads. Hauptwerk stores content in
// OrganInstallationPackages/<id zero-padded to SIX digits>, and the ODF names
// files relative to that package rather than to itself or to the set root. And
// ODFs are authored on Windows, so they carry backslashes, which are ordinary
// filename characters on Linux rather than separators.
std::filesystem::path resolvePath(const std::string& rootDir,
                                  const std::string& fileName,
                                  Id installationPackageId) {
  std::string rel = fileName;
  std::replace(rel.begin(), rel.end(), '\\', '/');
  while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());

  std::filesystem::path base = mp::pathOf(rootDir);
  if (installationPackageId > 0) {
    std::string digits = std::to_string(installationPackageId);
    if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
    base /= "OrganInstallationPackages";
    base /= digits;
  }
  return base / mp::pathOf(rel);
}

// Windows-authored sets record whatever case the author had; the disk on Linux
// does not care what they meant. Fall back to a case-insensitive walk of the
// parent directory rather than declaring a present file missing.
std::filesystem::path resolveIgnoringCase(const std::filesystem::path& wanted) {
  // Every folder of the path, not only the file name (#90).
  return mp::resolvePathIgnoringCase(wanted);
}

// Stand-ins for the format's own standard components (installation package 1),
// which come with the program that plays the format and with no sample set:
// generic key-action clicks, stop-action thumps and a blower. A set that names
// them -- a composite definition borrows them for its key and stop noises --
// would otherwise make no mechanical sound at all. Each is made once, kept in
// `dir`, and loaded as any sample is. A tremulant waveform in that package is
// not a sound and is not asked for.
//
// Returns the file made, or an empty path when the name is not one of those.
std::filesystem::path standardNoiseFor(const std::string& fileName, const juce::File& dir) {
  std::string n = fileName;
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
  // Spaces, underscores and the like differ from set to set.
  std::string squashed;
  for (unsigned char c : n)
    if (std::isalnum(c)) squashed.push_back(static_cast<char>(c));
  enum class Kind { Blower, Stop, Key, None };
  const Kind kind = squashed.find("blower") != std::string::npos      ? Kind::Blower
                    : squashed.find("stopaction") != std::string::npos ? Kind::Stop
                    : squashed.find("keyaction") != std::string::npos  ? Kind::Key
                                                                       : Kind::None;
  if (kind == Kind::None) return {};
  // Each file its own noise, so the keys do not all click alike.
  uint32_t seed = 2166136261u;
  for (unsigned char c : n) seed = (seed ^ c) * 16777619u;
  const char* stem = kind == Kind::Blower ? "blower" : kind == Kind::Stop ? "stop" : "key";
  const juce::File out = dir.getChildFile(juce::String(stem) + "-" + juce::String::toHexString(static_cast<int>(seed)) + ".wav");

  // One maker at a time: the loading threads may ask for the same file.
  static std::mutex making;
  std::lock_guard<std::mutex> lock(making);
  // In UTF-8 both ways: a data folder under a user name with an accent (#377).
  if (out.existsAsFile()) return mp::pathOf(out.getFullPathName().toStdString());
  if (!dir.createDirectory()) return {};

  constexpr double rate = 48000.0;
  const bool release = n.find("release") != std::string::npos || n.find("off") != std::string::npos ||
                       n.find("disengage") != std::string::npos;
  const int frames = kind == Kind::Blower ? 48000 : kind == Kind::Stop ? 4800 : 1440;
  juce::AudioBuffer<float> b(2, frames);
  juce::Random random(static_cast<juce::int64>(seed));
  // One-pole filters over white noise: the blower a low rumble, the stop a
  // thud with a little wood in it, the key a short tick.
  double lowL = 0.0, lowR = 0.0, low2 = 0.0, prev = 0.0, brownL = 0.0, brownR = 0.0;
  const double cutLow = kind == Kind::Blower ? 0.2 : kind == Kind::Stop ? 0.04 : 0.25;
  for (int i = 0; i < frames; ++i) {
    const double t = i / rate;
    const double wl = random.nextFloat() * 2.0 - 1.0, wr = random.nextFloat() * 2.0 - 1.0;
    lowL += cutLow * (wl - lowL);
    lowR += cutLow * (wr - lowR);
    double l = lowL, r = lowR;
    if (kind == Kind::Blower) {
      // Brown noise, a leaky sum of white, with a slow wobble in its level:
      // three whole cycles in the second, so the loop's ends match.
      brownL = 0.997 * brownL + 0.04 * wl;
      brownR = 0.997 * brownR + 0.04 * wr;
      const double env = 0.85 + 0.15 * std::sin(2.0 * 3.14159265358979 * 3.0 * t);
      // And a little air: the low-passed white, far below the rumble.
      l = (0.5 * brownL + 0.05 * lowL) * env;
      r = (0.5 * brownR + 0.05 * lowR) * env;
    } else if (kind == Kind::Stop) {
      const double env = std::exp(-t / (release ? 0.012 : 0.02));
      low2 += 0.3 * (wl - low2);
      l = (2.5 * lowL + 0.25 * low2) * env;
      r = (2.5 * lowR + 0.25 * low2) * env;
    } else {
      const double env = std::exp(-t / (release ? 0.003 : 0.005));
      const double hp = wl - prev;  // a tick has no body
      prev = wl;
      l = r = 0.35 * hp * env;
    }
    b.setSample(0, i, static_cast<float>(juce::jlimit(-1.0, 1.0, l)));
    b.setSample(1, i, static_cast<float>(juce::jlimit(-1.0, 1.0, r)));
  }
  // The loop's end fades into its start, so the seam is not heard.
  if (kind == Kind::Blower) {
    const int fade = 4800;
    for (int ch = 0; ch < 2; ++ch)
      for (int i = 0; i < fade; ++i) {
        const float a = static_cast<float>(i) / fade;
        const int at = frames - fade + i;
        b.setSample(ch, i, b.getSample(ch, i) * a + b.getSample(ch, at) * (1.0f - a));
      }
    b.setSize(2, frames - fade, true);
  }
  const int written = b.getNumSamples();
  // Written beside its name and moved into place, so a file that exists is whole.
  const juce::File partial = out.withFileExtension("tmp");
  partial.deleteFile();
  {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(partial.createOutputStream());
    if (os == nullptr) return {};
    juce::StringPairArray meta;
    if (kind == Kind::Blower) {
      meta.set("NumSampleLoops", "1");
      meta.set("Loop0Start", "0");
      meta.set("Loop0End", juce::String(written - 1));
      meta.set("Loop0Type", "0");
    }
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.get(), rate, 2, 16, meta, 0));
    if (w == nullptr) return {};
    os.release();
    w->writeFromAudioSampleBuffer(b, 0, written);
  }
  if (!partial.moveFileTo(out)) {
    partial.deleteFile();
    return {};
  }
  return mp::pathOf(out.getFullPathName().toStdString());
}

} // namespace

SampleLibrary::SampleLibrary() {
  // WAV and AIFF cover ADR-003's v1 scope. WavPack (.wv) joins here at ADR-011
  // by registering its own AudioFormat.
  registerSampleFormats(formats_);
  publish(std::make_shared<const Store>()); // start with an empty generation
}

SampleLibrary::~SampleLibrary() { finishCacheWrite(true); }

void SampleLibrary::finishCacheWrite(bool abandon) {
  if (abandon) cacheStop_.store(true);
  if (cacheThread_.joinable()) cacheThread_.join();
  cacheStop_.store(false);
}

void SampleLibrary::publish(std::shared_ptr<const Store> next) {
  const Store* raw = next.get();
  {
    std::lock_guard<std::mutex> lock(publishMutex_);
    generations_.push_back(std::move(next));
  }
  // Release: everything written into the new generation happens-before any
  // audio-thread read that observes this pointer.
  live_.store(raw, std::memory_order_release);
}

void SampleLibrary::retireOldGenerations() {
  std::lock_guard<std::mutex> lock(publishMutex_);
  const Store* keep = live_.load(std::memory_order_acquire);
  generations_.erase(
      std::remove_if(generations_.begin(), generations_.end(),
                     [keep](const std::shared_ptr<const Store>& g) {
                       return g.get() != keep;
                     }),
      generations_.end());
}

bool SampleLibrary::readInto(juce::AudioFormatReader& reader, SampleBuffer& out,
                             int64_t maxFrames, LoopSelection selection,
                             SampleStorage storage, bool loadMono,
                             double targetRate, bool releaseOnly, int releaseMaxMs) {
  const int64_t total = static_cast<int64_t>(reader.lengthInSamples);
  if (total <= 0) return false;

  // The preload head is a MINIMUM, not a cap, because a head that stops short
  // of the sustain loop makes the note die — which is not "less preloaded", it
  // is broken. Real organ samples run 6-7 s with their loop ending at 3-6 s,
  // so the loop is what decides how much has to be resident. Read the loop
  // points from the metadata first (no audio is decoded to do this) and extend
  // the read to cover the loop we are actually going to use.
  int64_t want = (maxFrames > 0) ? std::min(maxFrames, total) : total;

  const int64_t loopEnd = selectLoopEnd(reader, total, selection);
  if (loopEnd > 0) {
    // A few frames past the loop end so the crossfade has material to read
    // from; without them the blend at the wrap has nothing to fade out of.
    constexpr int64_t kCrossfadeMargin = 8192;
    want = std::max(want, std::min(total, loopEnd + kCrossfadeMargin));
  }

  // A file that carries its own release after the loop has to be read whole:
  // the release is played from the marker, and truncating the read at the
  // loop would leave nothing there to play. Read from the FILE's length, not
  // from what has been decided to be resident, because that is the number the
  // marker counts in.
  const int64_t cueInFile = releaseCueInFile(reader, total, loopEnd);
  if (cueInFile > 0) want = total;

  // A shortened release: read to its length after where it starts (the file's
  // head, or its release marker), the last quarter faded to silence. An
  // attack in the same file is untouched: its loop ends before the marker.
  int64_t fadeFrom = -1;
  const double fileRate = reader.sampleRate > 0.0 ? reader.sampleRate : 48000.0;
  const auto releaseMax = static_cast<int64_t>(releaseMaxMs * fileRate / 1000.0);
  if (releaseMax > 0 && (releaseOnly || cueInFile > 0)) {
    const int64_t start = cueInFile > 0 ? cueInFile : 0;
    const int64_t end = std::min(total, start + releaseMax);
    if (end < total && end > start) {
      want = end;
      fadeFrom = std::max(start, end - std::max<int64_t>(1, releaseMax / 4));
    }
  }
  const int fileChannels = std::max(1, static_cast<int>(reader.numChannels));

  juce::AudioBuffer<float> scratch(fileChannels, static_cast<int>(want));
  if (!reader.read(&scratch, 0, static_cast<int>(want), 0, true,
                   fileChannels > 1))
    return false;
  if (fadeFrom >= 0) {
    const int64_t n = want - fadeFrom;
    for (int c = 0; c < fileChannels; ++c) {
      float* d = scratch.getWritePointer(c);
      for (int64_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i + 1) / static_cast<double>(n);
        d[fadeFrom + i] *= static_cast<float>(0.5 * (1.0 + std::cos(3.141592653589793 * x)));
      }
    }
  }

  // Fold to mono before anything else looks at the buffer, so quantising,
  // interleaving and the byte count all see one channel. The average, not the
  // sum: summing two correlated channels clips.
  if (loadMono && fileChannels > 1) {
    for (int c = 1; c < fileChannels; ++c)
      scratch.addFrom(0, 0, scratch, c, 0, static_cast<int>(want));
    scratch.applyGain(0, 0, static_cast<int>(want),
                      1.0f / static_cast<float>(fileChannels));
  }
  const int channels = (loadMono && fileChannels > 1) ? 1 : fileChannels;

  out.numChannels = channels;
  out.sampleRate = reader.sampleRate > 0.0 ? reader.sampleRate : 48000.0;
  out.numFrames = want;

  // The loop is read here, before any conversion, because the smpl chunk
  // counts frames of the FILE. Reading it afterwards would measure the
  // converted buffer against the original's numbers.
  out.loopStart = -1;
  out.loopEnd = -1;
  out.releaseCue = cueInFile;
  readLoopPoints(reader, out, selection);
  // Read from the same metadata, and deliberately NOT adjusted by the rate
  // conversion below: converting the rate resamples the audio and plays it
  // back at the new rate, so what the recording sounds at does not move.
  readFileMidiNote(reader, out);

  // Convert to the requested rate, if it is not the one the file already has.
  // Lagrange rather than linear: this runs once per sample at load time, so
  // the cost is paid where nobody is listening, and a cheap interpolator here
  // would put its error into every note for the life of the organ.
  if (targetRate > 0.0 && std::abs(targetRate - out.sampleRate) > 1.0) {
    const double ratio = out.sampleRate / targetRate;   // source frames per output frame
    const auto newFrames =
        std::max<int64_t>(1, static_cast<int64_t>(std::floor(want / ratio)));
    juce::AudioBuffer<float> converted(channels, static_cast<int>(newFrames));
    for (int c = 0; c < channels; ++c) {
      juce::LagrangeInterpolator interp;
      interp.process(ratio, scratch.getReadPointer(c),
                     converted.getWritePointer(c), static_cast<int>(newFrames));
    }
    scratch = std::move(converted);

    // A loop point that lands a frame out clicks on every wrap, so these
    // move with the audio rather than being recomputed from the metadata.
    if (out.loopStart >= 0 && out.loopEnd > out.loopStart) {
      out.loopStart = static_cast<int64_t>(std::llround(out.loopStart / ratio));
      out.loopEnd = static_cast<int64_t>(std::llround(out.loopEnd / ratio));
      if (out.loopEnd > newFrames) out.loopEnd = newFrames;
      if (out.loopStart >= out.loopEnd) { out.loopStart = -1; out.loopEnd = -1; }
    }
    // The release marker is a position in the same audio, so it moves with it.
    if (out.releaseCue > 0) {
      out.releaseCue = static_cast<int64_t>(std::llround(out.releaseCue / ratio));
      if (out.releaseCue >= newFrames) out.releaseCue = -1;
    }
    want = newFrames;
    out.numFrames = newFrames;
    out.sampleRate = targetRate;
  }

  const auto count = static_cast<size_t>(want) * static_cast<size_t>(channels);

  // Interleave: the voice engine reads frame-major, which keeps a stereo
  // voice's two channels on the same cache line.
  if (storage == SampleStorage::Int24) {
    // No peak normalisation here, on purpose. JUCE decodes a 24-bit sample by
    // dividing by 8388608, so multiplying back recovers the original integer
    // exactly; scaling by the file's peak first would round it to something
    // else for no gain. What is stored is what the file holds.
    constexpr float kFull = 8388608.0f;
    out.pcmScale = 1.0f / kFull;
    out.pcm24.resize(count);
    for (int64_t f = 0; f < want; ++f)
      for (int c = 0; c < channels; ++c) {
        const float r = std::round(scratch.getSample(c, static_cast<int>(f)) * kFull);
        const int v = static_cast<int>(
            std::isfinite(r) ? std::clamp(r, -8388608.0f, 8388607.0f) : 0.0f);
        auto& d = out.pcm24[static_cast<size_t>(f * channels + c)];
        d.b[0] = static_cast<unsigned char>(v & 0xFF);
        d.b[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        d.b[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
      }
  } else if (storage == SampleStorage::Int20) {
    // Twenty bits a sample, packed two to five bytes. Quantised against this
    // file's own peak, as 16-bit is: a 24-bit source cannot be held exactly
    // in twenty bits, so the best use of them is to spend them on the file's
    // own range rather than on the top of the scale.
    float peak = 0.0f;
    for (int c = 0; c < channels; ++c)
      peak = std::max(peak, scratch.getMagnitude(c, 0, static_cast<int>(want)));

    // 524287 rather than 524288: the negative rail is one count further out,
    // and using it would make the scale asymmetric.
    constexpr float full = 524287.0f;

    out.pcmScale = peak > 0.0f ? peak / full : 1.0f;
    const float toCounts = peak > 0.0f ? full / peak : 0.0f;
    // The buffer carries its own padding, so the four-byte read of the last
    // sample never runs past the end.
    out.pcm20.assign(pcm20Bytes(count), Pcm20{});
    for (int64_t f = 0; f < want; ++f)
      for (int c = 0; c < channels; ++c) {
        const float v = scratch.getSample(c, static_cast<int>(f)) * toCounts;
        // Round rather than truncate, and clamp: getMagnitude is the peak, so
        // the product cannot exceed full scale except by rounding, but a
        // sample set with a NaN in it must not wrap to the opposite rail.
        const float r = std::round(v);
        const float q = std::isfinite(r) ? std::clamp(r, -524288.0f, full) : 0.0f;
        pcm20Put(out.pcm20.data(), static_cast<size_t>(f * channels + c),
                 static_cast<int32_t>(q));
      }
  } else if (storage == SampleStorage::Int16) {
    // Scale by this file's own peak before quantising. Organ samples are not
    // normalised — a soft stop's samples can sit 20 dB down, and truncating
    // those straight to int16 would throw away three bits that cost nothing to
    // keep. The scale rides along on the buffer and folds into the voice gain,
    // so nothing downstream pays for it.
    float peak = 0.0f;
    for (int c = 0; c < channels; ++c)
      peak = std::max(peak, scratch.getMagnitude(c, 0, static_cast<int>(want)));

    // 32767 rather than 32768: the negative rail is one count further out,
    // and using it would make the scale asymmetric.
    constexpr float full = 32767.0f;

    out.pcmScale = peak > 0.0f ? peak / full : 1.0f;
    const float toCounts = peak > 0.0f ? full / peak : 0.0f;
    out.pcm16.resize(count);
    for (int64_t f = 0; f < want; ++f)
      for (int c = 0; c < channels; ++c) {
        const float v = scratch.getSample(c, static_cast<int>(f)) * toCounts;
        // Round rather than truncate, and clamp: getMagnitude is the peak, so
        // the product cannot exceed full scale except by rounding, but a
        // sample set with a NaN in it must not wrap to the opposite rail.
        const float r = std::round(v);
        const float q = std::isfinite(r) ? std::clamp(r, -full, full) : 0.0f;
        out.pcm16[static_cast<size_t>(f * channels + c)] =
            static_cast<int16_t>(q);
      }
  } else {
    out.pcmScale = 1.0f;
    out.frames.resize(count);
    for (int64_t f = 0; f < want; ++f)
      for (int c = 0; c < channels; ++c)
        out.frames[static_cast<size_t>(f * channels + c)] =
            scratch.getSample(c, static_cast<int>(f));
  }

  // The sustain loop was read above, before any rate conversion, and scaled
  // with the audio if there was one. Without a loop a held note plays its
  // sample once and stops, which is the difference between an instrument and
  // a demo.
  return true;
}

int64_t SampleLibrary::selectLoopEnd(const juce::AudioFormatReader& reader,
                                     int64_t totalFrames,
                                     LoopSelection selection) {
  int64_t start = -1, end = -1;
  pickLoop(reader, totalFrames, selection, start, end);
  return end;
}

// Choose one loop out of everything the smpl chunk declares, judged against
// `limitFrames` (the full file when sizing the read, the resident length when
// applying the result).
void SampleLibrary::pickLoop(const juce::AudioFormatReader& reader,
                             int64_t limitFrames, LoopSelection selection,
                             int64_t& outStart, int64_t& outEnd) {
  outStart = outEnd = -1;
  const auto& md = reader.metadataValues;
  const int declared = md.getValue("NumSampleLoops", "0").getIntValue();
  if (declared <= 0) return;

  // Every loop the file declares, before any is chosen. Two kinds are left
  // out, both from sets that play in other programs (#53):
  //  - a loop that ends past the file's LAST cue point. That cue is where the
  //    release begins; a loop reaching into the tail keeps the note sounding
  //    after the key is let go. A cue at or before the loop's start marks no
  //    release inside it: St David's rear samples put their only cue on the
  //    loop start, and dropping the loop for it made every one stop at the
  //    end of its attack, about 3.5 s in (#189).
  //  - when some loops overlap each other, one that overlaps none. A voice
  //    that enters it stays there until the key is released, never reaching
  //    the others. A file whose loops are all apart keeps them all.
  // Judged on the whole file, so the read that sizes the head and the one
  // that applies it choose from the same loops.
  struct Span { int64_t start, end; };
  std::vector<Span> loops;
  const int64_t lastCue = lastCueInFile(reader);
  const int cap = juce::jmin(declared, 64);
  for (int i = 0; i < cap; ++i) {
    const juce::String prefix = "Loop" + juce::String(i);
    const int64_t s = md.getValue(prefix + "Start", "-1").getLargeIntValue();
    const int64_t lastSample = md.getValue(prefix + "End", "-1").getLargeIntValue();
    if (s < 0 || lastSample <= s) continue;
    const int64_t e = lastSample + 1; // dwEnd is inclusive; see below
    if (lastCue > s && e > lastCue) continue;
    loops.push_back({s, e});
  }
  if (loops.size() > 1) {
    std::vector<Span> linked;
    for (size_t i = 0; i < loops.size(); ++i)
      for (size_t j = 0; j < loops.size(); ++j)
        if (i != j && loops[i].start < loops[j].end && loops[j].start < loops[i].end) {
          linked.push_back(loops[i]);
          break;
        }
    if (!linked.empty()) loops = std::move(linked);
  }

  for (const Span& loop : loops) {
    const int64_t s = loop.start;
    const int64_t e = loop.end;
    if (e > limitFrames) continue;

    if (outStart < 0) {
      outStart = s;
      outEnd = e;
      if (selection == LoopSelection::First) return;
      continue;
    }
    const bool better = (selection == LoopSelection::Conservative)
                            ? (e - s < outEnd - outStart)
                            : (e - s > outEnd - outStart);
    if (better) {
      outStart = s;
      outEnd = e;
    }
  }
}

int64_t SampleLibrary::lastCueInFile(const juce::AudioFormatReader& reader) {
  const auto& meta = reader.metadataValues;
  const int numCues = meta.getValue("NumCuePoints", "0").getIntValue();
  int64_t last = -1;
  for (int i = 0; i < numCues; ++i) {
    const juce::String key = "Cue" + juce::String(i) + "Offset";
    if (!meta.containsKey(key)) continue;
    const auto at = static_cast<int64_t>(meta.getValue(key, "0").getLargeIntValue());
    if (at > 0 && at < reader.lengthInSamples && at > last) last = at;
  }
  return last;
}

int64_t SampleLibrary::releaseCueInFile(const juce::AudioFormatReader& reader,
                                       int64_t totalFrames, int64_t loopEnd) {
  // A set may ship one recording per pipe -- attack, sustain loop and release
  // together -- and mark where the release begins with a cue point. The organ
  // definition then names that same sample as the pipe's release and says, in
  // its load-range fields, that it starts at the marker. Without this the
  // release is the whole file played again from the top: the note sounds on
  // for several more seconds at full strength, and a piece silts up.
  //
  // JUCE exposes the cue chunk as flat metadata: "NumCuePoints", then
  // "Cue<N>Offset" per cue (juce_WavAudioFormat.cpp, CueChunk::copyTo).
  //
  // The release marker is the LAST cue: files with several also mark where
  // each sustain loop starts, and taking the earliest left notes sounding
  // after the key was let go (#33, #60). It is still never inside the sustain
  // loop -- a release that started there would play the loop's tail instead --
  // so with a loop to judge against, only cues past its end count; without
  // one, the last cue in the file serves.
  const auto& meta = reader.metadataValues;
  const int numCues = meta.getValue("NumCuePoints", "0").getIntValue();
  if (numCues <= 0) return -1;

  int64_t best = -1;
  for (int i = 0; i < numCues; ++i) {
    const juce::String key = "Cue" + juce::String(i) + "Offset";
    if (!meta.containsKey(key)) continue;
    const auto at = static_cast<int64_t>(meta.getValue(key, "0").getLargeIntValue());
    if (at <= 0 || at >= totalFrames) continue;
    if (loopEnd > 0 && at < loopEnd) continue;
    if (best < 0 || at > best) best = at;
  }
  return best;
}

void SampleLibrary::readLoopPoints(const juce::AudioFormatReader& reader,
                                   SampleBuffer& out, LoopSelection selection) {
  // JUCE exposes the smpl chunk as flat metadata strings: "NumSampleLoops",
  // then "Loop<N>Start" / "Loop<N>End" per loop. (Verified against
  // juce_WavAudioFormat.cpp, SMPLChunk::copyTo — these are plain keys, not
  // named constants.) AIFF uses the same key names.
  //
  // dwEnd is the LAST SAMPLE OF THE LOOP, inclusive (RIFF spec; GrandOrgue
  // converts it the same way, `end_pos = m_EndPosition + 1`), so pickLoop adds
  // one for the half-open end SampleBuffer wants. Treating dwEnd as already
  // exclusive shortens every loop by a sample — inaudible on a long loop, a
  // click and a pitch error on a short one.
  //
  // Judged against what is RESIDENT: the read was already sized to cover the
  // chosen loop, so this normally succeeds, but a caller that forced a short
  // head still gets a correct (loop-free) buffer rather than a loop pointing
  // into audio that is not there.
  int64_t start = -1, end = -1;
  pickLoop(reader, out.numFrames, selection, start, end);
  if (start < 0) return;
  out.loopStart = start;
  out.loopEnd = end;
}

void SampleLibrary::readFileMidiNote(const juce::AudioFormatReader& reader,
                                     SampleBuffer& out) {
  // Same flat-metadata convention as the loop points: "MidiUnityNote" and
  // "MidiPitchFraction" (juce_WavAudioFormat.cpp, SMPLChunk::copyTo).
  //
  // Read with an EMPTY default and reject it, never with a numeric one. JUCE
  // has a createDefaultSMPLMetadata() carrying "MidiUnityNote" = "60", and a
  // reader that asks for a default of 60 cannot tell a file that declares
  // middle C from one that declares nothing -- which would retune an entire
  // organ to a note none of its pipes claimed.
  const auto& meta = reader.metadataValues;
  const juce::String unity = meta.getValue("MidiUnityNote", "");
  if (unity.isEmpty()) return;
  const int note = unity.getIntValue();
  if (note < 0 || note > 127) return;

  // The fraction is the pipe's own detuning and is worth keeping: across
  // Friesach's Principal 8 it runs 2 to 9 cents and differs pipe by pipe. It
  // is an unsigned 32-bit fraction OF A SEMITONE, so it is read as a 64-bit
  // value -- .getIntValue() would overflow on anything above half a semitone
  // and come back negative.
  const juce::String fracStr = meta.getValue("MidiPitchFraction", "");
  double frac = 0.0;
  if (fracStr.isNotEmpty()) {
    const double raw = static_cast<double>(fracStr.getLargeIntValue());
    if (raw > 0.0) frac = raw / 4294967296.0; // 2^32
  }
  out.fileMidiNote = static_cast<double>(note) + frac;
}

namespace {

// Channels, bits and the size of the audio, from a WAV file's headers alone:
// a few small reads per file, no decoding. False when it is not a plain WAV.
struct WavShape {
  int channels = 0;
  int bits = 0;
  double rate = 0.0;
  int64_t dataBytes = 0;
};

// A WavPack file's own account of its length, channels and rate, from the
// first block header. Recognised by its signature: the GrandOrgue demo names
// WavPack files ".wav", so the extension says nothing, and a guessed ratio
// put the demo at a sixth of what it loads (#219).
bool readWavPackShape(const std::filesystem::path& path, WavShape& out) {
  std::ifstream in(path, std::ios::binary);
  unsigned char h[32];
  if (!in || !in.read(reinterpret_cast<char*>(h), 32) || std::memcmp(h, "wvpk", 4) != 0)
    return false;
  auto u32 = [](const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
  };
  const uint32_t total = u32(h + 12);
  if (total == 0xFFFFFFFFu) return false;  // length not stated
  const int64_t frames = static_cast<int64_t>(total) | (static_cast<int64_t>(h[11]) << 32);
  const uint32_t flags = u32(h + 24);
  static const double rates[] = {6000,  8000,  9600,  11025, 12000, 16000,  22050, 24000,
                                 32000, 44100, 48000, 64000, 88200, 96000, 192000};
  const uint32_t rateIndex = (flags >> 23) & 0xF;
  out.channels = (flags & 4u) != 0 ? 1 : 2;  // MONO_FLAG
  out.bits = static_cast<int>((flags & 3u) + 1) * 8;
  out.rate = rateIndex < 15 ? rates[rateIndex] : 0.0;
  out.dataBytes = frames * out.channels * (out.bits / 8);
  return frames > 0;
}

bool readWavShape(const std::filesystem::path& path, WavShape& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char riff[12];
  if (!in.read(riff, 12) || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
    return false;
  auto u32 = [](const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  };
  for (int chunk = 0; chunk < 64; ++chunk) {
    unsigned char head[8];
    if (!in.read(reinterpret_cast<char*>(head), 8)) return false;
    const uint32_t size = u32(head + 4);
    if (std::memcmp(head, "fmt ", 4) == 0) {
      unsigned char fmt[16];
      if (size < 16 || !in.read(reinterpret_cast<char*>(fmt), 16)) return false;
      out.channels = fmt[2] | (fmt[3] << 8);
      out.rate = static_cast<double>(u32(fmt + 4));
      out.bits = fmt[14] | (fmt[15] << 8);
      in.seekg(static_cast<std::streamoff>(size - 16 + (size & 1)), std::ios::cur);
    } else if (std::memcmp(head, "data", 4) == 0) {
      out.dataBytes = size;
      return out.channels > 0 && out.bits > 0;
    } else {
      in.seekg(static_cast<std::streamoff>(size + (size & 1)), std::ios::cur);
    }
  }
  return false;
}

}  // namespace

void SampleLibrary::pruneReleases(OrganModel& model, ReleaseChoice choice) {
  if (choice == ReleaseChoice::All) return;
  for (auto& [rankId, rank] : model.ranks) {
    (void)rankId;
    for (auto& pipe : rank.pipes)
      for (auto& layer : pipe.layers) {
        auto& rel = layer.releases;
        if (choice == ReleaseChoice::None || rel.empty()) {
          rel.clear();
          continue;
        }
        // The longest is the one written for a held note, with the most of the
        // room in it; the shortest, for a short one.
        int64_t keep = rel.front().holdTimeMsHigh;
        for (const auto& r : rel)
          keep = choice == ReleaseChoice::Longest ? std::max(keep, r.holdTimeMsHigh)
                                                  : std::min(keep, r.holdTimeMsHigh);
        rel.erase(std::remove_if(rel.begin(), rel.end(),
                                 [keep](const ReleaseSample& r) { return r.holdTimeMsHigh != keep; }),
                  rel.end());
        for (auto& r : rel) r.holdTimeMsHigh = INT32_MAX;
      }
  }
}

std::vector<SampleLibrary::ShapeJob> SampleLibrary::shapeJobs(
    const OrganModel& model, const std::string& organRootDir, const OrganArchive* archive,
    const SparseSamples* wanted) const {
  // Which samples the pipework plays, and which of them only as releases --
  // the same two lists loadAll() builds, for the same reasons.
  std::unordered_set<Id> attacks, releases;
  if (wanted != nullptr) {
    attacks = wanted->attacks;
    releases = wanted->releases;
  } else {
    for (const auto& [rankId, rank] : model.ranks) {
      (void)rankId;
      for (const auto& pipe : rank.pipes)
        for (const auto& layer : pipe.layers) {
          for (const auto& a : layer.attacks)
            if (a.sample.sampleId != 0) attacks.insert(a.sample.sampleId);
          for (const auto& r : layer.releases)
            if (r.sample.sampleId != 0) releases.insert(r.sample.sampleId);
        }
    }
  }
  std::vector<ShapeJob> jobs;
  jobs.reserve(attacks.size() + releases.size());
  auto add = [&](Id id, bool releaseOnly) {
    const auto it = model.samples.find(id);
    if (it == model.samples.end()) return;
    const SampleRef& ref = it->second;
    ShapeJob job;
    job.sampleId = id;
    job.releaseOnly = releaseOnly;
    const auto path = resolvePath(rootFor(organRootDir, ref.installationPackageId), ref.fileName, ref.installationPackageId);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    job.wavpack = ext == ".wv";
    job.path = mp::utf8Of(path);
    if (archive != nullptr) {
      const auto rel = std::filesystem::path(resolvePath("", ref.fileName, ref.installationPackageId))
                           .generic_string();
      if (const auto* e = archive->find(rel)) job.packageBytes = e->size;
    }
    jobs.push_back(std::move(job));
  };
  for (Id id : attacks) add(id, false);
  for (Id id : releases)
    if (attacks.count(id) == 0) add(id, true);
  return jobs;
}

SampleLibrary::SampleShape SampleLibrary::readShape(const ShapeJob& job) {
  SampleShape shape;
  shape.releaseOnly = job.releaseOnly;
  WavShape wav;
  const std::filesystem::path path = mp::pathOf(job.path);
  std::error_code ec;
  int64_t fileBytes = job.packageBytes;
  if (std::filesystem::exists(path, ec)) {
    if (readWavPackShape(path, wav) || (!job.wavpack && readWavShape(path, wav))) {
      shape.channels = wav.channels;
      shape.rate = wav.rate;
      shape.frames = wav.dataBytes / std::max(1, wav.channels * wav.bits / 8);
      return shape;
    }
    fileBytes = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  }
  if (fileBytes <= 0) return shape;  // missing: nothing will be loaded for it
  // No header to go on: 24-bit stereo, and a WavPack file about 55% of that.
  const double pcmBytes = job.wavpack ? static_cast<double>(fileBytes) / 0.55
                                      : static_cast<double>(fileBytes);
  shape.channels = 2;
  shape.frames = static_cast<int64_t>(pcmBytes / 6.0);
  return shape;
}

int64_t SampleLibrary::residentBytes(const SampleShape& shape) const {
  if (shape.frames <= 0 || shape.channels <= 0) return 0;
  // Twenty bits is two and a half bytes a sample, so the count is not a whole
  // number of bytes.
  const double storeBytes = storage_ == SampleStorage::Int16   ? 2.0
                            : storage_ == SampleStorage::Int24 ? 3.0
                            : storage_ == SampleStorage::Int20 ? 2.5
                                                               : 4.0;
  int64_t frames = shape.frames;
  if (loadRate_ > 0.0 && shape.rate > 0.0)
    frames = static_cast<int64_t>(static_cast<double>(frames) * loadRate_ / shape.rate);
  if (shape.releaseOnly && releaseMaxMs_ > 0) {
    const double rate = loadRate_ > 0.0 ? loadRate_ : shape.rate > 0.0 ? shape.rate : 48000.0;
    frames = std::min(frames, static_cast<int64_t>(releaseMaxMs_ * rate / 1000.0));
  } else if (shape.releaseOnly && streamReleases_) {
    frames = std::min(frames, streamHeadFor(frames));
  }
  const int channels = loadMono_ ? 1 : shape.channels;
  return static_cast<int64_t>(
      std::llround(static_cast<double>(frames) * static_cast<double>(channels) * storeBytes));
}

bool SampleLibrary::rankShipped(const Rank& rank, const OrganModel& model, const std::string& organRootDir) const {
  // Held already: read at this load, or from a sample cache, which opens the
  // organ when its installation files are gone.
  const Store* held = live_.load(std::memory_order_acquire);
  bool named = false;
  for (const auto& pipe : rank.pipes)
    for (const auto& layer : pipe.layers)
      for (const auto& a : layer.attacks) {
        const auto it = model.samples.find(a.sample.sampleId);
        if (it == model.samples.end() || it->second.fileName.empty()) continue;
        const SampleRef& ref = it->second;
        named = true;
        if (held != nullptr && held->count(a.sample.sampleId) != 0) return true;
        // The format's standard components are generated when they are missing.
        if (ref.installationPackageId == 1) return true;
        std::error_code ec;
        if (std::filesystem::exists(resolveIgnoringCase(resolvePath(rootFor(organRootDir, ref.installationPackageId),
                                                                    ref.fileName, ref.installationPackageId)),
                                    ec))
          return true;
        if (archive_ != nullptr &&
            archive_->find(std::filesystem::path(resolvePath("", ref.fileName, ref.installationPackageId))
                               .generic_string()) != nullptr)
          return true;
      }
  return !named;
}

bool SampleLibrary::samplePresent(Id sampleId, const OrganModel& model,
                                   const std::string& organRootDir) const {
  const auto it = model.samples.find(sampleId);
  if (it == model.samples.end()) return false;
  const SampleRef& ref = it->second;
  if (ref.fileName.empty()) return false;
  // An encrypted file is never decoded, and a licensed one waits for the
  // player's licence. Neither is there to be read.
  if (ref.encrypted) return false;
  if (ref.licenceRequired && !licenceConfirmed_) return false;
  // Held already: read at this load, or from the sample cache, which opens the
  // organ when its installation files are gone.
  const Store* held = live_.load(std::memory_order_acquire);
  if (held != nullptr && held->count(sampleId) != 0) return true;
  // The format's standard components are generated when they are missing.
  if (ref.installationPackageId == 1) return true;
  std::error_code ec;
  if (std::filesystem::exists(resolveIgnoringCase(resolvePath(rootFor(organRootDir, ref.installationPackageId),
                                                                ref.fileName, ref.installationPackageId)),
                              ec))
    return true;
  if (archive_ != nullptr &&
      archive_->find(std::filesystem::path(resolvePath("", ref.fileName, ref.installationPackageId))
                         .generic_string()) != nullptr)
    return true;
  return false;
}

SampleLoadReport SampleLibrary::loadAll(const OrganModel& model,
                                        const std::string& organRootDir,
                                        int64_t maxFramesPerSample,
                                        LoopSelection loopSelection,
                                        LoadProgress* progress,
                                        const std::unordered_set<Id>* onlyRanks) {
  auto rankWanted = [onlyRanks](Id rankId) {
    return onlyRanks == nullptr || onlyRanks->count(rankId) != 0;
  };
  SampleLoadReport report;
  // The previous load's cache, if it is still being written, is finished
  // first: this load may be about to read it.
  finishCacheWrite(false);
  cacheRead_ = 0;
  cacheWritten_ = 0;
  cacheTotal_ = 0;
  cacheDone_ = 0;
  cacheFailure_ = CacheFailure::None;

  // A cache of a previous load of this organ, at these settings, is the whole
  // of the work below already done. Reading it is sequential; doing it again
  // is twelve thousand file opens and a decode each.
  size_t licensedLeftOut = 0;
  if (!licenceConfirmed_)
    for (const auto& [id, ref] : model.samples)
      if (ref.licenceRequired) ++licensedLeftOut;
  const std::string fingerprint =
      cacheFingerprint(onlyRanks, maxFramesPerSample, loopSelection, licensedLeftOut);
  if (cacheMode_ != CacheMode::Off && !cacheDir_.empty()) {
    auto cached = std::make_shared<Store>();
    if (readCache(*cached, fingerprint, progress) && !cached->empty()) {
      report.loaded = static_cast<int>(cached->size());
      publish(std::move(cached));
      if (progress != nullptr) {
        progress->total.store(report.loaded, std::memory_order_release);
        progress->done.store(report.loaded, std::memory_order_release);
      }
      return report;
    }
    // Over the ceiling on the cache's own size: stopped before reading it.
    if (progress != nullptr && progress->overBudget.load(std::memory_order_acquire))
      return report;
  }

  // Only what the pipework can actually play. A Sample row for a rank with no
  // pipes is unreachable, and on a demo set that is most of them.
  std::vector<Id> wanted;
  wanted.reserve(model.samples.size());
  // Which of them are RELEASES, because those are the ones worth streaming: a
  // release runs for seconds, is played once from the start, and never loops.
  // An attack cannot be treated the same way — its sustain loop has to be
  // resident or the note does not sustain, and the loop is most of the file.
  std::unordered_set<Id> releaseIds;
  {
    std::unordered_set<Id> seen;
    for (const auto& [rankId, rank] : model.ranks) {
      if (!rankWanted(rankId)) continue;
      for (const auto& pipe : rank.pipes)
        for (const auto& layer : pipe.layers) {
          for (const auto& a : layer.attacks)
            if (a.sample.sampleId != 0 && seen.insert(a.sample.sampleId).second)
              wanted.push_back(a.sample.sampleId);
          for (const auto& r : layer.releases)
            if (r.sample.sampleId != 0) {
              // A sample used as BOTH is not streamed: the attack use needs it
              // resident, and one buffer serves both.
              releaseIds.insert(r.sample.sampleId);
              if (seen.insert(r.sample.sampleId).second)
                wanted.push_back(r.sample.sampleId);
            }
        }
    }
    for (const auto& [rankId, rank] : model.ranks) {
      if (!rankWanted(rankId)) continue;
      for (const auto& pipe : rank.pipes)
        for (const auto& layer : pipe.layers)
          for (const auto& a : layer.attacks) releaseIds.erase(a.sample.sampleId);
    }
    // A model with no pipework at all (a CODM shell, or a test) still has a
    // Sample table worth honouring; falling back keeps that case working.
    // Not when the caller named ranks: a filter that matches nothing means
    // nothing, not the whole organ.
    if (wanted.empty() && onlyRanks == nullptr)
      for (const auto& [id, ref] : model.samples) {
        (void)ref;
        wanted.push_back(id);
      }
  }

  auto next = std::make_shared<Store>();
  next->reserve(wanted.size());

  // Decoding is I/O bound, and on a network or 9p-backed path overwhelmingly
  // so, which is why this uses more threads than cores by default.
  int threads = loadThreads_;
  if (threads <= 0) {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    threads = std::clamp(cores * 2, 4, 32);
  }

  std::mutex resultMutex;
  std::atomic<size_t> cursor{0};
  // Samples not on disk that the organ's archives hold: key -> sample ids.
  std::unordered_map<std::string, std::vector<Id>> fromArchive;

  report.wanted = static_cast<int>(wanted.size());
  if (progress != nullptr)
    progress->beginPhase(LoadProgress::Phase::LoadingSamples,
                         static_cast<int>(wanted.size()));

  auto worker = [&]() {
    // Each thread needs its own format manager: AudioFormatManager is not
    // documented as thread-safe for concurrent reader creation.
    juce::AudioFormatManager formats;
    registerSampleFormats(formats);

    for (;;) {
      // Checked per file rather than per batch: a file is the granularity at
      // which this work can actually stop, and on a slow disk one of them can
      // take a noticeable moment.
      if (progress != nullptr && progress->isCancelled()) return;

      const size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
      if (i >= wanted.size()) return;
      if (progress != nullptr)
        progress->done.store(static_cast<int>(i), std::memory_order_relaxed);

      const auto refIt = model.samples.find(wanted[i]);
      if (refIt == model.samples.end()) continue;
      const SampleRef& ref = refIt->second;
      if (ref.fileName.empty()) continue;

      if (ref.encrypted || isEncrypted(ref.fileName)) {
        // ADR-003: detected and reported, never decoded.
        std::lock_guard<std::mutex> lock(resultMutex);
        ++report.encrypted;
        continue;
      }
      if (ref.licenceRequired && !licenceConfirmed_) {
        // ADR-003: plain files, but the publisher asks for its licence first,
        // and only the player can say they hold it.
        std::lock_guard<std::mutex> lock(resultMutex);
        ++report.licensed;
        continue;
      }

      auto path = resolveIgnoringCase(
          resolvePath(rootFor(organRootDir, ref.installationPackageId), ref.fileName, ref.installationPackageId));
      std::error_code ec;
      // The format's own standard components (package 1) come with the program
      // that plays it and with no sample set, so a set that names them finds
      // them missing, whether the package folder is there or not. Its key,
      // stop and blower noises are made here instead.
      if (ref.installationPackageId == 1 && !std::filesystem::exists(path, ec) &&
          (archive_ == nullptr ||
           archive_->find(std::filesystem::path(resolvePath("", ref.fileName, 1)).generic_string()) == nullptr)) {
        const juce::File dir = standardNoiseDir_.empty()
                                   ? mp::dataFolder()
                                         .getChildFile("Standard noises")
                                   : juce::File(standardNoiseDir_);
        const auto made = standardNoiseFor(ref.fileName, dir);
        if (!made.empty()) {
          path = made;
          std::lock_guard<std::mutex> lock(resultMutex);
          ++report.standIns;
        }
      }
      if (!std::filesystem::exists(path, ec)) {
        std::lock_guard<std::mutex> lock(resultMutex);
        if (archive_ != nullptr) {
          const auto rel = std::filesystem::path(resolvePath("", ref.fileName, ref.installationPackageId))
                               .generic_string();
          if (archive_->find(rel) != nullptr) {
            fromArchive[OrganArchive::key(rel)].push_back(refIt->first);
            continue;
          }
        }
        ++report.missing;
        if (report.missingFiles.size() < 50)
          report.missingFiles.push_back(ref.fileName);
        continue;
      }

      std::unique_ptr<juce::AudioFormatReader> reader(
          formats.createReaderFor(juce::File(juce::String::fromUTF8(mp::utf8Of(path).c_str()))));
      auto buffer = std::make_shared<SampleBuffer>();

      // A streamed release keeps only its head resident. The head has to cover
      // the gap between the note-off and the streamer's first fill, which is
      // milliseconds — a second of it is a very large margin.
      //
      // A release that is NOT streamed is read whole, whatever the preload
      // head. The head is for attacks, whose read is extended to their loop;
      // a release in a file of its own has no loop and no marker, so the head
      // simply cut it off, and it stopped dead after the preloaded length
      // (#120: Nancy's separate release files, with "Loop + 1 s"). Streaming
      // is the way to hold less of a release.
      const bool releaseOnly = releaseIds.count(refIt->first) != 0;
      const bool stream = streamReleases_ && releaseOnly && releaseMaxMs_ == 0;
      const int64_t head = stream ? streamHeadFor(reader != nullptr ? static_cast<int64_t>(reader->lengthInSamples) : 0) : releaseOnly ? 0 : maxFramesPerSample;
      const bool ok =
          reader != nullptr &&
          readInto(*reader, *buffer, head, loopSelection, storage_, loadMono_,
                   loadRate_, releaseOnly, releaseMaxMs_);
      if (ok && stream) {
        // Both sides of this comparison have to be in the same units. The
        // file's length is in ITS frames; the resident head may have been
        // converted to another rate, so the file's length is converted too
        // before asking whether anything is left to stream.
        const double srcRate =
            reader->sampleRate > 0.0 ? reader->sampleRate : buffer->sampleRate;
        const double dstRate = buffer->sampleRate;
        const double toResident = (srcRate > 0.0) ? dstRate / srcRate : 1.0;
        const auto totalResident = static_cast<int64_t>(
            std::llround(static_cast<double>(reader->lengthInSamples) * toResident));
        if (totalResident > buffer->numFrames)
          attachTail(*buffer, mp::utf8Of(path), totalResident, srcRate, dstRate);
      }

      // Read whole: its file's cached pages go (see dropFromFileCache). A
      // streamed release keeps them; its tail is read from the file again.
      if (ok && !stream) {
        reader.reset();
        dropFromFileCache(mp::utf8Of(path));
      }

      // Against the memory ceiling, before it is kept: the sample that would
      // take the load past it is not added, and the load stops here.
      if (ok && progress != nullptr && !progress->charge(buffer->residentBytes()))
        return;

      std::lock_guard<std::mutex> lock(resultMutex);
      if (!ok) {
        ++report.failed;
        if (report.failedFiles.size() < 50)
          report.failedFiles.push_back(ref.fileName);
        continue;
      }
      (*next)[refIt->first] = std::move(buffer);
      ++report.loaded;
    }
  };

  if (threads <= 1 || wanted.size() < 32) {
    worker();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }

  // What the disk did not have, from the archives. Each archive is read once,
  // front to back, by its own thread -- RAR has no other way in -- and the
  // files it yields are decoded from memory by the usual pool. The queue
  // between them is bounded, so a fast archive cannot fill memory with bytes
  // the decoders have not reached.
  if (!fromArchive.empty() && !(progress != nullptr && progress->isCancelled())) {
    // The progress count picks up where the disk left off.
    if (progress != nullptr) {
      size_t fromArchiveCount = 0;
      for (const auto& [key, ids] : fromArchive) fromArchiveCount += ids.size();
      progress->done.store(static_cast<int>(wanted.size() - fromArchiveCount), std::memory_order_relaxed);
    }
    std::vector<std::unordered_set<std::string>> perArchive(archive_->archives().size());
    for (const auto& [key, ids] : fromArchive)
      if (const auto* e = archive_->find(key)) perArchive[e->archive].insert(key);

    struct Item {
      std::string key;
      std::shared_ptr<juce::MemoryBlock> bytes;
    };
    std::deque<Item> queue;
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    size_t queuedBytes = 0;
    int producersLeft = 0;
    bool stop = false;
    constexpr size_t kQueueCap = size_t(512) << 20;

    std::vector<std::thread> producers;
    for (size_t a = 0; a < perArchive.size(); ++a) {
      if (perArchive[a].empty()) continue;
      ++producersLeft;
      producers.emplace_back([&, a] {
        std::string error;
        archive_->read(a, perArchive[a], [&](const std::string& key, std::vector<char>&& bytes) {
          auto block = std::make_shared<juce::MemoryBlock>(bytes.data(), bytes.size());
          std::unique_lock<std::mutex> lock(queueMutex);
          queueChanged.wait(lock, [&] { return stop || queuedBytes < kQueueCap; });
          if (stop) return false;
          queuedBytes += block->getSize();
          queue.push_back({key, std::move(block)});
          queueChanged.notify_all();
          return true;
        }, error);
        if (!error.empty()) {
          // One line per file that did not read.
          std::lock_guard<std::mutex> lock(resultMutex);
          size_t from = 0;
          while (from < error.size()) {
            const size_t to = std::min(error.find('\n', from), error.size());
            ++report.failed;
            if (report.failedFiles.size() < 50) report.failedFiles.push_back(error.substr(from, to - from));
            from = to + 1;
          }
        }
        std::lock_guard<std::mutex> lock(queueMutex);
        --producersLeft;
        queueChanged.notify_all();
      });
    }

    auto decoder = [&]() {
      juce::AudioFormatManager formats;
      registerSampleFormats(formats);
      for (;;) {
        Item item;
        {
          std::unique_lock<std::mutex> lock(queueMutex);
          queueChanged.wait(lock, [&] { return stop || !queue.empty() || producersLeft == 0; });
          if (stop || (queue.empty() && producersLeft == 0)) return;
          item = std::move(queue.front());
          queue.pop_front();
          queuedBytes -= item.bytes->getSize();
          queueChanged.notify_all();
        }
        for (const Id id : fromArchive[item.key]) {
          if (progress != nullptr && progress->isCancelled()) {
            std::lock_guard<std::mutex> lock(queueMutex);
            stop = true;
            queueChanged.notify_all();
            return;
          }
          std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(
              std::make_unique<juce::MemoryInputStream>(*item.bytes, false)));
          auto buffer = std::make_shared<SampleBuffer>();
          const bool ok = reader != nullptr &&
                          readInto(*reader, *buffer, maxFramesPerSample, loopSelection, storage_,
                                   loadMono_, loadRate_, releaseIds.count(id) != 0, releaseMaxMs_);
          if (ok && progress != nullptr && !progress->charge(buffer->residentBytes())) {
            std::lock_guard<std::mutex> lock(queueMutex);
            stop = true;
            queueChanged.notify_all();
            return;
          }
          std::lock_guard<std::mutex> lock(resultMutex);
          if (progress != nullptr) progress->done.fetch_add(1, std::memory_order_relaxed);
          if (!ok) {
            ++report.failed;
            if (report.failedFiles.size() < 50) report.failedFiles.push_back(item.key);
            continue;
          }
          (*next)[id] = std::move(buffer);
          ++report.loaded;
        }
      }
    };
    std::vector<std::thread> decoders;
    for (int t = 0; t < std::max(1, threads); ++t) decoders.emplace_back(decoder);
    for (auto& t : producers) t.join();
    for (auto& t : decoders) t.join();
  }

  // Keep the result, so the next load of this organ at these settings is a
  // read. Written after publish: the organ is playable either way, and a
  // cache that fails to write is not a failed load.
  // Never a load that stopped short -- at the memory limit, or cancelled: the
  // cache is keyed to the whole organ, and the next load would take the part
  // for the whole.
  const bool stoppedShort = progress != nullptr && progress->isCancelled();
  // Nor when files failed to read: a cache of a partial organ is trusted on
  // every later load, and kept it partial after the cause was fixed.
  const bool keep = cacheMode_ != CacheMode::Off && !cacheDir_.empty() && report.loaded > 0 &&
                    !stoppedShort && report.failed == 0;
  std::shared_ptr<const Store> written = next;
  publish(std::move(next));
  if (keep) {
    // After publishing, on a thread of its own: the organ plays while the
    // disk works. The buffers are shared and no longer change, so reading
    // them here while notes play is safe.
    cacheWriting_.store(true);
    cacheThread_ = std::thread([this, written, fingerprint, path = cachePath()] {
      if (cacheWhilePlaying_) runInTheBackground();
      const auto started = std::chrono::steady_clock::now();
      const bool saved = writeCache(*written, fingerprint, path);
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      // Said when it ends: whether the next load reads the cache depends on
      // it, and a write cut short by quitting is otherwise invisible.
      std::error_code sizeError;
      const auto bytes = std::filesystem::file_size(mp::pathOf(path), sizeError);
      if (saved)
        juce::Logger::writeToLog("cache: saved " +
                                 juce::String(sizeError ? 0.0 : bytes / 1048576.0, 1) +
                                 " MB in " + juce::String(seconds, 1) + " s");
      else if (cacheStop_.load())
        juce::Logger::writeToLog("cache: write stopped at quit after " +
                                 juce::String(seconds, 1) +
                                 " s; the next load reads the samples again");
      else
        juce::Logger::writeToLog("cache: could not be written to " + juce::String(path));
      cacheWriting_.store(false);
    });
  }
  return report;
}

namespace {

// The streamed tails whose files are open, least recently read first.
//
// A tail opens its file the first time it is played and used to keep it for
// the life of the organ. Every distinct release played held one more file
// handle, and a large set has tens of thousands. macOS gives a program 256 by
// default: a few minutes' playing used them all up, and from then on every
// file the program opened failed -- the next release tails, which went silent,
// and the console's own bitmaps, which vanished from the page (#53). So the
// open ones are counted, and past a limit the one read longest ago is closed;
// it opens again the next time it is needed.
struct OpenTails {
  std::mutex mutex;
  std::list<std::function<bool()>> closers;  // each returns false if busy
  size_t limit = 0;
};

OpenTails& openTails() {
  static OpenTails t;
  return t;
}

} // namespace

size_t SampleLibrary::raiseOpenFileLimit() {
  size_t allowed = 16384;  // Windows has no small per-process cap to raise
#if !defined(_WIN32)
  rlimit lim{};
  if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {
    rlim_t want = lim.rlim_max;
#if defined(__APPLE__)
    // macOS reports an unlimited hard limit and then refuses anything above
    // OPEN_MAX for the soft one.
    if (want == RLIM_INFINITY || want > static_cast<rlim_t>(OPEN_MAX))
      want = static_cast<rlim_t>(OPEN_MAX);
#endif
    if (want > lim.rlim_cur) {
      rlimit raised = lim;
      raised.rlim_cur = want;
      if (setrlimit(RLIMIT_NOFILE, &raised) == 0) lim.rlim_cur = want;
    }
    allowed = lim.rlim_cur == RLIM_INFINITY ? 16384 : static_cast<size_t>(lim.rlim_cur);
  }
#endif
  // Half for the tails, the rest for everything else the program opens:
  // images, archives, settings, the audio device. Never so few that the
  // releases sounding at one moment keep closing each other.
  const size_t tails = std::clamp<size_t>(allowed / 2, 96, 4096);
  std::lock_guard<std::mutex> lock(openTails().mutex);
  openTails().limit = tails;
  return tails;
}

size_t SampleLibrary::openTailFiles() {
  std::lock_guard<std::mutex> lock(openTails().mutex);
  return openTails().closers.size();
}

void SampleLibrary::setOpenTailLimitForTesting(size_t limit) {
  std::lock_guard<std::mutex> lock(openTails().mutex);
  openTails().limit = limit;
}

namespace {
// Where a plain PCM WAV keeps its frames: what the streamer needs to read a
// tail's frames straight into a voice's ring. 16- and 24-bit integer only,
// the formats sample sets ship; anything else is read through JUCE.
struct WavLayout {
  int64_t dataOffset = 0;
  int64_t frames = 0;
  int channels = 0;
  int bits = 0;
};

bool readWavLayout(juce::InputStream& in, WavLayout& out) {
  char id[4];
  if (in.read(id, 4) != 4 || std::memcmp(id, "RIFF", 4) != 0) return false;
  in.readInt();
  if (in.read(id, 4) != 4 || std::memcmp(id, "WAVE", 4) != 0) return false;
  bool haveFormat = false;
  while (!in.isExhausted()) {
    if (in.read(id, 4) != 4) return false;
    const auto size = static_cast<int64_t>(static_cast<uint32_t>(in.readInt()));
    const int64_t body = in.getPosition();
    if (std::memcmp(id, "fmt ", 4) == 0) {
      const int tag = static_cast<uint16_t>(in.readShort());
      out.channels = static_cast<uint16_t>(in.readShort());
      in.readInt();    // rate
      in.readInt();    // bytes per second
      in.readShort();  // block align
      out.bits = static_cast<uint16_t>(in.readShort());
      int sub = tag;
      if (tag == 0xFFFE && size >= 40) {
        in.readShort();  // extension size
        in.readShort();  // valid bits
        in.readInt();    // channel mask
        sub = static_cast<uint16_t>(in.readShort());  // the format GUID's first two bytes
      }
      haveFormat = sub == 1 && (out.bits == 16 || out.bits == 24) && out.channels > 0;
      if (!haveFormat) return false;
    } else if (std::memcmp(id, "data", 4) == 0) {
      if (!haveFormat) return false;
      out.dataOffset = body;
      out.frames = size / (static_cast<int64_t>(out.channels) * (out.bits / 8));
      return out.frames > 0;
    }
    if (!in.setPosition(body + size + (size & 1))) return false;
  }
  return false;
}
}  // namespace

void SampleLibrary::attachTail(SampleBuffer& out, const std::string& path,
                               int64_t totalFrames, double srcRate,
                               double dstRate) const {
  auto tail = std::make_shared<SampleTail>();
  tail->totalFrames = totalFrames;
  // The head may have been folded to mono; the file on disk was not. Without
  // folding the tail the same way, a streamed release would splice from a
  // downmix straight into a bare left channel.
  const bool foldToMono = loadMono_;
  // Source frames per resident frame. Exactly 1 when no conversion happened,
  // which is the path every set took before the rate became a setting.
  const double rateRatio =
      (srcRate > 0.0 && dstRate > 0.0) ? (srcRate / dstRate) : 1.0;

  // Recorded on the buffer as well as captured below, so the cache can write
  // down what it would take to rebuild this reader.
  out.tailPath = path;
  out.tailSrcRate = srcRate;
  out.tailDstRate = dstRate;

  // The reader is opened on first use, on the streaming thread, and then kept.
  // Opening it here would mean holding a file handle per streamed sample for
  // the life of the organ, and a large set has tens of thousands.
  struct Shared {
    std::mutex mutex;
    std::unique_ptr<juce::AudioFormatManager> formats;
    std::unique_ptr<juce::AudioFormatReader> reader;
    std::string path;
    // A plain PCM WAV is read straight from the file into the ring: one
    // conversion, where JUCE's reader made two and then a planar copy.
    std::unique_ptr<juce::FileInputStream> direct;
    WavLayout layout;
    bool tried = false;
    // Its place in openTails() while the file is open.
    bool listed = false;
    std::list<std::function<bool()>>::iterator place;
    // `listed` and `place` are only touched with the list locked: a tail
    // closed to make room is taken off the list by whoever closed it.
    ~Shared() {
      std::lock_guard<std::mutex> lock(openTails().mutex);
      if (listed) openTails().closers.erase(place);
    }
  };
  auto shared = std::make_shared<Shared>();
  shared->path = path;

  tail->read = [shared, foldToMono, rateRatio](int64_t startFrame, int numFrames,
                                               float* dest, int channels) -> int64_t {
    if (dest == nullptr || numFrames <= 0 || channels <= 0) return 0;
    // One reader, one thread at a time. The streamer is single-threaded today;
    // the lock is what makes that a choice rather than an assumption.
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->reader == nullptr && shared->direct == nullptr) {
      if (shared->tried) return 0; // already failed once; do not retry per block
      // Room first, so this open is not the one that finds none.
      {
        auto& open = openTails();
        std::lock_guard<std::mutex> olock(open.mutex);
        const size_t limit = open.limit > 0 ? open.limit : 256;
        // Oldest first; one that is being read right now is passed over.
        for (auto it = open.closers.begin();
             open.closers.size() >= limit && it != open.closers.end();)
          it = (*it)() ? open.closers.erase(it) : std::next(it);
      }
      shared->tried = true;
      shared->formats = std::make_unique<juce::AudioFormatManager>();
      registerSampleFormats(*shared->formats);
      shared->reader.reset(
          shared->formats->createReaderFor(juce::File(shared->path)));
      if (shared->reader == nullptr) return 0;
      {
        auto stream = std::make_unique<juce::FileInputStream>(juce::File(shared->path));
        WavLayout layout;
        if (stream->openedOk() && readWavLayout(*stream, layout) &&
            layout.channels == static_cast<int>(shared->reader->numChannels)) {
          shared->layout = layout;
          shared->direct = std::move(stream);
          // One file open per tail: the direct stream is all a tail at the
          // organ's own rate reads, so JUCE's reader is let go.
          if (rateRatio == 1.0) {
            shared->reader.reset();
            shared->formats.reset();
          }
        }
      }
      // A plain pointer, not a shared one, so the list never keeps a tail
      // alive: ~Shared takes it off the list first.
      Shared* self = shared.get();
      auto& open = openTails();
      std::lock_guard<std::mutex> olock(open.mutex);
      shared->place = open.closers.insert(open.closers.end(), [self]() -> bool {
        // Called with the list locked. The tail's own lock is taken only if
        // free: the thread holding it may be waiting for the list.
        std::unique_lock<std::mutex> own(self->mutex, std::try_to_lock);
        if (!own.owns_lock()) return false;
        self->reader.reset();
        self->formats.reset();
        self->direct.reset();
        self->tried = false;  // open again when next played
        self->listed = false;
        return true;
      });
      shared->listed = true;
    } else {
      // Read now, so it is the last to be closed.
      auto& open = openTails();
      std::lock_guard<std::mutex> olock(open.mutex);
      if (shared->listed)
        open.closers.splice(open.closers.end(), open.closers, shared->place);
    }

    // ---- converted tail -------------------------------------------------
    // Output frames are counted at the resident rate, so each one is placed
    // back into the file and interpolated there. Catmull-Rom needs one frame
    // before and two after the position, which is why the read is widened.
    if (rateRatio != 1.0) {
      const int fCh = std::max(1, static_cast<int>(shared->reader->numChannels));
      const int useCh = (foldToMono && channels == 1)
                            ? fCh
                            : std::min(channels, fCh);
      const double firstPos = static_cast<double>(startFrame) * rateRatio;
      const double lastPos =
          static_cast<double>(startFrame + numFrames - 1) * rateRatio;
      const int64_t srcFrom = static_cast<int64_t>(std::floor(firstPos)) - 1;
      const int64_t srcTo = static_cast<int64_t>(std::ceil(lastPos)) + 2;
      const auto srcCount = static_cast<int>(srcTo - srcFrom + 1);
      if (srcCount <= 0) return 0;

      // Kept for the thread that streams, from one read to the next: made new
      // for every read it was up to 128 KB, which the system hands out as
      // fresh pages each time.
      static thread_local juce::AudioBuffer<float> src;
      src.setSize(std::max(1, useCh), srcCount, false, false, true);
      src.clear();
      // A block at the very start of the file asks for a frame before it;
      // that gap stays zero rather than reading off the front.
      const int64_t readFrom = std::max<int64_t>(0, srcFrom);
      const int skip = static_cast<int>(readFrom - srcFrom);
      if (skip < srcCount)
        shared->reader->read(&src, skip, srcCount - skip, readFrom, true,
                             useCh > 1);

      if (foldToMono && channels == 1 && useCh > 1) {
        for (int c = 1; c < useCh; ++c)
          src.addFrom(0, 0, src, c, 0, srcCount);
        src.applyGain(0, 0, srcCount, 1.0f / static_cast<float>(useCh));
      }

      const auto at = [srcCount](int c, int64_t i) {
        const auto k = static_cast<int>(std::clamp<int64_t>(i, 0, srcCount - 1));
        return src.getSample(c, k);
      };
      for (int f = 0; f < numFrames; ++f) {
        const double pos =
            static_cast<double>(startFrame + f) * rateRatio - static_cast<double>(srcFrom);
        const auto i1 = static_cast<int64_t>(std::floor(pos));
        const auto t = static_cast<float>(pos - static_cast<double>(i1));
        for (int c = 0; c < channels; ++c) {
          const int sc = std::min(c, useCh - 1);
          const float p0 = at(sc, i1 - 1), p1 = at(sc, i1);
          const float p2 = at(sc, i1 + 1), p3 = at(sc, i1 + 2);
          const float a = 0.5f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3);
          const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
          const float cc = 0.5f * (-p0 + p2);
          dest[static_cast<size_t>(f) * static_cast<size_t>(channels) +
               static_cast<size_t>(c)] = ((a * t + b) * t + cc) * t + p1;
        }
      }
      return numFrames;
    }

    if (shared->direct != nullptr) {
      // Straight from the file: frames [startFrame, startFrame + numFrames),
      // each count divided by its full scale. JUCE reaches the same value
      // exactly: it shifts a count up to 32 bits and scales by 2^-31.
      const WavLayout& w = shared->layout;
      const int bytes = w.bits / 8;
      const int64_t bpf = static_cast<int64_t>(w.channels) * bytes;
      const int64_t avail = std::max<int64_t>(0, std::min<int64_t>(numFrames, w.frames - startFrame));
      static thread_local std::vector<uint8_t> raw;
      raw.resize(static_cast<size_t>(avail * bpf));
      int64_t got = 0;
      if (avail > 0 && shared->direct->setPosition(w.dataOffset + startFrame * bpf))
        got = std::max(0, shared->direct->read(raw.data(), static_cast<int>(avail * bpf))) / bpf;
      const float scale = w.bits == 16 ? 1.0f / 32768.0f : 1.0f / 8388608.0f;
      auto sampleAt = [&](int64_t f, int c) -> float {
        const uint8_t* q = raw.data() + f * bpf + c * bytes;
        if (bytes == 2) return static_cast<float>(static_cast<int16_t>(q[0] | (q[1] << 8))) * scale;
        const int32_t v = (static_cast<int32_t>(static_cast<uint32_t>(q[0]) << 8 |
                                                static_cast<uint32_t>(q[1]) << 16 |
                                                static_cast<uint32_t>(q[2]) << 24)) >> 8;
        return static_cast<float>(v) * scale;
      };
      const bool fold = foldToMono && channels == 1 && w.channels > 1;
      const float foldGain = 1.0f / static_cast<float>(w.channels);
      for (int64_t f = 0; f < numFrames; ++f) {
        float* to = dest + static_cast<size_t>(f) * static_cast<size_t>(channels);
        if (f >= got) {
          for (int c = 0; c < channels; ++c) to[c] = 0.0f;
          continue;
        }
        if (fold) {
          // As JUCE's path folds: the channels added in order, then scaled.
          float sum = sampleAt(f, 0);
          for (int c = 1; c < w.channels; ++c) sum += sampleAt(f, c);
          to[0] = sum * foldGain;
        } else {
          for (int c = 0; c < channels; ++c) to[c] = sampleAt(f, std::min(c, w.channels - 1));
        }
      }
      return numFrames;
    }

    const int fileCh = std::max(1, static_cast<int>(shared->reader->numChannels));
    // When folding, every channel of the file is needed to make the average,
    // even though only one comes out.
    const int ch = (foldToMono && channels == 1)
                       ? fileCh
                       : std::min(channels, fileCh);
    // See above: one for the thread, kept between reads.
    static thread_local juce::AudioBuffer<float> scratch;
    scratch.setSize(std::max(1, ch), numFrames, false, false, true);
    if (!shared->reader->read(&scratch, 0, numFrames, startFrame, true, ch > 1))
      return 0;

    if (foldToMono && channels == 1 && ch > 1) {
      for (int c = 1; c < ch; ++c) scratch.addFrom(0, 0, scratch, c, 0, numFrames);
      scratch.applyGain(0, 0, numFrames, 1.0f / static_cast<float>(ch));
    }

    // Interleave into the ring, the same layout the resident head uses: a
    // channel at a time, through plain pointers.
    for (int c = 0; c < channels; ++c) {
      const float* from = scratch.getReadPointer(std::min(c, ch - 1));
      float* to = dest + c;
      for (int f = 0; f < numFrames; ++f) to[static_cast<size_t>(f) * static_cast<size_t>(channels)] = from[f];
    }
    return numFrames;
  };

  out.tail = std::move(tail);
}

// ---------------------------------------------------------------- cache
//
// Format, little-endian throughout:
//
//   "MPSC", version, fingerprint, sample count
//   per sample: id, frames, channels, rate, loop points, release marker,
//               scale, width, payload, and — when it streams — what the tail
//               needs to be re-attached without opening the file now.
//
// Everything is fixed width or length-prefixed, so a truncated file fails at
// the length check rather than halfway through a buffer.
namespace {

constexpr char kCacheMagic[4] = {'M', 'P', 'S', 'C'};
// 2: the per-sample record carries the release marker of a file that holds
//    attack, loop and release together.
// The 20-bit width (code 3) needs no new version: a cache holds it only under a
// fingerprint with that width in it, which no earlier build can ask for, so
// the caches players already have stay valid.
constexpr uint32_t kCacheVersion = 5; // 5 adds a checksum per sample; 4 fixes release cues; 3 added file pitch

// A checksum of one sample's audio in the cache, so audio damaged on the
// disk is found as it is read back: a flipped bit there plays as a click or
// a burst of noise, and nothing else would notice. Eight bytes at a time,
// well ahead of any disk.
uint64_t cacheChecksum(const char* data, uint64_t bytes) {
  uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
  uint64_t i = 0;
  for (; i + 8 <= bytes; i += 8) {
    uint64_t w;
    std::memcpy(&w, data + i, 8);
    h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
  }
  for (; i < bytes; ++i) h = (h ^ static_cast<unsigned char>(data[i])) * 0x100000001B3ull;
  return h ^ (h >> 32);
}

template <typename T>
void putPod(std::ostream& os, const T& v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
bool getPod(std::istream& is, T& v) {
  is.read(reinterpret_cast<char*>(&v), sizeof(T));
  return static_cast<bool>(is);
}
void putStr(std::ostream& os, const std::string& s) {
  putPod<uint32_t>(os, static_cast<uint32_t>(s.size()));
  os.write(s.data(), static_cast<std::streamsize>(s.size()));
}
bool getStr(std::istream& is, std::string& s, uint32_t cap = 64u * 1024u) {
  uint32_t n = 0;
  if (!getPod(is, n) || n > cap) return false;
  s.assign(n, '\0');
  if (n) is.read(s.data(), n);
  return static_cast<bool>(is);
}

// 0 float32, 1 int24, 2 int16, 3 int20 — written rather than the enum's
// ordinal, so reordering SampleStorage cannot silently reinterpret an old
// cache.
uint8_t widthCode(const mp::SampleBuffer& b) {
  if (!b.pcm24.empty()) return 1;
  if (!b.pcm16.empty()) return 2;
  if (!b.pcm20.empty()) return 3;
  return 0;
}

} // namespace

std::string SampleLibrary::cacheFingerprint(const std::unordered_set<Id>* onlyRanks,
                                            int64_t maxFramesPerSample,
                                            LoopSelection loopSelection,
                                            size_t licensedLeftOut) const {
  std::string s = "v1|" + cacheOdfStamp_;
  // The resident width, by the enum's ordinal: the widths are distinct
  // ordinals, and a new one goes at the end of the enum, so a cache written
  // at one width is never read back at another.
  s += "|w" + std::to_string(static_cast<int>(storage_));
  s += "|m" + std::to_string(loadMono_ ? 1 : 0);
  s += "|r" + std::to_string(static_cast<int64_t>(loadRate_));
  s += "|h" + std::to_string(maxFramesPerSample);
  s += "|l" + std::to_string(static_cast<int>(loopSelection));
  s += "|s" + std::to_string(streamReleases_ ? 1 : 0);
  s += "|t" + std::to_string(streamHead_);
  s += "|tp" + std::to_string(streamHeadPercent_);
  s += "|rc" + std::to_string(static_cast<int>(releaseChoice_));
  s += "|rl" + std::to_string(releaseMaxMs_);
  // How many notes are loaded. A sparse load reads fewer samples than a full
  // one, so the two caches hold different things and must never be mistaken
  // for each other.
  s += "|sp" + std::to_string(sparseEvery_);
  // Without the licensed samples a cache is not the organ with them.
  s += "|c" + std::to_string(licenceConfirmed_ ? 1 : 0);
  // How many samples wait for the licence. A cache written when the licence
  // number was read as yes or no held St. Maximin's attacks without one; this
  // retires it, and only on organs that ask for a licence.
  if (licensedLeftOut > 0) s += "|lo" + std::to_string(licensedLeftOut);
  // Caches written before a read failure stopped being fatal may hold only
  // part of an organ (Buckeburg: 5639 of its samples); this retires them.
  s += "|r2";
  // The packages a composite found with other organs (#289): a cache written
  // before they were found held only the composite's own samples, and came
  // back after they were (Zamardi-Buda: 263 of 5228). Only when there are
  // any, so every other organ keeps the cache it has.
  for (const auto& package : packagesFromElsewhere_) s += "|k" + package;
  // A partial load holds a different set of samples from a full one, and the
  // two must never be mistaken for each other: a cache written by
  // --preload-drawn would otherwise come back as a whole organ with most of
  // its ranks silent.
  if (onlyRanks != nullptr) {
    std::vector<Id> ids(onlyRanks->begin(), onlyRanks->end());
    std::sort(ids.begin(), ids.end());
    uint64_t h = 1469598103934665603ull;
    for (Id id : ids) {
      h ^= static_cast<uint64_t>(id);
      h *= 1099511628211ull;
    }
    s += "|p" + std::to_string(ids.size()) + "-" + std::to_string(h);
  } else {
    s += "|pall";
  }
  return s;
}

std::string SampleLibrary::cachePath() const {
  if (cacheDir_.empty() || cacheMode_ == CacheMode::Off) return {};
  std::filesystem::path dir = mp::pathOf(cacheDir_);
  // One file, replaced as organs change, unless asked for one per organ.
  const std::string name = (cacheMode_ == CacheMode::PerOrgan && !cacheOrganId_.empty())
                               ? ("samples-" + cacheOrganId_ + ".mpcache")
                               : std::string("samples.mpcache");
  return mp::utf8Of(dir / name);
}

bool SampleLibrary::writeCache(const Store& store, const std::string& fingerprint,
                               const std::string& path) const {
  if (path.empty()) return false;

  std::error_code ec;
  std::filesystem::create_directories(mp::pathOf(path).parent_path(), ec);

  // Written beside and moved into place, so an interrupted write leaves the
  // old cache rather than a half one that passes its own length checks.
  const std::string tmp = path + ".part";
  // A write that fails, on a full disk most often, takes its part file with
  // it: left behind, it kept the disk full and the settings unsaved (#519).
  // The space is read while the part file still holds it: once it is
  // removed, the disk that just filled up shows the room the cache took.
  const auto failed = [&] {
    std::error_code spaceError;
    const auto space = std::filesystem::space(mp::pathOf(path).parent_path(), spaceError);
    std::filesystem::remove(mp::pathOf(tmp), ec);
    if (cacheStop_.load()) return false;
    cacheFailure_ = !spaceError && space.available < (uint64_t{64} << 20) ? CacheFailure::DiskFull
                                                                        : CacheFailure::Failed;
    return false;
  };
  {
    std::ofstream os(mp::pathOf(tmp), std::ios::binary | std::ios::trunc);
    if (!os) return failed();
    os.write(kCacheMagic, 4);
    putPod(os, kCacheVersion);
    putStr(os, fingerprint);
    putPod<uint64_t>(os, static_cast<uint64_t>(store.size()));

    // How much there is to write, so the status line can say how far along
    // it is and how long is left: on a large organ this takes minutes.
    int64_t total = 0;
    for (const auto& [id, buf] : store) {
      (void)id;
      if (buf == nullptr) continue;
      total += static_cast<int64_t>(buf->pcm24.size() * sizeof(Pcm24) +
                                    buf->pcm16.size() * sizeof(int16_t) +
                                    buf->pcm20.size() * sizeof(Pcm20) +
                                    buf->frames.size() * sizeof(float));
    }
    cacheTotal_ = total;
    int64_t done = 0;

    for (const auto& [id, buf] : store) {
      if (buf == nullptr) continue;
      // While the organ plays, the write waits for silence: it is not let near
      // the sound at all.
      while (cacheWhilePlaying_ && cacheBusy_ && cacheBusy_() && !cacheStop_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (cacheStop_.load()) break;
      putPod<uint32_t>(os, static_cast<uint32_t>(id));
      putPod<int64_t>(os, buf->numFrames);
      putPod<int32_t>(os, buf->numChannels);
      putPod<double>(os, buf->sampleRate);
      putPod<int64_t>(os, buf->loopStart);
      putPod<int64_t>(os, buf->loopEnd);
      putPod<int64_t>(os, buf->releaseCue);
      putPod<double>(os, buf->fileMidiNote);
      putPod<float>(os, buf->pcmScale);
      putPod<uint8_t>(os, widthCode(*buf));

      const char* data = nullptr;
      uint64_t bytes = 0;
      if (!buf->pcm24.empty()) {
        data = reinterpret_cast<const char*>(buf->pcm24.data());
        bytes = buf->pcm24.size() * sizeof(Pcm24);
      } else if (!buf->pcm16.empty()) {
        data = reinterpret_cast<const char*>(buf->pcm16.data());
        bytes = buf->pcm16.size() * sizeof(int16_t);
      } else if (!buf->pcm20.empty()) {
        data = reinterpret_cast<const char*>(buf->pcm20.data());
        bytes = buf->pcm20.size() * sizeof(Pcm20);
      } else {
        data = reinterpret_cast<const char*>(buf->frames.data());
        bytes = buf->frames.size() * sizeof(float);
      }
      putPod<uint64_t>(os, bytes);
      if (bytes) os.write(data, static_cast<std::streamsize>(bytes));
      putPod<uint64_t>(os, cacheChecksum(data, bytes));
      done += static_cast<int64_t>(bytes);
      cacheDone_ = done;

      // A streamed sample keeps only its head here. What the tail needs to be
      // rebuilt is recorded so the file is not opened until a note asks.
      const bool streams = buf->streams();
      putPod<uint8_t>(os, streams ? 1u : 0u);
      if (streams) {
        putPod<int64_t>(os, buf->tail->totalFrames);
        putStr(os, buf->tailPath);
        putPod<double>(os, buf->tailSrcRate);
        putPod<double>(os, buf->tailDstRate);
      }
      if (!os) break;
    }
    // The last bytes leave the stream's buffer here: a disk that fills on
    // them fails the write as surely as one that fills halfway.
    bool written = !cacheStop_.load() && static_cast<bool>(os);
    if (written) {
      cacheWritten_ = static_cast<int64_t>(os.tellp());
      os.flush();
      written = static_cast<bool>(os);
    }
    os.close();
    if (!written || os.fail()) return failed();
  }

  std::filesystem::rename(mp::pathOf(tmp), mp::pathOf(path), ec);
  if (ec) return failed();
  return true;
}

bool SampleLibrary::readCache(Store& out, const std::string& fingerprint,
                              LoadProgress* progress) const {
  const std::string path = cachePath();
  if (path.empty()) return false;
  std::ifstream is(mp::pathOf(path), std::ios::binary);
  if (!is) return false;

  char magic[4] = {};
  is.read(magic, 4);
  if (!is || std::memcmp(magic, kCacheMagic, 4) != 0) return false;
  uint32_t version = 0;
  if (!getPod(is, version) || version != kCacheVersion) return false;
  std::string got;
  if (!getStr(is, got) || got != fingerprint) return false;

  // This cache is this load's, and it is the organ's resident audio written
  // out, so its size is what reading it back would cost. Past the memory
  // ceiling, stop before reading any of it.
  std::error_code sizeError;
  const auto cacheBytes = std::filesystem::file_size(mp::pathOf(path), sizeError);
  if (!sizeError && progress != nullptr &&
      !progress->charge(static_cast<int64_t>(cacheBytes)))
    return false;

  uint64_t count = 0;
  if (!getPod(is, count) || count > 4000000ull) return false;

  Store loaded;
  loaded.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t id = 0;
    int32_t channels = 0;
    uint8_t width = 0, streams = 0;
    uint64_t bytes = 0;
    auto buf = std::make_shared<SampleBuffer>();
    if (!getPod(is, id) || !getPod(is, buf->numFrames) || !getPod(is, channels) ||
        !getPod(is, buf->sampleRate) || !getPod(is, buf->loopStart) ||
        !getPod(is, buf->loopEnd) || !getPod(is, buf->releaseCue) ||
        !getPod(is, buf->fileMidiNote) || !getPod(is, buf->pcmScale) ||
        !getPod(is, width) || !getPod(is, bytes))
      return false;
    buf->numChannels = channels;
    if (bytes > (uint64_t)1 << 34) return false;   // 16 GB for one sample: corrupt

    if (width == 1) {
      buf->pcm24.resize(static_cast<size_t>(bytes / sizeof(Pcm24)));
      if (bytes) is.read(reinterpret_cast<char*>(buf->pcm24.data()),
                         static_cast<std::streamsize>(bytes));
    } else if (width == 2) {
      buf->pcm16.resize(static_cast<size_t>(bytes / sizeof(int16_t)));
      if (bytes) is.read(reinterpret_cast<char*>(buf->pcm16.data()),
                         static_cast<std::streamsize>(bytes));
    } else if (width == 3) {
      // The twenty-bit buffer carries its own padding, so its size follows
      // from the frame count: a file that says otherwise is damaged.
      const auto want = static_cast<uint64_t>(pcm20Bytes(
          static_cast<size_t>(std::max<int64_t>(0, buf->numFrames)) *
          static_cast<size_t>(std::max(1, channels))));
      if (bytes != want) return false;
      buf->pcm20.resize(static_cast<size_t>(bytes));
      if (bytes) is.read(reinterpret_cast<char*>(buf->pcm20.data()),
                         static_cast<std::streamsize>(bytes));
    } else {
      buf->frames.resize(static_cast<size_t>(bytes / sizeof(float)));
      if (bytes) is.read(reinterpret_cast<char*>(buf->frames.data()),
                         static_cast<std::streamsize>(bytes));
    }
    if (!is) return false;
    {
      const char* got = !buf->pcm24.empty()   ? reinterpret_cast<const char*>(buf->pcm24.data())
                        : !buf->pcm16.empty() ? reinterpret_cast<const char*>(buf->pcm16.data())
                        : !buf->pcm20.empty() ? reinterpret_cast<const char*>(buf->pcm20.data())
                                              : reinterpret_cast<const char*>(buf->frames.data());
      uint64_t stored = 0;
      if (!getPod(is, stored)) return false;
      if (stored != cacheChecksum(got, bytes)) {
        juce::Logger::writeToLog("cache: sample " + juce::String(static_cast<int>(id)) +
                                 " is damaged on the disk; the samples are read instead "
                                 "and the cache written again");
        return false;
      }
    }

    if (!getPod(is, streams)) return false;
    if (streams) {
      int64_t total = 0;
      std::string tp;
      double sr = 0.0, dr = 0.0;
      if (!getPod(is, total) || !getStr(is, tp, 4096u) || !getPod(is, sr) ||
          !getPod(is, dr))
        return false;
      // Rebuilt rather than stored: the tail is a reader and a lambda, and
      // the only durable parts of it are the path and the two rates.
      attachTail(*buf, tp, total, sr, dr);
    }
    loaded.emplace(static_cast<Id>(id), std::move(buf));
  }
  cacheRead_ = static_cast<int64_t>(is.tellg());
  out = std::move(loaded);
  is.close();
  dropFromFileCache(path);
  return true;
}

size_t SampleLibrary::streamedCount() const {
  const Store* store = live_.load(std::memory_order_acquire);
  if (store == nullptr) return 0;
  size_t n = 0;
  for (const auto& [id, buf] : *store) {
    (void)id;
    if (buf->streams()) ++n;
  }
  return n;
}

int64_t SampleLibrary::streamedBytesSaved() const {
  const Store* store = live_.load(std::memory_order_acquire);
  if (store == nullptr) return 0;
  int64_t saved = 0;
  for (const auto& [id, buf] : *store) {
    (void)id;
    if (!buf->streams()) continue;
    const int64_t skipped = buf->totalFrames() - buf->numFrames;
    // Not compact() ? 2 : 4 -- compact() is true of 16-bit AND 24-bit, so
    // that understated every 24-bit figure by a third. Twenty bits is two
    // and a half bytes a sample.
    const double perFrame =
        static_cast<double>(buf->numChannels) * buf->bytesPerSampleExact();
    saved += static_cast<int64_t>(std::llround(static_cast<double>(skipped) * perFrame));
  }
  return saved;
}

SampleProvider SampleLibrary::provider() const {
  // One acquire load and a hash lookup — no lock, no refcount, no allocation.
  // Capturing `this` is safe because the library outlives the engine it feeds.
  return [this](Id sampleId) -> const SampleBuffer* {
    const Store* store = live_.load(std::memory_order_acquire);
    if (store == nullptr) return nullptr;
    const auto it = store->find(sampleId);
    return it == store->end() ? nullptr : it->second.get();
  };
}

size_t SampleLibrary::residentCount() const {
  const Store* store = live_.load(std::memory_order_acquire);
  return store == nullptr ? 0 : store->size();
}

int64_t SampleLibrary::residentBytes() const {
  const Store* store = live_.load(std::memory_order_acquire);
  if (store == nullptr) return 0;
  int64_t bytes = 0;
  for (const auto& [id, buf] : *store) {
    (void)id;
    bytes += buf->residentBytes();
  }
  return bytes;
}

void SampleLibrary::clear() {
  publish(std::make_shared<const Store>());
  retireOldGenerations();
}

} // namespace mp
