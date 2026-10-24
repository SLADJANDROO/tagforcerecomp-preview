/* Tests for plugins/src/DestinyDraw: the rules (destiny_logic.h) and the REAL plugin source against a fake TfModApi + 32 MB fake RAM.
 * The fake RAM uses the same layout the plugin assumes, so this cannot prove the layout matches Tag Force 1 - the in-game check (CONFIRMED /
 * MISMATCH in the log) does that. Run: sh tools/test/destiny/run_tests.sh */
#include "../../../plugins/src/DestinyDraw/destiny_draw.cpp"
#include <stdarg.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
struct CPU { uint32_t r[32]; };
static CPU g_cpu; static uint8_t *g_ram;
static std::map<uint32_t, TfHook> g_hooks; static std::vector<std::string> g_log;
static std::vector<std::wstring> g_panel, g_box; static uint64_t g_frame; static bool g_loaded = true; static uint32_t g_buttons;
static void (*g_key)(int); static int g_vk; static std::wstring g_dir; static uint32_t g_alloc = 0x09ff0000;
static std::map<std::pair<int,int>, int> g_atk;   /* (side, slot) -> ATK for the fake stats function */

static uint8_t *f_mem(uint32_t a) { return g_ram + (a - 0x08000000u); }
static uint32_t f_reg(TfCpu *c, int i) { return ((CPU *)c)->r[i]; }
static void f_set_reg(TfCpu *c, int i, uint32_t v) { if (i > 0) ((CPU *)c)->r[i] = v; }
static void f_hook(uint32_t a, TfHook fn) { g_hooks[a] = fn; }
static void W32(uint32_t a, uint32_t v) { memcpy(f_mem(a), &v, 4); }
static uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, f_mem(a), 4); return v; }
static void f_call(TfCpu *c, uint32_t addr) {
    CPU *k = (CPU *)c;
    if (addr == 0x0884b46c) k->r[2] = 0x08500000u + k->r[4] * 64u;
    else if (addr == 0x09f56da8) { W32(k->r[6] + 20, (uint32_t)g_atk[{(int)k->r[4], (int)k->r[5]}]); k->r[2] = 1; }
    k->r[4] = k->r[5] = k->r[6] = 0xdead;   /* a real call clobbers argument registers: the plugin must restore them */
}
static uint32_t f_buttons() { return g_buttons; }
static uint64_t f_frame() { return g_frame; }
static int f_loaded(const char *n) { return g_loaded && !strcmp(n, "libduel_eng"); }
static void f_log(const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_log.push_back(b); }
static void f_panel(int, const wchar_t *, const wchar_t *const *l, int n) { g_panel.clear(); for (int i = 0; i < n; i++) g_panel.push_back(l[i]); }
static void f_box(const wchar_t *t, const wchar_t *const *l, int n, const wchar_t *) { g_box.clear(); g_box.push_back(t); for (int i = 0; i < n; i++) g_box.push_back(l[i]); }
static void f_toggle(const wchar_t *, int vk, int (*)(void), void (*set)(int)) { g_vk = vk; g_key = set; }
static uint32_t f_alloc(uint32_t n) { uint32_t a = g_alloc; g_alloc += (n + 15) & ~15u; return a; }

