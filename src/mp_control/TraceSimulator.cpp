#include "TraceSimulator.h"
#include "WindSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace mp {

namespace {

bool onOff(const std::string& s, int& out) {
  if (s == "on" || s == "down" || s == "1") return (out = 1), true;
  if (s == "off" || s == "up" || s == "0") return (out = 0), true;
  return false;
}

std::string ms(double t) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", t);
  return buf;
}

template <typename Map>
std::string nameIn(const Map& m, Id id) {
  const auto it = m.find(id);
  return it == m.end() ? std::string() : it->second.name;
}

// Collects what the network reports while a step runs.
class Recorder final : public NetworkListener {
public:
  Recorder(const OrganModel& m, bool sounding) : model_(m), sounding_(sounding) {}
  void divisionNote(Id division, int note, bool on, double) override {
    if (sounding_)
      pending.push_back("division " + std::to_string(division) + " " + std::to_string(note) + (on ? " on" : " off") +
                        "  | " + nameIn(model_.divisions, division));
  }
  void pipeMoved(Id rank, Id pipe, bool on, double, const Pipe*) override {
    if (sounding_)
      pending.push_back("pipe " + std::to_string(rank) + ":" + std::to_string(pipe) + (on ? " on" : " off") +
                        "  | " + nameIn(model_.ranks, rank));
  }
  std::vector<std::string> pending;

private:
  const OrganModel& model_;
  bool sounding_;
};

}  // namespace

bool TraceScript::parse(const std::string& text, TraceScript& out, std::string& error) {
  out.steps.clear();
  std::istringstream in(text);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
    std::istringstream words(line);
    std::vector<std::string> w;
    for (std::string x; words >> x;) w.push_back(x);
    if (w.empty()) continue;
    TraceStep s;
    s.text = line;
    bool ok = false;
    try {
      const std::string& c = w[0];
      if (c == "key" && (w.size() == 4 || w.size() == 5)) {
        s.kind = TraceStep::Kind::Key;
        s.id = static_cast<Id>(std::stoll(w[1]));
        s.note = std::stoi(w[2]);
        int down = 0;
        ok = onOff(w[3], down);
        s.value = down != 0 ? (w.size() == 5 ? std::stoi(w[4]) : 100) : 0;
      } else if ((c == "switch" || c == "stop") && w.size() == 3) {
        s.kind = c == "switch" ? TraceStep::Kind::Switch : TraceStep::Kind::Stop;
        s.id = static_cast<Id>(std::stoll(w[1]));
        ok = onOff(w[2], s.value);
      } else if (c == "piston" && w.size() == 2) {
        s.kind = TraceStep::Kind::Piston;
        s.id = static_cast<Id>(std::stoll(w[1]));
        ok = true;
      } else if (c == "control" && w.size() == 3) {
        s.kind = TraceStep::Kind::Control;
        s.id = static_cast<Id>(std::stoll(w[1]));
        s.value = std::stoi(w[2]);
        ok = true;
      } else if ((c == "wait" || c == "tick") && w.size() == 2) {
        s.kind = c == "wait" ? TraceStep::Kind::Wait : TraceStep::Kind::Tick;
        s.ms = std::stod(w[1]);
        ok = s.ms >= 0.0 && (c == "wait" || s.ms > 0.0);
      }
    } catch (...) {
      ok = false;
    }
    if (!ok) {
      error = "line " + std::to_string(number) + ": cannot read \"" + line + "\"";
      return false;
    }
    out.steps.push_back(std::move(s));
  }
  return true;
}

