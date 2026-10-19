#include "SysexTransport.h"

#include "../mp_audio/MasterpieceProcessor.h"

namespace mp::api {

namespace {

// "0.8.1" as three numbers, for Identify.
std::vector<int> versionNumbers(const juce::String& v) {
  std::vector<int> out;
  for (const auto& part : juce::StringArray::fromTokens(v, ".", "")) out.push_back(part.getIntValue());
  while (out.size() < 3) out.push_back(0);
  return out;
}

}  // namespace

SysexTransport::SysexTransport(MasterpieceProcessor& p, Send send) : proc_(p), model_(p), send_(std::move(send)) {
  startTimerHz(20);
}

SysexTransport::~SysexTransport() { stopTimer(); }

bool SysexTransport::isOurs(const juce::MidiMessage& m) {
  if (!m.isSysEx()) return false;
  const uint8_t* d = m.getSysExData();
  const int n = m.getSysExDataSize();
  constexpr int kHead = static_cast<int>(sizeof(sysex::kHeader));
  if (n < kHead) return false;
  for (int i = 0; i < kHead; ++i)
    if (d[i] != sysex::kHeader[i]) return false;
  return true;
}

void SysexTransport::reply(Op op, int sequence, const std::vector<uint8_t>& payload) {
  if (!send_) return;
  sysex::Message m;
  m.unit = unit_;
  m.op = op;
  m.sequence = sequence;
  m.payload = payload;
  const auto bytes = sysex::build(m);
  send_(juce::MidiMessage(bytes.data(), static_cast<int>(bytes.size())));
}

void SysexTransport::error(Op op, int sequence, Error e) {
  std::vector<uint8_t> p{static_cast<uint8_t>(static_cast<int>(op) & 0x7F), static_cast<uint8_t>(static_cast<int>(e))};
  reply(Op::Error, sequence, p);
}

void SysexTransport::handle(const juce::MidiMessage& msg) {
  sysex::Message m;
  if (!sysex::parse(msg.getSysExData(), static_cast<size_t>(msg.getSysExDataSize()), m)) return;
  if (m.unit != unit_ && m.unit != sysex::kUnitAll) return;  // another program's
  if (m.major != kMajor) return error(m.op, m.sequence, Error::UnsupportedVersion);
  const auto& in = m.payload;
  const auto need = [&](size_t n) { return in.size() >= n; };
  const auto typeOf = [](uint8_t code) -> const TypeInfo* { return typeInfo(static_cast<Type>(code)); };
  std::vector<uint8_t> out;
  Error e = Error::None;

  switch (m.op) {
    case Op::Identify: {
      out.push_back(static_cast<uint8_t>(kMinor));
      const juce::var id = model_.identity();
      for (int n : versionNumbers(id["programVersion"].toString())) out.push_back(static_cast<uint8_t>(n & 0x7F));
      sysex::putValue(out, model_.session());
      out.push_back(static_cast<uint8_t>(unit_));
      sysex::putText(out, id["organ"].toString().toStdString());
      return reply(Op::Identify, m.sequence, out);
    }
    case Op::Count: {
      if (!need(1)) return error(m.op, m.sequence, Error::BadMessage);
      const TypeInfo* t = typeOf(in[0]);
      if (t == nullptr) return error(m.op, m.sequence, Error::UnknownType);
      out.push_back(in[0]);
      sysex::putIndex(out, model_.count(t->type));
      return reply(Op::Count, m.sequence, out);
    }
    case Op::Describe: {
      if (!need(3)) return error(m.op, m.sequence, Error::BadMessage);
      const TypeInfo* t = typeOf(in[0]);
      if (t == nullptr) return error(m.op, m.sequence, Error::UnknownType);
      const int index = sysex::getIndex(&in[1]);
      const juce::var o = model_.describe(t->type, index, e);
      if (e != Error::None) return error(m.op, m.sequence, e);
      out.assign(in.begin(), in.begin() + 3);
      // The numeric properties as (code, value) pairs, then the name.
      for (const auto& pr : {std::pair<Prop, const char*>{Prop::OdfId, "odfId"}, {Prop::Division, "division"},
                             {Prop::Latching, "latching"}, {Prop::Available, "available"}, {Prop::Channel, "channel"},
                             {Prop::Own, "own"}, {Prop::Set, "set"}, {Prop::Console, "console"},
                             {Prop::Tuning, "tuning"}, {Prop::Rank, "rank"}, {Prop::Key, "key"},
                             {Prop::Control, "control"}, {Prop::Rate, "rate"}, {Prop::Depth, "depth"},
                             {Prop::Bus, "bus"}}) {
        if (!o.hasProperty(pr.second)) continue;
        out.push_back(static_cast<uint8_t>(pr.first));
        sysex::putValue(out, static_cast<int>(o[pr.second]));
      }
      if (o.hasProperty("role")) {
        const juce::String role = o["role"].toString();
        for (int r = 0; r < 5; ++r)
          if (role == juce::String(std::string(kRoleNames[r]))) {
            out.push_back(static_cast<uint8_t>(Prop::Role));
            sysex::putValue(out, r);
          }
      }
      // The name last, cut to what the message has room for.
      out.push_back(static_cast<uint8_t>(Prop::Name));
      std::string name = o["name"].toString().toStdString();
      const int room = sysex::kMaxMessage - 9 - static_cast<int>(out.size());
      size_t n = name.size();
      while (n > 0 && static_cast<int>(n + (n + 6) / 7) > room) --n;
      while (n > 0 && n < name.size() && (static_cast<uint8_t>(name[n]) & 0xC0) == 0x80) --n;
      name.resize(n);
      sysex::putText(out, name);
      return reply(Op::Describe, m.sequence, out);
    }
    case Op::Get: {
      if (!need(3)) return error(m.op, m.sequence, Error::BadMessage);
      const TypeInfo* t = typeOf(in[0]);
      if (t == nullptr) return error(m.op, m.sequence, Error::UnknownType);
      int value = 0;
      if (!model_.get(t->type, sysex::getIndex(&in[1]), value, e)) return error(m.op, m.sequence, e);
      out.assign(in.begin(), in.begin() + 3);
      sysex::putValue(out, value);
      return reply(Op::Get, m.sequence, out);
    }
    case Op::Set: {
      if (!need(6)) return error(m.op, m.sequence, Error::BadMessage);
      const TypeInfo* t = typeOf(in[0]);
      if (t == nullptr) return error(m.op, m.sequence, Error::UnknownType);
      int value = sysex::getValue(&in[3]);
      e = model_.set(t->type, sysex::getIndex(&in[1]), value);
      if (e != Error::None) return error(m.op, m.sequence, e);
      out.assign(in.begin(), in.begin() + 3);
      sysex::putValue(out, value);
      return reply(Op::Set, m.sequence, out);
    }
    case Op::Press: {
      if (!need(3)) return error(m.op, m.sequence, Error::BadMessage);
      const TypeInfo* t = typeOf(in[0]);
      if (t == nullptr) return error(m.op, m.sequence, Error::UnknownType);
      e = model_.press(t->type, sysex::getIndex(&in[1]));
      if (e != Error::None) return error(m.op, m.sequence, e);
      out.assign(in.begin(), in.begin() + 3);
      return reply(Op::Press, m.sequence, out);
    }
    case Op::Dump: {
      if (!need(5)) return error(m.op, m.sequence, Error::BadMessage);
      const TypeInfo* t = typeOf(in[0]);
      if (t == nullptr || !t->hasValue) return error(m.op, m.sequence, Error::UnknownType);
      const int first = sysex::getIndex(&in[1]);
      const int total = model_.count(t->type);
      const bool bits = t->dump == Dump::Bits;
      const int most = bits ? sysex::kDumpBits : sysex::kDumpValues;
      const int n = std::max(0, std::min({sysex::getIndex(&in[3]), most, total - first}));
      std::vector<int> values;
      for (int i = 0; i < n; ++i) {
        int v = 0;
        model_.get(t->type, first + i, v, e);
        values.push_back(v);
      }
      out.push_back(in[0]);
      sysex::putIndex(out, first);
      sysex::putIndex(out, n);
      if (bits)
        sysex::putBits(out, values);
      else
        for (int v : values) sysex::putValue(out, v);
      return reply(Op::Dump, m.sequence, out);
    }
    case Op::Subscribe:
    case Op::Unsubscribe: {
      if (!need(3)) return error(m.op, m.sequence, Error::BadMessage);
      const uint32_t mask = sysex::getMask(&in[0]);
      mask_ = m.op == Op::Subscribe ? (mask_ | mask) : (mask_ & ~mask);
      sysex::putMask(out, mask_);
      return reply(m.op, m.sequence, out);
    }
    default:
      return error(m.op, m.sequence, Error::UnknownOperation);
  }
}

void SysexTransport::timerCallback() {
  std::vector<ControlModel::Change> changes;
  model_.poll(changes, ++ticks_ % 10 == 0);
  if (mask_ == 0) return;
  for (const auto& c : changes) {
    if (((mask_ >> static_cast<int>(c.type)) & 1u) == 0) continue;
    std::vector<uint8_t> p{static_cast<uint8_t>(c.type)};
    sysex::putIndex(p, c.index);
    sysex::putValue(p, c.value);
    reply(Op::Event, 0, p);
  }
}

}  // namespace mp::api
