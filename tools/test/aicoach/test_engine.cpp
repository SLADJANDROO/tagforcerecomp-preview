/* Unit tests for plugins/aicoach_core/engine.* (Milestone 3a). Run: sh tools/test/aicoach/run_tests.sh
 * The game is replaced by FakeView / FakeExec, so these tests prove the LOGIC (selection, sequencing, recovery, determinism) and nothing about the real game. */
#include "engine.h"
#include <stdio.h>
#include <functional>
#include <map>
using namespace aicoach;

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define TEST(name) static void name(); struct R_##name { R_##name() { tests().push_back({#name, name}); } } r_##name; static void name()
struct T { const char *n; void (*f)(); };
static std::vector<T> &tests() { static std::vector<T> v; return v; }

/* ---------------------------------------------------------------- fakes */
struct FakeView : DuelView {
    std::set<uint32_t> hand, field, gy, extra, unknown;           /* `unknown`: every question about these cards answers Unknown */
    std::map<uint32_t, std::string> pos;
    int mz = 5, sz = 5, lp_self = 8000, lp_opp = 8000, opp_n = 0, turn_no = 1; std::string ph = "main1";
    std::set<std::string> illegal, legal_unknown;                  /* "action:card" */
    static std::string key(const Step &s) { return s.action + ":" + std::to_string(s.card); }
    Tri has_card(Where w, uint32_t c) const override {
        if (unknown.count(c)) return Tri::Unknown;
        const std::set<uint32_t> &s = w == Where::Hand ? hand : w == Where::Field ? field : w == Where::Graveyard ? gy : extra; return tri_of(s.count(c) != 0);
    }
    Tri monster_position(uint32_t c, const std::string &p) const override { if (unknown.count(c)) return Tri::Unknown; auto it = pos.find(c); return it == pos.end() ? Tri::No : tri_of(it->second == p); }
    int free_zones(const std::string &z) const override { return z == "monster" ? mz : sz; }
    int life_points(bool self) const override { return self ? lp_self : lp_opp; }
    int opponent_monster_count() const override { return opp_n; }
    std::string phase() const override { return ph; }
    int turn() const override { return turn_no; }
    Tri action_legal(const Step &s) const override { return illegal.count(key(s)) ? Tri::No : legal_unknown.count(key(s)) ? Tri::Unknown : Tri::Yes; }
};
struct FakeExec : ActionExecutor {
    std::vector<std::string> subs;                                 /* every submission, in order */
    std::map<std::string, std::vector<ActionState>> script;        /* per "action:card": poll results for each successive submission attempt are concatenated */
    std::map<std::string, std::function<void()>> effect;           /* run when the action first reports Succeeded/NeedsInput */
    std::set<std::string> refuse; int cancels = 0; size_t cur = 0; std::string cur_key; bool fired = false;
    std::map<std::string, size_t> pos;                             /* next script index per key */
    bool submit(const Step &s, std::string &why) override {
        const std::string k = FakeView::key(s); if (refuse.count(k)) { why = "refused " + k; return false; }
        subs.push_back(k); cur_key = k; fired = false; return true;
    }
    ActionState poll(std::string &why) override {
        auto it = script.find(cur_key); ActionState st = ActionState::Succeeded;
        if (it != script.end()) { size_t &i = pos[cur_key]; st = i < it->second.size() ? it->second[i] : it->second.back(); if (st != ActionState::Pending) i++; else if (i + 1 < it->second.size()) { /* Pending entries are consumed one poll at a time */ i++; } }
        if (st == ActionState::Failed) why = "scripted failure";
        if ((st == ActionState::Succeeded || st == ActionState::NeedsInput) && !fired) { fired = true; auto e = effect.find(cur_key); if (e != effect.end()) e->second(); }
        return st;
    }
    void cancel() override { cancels++; }
};

