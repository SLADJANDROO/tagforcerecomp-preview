/* AI Coach plugin - OBSERVER stage (Milestone 3b). Reads the duel, works out which combo AI Coach would choose for each CPU side and says why, in a panel and
 * in the session log. It NEVER writes game memory and every hook returns 0, so the game plays exactly as without it. Action execution is a later milestone
 * (docs/AI_COACH.md, "execution backends"). Logic lives in plugins/aicoach_core (unit-tested without the game); this file is only the glue to the game:
 *   - 0883f4f8  duel setup commit: r4 = base of 0x18c-byte records per duelist; +0x120 duelist (character) id, +0x124 deck index (docs/DECKS.md; CPUvsCPU writes the same field).
 *               ASSUMPTION, unverified: record j describes duel player j.
 *   - 08833fc4  once per frame.
 *   - 0884b46c  card id -> name (UTF-16 pointer), called only to label cards in the panel.
 *   - 0x09f77888[p] == 1 means the CPU plays player p; 0x09f7d460 & 1 is the player whose turn it is (BetterAI, CPUvsCPU).
 * Files in the mod folder: data/defaults.json (shipped), user.json (the player's combos; edited by hand until the in-game editor exists; reloaded when it changes),
 * config.ini (panel=0/1, corner=0..3). Env: TF_COACH_PANEL=0/1. */
#include "tfmod.h"
#include "../../aicoach_core/game_view.h"
#include "../../aicoach_core/store.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <wchar.h>

using namespace aicoach;
namespace fs = std::filesystem;

static const TfModApi *A;
static const uint32_t F_FRAME = 0x08833fc4, F_SETUP = 0x0883f4f8, F_CARD_NAME = 0x0884b46c;
static const uint32_t CONTROL = 0x09f77888, TURN = 0x09f7d460, PLAYER = 0x09f79cec, STRIDE = 0xfac, SETUP_RECORD = 0x18c;
static const uint32_t RAM_LO = 0x08000000, RAM_HI = 0x0a000000;

struct Mem : GuestMemory {
    uint32_t u32(uint32_t a) const override { if (a < RAM_LO || a > RAM_HI - 4) return 0; uint32_t v; memcpy(&v, A->mem(a), 4); return v; }
    uint16_t u16(uint32_t a) const override { if (a < RAM_LO || a > RAM_HI - 2) return 0; uint16_t v; memcpy(&v, A->mem(a), 2); return v; }
};

/* ---------------------------------------------------------------- data */
static Defaults g_def;
static UserStore *g_user;
static Effective g_eff;
static fs::path g_defaults_path, g_user_path;
static std::pair<long long, long long> g_user_stamp = {-1, -1};   /* (mtime, size) of user.json at the last load */
static bool g_panel = true; static int g_corner = 2;

static std::pair<long long, long long> stamp(const fs::path &p) {
    std::error_code ec; const auto t = fs::last_write_time(p, ec); if (ec) return {-1, -1};
    const auto s = fs::file_size(p, ec); return {(long long)t.time_since_epoch().count(), ec ? -1 : (long long)s};
}
static void log_issues(const char *what, const Issues &is) {
    int n = 0; for (auto &i : is) { if (n++ >= 8) { A->log("%s: ... %d more issue(s)", what, (int)is.size() - 8); break; } A->log("%s: %s %s: %s", what, i.sev == Issue::Error ? "ERROR" : "warning", i.path.c_str(), i.msg.c_str()); }
}
/* Loads defaults + user data. Never throws, never writes, never deletes: a bad file is reported and treated as empty; the player's file is left untouched. */
static void reload() {
    ValidationContext ctx;
    std::string text; g_def = Defaults();
    if (!read_file(g_defaults_path, text)) A->log("data/defaults.json not found at %s: no shipped profiles or combos", g_defaults_path.string().c_str());
    else { LoadResult r = load_defaults(text, g_def, ctx); if (r.status != LoadStatus::Ok) A->log("defaults.json: %s", r.status == LoadStatus::Malformed ? "MALFORMED, ignored" : "unusable"); log_issues("defaults.json", r.issues); }
    if (!g_user) g_user = new UserStore(g_user_path);
    LoadResult u = g_user->load(ctx);
    if (u.status == LoadStatus::Malformed) A->log("user.json is MALFORMED: ignored this session, file left untouched");
    else if (u.status == LoadStatus::TooNew) A->log("user.json was written by a newer AI Coach: ignored, file left untouched");
    log_issues("user.json", u.issues);
    Issues mi; g_eff = merge(g_def, g_user->data, mi); log_issues("merge", mi);
    g_user_stamp = stamp(g_user_path);
    int enabled = 0; for (auto &c : g_eff.combos) if (c.enabled) enabled++;
    A->log("loaded %d profile(s), %d combo(s) (%d enabled)", (int)g_eff.profiles.size(), (int)g_eff.combos.size(), enabled);
}

