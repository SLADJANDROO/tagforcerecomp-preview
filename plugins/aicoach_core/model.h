/* AI Coach - combo / profile data model (Milestone 2). Pure data + validation + JSON conversion; no game access, no platform headers,
 * so it builds and is unit-tested on any machine (tools/test/aicoach/run_tests.sh).
 *
 * Status of what is declared here (see docs/AI_COACH.md):
 *   - Condition and action NAMES are a closed, validated vocabulary. A combo can never carry code.
 *   - Whether the running engine can EXECUTE an action is a separate question: pass a Capabilities set to validate_combo() and any action it does
 *     not list is an error (the editor offers only those). With no Capabilities only the structure is checked.
 *   - The phase names are provisional until the duel-state adapter (Milestone 3) maps them onto the game's real phase values. */
#pragma once
#include "json.h"
#include <stdint.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>

namespace aicoach {

constexpr int kSchemaVersion = 1;
constexpr uint32_t kMaxCardId = 0x1fff;      /* slots store the card id in 13 bits (id & 0x1fff): plugins/src/BetterAI/ia_aprimorada.cpp */
constexpr int kMinCharacterId = 2, kMaxCharacterId = 151, kMaxDeckId = 350;   /* decomp/common/duelists.h */
constexpr size_t kMaxSteps = 64, kMaxConditions = 16;

struct Issue {
    enum Severity { Error, Warning } sev;
    std::string path;     /* e.g. "combos[3].steps[1].card" */
    std::string msg;
};
typedef std::vector<Issue> Issues;
bool has_errors(const Issues &v);

struct Condition {
    std::string type;                 /* see condition_specs() */
    bool negate = false;
    uint32_t card = 0;
    std::string position, zone, op, who, phase, step;
    int value = 0;
};
struct FailPolicy { std::string policy = "abort"; std::string goto_step; };       /* abort | skip | goto | retry_once | fallback */
struct Branch { std::vector<Condition> conditions; std::string goto_step; };
struct Step {
    std::string id, action;
    uint32_t card = 0;
    std::string position, target_policy;
    uint32_t target_card = 0;
    std::vector<Condition> conditions;   /* preconditions */
    std::vector<Condition> expect;       /* expected state change after the action resolves */
    FailPolicy on_fail;
    std::vector<Branch> branches;        /* evaluated after success, first match jumps forward */
};
struct Combo {
    enum Origin { Default, UserOverride, UserNew };
    std::string id, name, description;
    int revision = 1;
    int character = 0;                   /* 0 = any character */
    std::vector<int> decks;              /* empty = every deck of the character */
    bool enabled = true;
    int priority = 100;                  /* 0..1000, higher first */
    std::vector<Condition> conditions;   /* starting state requirements */
    std::vector<uint32_t> required_cards, optional_cards;
    std::vector<Step> steps;
    std::vector<Condition> success;
    std::string interruption = "reevaluate";   /* abort | reevaluate | resume */
    std::vector<std::string> tags;
    std::string preferred_phase;         /* "" = any */
    int preferred_turn = 0;              /* 0 = any */
    Origin origin = Default;             /* not serialized */
};
struct Profile {
    std::string id, name;
    int character = 0;
    int aggression = 50, risk_tolerance = 50, signature_bias = 50;   /* 0..100; signature_bias = signature combos vs general decisions */
    std::string preferred_position = "auto";    /* auto | attack | defense */
    std::string attack_target_policy = "first_legal";
    std::vector<uint32_t> preferred_cards;
    std::string fallback = "original_ai";       /* original_ai | better_ai */
    bool enabled = true;
};

/* Does `deck` belong to `character` under the game's own rules? (ladders 2..39 = 6 decks each, 41..142 one deck each, plus the story's documented exceptions;
 * rules and sources: decomp/common/duelists.h, docs/DECKS.md). Used for a WARNING only: unknown game versions may differ. */
bool deck_belongs_to_character(int character, int deck);

/* ---- vocabulary ---- */
struct ParamSpec { const char *name; bool required; };
struct ActionSpec { const char *name; std::vector<const char *> required; };
const std::vector<ActionSpec> &action_specs();
const std::vector<const char *> &condition_types();
bool is_known_action(const std::string &a);
bool is_known_condition(const std::string &t);
const std::set<std::string> &positions(), &zones(), &ops(), &whos(), &phases(), &target_policies(), &fail_policies(), &interruptions(), &fallbacks();

/* ---- validation ---- */
struct Capabilities { std::set<std::string> actions; };   /* actions the running engine can execute (filled by the execution engine, Milestone 3) */
struct ValidationContext {
    const Capabilities *caps = nullptr;                                      /* null: structure only */
    std::function<bool(uint32_t)> card_exists;                               /* null: unknown ids are not checked */
    std::function<bool(int deck, uint32_t card)> deck_contains;              /* null: deck membership is not checked */
};
void validate_condition(const Condition &c, const std::string &path, const std::set<std::string> &earlier_steps, Issues &out);
void validate_combo(const Combo &c, const ValidationContext &ctx, const std::string &path, Issues &out);
void validate_profile(const Profile &p, const std::string &path, Issues &out);

/* ---- JSON ---- */
json::Value to_json(const Combo &c);
json::Value to_json(const Profile &p);
/* Returns false if the value cannot be read as a combo at all (wrong types); unknown keys only produce warnings. Run validate_combo() afterwards. */
bool combo_from_json(const json::Value &v, Combo &out, const std::string &path, Issues &issues);
bool profile_from_json(const json::Value &v, Profile &out, const std::string &path, Issues &issues);

}  // namespace
