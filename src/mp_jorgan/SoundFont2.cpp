#include "SoundFont2.h"

#include "mp_core/Utf8Path.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

namespace mp {
namespace {

// The generators this reads, by their numbers in the specification.
enum : int {
  kStartAddrsOffset = 0,
  kEndAddrsOffset = 1,
  kStartloopAddrsOffset = 2,
  kEndloopAddrsOffset = 3,
  kStartAddrsCoarseOffset = 4,
  kModEnvToPitch = 7,
  kInitialFilterFc = 8,
  kEndAddrsCoarseOffset = 12,
  kPan = 17,
  kSustainModEnv = 29,
  kAttackVolEnv = 34,
  kHoldVolEnv = 35,
  kDecayVolEnv = 36,
  kSustainVolEnv = 37,
  kReleaseVolEnv = 38,
  kInstrument = 41,
  kKeyRange = 43,
  kVelRange = 44,
  kStartloopAddrsCoarseOffset = 45,
  kInitialAttenuation = 48,
  kEndloopAddrsCoarseOffset = 50,
  kCoarseTune = 51,
  kFineTune = 52,
  kSampleId = 53,
  kSampleModes = 54,
  kScaleTuning = 56,
  kExclusiveClass = 57,
  kOverridingRootKey = 58,
  kGeneratorCount = 61,
};

uint16_t u16(const char* p) {
  return static_cast<uint16_t>(static_cast<unsigned char>(p[0]) | (static_cast<unsigned char>(p[1]) << 8));
}
uint32_t u32(const char* p) {
  return static_cast<uint32_t>(u16(p)) | (static_cast<uint32_t>(u16(p + 2)) << 16);
}

std::string name20(const char* p) {
  std::string s(p, strnlen(p, 20));
  while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
  return s;
}

double seconds(int timecents) { return std::pow(2.0, timecents / 1200.0); }

// A zone's generators as a table, with whether each was given.
struct Gens {
  int value[kGeneratorCount] = {};
  bool given[kGeneratorCount] = {};
  void take(int op, uint16_t amount) {
    if (op < 0 || op >= kGeneratorCount) return;
    value[op] = (op == kKeyRange || op == kVelRange) ? amount : static_cast<int16_t>(amount);
    given[op] = true;
  }
  int get(int op, int fallback) const { return given[op] ? value[op] : fallback; }
  bool has(int op) const { return given[op]; }
};

void rangeOf(const Gens& g, int op, int& lo, int& hi) {
  if (!g.has(op)) return;
  lo = std::max(lo, g.value[op] & 0xff);
  hi = std::min(hi, (g.value[op] >> 8) & 0xff);
}

}  // namespace

bool SoundFont2::open(const std::string& path, std::string& error) {
  path_ = path;
  std::ifstream f(pathOf(path), std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  char head[12];
  if (!f.read(head, 12) || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "sfbk", 4) != 0) {
    error = path + " is not a SoundFont 2 file";
    return false;
  }
  const uint64_t riffEnd = 8 + static_cast<uint64_t>(u32(head + 4));
  std::map<std::string, std::string> tables;  // the pdta sub-chunks, read whole
  uint64_t sm24Bytes = 0;
  uint64_t at = 12;
  while (at + 8 <= riffEnd) {
    char ch[12];
    f.seekg(static_cast<std::streamoff>(at));
    if (!f.read(ch, 8)) break;
    const uint64_t size = u32(ch + 4);
    if (std::memcmp(ch, "LIST", 4) == 0 && f.read(ch + 8, 4)) {
      const std::string kind(ch + 8, 4);
      uint64_t sub = at + 12;
      const uint64_t listEnd = at + 8 + size;
      while (sub + 8 <= listEnd) {
        char sh[8];
        f.seekg(static_cast<std::streamoff>(sub));
        if (!f.read(sh, 8)) break;
        const std::string id(sh, 4);
        const uint64_t subSize = u32(sh + 4);
        if (kind == "sdta" && id == "smpl") {
          smplOffset_ = sub + 8;
          smplBytes_ = subSize;
        } else if (kind == "sdta" && id == "sm24") {
          sm24Offset_ = sub + 8;
          sm24Bytes = subSize;
        } else if (kind == "pdta") {
          std::string data(static_cast<size_t>(subSize), '\0');
          if (!f.read(data.data(), static_cast<std::streamsize>(subSize))) break;
          tables[id] = std::move(data);
        }
        sub += 8 + subSize + (subSize & 1);
      }
    }
    at += 8 + size + (size & 1);
  }
  if (smplBytes_ == 0) {
    error = path + " holds no sample data";
    return false;
  }
  // An sm24 chunk of the wrong size is ignored, as the format asks.
  if (sm24Offset_ && sm24Bytes < smplBytes_ / 2) sm24Offset_ = 0;
  for (const char* id : {"phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr"})
    if (tables.find(id) == tables.end()) {
      error = path + " has no " + id + " table";
      return false;
    }

  const std::string& shdr = tables["shdr"];
  for (size_t i = 0; i + 46 <= shdr.size(); i += 46) {
    const char* p = shdr.data() + i;
    Sf2Sample s;
    s.name = name20(p);
    s.start = u32(p + 20);
    s.end = u32(p + 24);
    s.loopStart = u32(p + 28);
    s.loopEnd = u32(p + 32);
    s.rate = u32(p + 36);
    s.originalPitch = static_cast<unsigned char>(p[40]);
    s.pitchCorrection = static_cast<signed char>(p[41]);
    s.link = u16(p + 42);
    s.type = u16(p + 44);
    if (s.originalPitch > 127) s.originalPitch = 60;
    if (s.rate == 0) s.rate = 44100;
    samples_.push_back(s);
  }
  if (!samples_.empty()) samples_.pop_back();  // the terminal record

  // Bags and generators, shared by both layers.
  auto readLayer = [&](const std::string& headers, size_t recordSize, size_t bagAt,
                       const std::string& bags, const std::string& gens, bool preset,
                       std::vector<Item>& out) {
    const size_t count = headers.size() / recordSize;
    for (size_t i = 0; i + 1 < count; ++i) {
      const char* p = headers.data() + i * recordSize;
      const char* next = p + recordSize;
      Item item;
      item.name = name20(p);
      if (preset) {
        item.program = u16(p + 20);
        item.bank = u16(p + 22);
      }
      const size_t bagFrom = u16(p + bagAt), bagTo = u16(next + bagAt);
      for (size_t b = bagFrom; b < bagTo && b * 4 + 8 <= bags.size(); ++b) {
        const size_t genFrom = u16(bags.data() + b * 4), genTo = u16(bags.data() + b * 4 + 4);
        Zone z;
        for (size_t g = genFrom; g < genTo && g * 4 + 4 <= gens.size(); ++g)
          z.gens.push_back({u16(gens.data() + g * 4), u16(gens.data() + g * 4 + 2)});
        item.zones.push_back(std::move(z));
      }
      out.push_back(std::move(item));
    }
  };
  readLayer(tables["phdr"], 38, 24, tables["pbag"], tables["pgen"], true, presetItems_);
  readLayer(tables["inst"], 22, 20, tables["ibag"], tables["igen"], false, instruments_);
  for (const Item& p : presetItems_) presets_.push_back({p.name, p.bank, p.program});
  return true;
}

int SoundFont2::findPreset(int bank, int program) const {
  for (size_t i = 0; i < presets_.size(); ++i)
    if (presets_[i].bank == bank && presets_[i].program == program) return static_cast<int>(i);
  return -1;
}

std::vector<Sf2Region> SoundFont2::regions(int presetIndex) const {
  std::vector<Sf2Region> out;
  if (presetIndex < 0 || presetIndex >= static_cast<int>(presetItems_.size())) return out;

  // A layer's zones with its global zone -- the first, when it names no
  // instrument (or no sample) -- folded into each of the others.
  auto zonesOf = [](const Item& item, int terminal) {
    std::vector<Gens> zones;
    Gens global;
    for (size_t z = 0; z < item.zones.size(); ++z) {
      Gens g;
      for (const Gen& gen : item.zones[z].gens) g.take(gen.op, gen.amount);
      if (!g.has(terminal)) {
        if (z == 0) global = g;
        continue;
      }
      Gens merged = global;
      for (int op = 0; op < kGeneratorCount; ++op)
        if (g.has(op)) merged.take(op, static_cast<uint16_t>(g.value[op]));
      zones.push_back(merged);
    }
    return zones;
  };

  for (const Gens& pz : zonesOf(presetItems_[presetIndex], kInstrument)) {
    const int inst = pz.value[kInstrument];
    if (inst < 0 || inst >= static_cast<int>(instruments_.size())) continue;
    for (const Gens& iz : zonesOf(instruments_[inst], kSampleId)) {
      const int sample = iz.value[kSampleId];
      if (sample < 0 || sample >= static_cast<int>(samples_.size())) continue;
      Sf2Region r;
      r.sample = sample;
      rangeOf(pz, kKeyRange, r.keyLo, r.keyHi);
      rangeOf(iz, kKeyRange, r.keyLo, r.keyHi);
      rangeOf(pz, kVelRange, r.velLo, r.velHi);
      rangeOf(iz, kVelRange, r.velLo, r.velHi);
      if (r.keyLo > r.keyHi || r.velLo > r.velHi) continue;
      // The preset layer adds to what the instrument says.
      auto sum = [&](int op, int fallback) { return iz.get(op, fallback) + pz.get(op, 0); };
      const Sf2Sample& s = samples_[sample];
      const int root = iz.get(kOverridingRootKey, -1);
      r.rootKey = root >= 0 && root <= 127 ? root : s.originalPitch;
      // The pitch the note holds: its tuning, plus what the modulation
      // envelope adds at its sustain level. Some sets tune a sample down
      // with coarse tune and back up with the envelope, so that the pitch
      // falls only as the note is released.
      const double modSustain = 1.0 - std::clamp(sum(kSustainModEnv, 0), 0, 1000) / 1000.0;
      r.tuneCents = sum(kCoarseTune, 0) * 100.0 + sum(kFineTune, 0) + s.pitchCorrection +
                    sum(kModEnvToPitch, 0) * modSustain;
      r.scaleTuning = sum(kScaleTuning, 100);
      r.loopMode = iz.get(kSampleModes, 0) & 3;
      r.startOffset = iz.get(kStartAddrsOffset, 0) + 32768ll * iz.get(kStartAddrsCoarseOffset, 0);
      r.endOffset = iz.get(kEndAddrsOffset, 0) + 32768ll * iz.get(kEndAddrsCoarseOffset, 0);
      r.loopStartOffset = iz.get(kStartloopAddrsOffset, 0) + 32768ll * iz.get(kStartloopAddrsCoarseOffset, 0);
      r.loopEndOffset = iz.get(kEndloopAddrsOffset, 0) + 32768ll * iz.get(kEndloopAddrsCoarseOffset, 0);
      r.attenuationCb = std::max(0, sum(kInitialAttenuation, 0));
      r.pan = std::clamp(sum(kPan, 0), -500, 500);
      r.attackSec = seconds(sum(kAttackVolEnv, -12000));
      r.holdSec = seconds(sum(kHoldVolEnv, -12000));
      r.decaySec = seconds(sum(kDecayVolEnv, -12000));
      r.releaseSec = seconds(std::min(8000, sum(kReleaseVolEnv, -12000)));
      r.sustainCb = std::clamp(sum(kSustainVolEnv, 0), 0, 1440);
      r.filterFcCents = sum(kInitialFilterFc, 13500);
      r.exclusiveClass = iz.get(kExclusiveClass, 0);
      out.push_back(r);
    }
  }
  return out;
}

bool SoundFont2::readFrames(uint32_t from, uint32_t to, std::vector<float>& out) const {
  out.clear();
  if (to <= from || static_cast<uint64_t>(to) * 2 > smplBytes_) return false;
  std::ifstream f(pathOf(path_), std::ios::binary);
  if (!f) return false;
  const size_t n = to - from;
  std::string pcm(n * 2, '\0');
  f.seekg(static_cast<std::streamoff>(smplOffset_ + static_cast<uint64_t>(from) * 2));
  if (!f.read(pcm.data(), static_cast<std::streamsize>(pcm.size()))) return false;
  std::string low;
  if (sm24Offset_) {
    low.assign(n, '\0');
    f.seekg(static_cast<std::streamoff>(sm24Offset_ + from));
    if (!f.read(low.data(), static_cast<std::streamsize>(n))) low.clear();
  }
  out.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const int hi = static_cast<int16_t>(u16(pcm.data() + i * 2));
    if (!low.empty())
      out[i] = static_cast<float>((hi * 256 + static_cast<unsigned char>(low[i])) / 8388608.0);
    else
      out[i] = static_cast<float>(hi / 32768.0);
  }
  return true;
}

