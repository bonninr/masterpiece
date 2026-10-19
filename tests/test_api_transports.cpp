// The control protocol over MIDI System Exclusive and over OSC: the byte
// encoding against the manual's example, then each transport on a small
// organ, with replies captured where they would be sent.
#include "MpTest.h"
#include "../src/mp_api/OscTransport.h"
#include "../src/mp_api/SysexCodec.h"
#include "../src/mp_api/SysexTransport.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <string>
#include <vector>

namespace {

using mp::test::Category;
using namespace mp::api;

juce::File fixture() { return juce::File(juce::String(MP_TEST_FIXTURES_DIR) + "/m24.expression.Organ_Hauptwerk_xml"); }

int findSwitch(ControlModel& m, const char* name) {
  Error e;
  const juce::var list = m.list(Type::Switch, e);
  for (const auto& item : *list["items"].getArray())
    if (item["name"].toString() == name) return static_cast<int>(item["index"]);
  return -1;
}

class SysexCodecTest final : public mp::test::Test {
public:
  SysexCodecTest() : Test("functional.api.sysex-codec", Category::Functional) {}
  void run() override {
    // The manual's example: draw switch 12 on unit 0, sequence 5.
    sysex::Message m;
    m.op = Op::Set;
    m.sequence = 5;
    m.payload = {static_cast<uint8_t>(Type::Switch)};
    sysex::putIndex(m.payload, 12);
    sysex::putValue(m.payload, 1);
    const std::vector<uint8_t> expected{0xF0, 0x7D, 0x4D, 0x50, 0x00, 0x00, 0x05, 0x05,
                                        0x01, 0x0C, 0x00, 0x01, 0x00, 0x00, 0xF7};
    MP_CHECK(sysex::build(m) == expected, "the bytes of the manual's example");

    sysex::Message back;
    MP_CHECK(sysex::parse(expected.data(), expected.size(), back), "and they parse");
    MP_CHECK(back.op == Op::Set && back.sequence == 5 && back.unit == 0 && back.major == kMajor, "the header");
    MP_CHECK(back.payload.size() == 6 && sysex::getIndex(&back.payload[1]) == 12 &&
                 sysex::getValue(&back.payload[3]) == 1,
             "the payload");
    MP_CHECK(sysex::parse(expected.data() + 1, expected.size() - 2, back), "without F0 and F7 too");
    const std::vector<uint8_t> other{0xF0, 0x7D, 0x00, 0x01, 0x00, 0x00, 0x05, 0x05, 0xF7};
    MP_CHECK(!sysex::parse(other.data(), other.size(), back), "another 7D message is not ours");

    for (int v : {0, 1, -1, -30, 127, 128, -240, 1048575, -1048576}) {
      std::vector<uint8_t> b;
      sysex::putValue(b, v);
      MP_CHECK(b.size() == 3 && sysex::getValue(b.data()) == v && (b[0] | b[1] | b[2]) < 0x80,
               "value " + std::to_string(v) + " in 7-bit bytes and back");
    }
    std::vector<uint8_t> b;
    sysex::putIndex(b, 16383);
    MP_CHECK(sysex::getIndex(b.data()) == 16383, "the largest index");
    b.clear();
    sysex::putMask(b, 1u << 16 | 1u << 1);
    MP_CHECK(sysex::getMask(b.data()) == (1u << 16 | 1u << 1), "a mask reaching type 16");

    const std::string flute = "Fl\xc3\xbbte harmonique 8'";
    b.clear();
    sysex::putText(b, flute);
    bool seven = true;
    for (uint8_t x : b) seven = seven && x < 0x80;
    MP_CHECK(seven && sysex::getText(b.data(), b.size()) == flute, "UTF-8 text packed into 7 bits and back");

    b.clear();
    sysex::putBits(b, {1, 0, 1, 0, 0, 0, 0, 1});
    MP_CHECK(b.size() == 2 && b[0] == 0x05 && b[1] == 0x01, "bits, 7 to a byte, low first");
  }
};
static SysexCodecTest g_sysexCodec;

class SysexTransportTest final : public mp::test::Test {
public:
  SysexTransportTest() : Test("functional.api.sysex", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    std::vector<sysex::Message> sent;
    SysexTransport t(proc, [&](const juce::MidiMessage& msg) {
      sysex::Message r;
      if (sysex::parse(msg.getRawData(), static_cast<size_t>(msg.getRawDataSize()), r)) sent.push_back(r);
    });
    int sequence = 0;
    const auto ask = [&](Op op, std::vector<uint8_t> payload, int unit = 0) {
      sysex::Message m;
      m.unit = unit;
      m.op = op;
      m.sequence = ++sequence & 0x7F;
      m.payload = std::move(payload);
      const auto bytes = sysex::build(m);
      const juce::MidiMessage msg(bytes.data(), static_cast<int>(bytes.size()));
      MP_CHECK(SysexTransport::isOurs(msg), "a request is recognised as ours");
      sent.clear();
      t.handle(msg);
      return sent.empty() ? sysex::Message{} : sent.back();
    };
    const auto typeIndex = [](Type type, int index) {
      std::vector<uint8_t> p{static_cast<uint8_t>(type)};
      sysex::putIndex(p, index);
      return p;
    };

    MP_CHECK(!SysexTransport::isOurs(juce::MidiMessage::noteOn(1, 60, 0.5f)), "a note is not ours");
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    t.pollNow();  // the load: the session, then the state from here on

    auto r = ask(Op::Identify, {});
    MP_CHECK(r.op == Op::Identify && r.sequence == sequence && r.payload.size() > 6 && r.payload[0] == kMinor,
             "Identify answers with the minor version, paired by sequence");
    MP_CHECK(sysex::getValue(&r.payload[4]) == 1, "and the session");

    r = ask(Op::Count, {static_cast<uint8_t>(Type::Switch)});
    MP_CHECK(r.op == Op::Count && sysex::getIndex(&r.payload[1]) == 3, "three switches");

    ControlModel m(proc);
    const int trem = findSwitch(m, "Swell Tremulant");
    MP_CHECK(trem >= 0, "the tremulant found");

    r = ask(Op::Describe, typeIndex(Type::Switch, trem));
    MP_CHECK(r.op == Op::Describe && r.payload.size() <= static_cast<size_t>(sysex::kMaxMessage - 9),
             "Describe fits one message");
    bool named = false, isTremulant = false;
    for (size_t i = 3; i < r.payload.size();) {
      const auto prop = static_cast<Prop>(r.payload[i++]);
      if (prop == Prop::Name) {
        named = sysex::getText(&r.payload[i], r.payload.size() - i) == "Swell Tremulant";
        break;
      }
      if (prop == Prop::Role) isTremulant = sysex::getValue(&r.payload[i]) == static_cast<int>(Role::Tremulant);
      i += 3;
    }
    MP_CHECK(named && isTremulant, "with its role and its name");

    auto set = typeIndex(Type::Switch, trem);
    sysex::putValue(set, 1);
    r = ask(Op::Set, set);
    Error e;
    int v = 0;
    MP_CHECK(r.op == Op::Set && sysex::getValue(&r.payload[3]) == 1, "Set answers with the value now");
    MP_CHECK(m.get(Type::Switch, trem, v, e) && v == 1, "and the tremulant is drawn");

    r = ask(Op::Get, typeIndex(Type::Switch, trem), sysex::kUnitAll);
    MP_CHECK(r.op == Op::Get && sysex::getValue(&r.payload[3]) == 1, "a message to every unit is answered");
    r = ask(Op::Get, typeIndex(Type::Switch, trem), 5);
    MP_CHECK(sent.empty(), "one to another unit is not");

    std::vector<uint8_t> dump = typeIndex(Type::Switch, 0);
    sysex::putIndex(dump, 280);
    r = ask(Op::Dump, dump);
    MP_CHECK(r.op == Op::Dump && sysex::getIndex(&r.payload[3]) == 3 && r.payload.size() == 6 &&
                 ((r.payload[5] >> trem) & 1) == 1,
             "Dump: the three switches as bits");

    r = ask(Op::Get, {99, 0, 0});
    MP_CHECK(r.op == Op::Error && r.payload[0] == static_cast<uint8_t>(Op::Get) &&
                 r.payload[1] == static_cast<uint8_t>(Error::UnknownType),
             "an unknown type is an error naming the operation");
    r = ask(Op::Set, {static_cast<uint8_t>(Type::Switch)});
    MP_CHECK(r.op == Op::Error && r.payload[1] == static_cast<uint8_t>(Error::BadMessage), "a short Set");
    r = ask(Op::Press, typeIndex(Type::Command, static_cast<int>(Command::PowerOff)));
    MP_CHECK(r.op == Op::Error && r.payload[1] == static_cast<uint8_t>(Error::NotAllowed),
             "power-off is refused unless allowed");

    std::vector<uint8_t> mask;
    sysex::putMask(mask, 1u << static_cast<int>(Type::Switch));
    r = ask(Op::Subscribe, mask);
    MP_CHECK(r.op == Op::Subscribe && sysex::getMask(r.payload.data()) == 1u << static_cast<int>(Type::Switch),
             "subscribed to switches");
    t.pollNow();
    sent.clear();
    v = 0;
    m.set(Type::Switch, trem, v);
    t.pollNow();
    bool event = false;
    for (const auto& s : sent)
      event = event || (s.op == Op::Event && s.sequence == 0 && s.payload[0] == static_cast<uint8_t>(Type::Switch) &&
                        sysex::getIndex(&s.payload[1]) == trem && sysex::getValue(&s.payload[3]) == 0);
    MP_CHECK(event, "a change made elsewhere arrives as an event");
    r = ask(Op::Unsubscribe, mask);
    MP_CHECK(sysex::getMask(r.payload.data()) == 0, "and unsubscribed");
  }
};
static SysexTransportTest g_sysexTransport;

juce::OSCMessage osc(const juce::String& address) { return juce::OSCMessage(juce::OSCAddressPattern(address)); }

class OscTransportTest final : public mp::test::Test {
public:
  OscTransportTest() : Test("functional.api.osc", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    OscTransport t(proc);
    std::vector<juce::OSCMessage> sent;
    t.onSend = [&](const juce::OSCMessage& msg) { sent.push_back(msg); };
    const auto ask = [&](juce::OSCMessage msg) {
      sent.clear();
      t.handle(msg);
      return sent.empty() ? osc("/none") : sent.back();
    };
    const auto addr = [](const juce::OSCMessage& msg) { return msg.getAddressPattern().toString(); };

    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    t.pollNow();
    ControlModel m(proc);
    const int trem = findSwitch(m, "Swell Tremulant");

    auto r = ask(osc("/mp/identify"));
    MP_CHECK(addr(r) == "/mp/identify" && r.size() == 5 && r[0].getString() == "Masterpiece control",
             "Identify: the protocol first");
    r = ask(osc("/mp/switches/count"));
    MP_CHECK(addr(r) == "/mp/switches/count" && r[0].getInt32() == 3, "three switches");

    const juce::String tremAddress = "/mp/switches/" + juce::String(trem);
    auto set = osc(tremAddress);
    set.addInt32(1);
    r = ask(set);
    Error e;
    int v = 0;
    MP_CHECK(addr(r) == tremAddress && r[0].getInt32() == 1, "Set answers on the same address");
    MP_CHECK(m.get(Type::Switch, trem, v, e) && v == 1, "and the tremulant is drawn");
    r = ask(osc(tremAddress));
    MP_CHECK(r.size() == 1 && r[0].getInt32() == 1, "Get with no argument");

    r = ask(osc(tremAddress + "/describe"));
    bool named = false;
    for (int i = 0; i + 1 < r.size(); ++i)
      named = named || (r[i].isString() && r[i].getString() == "name" && r[i + 1].getString() == "Swell Tremulant");
    MP_CHECK(named, "Describe: name and value pairs");

    auto transpose = osc("/mp/program/transpose");
    transpose.addFloat32(2.2f);
    r = ask(transpose);
    MP_CHECK(addr(r) == "/mp/program/transpose" && r[0].getInt32() == 2, "a property by name, a float rounded");
    transpose = osc("/mp/program/transpose");
    transpose.addInt32(0);
    ask(transpose);
    r = ask(osc("/mp/commands/panic/press"));
    MP_CHECK(addr(r) == "/mp/commands/panic/press" && r.isEmpty(), "a command by name");

    auto dump = osc("/mp/switches/dump");
    r = ask(dump);
    MP_CHECK(r.size() == 4 && r[0].getInt32() == 0 && r[1 + trem].getInt32() == 1, "Dump: first, then values");

    r = ask(osc("/mp/stops/1"));
    MP_CHECK(addr(r) == "/mp/error" && r[0].getString() == "/mp/stops/1" && r[2].getString() == "unknown-type",
             "an unknown type is an error with the address");
    r = ask(osc("/mp/switches/99"));
    MP_CHECK(addr(r) == "/mp/error" && r[2].getString() == "out-of-range", "no switch 99");
    r = ask(osc("/mp/commands/powerOff/press"));
    MP_CHECK(addr(r) == "/mp/error" && r[2].getString() == "not-allowed", "power-off refused unless allowed");
    MP_CHECK(ask(osc("/other/thing")).getAddressPattern().toString() == "/none", "another program's address is left");

    auto sub = osc("/mp/subscribe");
    sub.addString("switches");
    r = ask(sub);
    MP_CHECK(r[0].getInt32() == 1 << static_cast<int>(Type::Switch), "subscribed to switches");
    t.pollNow();
    sent.clear();
    v = 0;
    m.set(Type::Switch, trem, v);
    t.pollNow();
    bool event = false;
    for (const auto& s : sent) event = event || (addr(s) == tremAddress && s[0].getInt32() == 0);
    MP_CHECK(event, "a change made elsewhere arrives on the Get address");

    t.setAllowChanges(false);
    set = osc(tremAddress);
    set.addInt32(1);
    r = ask(set);
    MP_CHECK(addr(r) == "/mp/error" && r[2].getString() == "not-allowed", "read only refuses a change");
    r = ask(osc(tremAddress));
    MP_CHECK(addr(r) == tremAddress, "and still answers reads");

    // The socket: a free UDP port taken and given back.
    juce::String error;
    int port = 0;
    for (int p = 39100; p < 39200 && port == 0; ++p)
      if (t.start(p, error)) port = p;
    MP_CHECK(port != 0 && t.running(), "listens on a UDP port");
    t.stop();
    MP_CHECK(!t.running(), "and stops");
  }
};
static OscTransportTest g_oscTransport;

}  // namespace
