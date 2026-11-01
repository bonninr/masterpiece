#include "ScoreMarks.h"

#include <pugixml.hpp>

#include <map>
#include <sstream>

namespace mp {

namespace {
constexpr const char* kNoteTag = "masterpiece-note";
constexpr const char* kRegistrationTag = "masterpiece-registration";

// Whitespace between elements, kept so a file saved again looks as it did.
bool blank(const pugi::xml_node& n) {
  if (n.type() != pugi::node_pcdata) return false;
  for (const char* c = n.value(); *c != 0; ++c)
    if (*c != ' ' && *c != '\t' && *c != '\n' && *c != '\r') return false;
  return true;
}

pugi::xml_node otherDirectionOf(const pugi::xml_node& direction) {
  for (auto type : direction.children("direction-type"))
    if (auto other = type.child("other-direction")) return other;
  return {};
}

std::string firstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

std::string afterFirstLine(const std::string& text) {
  const auto nl = text.find('\n');
  return nl == std::string::npos ? std::string() : text.substr(nl + 1);
}

struct Writer : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};
}  // namespace

struct ScoreMarks::Impl {
  pugi::xml_document doc;
  std::vector<pugi::xml_node> notes;
  std::map<std::string, size_t> byId;
  // The file's own text around the score's root element (the declaration,
  // the doctype, the lines between them), which the parser does not keep.
  std::string before, after;
  bool around = false;

  void keepAround(const std::string& text) {
    around = false;
    const auto root = doc.document_element();
    if (!root) return;
    const std::string name = root.name();
    const auto at = static_cast<size_t>(root.offset_debug());
    if (at == 0 || at > text.size() || text.compare(at - 1, name.size() + 1, "<" + name) != 0) return;
    const auto close = text.rfind("</" + name);
    if (close == std::string::npos || close < at) return;
    const auto end = text.find('>', close);
    if (end == std::string::npos) return;
    before = text.substr(0, at - 1);
    after = text.substr(end + 1);
    around = true;
  }

  std::string text() const {
    Writer w;
    if (!around) {
      doc.save(w, "", pugi::format_raw | pugi::format_no_declaration);
      return w.out;
    }
    doc.document_element().print(w, "", pugi::format_raw);
    return before + w.out + after;
  }
};

ScoreMarks::ScoreMarks() : impl_(new Impl) {}
ScoreMarks::~ScoreMarks() = default;

bool ScoreMarks::hasNote(const std::string& id) const { return impl_->byId.count(id) != 0; }

