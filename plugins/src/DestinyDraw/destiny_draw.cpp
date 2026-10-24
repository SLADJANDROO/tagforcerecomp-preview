/* Destiny Draw for Tag Force 1 (European build ULES-00600). The rules are in destiny_logic.h; this file connects them to the game.
 *   0883f4f8  duel setup (as in AI Coach / CPUvsCPU): a new duel, so each side may use Destiny Draw again.
 *   08833fc4  once per frame: the key / pad request, the "ready" panel, the message box and the check of the first draw after a use.
 *   09f56da8  (BetterAI's F_STATS) current stats of a field monster: (player, slot, buffer) -> ATK at buffer+20. Only called while libduel_eng is loaded.
 *   0884b46c  card id -> name (UTF-16 pointer in r2), to match Destiny Cards given by name and to show the card.
 *   0x09f77888 + 4p == 1: the CPU plays side p (only human sides get Destiny Draw).
 * Writes to game memory: ONLY the deck order of the side that uses it (destiny_logic.h move_to_top), at most once per duel, and never again
 * this session if the first draw after a use shows that the deck layout assumption is wrong. Settings: config.ini (see README.md). */
#include "tfmod.h"
#include "destiny_logic.h"
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

namespace fs = std::filesystem;
static const TfModApi *A;
static const uint32_t F_FRAME = 0x08833fc4, F_SETUP = 0x0883f4f8, F_STATS = 0x09f56da8, F_CARD_NAME = 0x0884b46c;
static const uint32_t CONTROL = 0x09f77888, RAM_LO = 0x08000000, RAM_HI = 0x0a000000;
static const uint32_t PAD_L = 0x100, PAD_R = 0x200, PAD_SELECT = 0x1;

struct GuestMem : dd::Memory {
    uint32_t r32(uint32_t a) const override { if (a < RAM_LO || a > RAM_HI - 4) return 0; uint32_t v; memcpy(&v, A->mem(a), 4); return v; }
    void w32(uint32_t a, uint32_t v) override { if (a < RAM_LO || a > RAM_HI - 4) return; memcpy(A->mem(a), &v, 4); }
};

/* ---------------------------------------------------------------- settings */
struct Cfg { std::set<uint32_t> ids; std::vector<std::wstring> names; dd::Rules rules; bool combo = false, panel = true, dry_run = false; int corner = 1; };
static Cfg g_cfg;
static std::wstring utf8(const std::string &s) {
    std::wstring w; for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i]; uint32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 0x1f; n = 2; } else if ((c >> 4) == 14) { cp = c & 0x0f; n = 3; } else { cp = '?'; n = 1; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
        w += (wchar_t)cp; i += n;
    }
    return w;
}
static void cfg_load() {
    g_cfg = Cfg(); std::ifstream f(fs::path(A->mod_dir) / "config.ini"); std::string l;
    while (f && std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        const size_t eq = l.find('='); if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "cards") g_cfg.ids = dd::parse_ids(v);
        else if (k == "names") g_cfg.names = dd::parse_names(utf8(v));
        else if (k == "lp_below") g_cfg.rules.lp_below = atoi(v.c_str());
        else if (k == "combo") g_cfg.combo = atoi(v.c_str()) != 0;
        else if (k == "panel") g_cfg.panel = atoi(v.c_str()) != 0;
        else if (k == "corner") g_cfg.corner = std::max(0, std::min(3, atoi(v.c_str())));
        else if (k == "dry_run") g_cfg.dry_run = atoi(v.c_str()) != 0;
    }
    A->log("Destiny Cards: %d id(s), %d name(s); LP below %d; pad combo L+R+Select %s%s", (int)g_cfg.ids.size(), (int)g_cfg.names.size(), g_cfg.rules.lp_below,
           g_cfg.combo ? "on" : "off", g_cfg.dry_run ? "; DRY RUN (nothing is written)" : "");
}

