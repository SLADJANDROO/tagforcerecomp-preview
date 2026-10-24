#include "engine.h"
#include <algorithm>

namespace aicoach {

const char *tri_name(Tri t) { return t == Tri::Yes ? "yes" : t == Tri::No ? "no" : "unknown"; }
const char *outcome_name(Outcome o) {
    switch (o) {
        case Outcome::None: return "none"; case Outcome::Completed: return "completed"; case Outcome::CompletedUnverified: return "completed_unverified";
        case Outcome::Aborted: return "aborted"; case Outcome::Fallback: return "fallback"; case Outcome::TimedOut: return "timed_out";
        case Outcome::Interrupted: return "interrupted"; case Outcome::Invalid: return "invalid";
    }
    return "?";
}

/* ---------------------------------------------------------------- ledger */
static std::string action_key(const std::string &combo, const std::string &step, const std::string &action, uint32_t card) {
    return combo + "|" + step + "|" + action + "|" + std::to_string(card);
}
void AttemptLedger::new_turn() { starts_.clear(); failed_.clear(); completed_.clear(); failed_actions_.clear(); used_.clear(); }
void AttemptLedger::note_start(const std::string &combo) { starts_[combo]++; }
void AttemptLedger::note_failure(const std::string &combo, const std::string &step, const std::string &action, uint32_t card) {
    failed_[combo]++; if (!step.empty()) failed_actions_.insert(action_key(combo, step, action, card));
}
void AttemptLedger::note_completed(const std::string &combo) { completed_.insert(combo); }
bool AttemptLedger::blocked(const std::string &combo) const {
    if (completed_.count(combo)) return true;
    auto f = failed_.find(combo); if (f != failed_.end() && f->second > 0) return true;
    auto s = starts_.find(combo); return s != starts_.end() && s->second >= max_starts_per_turn;
}
int AttemptLedger::failures(const std::string &combo) const { auto f = failed_.find(combo); return f == failed_.end() ? 0 : f->second; }
bool AttemptLedger::same_action_failed(const std::string &combo, const std::string &step, const std::string &action, uint32_t card) const {
    return failed_actions_.count(action_key(combo, step, action, card)) != 0;
}

/* ---------------------------------------------------------------- conditions */
static Tri compare(int have, const std::string &op, int want) {
    if (op == "lt") return tri_of(have < want); if (op == "le") return tri_of(have <= want); if (op == "eq") return tri_of(have == want);
    if (op == "ge") return tri_of(have >= want); if (op == "gt") return tri_of(have > want); if (op == "ne") return tri_of(have != want);
    return Tri::Unknown;
}
CondResult eval_condition(const Condition &c, const EvalEnv &env) {
    const std::string &t = c.type;
    Tri v = Tri::Unknown; std::string text = t;
    auto card_text = [&](const char *where) { text = "card " + std::to_string(c.card) + " " + where; };
    if (!env.view) { text += " (no duel view)"; return {Tri::Unknown, text}; }
    const DuelView &dv = *env.view;
    if (t == "card_in_hand") { card_text("in hand"); v = dv.has_card(Where::Hand, c.card); }
    else if (t == "card_on_field") { card_text("on field"); v = dv.has_card(Where::Field, c.card); }
    else if (t == "card_in_graveyard") { card_text("in graveyard"); v = dv.has_card(Where::Graveyard, c.card); }
    else if (t == "card_in_extra") { card_text("in extra deck"); v = dv.has_card(Where::Extra, c.card); }
    else if (t == "monster_position") { text = "card " + std::to_string(c.card) + " in " + c.position + " position"; v = dv.monster_position(c.card, c.position); }
    else if (t == "free_zones") {
        const int n = dv.free_zones(c.zone); text = "free " + c.zone + " zones " + c.op + " " + std::to_string(c.value) + " (have " + (n < 0 ? "?" : std::to_string(n)) + ")";
        v = n < 0 ? Tri::Unknown : compare(n, c.op, c.value);
    } else if (t == "lp_compare") {
        const int n = dv.life_points(c.who == "self"); text = c.who + " LP " + c.op + " " + std::to_string(c.value) + " (have " + (n < 0 ? "?" : std::to_string(n)) + ")";
        v = n < 0 ? Tri::Unknown : compare(n, c.op, c.value);
    } else if (t == "opponent_monster_count") {
        const int n = dv.opponent_monster_count(); text = "opponent monsters " + c.op + " " + std::to_string(c.value) + " (have " + (n < 0 ? "?" : std::to_string(n)) + ")";
        v = n < 0 ? Tri::Unknown : compare(n, c.op, c.value);
    } else if (t == "phase_is") {
        const std::string p = dv.phase(); text = "phase is " + c.phase + " (now " + (p.empty() ? "?" : p) + ")"; v = p.empty() ? Tri::Unknown : tri_of(p == c.phase);
    } else if (t == "step_succeeded" || t == "step_failed") {
        text = "step " + c.step + (t == "step_succeeded" ? " succeeded" : " failed"); v = Tri::No;   /* a step that never ran is neither */
        if (env.steps) for (auto &p : *env.steps) if (p.first == c.step) v = tri_of(p.second == (t == "step_succeeded" ? StepStatus::Succeeded : StepStatus::Failed));
    } else if (t == "action_legal") {
        text = "current action is legal"; v = env.current ? dv.action_legal(*env.current) : Tri::Unknown;
    } else if (t == "card_unused") {
        card_text("not used yet this turn"); v = env.ledger ? tri_of(!env.ledger->card_used(c.card)) : Tri::Unknown;
    } else text = "unknown condition type '" + t + "'";
    if (c.negate) { v = tri_not(v); text = "NOT (" + text + ")"; }
    return {v, text + ": " + tri_name(v)};
}
Tri eval_all(const std::vector<Condition> &cs, const EvalEnv &env, std::vector<CondResult> *results) {
    Tri total = Tri::Yes;
    for (auto &c : cs) {
        CondResult r = eval_condition(c, env); if (results) results->push_back(r);
        if (r.value == Tri::No) total = Tri::No; else if (r.value == Tri::Unknown && total == Tri::Yes) total = Tri::Unknown;
    }
    return total;
}

/* ---------------------------------------------------------------- ranking */
int bias_min_priority(const Profile *p) { if (!p) return 0; const int b = std::max(0, std::min(100, p->signature_bias)); return 5 * (100 - b); }

static Tri available(const DuelView &v, uint32_t card) {   /* usable somewhere: hand, field, graveyard or extra deck */
    Tri t = Tri::No;
    for (Where w : {Where::Hand, Where::Field, Where::Graveyard, Where::Extra}) { Tri r = v.has_card(w, card); if (r == Tri::Yes) return Tri::Yes; if (r == Tri::Unknown) t = Tri::Unknown; }
    return t;
}
std::vector<Candidate> rank_combos(const std::vector<Combo> &combos, const SelectionContext &ctx, const DuelView &view, const AttemptLedger &ledger) {
    std::vector<Candidate> out; out.reserve(combos.size());
    EvalEnv env; env.view = &view; env.ledger = &ledger;
    for (size_t ci = 0; ci < combos.size(); ci++) {
        const Combo &c = combos[ci];
        Candidate k; k.index = ci; k.id = c.id;
        auto no = [&](const std::string &why) { k.eligible = false; k.reason = why; out.push_back(k); };
        if (!c.enabled) { no("disabled"); continue; }
        if (c.character != 0 && c.character != ctx.character) { no("belongs to another character"); continue; }
        if (!c.decks.empty() && std::find(c.decks.begin(), c.decks.end(), ctx.deck) == c.decks.end()) { no("belongs to other decks"); continue; }
        if (ledger.blocked(c.id)) { no("blocked this turn (failed, completed or out of starts)"); continue; }
        if (c.priority < bias_min_priority(ctx.profile)) { no("priority below the profile's signature bias"); continue; }
        if (c.preferred_turn != 0) { const int t = view.turn(); if (t == 0) { no("turn number unavailable"); continue; } if (t != c.preferred_turn) { no("preferred turn is " + std::to_string(c.preferred_turn)); continue; } }
        if (!c.preferred_phase.empty()) { const std::string p = view.phase(); if (p.empty()) { no("phase unavailable"); continue; } if (p != c.preferred_phase) { no("preferred phase is " + c.preferred_phase); continue; } }
        bool bad = false;
        for (uint32_t rc : c.required_cards) {
            const Tri a = available(view, rc); k.checks.push_back({a, "required card " + std::to_string(rc) + " available: " + tri_name(a)});
            if (a != Tri::Yes && !bad) {
                bad = true;
                if (a == Tri::No) k.reason = "missing required card " + std::to_string(rc);
                else if (view.has_card(Where::Hand, rc) == Tri::No && view.has_card(Where::Field, rc) == Tri::No) k.reason = "required card " + std::to_string(rc) + " is not in hand or on the field (graveyard / extra deck cannot be read)";
                else k.reason = "state unavailable for required card " + std::to_string(rc);
            }
        }
        if (bad) { k.eligible = false; out.push_back(k); continue; }
        const Tri st = eval_all(c.conditions, env, &k.checks);
        if (st != Tri::Yes) {
            for (auto &r : k.checks) if (r.value != Tri::Yes) { k.reason = std::string("start condition ") + (st == Tri::No ? "failed: " : "unknown: ") + r.text; break; }
            k.eligible = false; out.push_back(k); continue;
        }
        if (!c.success.empty() && eval_all(c.success, env, nullptr) == Tri::Yes) { no("goal already achieved"); continue; }
        int score = c.priority;
        for (uint32_t oc : c.optional_cards) { const Tri a = available(view, oc); if (a == Tri::Yes) score += 5; }
        k.eligible = true; k.score = score; k.reason = "eligible"; out.push_back(k);
    }
    std::stable_sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) {
        if (a.eligible != b.eligible) return a.eligible;
        if (a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    });
    return out;
}

