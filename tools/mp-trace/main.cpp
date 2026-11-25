// mp-trace: play a script through an organ, headless, and print every switch,
// control and speaking pipe that changed, with the time.
//
//   mp-trace <definition> <script> [--network legacy|event|ab|list]
//            [--engine processor|network] [--all] [--frames <n>] [--tick <ms>]
//
// --engine processor (the default where the audio engine is built) plays the
//   script through a whole MasterpieceProcessor: what the player hears. Keys
//   go in as MIDI. --frames caps the frames loaded per sample (0: all).
// --engine network plays it through the network alone (TraceSimulator).
// --network ab runs both networks and prints the difference: "-" lines only
//   the legacy network wrote, "+" lines only the event network wrote. --all
//   compares every kind of line; without it, switches, controls and pipes.
// --network list prints the keyboards and stops a script can name.
// Exit status: 0, or 1 when ab found a difference, 2 on a usage or load error.
#include "../../src/mp_control/TraceSimulator.h"
#include "../../src/mp_core/OdfLoader.h"
#ifdef MP_TRACE_HAS_PROCESSOR
#include "../../src/mp_audio/ProcessorTrace.h"
#include <juce_gui_basics/juce_gui_basics.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {
int usage() {
  std::fprintf(stderr,
               "usage: mp-trace <definition> <script> [--network legacy|event|ab|list] "
               "[--engine processor|network] [--all] [--frames <n>] [--tick <ms>]\n");
  return 2;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string odf = argv[1];
  const std::string scriptPath = argv[2];
  std::string network = "ab";
#ifdef MP_TRACE_HAS_PROCESSOR
  std::string engine = "processor";
  mp::ProcessorTraceOptions popts;
#else
  std::string engine = "network";
#endif
  bool all = false;
  mp::TraceOptions options;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--network") == 0 && i + 1 < argc) network = argv[++i];
    else if (std::strcmp(argv[i], "--engine") == 0 && i + 1 < argc) engine = argv[++i];
    else if (std::strcmp(argv[i], "--all") == 0) all = true;
    else if (std::strcmp(argv[i], "--tick") == 0 && i + 1 < argc) options.tickMs = std::stod(argv[++i]);
#ifdef MP_TRACE_HAS_PROCESSOR
    else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) popts.maxFramesPerSample = std::stoll(argv[++i]);
#endif
    else return usage();
  }
  if (network != "legacy" && network != "event" && network != "ab" && network != "list") return usage();
#ifdef MP_TRACE_HAS_PROCESSOR
  if (engine != "processor" && engine != "network") return usage();
  juce::ScopedJuceInitialiser_GUI juce;
#else
  if (engine != "network") return usage();
#endif

  std::ifstream in(scriptPath);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", scriptPath.c_str());
    return 2;
  }
  std::stringstream text;
  text << in.rdbuf();
  mp::TraceScript script;
  std::string error;
  if (!mp::TraceScript::parse(text.str(), script, error)) {
    std::fprintf(stderr, "%s: %s\n", scriptPath.c_str(), error.c_str());
    return 2;
  }

  mp::OrganModel model;
  mp::OdfDiagnostics diag;
  mp::OdfLoader loader;
  mp::OdfLoader::Options lo;
  if (engine == "network" || network == "list")
    if (!loader.load(odf, lo, model, diag)) {
      std::fprintf(stderr, "cannot load %s\n", odf.c_str());
      return 2;
    }

  if (network == "list") {
    // What a script can name: keyboards, stops and their switches.
    std::map<mp::Id, std::string> lines;
    for (const auto& [id, k] : model.keyboards) lines[id] = "keyboard " + std::to_string(id) + "  | " + k.name;
    for (const auto& [id, l] : lines) std::cout << l << "\n";
    lines.clear();
    for (const auto& [id, s] : model.stops)
      lines[id] = "stop " + std::to_string(id) + " switch " + std::to_string(s.controllingSwitchId) + " division " +
                  std::to_string(s.divisionId) + "  | " + s.name;
    for (const auto& [id, l] : lines) std::cout << l << "\n";
    std::cout << "# " << model.switches.size() << " switches, " << model.switchLinkages.size() << " wires, "
              << model.keyActions.size() << " key actions, " << model.divisionInputs.size() << " division inputs, "
              << model.continuousControls.size() << " controls, " << model.controlLinkages.size()
              << " control linkages, " << model.controlStageSwitches.size() << " stage switches\n";
    return 0;
  }

  auto trace = [&](mp::OrganNetwork::Kind kind) -> std::vector<std::string> {
#ifdef MP_TRACE_HAS_PROCESSOR
    if (engine == "processor") {
      auto r = mp::runProcessorTrace(juce::File(odf), kind, script, popts);
      if (!r.loaded) {
        std::fprintf(stderr, "cannot load %s: %s\n", odf.c_str(), r.error.c_str());
        std::exit(2);
      }
      return r.lines;
    }
#endif
    return mp::runTrace(model, kind, script, options);
  };

  if (network != "ab") {
    for (const auto& line : trace(network == "event" ? mp::OrganNetwork::Kind::Event : mp::OrganNetwork::Kind::Legacy))
      std::cout << line << "\n";
    return 0;
  }
  const auto a = trace(mp::OrganNetwork::Kind::Legacy);
  const auto b = trace(mp::OrganNetwork::Kind::Event);
  const std::vector<std::string> kinds =
      all ? std::vector<std::string>{} : std::vector<std::string>{"switch", "control", "pipe"};
  const auto d = mp::diffTraces(a, b, kinds);
  for (const auto& line : d.lines) std::cout << line << "\n";
  std::cout << "# " << engine << ": legacy " << a.size() << " lines, event " << b.size() << " lines; " << d.onlyA
            << " only in legacy, " << d.onlyB << " only in event\n";
  return d.same() ? 0 : 1;
}
