#include "SampleLibrary.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <filesystem>
#include <thread>
#include <unordered_set>
#include <vector>

namespace mp {
namespace {

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

  std::filesystem::path base(rootDir);
  if (installationPackageId > 0) {
    std::string digits = std::to_string(installationPackageId);
    if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
    base /= "OrganInstallationPackages";
    base /= digits;
  }
  return base / rel;
}

// Windows-authored sets record whatever case the author had; the disk on Linux
// does not care what they meant. Fall back to a case-insensitive walk of the
// parent directory rather than declaring a present file missing.
std::filesystem::path resolveIgnoringCase(const std::filesystem::path& wanted) {
  std::error_code ec;
  if (std::filesystem::exists(wanted, ec)) return wanted;

  const auto dir = wanted.parent_path();
  if (!std::filesystem::is_directory(dir, ec)) return wanted;

  std::string target = wanted.filename().string();
  std::transform(target.begin(), target.end(), target.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    std::string have = entry.path().filename().string();
    std::transform(have.begin(), have.end(), have.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (have == target) return entry.path();
  }
  return wanted;
}

} // namespace

SampleLibrary::SampleLibrary() {
  // WAV and AIFF cover ADR-003's v1 scope. WavPack (.wv) joins here at ADR-011
  // by registering its own AudioFormat.
  formats_.registerBasicFormats();
  publish(std::make_shared<const Store>()); // start with an empty generation
}

SampleLibrary::~SampleLibrary() = default;

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
                             double targetRate) {
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
  const int fileChannels = std::max(1, static_cast<int>(reader.numChannels));

  juce::AudioBuffer<float> scratch(fileChannels, static_cast<int>(want));
  if (!reader.read(&scratch, 0, static_cast<int>(want), 0, true,
                   fileChannels > 1))
    return false;

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
  readLoopPoints(reader, out, selection);

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

  const int cap = juce::jmin(declared, 64);
  for (int i = 0; i < cap; ++i) {
    const juce::String prefix = "Loop" + juce::String(i);
    const int64_t s = md.getValue(prefix + "Start", "-1").getLargeIntValue();
    const int64_t lastSample = md.getValue(prefix + "End", "-1").getLargeIntValue();
    if (s < 0 || lastSample <= s) continue;
    const int64_t e = lastSample + 1; // dwEnd is inclusive; see below
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
    if (wanted.empty())
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

  if (progress != nullptr)
    progress->beginPhase(LoadProgress::Phase::LoadingSamples,
                         static_cast<int>(wanted.size()));

  auto worker = [&]() {
    // Each thread needs its own format manager: AudioFormatManager is not
    // documented as thread-safe for concurrent reader creation.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

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

      const auto path = resolveIgnoringCase(
          resolvePath(organRootDir, ref.fileName, ref.installationPackageId));
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        std::lock_guard<std::mutex> lock(resultMutex);
        ++report.missing;
        if (report.missingFiles.size() < 50)
          report.missingFiles.push_back(ref.fileName);
        continue;
      }

      std::unique_ptr<juce::AudioFormatReader> reader(
          formats.createReaderFor(juce::File(path.string())));
      auto buffer = std::make_shared<SampleBuffer>();

      // A streamed release keeps only its head resident. The head has to cover
      // the gap between the note-off and the streamer's first fill, which is
      // milliseconds — a second of it is a very large margin.
      const bool stream = streamReleases_ && releaseIds.count(refIt->first) != 0;
      const int64_t head = stream ? streamHead_ : maxFramesPerSample;
      const bool ok =
          reader != nullptr &&
          readInto(*reader, *buffer, head, loopSelection, storage_, loadMono_,
                   loadRate_);
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
          attachTail(*buffer, path.string(), totalResident, srcRate, dstRate);
      }

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

  publish(std::move(next));
  return report;
}

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

  // The reader is opened on first use, on the streaming thread, and then kept.
  // Opening it here would mean holding a file handle per streamed sample for
  // the life of the organ, and a large set has tens of thousands.
  struct Shared {
    std::mutex mutex;
    std::unique_ptr<juce::AudioFormatManager> formats;
    std::unique_ptr<juce::AudioFormatReader> reader;
    std::string path;
    bool tried = false;
  };
  auto shared = std::make_shared<Shared>();
  shared->path = path;

  tail->read = [shared, foldToMono, rateRatio](int64_t startFrame, int numFrames,
                                               float* dest, int channels) -> int64_t {
    if (dest == nullptr || numFrames <= 0 || channels <= 0) return 0;
    // One reader, one thread at a time. The streamer is single-threaded today;
    // the lock is what makes that a choice rather than an assumption.
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->reader == nullptr) {
      if (shared->tried) return 0; // already failed once; do not retry per block
      shared->tried = true;
      shared->formats = std::make_unique<juce::AudioFormatManager>();
      shared->formats->registerBasicFormats();
      shared->reader.reset(
          shared->formats->createReaderFor(juce::File(shared->path)));
      if (shared->reader == nullptr) return 0;
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

      juce::AudioBuffer<float> src(std::max(1, useCh), srcCount);
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

      const auto at = [&src, srcCount](int c, int64_t i) {
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

    const int fileCh = std::max(1, static_cast<int>(shared->reader->numChannels));
    // When folding, every channel of the file is needed to make the average,
    // even though only one comes out.
    const int ch = (foldToMono && channels == 1)
                       ? fileCh
                       : std::min(channels, fileCh);
    juce::AudioBuffer<float> scratch(std::max(1, ch), numFrames);
    if (!shared->reader->read(&scratch, 0, numFrames, startFrame, true, ch > 1))
      return 0;

    if (foldToMono && channels == 1 && ch > 1) {
      for (int c = 1; c < ch; ++c) scratch.addFrom(0, 0, scratch, c, 0, numFrames);
      scratch.applyGain(0, 0, numFrames, 1.0f / static_cast<float>(ch));
    }

    // Interleave into the ring, the same layout the resident head uses.
    for (int f = 0; f < numFrames; ++f)
      for (int c = 0; c < channels; ++c)
        dest[static_cast<size_t>(f) * static_cast<size_t>(channels) +
             static_cast<size_t>(c)] = scratch.getSample(std::min(c, ch - 1), f);
    return numFrames;
  };

  out.tail = std::move(tail);
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
    const int64_t perFrame = static_cast<int64_t>(buf->numChannels) *
                             (buf->compact() ? 2 : 4);
    saved += skipped * perFrame;
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
