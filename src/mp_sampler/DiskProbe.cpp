#include "DiskProbe.h"
#include "../mp_core/Utf8Path.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

namespace mp {

DiskTier classifyTier(double mbPerSec) {
  if (mbPerSec <= 0.0) return DiskTier::Unknown;
  if (mbPerSec >= 1200.0) return DiskTier::Nvme;
  if (mbPerSec >= 350.0) return DiskTier::SataSsd;
  return DiskTier::Hdd; // conservative: slow/unknown magnetic-class handling
}

int headScaleForTier(DiskTier tier) {
  switch (tier) {
    case DiskTier::Nvme: return 100;
    case DiskTier::SataSsd: return 200;
    case DiskTier::Hdd: return 400;
    case DiskTier::RamOnly: return 100; // preloaded; heads irrelevant
    case DiskTier::Unknown: return 200; // conservative default
  }
  return 200;
}

double probeSequentialMBps(const std::string& dir, int megabytes) {
  if (megabytes < 8) megabytes = 8;
  if (megabytes > 256) megabytes = 256;
  // Unique temp name; removed afterwards. Incompressible-ish data so no
  // dedup/compression layer can fake the number.
  std::string path = dir;
  if (!path.empty() && path.back() != '/' && path.back() != '\\')
    path += '/';
  path += "mp-disk-probe.tmp";
  auto drop = [&path] {
    std::error_code ec;
    std::filesystem::remove(mp::pathOf(path), ec);
  };

  std::mt19937_64 rng(0x9E3779B97F4A7C15ull);
  std::vector<char> block(1024 * 1024);
  for (auto& b : block) b = static_cast<char>(rng());

  {
    std::ofstream out(mp::pathOf(path), std::ios::binary | std::ios::trunc);
    if (!out) return -1.0;
    for (int i = 0; i < megabytes; ++i) {
      out.write(block.data(), static_cast<std::streamsize>(block.size()));
      if (!out) { drop(); return -1.0; }
    }
    out.flush();
    if (!out) { drop(); return -1.0; }
  }

  std::vector<char> sink(block.size());
  const auto t0 = std::chrono::steady_clock::now();
  {
    std::ifstream in(mp::pathOf(path), std::ios::binary);
    if (!in) { drop(); return -1.0; }
    for (int i = 0; i < megabytes; ++i) {
      in.read(sink.data(), static_cast<std::streamsize>(sink.size()));
      if (!in) { drop(); return -1.0; }
      // Touch every page so the read can't be optimised away.
      volatile char acc = 0;
      for (size_t k = 0; k < sink.size(); k += 4096) acc += sink[k];
      (void)acc;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  drop();

  const double seconds =
      std::chrono::duration<double>(t1 - t0).count();
  if (seconds <= 0.0) return -1.0;
  return static_cast<double>(megabytes) / seconds;
}

} // namespace mp