std::string SoundFont2::Builder::bytes() const {
  auto put16 = [](std::string& s, int v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
  };
  auto put32 = [&](std::string& s, uint32_t v) {
    put16(s, static_cast<int>(v & 0xffff));
    put16(s, static_cast<int>(v >> 16));
  };
  auto name = [](std::string& s, const std::string& n) {
    std::string t = n.substr(0, 19);
    t.resize(20, '\0');
    s += t;
  };
  auto chunk = [&](const std::string& id, const std::string& body) {
    std::string c = id;
    put32(c, static_cast<uint32_t>(body.size()));
    c += body;
    if (body.size() & 1) c += '\0';
    return c;
  };
  // Samples, each followed by the 46 points of silence the format asks for.
  std::string smpl, shdr;
  for (const Sample& s : samples) {
    const uint32_t start = static_cast<uint32_t>(smpl.size() / 2);
    for (int16_t v : s.pcm) put16(smpl, v);
    for (int i = 0; i < 46; ++i) put16(smpl, 0);
    name(shdr, s.name);
    put32(shdr, start);
    put32(shdr, start + static_cast<uint32_t>(s.pcm.size()));
    put32(shdr, start + s.loopStart);
    put32(shdr, start + s.loopEnd);
    put32(shdr, s.rate);
    shdr += static_cast<char>(s.originalPitch);
    shdr += static_cast<char>(s.pitchCorrection);
    put16(shdr, 0);
    put16(shdr, 1);
  }
  name(shdr, "EOS");
  shdr.append(26, '\0');
  // One instrument per preset, one preset zone naming it.
  std::string phdr, pbag, pgen, inst, ibag, igen, pmod(10, '\0'), imod(10, '\0');
  int pGens = 0, iGens = 0, iBags = 0;
  for (size_t p = 0; p < presets.size(); ++p) {
    name(phdr, presets[p].name);
    put16(phdr, presets[p].program);
    put16(phdr, presets[p].bank);
    put16(phdr, static_cast<int>(p));
    put32(phdr, 0);
    put32(phdr, 0);
    put32(phdr, 0);
    put16(pbag, pGens);
    put16(pbag, 0);
    put16(pgen, kInstrument);
    put16(pgen, static_cast<int>(p));
    ++pGens;
    name(inst, presets[p].name);
    put16(inst, iBags);
    for (const Zone& z : presets[p].zones) {
      put16(ibag, iGens);
      put16(ibag, 0);
      ++iBags;
      auto gen = [&](int op, int v) {
        put16(igen, op);
        put16(igen, v & 0xffff);
        ++iGens;
      };
      gen(kKeyRange, z.keyLo | (z.keyHi << 8));
      if (z.rootKey >= 0) gen(kOverridingRootKey, z.rootKey);
      if (z.fineTune) gen(kFineTune, z.fineTune);
      if (z.coarseTune) gen(kCoarseTune, z.coarseTune);
      if (z.modEnvToPitch) gen(kModEnvToPitch, z.modEnvToPitch);
      if (z.attenuationCb) gen(kInitialAttenuation, z.attenuationCb);
      gen(kReleaseVolEnv, z.releaseTimecents);
      gen(kSampleModes, z.loopMode);
      gen(kSampleId, z.sample);
    }
  }
  // The terminal record: its bag index ends the last preset's zones.
  name(phdr, "EOP");
  put16(phdr, 0);
  put16(phdr, 0);
  put16(phdr, static_cast<int>(presets.size()));
  phdr.append(12, '\0');
  put16(pbag, pGens);
  put16(pbag, 0);
  pgen.append(4, '\0');
  name(inst, "EOI");
  put16(inst, iBags);
  put16(ibag, iGens);
  put16(ibag, 0);
  igen.append(4, '\0');

  std::string info = "INFO" + chunk("ifil", std::string("\2\0\1\0", 4)) + chunk("INAM", std::string("test\0\0", 6));
  std::string sdta = "sdta" + chunk("smpl", smpl);
  std::string pdta = "pdta" + chunk("phdr", phdr) + chunk("pbag", pbag) + chunk("pmod", pmod) +
                     chunk("pgen", pgen) + chunk("inst", inst) + chunk("ibag", ibag) +
                     chunk("imod", imod) + chunk("igen", igen) + chunk("shdr", shdr);
  std::string body = "sfbk" + chunk("LIST", info) + chunk("LIST", sdta) + chunk("LIST", pdta);
  return chunk("RIFF", body);
}

