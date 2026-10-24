/* Harness test for plugins/src/AICoach/aicoach.cpp: the REAL plugin source is compiled here (Linux) against a fake TfModApi with a 32 MB fake PSP RAM, and its hooks are
 * called the way the runtime would. This tests the glue (loading, hooks, panel, logging, "never writes the game") - NOT that the memory layout matches the real game,
 * because the fake RAM is filled using the same documented offsets. Run: sh tools/test/aicoach/run_tests.sh */
#include "../../../plugins/src/AICoach/aicoach.cpp"
#include <stdarg.h>
#include <memory>

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct CPU { uint32_t r[32]; };
static CPU g_cpu;
static uint8_t *g_ram;
static std::map<uint32_t, TfHook> g_hooks;
static std::vector<std::string> g_log;
struct PanelCall { int corner; std::wstring title; std::vector<std::wstring> lines; };
static std::vector<PanelCall> g_panels;
static uint64_t g_frame; static bool g_engine_loaded; static int (*g_toggle_get)(void); static void (*g_toggle_set)(int); static int g_toggle_vk;
static std::wstring g_mod_dir;

static uint8_t *f_mem(uint32_t a) { return g_ram + (a - 0x08000000u); }
static uint32_t f_reg(TfCpu *c, int i) { return ((CPU *)c)->r[i]; }
static void f_set_reg(TfCpu *c, int i, uint32_t v) { if (i > 0) ((CPU *)c)->r[i] = v; }
static void f_hook(uint32_t a, TfHook fn) { g_hooks[a] = fn; }
static void f_call(TfCpu *c, uint32_t addr) {
    if (addr == 0x0884b46c) ((CPU *)c)->r[2] = 0x08500000u + ((CPU *)c)->r[4] * 64u;   /* card name: pre-written UTF-16 in fake RAM */
}
static uint32_t f_buttons() { return 0; }
static uint64_t f_frame() { return g_frame; }
static int f_module_loaded(const char *n) { return g_engine_loaded && !strcmp(n, "libduel_eng"); }
static void f_log(const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); g_log.push_back(b); }
static void f_panel(int corner, const wchar_t *title, const wchar_t *const *lines, int n) { PanelCall p; p.corner = corner; p.title = title ? title : L""; for (int i = 0; i < n; i++) p.lines.push_back(lines[i]); g_panels.push_back(p); }
static void f_toggle(const wchar_t *, int vk, int (*get)(void), void (*set)(int)) { g_toggle_vk = vk; g_toggle_get = get; g_toggle_set = set; }
static void f_nop_msg(const wchar_t *, const wchar_t *const *, int, const wchar_t *) {}

static void w32(uint32_t a, uint32_t v) { memcpy(f_mem(a), &v, 4); }
static void w16(uint32_t a, uint16_t v) { memcpy(f_mem(a), &v, 2); }
static uint32_t PB(int p) { return 0x09f79cec + (uint32_t)p * 0xfac; }
static bool logged(const char *sub) { for (auto &l : g_log) if (l.find(sub) != std::string::npos) return true; return false; }
static int logged_count(const char *sub) { int n = 0; for (auto &l : g_log) if (l.find(sub) != std::string::npos) n++; return n; }
static bool panel_has(const wchar_t *sub) { if (g_panels.empty()) return false; for (auto &l : g_panels.back().lines) if (l.find(sub) != std::wstring::npos) return true; return false; }
static int run_frame(uint64_t f) { g_frame = f; return g_hooks[0x08833fc4](reinterpret_cast<TfCpu *>(&g_cpu)); }
static void set_hand(int p, std::initializer_list<uint16_t> ids) { int i = 0; for (auto id : ids) w16(PB(p) + 0x120 + (uint32_t)i++ * 4, id); w32(PB(p) + 0x0c, (uint32_t)i); }
static void duel_basics() { w32(PB(0), 8000); w32(PB(1), 8000); w32(0x09f77888, 0); w32(0x09f77888 + 4, 1); w32(0x09f7d460, 1); }
static int setup(uint32_t c0, uint32_t d0, uint32_t c1, uint32_t d1) {
    const uint32_t base = 0x091c3d6c; w16(base + 0x120, (uint16_t)c0); w16(base + 0x124, (uint16_t)d0); w16(base + 0x18c + 0x120, (uint16_t)c1); w16(base + 0x18c + 0x124, (uint16_t)d1);
    g_cpu.r[4] = base; return g_hooks[0x0883f4f8](reinterpret_cast<TfCpu *>(&g_cpu));
}
static void write_user(const std::vector<Combo> &cs) {
    UserData u; u.combos = cs; std::string e; fs::path f = fs::path(g_mod_dir) / "user.json"; write_file_atomic(f, serialize_user(u), e);
}
static Combo mk(const char *id, const char *name, int ch, std::vector<int> decks, uint32_t req, int prio) {
    Combo c; c.id = id; c.name = name; c.character = ch; c.decks = decks; c.priority = prio; c.required_cards = {req};
    Step s; s.id = "s1"; s.action = "normal_summon"; s.card = req; c.steps = {s}; return c;
}
static std::string slurp(const fs::path &p) { std::string t; read_file(p, t); return t; }