std::vector<std::string> runTrace(const OrganModel& model, OrganNetwork::Kind kind, const TraceScript& script,
                                  const TraceOptions& options) {
  std::vector<std::string> trace;
  auto net = makeOrganNetwork(kind);
  Recorder rec(model, options.sounding);
  net->setListener(&rec);
  net->setDrivesStages(true);

  // Loaded as the processor loads it: switches, then controls settled with
  // the switch state, then the timed linkages live from the first block.
  net->resetSwitches(model);
  const std::unordered_set<Id> windStart = WindSolver::relayStartControls(model);
  net->resetControls(model, windStart.empty() ? nullptr : &windStart);
  net->propagate(0, &net->engagedSwitches());
  net->setTimed(true);
  net->startUp();

  std::vector<Id> switches, controls;
  for (const auto& kv : model.switches) switches.push_back(kv.first);
  for (const auto& kv : model.continuousControls) controls.push_back(kv.first);
  std::sort(switches.begin(), switches.end());
  std::sort(controls.begin(), controls.end());
  std::vector<uint8_t> was(switches.size());
  std::vector<int> valueWas(controls.size());

  // Everything that moved since the last look, at time `t`.
  auto flush = [&](const std::string& t) {
    std::vector<std::string> lines;
    for (size_t i = 0; i < switches.size(); ++i) {
      const uint8_t on = net->engaged(switches[i]) ? 1 : 0;
      if (on == was[i]) continue;
      was[i] = on;
      lines.push_back("switch " + std::to_string(switches[i]) + (on != 0 ? " on" : " off") + "  | " +
                      nameIn(model.switches, switches[i]));
    }
    for (size_t i = 0; i < controls.size(); ++i) {
      const int v = net->value(controls[i]);
      if (v == valueWas[i]) continue;
      valueWas[i] = v;
      lines.push_back("control " + std::to_string(controls[i]) + " " + std::to_string(v) + "  | " +
                      nameIn(model.continuousControls, controls[i]));
    }
    for (auto& l : rec.pending) lines.push_back(std::move(l));
    rec.pending.clear();
    std::sort(lines.begin(), lines.end());
    for (const auto& l : lines) trace.push_back(t + " " + l);
  };
  for (size_t i = 0; i < controls.size(); ++i) {
    const auto& c = model.continuousControls.at(controls[i]);
    valueWas[i] = std::clamp(c.defaultValue, std::min(c.minValue, c.maxValue), std::max(c.minValue, c.maxValue));
  }
  flush("init");

  double t = 0.0;
  double tick = options.tickMs;
  for (const TraceStep& s : script.steps) {
    switch (s.kind) {
      case TraceStep::Kind::Tick: tick = s.ms; continue;
      case TraceStep::Kind::Wait: {
        const auto n = static_cast<long>(std::ceil(s.ms / tick - 1e-9));
        for (long i = 0; i < n; ++i) {
          t += tick;
          net->advance(tick / 1000.0);
          net->propagate(0, &net->engagedSwitches());
          flush(ms(t));
        }
        continue;
      }
      case TraceStep::Kind::Key:
        if (s.value != 0) net->keyDown(s.id, s.note, s.value);
        else net->keyUp(s.id, s.note);
        break;
      case TraceStep::Kind::Switch: net->set(s.id, s.value != 0); break;
      case TraceStep::Kind::Stop: {
        const auto st = model.stops.find(s.id);
        if (st != model.stops.end() && st->second.controllingSwitchId != 0)
          net->set(st->second.controllingSwitchId, s.value != 0);
        break;
      }
      case TraceStep::Kind::Piston:
        net->set(s.id, true);
        net->propagate(0, &net->engagedSwitches());
        flush(ms(t));
        net->set(s.id, false);
        break;
      case TraceStep::Kind::Control:
        net->setValue(s.id, s.value);
        net->propagate(s.id, &net->engagedSwitches());
        break;
    }
    net->propagate(0, &net->engagedSwitches());
    flush(ms(t));
  }
  net->setListener(nullptr);
  return trace;
}

TraceDiff diffTraces(const std::vector<std::string>& a, const std::vector<std::string>& b,
                     const std::vector<std::string>& kinds) {
  // Time -> lines at that time, in the order times first appear.
  using Lines = std::multiset<std::string>;
  std::vector<std::string> order;
  std::map<std::string, std::pair<Lines, Lines>> at;
  auto take = [&](const std::vector<std::string>& trace, bool first) {
    for (const auto& line : trace) {
      const auto sp = line.find(' ');
      if (sp == std::string::npos) continue;
      const std::string time = line.substr(0, sp);
      std::string rest = line.substr(sp + 1);
      if (!kinds.empty()) {
        const std::string kind = rest.substr(0, rest.find(' '));
        if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) continue;
      }
      auto [it, added] = at.try_emplace(time);
      if (added) order.push_back(time);
      (first ? it->second.first : it->second.second).insert(std::move(rest));
    }
  };
  take(a, true);
  take(b, false);
  // "init" first, then by time.
  std::stable_sort(order.begin(), order.end(), [](const std::string& x, const std::string& y) {
    if (x == "init" || y == "init") return x == "init" && y != "init";
    return std::stod(x) < std::stod(y);
  });
  TraceDiff d;
  for (const auto& time : order) {
    auto& [la, lb] = at[time];
    for (const auto& l : la)
      if (auto it = lb.find(l); it != lb.end()) lb.erase(it);
      else {
        ++d.onlyA;
        d.lines.push_back("- " + time + " " + l);
      }
    for (const auto& l : lb) {
      ++d.onlyB;
      d.lines.push_back("+ " + time + " " + l);
    }
  }
  return d;
}

} // namespace mp
