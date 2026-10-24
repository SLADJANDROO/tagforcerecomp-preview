/* Unit tests for plugins/aicoach_core/editor.* (Milestone 6a): the editor's screens, driven with scripted pad presses.
 * Nothing here touches the game; "pad" and "screen" are the Btn enum and the Line vector. */
#include "editor.h"
#include <stdio.h>
#include <fstream>
#include <string.h>
using namespace aicoach;
namespace fs = std::filesystem;

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define TEST(name) static void name(); struct R_##name { R_##name() { tests().push_back({#name, name}); } } r_##name; static void name()
struct T { const char *n; void (*f)(); };
static std::vector<T> &tests() { static std::vector<T> v; return v; }

static std::string tmpdir(const char *n) { std::string d = (fs::temp_directory_path() / (std::string("aicoach_ed_") + n)).string(); fs::remove_all(d); fs::create_directories(d); return d; }
static std::string narrow(const std::wstring &w) { std::string s; for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?'); return s; }

static Step mk_step(const char *id, const char *action, uint32_t card = 0) { Step s; s.id = id; s.action = action; s.card = card; return s; }
static Combo default_combo() {
    Combo c; c.id = "d.one"; c.name = "Default one"; c.character = 2; c.priority = 300; c.required_cards = {1001}; c.steps = {mk_step("s1", "normal_summon", 1001)};
    return c;
}
static std::wstring card_name(uint32_t id) {
    switch (id) { case 1001: return L"Alpha Dragon"; case 1002: return L"Beta Mage"; case 1003: return L"alphabet soup"; case 2000: return L"Zed"; default: return L""; }
}

struct Fix {
    std::string dir; Defaults defs; UserStore store; Capabilities caps; ValidationContext ctx; CardIndex cards; Editor ed;
    static Capabilities mkcaps(std::initializer_list<const char *> a) { Capabilities c; for (auto *x : a) c.actions.insert(x); return c; }
    static ValidationContext mkctx(const Capabilities *c, bool use) { ValidationContext v; if (use) v.caps = c; return v; }
    explicit Fix(const char *name, std::initializer_list<const char *> actions = {}) : dir(tmpdir(name)), store(fs::path(dir) / "user.json"), caps(mkcaps(actions)),
        ctx(mkctx(&caps, actions.size() > 0)), cards(card_name, 3000), ed(defs, store, ctx, cards) {
        defs.combos.push_back(default_combo());
        cards.pump(100000);
    }
    /* Presses: U D L R  X=cross O=circle T=triangle S=square 1=L 2=R A=start E=select. Checks the screen limits after every press. */
    void keys(const char *s) {
        for (; *s; s++) {
            Btn b = Btn::Up;
            switch (*s) { case 'U': b = Btn::Up; break; case 'D': b = Btn::Down; break; case 'L': b = Btn::Left; break; case 'R': b = Btn::Right; break;
                case 'X': b = Btn::Cross; break; case 'O': b = Btn::Circle; break; case 'T': b = Btn::Triangle; break; case 'S': b = Btn::Square; break;
                case '1': b = Btn::L; break; case '2': b = Btn::R; break; case 'A': b = Btn::Start; break; case 'E': b = Btn::Select; break; default: CHECK(false); }
            ed.press(b); sane();
        }
    }
    void sane() {
        auto l = ed.render(); CHECK(l.size() <= 14); int sel = 0;
        for (auto &x : l) { CHECK(x.text.size() <= 40); if (x.selected) sel++; }
        CHECK(sel <= 1);
    }
    /* Moves the cursor down (wrapping) until the selected row starts with `prefix`. */
    bool go(const char *prefix) { for (int i = 0; i < 40; i++) { if (selected().compare(0, strlen(prefix), prefix) == 0) return true; keys("D"); } return false; }
    std::string selected() { for (auto &x : ed.render()) if (x.selected) return narrow(x.text); return ""; }
    std::string screen() { auto l = ed.render(); return l.empty() ? "" : narrow(l[0].text); }
    bool shows(const char *part) { for (auto &x : ed.render()) if (narrow(x.text).find(part) != std::string::npos) return true; return false; }
    UserData reload() { UserStore s2(fs::path(dir) / "user.json"); s2.load(ctx); return s2.data; }
};

TEST(pad_reader_gives_one_press_then_repeats_only_for_directions) {
    PadReader p(3, 2); std::vector<int> counts;
    auto n = [&](uint32_t m) { return p.update(m).size(); };
    CHECK(n(kCross) == 1); CHECK(n(kCross) == 0); CHECK(n(kCross) == 0); CHECK(n(kCross) == 0); CHECK(n(kCross) == 0);   /* cross never repeats */
    CHECK(n(0) == 0); CHECK(n(kCross) == 1);                                                                              /* released, pressed again */
    PadReader q(3, 2); CHECK(q.update(kDown).size() == 1); CHECK(q.update(kDown).empty()); CHECK(q.update(kDown).empty());
    auto r = q.update(kDown); CHECK(r.size() == 1 && r[0] == Btn::Down);                                                  /* 4th frame = first repeat */
    CHECK(q.update(kDown).empty()); CHECK(q.update(kDown).size() == 1);
    PadReader m; auto both = m.update(kUp | kCross); CHECK(both.size() == 2);
    auto none = m.update(0); CHECK(none.empty());
}

TEST(card_index_resolves_gradually_and_searches) {
    CardIndex ci(card_name, 3000);
    CHECK(!ci.complete()); CHECK(ci.search(L"").empty()); ci.pump(1001); CHECK(ci.name(1001) == L"Alpha Dragon"); CHECK(ci.name(1002).empty());
    ci.pump(100000); CHECK(ci.complete()); CHECK(ci.resolved() == 3000);
    CHECK(ci.search(L"").size() == 4);
    auto a = ci.search(L"ALPHA"); CHECK(a.size() == 2 && a[0] == 1001 && a[1] == 1003);                                       /* case-insensitive, ascending */
    auto id = ci.search(L"1002"); CHECK(id.size() == 1 && id[0] == 1002);
    CHECK(ci.search(L"zzz").empty()); CHECK(ci.search(L"", 2).size() == 2);
    CHECK(ci.name(0).empty()); CHECK(ci.name(999999).empty());
}

TEST(list_shows_defaults_and_opens_closes) {
    Fix f("list"); CHECK(!f.ed.active()); CHECK(f.ed.render().empty()); f.keys("X");   /* ignored while closed */
    f.ed.open(); CHECK(f.ed.active()); CHECK(f.screen().find("combos") != std::string::npos);
    CHECK(f.shows("[x] Default one  p300")); CHECK(f.shows("+ New combo"));
    f.keys("O"); CHECK(!f.ed.active());
}

TEST(new_combo_is_built_with_the_pad_and_saved_through_the_normal_validator) {
    Fix f("newcombo"); f.ed.open();
    f.keys("S");                                            /* Square: new combo */
    CHECK(f.screen() == "Edit combo"); CHECK(f.ed.dirty()); CHECK(f.selected() == "Name: New combo");
    f.keys("X"); CHECK(f.screen() == "Combo name");
    f.keys("SSSSSSSSS");                                     /* erase "New combo" */
    f.keys("RRRRRRRXDDRXA");                                 /* H (row 0, col 7) -> h -> i, then Start */
    CHECK(f.selected() == "Name: Hi");
    f.keys("DDDDD");                                         /* Required cards */
    f.keys("XS"); CHECK(f.screen() == "Add card"); CHECK(f.shows("1001 Alpha Dragon")); CHECK(f.shows("1003 alphabet soup"));
    f.keys("X"); CHECK(f.screen() == "Required cards"); CHECK(f.shows("1001 Alpha Dragon")); f.keys("O");
    CHECK(f.selected() == "Required cards: 1");
    f.keys("DDD");                                           /* Steps */
    CHECK(f.selected() == "Steps: 0"); f.keys("X"); CHECK(f.screen() == "Steps");
    f.keys("S"); CHECK(f.screen() == "New step: action"); CHECK(f.selected().find("normal_summon") != std::string::npos);
    f.keys("X"); CHECK(f.screen() == "Step 1"); CHECK(f.selected() == "Action: normal_summon");
    f.keys("DX"); CHECK(f.screen() == "Step card"); f.keys("X");            /* first match = 1001 */
    CHECK(f.screen() == "Step 1"); CHECK(f.selected() == "Card: 1001 Alpha Dragon");
    f.keys("OO"); CHECK(f.screen() == "Edit combo"); CHECK(f.selected() == "Steps: 1");
    f.keys("DDDDD");                                         /* [ Save ] */
    CHECK(f.selected().find("[ Save ]") == 0); f.keys("X");
    CHECK(f.ed.message() == "Saved"); CHECK(!f.ed.dirty());
    UserData u = f.reload(); CHECK(u.combos.size() == 1);
    if (u.combos.size() == 1) {
        const Combo &c = u.combos[0];
        CHECK(c.id == "user.1"); CHECK(c.name == "Hi"); CHECK(c.priority == 300); CHECK(c.required_cards == std::vector<uint32_t>{1001});
        CHECK(c.steps.size() == 1 && c.steps[0].action == "normal_summon" && c.steps[0].card == 1001 && c.steps[0].id == "s1");
        Issues is; validate_combo(c, ValidationContext(), "c", is); CHECK(!has_errors(is));
    }
    f.keys("O"); CHECK(f.screen().find("combos") != std::string::npos); CHECK(f.shows("Hi  p300 *mine"));
    CHECK(f.ed.effective().combos.size() == 2);
}

TEST(saving_a_combo_with_errors_is_refused_and_check_explains) {
    Fix f("errors"); f.ed.open(); f.keys("S");              /* a new combo has no steps */
    f.keys("DDDDDDDDDDDD"); CHECK(f.selected() == "[ Check for problems ]");
    f.keys("X"); CHECK(f.screen().find("Check:") == 0); CHECK(f.shows("[E]")); f.keys("O");
    f.keys("DX"); CHECK(f.ed.message().find("Not saved") == 0); CHECK(f.ed.dirty());
    CHECK(!fs::exists(fs::path(f.dir) / "user.json"));      /* nothing written */
}

TEST(leaving_with_unsaved_changes_asks_first) {
    Fix f("leave"); f.ed.open(); f.keys("X");               /* open the default */
    CHECK(f.screen() == "Edit combo"); CHECK(!f.ed.dirty());
    f.keys("O"); CHECK(f.screen().find("combos") != std::string::npos);   /* untouched: leaves at once */
    f.keys("XDDR"); CHECK(f.ed.dirty()); CHECK(f.selected() == "Priority: 310");
    f.keys("O"); CHECK(f.screen() == "Unsaved changes"); f.keys("DDX");   /* Keep editing */
    CHECK(f.screen() == "Edit combo"); CHECK(f.ed.dirty());
    f.keys("O"); f.keys("DX");                               /* Leave without saving */
    CHECK(f.screen().find("combos") != std::string::npos); CHECK(!f.ed.dirty()); CHECK(f.shows("p300"));
    CHECK(!fs::exists(fs::path(f.dir) / "user.json"));
    f.keys("XDDR"); f.keys("O"); f.keys("X");                // Save and leave
    CHECK(f.screen().find("combos") != std::string::npos); CHECK(f.shows("p310 *edited"));
    CHECK(f.reload().combos.size() == 1);
}

TEST(list_shortcuts_patch_defaults_without_touching_them) {
    Fix f("patch"); const std::string before = serialize_defaults(f.defs); f.ed.open();
    f.keys("T"); CHECK(f.shows("[ ] Default one")); CHECK(f.ed.message() == "Disabled");
    f.keys("R"); CHECK(f.shows("p310")); f.keys("LLL"); CHECK(f.shows("p280"));
    UserData u = f.reload(); CHECK(u.patches.count("d.one") == 1 && u.patches["d.one"].has_enabled && !u.patches["d.one"].enabled && u.patches["d.one"].priority == 280);
    CHECK(u.combos.empty());                                 /* a patch is not a copy of the combo */
    CHECK(serialize_defaults(f.defs) == before);
    f.keys("T"); CHECK(f.shows("[x] Default one"));
    for (int i = 0; i < 40; i++) f.keys("L"); CHECK(f.shows("p0"));   /* clamped */
    for (int i = 0; i < 120; i++) f.keys("R"); CHECK(f.shows("p1000"));
}

TEST(delete_hides_defaults_reverts_overrides_and_removes_user_combos) {
    Fix f("delete"); f.ed.open();
    f.keys("E"); CHECK(f.screen().find("Hide") == 0); f.keys("DX"); CHECK(f.shows("[x] Default one") && f.shows("p300"));   /* No */
    f.keys("EX"); CHECK(!f.shows("Default one")); CHECK(f.shows("Restore hidden defaults (1)")); CHECK(f.reload().deleted.size() == 1);
    f.keys("DX"); CHECK(f.shows("Default one")); CHECK(f.reload().deleted.empty());                                          /* restore */
    f.keys("UX"); f.go("Priority"); f.keys("R"); CHECK(f.go("[ Save ]")); f.keys("X"); f.keys("O");   /* make an override */
    CHECK(f.shows("*edited")); f.keys("E"); CHECK(f.screen().find("Revert") == 0); f.keys("X");
    CHECK(f.shows("p300") && !f.shows("*edited")); CHECK(f.reload().combos.empty());
    f.keys("S"); f.keys("O"); f.keys("DX");                  /* new draft, leave without saving: nothing created */
    CHECK(f.ed.effective().combos.size() == 1);
}

TEST(editing_a_default_makes_an_override_and_undoing_it_removes_it_again) {
    Fix f("override"); f.ed.open();
    f.keys("X"); f.go("Priority"); f.keys("R"); f.go("[ Save ]"); f.keys("X");    // priority 310, save
    UserData u = f.reload(); CHECK(u.combos.size() == 1 && u.combos[0].id == "d.one" && u.combos[0].priority == 310);
    Effective e = f.ed.effective(); CHECK(e.combos.size() == 1 && e.combos[0].origin == Combo::UserOverride);
    f.go("Priority"); f.keys("L"); f.go("[ Save ]"); f.keys("X");    // back to 300: identical to the default
    CHECK(f.ed.message().find("identical") != std::string::npos); CHECK(f.reload().combos.empty());
    CHECK(f.ed.effective().combos[0].origin == Combo::Default);
}

TEST(duplicate_creates_a_copy_that_exists_only_after_saving) {
    Fix f("dup"); f.ed.open(); f.keys("A");
    CHECK(f.screen() == "Edit combo"); CHECK(f.selected() == "Name: Default one (copy)"); CHECK(f.ed.dirty());
    CHECK(f.ed.effective().combos.size() == 1);
    f.keys("DDDDDDDDDDDDD"); CHECK(f.selected().find("[ Save ]") == 0); f.keys("X"); CHECK(f.ed.message() == "Saved");
    Effective e = f.ed.effective(); CHECK(e.combos.size() == 2 && e.combos[1].id == "d.one.copy1" && e.combos[1].origin == Combo::UserNew);
    CHECK(e.combos[0].id == "d.one" && e.combos[0].origin == Combo::Default);
}

TEST(a_failed_write_keeps_the_previous_data_and_says_so) {
    Fix f("failwrite");
    { std::ofstream(fs::path(f.dir) / "blocker") << "x"; }
    UserStore bad(fs::path(f.dir) / "blocker" / "user.json");            /* parent is a file: cannot be written */
    Editor ed(f.defs, bad, f.ctx, f.cards); ed.open(); ed.press(Btn::Triangle);
    CHECK(ed.message().find("Not saved") == 0); CHECK(bad.data.patches.empty());
    CHECK(ed.effective().combos[0].enabled);                              /* still enabled in memory too */
}

TEST(a_newer_schema_file_is_never_overwritten) {
    Fix f("toonew"); const std::string text = "{\"kind\":\"aicoach.user\",\"schema_version\":99}";
    { std::ofstream(fs::path(f.dir) / "user.json") << text; }
    f.store.load(f.ctx); CHECK(f.store.status() == LoadStatus::TooNew);
    f.ed.open(); f.keys("T"); CHECK(f.ed.message().find("Not saved") == 0);
    std::string now; CHECK(read_file(fs::path(f.dir) / "user.json", now)); CHECK(now == text);
}

TEST(only_actions_the_engine_can_execute_are_offered) {
    Fix f("caps", {"normal_summon", "end_turn"});
    f.ed.open(); f.keys("S"); CHECK(f.go("Steps:")); CHECK(f.selected() == "Steps: 0"); f.keys("XS");
    CHECK(f.screen() == "New step: action"); int rows = 0; for (auto &l : f.ed.render()) if (narrow(l.text).find("  ") == 0 || narrow(l.text).find("* ") == 0) rows++;
    CHECK(rows == 2); CHECK(f.shows("normal_summon")); CHECK(f.shows("end_turn")); CHECK(!f.shows("special_summon"));
}

TEST(conditions_are_edited_with_defaults_that_validate) {
    Fix f("conds"); f.ed.open(); f.keys("S"); f.go("Steps:"); f.keys("XS"); f.keys("X");   // step 1: normal_summon
    CHECK(f.screen() == "Step 1"); CHECK(f.go("Before:"));
    CHECK(f.selected() == "Before: 0 condition(s)"); f.keys("XS"); CHECK(f.screen() == "New condition type");
    CHECK(f.go("  monster_position")); f.keys("X"); CHECK(f.screen() == "Condition"); CHECK(f.selected() == "Type: monster_position");
    CHECK(f.go("Card:")); f.keys("X"); CHECK(f.screen() == "Condition card"); f.keys("DX");                  // Alpha is first, Beta second
    CHECK(f.shows("Card: 1002 Beta Mage")); CHECK(f.go("Position:")); f.keys("X"); CHECK(f.screen() == "Position"); f.keys("DX");   // attack, defense, facedown
    CHECK(f.shows("Position: defense")); CHECK(f.go("Negate")); f.keys("X"); CHECK(f.shows("Negate (not): yes"));
    f.keys("O"); CHECK(f.screen() == "Conditions before the step"); CHECK(f.shows("1. not monster_position 1002 defense"));
    f.keys("T"); CHECK(f.shows("(empty)"));
    f.keys("S"); CHECK(f.go("  free_zones")); f.keys("X"); CHECK(f.selected() == "Type: free_zones"); CHECK(f.shows("Zone: monster")); CHECK(f.shows("Compare: ge")); CHECK(f.shows("Value: 1"));
}

TEST(steps_can_be_added_moved_and_removed) {
    Fix f("steps", {"normal_summon", "end_turn"});
    f.ed.open(); f.keys("S"); CHECK(f.go("Required")); f.keys("XSX"); f.keys("O"); CHECK(f.selected() == "Required cards: 1");   // required card 1001
    CHECK(f.go("Steps:")); f.keys("XS"); f.keys("X"); CHECK(f.go("Card:")); f.keys("XX"); f.keys("O");   // step 1: normal_summon 1001
    CHECK(f.screen() == "Steps"); f.keys("SDX"); CHECK(f.screen() == "Step 2"); CHECK(f.selected() == "Action: end_turn"); f.keys("O");
    CHECK(f.shows("1. normal_summon 1001 Alpha Dragon")); CHECK(f.shows("2. end_turn"));
    f.keys("D1"); CHECK(f.selected() == "1. end_turn"); CHECK(f.shows("2. normal_summon"));     // moved up, the cursor follows
    f.keys("2"); CHECK(f.selected() == "2. end_turn");                                          // and back down
    f.keys("T"); CHECK(!f.shows("end_turn")); CHECK(f.shows("1. normal_summon"));
    f.keys("O"); CHECK(f.screen() == "Edit combo"); CHECK(f.selected() == "Steps: 1");
    CHECK(f.go("[ Save ]")); f.keys("X"); CHECK(f.ed.message() == "Saved");
    UserData u = f.reload(); CHECK(u.combos.size() == 1 && u.combos[0].steps.size() == 1 && u.combos[0].steps[0].card == 1001);
}

int main() {
    for (auto &t : tests()) { int before = g_fail; g_run++; t.f(); printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", t.n); }
    printf("%d tests, %d failed checks\n", g_run, g_fail); return g_fail ? 1 : 0;
}
