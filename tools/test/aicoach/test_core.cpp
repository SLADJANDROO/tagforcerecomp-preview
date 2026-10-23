/* Unit tests for plugins/aicoach_core (Milestone 2). Run: sh tools/test/aicoach/run_tests.sh */
#include "store.h"
#include <stdio.h>
#include <fstream>
using namespace aicoach;
namespace fs = std::filesystem;

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define TEST(name) static void name(); struct R_##name { R_##name() { tests().push_back({#name, name}); } } r_##name; static void name()
struct T { const char *n; void (*f)(); };
static std::vector<T> &tests() { static std::vector<T> v; return v; }

static bool has(const Issues &v, const std::string &pathpart, Issue::Severity s) { for (auto &i : v) if (i.sev == s && i.path.find(pathpart) != std::string::npos) return true; return false; }
static Condition cond(const char *type) { Condition c; c.type = type; return c; }
static Step step(const char *id, const char *action, uint32_t card = 0) { Step s; s.id = id; s.action = action; s.card = card; return s; }
static Combo good_combo(const char *id = "test.combo") {
    Combo c; c.id = id; c.name = "Test combo"; c.description = "d"; c.character = 2; c.decks = {1}; c.priority = 200;
    c.required_cards = {1001, 1002}; c.optional_cards = {1003};
    Condition h = cond("card_in_hand"); h.card = 1001; c.conditions = {h};
    Step a = step("s1", "normal_summon", 1001);
    Step b = step("s2", "activate_spell", 1002); Condition ok = cond("step_succeeded"); ok.step = "s1"; b.conditions = {ok}; b.on_fail.policy = "skip";
    Step e = step("s3", "end_turn");
    a.branches.push_back({{cond("action_legal")}, "s3"});
    c.steps = {a, b, e}; c.success = {ok};
    return c;
}
static std::string tmpdir() { static std::string d = (fs::temp_directory_path() / "aicoach_test").string(); fs::remove_all(d); fs::create_directories(d); return d; }
static ValidationContext noctx;

/* ---------------- JSON ---------------- */
TEST(json_roundtrip_and_rejects) {
    json::Value v; std::string e;
    CHECK(json::parse("{\"a\":[1,2.5,\"x\\u00e9\\n\",true,null],\"b\":{}}", v, e));
    json::Value v2; CHECK(json::parse(json::dump(v), v2, e)); CHECK(json::dump(v) == json::dump(v2));
    const char *bad[] = {"", "{", "{\"a\":1,}", "[1,]", "{\"a\":1} x", "{\"a\":1,\"a\":2}", "01", "1.", "\"\\q\"", "\"abc", "[NaN]", "{'a':1}", "\"\\ud800\"", "-", "tru"};
    for (auto *b : bad) { json::Value t; std::string er; if (json::parse(b, t, er)) { printf("  accepted bad json: %s\n", b); g_fail++; } else CHECK(!er.empty()); }
    std::string deep(40, '['); deep += std::string(40, ']'); CHECK(!json::parse(deep, v, e));
    CHECK(json::parse("\xEF\xBB\xBF{}", v, e));   /* BOM tolerated */
}

