#include "OrganStructure.h"

#include "ObjectLabels.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <vector>

namespace mp {

namespace {

std::string format(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  return buf;
}

std::string quoted(const std::string& name) {
  return name.empty() ? std::string() : " \"" + name + "\"";
}

std::string switchText(const OrganModel& m, Id id) {
  std::string name = switchLabel(m, id);
  if (name.empty()) {
    const auto s = m.switches.find(id);
    if (s != m.switches.end()) name = s->second.name;
  }
  return format("switch %lld", static_cast<long long>(id)) + quoted(name);
}

std::string controlText(const OrganModel& m, Id id) {
  const auto c = m.continuousControls.find(id);
  return format("control %lld", static_cast<long long>(id)) +
         quoted(c == m.continuousControls.end() ? std::string() : c->second.name);
}

std::string rankText(const OrganModel& m, Id id) {
  const auto r = m.ranks.find(id);
  return format("rank %lld", static_cast<long long>(id)) +
         quoted(r == m.ranks.end() ? std::string() : r->second.name);
}

std::string drift(const RandomWalk& w) {
  return w.active() ? format("%.2f %%", w.maxPct) : std::string("none");
}

template <class Map>
std::vector<Id> sortedIds(const Map& map) {
  std::vector<Id> ids;
  ids.reserve(map.size());
  for (const auto& entry : map) ids.push_back(entry.first);
  std::sort(ids.begin(), ids.end());
  return ids;
}

// Lines under a heading, cut at maxRows with the number left out.
void section(std::string& out, const std::string& heading, const std::vector<std::string>& rows, int maxRows) {
  out += heading + "\n";
  const int shown = std::min(static_cast<int>(rows.size()), maxRows);
  for (int i = 0; i < shown; ++i) out += "  " + rows[static_cast<size_t>(i)] + "\n";
  if (static_cast<int>(rows.size()) > shown)
    out += format("  and %d more\n", static_cast<int>(rows.size()) - shown);
}

}  // namespace

std::string windAndTremulantReport(const OrganModel& m, const CompartmentPressures& pressures, int maxRows) {
  std::string out;

  // What the pipes draw, per compartment.
  struct Draw {
    int pipes = 0;
    double flow = 0.0;
    int drifting = 0;
  };
  std::map<Id, Draw> draws;
  int pipes = 0, windless = 0;
  for (const auto& [rankId, rank] : m.ranks) {
    for (const auto& pipe : rank.pipes) {
      ++pipes;
      if (pipe.windSourceCompartmentId == 0) {
        ++windless;
        continue;
      }
      auto& d = draws[pipe.windSourceCompartmentId];
      ++d.pipes;
      d.flow += pipe.windMassFlowKgPerSec;
      if (pipe.windFlowRandom.active()) ++d.drifting;
    }
  }

  std::vector<std::string> rows;
  for (const Id id : sortedIds(m.wind)) {
    const auto& c = m.wind.at(id);
    std::string row = format("compartment %lld", static_cast<long long>(id)) + quoted(c.name) + ": ";
    if (c.infiniteVolume) row += format("fixed pressure %.2f in", c.defaultPressureInches);
    else row += format("%.3f m3, %.2f in", c.volumeM3, c.defaultPressureInches);
    if (c.hasBellows) {
      row += format("; bellows %.1f kg, damping %.2f, board %.2f x %.2f x %.2f m", c.bellowsMassKg, c.bellowsDamping,
                    c.bellowsWidthM, c.bellowsLengthM, c.bellowsExtensionM);
      if (c.bellowsGravityMassKg > 0.0)
        row += format(", weight %.1f kg, shape %d", c.bellowsGravityMassKg, c.bellowsShapeCode);
    }
    if (c.bellowsExtensionOutputControlId != 0)
      row += "; extension to " + controlText(m, c.bellowsExtensionOutputControlId);
    if (c.pressureOutputControlId != 0) row += "; pressure to " + controlText(m, c.pressureOutputControlId);
    if (pressures) {
      const auto [working, now] = pressures(id);
      row += format("; working %.3f in, now %.3f in", working, now);
    }
    if (const auto d = draws.find(id); d != draws.end()) {
      row += format("; %d pipes draw %.4f kg/s", d->second.pipes, d->second.flow);
      if (d->second.drifting > 0) row += format(", %d with a flow drift", d->second.drifting);
    }
    rows.push_back(row);
  }
  section(out,
          format("Wind: %d compartments, %d linkages; %d of %d pipes draw wind", static_cast<int>(m.wind.size()),
                 static_cast<int>(m.windLinks.size()), pipes - windless, pipes),
          rows, maxRows);

  rows.clear();
  int index = 0;
  for (const auto& link : m.windLinks) {
    ++index;
    std::string row = format("linkage %d, compartment %lld to %lld", index,
                             static_cast<long long>(link.firstCompartmentId),
                             static_cast<long long>(link.secondCompartmentId)) +
                      quoted(link.name) + ": ";
    if (link.valveSwitchId != 0)
      row += "valve " + switchText(m, link.valveSwitchId) +
             (link.valveOpenWhenEngaged ? " (open when engaged)" : " (open when disengaged)") + "; ";
    if (link.valveControlId != 0)
      row += "valve " + controlText(m, link.valveControlId) + format(", type %d; ", link.valveControlTypeCode);
    if (link.valveSwitchId == 0 && link.valveControlId == 0) row += "open; ";
    row += format("%.4f kg/s at %.2f in; drift ", link.massFlowKgPerSec, link.refPressureInches) +
           drift(link.flowRandom);
    rows.push_back(row);
  }
  section(out, format("Wind linkages: %d", static_cast<int>(m.windLinks.size())), rows, maxRows);

  // What each tremulant reaches, from the per-pipe rows.
  struct Reach {
    int pipes = 0;
    double dbLow = 0.0, dbHigh = 0.0, pctLow = 0.0, pctHigh = 0.0;
  };
  std::map<Id, Reach> reach;
  for (const auto& [pipeId, mod] : m.tremulantPipes) {
    auto& r = reach[mod.tremulantId];
    if (r.pipes == 0) {
      r.dbLow = r.dbHigh = mod.ampDepthDb;
      r.pctLow = r.pctHigh = mod.pitchDepthPct;
    }
    ++r.pipes;
    r.dbLow = std::min(r.dbLow, mod.ampDepthDb);
    r.dbHigh = std::max(r.dbHigh, mod.ampDepthDb);
    r.pctLow = std::min(r.pctLow, mod.pitchDepthPct);
    r.pctHigh = std::max(r.pctHigh, mod.pitchDepthPct);
  }
  rows.clear();
  for (const Id id : sortedIds(m.tremulants)) {
    const auto& t = m.tremulants.at(id);
    std::string row = format("tremulant %lld", static_cast<long long>(id)) + quoted(t.name) + ": ";
    row += t.controllingSwitchId != 0 ? switchText(m, t.controllingSwitchId) : std::string("no switch");
    row += format("; %.2f Hz drawn, %.2f Hz off; depth %.1f %%; start %.0f %%, stop %.0f %%", t.engagedHz,
                  t.disengagedHz, t.depthPercent, t.startPercent, t.stopPercent);
    if (t.hasWaveform) row += format("; waveform %lld", static_cast<long long>(t.waveformId));
    if (const auto r = reach.find(id); r != reach.end())
      row += format("; %d pipes, %.1f to %.1f dB, pitch %.0f to %.0f %%", r->second.pipes, r->second.dbLow,
                    r->second.dbHigh, r->second.pctLow, r->second.pctHigh);
    else
      row += "; no pipes";
    row += "; rate drift " + drift(t.rateRandom) + ", depth drift " + drift(t.depthRandom);
    rows.push_back(row);
  }
  section(out, format("Tremulants: %d", static_cast<int>(m.tremulants.size())), rows, maxRows);

  // Ranks a switch swaps in for another: a tremulant's recorded pipes, most
  // often. One line per stop row that names one.
  rows.clear();
  for (const Id id : sortedIds(m.stops)) {
    const auto& stop = m.stops.at(id);
    for (const auto& entry : stop.ranks) {
      if (entry.alternateRankId == 0) continue;
      std::string row = rankText(m, entry.alternateRankId) + " for " + rankText(m, entry.rankId) +
                        format(" in stop %lld", static_cast<long long>(id)) + quoted(stop.name);
      row += entry.alternateSwitchId != 0 ? ", by " + switchText(m, entry.alternateSwitchId)
                                          : std::string(", by no switch");
      if (entry.retriggerOnAlternate) row += ", sounding again on the swap";
      rows.push_back(row);
    }
  }
  section(out, format("Alternate ranks: %d", static_cast<int>(rows.size())), rows, maxRows);
  return out;
}

}  // namespace mp
