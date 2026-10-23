#include "MidiArchive.h"
#include "DataFolder.h"

#include <algorithm>

namespace mp {

namespace {

// A few minutes of playing before the list grows on the audio thread.
constexpr size_t kReserve = 32768;

struct Listed {
  juce::int64 writtenMs = 0;
  juce::String path;  // relative to the archive folder
};

std::vector<Listed> readIndex(const juce::File& folder) {
  std::vector<Listed> out;
  juce::StringArray lines;
  folder.getChildFile(MidiArchive::kIndexName).readLines(lines);
  for (const auto& line : lines) {
    const int tab = line.indexOfChar('\t');
    if (tab <= 0) continue;
    out.push_back({line.substring(0, tab).getLargeIntValue(), line.substring(tab + 1)});
  }
  return out;
}

void writeIndex(const juce::File& folder, const std::vector<Listed>& listed) {
  juce::String text;
  for (const auto& l : listed) text << juce::String(l.writtenMs) << "\t" << l.path << "\n";
  mp::writeTextFile(folder.getChildFile(MidiArchive::kIndexName), text);
}

}  // namespace

void MidiArchive::prepare(double sampleRate) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  const juce::SpinLock::ScopedLockType hold(lock_);
  events_.reserve(kReserve);
  spare_.reserve(kReserve);
}

void MidiArchive::capture(const juce::MidiMessage& message, int deviceId) {
  // A console's clock and active sensing never fall silent, and carry nothing
  // played.
  if (message.isMidiClock() || message.isActiveSense()) return;
  const juce::SpinLock::ScopedTryLockType hold(lock_);
  if (!hold.isLocked()) return;  // the message thread is taking the take: a silence
  const int64_t pos = pos_.load(std::memory_order_relaxed);
  if (events_.empty()) {
    if (!message.isNoteOn()) return;
    first_.store(pos, std::memory_order_relaxed);
  }
  events_.push_back({pos, message, deviceId});
  last_.store(pos, std::memory_order_relaxed);
  count_.store(events_.size(), std::memory_order_release);
}

bool MidiArchive::takeEnded(double silenceSeconds, bool now, std::vector<MidiRecorder::TakeEvent>& out,
                            double& startedAgo) {
  if (!hasTake()) return false;
  const int64_t pos = pos_.load(std::memory_order_acquire);
  const double quiet = static_cast<double>(pos - last_.load(std::memory_order_relaxed)) / sampleRate_;
  const double length = static_cast<double>(pos - first_.load(std::memory_order_relaxed)) / sampleRate_;
  if (!now && quiet < silenceSeconds && length < kMaxTakeSeconds) return false;
  {
    const juce::SpinLock::ScopedLockType hold(lock_);
    std::swap(events_, spare_);
    count_.store(0, std::memory_order_release);
  }
  const int64_t first = spare_.empty() ? 0 : spare_.front().samplePos;
  startedAgo = static_cast<double>(pos - first) / sampleRate_;
  out.clear();
  out.reserve(spare_.size());
  for (const auto& e : spare_)
    out.push_back({static_cast<double>(e.samplePos - first) / sampleRate_, e.message, e.deviceId});
  spare_.clear();
  spare_.reserve(kReserve);
  return !out.empty();
}

juce::File MidiArchive::save(const juce::File& folder, const std::vector<MidiRecorder::TakeEvent>& events,
                             const juce::String& organ, juce::Time started, const MidiRecorder::DeviceName& nameOf) {
  const juce::File day = folder.getChildFile(started.formatted("%Y-%m-%d"));
  const juce::String name = juce::File::createLegalFileName(started.formatted("%H-%M-%S") + " " +
                                                            (organ.isEmpty() ? juce::String("Masterpiece") : organ));
  const juce::File file = day.getChildFile(name + ".mid").getNonexistentSibling();
  if (!day.createDirectory() || !MidiRecorder::writeTake(file, events, nameOf)) return {};
  auto listed = readIndex(folder);
  listed.push_back({juce::Time::getCurrentTime().toMilliseconds(), file.getRelativePathFrom(folder)});
  writeIndex(folder, listed);
  return file;
}

int MidiArchive::prune(const juce::File& folder, int64_t maxBytes, int maxDays, juce::Time now) {
  auto listed = readIndex(folder);
  // Only what the archive wrote, and is still there.
  std::vector<Listed> kept;
  for (const auto& l : listed) {
    const juce::File f = folder.getChildFile(l.path);
    if (f.existsAsFile() && f.isAChildOf(folder) && f.hasFileExtension(".mid")) kept.push_back(l);
  }
  std::stable_sort(kept.begin(), kept.end(), [](const Listed& a, const Listed& b) { return a.writtenMs < b.writtenMs; });
  int64_t total = 0;
  for (const auto& l : kept) total += folder.getChildFile(l.path).getSize();
  const juce::int64 oldest = now.toMilliseconds() - static_cast<juce::int64>(maxDays) * 24 * 60 * 60 * 1000;
  int deleted = 0;
  size_t from = 0;
  while (kept.size() - from > 1 && (kept[from].writtenMs < oldest || total > maxBytes)) {
    const juce::File f = folder.getChildFile(kept[from].path);
    const juce::File day = f.getParentDirectory();
    total -= f.getSize();
    if (f.deleteFile()) ++deleted;
    if (day != folder && day.isAChildOf(folder) && day.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0)
      day.deleteFile();
    ++from;
  }
  kept.erase(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(from));
  writeIndex(folder, kept);
  return deleted;
}

}  // namespace mp
