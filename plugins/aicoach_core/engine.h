/* AI Coach - decision engine core (Milestone 3a): condition evaluation, combo ranking, step runner, attempt ledger.
 * Pure logic: no game access, no platform headers, no exceptions, no allocation in any per-frame loop beyond trace strings. It talks to the game only
 * through two abstract interfaces that the game adapter (Milestone 3b+) implements and the unit tests fake:
 *   DuelView        - read the duel state. Anything the game does not expose answers Unknown (never a guess).
 *   ActionExecutor  - submit ONE action and poll its result across frames.
 * See docs/AI_COACH.md section 2. Nothing here has been run against the real game. */
#pragma once
#include "model.h"
#include <stdint.h>
#include <set>
#include <string>
#include <vector>

namespace aicoach {

/* ------------------------------------------------------------ three-valued logic */
enum class Tri { No = 0, Yes = 1, Unknown = 2 };
inline Tri tri_not(Tri t) { return t == Tri::Yes ? Tri::No : t == Tri::No ? Tri::Yes : Tri::Unknown; }
inline Tri tri_of(bool b) { return b ? Tri::Yes : Tri::No; }
const char *tri_name(Tri t);

enum class Where { Hand, Field, Graveyard, Extra };

/* Read-only view of the duel from the point of view of the acting side. Implemented by the game adapter. -1 / "" / 0 mean "not available". */
struct DuelView {
    virtual ~DuelView() {}
    virtual Tri has_card(Where w, uint32_t card) const = 0;
    virtual Tri monster_position(uint32_t card, const std::string &position) const = 0;   /* attack | defense | facedown_defense */
    virtual int free_zones(const std::string &zone) const = 0;                            /* monster | spelltrap; -1 unknown */
    virtual int life_points(bool self) const = 0;                                         /* -1 unknown */
    virtual int opponent_monster_count() const = 0;                                       /* -1 unknown */
    virtual std::string phase() const = 0;                                                /* one of phases(); "" unknown */
    virtual int turn() const = 0;                                                         /* 1-based turn number of the acting side; 0 unknown */
    virtual Tri action_legal(const Step &s) const = 0;                                    /* would the game accept this action right now */
};

enum class ActionState { Pending, Succeeded, Failed, Cancelled, NeedsInput };

/* Submits one action to the game and reports its fate. The runner never submits a second action before poll() stops returning Pending. */
struct ActionExecutor {
    virtual ~ActionExecutor() {}
    virtual bool submit(const Step &s, std::string &why) = 0;   /* false = refused immediately (illegal, unsupported); `why` says so */
    virtual ActionState poll(std::string &why) = 0;             /* NeedsInput: the action succeeded but opened a selection prompt (answer it with select_card) */
    virtual void cancel() = 0;                                  /* abandon the submitted action if possible */
};

/* ------------------------------------------------------------ attempt tracking (per turn) */
/* Prevents the AI from re-selecting a combo that failed, finished, or already ran its allowed number of times, and records which cards were used. */
class AttemptLedger {
public:
    int max_starts_per_turn = 2;                        /* a combo may start this many times in one turn (one start + one re-evaluation) */
    void new_turn();                                     /* clears everything */
    void note_start(const std::string &combo);
    void note_failure(const std::string &combo, const std::string &step, const std::string &action, uint32_t card);
    void note_completed(const std::string &combo);
    void note_card_used(uint32_t card) { if (card) used_.insert(card); }
    bool blocked(const std::string &combo) const;        /* failed, completed or out of starts */
    bool card_used(uint32_t card) const { return used_.count(card) != 0; }
    int failures(const std::string &combo) const;
    bool same_action_failed(const std::string &combo, const std::string &step, const std::string &action, uint32_t card) const;
private:
    std::map<std::string, int> starts_, failed_;
    std::set<std::string> completed_, failed_actions_;
    std::set<uint32_t> used_;
};

/* ------------------------------------------------------------ condition evaluation */
enum class StepStatus { Succeeded, Failed };
struct EvalEnv {
    const DuelView *view = nullptr;
    const AttemptLedger *ledger = nullptr;
    const std::vector<std::pair<std::string, StepStatus>> *steps = nullptr;   /* statuses of steps that already ran */
    const Step *current = nullptr;                                            /* for action_legal */
};
struct CondResult { Tri value; std::string text; };           /* text is human readable: what was checked and what was seen */
CondResult eval_condition(const Condition &c, const EvalEnv &env);
/* All must be Yes. `results` (optional) receives one entry per condition for diagnostics. Returns Yes / No / Unknown (No beats Unknown). */
Tri eval_all(const std::vector<Condition> &cs, const EvalEnv &env, std::vector<CondResult> *results = nullptr);

/* ------------------------------------------------------------ ranking */
struct SelectionContext {
    int character = 0, deck = 0;
    const Profile *profile = nullptr;           /* optional: signature_bias gates low-priority combos */
};
struct Candidate {
    size_t index = 0;                            /* position in the vector passed to rank_combos (no pointer: nothing can dangle) */
    std::string id;                              /* the combo's id */
    bool eligible = false;
    int score = 0;
    std::string reason;                          /* first reason it is not eligible, or "eligible" */
    std::vector<CondResult> checks;              /* every start condition / required-card check, for the diagnostics view */
};
/* Evaluates every combo and returns ALL of them, eligible first, ordered by score (desc) then id (asc): deterministic.
 * Combos are assumed validated (store.h loaders drop invalid ones; ComboRunner::begin re-validates). */
std::vector<Candidate> rank_combos(const std::vector<Combo> &combos, const SelectionContext &ctx, const DuelView &view, const AttemptLedger &ledger);
/* Minimum combo priority that may run for a profile: signature_bias 100 -> 0 (any), 50 -> 250, 0 -> 500. A provisional tuning rule, see docs. */
int bias_min_priority(const Profile *p);

/* ------------------------------------------------------------ runner */
enum class Outcome { None, Completed, CompletedUnverified, Aborted, Fallback, TimedOut, Interrupted, Invalid };
const char *outcome_name(Outcome o);
struct RunnerConfig { uint64_t step_timeout_ticks = 600; uint64_t total_timeout_ticks = 6000; size_t max_trace = 256; };
struct TraceEvent { uint64_t tick; std::string combo, step, kind, detail; };

class ComboRunner {
public:
    explicit ComboRunner(RunnerConfig cfg = RunnerConfig()) : cfg_(cfg) {}
    /* Starts `c`. Refuses (false, reason() filled, outcome Invalid) if it is already running or fails validation. Counts as one start in the ledger. */
    bool begin(const Combo &c, AttemptLedger &ledger, uint64_t now);
    /* Advances by at most one submitted action. Call once per decision opportunity (frame or AI tick); cheap and bounded. */
    void tick(const DuelView &view, ActionExecutor &exec, AttemptLedger &ledger, uint64_t now);
    /* An external event changed the situation (opponent chain, forced prompt...). Follows the combo's `interruption` policy. */
    void interrupt(const std::string &why, ActionExecutor &exec, AttemptLedger &ledger, uint64_t now);

