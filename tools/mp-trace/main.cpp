// mp-trace: play a script through an organ's network, headless, and print
// every switch and control that moved.
//
//   mp-trace <definition> <script> [--network legacy|event|ab|list] [--all]
//            [--no-sounding] [--tick <ms>]
//
// --network ab runs both and prints the difference: "-" lines only the
// legacy network wrote, "+" lines only the event network wrote. It compares
// switches and controls; --all compares division and pipe lines too.
// Exit status: 0, or 1 when ab found a difference, 2 on a usage or load error.
#include "../../src/mp_control/TraceSimulator.h"
#include "../../src/mp_core/OdfLoader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {
int usage() {
  std::fprintf(stderr,
               "usage: mp-trace <definition> <script> [--network legacy|event|ab|list] [--all] [--no-sounding] "
               "[--tick <ms>]\n");
  return 2;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string odf = argv[1];
  const std::string scriptPath = argv[2];
  std::string network = "ab";
  bool all = false;
  mp::TraceOptions options;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--network") == 0 && i + 1 < argc) network = argv[++i];
    else if (std::strcmp(argv[i], "--all") == 0) all = true;
    else if (std::strcmp(argv[i], "--no-sounding") == 0) options.sounding = false;
    else if (std::strcmp(argv[i], "--tick") == 0 && i + 1 < argc) options.tickMs = std::stod(argv[++i]);
    else return usage();
  }
  if (network != "legacy" && network != "event" && network != "ab" && network != "list") return usage();

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
  if (network != "ab") {
    const auto kind = network == "event" ? mp::OrganNetwork::Kind::Event : mp::OrganNetwork::Kind::Legacy;
    for (const auto& line : mp::runTrace(model, kind, script, options)) std::cout << line << "\n";
    return 0;
  }
  const auto a = mp::runTrace(model, mp::OrganNetwork::Kind::Legacy, script, options);
  const auto b = mp::runTrace(model, mp::OrganNetwork::Kind::Event, script, options);
  const std::vector<std::string> kinds =
      all ? std::vector<std::string>{} : std::vector<std::string>{"switch", "control"};
  const auto d = mp::diffTraces(a, b, kinds);
  for (const auto& line : d.lines) std::cout << line << "\n";
  std::cout << "# legacy " << a.size() << " lines, event " << b.size() << " lines; " << d.onlyA
            << " only in legacy, " << d.onlyB << " only in event\n";
  return d.same() ? 0 : 1;
}