/* ---------------------------------------------------------------- runner */
void ComboRunner::note(uint64_t now, const std::string &kind, const std::string &detail) {
    trace_.push_back({now, combo_.id, idx_ < combo_.steps.size() ? combo_.steps[idx_].id : std::string(), kind, detail});
    if (trace_.size() > cfg_.max_trace) trace_.erase(trace_.begin(), trace_.begin() + (trace_.size() - cfg_.max_trace));
}
EvalEnv ComboRunner::env(const DuelView &v, const AttemptLedger &l, const Step *cur) const {
    EvalEnv e; e.view = &v; e.ledger = &l; e.steps = &status_; e.current = cur; return e;
}
void ComboRunner::set_status(const std::string &step, StepStatus s) {
    for (auto &p : status_) if (p.first == step) { p.second = s; return; }
    status_.push_back({step, s});
}
std::string ComboRunner::trace_text() const {
    std::string o;
    for (auto &e : trace_) o += std::to_string(e.tick) + "|" + e.combo + "|" + e.step + "|" + e.kind + "|" + e.detail + "\n";
    return o;
}

bool ComboRunner::begin(const Combo &c, AttemptLedger &ledger, uint64_t now) {
    if (running_) { reason_ = "a combo is already running"; return false; }
    Issues is; ValidationContext vc; validate_combo(c, vc, "combo", is);
    if (has_errors(is)) {
        outcome_ = Outcome::Invalid; reason_ = "invalid combo";
        for (auto &i : is) if (i.sev == Issue::Error) { reason_ = i.path + ": " + i.msg; break; }
        return false;
    }
    combo_ = c; running_ = true; reselect_ = false; retried_ = false; retry_now_ = false; prompt_open_ = false;
    phase_ = Phase::Prepare; idx_ = 0; started_ = now; step_started_ = now; outcome_ = Outcome::None; reason_.clear(); status_.clear(); trace_.clear();
    ledger.note_start(combo_.id);
    note(now, "begin", combo_.name);
    return true;
}

