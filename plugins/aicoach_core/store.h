/* AI Coach - configuration documents: shipped defaults + user overrides, versioned, validated on load, never destructive.
 *
 *   defaults.json  (kind "aicoach.defaults")  shipped with the mod, read-only at run time: profiles + combos.
 *   user.json      (kind "aicoach.user")      everything the player changes: new combos, full replacements of a default combo (same id),
 *                                             small patches (enabled / priority) of default combos, deletions, profile overrides.
 *   effective view = merge(defaults, user). The defaults structure is never modified and never written back by this code.
 * Bad data degrades, it does not abort: a malformed combo is dropped and reported, a malformed file loads as "empty" and is reported,
 * and a file written by a NEWER schema is refused (status TooNew) and must not be overwritten (UserStore enforces this). */
#pragma once
#include "model.h"
#include <filesystem>

namespace aicoach {

struct Defaults { std::vector<Profile> profiles; std::vector<Combo> combos; };
struct Patch { bool has_enabled = false, enabled = true; bool has_priority = false; int priority = 100; };
struct UserData {
    std::vector<Profile> profiles;               /* replace a default profile with the same id, or add a new one */
    std::vector<Combo> combos;                   /* replace a default combo with the same id, or add a new one */
    std::map<std::string, Patch> patches;        /* by combo id */
    std::vector<std::string> deleted;            /* combo ids hidden from the effective view */
};
struct Effective { std::vector<Profile> profiles; std::vector<Combo> combos; };

enum class LoadStatus { Ok, Missing, Malformed, TooNew };
struct LoadResult { LoadStatus status = LoadStatus::Ok; Issues issues; };

/* Upgrades a document in place from version N to N+1. `target` is the schema version the code understands. */
struct MigrationTable { int target = kSchemaVersion; std::map<int, std::function<bool(json::Value &, Issues &)>> steps; };
const MigrationTable &builtin_migrations();   /* empty today: schema 1 is the first version, nothing older exists */

LoadResult load_defaults(const std::string &text, Defaults &out, const ValidationContext &ctx, const MigrationTable &mt = builtin_migrations());
LoadResult load_user(const std::string &text, UserData &out, const ValidationContext &ctx, const MigrationTable &mt = builtin_migrations());
std::string serialize_defaults(const Defaults &d);
std::string serialize_user(const UserData &u);          /* also the export format */
Effective merge(const Defaults &d, const UserData &u, Issues &issues);
/* Import: validates `text` as a user document and folds it into `into` (same-id combos are replaced). Returns false if the document is unusable. */
bool import_user(const std::string &text, UserData &into, const ValidationContext &ctx, Issues &issues);

/* File helpers (UTF-8 paths via std::filesystem). */
bool read_file(const std::filesystem::path &p, std::string &out);
bool write_file_atomic(const std::filesystem::path &p, const std::string &text, std::string &error);   /* temp file + rename, keeps the previous file as <name>.bak */

/* Owns the user file. load() never throws; save() refuses to overwrite a file from a newer schema and moves a corrupt file aside first. */
class UserStore {
public:
    explicit UserStore(std::filesystem::path file) : file_(std::move(file)) {}
    LoadResult load(const ValidationContext &ctx);
    bool save(std::string &error);
    void reset(std::string &error);                      /* clears user data and removes the file (defaults are untouched) */
    UserData data;
    LoadStatus status() const { return status_; }
private:
    std::filesystem::path file_; LoadStatus status_ = LoadStatus::Missing;
};

}  // namespace
