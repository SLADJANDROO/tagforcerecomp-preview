/* Unit tests for plugins/aicoach_core/game_view.* (Milestone 3b). The "game" is a sparse fake memory image built from the layout documented in game_view.h. */
#include "game_view.h"
#include <stdio.h>
#include <map>
using namespace aicoach;

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define TEST(name) static void name(); struct R_##name { R_##name() { tests().push_back({#name, name}); } } r_##name; static void name()
struct T { const char *n; void (*f)(); };
static std::vector<T> &tests() { static std::vector<T> v; return v; }

struct FakeMem : GuestMemory {
    std::map<uint32_t, uint32_t> w;                         /* 32-bit words, little endian */
    uint32_t u32(uint32_t a) const override { auto i = w.find(a); return i == w.end() ? 0 : i->second; }
    uint16_t u16(uint32_t a) const override { return (uint16_t)(u32(a & ~3u) >> ((a & 2) ? 16 : 0)); }
    void set32(uint32_t a, uint32_t v) { w[a] = v; }
    void set16(uint32_t a, uint16_t v) { uint32_t o = u32(a & ~3u); o = (a & 2) ? (o & 0xffffu) | ((uint32_t)v << 16) : (o & 0xffff0000u) | v; w[a & ~3u] = o; }
    static uint32_t base(int p) { return 0x09f79cec + (uint32_t)p * 0xfac; }
    static uint32_t slot(int p, int i) { return 0x09f79d1c + (uint32_t)p * 0xfac + (uint32_t)i * 0x14; }
    void duel(int lp0, int lp1) { set32(base(0), (uint32_t)lp0); set32(base(1), (uint32_t)lp1); }
    void hand(int p, std::initializer_list<uint16_t> ids) { int i = 0; for (auto id : ids) set16(base(p) + 0x120 + (uint32_t)i++ * 4, id); set32(base(p) + 0x0c, (uint32_t)i); }
    void put(int p, int i, uint16_t id, uint32_t flags = 0x80) { set16(slot(p, i), id); set32(slot(p, i) + 0x10, flags); }
};

TEST(reads_hand_field_and_life_points_of_the_acting_side) {
    FakeMem m; m.duel(8000, 6500); m.hand(0, {1001, 1002}); m.hand(1, {2001}); m.put(0, 0, 3001); m.put(0, 6, 3002); m.put(0, 10, 3003); m.put(1, 0, 4001); m.put(1, 2, 4002);
    GameDuelView v(m, 0);
    CHECK(v.plausible());
    CHECK(v.has_card(Where::Hand, 1001) == Tri::Yes); CHECK(v.has_card(Where::Hand, 2001) == Tri::No);        /* the other side's hand is not "mine" */
    CHECK(v.has_card(Where::Field, 3001) == Tri::Yes); CHECK(v.has_card(Where::Field, 3002) == Tri::Yes); CHECK(v.has_card(Where::Field, 3003) == Tri::Yes);
    CHECK(v.has_card(Where::Field, 4001) == Tri::No);
    CHECK(v.life_points(true) == 8000); CHECK(v.life_points(false) == 6500); CHECK(v.opponent_monster_count() == 2);
    GameDuelView o(m, 1); CHECK(o.life_points(true) == 6500); CHECK(o.has_card(Where::Hand, 2001) == Tri::Yes); CHECK(o.opponent_monster_count() == 1);
}
TEST(positions_and_free_zones) {
    FakeMem m; m.duel(8000, 8000); m.put(0, 0, 3001, 0x80); m.put(0, 1, 3002, 0x80 | 0x08); m.put(0, 2, 3003, 0x00); m.put(0, 5, 3004);
    GameDuelView v(m, 0);
    auto is = [&](uint32_t c, const char *p) { return v.monster_position(c, p) == Tri::Yes; };
    CHECK(is(3001, "attack") && !is(3001, "defense")); CHECK(is(3002, "defense")); CHECK(is(3003, "facedown_defense"));
    CHECK(v.monster_position(9999, "attack") == Tri::No);                       /* not on the field */
    CHECK(v.monster_position(3004, "attack") == Tri::No);                       /* a spell/trap is not a monster */
    CHECK(v.free_zones("monster") == 2); CHECK(v.free_zones("spelltrap") == 4); CHECK(v.free_zones("nonsense") == -1);
}
TEST(ids_are_masked_to_13_bits) {
    FakeMem m; m.duel(8000, 8000); m.hand(0, {(uint16_t)(0xE000 | 1234)}); m.put(0, 0, (uint16_t)(0xA000 | 777));
    GameDuelView v(m, 0); CHECK(v.has_card(Where::Hand, 1234) == Tri::Yes); CHECK(v.has_card(Where::Field, 777) == Tri::Yes);
}
TEST(what_the_game_does_not_expose_is_unknown_not_guessed) {
    FakeMem m; m.duel(8000, 8000); m.hand(0, {1001}); GameDuelView v(m, 0);
    CHECK(v.has_card(Where::Graveyard, 1001) == Tri::Unknown); CHECK(v.has_card(Where::Extra, 1001) == Tri::Unknown);
    CHECK(v.phase().empty()); CHECK(v.turn() == 0); Step s; s.action = "normal_summon"; s.card = 1001; CHECK(v.action_legal(s) == Tri::Unknown);
    CHECK(v.has_card(Where::Hand, 0) == Tri::Unknown);
}
TEST(memory_that_is_not_a_duel_answers_unknown) {
    FakeMem m; m.duel(8000, 8000); m.set32(FakeMem::base(0) + 0x0c, 500);       /* absurd hand count: stale or foreign memory */
    GameDuelView v(m, 0); CHECK(!v.plausible());
    CHECK(v.has_card(Where::Hand, 1) == Tri::Unknown); CHECK(v.life_points(true) == -1); CHECK(v.free_zones("monster") == -1); CHECK(v.opponent_monster_count() == -1);
    FakeMem lp; lp.duel(5000000, 8000); CHECK(!GameDuelView(lp, 0).plausible());
    FakeMem neg; neg.duel(-300, 8000); GameDuelView n(neg, 0); CHECK(n.plausible()); CHECK(n.life_points(true) == 0);   /* a lethal hit can leave LP below zero */
}
TEST(turn_owner) {
    FakeMem m; m.duel(8000, 8000); m.set32(0x09f7d460, 1); CHECK(!GameDuelView(m, 0).my_turn()); CHECK(GameDuelView(m, 1).my_turn());
}
TEST(works_as_the_engines_view) {   /* end to end with the ranking code: the real view drives eligibility */
    FakeMem m; m.duel(8000, 8000); m.hand(0, {1001}); GameDuelView v(m, 0); AttemptLedger l; SelectionContext ctx;
    Combo c; c.id = "c"; c.name = "c"; Step s; s.id = "s1"; s.action = "normal_summon"; s.card = 1001; c.steps = {s}; c.required_cards = {1001};
    CHECK(rank_combos({c}, ctx, v, l)[0].eligible);
    FakeMem empty; empty.duel(8000, 8000); GameDuelView ve(empty, 0); CHECK(!rank_combos({c}, ctx, ve, l)[0].eligible);
}

int main() {
    for (auto &t : tests()) { int before = g_fail; g_run++; t.f(); printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", t.n); }
    printf("%d tests, %d failed checks\n", g_run, g_fail); return g_fail ? 1 : 0;
}