/* ---------------------------------------------------------------- builders */
static Condition cond(const char *type, uint32_t card = 0) { Condition c; c.type = type; c.card = card; return c; }
static Condition stepc(const char *type, const char *step) { Condition c; c.type = type; c.step = step; return c; }
static Step mk(const char *id, const char *action, uint32_t card = 0, const char *policy = "abort") { Step s; s.id = id; s.action = action; s.card = card; s.on_fail.policy = policy; return s; }
static Combo combo(const char *id, std::vector<Step> steps, int prio = 200) {
    Combo c; c.id = id; c.name = id; c.priority = prio; c.steps = std::move(steps);
    for (auto &s : c.steps) if (s.card && s.action != "select_card") c.required_cards.push_back(s.card);
    return c;
}
/* runs the combo until it stops (or `max` ticks); returns ticks used */
static int run(ComboRunner &r, FakeView &v, FakeExec &x, AttemptLedger &l, int max = 200) {
    int t = 1; for (; t <= max && r.running(); t++) r.tick(v, x, l, (uint64_t)t); return t - 1;
}
static bool has_kind(const ComboRunner &r, const char *kind) { for (auto &e : r.trace()) if (e.kind == kind) return true; return false; }

/* ---------------------------------------------------------------- conditions */
TEST(conditions_read_the_view) {
    FakeView v; v.hand = {1001}; v.field = {2001}; v.gy = {3001}; v.extra = {4001}; v.pos[2001] = "attack"; v.mz = 3; v.lp_self = 1500; v.opp_n = 2; v.ph = "battle";
    AttemptLedger l; EvalEnv e; e.view = &v; e.ledger = &l;
    auto yes = [&](Condition c) { return eval_condition(c, e).value == Tri::Yes; };
    auto no = [&](Condition c) { return eval_condition(c, e).value == Tri::No; };
    CHECK(yes(cond("card_in_hand", 1001))); CHECK(no(cond("card_in_hand", 2001)));
    CHECK(yes(cond("card_on_field", 2001))); CHECK(yes(cond("card_in_graveyard", 3001))); CHECK(yes(cond("card_in_extra", 4001)));
    Condition p = cond("monster_position", 2001); p.position = "attack"; CHECK(yes(p)); p.position = "defense"; CHECK(no(p));
    Condition z = cond("free_zones"); z.zone = "monster"; z.op = "ge"; z.value = 3; CHECK(yes(z)); z.value = 4; CHECK(no(z)); z.op = "lt"; CHECK(yes(z));
    Condition lp = cond("lp_compare"); lp.who = "self"; lp.op = "le"; lp.value = 1500; CHECK(yes(lp)); lp.who = "opponent"; CHECK(no(lp));
    Condition om = cond("opponent_monster_count"); om.op = "eq"; om.value = 2; CHECK(yes(om));
    Condition ph = cond("phase_is"); ph.phase = "battle"; CHECK(yes(ph)); ph.phase = "main1"; CHECK(no(ph));
    Condition n = cond("card_in_hand", 1001); n.negate = true; CHECK(no(n));
    CHECK(yes(cond("card_unused", 1001))); l.note_card_used(1001); CHECK(no(cond("card_unused", 1001)));
    CHECK(!eval_condition(cond("card_in_hand", 1001), e).text.empty());       /* diagnostics always carry text */
}
TEST(unknown_state_is_explicit_and_never_satisfies_a_requirement) {
    FakeView v; v.unknown = {7777}; v.ph = ""; v.lp_self = -1; AttemptLedger l; EvalEnv e; e.view = &v; e.ledger = &l;
    CHECK(eval_condition(cond("card_in_hand", 7777), e).value == Tri::Unknown);
    Condition neg = cond("card_in_hand", 7777); neg.negate = true; CHECK(eval_condition(neg, e).value == Tri::Unknown);   /* NOT unknown is still unknown */
    Condition ph = cond("phase_is"); ph.phase = "main1"; CHECK(eval_condition(ph, e).value == Tri::Unknown);
    Condition lp = cond("lp_compare"); lp.who = "self"; lp.op = "ge"; lp.value = 1; CHECK(eval_condition(lp, e).value == Tri::Unknown);
    CHECK(eval_all({cond("card_in_hand", 7777)}, e) == Tri::Unknown);
    v.hand = {1}; CHECK(eval_all({cond("card_in_hand", 7777), cond("card_in_hand", 2)}, e) == Tri::No);   /* a definite No beats Unknown */
    EvalEnv none; CHECK(eval_condition(cond("card_in_hand", 1), none).value == Tri::Unknown);              /* no view at all */
}

