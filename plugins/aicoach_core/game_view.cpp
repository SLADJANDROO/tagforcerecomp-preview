#include "game_view.h"

namespace aicoach {

static const uint32_t kPlayer = 0x09f79cec, kStride = 0xfac, kSlots = 0x09f79d1c, kTurn = 0x09f7d460;
static const int kMonsterZones = 5, kSpellTrapZones = 5, kFieldSlot = 10, kMaxHand = 12;

uint32_t GameDuelView::base(int p) const { return kPlayer + (uint32_t)(p & 1) * kStride; }
uint32_t GameDuelView::slot(int p, int i) const { return kSlots + (uint32_t)(p & 1) * kStride + (uint32_t)i * 0x14; }
uint32_t GameDuelView::slot_id(int p, int i) const { return m_.u16(slot(p, i)) & 0x1fffu; }

bool GameDuelView::plausible() const {
    for (int p = 0; p < 2; p++) {
        if (m_.u32(base(p) + 0x0c) > (uint32_t)kMaxHand) return false;
        if (m_.u32(base(p) + 0x10) > 80u) return false;
        const int32_t lp = (int32_t)m_.u32(base(p));
        if (lp < -1000000 || lp > 1000000) return false;
    }
    return true;
}
bool GameDuelView::my_turn() const { return (m_.u32(kTurn) & 1) == (uint32_t)self_; }

Tri GameDuelView::has_card(Where w, uint32_t card) const {
    if (!plausible() || card == 0) return Tri::Unknown;
    if (w == Where::Graveyard || w == Where::Extra) return Tri::Unknown;   /* location in memory not known */
    if (w == Where::Hand) {
        const uint32_t n = m_.u32(base(self_) + 0x0c);
        for (uint32_t i = 0; i < n; i++) if ((m_.u16(base(self_) + 0x120 + i * 4) & 0x1fffu) == card) return Tri::Yes;
        return Tri::No;
    }
    for (int i = 0; i <= kFieldSlot; i++) if (slot_id(self_, i) == card) return Tri::Yes;
    return Tri::No;
}

Tri GameDuelView::monster_position(uint32_t card, const std::string &position) const {
    if (!plausible() || card == 0) return Tri::Unknown;
    for (int i = 0; i < kMonsterZones; i++) {
        if (slot_id(self_, i) != card) continue;
        const uint32_t f = m_.u32(slot(self_, i) + 0x10);
        const bool up = (f & 0x80u) != 0, def = (f & 0x08u) != 0;
        const char *now = !up ? "facedown_defense" : def ? "defense" : "attack";
        return tri_of(position == now);
    }
    return Tri::No;
}

int GameDuelView::free_zones(const std::string &zone) const {
    if (!plausible()) return -1;
    const int first = zone == "monster" ? 0 : zone == "spelltrap" ? kMonsterZones : -1;
    if (first < 0) return -1;
    const int count = zone == "monster" ? kMonsterZones : kSpellTrapZones;
    int free = 0; for (int i = first; i < first + count; i++) if (slot_id(self_, i) == 0) free++;
    return free;
}

int GameDuelView::life_points(bool self) const {
    if (!plausible()) return -1;
    const int32_t lp = (int32_t)m_.u32(base(self ? self_ : 1 - self_));
    return lp < 0 ? 0 : (int)lp;
}

int GameDuelView::opponent_monster_count() const {
    if (!plausible()) return -1;
    int n = 0; for (int i = 0; i < kMonsterZones; i++) if (slot_id(1 - self_, i) != 0) n++;
    return n;
}

}  // namespace
