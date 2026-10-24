/* Destiny Draw for Tag Force 1 - the rules, with no game access (unit-tested on Linux: tools/test/destiny/run_tests.sh).
 *
 * Rule (from Tag Force 2+, docs/DESTINY_DRAW_RESEARCH.md): available to a side while its LP is below a threshold (4000), the opponent controls
 * at least one monster and the side controls no monster with higher ATK than the opponent's strongest. Once per duel. Effect: a random Destiny Card
 * from the side's deck is put on top of the deck, so the next normal draw takes it.
 *
 * Duel memory (European build ULES-00600): player record p at 0x09f79cec + p*0xfac; LP +0x00, hand count +0x0c, deck count +0x10, hand entries
 * +0x120 (u32, card id in the low 13 bits). The DECK ORDER array at +0x3a0 (u32 entries, same format, index 0 = top) was observed in Tag Force 2 USA;
 * for Tag Force 1 it is an ASSUMPTION that the plugin verifies after the first use (VerifyDraw) and stops writing if it is wrong. */
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <set>

namespace dd {

constexpr uint32_t kPlayer = 0x09f79cec, kStride = 0xfac, kSlots = 0x09f79d1c, kSlotStride = 0x14;
constexpr uint32_t kHandCount = 0x0c, kDeckCount = 0x10, kHand = 0x120, kDeck = 0x3a0;
constexpr uint32_t kMaxDeck = 80, kMaxHand = 12;
inline uint32_t card_id(uint32_t entry) { return entry & 0x1fffu; }
inline uint32_t player(int p) { return kPlayer + (uint32_t)p * kStride; }

struct Memory {
    virtual ~Memory() {}
    virtual uint32_t r32(uint32_t a) const = 0;
    virtual void w32(uint32_t a, uint32_t v) = 0;
};

struct Deck { bool ok = false; std::string why; uint32_t count = 0; std::vector<uint32_t> entries; };

/* Reads side p's deck order and refuses anything that does not look like a deck. */
inline Deck read_deck(const Memory &m, int p) {
    Deck d; const uint32_t b = player(p);
    const uint32_t hand = m.r32(b + kHandCount); d.count = m.r32(b + kDeckCount);
    if (hand > kMaxHand) { d.why = "hand count " + std::to_string(hand) + " is not plausible"; return d; }
    if (d.count == 0) { d.why = "the deck is empty"; return d; }
    if (d.count > kMaxDeck) { d.why = "deck count " + std::to_string(d.count) + " is not plausible"; return d; }
    for (uint32_t i = 0; i < d.count; i++) {
        const uint32_t e = m.r32(b + kDeck + 4 * i);
        if (card_id(e) == 0) { d.why = "deck entry " + std::to_string(i) + " has no card id (layout differs?)"; d.entries.clear(); return d; }
        d.entries.push_back(e);
    }
    d.ok = true; return d;
}

struct Board { int lp = 0, opp_monsters = 0, my_best_atk = -1, opp_best_atk = -1; };

struct Rules { int lp_below = 4000; };
struct Verdict { bool ok = false; std::string why; };

inline Verdict available(const Board &b, bool used, const Rules &r) {
    Verdict v;
    if (used) { v.why = "already used this duel"; return v; }
    if (b.lp >= r.lp_below) { v.why = "LP " + std::to_string(b.lp) + " is not below " + std::to_string(r.lp_below); return v; }
    if (b.opp_monsters < 1) { v.why = "the opponent controls no monster"; return v; }
    if (b.my_best_atk > b.opp_best_atk) { v.why = "you control a monster with higher ATK (" + std::to_string(b.my_best_atk) + " > " + std::to_string(b.opp_best_atk) + ")"; return v; }
    v.ok = true; return v;
}

/* Index in the deck of a randomly chosen Destiny Card, or -1. `seed` makes it deterministic for a given frame. */
inline int pick(const Deck &d, const std::set<uint32_t> &destiny, uint32_t seed) {
    std::vector<int> idx; for (size_t i = 0; i < d.entries.size(); i++) if (destiny.count(card_id(d.entries[i]))) idx.push_back((int)i);
    if (idx.empty()) return -1;
    uint32_t x = seed * 2654435761u + 0x9e3779b9u; x ^= x >> 15; x *= 0x85ebca6bu; x ^= x >> 13;
    return idx[x % idx.size()];
}

/* Moves deck entry k to the top (index 0); the entries above it move down by one. The deck count and the hand do not change. */
inline bool move_to_top(Memory &m, int p, const Deck &d, int k) {
    if (!d.ok || k < 0 || (uint32_t)k >= d.count) return false;
    const uint32_t b = player(p) + kDeck, chosen = d.entries[(size_t)k];
    for (int i = k; i > 0; i--) m.w32(b + 4 * (uint32_t)i, d.entries[(size_t)i - 1]);
    m.w32(b, chosen);
    return true;
}

/* After a use: when the hand grows by one, the new last hand entry must be the card that was put on top. */
enum class Check { Waiting, Confirmed, Mismatch };
inline Check verify_draw(const Memory &m, int p, uint32_t hand_before, uint32_t expected_id, uint32_t *got = nullptr) {
    const uint32_t b = player(p), hand = m.r32(b + kHandCount);
    if (hand <= hand_before || hand > kMaxHand) return Check::Waiting;
    const uint32_t id = card_id(m.r32(b + kHand + 4 * (hand - 1)));
    if (got) *got = id;
    return id == expected_id ? Check::Confirmed : Check::Mismatch;
}

/* "1234, 5678 ,99" -> ids; anything else is ignored. */
inline std::set<uint32_t> parse_ids(const std::string &s) {
    std::set<uint32_t> out; uint32_t v = 0; bool any = false;
    for (char c : s + ",") {
        if (c >= '0' && c <= '9') { v = v * 10 + (uint32_t)(c - '0'); any = true; if (v > 0x1fff) v = 0x2000; }
        else { if (any && v >= 1 && v <= 0x1fff) out.insert(v); v = 0; any = false; }
    }
    return out;
}
/* "Blue-Eyes White Dragon; Dark Magician" -> lower-case trimmed names. */
inline std::vector<std::wstring> parse_names(const std::wstring &s) {
    std::vector<std::wstring> out; std::wstring cur;
    auto flush = [&] { size_t a = cur.find_first_not_of(L" \t"), z = cur.find_last_not_of(L" \t\r"); if (a != std::wstring::npos) { std::wstring t = cur.substr(a, z - a + 1); for (auto &ch : t) if (ch >= L'A' && ch <= L'Z') ch = ch - L'A' + L'a'; out.push_back(t); } cur.clear(); };
    for (wchar_t c : s) { if (c == L';') flush(); else cur += c; }
    flush(); return out;
}
inline std::wstring lower(std::wstring s) { for (auto &ch : s) if (ch >= L'A' && ch <= L'Z') ch = ch - L'A' + L'a'; return s; }

}  // namespace dd
