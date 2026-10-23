#include "OscTransport.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace mp::api {

namespace {

juce::String str(std::string_view s) { return juce::String(std::string(s)); }

constexpr char kBundleTag[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};

bool startsBundle(const uint8_t* p, std::size_t n) {
  return n >= 8 && std::memcmp(p, kBundleTag, 8) == 0;
}

int32_t beInt32(const uint8_t* p) {
  return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                              (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
}

juce::OSCMessage message(const juce::String& address) { return juce::OSCMessage(juce::OSCAddressPattern(address)); }

// A number argument as an int: OSC faders send floats.
bool intArg(const juce::OSCArgument& a, int& out) {
  if (a.isInt32()) {
    out = a.getInt32();
    return true;
  }
  if (a.isFloat32()) {
    out = juce::roundToInt(a.getFloat32());
    return true;
  }
  return false;
}

// A property's value in a describe reply: numbers as ints, text as strings,
// a list as its length and then its numbers.
void addValue(juce::OSCMessage& m, const juce::var& v) {
  if (v.isString())
    m.addString(v.toString());
  else if (const auto* list = v.getArray()) {
    m.addInt32(list->size());
    for (const auto& item : *list) m.addInt32(static_cast<int>(item));
  } else
    m.addInt32(static_cast<int>(v));
}

}  // namespace

OscTransport::OscTransport(MasterpieceProcessor& p) : model_(p) {
  receiver_.addListener(this);
  startTimerHz(20);
}

OscTransport::~OscTransport() {
  stopTimer();
  stop();
  receiver_.removeListener(this);
}

bool OscTransport::packetWithinLimits(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  // A message, or something too short or not a bundle: no nesting, so nothing
  // for the recursive parser to run away on. The parser reports any error.
  if (!startsBundle(bytes, size)) return true;
  // Walk the bundle tree with an explicit stack, never recursing. Each entry is
  // a byte range of a bundle's elements and the depth that bundle sits at.
  struct Range {
    const uint8_t* p;
    const uint8_t* end;
    int depth;
  };
  std::vector<Range> stack;
  stack.push_back({bytes + 16, bytes + size, 1});  // past "#bundle\0" and the 8-byte time tag
  while (!stack.empty()) {
    const Range r = stack.back();
    stack.pop_back();
    if (r.depth > kMaxBundleDepth) return false;
    const uint8_t* p = r.p;
    while (p + 4 <= r.end) {
      const int32_t elemSize = beInt32(p);
      p += 4;
      if (elemSize < 0 || (elemSize % 4) != 0 || p + elemSize > r.end) return false;  // not well formed
      if (startsBundle(p, static_cast<std::size_t>(elemSize))) {
        if (r.depth + 1 > kMaxBundleDepth) return false;
        stack.push_back({p + 16, p + elemSize, r.depth + 1});
      }
      p += elemSize;
    }
  }
  return true;
}

void OscTransport::filterLoop() {
  std::vector<char> buffer(65536);
  while (filtering_.load()) {
    const int ready = publicSocket_->waitUntilReady(true, 200);
    if (ready < 0) break;      // the socket failed
    if (ready == 0) continue;  // timed out: look at the flag again
    const int n = publicSocket_->read(buffer.data(), static_cast<int>(buffer.size()), false);
    if (n <= 0) continue;
    if (packetWithinLimits(buffer.data(), static_cast<std::size_t>(n)))
      forwardSocket_->write("127.0.0.1", relayPort_, buffer.data(), n);
  }
}

bool OscTransport::start(int port, juce::String& error, bool loopbackOnly) {
  stop();
  publicSocket_ = std::make_unique<juce::DatagramSocket>(false);
  const bool bound = loopbackOnly ? publicSocket_->bindToPort(port, "127.0.0.1") : publicSocket_->bindToPort(port);
  if (!bound) {
    publicSocket_.reset();
    error = "UDP port " + juce::String(port) + " is in use";
    return false;
  }
  // The JUCE receiver reads a private loopback socket; only packets that pass
  // packetWithinLimits are forwarded to it.
  relaySocket_ = std::make_unique<juce::DatagramSocket>(false);
  forwardSocket_ = std::make_unique<juce::DatagramSocket>(false);
  if (!relaySocket_->bindToPort(0, "127.0.0.1") || !forwardSocket_->bindToPort(0, "127.0.0.1") ||
      !receiver_.connectToSocket(*relaySocket_)) {
    forwardSocket_.reset();
    relaySocket_.reset();
    publicSocket_.reset();
    error = "the OSC port could not be opened";
    return false;
  }
  relayPort_ = relaySocket_->getBoundPort();
  filtering_.store(true);
  filterThread_ = std::thread([this] { filterLoop(); });
  port_ = port;
  return true;
}

void OscTransport::stop() {
  filtering_.store(false);
  if (filterThread_.joinable()) filterThread_.join();
  receiver_.disconnect();
  forwardSocket_.reset();
  relaySocket_.reset();
  publicSocket_.reset();
  relayPort_ = 0;
  port_ = 0;
}

void OscTransport::send(const juce::OSCMessage& m) {
  if (onSend)
    onSend(m);
  else if (hasTarget_)
    sender_.send(m);
}

void OscTransport::error(const juce::String& address, Error e) {
  auto m = message("/mp/error");
  m.addString(address);
  m.addInt32(static_cast<int>(e));
  const auto* info = errorInfo(e);
  m.addString(info != nullptr ? str(info->name) : juce::String());
  send(m);
}

int OscTransport::indexOf(Type type, const juce::String& part) {
  if (part.containsOnly("0123456789") && part.isNotEmpty()) return part.getIntValue();
  if (type == Type::Program)
    for (const auto& p : kProgram)
      if (part == str(p.name)) return static_cast<int>(p.prop);
  if (type == Type::Command)
    for (int i = 0; i < kCommandCount; ++i)
      if (part == str(kCommandNames[i])) return i;
  return -1;
}

uint32_t OscTransport::maskOf(const juce::OSCMessage& m) {
  if (m.isEmpty()) return 0x1FFFFF;  // every type
  uint32_t mask = 0;
  for (const auto& a : m) {
    if (a.isString()) {
      if (const auto* t = typeByPlural(a.getString().toStdString())) mask |= 1u << static_cast<int>(t->type);
    } else if (int code = 0; intArg(a, code) && code >= 0 && code < 21) {
      mask |= 1u << code;
    }
  }
  return mask;
}

void OscTransport::oscMessageReceived(const juce::OSCMessage& m) { handle(m); }

void OscTransport::oscBundleReceived(const juce::OSCBundle& b) {
  for (const auto& e : b) {
    if (e.isMessage())
      handle(e.getMessage());
    else if (e.isBundle())
      oscBundleReceived(e.getBundle());
  }
}

void OscTransport::handle(const juce::OSCMessage& m) {
  const juce::String address = m.getAddressPattern().toString();
  auto parts = juce::StringArray::fromTokens(address, "/", "");
  parts.removeEmptyStrings();
  if (parts.size() < 2 || parts[0] != "mp") return;  // not ours
  const juce::String head = parts[1];

  if (head == "identify" && parts.size() == 2) {
    const juce::var id = model_.identity();
    auto r = message("/mp/identify");
    r.addString(id["protocol"].toString());
    r.addString(id["version"].toString());
    r.addString(id["programVersion"].toString());
    r.addInt32(static_cast<int>(id["session"]));
    r.addString(id["organ"].toString());
    return send(r);
  }
  if (head == "replyto" && parts.size() == 2) {
    juce::String host = "127.0.0.1";
    int port = 0;
    for (const auto& a : m) {
      if (a.isString()) host = a.getString();
      else intArg(a, port);
    }
    if (port < 1 || port > 65535) return error(address, Error::BadValue);
    hasTarget_ = sender_.connect(host, port);
    if (!hasTarget_) return error(address, Error::BadValue);
    auto r = message(address);
    r.addString(host);
    r.addInt32(port);
    return send(r);
  }
  if ((head == "subscribe" || head == "unsubscribe") && parts.size() == 2) {
    const uint32_t mask = maskOf(m);
    mask_ = head == "subscribe" ? (mask_ | mask) : (mask_ & ~mask);
    auto r = message(address);
    r.addInt32(static_cast<int>(mask_));
    return send(r);
  }

  const TypeInfo* t = typeByPlural(head.toStdString());
  if (t == nullptr) return error(address, Error::UnknownType);
  const Type type = t->type;
  Error e = Error::None;

  if (parts.size() == 3 && parts[2] == "count") {
    auto r = message(address);
    r.addInt32(model_.count(type));
    return send(r);
  }
  if (parts.size() == 3 && parts[2] == "dump") {
    if (!t->hasValue) return error(address, Error::UnknownType);
    int first = 0, n = 1000;
    if (m.size() > 0 && !intArg(m[0], first)) return error(address, Error::BadValue);
    if (m.size() > 1 && !intArg(m[1], n)) return error(address, Error::BadValue);
    // A UDP datagram holds some 8,000 values; a page of 1,000 is safe.
    n = juce::jlimit(0, juce::jmax(0, model_.count(type) - first), juce::jmin(n, 1000));
    auto r = message(address);
    r.addInt32(first);
    for (int i = 0; i < n; ++i) {
      int v = 0;
      if (!model_.get(type, first + i, v, e)) return error(address, e);
      r.addInt32(v);
    }
    return send(r);
  }

  if (parts.size() < 3 || parts.size() > 4) return error(address, Error::UnknownOperation);
  const int index = indexOf(type, parts[2]);
  if (index < 0) return error(address, Error::OutOfRange);

  if (parts.size() == 4) {
    if (parts[3] == "press") {
      if (!allowChanges_) return error(address, Error::NotAllowed);
      e = model_.press(type, index);
      if (e != Error::None) return error(address, e);
      return send(message(address));
    }
    if (parts[3] == "describe") {
      const juce::var o = model_.describe(type, index, e);
      if (e != Error::None) return error(address, e);
      auto r = message(address);
      if (const auto* obj = o.getDynamicObject())
        for (const auto& p : obj->getProperties()) {
          r.addString(p.name.toString());
          addValue(r, p.value);
        }
      return send(r);
    }
    return error(address, Error::UnknownOperation);
  }

  int value = 0;
  if (m.size() > 0) {
    if (!intArg(m[0], value)) return error(address, Error::BadValue);
    if (!allowChanges_) return error(address, Error::NotAllowed);
    e = model_.set(type, index, value);
    if (e != Error::None) return error(address, e);
  } else if (!model_.get(type, index, value, e)) {
    return error(address, e);
  }
  auto r = message(address);
  r.addInt32(value);
  send(r);
}

void OscTransport::timerCallback() {
  std::vector<ControlModel::Change> changes;
  model_.poll(changes, ++ticks_ % 10 == 0);
  if (mask_ == 0) return;
  for (const auto& c : changes) {
    if (((mask_ >> static_cast<int>(c.type)) & 1u) == 0) continue;
    const auto* t = typeInfo(c.type);
    juce::String address = "/mp/" + str(t->plural) + "/";
    address << (c.type == Type::Program ? str(kProgram[c.index].name) : juce::String(c.index));
    auto r = message(address);
    r.addInt32(c.value);
    send(r);
  }
}

}  // namespace mp::api