/* ---------------------------------------------------------------- game helpers */
static uint32_t g_buf;
static uint32_t call_game(TfCpu *c, uint32_t fn, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0) {   /* keeps every register of the hooked function */
    uint32_t s[32]; for (int i = 1; i < 32; i++) s[i] = A->reg(c, i);
    A->set_reg(c, 4, a0); A->set_reg(c, 5, a1); A->set_reg(c, 6, a2); A->call(c, fn);
    const uint32_t r = A->reg(c, 2);
    for (int i = 1; i < 32; i++) if (i != 26 && i != 27) A->set_reg(c, i, s[i]);
    return r;
}
static std::map<uint32_t, std::wstring> g_names;
static std::wstring card_name(TfCpu *c, uint32_t id) {
    auto it = g_names.find(id); if (it != g_names.end()) return it->second;
    const uint32_t p = call_game(c, F_CARD_NAME, id); GuestMem m; std::wstring w;
    if (p >= RAM_LO && p < RAM_HI - 96) for (uint32_t i = 0; i < 48; i++) { const uint32_t a = p + 2 * i; const uint16_t ch = (uint16_t)(m.r32(a & ~3u) >> ((a & 2) ? 16 : 0)); if (!ch) break; w += (wchar_t)ch; }
    return g_names[id] = w;
}
static bool cpu_side(int p) { return GuestMem().r32(CONTROL + 4u * (uint32_t)p) == 1; }
static dd::Board board(TfCpu *c, int p) {
    GuestMem m; dd::Board b; b.lp = (int)m.r32(dd::player(p));
    for (int side = 0; side < 2; side++) for (int s = 0; s < 5; s++) {
        const uint32_t slot = dd::kSlots + (uint32_t)side * dd::kStride + (uint32_t)s * dd::kSlotStride;
        if (!(m.r32(slot) & 0x1fffu)) continue;
        call_game(c, F_STATS, (uint32_t)side, (uint32_t)s, g_buf); const int atk = (int)m.r32(g_buf + 20);
        if (side == p) b.my_best_atk = std::max(b.my_best_atk, atk); else { b.opp_monsters++; b.opp_best_atk = std::max(b.opp_best_atk, atk); }
    }
    return b;
}
/* Destiny Cards of this deck: the ids from config.ini plus every deck card whose name is listed. */
static std::set<uint32_t> destiny_set(TfCpu *c, const dd::Deck &d) {
    std::set<uint32_t> s = g_cfg.ids;
    if (!g_cfg.names.empty()) for (uint32_t e : d.entries) { const uint32_t id = dd::card_id(e); const std::wstring n = dd::lower(card_name(c, id)); for (auto &want : g_cfg.names) if (n == want) s.insert(id); }
    return s;
}
static int destiny_in_deck(const dd::Deck &d, const std::set<uint32_t> &s) { int n = 0; for (uint32_t e : d.entries) if (s.count(dd::card_id(e))) n++; return n; }

/* ---------------------------------------------------------------- duel state */
static bool g_in_duel, g_used[2], g_broken, g_request, g_combo_held;
struct Pending { bool on = false; int side = 0; uint32_t hand_before = 0, expected = 0; };
static Pending g_pending;
static std::wstring g_msg_title; static std::vector<std::wstring> g_msg; static uint64_t g_msg_until;
static std::vector<std::wstring> g_panel_lines;

static void message(const std::wstring &title, std::vector<std::wstring> lines, int seconds = 3) { g_msg_title = title; g_msg = std::move(lines); g_msg_until = A->frame() + 60u * (uint64_t)seconds; }
static void new_duel(const char *why) { g_used[0] = g_used[1] = false; g_pending = Pending(); g_panel_lines.clear(); A->log("new duel (%s): Destiny Draw available again", why); }

static int hk_setup(TfCpu *) { new_duel("duel setup"); g_in_duel = true; return 0; }

static int human_side() { for (int p = 0; p < 2; p++) if (!cpu_side(p)) return p; return -1; }

static void use(TfCpu *c) {
    const int p = human_side();
    if (p < 0) { message(L"Destiny Draw", {L"No human player in this duel."}); return; }
    if (g_broken) { message(L"Destiny Draw is off", {L"The deck layout check failed earlier", L"this session (see the log)."}); return; }
    GuestMem m; const dd::Deck d = dd::read_deck(m, p);
    if (!d.ok) { A->log("side %d: deck not readable: %s", p, d.why.c_str()); message(L"Destiny Draw", {L"Can't read your deck right now."}); return; }
    const dd::Board b = board(c, p);
    const dd::Verdict v = dd::available(b, g_used[p], g_cfg.rules);
    A->log("side %d request: LP %d, opponent monsters %d, best ATK mine %d / theirs %d -> %s", p, b.lp, b.opp_monsters, b.my_best_atk, b.opp_best_atk, v.ok ? "available" : v.why.c_str());
    if (!v.ok) { std::wstring w(v.why.begin(), v.why.end()); message(L"Destiny Draw not available", {w}); return; }
    const std::set<uint32_t> s = destiny_set(c, d);
    const int k = dd::pick(d, s, (uint32_t)A->frame());
    if (k < 0) { message(L"Destiny Draw", {L"No Destiny Card left in your deck.", L"Set them in mods\\DestinyDraw\\config.ini"}); A->log("side %d: no Destiny Card in the deck (%d configured)", p, (int)s.size()); return; }
    const uint32_t id = dd::card_id(d.entries[(size_t)k]); const std::wstring name = card_name(c, id);
    std::string top; char t[16]; for (size_t i = 0; i < d.entries.size() && i < 5; i++) { snprintf(t, sizeof t, " %08x", d.entries[i]); top += t; }
    A->log("side %d: Destiny Card %u (%ls) at deck position %d of %u; top before:%s", p, id, name.c_str(), k, d.count, top.c_str());
    if (g_cfg.dry_run) { message(L"Destiny Draw (dry run)", {L"Would put on top:", name.empty() ? std::to_wstring(id) : name}); return; }
    dd::Deck dm = d; GuestMem w; dd::move_to_top(w, p, dm, k);
    g_used[p] = true;
    g_pending.on = true; g_pending.side = p; g_pending.expected = id; g_pending.hand_before = m.r32(dd::player(p) + dd::kHandCount);
    A->log("side %d: Destiny Draw used, card %u is on top; waiting for the next draw to confirm the layout", p, id);
    message(L"DESTINY DRAW!", {name.empty() ? std::to_wstring(id) : name, L"is on top of your deck."}, 4);
}