void ComboRunner::finish(Outcome o, const std::string &why, AttemptLedger &l, uint64_t now) {
    note(now, "end", std::string(outcome_name(o)) + (why.empty() ? "" : ": " + why));
    running_ = false; outcome_ = o; reason_ = why;
    if (o == Outcome::Completed || o == Outcome::CompletedUnverified) l.note_completed(combo_.id);
    else if (o == Outcome::Aborted || o == Outcome::Fallback || o == Outcome::TimedOut) l.note_failure(combo_.id, "", "", 0);   /* never re-selected this turn */
}

void ComboRunner::fail(const std::string &why, Cause cause, AttemptLedger &l, uint64_t now) {
    const Step &s = combo_.steps[idx_];
    set_status(s.id, StepStatus::Failed); l.note_failure(combo_.id, s.id, s.action, s.card);
    note(now, "fail", why);
    phase_ = Phase::Prepare;
    const Outcome abort_as = cause == Cause::Timeout ? Outcome::TimedOut : Outcome::Aborted;
    const std::string &pol = s.on_fail.policy;
    auto jump = [&](const std::string &target) {
        for (size_t i = 0; i < combo_.steps.size(); i++) if (combo_.steps[i].id == target) { idx_ = i; return true; }
        return false;
    };
    if (pol == "skip") { idx_++; retried_ = false; note(now, "skip", s.id); }
    else if (pol == "goto") { if (jump(s.on_fail.goto_step)) { retried_ = false; note(now, "goto", s.on_fail.goto_step); } else finish(abort_as, why + " (goto target missing)", l, now); }
    else if (pol == "retry_once") {
        if (cause != Cause::Expectation && !retried_) { retried_ = true; retry_now_ = true; note(now, "retry", s.id); }
        else finish(abort_as, why + (retried_ ? " (retry failed)" : " (not retryable: the action already happened)"), l, now);
    }
    else if (pol == "fallback") finish(Outcome::Fallback, why, l, now);
    else finish(abort_as, why, l, now);   /* abort, or anything unexpected */
}