bool ScoreMarks::load(const std::string& musicXml) {
  Impl& i = *impl_;
  i.doc.reset();
  i.notes.clear();
  i.byId.clear();
  noteIds_.clear();
  marks.clear();
  engraving_ = musicXml;
  // The line ends kept as they are, so a file is written back as it was read.
  const auto parsed = i.doc.load_buffer(musicXml.data(), musicXml.size(),
                                        (pugi::parse_full | pugi::parse_ws_pcdata) & ~pugi::parse_eol);
  if (!parsed) {
    i.doc.reset();
    return false;
  }
  i.keepAround(musicXml);

  // The notes in the file's order. Ids of their own for the engraver, which
  // takes a note's id from the file: "mpn1", "mpn2", ... never clash with a
  // file's, since those are put back once the copy is made.
  for (const auto& x : i.doc.select_nodes("//note")) i.notes.push_back(x.node());
  for (size_t k = 0; k < i.notes.size(); ++k) {
    std::string id = "mpn" + std::to_string(k + 1);
    i.byId.emplace(id, k);
    noteIds_.push_back(std::move(id));
  }

  // The marks, each belonging to the note after it in its measure; one
  // written after the measure's last note belongs to that note.
  std::vector<pugi::xml_node> taken;
  for (const auto& x : i.doc.select_nodes("//direction")) {
    const auto direction = x.node();
    const auto other = otherDirectionOf(direction);
    if (!other) continue;
    const std::string body = other.child_value();
    const std::string head = firstLine(body);
    if (head != kNoteTag && head != kRegistrationTag) continue;
    pugi::xml_node note;
    for (auto n = direction.next_sibling(); n && !note; n = n.next_sibling())
      if (std::string(n.name()) == "note") note = n;
    for (auto n = direction.previous_sibling(); n && !note; n = n.previous_sibling())
      if (std::string(n.name()) == "note") note = n;
    taken.push_back(direction);
    if (!note) continue;
    size_t index = 0;
    while (index < i.notes.size() && i.notes[index] != note) ++index;
    if (index == i.notes.size()) continue;
    ScoreMark m;
    m.note = noteIds_[index];
    if (head == kNoteTag) {
      m.kind = ScoreMark::Kind::Note;
      m.text = afterFirstLine(body);
    } else {
      m.kind = ScoreMark::Kind::Registration;
      const std::string rest = afterFirstLine(body);
      m.text = firstLine(rest);
      m.states = afterFirstLine(rest);
    }
    marks.push_back(std::move(m));
  }
  // Out of the score, with the line each stood on.
  for (auto& direction : taken) {
    auto parent = direction.parent();
    if (const auto before = direction.previous_sibling(); blank(before)) parent.remove_child(before);
    parent.remove_child(direction);
  }

  // The copy for the engraver, the file's own ids put back after it.
  std::vector<std::pair<bool, std::string>> kept;
  kept.reserve(i.notes.size());
  for (size_t k = 0; k < i.notes.size(); ++k) {
    auto& n = i.notes[k];
    auto a = n.attribute("id");
    kept.emplace_back(static_cast<bool>(a), a.value());
    if (!a) a = n.append_attribute("id");
    a.set_value(noteIds_[k].c_str());
  }
  engraving_ = i.text();
  for (size_t k = 0; k < i.notes.size(); ++k) {
    auto& n = i.notes[k];
    if (kept[k].first) n.attribute("id").set_value(kept[k].second.c_str());
    else n.remove_attribute("id");
  }
  return true;
}

std::string ScoreMarks::save() const {
  Impl& i = *impl_;
  if (i.notes.empty() && !i.doc.first_child()) return engraving_;
  std::vector<std::pair<pugi::xml_node, pugi::xml_node>> added;  // the parent, the node
  for (const auto& m : marks) {
    const auto found = i.byId.find(m.note);
    if (found == i.byId.end()) continue;
    const auto note = i.notes[found->second];
    // A chord's notes start together, and nothing may come between them: the
    // mark goes before the chord's first.
    auto head = note;
    while (head.child("chord")) {
      auto prev = head.previous_sibling("note");
      if (!prev) break;
      head = prev;
    }
    auto parent = head.parent();
    auto direction = parent.insert_child_before("direction", head);
    direction.append_attribute("placement").set_value("above");
    auto other = direction.append_child("direction-type").append_child("other-direction");
    other.append_attribute("print-object").set_value("no");
    std::string body;
    if (m.kind == ScoreMark::Kind::Note) {
      body = std::string(kNoteTag) + "\n" + m.text;
    } else {
      std::string name = m.text;
      for (auto& c : name)
        if (c == '\n' || c == '\r') c = ' ';
      body = std::string(kRegistrationTag) + "\n" + name + "\n" + m.states;
    }
    other.append_child(pugi::node_pcdata).set_value(body.c_str());
    if (const auto staff = note.child("staff")) direction.append_copy(staff);
    added.emplace_back(parent, direction);
    // On a line of its own, indented as the note is.
    if (const auto before = direction.previous_sibling(); blank(before))
      added.emplace_back(parent, parent.insert_copy_after(before, direction));
  }
  std::string out = i.text();
  for (auto it = added.rbegin(); it != added.rend(); ++it) it->first.remove_child(it->second);
  return out;
}

}  // namespace mp
