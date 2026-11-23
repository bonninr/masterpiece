#include "JOrganDisposition.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace mp {
namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// An element id. jOrgan before 3.8 could write negative ids; its conversion
// turns the minus sign into a 0, and so does this.
int idOf(std::string s) {
  std::replace(s.begin(), s.end(), '-', '0');
  return std::atoi(s.c_str());
}

bool isNumber(const std::string& s) {
  if (s.empty()) return false;
  char* end = nullptr;
  std::strtod(s.c_str(), &end);
  return end && *end == '\0';
}

}  // namespace

std::string JOrganElement::prop(const std::string& key, const std::string& fallback) const {
  const auto it = props.find(key);
  return it == props.end() ? fallback : it->second;
}

int JOrganElement::num(const std::string& key, int fallback) const {
  const std::string v = trim(prop(key));
  return isNumber(v) ? static_cast<int>(std::lround(std::strtod(v.c_str(), nullptr))) : fallback;
}

double JOrganElement::real(const std::string& key, double fallback) const {
  const std::string v = trim(prop(key));
  return isNumber(v) ? std::strtod(v.c_str(), nullptr) : fallback;
}

std::vector<std::string> JOrganElement::messagesOf(const std::string& kind) const {
  std::vector<std::string> out;
  for (const auto& [k, text] : messages)
    if (k == kind) out.push_back(text);
  return out;
}

const JOrganElement* JOrganDisposition::find(int id) const {
  const auto it = byId.find(id);
  return it == byId.end() ? nullptr : &elements[it->second];
}

bool readJOrganDisposition(const std::string& text, JOrganDisposition& out, std::string& error) {
  pugi::xml_document doc;
  const pugi::xml_parse_result parsed = doc.load_buffer(text.data(), text.size());
  if (!parsed) {
    error = std::string("the disposition is not readable XML: ") + parsed.description();
    return false;
  }
  const pugi::xml_node organ = doc.child("organ");
  if (!organ) {
    error = "not a jOrgan disposition: no <organ> element";
    return false;
  }
  out = {};
  out.version = organ.attribute("version").as_string();
  for (pugi::xml_node e : organ.child("elements").children()) {
    if (e.type() != pugi::node_element) continue;
    JOrganElement el;
    el.type = e.name();
    el.id = idOf(e.attribute("id").as_string());
    for (pugi::xml_node c : e.children()) {
      if (c.type() != pugi::node_element) continue;
      const std::string tag = c.name();
      if (tag == "name") {
        el.name = trim(c.text().as_string());
      } else if (tag == "description") {
        el.description = c.text().as_string();
      } else if (tag == "style") {
        el.style = trim(c.text().as_string());
      } else if (tag == "zoom") {
        el.zoom = c.text().as_double(1.0);
      } else if (tag == "references") {
        for (pugi::xml_node r : c.children()) {
          if (r.type() != pugi::node_element) continue;
          JOrganReference ref;
          // Before jOrgan 3.8 the id is an <element ref="..."/> inside.
          ref.id = idOf(r.attribute("id") ? r.attribute("id").as_string()
                                          : r.child("element").attribute("ref").as_string());
          if (r.child("x") || r.child("y")) {
            ref.x = r.child("x").text().as_int(0);
            ref.y = r.child("y").text().as_int(0);
            ref.located = true;
          }
          if (ref.id) el.references.push_back(ref);
        }
      } else if (tag == "messages") {
        for (pugi::xml_node m : c.children()) {
          if (m.type() != pugi::node_element) continue;
          // Before jOrgan 3.11 a message is <status>, <data1>, <data2>.
          if (m.child("status"))
            el.messages.emplace_back(m.name(), std::string(m.child("status").text().as_string()) + ", " +
                                                   m.child("data1").text().as_string() + ", " +
                                                   m.child("data2").text().as_string());
          else
            el.messages.emplace_back(m.name(), m.text().as_string());
        }
      } else if (!c.first_child() || c.first_child().type() == pugi::node_pcdata) {
        el.props[tag] = c.text().as_string();
      }
    }
    if (el.id == 0) continue;
    out.byId[el.id] = out.elements.size();
    out.elements.push_back(std::move(el));
  }
  if (out.elements.empty()) {
    error = "the disposition lists no elements";
    return false;
  }
  return true;
}

double evaluateMpl(const std::string& chain, double value, MplVariables& vars) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const std::string& rawStep : split(chain, '|')) {
    const std::string step = trim(rawStep);
    if (step.empty()) continue;  // a step with nothing in it passes the value on
    std::istringstream in(step);
    std::string op;
    in >> op;
    std::string rest;
    std::getline(in, rest);
    rest = trim(rest);
    // A value: a number, a variable, or a variable with the number to use
    // when it is not set.
    auto operand = [&]() {
      if (rest.empty()) return nan;
      const auto space = rest.find(' ');
      if (space == std::string::npos) {
        if (isNumber(rest)) return std::strtod(rest.c_str(), nullptr);
        const auto it = vars.find(rest);
        return it == vars.end() ? nan : it->second;
      }
      const std::string name = rest.substr(0, space);
      const auto it = vars.find(name);
      if (it != vars.end() && !std::isnan(it->second)) return it->second;
      return std::strtod(trim(rest.substr(space + 1)).c_str(), nullptr);
    };
    const double x = isNumber(rest) ? std::strtod(rest.c_str(), nullptr) : nan;
    if (op == "set") value = operand();
    else if (op == "add") value += operand();
    else if (op == "sub") value -= operand();
    else if (op == "mult") value *= operand();
    else if (op == "div") value /= operand();
    else if (op == "mod") value = std::fmod(value, operand());
    else if (op == "min") value = std::min(value, operand());
    else if (op == "max") value = std::max(value, operand());
    else if (op == "round") value = std::round(value);
    else if (op == "get") vars[rest] = value;
    else if (op == "equal") value = value == x ? value : nan;
    else if (op == "notEqual") value = value != x ? value : nan;
    else if (op == "greater") value = value > x ? value : nan;
    else if (op == "greaterEqual") value = value >= x ? value : nan;
    else if (op == "less") value = value < x ? value : nan;
    else if (op == "lessEqual") value = value <= x ? value : nan;
    else value = nan;
    if (std::isnan(value)) return nan;
  }
  return value;
}

std::vector<int> mplBytes(const std::string& message, const MplVariables& vars) {
  std::vector<int> out;
  MplVariables v = vars;
  for (const std::string& command : split(message, ',')) {
    const double x = evaluateMpl(command, 0.0, v);
    const long r = std::isnan(x) ? -1 : std::lround(x);
    out.push_back(r < 0 || r > 255 ? -1 : static_cast<int>(r));
  }
  return out;
}

int mplMatchedStatus(const std::string& message) {
  const std::string first = trim(split(split(message, ',')[0], '|')[0]);
  if (first.rfind("equal ", 0) != 0) return -1;
  const std::string n = trim(first.substr(6));
  return isNumber(n) ? static_cast<int>(std::strtod(n.c_str(), nullptr)) : -1;
}

}  // namespace mp