/* ---------------- model validation ---------------- */
TEST(valid_combo_has_no_errors) { Issues is; validate_combo(good_combo(), noctx, "c", is); CHECK(!has_errors(is)); }
TEST(combo_json_is_stable) {
    Combo c = good_combo(); json::Value j = to_json(c); Issues is; Combo d; CHECK(combo_from_json(j, d, "c", is)); CHECK(is.empty());
    CHECK(json::dump(to_json(d)) == json::dump(j));
}
TEST(required_fields_and_ranges) {
    Combo c = good_combo(); c.id = "Bad Id"; c.name = ""; c.priority = 5000; c.character = 999; c.steps.clear(); Issues is; validate_combo(c, noctx, "c", is);
    CHECK(has(is, "c.id", Issue::Error)); CHECK(has(is, "c.name", Issue::Error)); CHECK(has(is, "c.priority", Issue::Error)); CHECK(has(is, "c.character", Issue::Error)); CHECK(has(is, "c.steps", Issue::Error));
}
TEST(unknown_character_and_deck_ids) {
    Combo c = good_combo(); c.decks = {351}; Issues is; validate_combo(c, noctx, "c", is); CHECK(has(is, "decks[0]", Issue::Error));
    c = good_combo(); c.character = 0; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "c.decks", Issue::Error));   /* decks need a character */
    c = good_combo(); c.character = 0; c.decks.clear(); is.clear(); validate_combo(c, noctx, "c", is); CHECK(!has_errors(is));     /* shared combo, any character */
}
TEST(card_ids) {
    Combo c = good_combo(); c.steps[0].card = 0; Issues is; validate_combo(c, noctx, "c", is); CHECK(has(is, "steps[0].card", Issue::Error));
    c = good_combo(); c.required_cards.push_back(0x2000); is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "required_cards", Issue::Error));
    ValidationContext ctx; ctx.card_exists = [](uint32_t id) { return id != 1002; };   /* unknown id: warning only, combo stays valid */
    c = good_combo(); is.clear(); validate_combo(c, ctx, "c", is); CHECK(!has_errors(is)); CHECK(has(is, "required_cards[1]", Issue::Warning));
}
TEST(deck_membership_is_a_warning) {
    ValidationContext ctx; ctx.deck_contains = [](int, uint32_t card) { return card != 1002; };
    Issues is; validate_combo(good_combo(), ctx, "c", is); CHECK(!has_errors(is)); CHECK(has(is, "required_cards", Issue::Warning));
}
TEST(step_structure) {
    Combo c = good_combo(); c.steps[1].id = "s1"; Issues is; validate_combo(c, noctx, "c", is); CHECK(has(is, "steps[1].id", Issue::Error));   /* duplicate */
    c = good_combo(); c.steps[0].branches[0].goto_step = "nope"; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "branches[0].goto", Issue::Error));
    c = good_combo(); c.steps[1].on_fail.policy = "goto"; c.steps[1].on_fail.goto_step = "s1"; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "on_fail.goto", Issue::Error));   /* backward = loop */
    c = good_combo(); c.steps[1].on_fail.policy = "goto"; c.steps[1].on_fail.goto_step = "s3"; is.clear(); validate_combo(c, noctx, "c", is); CHECK(!has_errors(is));
    c = good_combo(); c.steps[0].conditions = c.steps[1].conditions; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "steps[0].conditions[0].step", Issue::Error));   /* refers to a step that is not earlier */
    c = good_combo(); c.steps[0].action = "run_script"; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "steps[0].action", Issue::Error));
    c = good_combo(); c.steps[0].target_policy = "specific_card"; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "target_card", Issue::Error));
    c = good_combo(); c.steps[1].on_fail.policy = "explode"; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "on_fail.policy", Issue::Error));
    c = good_combo(); c.steps[0].conditions = {cond("monster_position")}; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "conditions[0].card", Issue::Error)); CHECK(has(is, "conditions[0].position", Issue::Error));
    c = good_combo(); c.steps[0].conditions = {cond("lisp_eval")}; is.clear(); validate_combo(c, noctx, "c", is); CHECK(has(is, "conditions[0].type", Issue::Error));   /* closed vocabulary: no code */
}
TEST(capabilities_gate_actions) {
    Capabilities caps; caps.actions = {"normal_summon", "end_turn"}; ValidationContext ctx; ctx.caps = &caps;
    Issues is; validate_combo(good_combo(), ctx, "c", is); CHECK(has(is, "steps[1].action", Issue::Error)); CHECK(!has(is, "steps[0].action", Issue::Error));
}
TEST(from_json_type_errors) {
    json::Value v; std::string e; Combo c; Issues is;
    json::parse("{\"id\":5}", v, e); CHECK(!combo_from_json(v, c, "c", is)); CHECK(has(is, "c.id", Issue::Error));
    is.clear(); json::parse("{\"id\":\"x\",\"steps\":[{\"id\":\"a\",\"card\":-3}]}", v, e); CHECK(!combo_from_json(v, c, "c", is));
    is.clear(); json::parse("{\"id\":\"x\",\"mystery\":1}", v, e); CHECK(combo_from_json(v, c, "c", is)); CHECK(has(is, "mystery", Issue::Warning));   /* forward-compatible */
}
TEST(profile_validation) {
    Profile p; p.id = "p"; p.name = "P"; p.character = 2; Issues is; validate_profile(p, "p", is); CHECK(!has_errors(is));
    p.character = 1; p.aggression = 101; p.fallback = "x"; is.clear(); validate_profile(p, "p", is); CHECK(has_errors(is));
}