    bool running() const { return running_; }
    Outcome outcome() const { return outcome_; }
    const std::string &reason() const { return reason_; }
    bool wants_reselect() const { return reselect_; }         /* after Interrupted with policy reevaluate: pick a combo again */
    const std::string &combo_id() const { return combo_.id; }
    int current_step() const { return running_ ? (int)idx_ : -1; }
    const std::vector<TraceEvent> &trace() const { return trace_; }
    std::string trace_text() const;                            /* one line per event, stable format (used by the determinism test) */
private:
    enum class Phase { Prepare, Waiting };
    enum class Cause { Precondition, Execution, Timeout, Expectation };   /* Expectation: the action DID happen, so it is never retried */
    void fail(const std::string &why, Cause cause, AttemptLedger &l, uint64_t now);
    void finish(Outcome o, const std::string &why, AttemptLedger &l, uint64_t now);
    void on_success(const Step &s, ActionState st, const DuelView &v, AttemptLedger &l, uint64_t now);
    void note(uint64_t now, const std::string &kind, const std::string &detail);
    EvalEnv env(const DuelView &v, const AttemptLedger &l, const Step *cur) const;
    void set_status(const std::string &step, StepStatus s);

    RunnerConfig cfg_;
    Combo combo_;
    bool running_ = false, reselect_ = false, retried_ = false, retry_now_ = false, prompt_open_ = false;
    Phase phase_ = Phase::Prepare;
    size_t idx_ = 0;
    uint64_t started_ = 0, step_started_ = 0;
    Outcome outcome_ = Outcome::None;
    std::string reason_;
    std::vector<std::pair<std::string, StepStatus>> status_;
    std::vector<TraceEvent> trace_;
};

}  // namespace
