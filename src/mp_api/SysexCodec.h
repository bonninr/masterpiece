// The control protocol's MIDI System Exclusive encoding (protocol/control.yaml,
// encodings.sysex): the bytes of a message, and its fields in 7-bit data.
//
//   F0 7D 4D 50 <unit> <major> <operation> <sequence> <payload ...> F7
//
// No JUCE: a console's firmware can use the same rules.
#pragma once

#include "ControlProtocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mp::api::sysex {

struct Message {
  int unit = 0;
  int major = kMajor;
  Op op = Op::Identify;
  int sequence = 0;
  std::vector<uint8_t> payload;
};

// The message in `data`, with or without its F0 and F7. False when it is
// not one of ours or is too short.
inline bool parse(const uint8_t* data, size_t size, Message& out) {
  if (size > 0 && data[0] == 0xF0) {
    ++data;
    --size;
  }
  if (size > 0 && data[size - 1] == 0xF7) --size;
  constexpr size_t kHead = sizeof(kHeader);
  if (size < kHead + 4) return false;
  for (size_t i = 0; i < kHead; ++i)
    if (data[i] != kHeader[i]) return false;
  out.unit = data[kHead];
  out.major = data[kHead + 1];
  out.op = static_cast<Op>(data[kHead + 2]);
  out.sequence = data[kHead + 3];
  out.payload.assign(data + kHead + 4, data + size);
  for (uint8_t b : out.payload)
    if (b & 0x80) return false;
  return true;
}

// The whole message, F0 to F7.
inline std::vector<uint8_t> build(const Message& m) {
  std::vector<uint8_t> out;
  out.reserve(m.payload.size() + 10);
  out.push_back(0xF0);
  for (uint8_t b : kHeader) out.push_back(b);
  out.push_back(static_cast<uint8_t>(m.unit & 0x7F));
  out.push_back(static_cast<uint8_t>(m.major & 0x7F));
  out.push_back(static_cast<uint8_t>(static_cast<int>(m.op) & 0x7F));
  out.push_back(static_cast<uint8_t>(m.sequence & 0x7F));
  out.insert(out.end(), m.payload.begin(), m.payload.end());
  out.push_back(0xF7);
  return out;
}

// An index or a count: 14 bits, low 7 first.
inline void putIndex(std::vector<uint8_t>& out, int v) {
  out.push_back(static_cast<uint8_t>(v & 0x7F));
  out.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
}
inline int getIndex(const uint8_t* p) { return p[0] | (p[1] << 7); }

// A value: 21 bits, two's complement, low 7 first.
inline void putValue(std::vector<uint8_t>& out, int v) {
  const auto u = static_cast<uint32_t>(v) & 0x1FFFFF;
  out.push_back(static_cast<uint8_t>(u & 0x7F));
  out.push_back(static_cast<uint8_t>((u >> 7) & 0x7F));
  out.push_back(static_cast<uint8_t>((u >> 14) & 0x7F));
}
inline int getValue(const uint8_t* p) {
  uint32_t u = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 7) | (static_cast<uint32_t>(p[2]) << 14);
  if (u & 0x100000) u |= 0xFFE00000u;  // negative
  return static_cast<int>(u);
}

// A type mask: bit n for type n, 21 bits in 3 bytes.
inline void putMask(std::vector<uint8_t>& out, uint32_t mask) { putValue(out, static_cast<int>(mask & 0x1FFFFF)); }
inline uint32_t getMask(const uint8_t* p) { return static_cast<uint32_t>(getValue(p)) & 0x1FFFFF; }

// Text: UTF-8 in groups of up to 7 bytes, each group led by a byte holding
// their high bits (bit n for byte n of the group), as sample dumps pack.
inline void putText(std::vector<uint8_t>& out, const std::string& s) {
  for (size_t i = 0; i < s.size(); i += 7) {
    const size_t n = std::min<size_t>(7, s.size() - i);
    uint8_t high = 0;
    for (size_t k = 0; k < n; ++k)
      if (static_cast<uint8_t>(s[i + k]) & 0x80) high |= static_cast<uint8_t>(1u << k);
    out.push_back(high);
    for (size_t k = 0; k < n; ++k) out.push_back(static_cast<uint8_t>(s[i + k]) & 0x7F);
  }
}
inline std::string getText(const uint8_t* p, size_t size) {
  std::string s;
  for (size_t i = 0; i < size;) {
    const uint8_t high = p[i++];
    for (size_t k = 0; k < 7 && i < size; ++k, ++i)
      s.push_back(static_cast<char>(p[i] | ((high >> k) & 1 ? 0x80 : 0)));
  }
  return s;
}

// 0/1 values, 7 to a byte, low bit first.
inline void putBits(std::vector<uint8_t>& out, const std::vector<int>& values) {
  for (size_t i = 0; i < values.size(); i += 7) {
    uint8_t b = 0;
    for (size_t k = 0; k < 7 && i + k < values.size(); ++k)
      if (values[i + k] != 0) b |= static_cast<uint8_t>(1u << k);
    out.push_back(b);
  }
}

}  // namespace mp::api::sysex