/* ---------------------------------------------------------------- ranking */
TEST(ranking_picks_eligible_combos_deterministically) {
    FakeView v; v.hand = {1001, 1002, 1003, 1004}; v.turn_no = 3; AttemptLedger l;
    std::vector<Combo> cs;
    cs.push_back(combo("b.same", {mk("s1", "normal_summon", 1001)}, 300));
    cs.push_back(combo("a.same", {mk("s1", "normal_summon", 1002)}, 300));
    cs.push_back(combo("c.low", {mk("s1", "normal_summon", 1003)}, 100));
    Combo opt = combo("d.opt", {mk("s1", "normal_summon", 1004)}, 100); opt.optional_cards = {1001}; cs.push_back(opt);   /* optional card present: +5 */
    SelectionContext ctx; ctx.character = 2; ctx.deck = 1;
    auto r = rank_combos(cs, ctx, v, l);
    CHECK(r.size() == 4); CHECK(r[0].id == "a.same"); CHECK(r[1].id == "b.same");   /* tie broken by id, not by input order */
    CHECK(r[2].id == "d.opt"); CHECK(r[2].score == 105); CHECK(r[3].id == "c.low");
    for (auto &c : r) CHECK(c.eligible);
    auto r2 = rank_combos(cs, ctx, v, l); for (size_t i = 0; i < r.size(); i++) CHECK(r[i].id == r2[i].id);
}
TEST(ranking_rejects_ineligible_combos_with_a_reason) {
    FakeView v; v.hand = {1001}; v.turn_no = 2; v.ph = "main1"; AttemptLedger l; SelectionContext ctx; ctx.character = 2; ctx.deck = 1;
    std::vector<Combo> cs;
    Combo ok = combo("ok", {mk("s1", "normal_summon", 1001)}); cs.push_back(ok);
    Combo dis = ok; dis.id = "disabled"; dis.enabled = false; cs.push_back(dis);
    Combo ch = ok; ch.id = "other.char"; ch.character = 3; cs.push_back(ch);
    Combo dk = ok; dk.id = "other.deck"; dk.character = 2; dk.decks = {9}; cs.push_back(dk);
    Combo miss = combo("missing", {mk("s1", "normal_summon", 5001)}); cs.push_back(miss);
    Combo sc = ok; sc.id = "start.cond"; sc.conditions = {cond("card_on_field", 1001)}; cs.push_back(sc);
    Combo pt = ok; pt.id = "turn"; pt.preferred_turn = 5; cs.push_back(pt);
    Combo pp = ok; pp.id = "phase"; pp.preferred_phase = "battle"; cs.push_back(pp);
    Combo done = ok; done.id = "achieved"; done.success = {cond("card_in_hand", 1001)}; cs.push_back(done);
    Combo blk = ok; blk.id = "blocked"; cs.push_back(blk); l.note_failure("blocked", "s1", "normal_summon", 1001);
    auto r = rank_combos(cs, ctx, v, l);
    std::map<std::string, const Candidate *> by; for (auto &c : r) by[c.id] = &c;
    CHECK(by["ok"]->eligible); CHECK(r[0].id == "ok");
    const char *bad[] = {"disabled", "other.char", "other.deck", "missing", "start.cond", "turn", "phase", "achieved", "blocked"};
    for (const char *id : bad) { CHECK(!by[id]->eligible); CHECK(!by[id]->reason.empty()); CHECK(by[id]->reason != "eligible"); }
    CHECK(by["missing"]->reason.find("5001") != std::string::npos);
    CHECK(by["start.cond"]->reason.find("start condition failed") != std::string::npos); CHECK(!by["start.cond"]->checks.empty());
    CHECK(by["achieved"]->reason.find("already") != std::string::npos);
    /* unknown state: not eligible, and says it is the STATE that is unavailable rather than claiming the card is missing */
    FakeView u; u.unknown = {1001}; auto ru = rank_combos({ok}, ctx, u, l); CHECK(!ru[0].eligible); CHECK(ru[0].reason.find("unavailable") != std::string::npos);
    FakeView nt; nt.turn_no = 0; ru = rank_combos({pt}, ctx, nt, l); CHECK(!ru[0].eligible); CHECK(ru[0].reason.find("unavailable") != std::string::npos);
}
TEST(signature_bias_gates_low_priority_combos) {
    FakeView v; v.hand = {1001}; AttemptLedger l; SelectionContext ctx; Profile p; p.signature_bias = 0;
    Combo lo = combo("lo", {mk("s1", "normal_summon", 1001)}, 300), hi = combo("hi", {mk("s1", "normal_summon", 1001)}, 700);
    ctx.profile = &p; auto r = rank_combos({lo, hi}, ctx, v, l); CHECK(r[0].id == "hi" && r[0].eligible); CHECK(!r[1].eligible);
    p.signature_bias = 100; r = rank_combos({lo, hi}, ctx, v, l); CHECK(r[0].eligible && r[1].eligible);
    CHECK(bias_min_priority(nullptr) == 0); p.signature_bias = 50; CHECK(bias_min_priority(&p) == 250);
}
TEST(a_failed_or_finished_combo_is_not_reselected_in_the_same_turn) {
    FakeView v; v.hand = {1001}; FakeExec x; AttemptLedger l; ComboRunner r; SelectionContext ctx;
    Combo c = combo("c", {mk("s1", "normal_summon", 1001)}); x.script["normal_summon:1001"] = {ActionState::Failed};
    CHECK(rank_combos({c}, ctx, v, l)[0].eligible);
    CHECK(r.begin(c, l, 0)); run(r, v, x, l); CHECK(r.outcome() == Outcome::Aborted);
    CHECK(!rank_combos({c}, ctx, v, l)[0].eligible);                     /* failed: never retried by selection */
    l.new_turn(); CHECK(rank_combos({c}, ctx, v, l)[0].eligible);        /* a new turn is a fresh start */
    AttemptLedger l2; CHECK(!l2.same_action_failed("c", "s1", "normal_summon", 1001)); l2.note_failure("c", "s1", "normal_summon", 1001);
    CHECK(l2.same_action_failed("c", "s1", "normal_summon", 1001)); CHECK(!l2.same_action_failed("c", "s1", "normal_summon", 1002)); l2.new_turn(); CHECK(!l2.same_action_failed("c", "s1", "normal_summon", 1001));
}