/* ---------------- configuration ---------------- */
static std::string defaults_text() {
    Defaults d; Profile p; p.id = "jaden"; p.name = "Jaden"; p.character = 2; d.profiles = {p};
    d.combos = {good_combo("d.one"), good_combo("d.two")}; return serialize_defaults(d);
}
TEST(load_shipped_defaults_file) {
    std::string t; CHECK(read_file(fs::path(SRCDIR) / "plugins/aicoach_core/data/defaults.json", t));
    Defaults d; LoadResult r = load_defaults(t, d, noctx); CHECK(r.status == LoadStatus::Ok); CHECK(!has_errors(r.issues)); CHECK(d.profiles.size() == 4);
}
TEST(defaults_roundtrip_and_malformed) {
    Defaults d; LoadResult r = load_defaults(defaults_text(), d, noctx); CHECK(r.status == LoadStatus::Ok); CHECK(d.combos.size() == 2); CHECK(serialize_defaults(d) == defaults_text());
    r = load_defaults("{ nope", d, noctx); CHECK(r.status == LoadStatus::Malformed); CHECK(d.combos.empty());
    r = load_defaults("{\"kind\":\"aicoach.user\",\"schema_version\":1}", d, noctx); CHECK(r.status == LoadStatus::Malformed);   /* wrong file kind */
    r = load_defaults("{\"kind\":\"aicoach.defaults\"}", d, noctx); CHECK(r.status == LoadStatus::Malformed);
}
TEST(bad_combo_is_dropped_good_ones_kept) {
    Defaults d; Combo bad = good_combo("d.bad"); bad.steps[0].action = "nonsense"; Defaults src; src.combos = {good_combo("d.ok"), bad, good_combo("d.ok2")};
    LoadResult r = load_defaults(serialize_defaults(src), d, noctx); CHECK(r.status == LoadStatus::Ok); CHECK(d.combos.size() == 2); CHECK(has(r.issues, "combos[1]", Issue::Error));
}
TEST(duplicate_ids_ignored) {
    Defaults d, src; src.combos = {good_combo("same"), good_combo("same")}; LoadResult r = load_defaults(serialize_defaults(src), d, noctx); CHECK(d.combos.size() == 1); CHECK(has(r.issues, ".id", Issue::Warning));
}
TEST(unknown_card_ids_load_with_warning) {
    ValidationContext ctx; ctx.card_exists = [](uint32_t) { return false; }; Defaults d, src; src.combos = {good_combo("c1")};
    LoadResult r = load_defaults(serialize_defaults(src), d, ctx); CHECK(d.combos.size() == 1); CHECK(!has_errors(r.issues));
}
TEST(user_overrides_do_not_touch_defaults) {
    Defaults d; load_defaults(defaults_text(), d, noctx); const std::string before = serialize_defaults(d);
    UserData u; u.patches["d.one"].has_enabled = true; u.patches["d.one"].enabled = false; u.patches["d.two"].has_priority = true; u.patches["d.two"].priority = 7;
    Combo mine = good_combo("u.mine"); mine.name = "Mine"; u.combos.push_back(mine);
    Combo repl = good_combo("d.two"); repl.name = "Replaced"; repl.priority = 900; u.combos.push_back(repl);
    Issues is; Effective e = merge(d, u, is);
    CHECK(serialize_defaults(d) == before);   /* defaults untouched */
    CHECK(e.combos.size() == 3);
    for (auto &c : e.combos) { if (c.id == "d.one") { CHECK(!c.enabled); CHECK(c.origin == Combo::Default); } if (c.id == "d.two") { CHECK(c.name == "Replaced"); CHECK(c.priority == 7); CHECK(c.origin == Combo::UserOverride); } if (c.id == "u.mine") CHECK(c.origin == Combo::UserNew); }
    u.deleted = {"d.one"}; e = merge(d, u, is); CHECK(e.combos.size() == 2);
    u.patches["ghost"].has_enabled = true; is.clear(); merge(d, u, is); CHECK(has(is, "patches.ghost", Issue::Warning));
}
TEST(user_file_save_and_reload) {
    std::string dir = tmpdir(); UserStore s(fs::path(dir) / "user.json"); CHECK(s.load(noctx).status == LoadStatus::Missing);
    Combo c = good_combo("u.saved"); c.steps[0].branches.clear(); s.data.combos.push_back(c); s.data.patches["d.one"].has_priority = true; s.data.patches["d.one"].priority = 3; s.data.deleted = {"d.two"};
    std::string err; CHECK(s.save(err));
    UserStore s2(fs::path(dir) / "user.json"); LoadResult r = s2.load(noctx); CHECK(r.status == LoadStatus::Ok); CHECK(s2.data.combos.size() == 1); CHECK(s2.data.patches["d.one"].priority == 3); CHECK(s2.data.deleted.size() == 1);
    CHECK(serialize_user(s2.data) == serialize_user(s.data));
    s2.data.combos[0].name = "Changed"; CHECK(s2.save(err)); CHECK(fs::exists(fs::path(dir) / "user.json.bak"));   /* previous version kept */
    s2.reset(err); CHECK(!fs::exists(fs::path(dir) / "user.json")); CHECK(s2.data.combos.empty());
}
TEST(corrupt_user_file_is_moved_aside) {
    std::string dir = tmpdir(); fs::path f = fs::path(dir) / "user.json"; { std::ofstream o(f); o << "{ broken"; }
    UserStore s(f); LoadResult r = s.load(noctx); CHECK(r.status == LoadStatus::Malformed); CHECK(s.data.combos.empty());
    std::string err; CHECK(s.save(err)); CHECK(fs::exists(f.string() + ".corrupt")); UserStore s2(f); CHECK(s2.load(noctx).status == LoadStatus::Ok);
}
TEST(newer_schema_is_refused_and_preserved) {
    std::string dir = tmpdir(); fs::path f = fs::path(dir) / "user.json"; const std::string body = "{\"kind\":\"aicoach.user\",\"schema_version\":99,\"x\":1}\n"; { std::ofstream o(f, std::ios::binary); o << body; }
    UserStore s(f); CHECK(s.load(noctx).status == LoadStatus::TooNew); std::string err; CHECK(!s.save(err)); s.reset(err); CHECK(!err.empty());
    std::string now; read_file(f, now); CHECK(now == body);   /* untouched */
}
TEST(migration_chain) {
    MigrationTable mt; mt.target = 3;
    mt.steps[1] = [](json::Value &d, Issues &) { d.set("patches", json::Value::make_object()); return true; };
    mt.steps[2] = [](json::Value &d, Issues &) { const json::Value *old = d.find("old_deleted"); if (old) d.set("deleted", *old); return true; };
    UserData u; LoadResult r = load_user("{\"kind\":\"aicoach.user\",\"schema_version\":1,\"old_deleted\":[\"a\"]}", u, noctx, mt);
    CHECK(r.status == LoadStatus::Ok); CHECK(u.deleted.size() == 1);
    mt.steps.erase(2); r = load_user("{\"kind\":\"aicoach.user\",\"schema_version\":1}", u, noctx, mt); CHECK(r.status == LoadStatus::Malformed);   /* gap in the chain */
    r = load_user("{\"kind\":\"aicoach.user\",\"schema_version\":1}", u, noctx); CHECK(r.status == LoadStatus::Ok);   /* built-in: schema 1 is current */
}
TEST(import_export) {
    UserData a; a.combos.push_back(good_combo("u.a")); a.patches["d.one"].has_enabled = true; a.patches["d.one"].enabled = false;
    std::string exported = serialize_user(a);
    UserData b; b.combos.push_back(good_combo("u.b")); Combo old = good_combo("u.a"); old.name = "Old"; b.combos.push_back(old);
    Issues is; CHECK(import_user(exported, b, noctx, is)); CHECK(b.combos.size() == 2); for (auto &c : b.combos) if (c.id == "u.a") CHECK(c.name == "Test combo");
    CHECK(!b.patches["d.one"].enabled);
    UserData c; CHECK(!import_user("{{", c, noctx, is)); CHECK(c.combos.empty());   /* unusable import changes nothing */
    CHECK(!import_user(defaults_text(), c, noctx, is));                              /* wrong kind */
}
TEST(bad_patch_values_are_ignored) {
    UserData u; LoadResult r = load_user("{\"kind\":\"aicoach.user\",\"schema_version\":1,\"patches\":{\"x\":{\"priority\":99999},\"y\":{\"enabled\":false}}}", u, noctx);
    CHECK(r.status == LoadStatus::Ok); CHECK(!u.patches.count("x")); CHECK(u.patches.count("y")); CHECK(has(r.issues, "patches.x.priority", Issue::Error));
}

int main() {
    for (auto &t : tests()) { int before = g_fail; g_run++; t.f(); printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", t.n); }
    printf("%d tests, %d failed checks\n", g_run, g_fail); return g_fail ? 1 : 0;
}