bool writeWav(const std::string& path, const std::vector<float>& interleaved, int channels,
              uint32_t rate, int bits, int unityNote, double unityCents, int64_t loopStart,
              int64_t loopEnd, int64_t releaseCue, std::string& error) {
  auto put16 = [](std::string& s, int v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
  };
  auto put32 = [&](std::string& s, uint32_t v) {
    put16(s, static_cast<int>(v & 0xffff));
    put16(s, static_cast<int>(v >> 16));
  };
  const int bytes = bits == 24 ? 3 : 2;
  std::string fmt;
  put16(fmt, 1);
  put16(fmt, channels);
  put32(fmt, rate);
  put32(fmt, rate * static_cast<uint32_t>(channels * bytes));
  put16(fmt, channels * bytes);
  put16(fmt, bytes * 8);

  std::string data;
  data.reserve(interleaved.size() * static_cast<size_t>(bytes));
  for (float v : interleaved) {
    const double c = std::clamp(static_cast<double>(v), -1.0, 1.0);
    if (bytes == 3) {
      const int x = static_cast<int>(std::lround(c * 8388607.0));
      data += static_cast<char>(x & 0xff);
      data += static_cast<char>((x >> 8) & 0xff);
      data += static_cast<char>((x >> 16) & 0xff);
    } else {
      put16(data, static_cast<int>(std::lround(c * 32767.0)));
    }
  }

  // The unity note and its fraction: a positive fraction of a semitone above it.
  int note = unityNote;
  double cents = unityCents;
  while (cents < 0) {
    cents += 100.0;
    --note;
  }
  while (cents >= 100.0) {
    cents -= 100.0;
    ++note;
  }
  std::string smpl;
  put32(smpl, 0);
  put32(smpl, 0);
  put32(smpl, static_cast<uint32_t>(1e9 / rate));
  put32(smpl, static_cast<uint32_t>(std::clamp(note, 0, 127)));
  put32(smpl, static_cast<uint32_t>(std::min(4294967295.0, cents / 100.0 * 4294967296.0)));
  put32(smpl, 0);
  put32(smpl, 0);
  const bool looped = loopStart >= 0 && loopEnd > loopStart;
  put32(smpl, looped ? 1 : 0);
  put32(smpl, 0);
  if (looped) {
    put32(smpl, 0);
    put32(smpl, 0);
    put32(smpl, static_cast<uint32_t>(loopStart));
    put32(smpl, static_cast<uint32_t>(loopEnd - 1));  // the last point of the loop
    put32(smpl, 0);
    put32(smpl, 0);
  }

  std::string riff = "WAVE";
  auto chunk = [&](const std::string& id, const std::string& body) {
    riff += id;
    put32(riff, static_cast<uint32_t>(body.size()));
    riff += body;
    if (body.size() & 1) riff += '\0';
  };
  chunk("fmt ", fmt);
  chunk("data", data);
  chunk("smpl", smpl);
  if (releaseCue >= 0) {
    std::string cue;
    put32(cue, 1);
    put32(cue, 1);
    put32(cue, static_cast<uint32_t>(releaseCue));
    cue += "data";
    put32(cue, 0);
    put32(cue, 0);
    put32(cue, static_cast<uint32_t>(releaseCue));
    chunk("cue ", cue);
  }
  std::string file = "RIFF";
  put32(file, static_cast<uint32_t>(riff.size()));
  file += riff;

  const std::filesystem::path p = pathOf(path);
  std::filesystem::path tmp = p;
  tmp += ".part";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    if (!o || !o.write(file.data(), static_cast<std::streamsize>(file.size()))) {
      error = "cannot write " + path;
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    error = "cannot write " + path + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace mp