void ComboRunner::on_success(const Step &s, ActionState st, const DuelView &v, AttemptLedger &l, uint64_t now) {
    prompt_open_ = st == ActionState::NeedsInput;
    if (prompt_open_) note(now, "prompt", "action opened a selection prompt; expectations are checked on the select_card step");
    else if (!s.expect.empty()) {
        std::vector<CondResult> res; const Tri t = eval_all(s.expect, env(v, l, &s), &res);
        if (t == Tri::No) { std::string w = "expected state not reached"; for (auto &r : res) if (r.value == Tri::No) { w += ": " + r.text; break; } fail(w, Cause::Expectation, l, now); return; }
        if (t == Tri::Unknown) { std::string w = "expected state cannot be verified"; for (auto &r : res) if (r.value == Tri::Unknown) { w += ": " + r.text; break; } note(now, "unverified", w); }
    }
    set_status(s.id, StepStatus::Succeeded);
    if (s.card && s.action != "select_card") l.note_card_used(s.card);
    note(now, "ok", s.action + (s.card ? " " + std::to_string(s.card) : std::string()));
    size_t next = idx_ + 1;
    for (auto &b : s.branches) {
        if (eval_all(b.conditions, env(v, l, &s), nullptr) != Tri::Yes) continue;
        for (size_t i = 0; i < combo_.steps.size(); i++) if (combo_.steps[i].id == b.goto_step) { next = i; break; }
        note(now, "branch", "jump to " + b.goto_step); break;
    }
    idx_ = next; retried_ = false; phase_ = Phase::Prepare;
}

