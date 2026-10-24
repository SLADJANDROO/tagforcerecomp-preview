#include "model.h"
#include <algorithm>
#include <stdio.h>

namespace aicoach {

bool has_errors(const Issues &v) { for (auto &i : v) if (i.sev == Issue::Error) return true; return false; }
static void err(Issues &o, const std::string &p, const std::string &m) { o.push_back({Issue::Error, p, m}); }
static void warn(Issues &o, const std::string &p, const std::string &m) { o.push_back({Issue::Warning, p, m}); }

/* ---------------------------------------------------------------- vocabulary */
const std::vector<ActionSpec> &action_specs() {
    static const std::vector<ActionSpec> v = {
        {"normal_summon", {"card"}}, {"set_monster", {"card"}}, {"flip_summon", {"card"}}, {"special_summon", {"card"}},
        {"change_position", {"card", "position"}}, {"activate_spell", {"card"}}, {"activate_trap", {"card"}}, {"set_spelltrap", {"card"}},
        {"activate_effect", {"card"}}, {"declare_attack", {"card"}}, {"select_card", {}}, {"end_phase", {}}, {"end_turn", {}},
    };
    return v;
}
const std::vector<const char *> &condition_types() {
    static const std::vector<const char *> v = {"card_in_hand", "card_on_field", "card_in_graveyard", "card_in_extra", "monster_position", "free_zones",
        "lp_compare", "opponent_monster_count", "phase_is", "step_succeeded", "step_failed", "action_legal", "card_unused"};
    return v;
}
bool is_known_action(const std::string &a) { for (auto &s : action_specs()) if (a == s.name) return true; return false; }
bool is_known_condition(const std::string &t) { for (auto *s : condition_types()) if (t == s) return true; return false; }
#define SETFN(name, ...) const std::set<std::string> &name() { static const std::set<std::string> s = __VA_ARGS__; return s; }
SETFN(positions, {"attack", "defense", "facedown_defense"})
SETFN(zones, {"monster", "spelltrap"})
SETFN(ops, {"lt", "le", "eq", "ge", "gt", "ne"})
SETFN(whos, {"self", "opponent"})
SETFN(phases, {"draw", "standby", "main1", "battle", "main2", "end"})
SETFN(target_policies, {"first_legal", "highest_atk", "lowest_atk", "highest_def", "lowest_def", "specific_card"})
SETFN(fail_policies, {"abort", "skip", "goto", "retry_once", "fallback"})
SETFN(interruptions, {"abort", "reevaluate", "resume"})
SETFN(fallbacks, {"original_ai", "better_ai"})

bool deck_belongs_to_character(int ch, int deck) {
    if (ch >= 2 && ch <= 39) { const int first = 1 + 6 * (ch - 2); if (deck >= first && deck < first + 6) return true; }
    if (ch >= 41 && ch <= 142 && ch != 85 && deck == ch + 199) return true;
    if (ch == 85 && (deck == 229 || deck == 230)) return true;     /* Colin: special decks (his rule slot 284 is a dummy) */
    if (ch == 40 && deck == 55) return true;                        /* Nightshroud duels with Atticus's level 1 deck */
    if (ch == 24 && deck == 109) return true;                       /* Blair's script also names Belowski's first deck */
    return false;
}

/* ---------------------------------------------------------------- validation */
static bool valid_id(const std::string &s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
static void check_card(uint32_t card, const std::string &p, const ValidationContext &ctx, Issues &o) {
    if (card == 0 || card > kMaxCardId) { err(o, p, "card id must be 1.." + std::to_string(kMaxCardId)); return; }
    if (ctx.card_exists && !ctx.card_exists(card)) warn(o, p, "card id " + std::to_string(card) + " is not in the card database (kept; the id may belong to another game version)");
}

void validate_condition(const Condition &c, const std::string &p, const std::set<std::string> &earlier, Issues &o) {
    if (!is_known_condition(c.type)) { err(o, p + ".type", "unknown condition type '" + c.type + "'"); return; }
    const std::string &t = c.type;
    auto need_card = [&] { if (c.card == 0 || c.card > kMaxCardId) err(o, p + ".card", "requires a card id 1.." + std::to_string(kMaxCardId)); };
    auto need_cmp = [&] { if (!ops().count(c.op)) err(o, p + ".op", "requires op in lt,le,eq,ge,gt,ne"); };
    if (t == "card_in_hand" || t == "card_on_field" || t == "card_in_graveyard" || t == "card_in_extra" || t == "card_unused") need_card();
    else if (t == "monster_position") { need_card(); if (!positions().count(c.position)) err(o, p + ".position", "requires attack, defense or facedown_defense"); }
    else if (t == "free_zones") { if (!zones().count(c.zone)) err(o, p + ".zone", "requires monster or spelltrap"); need_cmp(); if (c.value < 0 || c.value > 5) err(o, p + ".value", "must be 0..5"); }
    else if (t == "lp_compare") { if (!whos().count(c.who)) err(o, p + ".who", "requires self or opponent"); need_cmp(); if (c.value < 0 || c.value > 99999) err(o, p + ".value", "must be 0..99999"); }
    else if (t == "opponent_monster_count") { need_cmp(); if (c.value < 0 || c.value > 5) err(o, p + ".value", "must be 0..5"); }
    else if (t == "phase_is") { if (!phases().count(c.phase)) err(o, p + ".phase", "unknown phase"); }
    else if (t == "step_succeeded" || t == "step_failed") { if (c.step.empty()) err(o, p + ".step", "requires a step id"); else if (!earlier.count(c.step)) err(o, p + ".step", "'" + c.step + "' is not an earlier step of this combo"); }
    /* action_legal: refers to the step's own action; no parameters */
}
static void validate_conds(const std::vector<Condition> &v, const std::string &p, const std::set<std::string> &earlier, Issues &o) {
    if (v.size() > kMaxConditions) err(o, p, "too many conditions (max " + std::to_string(kMaxConditions) + ")");
    for (size_t i = 0; i < v.size(); i++) validate_condition(v[i], p + "[" + std::to_string(i) + "]", earlier, o);
}

void validate_combo(const Combo &c, const ValidationContext &ctx, const std::string &p, Issues &o) {
    if (!valid_id(c.id)) err(o, p + ".id", "id must be 1-64 chars of a-z 0-9 _ - .");
    if (c.name.empty()) err(o, p + ".name", "name is required"); else if (c.name.size() > 80) err(o, p + ".name", "name longer than 80 bytes");
    if (c.description.size() > 400) err(o, p + ".description", "description longer than 400 bytes");
    if (c.revision < 1) err(o, p + ".revision", "must be >= 1");
    if (c.character != 0 && (c.character < kMinCharacterId || c.character > kMaxCharacterId)) err(o, p + ".character", "must be 0 (any) or 2..151");
    if (c.character == 0 && !c.decks.empty()) err(o, p + ".decks", "decks need a character (deck ids are not unique across characters in the editor)");
    for (size_t i = 0; i < c.decks.size(); i++) {
        const std::string dp = p + ".decks[" + std::to_string(i) + "]";
        if (c.decks[i] < 0 || c.decks[i] > kMaxDeckId) err(o, dp, "deck id must be 0..350");
        else if (c.character != 0 && !deck_belongs_to_character(c.character, c.decks[i]))
            warn(o, dp, "deck " + std::to_string(c.decks[i]) + " is not one of character " + std::to_string(c.character) + "'s decks by the game's rules (kept; other game versions may differ)");
    }
    if (c.priority < 0 || c.priority > 1000) err(o, p + ".priority", "must be 0..1000");
    if (!interruptions().count(c.interruption)) err(o, p + ".interruption", "must be abort, reevaluate or resume");
    if (!c.preferred_phase.empty() && !phases().count(c.preferred_phase)) err(o, p + ".preferred_phase", "unknown phase");
    if (c.preferred_turn < 0 || c.preferred_turn > 99) err(o, p + ".preferred_turn", "must be 0..99");
    if (c.steps.empty()) err(o, p + ".steps", "a combo needs at least one step");
    if (c.steps.size() > kMaxSteps) err(o, p + ".steps", "too many steps (max " + std::to_string(kMaxSteps) + ")");
    for (size_t i = 0; i < c.required_cards.size(); i++) check_card(c.required_cards[i], p + ".required_cards[" + std::to_string(i) + "]", ctx, o);
    for (size_t i = 0; i < c.optional_cards.size(); i++) check_card(c.optional_cards[i], p + ".optional_cards[" + std::to_string(i) + "]", ctx, o);
    std::set<std::string> none; validate_conds(c.conditions, p + ".conditions", none, o);
    /* static check: a required card that no assigned deck contains can never be drawn (a warning: cards can also arrive during the duel) */
    if (ctx.deck_contains && c.character != 0) for (int d : c.decks) for (uint32_t rc : c.required_cards)
        if (rc && rc <= kMaxCardId && !ctx.deck_contains(d, rc)) warn(o, p + ".required_cards", "card " + std::to_string(rc) + " is not in deck " + std::to_string(d));

    std::map<std::string, size_t> index;
    for (size_t i = 0; i < c.steps.size(); i++) {
        const Step &s = c.steps[i]; const std::string sp = p + ".steps[" + std::to_string(i) + "]";
        if (!valid_id(s.id)) err(o, sp + ".id", "step id must be 1-64 chars of a-z 0-9 _ - ."); else if (index.count(s.id)) err(o, sp + ".id", "duplicate step id '" + s.id + "'"); else index[s.id] = i;
    }
    std::set<std::string> earlier;
    for (size_t i = 0; i < c.steps.size(); i++) {
        const Step &s = c.steps[i]; const std::string sp = p + ".steps[" + std::to_string(i) + "]";
        const ActionSpec *spec = nullptr; for (auto &a : action_specs()) if (s.action == a.name) spec = &a;
        if (!spec) err(o, sp + ".action", "unknown action '" + s.action + "'");
        else {
            if (ctx.caps && !ctx.caps->actions.count(s.action)) err(o, sp + ".action", "action '" + s.action + "' is not executable by the current engine");
            for (const char *r : spec->required) {
                std::string n = r;
                if (n == "card") check_card(s.card, sp + ".card", ctx, o);
                else if (n == "position" && !positions().count(s.position)) err(o, sp + ".position", "requires attack, defense or facedown_defense");
            }
            if (s.card && std::string(spec->name) != "select_card" && spec->required.empty()) warn(o, sp + ".card", "ignored by action '" + s.action + "'");
            if (s.action == "select_card" && s.card == 0 && s.target_policy.empty()) err(o, sp, "select_card needs a card or a target_policy");
            if (s.card && s.card <= kMaxCardId) {
                bool listed = std::find(c.required_cards.begin(), c.required_cards.end(), s.card) != c.required_cards.end() || std::find(c.optional_cards.begin(), c.optional_cards.end(), s.card) != c.optional_cards.end();
                if (!listed) warn(o, sp + ".card", "card " + std::to_string(s.card) + " is used by a step but not listed in required_cards or optional_cards");
            }
            if (s.action == "end_turn" && i + 1 != c.steps.size()) warn(o, sp, "steps after end_turn will never run");
        }
        if (!s.target_policy.empty()) {
            if (!target_policies().count(s.target_policy)) err(o, sp + ".target_policy", "unknown target policy");
            else if (s.target_policy == "specific_card") check_card(s.target_card, sp + ".target_card", ctx, o);
        }
        validate_conds(s.conditions, sp + ".conditions", earlier, o);
        validate_conds(s.expect, sp + ".expect", earlier, o);
        if (!fail_policies().count(s.on_fail.policy)) err(o, sp + ".on_fail.policy", "must be abort, skip, goto, retry_once or fallback");
        auto fwd = [&](const std::string &target, const std::string &tp) {
            auto it = index.find(target);
            if (target.empty()) err(o, tp, "requires a target step id");
            else if (it == index.end()) err(o, tp, "no step named '" + target + "'");
            else if (it->second <= i) err(o, tp, "goto must jump forward (backward jumps could loop forever)");
        };
        if (s.on_fail.policy == "goto") fwd(s.on_fail.goto_step, sp + ".on_fail.goto");
        for (size_t b = 0; b < s.branches.size(); b++) {
            validate_conds(s.branches[b].conditions, sp + ".branches[" + std::to_string(b) + "].conditions", earlier, o);
            fwd(s.branches[b].goto_step, sp + ".branches[" + std::to_string(b) + "].goto");
        }
        earlier.insert(s.id);
    }
    validate_conds(c.success, p + ".success", earlier, o);
}

void validate_profile(const Profile &pr, const std::string &p, Issues &o) {
    if (!valid_id(pr.id)) err(o, p + ".id", "id must be 1-64 chars of a-z 0-9 _ - .");
    if (pr.name.empty()) err(o, p + ".name", "name is required");
    if (pr.character < kMinCharacterId || pr.character > kMaxCharacterId) err(o, p + ".character", "must be 2..151");
    for (auto *f : {&pr.aggression, &pr.risk_tolerance, &pr.signature_bias}) if (*f < 0 || *f > 100) err(o, p, "aggression, risk_tolerance and signature_bias must be 0..100");
    if (pr.preferred_position != "auto" && !positions().count(pr.preferred_position)) err(o, p + ".preferred_position", "must be auto, attack, defense or facedown_defense");
    if (!target_policies().count(pr.attack_target_policy) || pr.attack_target_policy == "specific_card") err(o, p + ".attack_target_policy", "unknown or unsupported policy");
    if (!fallbacks().count(pr.fallback)) err(o, p + ".fallback", "must be original_ai or better_ai");
    for (size_t i = 0; i < pr.preferred_cards.size(); i++) if (pr.preferred_cards[i] == 0 || pr.preferred_cards[i] > kMaxCardId) err(o, p + ".preferred_cards[" + std::to_string(i) + "]", "card id must be 1.." + std::to_string(kMaxCardId));
}

/* ---------------------------------------------------------------- JSON out */
using json::Value;
static Value cards_json(const std::vector<uint32_t> &v) { Value a = Value::make_array(); for (auto x : v) a.push(Value::make_int(x)); return a; }
static Value cond_json(const Condition &c) {
    Value o = Value::make_object(); o.set("type", Value::make_string(c.type));
    if (c.negate) o.set("negate", Value::make_bool(true));
    if (c.card) o.set("card", Value::make_int(c.card));
    if (!c.position.empty()) o.set("position", Value::make_string(c.position));
    if (!c.zone.empty()) o.set("zone", Value::make_string(c.zone));
    if (!c.op.empty()) o.set("op", Value::make_string(c.op));
    if (!c.who.empty()) o.set("who", Value::make_string(c.who));
    if (!c.phase.empty()) o.set("phase", Value::make_string(c.phase));
    if (!c.step.empty()) o.set("step", Value::make_string(c.step));
    if (c.value) o.set("value", Value::make_int(c.value));
    return o;
}
static Value conds_json(const std::vector<Condition> &v) { Value a = Value::make_array(); for (auto &c : v) a.push(cond_json(c)); return a; }
Value to_json(const Combo &c) {
    Value o = Value::make_object();
    o.set("id", Value::make_string(c.id)); o.set("name", Value::make_string(c.name)); o.set("description", Value::make_string(c.description));
    o.set("revision", Value::make_int(c.revision)); o.set("character", Value::make_int(c.character));
    { Value d = Value::make_array(); for (int x : c.decks) d.push(Value::make_int(x)); o.set("decks", d); }
    o.set("enabled", Value::make_bool(c.enabled)); o.set("priority", Value::make_int(c.priority));
    o.set("conditions", conds_json(c.conditions)); o.set("required_cards", cards_json(c.required_cards)); o.set("optional_cards", cards_json(c.optional_cards));
    Value st = Value::make_array();
    for (auto &s : c.steps) {
        Value x = Value::make_object(); x.set("id", Value::make_string(s.id)); x.set("action", Value::make_string(s.action));
        if (s.card) x.set("card", Value::make_int(s.card));
        if (!s.position.empty()) x.set("position", Value::make_string(s.position));
        if (!s.target_policy.empty()) x.set("target_policy", Value::make_string(s.target_policy));
        if (s.target_card) x.set("target_card", Value::make_int(s.target_card));
        if (!s.conditions.empty()) x.set("conditions", conds_json(s.conditions));
        if (!s.expect.empty()) x.set("expect", conds_json(s.expect));
        { Value f = Value::make_object(); f.set("policy", Value::make_string(s.on_fail.policy)); if (!s.on_fail.goto_step.empty()) f.set("goto", Value::make_string(s.on_fail.goto_step)); x.set("on_fail", f); }
        if (!s.branches.empty()) { Value b = Value::make_array(); for (auto &br : s.branches) { Value y = Value::make_object(); y.set("conditions", conds_json(br.conditions)); y.set("goto", Value::make_string(br.goto_step)); b.push(y); } x.set("branches", b); }
        st.push(x);
    }
    o.set("steps", st); o.set("success", conds_json(c.success)); o.set("interruption", Value::make_string(c.interruption));
    { Value t = Value::make_array(); for (auto &x : c.tags) t.push(Value::make_string(x)); o.set("tags", t); }
    if (!c.preferred_phase.empty()) o.set("preferred_phase", Value::make_string(c.preferred_phase));
    if (c.preferred_turn) o.set("preferred_turn", Value::make_int(c.preferred_turn));
    return o;
}
Value to_json(const Profile &p) {
    Value o = Value::make_object();
    o.set("id", Value::make_string(p.id)); o.set("name", Value::make_string(p.name)); o.set("character", Value::make_int(p.character));
    o.set("aggression", Value::make_int(p.aggression)); o.set("risk_tolerance", Value::make_int(p.risk_tolerance)); o.set("signature_bias", Value::make_int(p.signature_bias));
    o.set("preferred_position", Value::make_string(p.preferred_position)); o.set("attack_target_policy", Value::make_string(p.attack_target_policy));
    o.set("preferred_cards", cards_json(p.preferred_cards)); o.set("fallback", Value::make_string(p.fallback)); o.set("enabled", Value::make_bool(p.enabled));
    return o;
}

/* ---------------------------------------------------------------- JSON in */
namespace {
struct R {   /* typed field reader that records type errors and unknown keys */
    const Value &v; const std::string &path; Issues &is; bool ok = true; std::set<std::string> seen;
    R(const Value &x, const std::string &p, Issues &i) : v(x), path(p), is(i) { if (v.type != Value::Object) { err(is, path, "expected an object"); ok = false; } }
    const Value *get(const char *k) { seen.insert(k); return ok ? v.find(k) : nullptr; }
    bool str(const char *k, std::string &o) { auto *x = get(k); if (!x) return true; if (x->type != Value::String) { err(is, path + "." + k, "expected a string"); return ok = false; } o = x->str; return true; }
    bool boolean(const char *k, bool &o) { auto *x = get(k); if (!x) return true; if (x->type != Value::Bool) { err(is, path + "." + k, "expected true or false"); return ok = false; } o = x->b; return true; }
    bool integer(const char *k, int &o) { auto *x = get(k); if (!x) return true; if (x->type != Value::Number || !x->is_int || x->num < -2e9 || x->num > 2e9) { err(is, path + "." + k, "expected an integer"); return ok = false; } o = (int)x->num; return true; }
    bool card(const char *k, uint32_t &o) { int t = (int)o; bool r = integer(k, t); if (r) { if (t < 0) { err(is, path + "." + k, "card id cannot be negative"); return ok = false; } o = (uint32_t)t; } return r; }
    void unknown() { if (ok) for (auto &p : v.obj) if (!seen.count(p.first)) warn(is, path + "." + p.first, "unknown field ignored"); }
};
bool cards_in(const Value *a, std::vector<uint32_t> &o, const std::string &p, Issues &is) {
    if (!a) return true;
    if (a->type != Value::Array) { err(is, p, "expected an array of card ids"); return false; }
    for (size_t i = 0; i < a->arr.size(); i++) { auto &x = a->arr[i]; if (x.type != Value::Number || !x.is_int || x.num < 0 || x.num > 2e9) { err(is, p + "[" + std::to_string(i) + "]", "expected a card id"); return false; } o.push_back((uint32_t)x.num); }
    return true;
}
bool cond_in(const Value &v, Condition &c, const std::string &p, Issues &is) {
    R r(v, p, is); if (!r.ok) return false;
    r.str("type", c.type); r.boolean("negate", c.negate); r.card("card", c.card); r.str("position", c.position); r.str("zone", c.zone); r.str("op", c.op);
    r.str("who", c.who); r.str("phase", c.phase); r.str("step", c.step); r.integer("value", c.value); r.unknown(); return r.ok;
}
bool conds_in(const Value *a, std::vector<Condition> &o, const std::string &p, Issues &is) {
    if (!a) return true;
    if (a->type != Value::Array) { err(is, p, "expected an array"); return false; }
    bool ok = true; for (size_t i = 0; i < a->arr.size(); i++) { Condition c; if (cond_in(a->arr[i], c, p + "[" + std::to_string(i) + "]", is)) o.push_back(c); else ok = false; }
    return ok;
}
}  // namespace

bool combo_from_json(const Value &v, Combo &out, const std::string &path, Issues &is) {
    R r(v, path, is); if (!r.ok) return false;
    Combo c;
    r.str("id", c.id); r.str("name", c.name); r.str("description", c.description); r.integer("revision", c.revision); r.integer("character", c.character);
    r.boolean("enabled", c.enabled); r.integer("priority", c.priority); r.str("interruption", c.interruption); r.str("preferred_phase", c.preferred_phase); r.integer("preferred_turn", c.preferred_turn);
    if (auto *d = r.get("decks")) { if (d->type != Value::Array) { err(is, path + ".decks", "expected an array"); r.ok = false; } else for (auto &x : d->arr) { if (x.type != Value::Number || !x.is_int) { err(is, path + ".decks", "expected integers"); r.ok = false; break; } c.decks.push_back((int)x.num); } }
    if (auto *t = r.get("tags")) { if (t->type != Value::Array) { err(is, path + ".tags", "expected an array"); r.ok = false; } else for (auto &x : t->arr) { if (x.type != Value::String) { err(is, path + ".tags", "expected strings"); r.ok = false; break; } c.tags.push_back(x.str); } }
    if (!cards_in(r.get("required_cards"), c.required_cards, path + ".required_cards", is)) r.ok = false;
    if (!cards_in(r.get("optional_cards"), c.optional_cards, path + ".optional_cards", is)) r.ok = false;
    if (!conds_in(r.get("conditions"), c.conditions, path + ".conditions", is)) r.ok = false;
    if (!conds_in(r.get("success"), c.success, path + ".success", is)) r.ok = false;
    if (auto *st = r.get("steps")) {
        if (st->type != Value::Array) { err(is, path + ".steps", "expected an array"); r.ok = false; }
        else for (size_t i = 0; i < st->arr.size(); i++) {
            const std::string sp = path + ".steps[" + std::to_string(i) + "]"; R sr(st->arr[i], sp, is); if (!sr.ok) { r.ok = false; continue; }
            Step s; sr.str("id", s.id); sr.str("action", s.action); sr.card("card", s.card); sr.str("position", s.position); sr.str("target_policy", s.target_policy); sr.card("target_card", s.target_card);
            if (!conds_in(sr.get("conditions"), s.conditions, sp + ".conditions", is)) sr.ok = false;
            if (!conds_in(sr.get("expect"), s.expect, sp + ".expect", is)) sr.ok = false;
            if (auto *f = sr.get("on_fail")) { R fr(*f, sp + ".on_fail", is); if (fr.ok) { fr.str("policy", s.on_fail.policy); fr.str("goto", s.on_fail.goto_step); fr.unknown(); } sr.ok = sr.ok && fr.ok; }
            if (auto *b = sr.get("branches")) {
                if (b->type != Value::Array) { err(is, sp + ".branches", "expected an array"); sr.ok = false; }
                else for (size_t k = 0; k < b->arr.size(); k++) { const std::string bp = sp + ".branches[" + std::to_string(k) + "]"; R br(b->arr[k], bp, is); if (!br.ok) { sr.ok = false; continue; }
                    Branch x; if (!conds_in(br.get("conditions"), x.conditions, bp + ".conditions", is)) br.ok = false; br.str("goto", x.goto_step); br.unknown(); sr.ok = sr.ok && br.ok; s.branches.push_back(x); }
            }
            sr.unknown(); r.ok = r.ok && sr.ok; c.steps.push_back(s);
        }
    }
    r.unknown(); if (!r.ok) return false; out = c; return true;
}
bool profile_from_json(const Value &v, Profile &out, const std::string &path, Issues &is) {
    R r(v, path, is); if (!r.ok) return false; Profile p;
    r.str("id", p.id); r.str("name", p.name); r.integer("character", p.character); r.integer("aggression", p.aggression); r.integer("risk_tolerance", p.risk_tolerance);
    r.integer("signature_bias", p.signature_bias); r.str("preferred_position", p.preferred_position); r.str("attack_target_policy", p.attack_target_policy);
    r.str("fallback", p.fallback); r.boolean("enabled", p.enabled);
    if (!cards_in(r.get("preferred_cards"), p.preferred_cards, path + ".preferred_cards", is)) r.ok = false;
    r.unknown(); if (!r.ok) return false; out = p; return true;
}

}  // namespace