int main() {
    const size_t RAM = 0x02000000; g_ram = (uint8_t *)calloc(RAM, 1);
    const fs::path dir = fs::temp_directory_path() / "aicoach_plugin_test"; fs::remove_all(dir); fs::create_directories(dir / "data");
    fs::copy_file(fs::path(SRCDIR) / "plugins/src/AICoach/data/defaults.json", dir / "data" / "defaults.json"); g_mod_dir = dir.wstring();
    for (uint32_t id : {1001u, 1002u, 1003u}) { std::wstring n = L"Card " + std::to_wstring(id); for (size_t i = 0; i <= n.size(); i++) w16(0x08500000u + id * 64u + (uint32_t)i * 2, (uint16_t)(i < n.size() ? n[i] : 0)); }
    TfModApi api; memset(&api, 0, sizeof api);
    api.version = 6; api.mod_dir = g_mod_dir.c_str(); api.mem = f_mem; api.reg = f_reg; api.set_reg = f_set_reg; api.hook = f_hook; api.call = f_call; api.buttons = f_buttons;
    api.frame = f_frame; api.module_loaded = f_module_loaded; api.message_box = f_nop_msg; api.log = f_log; api.panel = f_panel; api.add_toggle = f_toggle;
    auto T = [&](const char *name, bool before_fail_count, int before) { printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", name); g_run++; (void)before_fail_count; };
    int b;

    /* ---- init ---- */
    b = g_fail; CHECK(tfmod_init(&api) == 0);
    CHECK(g_hooks.count(0x0883f4f8) && g_hooks.count(0x08833fc4)); CHECK(g_toggle_vk == 0x73 && g_toggle_get && g_toggle_get() == 1);
    CHECK(logged("loaded 4 profile(s), 0 combo(s)")); CHECK(logged("does not change how the CPU plays"));
    api.version = 3; { auto *saved = A; CHECK(tfmod_init(&api) == 2); A = saved; } api.version = 6;   /* refuses an old runtime */
    T("init_loads_defaults_installs_hooks_and_refuses_old_runtimes", false, b);

    /* ---- selection shown in the panel ---- */
    b = g_fail; duel_basics(); set_hand(1, {1001}); g_engine_loaded = true;
    CHECK(setup(8, 37, 2, 1) == 0); CHECK(logged("side 1 = character 2, deck 1")); CHECK(logged("0 enabled combo(s) apply"));
    write_user({mk("main", "Main line", 2, {1}, 1001, 300), mk("shared", "Shared line", 0, {}, 1002, 250), mk("other", "Other char", 3, {}, 1001, 900)});
    const std::vector<uint8_t> before(g_ram, g_ram + RAM);          /* snapshot: from here on the plugin must not write the game */
    CHECK(run_frame(120) == 0); CHECK(logged("user.json changed: reloading")); CHECK(logged("loaded 4 profile(s), 3 combo(s)"));
    CHECK(run_frame(150) == 0);
    CHECK(logged("would select 'main' (score 300)"));
    CHECK(!g_panels.empty() && g_panels.back().corner == 2 && g_panels.back().title.find(L"AI Coach") != std::wstring::npos);
    CHECK(panel_has(L"Main line")); CHECK(panel_has(L"shared: required card 1002 is not in")); CHECK(panel_has(L"other: belongs to another character")); CHECK(panel_has(L"1001 Card 1001"));
    for (int f = 151; f < 180; f++) CHECK(run_frame((uint64_t)f) == 0);
    CHECK(logged_count("would select 'main'") == 1);                // unchanged state: no repeated log lines
    T("observer_ranks_the_cpu_side_and_explains_in_the_panel", false, b);

    b = g_fail; set_hand(1, {1002}); CHECK(run_frame(210) == 0);
    CHECK(logged("would select 'shared'")); CHECK(panel_has(L"main: required card 1001 is not in"));
    set_hand(1, {1003}); run_frame(240); CHECK(logged("no eligible combo -> the normal AI plays"));
    T("selection_follows_the_hand_and_falls_back_to_the_normal_ai", false, b);

    b = g_fail; setup(8, 37, 999, 999); set_hand(1, {1002}); size_t np = g_panels.size(); run_frame(270);
    CHECK(logged("character 999, deck 999")); CHECK(logged("(no profile: generic)")); CHECK(logged("would select 'shared'")); CHECK(g_panels.size() > np);
    T("unknown_character_and_deck_use_shared_combos_only", false, b);

    b = g_fail; w32(0x09f77888 + 4, 0); np = g_panels.size(); run_frame(300); run_frame(301); CHECK(g_panels.size() == np);          // human turn: nothing drawn
    w32(0x09f77888 + 4, 1); g_engine_loaded = false; np = g_panels.size(); CHECK(run_frame(330) == 0); CHECK(g_panels.size() == np);   // not in a duel
    g_engine_loaded = true; w32(PB(1) + 0x0c, 500); CHECK(run_frame(360) == 0);                                                       // garbage memory: no crash
    w32(PB(1) + 0x0c, 1);
    T("no_panel_on_human_turns_outside_duels_or_on_garbage_memory", false, b);

    b = g_fail; setup(8, 37, 2, 1); set_hand(1, {1001}); run_frame(390);
    const std::string user_before = slurp(fs::path(g_mod_dir) / "user.json");
    { std::ofstream f(fs::path(g_mod_dir) / "user.json", std::ios::trunc); f << "{{ not json at all, but long enough to change size"; }
    const std::string bad = slurp(fs::path(g_mod_dir) / "user.json"); int n0 = logged_count("loaded 4 profile(s)");
    run_frame(480); CHECK(logged("user.json is MALFORMED")); CHECK(logged_count("loaded 4 profile(s)") == n0 + 1); CHECK(logged("0 combo(s)"));
    CHECK(slurp(fs::path(g_mod_dir) / "user.json") == bad);                  // the player's file is never rewritten, renamed or deleted
    CHECK(!fs::exists(fs::path(g_mod_dir) / "user.json.corrupt") && !fs::exists(fs::path(g_mod_dir) / "user.json.bak"));
    run_frame(600); run_frame(720); CHECK(logged_count("user.json changed") == 2);       // no reload loop while the file stays the same
    { std::ofstream f(fs::path(g_mod_dir) / "user.json", std::ios::trunc); f << user_before; } run_frame(840); CHECK(logged("loaded 4 profile(s), 3 combo(s)"));   // fixed file is picked up
    T("malformed_user_file_is_reported_left_untouched_and_recovers_when_fixed", false, b);

    b = g_fail; g_toggle_set(0); CHECK(g_toggle_get() == 0); np = g_panels.size(); run_frame(870); run_frame(871); CHECK(g_panels.size() == np);
    CHECK(slurp(fs::path(g_mod_dir) / "config.ini").find("panel=0") != std::string::npos);
    CHECK(logged("would select") ); g_toggle_set(1); run_frame(900); CHECK(g_panels.size() > np);
    T("panel_toggle_persists_and_logging_continues_without_it", false, b);

    b = g_fail; size_t nl = g_log.size(); tfmod_marca(7);
    CHECK(logged("[mark #7] AI Coach ranking")); CHECK(logged("[mark] side 1: character 2 deck 1 cpu=1")); CHECK(logged("[mark]   OK   main score 300")); CHECK(logged("required card 1001 available: yes")); CHECK(g_log.size() > nl);
    T("mark_dumps_the_ranking_with_every_check", false, b);

    b = g_fail; const std::vector<uint8_t> after(g_ram, g_ram + RAM);
    size_t diff = 0; for (size_t i = 0; i < RAM; i++) if (before[i] != after[i]) diff++;
    (void)diff;   // the test restores its own writes by now, so a before/after diff proves nothing; the strong check is a pure read-only pass over a fresh copy:
    const std::vector<uint8_t> s1(g_ram, g_ram + RAM); for (uint64_t f = 960; f < 1500; f += 7) run_frame(f); tfmod_marca(8); setup(8, 37, 2, 1);
    CHECK(memcmp(s1.data(), g_ram, RAM) == 0);
    T("observer_never_writes_game_memory", false, b);

    printf("%d tests, %d failed checks\n", g_run, g_fail); return g_fail ? 1 : 0;
}