void ComboRunner::tick(const DuelView &v, ActionExecutor &x, AttemptLedger &l, uint64_t now) {
    if (!running_) return;
    if (now - started_ > cfg_.total_timeout_ticks) { x.cancel(); finish(Outcome::TimedOut, "combo exceeded its total time budget", l, now); return; }
    for (size_t guard = 0; guard < 2 * kMaxSteps + 8 && running_; guard++) {
        if (idx_ >= combo_.steps.size()) {
            std::vector<CondResult> res; const Tri t = eval_all(combo_.success, env(v, l, nullptr), &res);
            if (t == Tri::Yes) finish(Outcome::Completed, "", l, now);
            else if (t == Tri::Unknown) finish(Outcome::CompletedUnverified, "success conditions cannot be verified", l, now);
            else { std::string w = "all steps ran but the goal was not reached"; for (auto &r : res) if (r.value == Tri::No) { w += ": " + r.text; break; } finish(Outcome::Aborted, w, l, now); }
            return;
        }
        const Step &s = combo_.steps[idx_];
        if (phase_ == Phase::Prepare) {
            if (prompt_open_ && s.action != "select_card") { fail("a selection prompt is open but this step is not select_card", Cause::Precondition, l, now); continue; }
            std::vector<CondResult> res; const Tri pre = eval_all(s.conditions, env(v, l, &s), &res);
            if (pre != Tri::Yes) {
                std::string w = std::string("precondition ") + (pre == Tri::No ? "failed" : "unknown"); for (auto &r : res) if (r.value != Tri::Yes) { w += ": " + r.text; break; }
                fail(w, Cause::Precondition, l, now); continue;
            }
            if (!retry_now_ && l.same_action_failed(combo_.id, s.id, s.action, s.card)) { fail("the identical action already failed this turn", Cause::Precondition, l, now); continue; }
            const Tri lg = v.action_legal(s);
            if (lg == Tri::No) { fail("the game does not allow this action now", Cause::Precondition, l, now); continue; }
            if (lg == Tri::Unknown) note(now, "legality_unknown", "submitting anyway; the game decides");
            std::string why;
            if (!x.submit(s, why)) { fail("submit refused: " + why, Cause::Execution, l, now); continue; }
            retry_now_ = false; phase_ = Phase::Waiting; step_started_ = now;
            note(now, "submit", s.action + (s.card ? " " + std::to_string(s.card) : std::string()));
            return;   /* one action in flight; the next tick polls it */
        }
        std::string why; const ActionState st = x.poll(why);
        if (st == ActionState::Pending) {
            if (now - step_started_ > cfg_.step_timeout_ticks) { x.cancel(); fail("timed out waiting for the action to resolve", Cause::Timeout, l, now); continue; }
            return;
        }
        if (st == ActionState::Succeeded || st == ActionState::NeedsInput) { on_success(s, st, v, l, now); return; }
        fail(std::string(st == ActionState::Cancelled ? "action cancelled" : "action failed") + (why.empty() ? "" : ": " + why), Cause::Execution, l, now);
    }
    if (running_) finish(Outcome::Aborted, "internal loop guard tripped", l, now);
}

void ComboRunner::interrupt(const std::string &why, ActionExecutor &x, AttemptLedger &l, uint64_t now) {
    if (!running_) return;
    note(now, "interrupt", why);
    x.cancel();
    const std::string &pol = combo_.interruption;
    if (pol == "resume") { phase_ = Phase::Prepare; retry_now_ = true; note(now, "resume", "re-checking the current step"); return; }
    if (pol == "abort") l.note_failure(combo_.id, "", "", 0);
    finish(Outcome::Interrupted, why, l, now);
    reselect_ = pol == "reevaluate";
}

}  // namespace