static bool logged(const char *s) { for (auto &l : g_log) if (l.find(s) != std::string::npos) return true; return false; }
static bool box_has(const wchar_t *s) { for (auto &l : g_box) if (l.find(s) != std::wstring::npos) return true; return false; }
static bool panel_has(const wchar_t *s) { for (auto &l : g_panel) if (l.find(s) != std::wstring::npos) return true; return false; }
static uint32_t PB(int p) { return 0x09f79cec + (uint32_t)p * 0xfac; }
static void frames(int n) { for (int i = 0; i < n; i++) { g_frame++; g_panel.clear(); g_box.clear(); g_hooks[0x08833fc4]((TfCpu *)&g_cpu); } }
static void set_deck(int p, std::vector<uint32_t> ids) { for (size_t i = 0; i < ids.size(); i++) W32(PB(p) + 0x3a0 + 4 * (uint32_t)i, (uint32_t)((i + 1) << 22) | ids[i]); W32(PB(p) + 0x10, (uint32_t)ids.size()); }
static std::vector<uint32_t> deck_ids(int p) { std::vector<uint32_t> v; for (uint32_t i = 0; i < R32(PB(p) + 0x10); i++) v.push_back(R32(PB(p) + 0x3a0 + 4 * i) & 0x1fff); return v; }
static void monster(int p, int s, uint32_t id, int atk) { W32(0x09f79d1c + (uint32_t)p * 0xfac + (uint32_t)s * 0x14, id); g_atk[{p, s}] = atk; }
static void draw(int p) { const uint32_t h = R32(PB(p) + 0x0c), top = R32(PB(p) + 0x3a0); W32(PB(p) + 0x120 + 4 * h, top); W32(PB(p) + 0x0c, h + 1);
    const uint32_t n = R32(PB(p) + 0x10); for (uint32_t i = 0; i + 1 < n; i++) W32(PB(p) + 0x3a0 + 4 * i, R32(PB(p) + 0x3a0 + 4 * (i + 1))); W32(PB(p) + 0x10, n - 1); }