/* ---------------------------------------------------------------- runner: sequencing */
TEST(runner_executes_steps_in_order_and_never_runs_ahead_of_the_game) {
    FakeView v; v.hand = {1001, 1002}; FakeExec x; AttemptLedger l; ComboRunner r;
    Step s1 = mk("s1", "normal_summon", 1001); s1.expect = {cond("card_on_field", 1001)};
    Step s2 = mk("s2", "activate_spell", 1002); s2.conditions = {stepc("step_succeeded", "s1")};
    Step s3 = mk("s3", "end_turn");
    Combo c = combo("seq", {s1, s2, s3}); c.success = {cond("card_on_field", 1001)};
    x.script["normal_summon:1001"] = {ActionState::Pending, ActionState::Succeeded};
    x.effect["normal_summon:1001"] = [&] { v.hand.erase(1001); v.field.insert(1001); };
    CHECK(r.begin(c, l, 0)); CHECK(r.running());
    r.tick(v, x, l, 1); CHECK(x.subs.size() == 1);
    r.tick(v, x, l, 2); CHECK(x.subs.size() == 1);                          /* still pending: nothing else is submitted */
    r.tick(v, x, l, 3); CHECK(x.subs.size() == 1);                          /* it resolved this tick, but the next step waits for a fresh tick */
    r.tick(v, x, l, 4); CHECK(x.subs.size() == 2 && x.subs[1] == "activate_spell:1002");
    run(r, v, x, l); CHECK(!r.running()); CHECK(r.outcome() == Outcome::Completed); CHECK(x.subs.size() == 3 && x.subs[2] == "end_turn:0");
    CHECK(l.card_used(1001) && l.card_used(1002)); CHECK(l.blocked("seq"));
    CHECK(has_kind(r, "begin") && has_kind(r, "submit") && has_kind(r, "ok") && has_kind(r, "end"));
}
TEST(preconditions_legality_and_refusal_stop_before_anything_is_sent) {
    FakeView v; v.hand = {1001}; AttemptLedger l; ComboRunner r;
    { FakeExec x; Step s = mk("s1", "normal_summon", 1001); s.conditions = {cond("card_on_field", 1001)}; AttemptLedger l1; CHECK(r.begin(combo("p", {s}), l1, 0)); run(r, v, x, l1);
      CHECK(r.outcome() == Outcome::Aborted); CHECK(x.subs.empty()); CHECK(r.reason().find("precondition failed") != std::string::npos); }
    { FakeExec x; FakeView v2 = v; v2.unknown = {1001}; Step s = mk("s1", "normal_summon", 1001); s.conditions = {cond("card_in_hand", 1001)}; AttemptLedger l1; CHECK(r.begin(combo("p", {s}), l1, 0)); run(r, v2, x, l1);
      CHECK(r.outcome() == Outcome::Aborted); CHECK(x.subs.empty()); CHECK(r.reason().find("unknown") != std::string::npos); }   /* unknown precondition: fail safe */
    { FakeExec x; FakeView v2 = v; v2.illegal = {"normal_summon:1001"}; AttemptLedger l1; CHECK(r.begin(combo("p", {mk("s1", "normal_summon", 1001)}), l1, 0)); run(r, v2, x, l1);
      CHECK(r.outcome() == Outcome::Aborted); CHECK(x.subs.empty()); CHECK(r.reason().find("does not allow") != std::string::npos); }
    { FakeExec x; x.refuse = {"normal_summon:1001"}; AttemptLedger l1; CHECK(r.begin(combo("p", {mk("s1", "normal_summon", 1001)}), l1, 0)); run(r, v, x, l1);
      CHECK(r.outcome() == Outcome::Aborted); CHECK(r.reason().find("refused") != std::string::npos); }
    { FakeExec x; FakeView v2 = v; v2.legal_unknown = {"normal_summon:1001"}; AttemptLedger l1; CHECK(r.begin(combo("p", {mk("s1", "normal_summon", 1001)}), l1, 0)); run(r, v2, x, l1);
      CHECK(r.outcome() == Outcome::Completed); CHECK(has_kind(r, "legality_unknown")); }   /* unknown legality: the game itself is the judge */
}

