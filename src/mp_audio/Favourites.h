// Favourites: the organs, temperaments and combination sets a player actually
// uses, on numbered slots they can reach without a file dialog.
//
// The point is not bookmarking. On a console the player is standing at a
// keyboard with both hands busy, and "open a file browser and find the 19 GB
// set again" is not a thing that can happen between two pieces. A slot number
// is something a thumb piston can be mapped to; a path is not.
//
// Sixty-four of each, which is the number a real console offers and is also
// about the point where a numbered list stops being faster than reading.
//
// Deliberately JUCE-free and header-only: this is a list of strings, and the
// fast loop should be able to test its ordering and bounds without audio.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace mp {

inline constexpr int kFavouriteSlots = 64;

enum class FavouriteKind { Organ, Temperament, CombinationSet };

// How a list of favourites is shown. The slots stay as they are either way: a
// piston is mapped to a slot.
enum class FavouriteSort { Slot = 0, LastUsed = 1, MostUsed = 2 };

struct Favourite {
  // What the player sees. Free text, because the file name of an organ is
  // rarely what its player calls it.
  std::string name;
  // What it resolves to: a path for an organ, a name for a temperament or a
  // combination set.
  std::string target;
  // How often it was opened, and when last, in seconds since 1970.
  int uses = 0;
  long long lastUsed = 0;

  bool empty() const { return target.empty(); }
};

// One numbered bank per kind. Slots are 1..64 as a player counts them, and an
// unset slot is empty rather than absent — the gaps are part of the layout,
// and renumbering to close them would move everything a player had learned.
class FavouriteBank {
 public:
  FavouriteBank() { slots_.resize(kFavouriteSlots); }

  bool valid(int slot) const { return slot >= 1 && slot <= kFavouriteSlots; }

  const Favourite& at(int slot) const {
    static const Favourite none;
    return valid(slot) ? slots_[static_cast<size_t>(slot - 1)] : none;
  }

  void set(int slot, Favourite f) {
    if (!valid(slot)) return;
    slots_[static_cast<size_t>(slot - 1)] = std::move(f);
  }

  void clear(int slot) {
    if (valid(slot)) slots_[static_cast<size_t>(slot - 1)] = Favourite{};
  }

  // The lowest free slot, or 0 when the bank is full. What "add this one"
  // means when the player has not said where.
  int firstFree() const {
    for (int i = 1; i <= kFavouriteSlots; ++i)
      if (at(i).empty()) return i;
    return 0;
  }

  // Slots that hold something, in order. For drawing a list without 64 rows of
  // nothing.
  std::vector<int> used() const {
    std::vector<int> out;
    for (int i = 1; i <= kFavouriteSlots; ++i)
      if (!at(i).empty()) out.push_back(i);
    return out;
  }

  int count() const { return static_cast<int>(used().size()); }

  // The slots that hold something, as shown. Ties stay in slot order.
  std::vector<int> ordered(FavouriteSort sort) const {
    auto out = used();
    if (sort == FavouriteSort::LastUsed)
      std::stable_sort(out.begin(), out.end(), [this](int a, int b) { return at(a).lastUsed > at(b).lastUsed; });
    else if (sort == FavouriteSort::MostUsed)
      std::stable_sort(out.begin(), out.end(), [this](int a, int b) { return at(a).uses > at(b).uses; });
    return out;
  }

  // The favourite on `slot` taken to `place` among the used slots, counted
  // from 0, those between closing up. The gaps a player left stay where they
  // are. False when nothing moved.
  bool moveToPlace(int slot, int place) {
    const auto slots = used();
    const auto from = std::find(slots.begin(), slots.end(), slot);
    if (from == slots.end() || slots.empty()) return false;
    const int a = static_cast<int>(from - slots.begin());
    const int b = std::clamp(place, 0, static_cast<int>(slots.size()) - 1);
    if (a == b) return false;
    std::vector<Favourite> held;
    for (int s : slots) held.push_back(at(s));
    if (a < b) std::rotate(held.begin() + a, held.begin() + a + 1, held.begin() + b + 1);
    else std::rotate(held.begin() + b, held.begin() + a, held.begin() + a + 1);
    for (size_t i = 0; i < slots.size(); ++i) set(slots[i], held[i]);
    return true;
  }

  // One more opening of `target`, if it is here.
  void markUsed(const std::string& target, long long now) {
    if (const int slot = slotOf(target); slot != 0) {
      auto& fav = slots_[static_cast<size_t>(slot - 1)];
      ++fav.uses;
      fav.lastUsed = now;
    }
  }

  // Already here? Answering by TARGET, not by name: the same organ added
  // twice under two names is the same organ, and a second copy is a way to
  // wonder later which slot is the real one.
  int slotOf(const std::string& target) const {
    for (int i = 1; i <= kFavouriteSlots; ++i)
      if (!at(i).empty() && at(i).target == target) return i;
    return 0;
  }

 private:
  std::vector<Favourite> slots_;
};

struct Favourites {
  FavouriteBank organs;
  FavouriteBank temperaments;
  FavouriteBank combinationSets;

  FavouriteBank& bank(FavouriteKind k) {
    switch (k) {
      case FavouriteKind::Temperament: return temperaments;
      case FavouriteKind::CombinationSet: return combinationSets;
      case FavouriteKind::Organ: break;
    }
    return organs;
  }
  const FavouriteBank& bank(FavouriteKind k) const {
    return const_cast<Favourites*>(this)->bank(k);
  }

  static const char* kindKey(FavouriteKind k) {
    switch (k) {
      case FavouriteKind::Temperament: return "temperament";
      case FavouriteKind::CombinationSet: return "combinationset";
      case FavouriteKind::Organ: break;
    }
    return "organ";
  }
  // Unknown text resolves to Organ rather than being rejected: a settings file
  // from a newer build should lose one favourite, not fail to load.
  static FavouriteKind kindFromKey(const std::string& key) {
    if (key == "temperament") return FavouriteKind::Temperament;
    if (key == "combinationset") return FavouriteKind::CombinationSet;
    return FavouriteKind::Organ;
  }
};

}  // namespace mp