static void check_pending() {
    if (!g_pending.on) return;
    GuestMem m; uint32_t got = 0;
    const dd::Check r = dd::verify_draw(m, g_pending.side, g_pending.hand_before, g_pending.expected, &got);
    if (r == dd::Check::Waiting) {   /* the hand can also shrink (a card was played) before the draw: follow it */
        const uint32_t h = m.r32(dd::player(g_pending.side) + dd::kHandCount); if (h < g_pending.hand_before) g_pending.hand_before = h;
        return;
    }
    g_pending.on = false;
    if (r == dd::Check::Confirmed) A->log("CONFIRMED: side %d drew card %u, the Destiny Card. The Tag Force 1 deck layout is right.", g_pending.side, got);
    else {
        g_broken = true;
        A->log("MISMATCH: side %d drew card %u, expected the Destiny Card %u. The deck layout assumption is WRONG for this game: Destiny Draw is disabled for the rest of the session.", g_pending.side, got, g_pending.expected);
        message(L"Destiny Draw: check failed", {L"The drawn card was not the Destiny Card.", L"Disabled; please send the log."}, 6);
    }
}

static int hk_frame(TfCpu *c) {
    if (!A->module_loaded("libduel_eng")) { if (g_in_duel) { g_in_duel = false; g_pending = Pending(); g_panel_lines.clear(); } g_request = false; return 0; }
    if (!g_in_duel) { g_in_duel = true; new_duel("duel engine loaded"); }
    const uint64_t f = A->frame();
    if (g_cfg.combo) { const uint32_t b = A->buttons(); const bool held = (b & (PAD_L | PAD_R | PAD_SELECT)) == (PAD_L | PAD_R | PAD_SELECT); if (held && !g_combo_held) g_request = true; g_combo_held = held; }
    if (g_request) { g_request = false; use(c); }
    check_pending();
    if (g_cfg.panel && f % 30 == 0) {   /* the "option appears" part of the rule: a small panel while it can be used */
        g_panel_lines.clear(); const int p = human_side();
        if (p >= 0 && !g_used[p] && !g_broken) {
            GuestMem m; const dd::Deck d = dd::read_deck(m, p);
            if (d.ok && dd::available(board(c, p), false, g_cfg.rules).ok) {
                const int n = destiny_in_deck(d, destiny_set(c, d));
                if (n > 0) { wchar_t t[64]; swprintf(t, 64, L"#Ready\t%d in deck", n); g_panel_lines.push_back(t); g_panel_lines.push_back(g_cfg.combo ? L"F2 or L+R+Select" : L"Press F2"); }
            }
        }
    }
    if (!g_panel_lines.empty()) { std::vector<const wchar_t *> l; for (auto &s : g_panel_lines) l.push_back(s.c_str()); A->panel(g_cfg.corner, L"@eDestiny Draw", l.data(), (int)l.size()); }
    if (f < g_msg_until) { std::vector<const wchar_t *> l; for (auto &s : g_msg) l.push_back(s.c_str()); A->message_box(g_msg_title.c_str(), l.data(), (int)l.size(), L""); }
    return 0;
}

static int key_get() { return 0; }
static void key_set(int) { g_request = true; }

extern "C" __declspec(dllexport) int tfmod_init(const TfModApi *api) {
    A = api; TFMOD_REQUIRE(api, 4);
    cfg_load();
    g_buf = A->guest_alloc(64); if (!g_buf) { A->log("no guest memory for the stats buffer"); return 3; }
    A->hook(F_SETUP, hk_setup); A->hook(F_FRAME, hk_frame);
    A->add_toggle(L"Destiny Draw (use now)", 0x71 /* F2 */, key_get, key_set);
    A->log("Destiny Draw loaded: F2 (or L+R+Select if combo=1) when your LP is low and the opponent is stronger, once per duel");
    return 0;
}
extern "C" { __declspec(dllexport) const char *tfmod_game_ids = "ULES-00600"; }