static void name(uint32_t id, const std::wstring &n) { for (size_t i = 0; i <= n.size(); i++) { uint16_t ch = i < n.size() ? (uint16_t)n[i] : 0; memcpy(f_mem(0x08500000u + id * 64u + (uint32_t)i * 2), &ch, 2); } }
static void write_cfg(const std::string &s) { std::ofstream(fs::path(g_dir) / "config.ini", std::ios::trunc) << s; }
/* a fresh duel: side 0 human with LP 2000 and a weaker monster, side 1 CPU with a stronger one; decks of 10 */
static void duel(int lp = 2000) {
    memset(f_mem(0x09f77000), 0, 0x8000); g_atk.clear();
    W32(0x09f77888, 0); W32(0x09f77888 + 4, 1); W32(PB(0), (uint32_t)lp); W32(PB(1), 8000); W32(PB(0) + 0x0c, 5); W32(PB(1) + 0x0c, 5);
    set_deck(0, {101, 102, 103, 104, 105, 106, 107, 108, 109, 110}); set_deck(1, {201, 202, 203, 204, 205});
    monster(0, 0, 301, 1000); monster(1, 2, 401, 2400);
    g_cpu.r[4] = 0x091c3d6c; g_hooks[0x0883f4f8]((TfCpu *)&g_cpu);
}
static void T(const char *n, int before) { printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", n); }

/* ---------------- rules ---------------- */
struct FakeMem : dd::Memory { std::map<uint32_t, uint32_t> m; uint32_t r32(uint32_t a) const override { auto i = m.find(a); return i == m.end() ? 0 : i->second; } void w32(uint32_t a, uint32_t v) override { m[a] = v; } };
static void rules() {
    int b = g_fail;
    dd::Rules r; dd::Board x; x.lp = 2000; x.opp_monsters = 1; x.my_best_atk = 1000; x.opp_best_atk = 2400;
    CHECK(dd::available(x, false, r).ok);
    CHECK(!dd::available(x, true, r).ok);
    dd::Board y = x; y.lp = 4000; CHECK(!dd::available(y, false, r).ok);               /* "below 4000": 4000 itself is not enough */
    y = x; y.opp_monsters = 0; y.opp_best_atk = -1; CHECK(!dd::available(y, false, r).ok);
    y = x; y.my_best_atk = 2500; CHECK(!dd::available(y, false, r).ok);
    y = x; y.my_best_atk = 2400; CHECK(dd::available(y, false, r).ok);                 /* equal ATK is not "higher" */
    y = x; y.my_best_atk = -1; CHECK(dd::available(y, false, r).ok);                   /* no monsters of mine */
    T("availability_follows_the_tag_force_2_rule", b);

    b = g_fail; FakeMem m; const uint32_t P = dd::player(0);
    m.w32(P + 0x0c, 5); m.w32(P + 0x10, 4); for (uint32_t i = 0; i < 4; i++) m.w32(P + 0x3a0 + 4 * i, 0x00400000u * (i + 1) | (101 + i));
    dd::Deck d = dd::read_deck(m, 0); CHECK(d.ok && d.count == 4 && dd::card_id(d.entries[3]) == 104);
    FakeMem bad = m; bad.w32(P + 0x3a0 + 8, 0); CHECK(!dd::read_deck(bad, 0).ok);       /* a hole: layout differs */
    FakeMem big = m; big.w32(P + 0x10, 200); CHECK(!dd::read_deck(big, 0).ok);
    FakeMem empty = m; empty.w32(P + 0x10, 0); CHECK(!dd::read_deck(empty, 0).ok);
    FakeMem hand = m; hand.w32(P + 0x0c, 40); CHECK(!dd::read_deck(hand, 0).ok);
    T("deck_reading_refuses_implausible_memory", b);

    b = g_fail;
    CHECK(dd::pick(d, {103}, 7) == 2); CHECK(dd::pick(d, {999}, 7) == -1);
    std::set<int> seen; for (uint32_t s = 0; s < 200; s++) { int k = dd::pick(d, {101, 104}, s); CHECK(k == 0 || k == 3); seen.insert(k); }
    CHECK(seen.size() == 2); CHECK(dd::pick(d, {101, 104}, 42) == dd::pick(d, {101, 104}, 42));
    T("pick_is_random_among_destiny_cards_only_and_deterministic", b);

    b = g_fail; const uint32_t e2 = d.entries[2];
    CHECK(dd::move_to_top(m, 0, d, 2));
    CHECK(m.r32(P + 0x3a0) == e2); CHECK(m.r32(P + 0x3a4) == d.entries[0]); CHECK(m.r32(P + 0x3a8) == d.entries[1]); CHECK(m.r32(P + 0x3ac) == d.entries[3]);
    CHECK(m.r32(P + 0x10) == 4 && m.r32(P + 0x0c) == 5);                                 /* counts and hand untouched */
    CHECK(!dd::move_to_top(m, 0, d, 4)); CHECK(!dd::move_to_top(m, 0, d, -1));
    T("move_to_top_keeps_every_card_and_the_counts", b);

    b = g_fail; FakeMem v; v.w32(P + 0x0c, 3);
    CHECK(dd::verify_draw(v, 0, 3, 500) == dd::Check::Waiting);
    v.w32(P + 0x0c, 4); v.w32(P + 0x120 + 12, 0x01c00000u | 500); CHECK(dd::verify_draw(v, 0, 3, 500) == dd::Check::Confirmed);
    v.w32(P + 0x120 + 12, 501); uint32_t got = 0; CHECK(dd::verify_draw(v, 0, 3, 500, &got) == dd::Check::Mismatch && got == 501);
    T("the_first_draw_confirms_or_refutes_the_layout", b);

    b = g_fail;
    CHECK(dd::parse_ids("1234, 5678 ,99,0,9000,abc,12") == (std::set<uint32_t>{12, 99, 1234, 5678}));
    auto n = dd::parse_names(L" Blue-Eyes White Dragon ;Dark Magician;; "); CHECK(n.size() == 2 && n[0] == L"blue-eyes white dragon" && n[1] == L"dark magician");
    T("config_lists_are_parsed_strictly", b);
}

int main() {
    g_ram = (uint8_t *)calloc(0x02000000, 1);
    const fs::path dir = fs::temp_directory_path() / "destiny_test"; fs::remove_all(dir); fs::create_directories(dir); g_dir = dir.wstring();
    write_cfg("names=Lucky Star\ncards=103\n");
    name(103, L"Pot of Plenty"); name(107, L"Lucky Star"); name(105, L"Other Card");
    TfModApi api; memset(&api, 0, sizeof api);
    api.version = 6; api.mod_dir = g_dir.c_str(); api.mem = f_mem; api.reg = f_reg; api.set_reg = f_set_reg; api.hook = f_hook; api.call = f_call; api.buttons = f_buttons;
    api.frame = f_frame; api.module_loaded = f_loaded; api.message_box = f_box; api.log = f_log; api.panel = f_panel; api.add_toggle = f_toggle; api.guest_alloc = f_alloc;
    rules();
    int b;

    b = g_fail; CHECK(tfmod_init(&api) == 0); CHECK(g_hooks.count(0x08833fc4) && g_hooks.count(0x0883f4f8)); CHECK(g_vk == 0x71);
    CHECK(logged("Destiny Cards: 1 id(s), 1 name(s)"));
    { TfModApi old = api; old.version = 3; CHECK(tfmod_init(&old) != 0); tfmod_init(&api); }
    T("init_reads_config_hooks_and_refuses_old_runtimes", b);

    b = g_fail; duel(); frames(30);
    CHECK(panel_has(L"Ready")); CHECK(panel_has(L"2 in deck"));                           /* id 103 + name "Lucky Star" (107) */
    T("ready_panel_shows_when_the_rule_holds", b);

    b = g_fail; duel(5000); frames(30); CHECK(!panel_has(L"Ready"));
    const std::vector<uint32_t> before = deck_ids(0);
    g_key(1); frames(1); CHECK(box_has(L"not available")); CHECK(box_has(L"LP 5000")); CHECK(deck_ids(0) == before);
    duel(); monster(0, 1, 302, 3000); g_key(1); frames(1); CHECK(box_has(L"higher ATK")); CHECK(deck_ids(0) == before);
    T("when_the_rule_does_not_hold_nothing_changes_and_it_says_why", b);

    b = g_fail; duel(); const uint32_t sig = R32(0x08500000u + 103 * 64u);
    g_key(1); frames(1);
    std::vector<uint32_t> after = deck_ids(0); CHECK(after.size() == 10);
    CHECK(after[0] == 103 || after[0] == 107); CHECK(box_has(L"DESTINY DRAW!"));
    std::vector<uint32_t> s1 = before, s2 = after; std::sort(s1.begin(), s1.end()); std::sort(s2.begin(), s2.end()); CHECK(s1 == s2);   /* same cards */
    CHECK(R32(PB(0) + 0x0c) == 5 && R32(PB(0) + 0x10) == 10);
    CHECK(deck_ids(1) == (std::vector<uint32_t>{201, 202, 203, 204, 205}));              /* the CPU's deck is never touched */
    CHECK(g_cpu.r[4] == 0x091c3d6c); CHECK(R32(0x08500000u + 103 * 64u) == sig);
    const uint32_t chosen = after[0];
    g_key(1); frames(1); CHECK(box_has(L"already used")); CHECK(deck_ids(0) == after);   /* once per duel */
    frames(30); CHECK(!panel_has(L"Ready"));
    draw(0); frames(1); CHECK(logged("CONFIRMED")); CHECK((R32(PB(0) + 0x120 + 4 * 5) & 0x1fff) == chosen);
    T("use_puts_a_destiny_card_on_top_once_and_the_draw_confirms_it", b);

    b = g_fail; duel(); frames(30); CHECK(panel_has(L"Ready"));                          /* a new duel resets the use */
    T("a_new_duel_allows_it_again", b);

    b = g_fail; duel(); g_key(1); frames(1);
    W32(PB(0) + 0x120 + 4 * 5, 999); W32(PB(0) + 0x0c, 6); frames(1);                     /* the game drew something else: layout is wrong */
    CHECK(logged("MISMATCH")); CHECK(box_has(L"check failed"));
    duel(); const std::vector<uint32_t> d2 = deck_ids(0); g_key(1); frames(1); CHECK(deck_ids(0) == d2); CHECK(box_has(L"is off"));
    frames(30); CHECK(!panel_has(L"Ready"));
    T("a_wrong_layout_disables_every_write_for_the_session", b);
    return g_fail ? 1 : 0;
}