/* ---------------------------------------------------------------- game state */
static int g_char[2], g_deck[2];
static bool g_in_duel; static int g_last_turn = -1; static AttemptLedger g_ledger;
static std::string g_last_top[2]; static std::vector<std::wstring> g_panel_lines; static std::wstring g_panel_title;

static bool cpu_side(int p) { return Mem().u32(CONTROL + 4u * (uint32_t)p) == 1; }
static const Profile *profile_for(int character) { for (auto &p : g_eff.profiles) if (p.enabled && p.character == character) return &p; return nullptr; }

static int hk_setup(TfCpu *c) {   /* observe only */
    const uint32_t base = A->reg(c, 4); if (base < RAM_LO || base > RAM_HI - 2 * SETUP_RECORD) return 0;
    Mem m;
    for (int j = 0; j < 2; j++) {
        g_char[j] = m.u16(base + (uint32_t)j * SETUP_RECORD + 0x120); g_deck[j] = m.u16(base + (uint32_t)j * SETUP_RECORD + 0x124);
        int n = 0; for (auto &cb : g_eff.combos) if (cb.enabled && (cb.character == 0 || cb.character == g_char[j]) && (cb.decks.empty() || std::find(cb.decks.begin(), cb.decks.end(), g_deck[j]) != cb.decks.end())) n++;
        A->log("duel setup: side %d = character %d, deck %d -> %d enabled combo(s) apply%s", j, g_char[j], g_deck[j], n, profile_for(g_char[j]) ? "" : " (no profile: generic)");
    }
    return 0;
}

/* card names for the panel (and for finding card ids to use in combos) */
static std::map<uint32_t, std::wstring> g_names;
static std::wstring card_name(TfCpu *c, uint32_t id) {
    auto it = g_names.find(id); if (it != g_names.end()) return it->second;
    uint32_t s[32]; for (int i = 1; i < 32; i++) s[i] = A->reg(c, i);   /* a hooked function's arguments live in these registers: save everything, restore after the call */
    A->set_reg(c, 4, id); A->call(c, F_CARD_NAME); const uint32_t p = A->reg(c, 2);
    for (int i = 1; i < 32; i++) if (i != 26 && i != 27) A->set_reg(c, i, s[i]);
    std::wstring w; Mem m;
    if (p >= RAM_LO && p < RAM_HI - 80) for (uint32_t i = 0; i < 40; i++) { const uint16_t ch = m.u16(p + 2 * i); if (!ch) break; w += (wchar_t)ch; }
    if (w.empty()) w = L"?";
    return g_names[id] = w;
}
static std::wstring clip(const std::wstring &s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + L"~"; }
static std::wstring widen(const std::string &s) { std::wstring w; for (unsigned char ch : s) w += (wchar_t)ch; return w; }

static void observe(TfCpu *c, int p) {
    Mem m; GameDuelView v(m, p); if (!v.plausible()) return;
    SelectionContext ctx; ctx.character = g_char[p]; ctx.deck = g_deck[p]; ctx.profile = profile_for(g_char[p]);
    const std::vector<Candidate> r = rank_combos(g_eff.combos, ctx, v, g_ledger);
    const bool have = !r.empty() && r[0].eligible; const std::string top = have ? r[0].id : std::string();
    if (top != g_last_top[p]) {   /* log only changes */
        g_last_top[p] = top;
        if (have) A->log("side %d (char %d deck %d): would select '%s' (score %d); %d combo(s) considered", p, g_char[p], g_deck[p], top.c_str(), r[0].score, (int)r.size());
        else A->log("side %d (char %d deck %d): no eligible combo -> the normal AI plays%s", p, g_char[p], g_deck[p], r.empty() ? " (no combos defined)" : "");
    }
    if (!g_panel) return;
    wchar_t b[96]; std::vector<std::wstring> L;
    swprintf(b, 96, L"@eAI Coach (observer)\tside %d", p); g_panel_title = b;
    swprintf(b, 96, L"#Character %d  deck %d\t%d combos", g_char[p], g_deck[p], (int)r.size()); L.push_back(b);
    if (have) { swprintf(b, 96, L"> %ls\t%d", clip(widen(g_eff.combos[r[0].index].name), 28).c_str(), r[0].score); L.push_back(b); }
    else L.push_back(L"No eligible combo: normal AI");
    int shown = 0; for (size_t i = have ? 1 : 0; i < r.size() && shown < 4; i++, shown++) L.push_back(clip(widen(r[i].id + ": " + r[i].reason), 44));
    const uint32_t base = PLAYER + (uint32_t)p * STRIDE, n = std::min<uint32_t>(m.u32(base + 0x0c), 12);
    swprintf(b, 96, L"#Hand (card ids)\t%u", n); L.push_back(b);
    for (uint32_t i = 0; i < n; i++) { const uint32_t id = m.u16(base + 0x120 + i * 4) & 0x1fffu; swprintf(b, 96, L"%u %ls", id, clip(card_name(c, id), 32).c_str()); L.push_back(b); }
    g_panel_lines = L;
}