/* ---------------------------------------------------------------- runner: recovery */
TEST(failure_policies) {
    FakeView v; v.hand = {1001, 1002, 1003}; v.field = {9};
    auto fails = [](FakeExec &x, const char *key) { x.script[key] = {ActionState::Failed}; };
    { /* abort */ FakeExec x; fails(x, "normal_summon:1001"); AttemptLedger l; ComboRunner r;
      CHECK(r.begin(combo("a", {mk("s1", "normal_summon", 1001, "abort"), mk("s2", "activate_spell", 1002)}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Aborted); CHECK(x.subs.size() == 1); CHECK(l.blocked("a")); CHECK(r.reason().find("scripted failure") != std::string::npos); }
    { /* skip: the failure is visible to later steps through step_failed */ FakeExec x; fails(x, "normal_summon:1001"); AttemptLedger l; ComboRunner r;
      Step s2 = mk("s2", "activate_spell", 1002); s2.conditions = {stepc("step_failed", "s1")};
      CHECK(r.begin(combo("s", {mk("s1", "normal_summon", 1001, "skip"), s2}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Completed); CHECK(x.subs.size() == 2 && x.subs[1] == "activate_spell:1002"); }
    { /* goto: skips the steps in between */ FakeExec x; fails(x, "normal_summon:1001"); AttemptLedger l; ComboRunner r;
      Step s1 = mk("s1", "normal_summon", 1001, "goto"); s1.on_fail.goto_step = "s3";
      CHECK(r.begin(combo("g", {s1, mk("s2", "activate_spell", 1002), mk("s3", "activate_spell", 1003)}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Completed); CHECK(x.subs.size() == 2 && x.subs[1] == "activate_spell:1003"); }
    { /* retry_once, second attempt works */ FakeExec x; x.script["normal_summon:1001"] = {ActionState::Failed, ActionState::Succeeded}; AttemptLedger l; ComboRunner r;
      CHECK(r.begin(combo("r1", {mk("s1", "normal_summon", 1001, "retry_once")}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Completed); CHECK(x.subs.size() == 2); CHECK(has_kind(r, "retry")); }
    { /* retry_once, fails again: exactly two attempts, then stop */ FakeExec x; fails(x, "normal_summon:1001"); AttemptLedger l; ComboRunner r;
      CHECK(r.begin(combo("r2", {mk("s1", "normal_summon", 1001, "retry_once")}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Aborted); CHECK(x.subs.size() == 2); CHECK(r.reason().find("retry failed") != std::string::npos); }
    { /* fallback: hand control back to the normal AI */ FakeExec x; fails(x, "normal_summon:1001"); AttemptLedger l; ComboRunner r;
      CHECK(r.begin(combo("f", {mk("s1", "normal_summon", 1001, "fallback"), mk("s2", "activate_spell", 1002)}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Fallback); CHECK(x.subs.size() == 1); CHECK(l.blocked("f")); }
    { /* Cancelled is a failure too */ FakeExec x; x.script["normal_summon:1001"] = {ActionState::Cancelled}; AttemptLedger l; ComboRunner r;
      CHECK(r.begin(combo("c", {mk("s1", "normal_summon", 1001)}), l, 0)); run(r, v, x, l); CHECK(r.outcome() == Outcome::Aborted); CHECK(r.reason().find("cancelled") != std::string::npos); }
}
TEST(an_unexpected_state_change_is_a_failure_and_is_not_retried) {
    FakeView v; v.hand = {1001}; FakeExec x; AttemptLedger l; ComboRunner r;       /* the action "succeeds" but the card never reaches the field */
    Step s = mk("s1", "normal_summon", 1001, "retry_once"); s.expect = {cond("card_on_field", 1001)};
    CHECK(r.begin(combo("e", {s}), l, 0)); run(r, v, x, l);
    CHECK(r.outcome() == Outcome::Aborted); CHECK(x.subs.size() == 1);            /* it already happened: never submitted twice */
    CHECK(r.reason().find("expected state not reached") != std::string::npos); CHECK(r.reason().find("not retryable") != std::string::npos);
    /* an expectation that cannot be read is accepted but flagged */
    FakeView u; u.unknown = {1001}; FakeExec x2; AttemptLedger l2; ComboRunner r2; Step s2 = mk("s1", "normal_summon", 1001); s2.expect = {cond("card_on_field", 1001)};
    CHECK(r2.begin(combo("u", {s2}), l2, 0)); run(r2, u, x2, l2); CHECK(r2.outcome() == Outcome::Completed); CHECK(has_kind(r2, "unverified"));
}
TEST(unmet_goal_after_all_steps_is_not_success) {
    FakeView v; v.hand = {1001}; FakeExec x; AttemptLedger l; ComboRunner r; Combo c = combo("g", {mk("s1", "normal_summon", 1001)}); c.success = {cond("card_on_field", 1001)};
    CHECK(r.begin(c, l, 0)); run(r, v, x, l); CHECK(r.outcome() == Outcome::Aborted); CHECK(r.reason().find("goal was not reached") != std::string::npos); CHECK(l.blocked("g"));
    FakeView u; u.unknown = {1001}; FakeExec x2; AttemptLedger l2; ComboRunner r2; CHECK(r2.begin(c, l2, 0)); run(r2, u, x2, l2); CHECK(r2.outcome() == Outcome::CompletedUnverified);
}
TEST(stalled_actions_time_out_and_are_cancelled) {
    FakeView v; v.hand = {1001}; FakeExec x; x.script["normal_summon:1001"] = {ActionState::Pending}; AttemptLedger l; RunnerConfig cfg; cfg.step_timeout_ticks = 5; ComboRunner r(cfg);
    CHECK(r.begin(combo("t", {mk("s1", "normal_summon", 1001)}), l, 0)); const int used = run(r, v, x, l, 50);
    CHECK(r.outcome() == Outcome::TimedOut); CHECK(x.cancels >= 1); CHECK(used <= 10); CHECK(l.blocked("t"));
    RunnerConfig cfg2; cfg2.step_timeout_ticks = 1000; cfg2.total_timeout_ticks = 8; ComboRunner r2(cfg2); FakeExec x2; x2.script["normal_summon:1001"] = {ActionState::Pending}; AttemptLedger l2;
    CHECK(r2.begin(combo("t2", {mk("s1", "normal_summon", 1001)}), l2, 0)); run(r2, v, x2, l2, 50); CHECK(r2.outcome() == Outcome::TimedOut); CHECK(r2.reason().find("total") != std::string::npos);
}
TEST(branches_jump_forward_when_their_conditions_hold) {
    for (int has : {1, 0}) {
        FakeView v; v.hand = {1001, 1002, 1003}; if (has) v.field.insert(77); FakeExec x; AttemptLedger l; ComboRunner r;
        Step s1 = mk("s1", "normal_summon", 1001); Branch b; b.conditions = {cond("card_on_field", 77)}; b.goto_step = "s3"; s1.branches.push_back(b);
        CHECK(r.begin(combo("b", {s1, mk("s2", "activate_spell", 1002), mk("s3", "activate_spell", 1003)}), l, 0)); run(r, v, x, l);
        CHECK(r.outcome() == Outcome::Completed); CHECK(x.subs.size() == (has ? 2u : 3u)); CHECK(!x.subs.empty() && x.subs.back() == "activate_spell:1003"); CHECK(has_kind(r, "branch") == (has != 0));
    }
}
TEST(selection_prompts_must_be_answered_by_select_card) {
    FakeView v; v.hand = {1001, 1002}; { FakeExec x; x.script["activate_spell:1001"] = {ActionState::NeedsInput}; AttemptLedger l; ComboRunner r;
      Step sel = mk("s2", "select_card"); sel.target_policy = "highest_atk";
      CHECK(r.begin(combo("p", {mk("s1", "activate_spell", 1001), sel, mk("s3", "end_turn")}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Completed); CHECK(x.subs.size() == 3 && x.subs[1] == "select_card:0"); CHECK(has_kind(r, "prompt")); }
    { FakeExec x; x.script["activate_spell:1001"] = {ActionState::NeedsInput}; AttemptLedger l; ComboRunner r;       /* a prompt is open but the next step ignores it */
      CHECK(r.begin(combo("q", {mk("s1", "activate_spell", 1001), mk("s2", "normal_summon", 1002)}), l, 0)); run(r, v, x, l);
      CHECK(r.outcome() == Outcome::Aborted); CHECK(x.subs.size() == 1); CHECK(r.reason().find("prompt") != std::string::npos); }
}
TEST(interruptions_follow_the_combo_policy) {
    FakeView v; v.hand = {1001, 1002}; Combo c = combo("i", {mk("s1", "normal_summon", 1001), mk("s2", "activate_spell", 1002)});
    for (const char *pol : {"abort", "reevaluate", "resume"}) {
        FakeExec x; x.script["normal_summon:1001"] = {ActionState::Pending, ActionState::Pending, ActionState::Succeeded}; AttemptLedger l; ComboRunner r; Combo ci = c; ci.interruption = pol;
        CHECK(r.begin(ci, l, 0)); r.tick(v, x, l, 1); CHECK(x.subs.size() == 1); r.interrupt("opponent chain", x, l, 2); CHECK(x.cancels == 1);
        if (std::string(pol) == "resume") { CHECK(r.running()); run(r, v, x, l); CHECK(r.outcome() == Outcome::Completed); CHECK(x.subs.size() >= 3 && x.subs[1] == "normal_summon:1001"); }   /* step re-checked and re-sent */
        else { CHECK(!r.running()); CHECK(r.outcome() == Outcome::Interrupted); CHECK(r.wants_reselect() == (std::string(pol) == "reevaluate")); CHECK(l.blocked("i") == (std::string(pol) == "abort")); }
    }
    ComboRunner idle; FakeExec x; AttemptLedger l; idle.interrupt("nothing running", x, l, 1); CHECK(x.cancels == 0);   /* harmless when idle */
}
TEST(starts_are_limited_per_turn) {
    FakeView v; v.hand = {1001}; FakeExec x; AttemptLedger l; Combo c = combo("m", {mk("s1", "normal_summon", 1001)}); c.interruption = "reevaluate"; SelectionContext ctx;
    for (int i = 0; i < 2; i++) { CHECK(rank_combos({c}, ctx, v, l)[0].eligible); ComboRunner r; x.script["normal_summon:1001"] = {ActionState::Pending}; CHECK(r.begin(c, l, 0)); r.tick(v, x, l, 1); r.interrupt("changed", x, l, 2); }
    CHECK(!rank_combos({c}, ctx, v, l)[0].eligible);                       /* two starts used: the loop of re-selection ends */
}

/* ---------------------------------------------------------------- misc safety */
TEST(runner_rejects_invalid_or_second_combos) {
    FakeView v; AttemptLedger l; ComboRunner r; Combo bad = combo("Bad Id", {mk("s1", "normal_summon", 1001)});
    CHECK(!r.begin(bad, l, 0)); CHECK(!r.running()); CHECK(r.outcome() == Outcome::Invalid); CHECK(!r.reason().empty());
    Combo empty = combo("empty", {}); CHECK(!r.begin(empty, l, 0));
    Combo ok = combo("ok", {mk("s1", "normal_summon", 1001)}); CHECK(r.begin(ok, l, 0)); CHECK(!r.begin(ok, l, 0)); CHECK(r.reason().find("already") != std::string::npos); CHECK(r.running());
    FakeExec x; r.tick(v, x, l, 1);   /* unlisted/unsupported actions are refused by validation, never reach the executor */
    Combo unk = combo("unk", {mk("s1", "teleport", 1001)}); ComboRunner r2; CHECK(!r2.begin(unk, l, 0));
}
TEST(identical_inputs_give_identical_traces_and_the_trace_is_bounded) {
    auto once = [] {
        FakeView v; v.hand = {1001, 1002}; FakeExec x; x.script["normal_summon:1001"] = {ActionState::Failed, ActionState::Pending, ActionState::Succeeded}; AttemptLedger l; ComboRunner r;
        CHECK(r.begin(combo("d", {mk("s1", "normal_summon", 1001, "retry_once"), mk("s2", "activate_spell", 1002)}), l, 0)); run(r, v, x, l); return r.trace_text();
    };
    const std::string a = once(), b = once(); CHECK(!a.empty()); CHECK(a == b);
    RunnerConfig cfg; cfg.max_trace = 3; ComboRunner r(cfg); FakeView v; v.hand = {1001}; FakeExec x; AttemptLedger l;
    CHECK(r.begin(combo("n", {mk("s1", "normal_summon", 1001), mk("s2", "end_turn")}), l, 0)); run(r, v, x, l); CHECK(r.trace().size() == 3);
}

int main() {
    for (auto &t : tests()) { int before = g_fail; g_run++; t.f(); printf("%s %s\n", g_fail == before ? "ok  " : "FAIL", t.n); }
    printf("%d tests, %d failed checks\n", g_run, g_fail); return g_fail ? 1 : 0;
}