static int hk_frame(TfCpu *c) {
    if (!A->module_loaded("libduel_eng")) { if (g_in_duel) { g_in_duel = false; g_last_turn = -1; g_last_top[0] = g_last_top[1] = "?"; g_panel_lines.clear(); } return 0; }
    const uint64_t f = A->frame(); Mem m;
    if (!g_in_duel) { g_in_duel = true; g_ledger.new_turn(); g_last_top[0] = g_last_top[1] = "?"; }
    if (f % 120 == 0 && g_user && stamp(g_user_path) != g_user_stamp) { A->log("user.json changed: reloading"); reload(); g_last_top[0] = g_last_top[1] = "?"; }
    const int turn = (int)(m.u32(TURN) & 1);
    if (turn != g_last_turn) { g_last_turn = turn; g_ledger.new_turn(); g_last_top[0] = g_last_top[1] = "?"; }
    if (f % 30 == 0) {
        if (cpu_side(turn)) observe(c, turn); else if (!cpu_side(1 - turn)) g_panel_lines.clear();   /* a human turn: nothing to show */
    }
    if (g_panel && !g_panel_lines.empty() && cpu_side(turn)) {
        std::vector<const wchar_t *> p; for (auto &l : g_panel_lines) p.push_back(l.c_str());
        A->panel(g_corner, g_panel_title.c_str(), p.data(), (int)p.size());
    }
    return 0;
}

/* ---------------------------------------------------------------- settings */
static void cfg_save() { std::ofstream f(fs::path(A->mod_dir) / "config.ini", std::ios::trunc); if (f) f << "panel=" << (g_panel ? 1 : 0) << "\ncorner=" << g_corner << "\n"; }
static void cfg_load() {
    std::ifstream f(fs::path(A->mod_dir) / "config.ini"); std::string l; int v;
    while (f && std::getline(f, l)) { if (sscanf(l.c_str(), "panel=%d", &v) == 1) g_panel = v != 0; else if (sscanf(l.c_str(), "corner=%d", &v) == 1) g_corner = std::max(0, std::min(3, v)); }
    if (const char *e = getenv("TF_COACH_PANEL")) g_panel = atoi(e) != 0;
}
static int panel_get() { return g_panel; }
static void panel_set(int v) { g_panel = v != 0; if (!g_panel) g_panel_lines.clear(); cfg_save(); A->log("panel %s", g_panel ? "on" : "off"); }

extern "C" __declspec(dllexport) int tfmod_init(const TfModApi *api) {
    A = api; TFMOD_REQUIRE(api, 4);
    cfg_load();
    g_defaults_path = fs::path(A->mod_dir) / "data" / "defaults.json"; g_user_path = fs::path(A->mod_dir) / "user.json";
    reload();
    A->hook(F_SETUP, hk_setup); A->hook(F_FRAME, hk_frame);
    A->add_toggle(L"AI Coach panel", 0x73 /* F4 */, panel_get, panel_set);
    A->log("AI Coach observer loaded (it does not change how the CPU plays)");
    return 0;
}
/* F7 / L3+R3 mark: dump the full ranking with every check, for the side whose turn it is (or both when unclear) */
extern "C" __declspec(dllexport) void tfmod_marca(int n) {
    Mem m; A->log("[mark #%d] AI Coach ranking", n);
    for (int p = 0; p < 2; p++) {
        GameDuelView v(m, p); if (!v.plausible()) { A->log("[mark] side %d: duel state not plausible", p); continue; }
        SelectionContext ctx; ctx.character = g_char[p]; ctx.deck = g_deck[p]; ctx.profile = profile_for(g_char[p]);
        const std::vector<Candidate> r = rank_combos(g_eff.combos, ctx, v, g_ledger);
        A->log("[mark] side %d: character %d deck %d cpu=%d, %d combo(s)", p, g_char[p], g_deck[p], cpu_side(p) ? 1 : 0, (int)r.size());
        for (auto &k : r) { A->log("[mark]   %s %s score %d: %s", k.eligible ? "OK  " : "skip", k.id.c_str(), k.score, k.reason.c_str()); for (auto &ck : k.checks) A->log("[mark]       %s", ck.text.c_str()); }
    }
}
extern "C" { __declspec(dllexport) const char *tfmod_game_ids = "ULES-00600"; }
