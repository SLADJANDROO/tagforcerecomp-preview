#include "store.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <system_error>

namespace aicoach {
using json::Value;

const MigrationTable &builtin_migrations() { static const MigrationTable t; return t; }

namespace {
void E(Issues &o, const std::string &p, const std::string &m) { o.push_back({Issue::Error, p, m}); }
void W(Issues &o, const std::string &p, const std::string &m) { o.push_back({Issue::Warning, p, m}); }

/* parse + kind + version + migrate. Returns Ok when `doc` is ready to be read. */
LoadStatus open_doc(const std::string &text, const char *kind, Value &doc, Issues &is, const MigrationTable &mt) {
    std::string pe;
    if (!json::parse(text, doc, pe)) { E(is, "$", "malformed JSON: " + pe); return LoadStatus::Malformed; }
    if (doc.type != Value::Object) { E(is, "$", "document must be an object"); return LoadStatus::Malformed; }
    const Value *k = doc.find("kind");
    if (!k || k->type != Value::String || k->str != kind) { E(is, "$.kind", std::string("expected kind \"") + kind + "\""); return LoadStatus::Malformed; }
    const Value *v = doc.find("schema_version");
    if (!v || v->type != Value::Number || !v->is_int || v->num < 1) { E(is, "$.schema_version", "missing or invalid schema_version"); return LoadStatus::Malformed; }
    int ver = (int)v->num;
    if (ver > mt.target) { E(is, "$.schema_version", "file is schema " + std::to_string(ver) + ", this build understands up to " + std::to_string(mt.target) + " (not loaded, not modified)"); return LoadStatus::TooNew; }
    while (ver < mt.target) {
        auto it = mt.steps.find(ver);
        if (it == mt.steps.end()) { E(is, "$.schema_version", "no migration from schema " + std::to_string(ver)); return LoadStatus::Malformed; }
        if (!it->second(doc, is)) { E(is, "$.schema_version", "migration from schema " + std::to_string(ver) + " failed"); return LoadStatus::Malformed; }
        ver++; doc.set("schema_version", Value::make_int(ver));
        W(is, "$.schema_version", "migrated from schema " + std::to_string(ver - 1) + " to " + std::to_string(ver));
    }
    return LoadStatus::Ok;
}
template <class T, class FromJson, class Validate>
void read_list(const Value &doc, const char *key, std::vector<T> &out, FromJson from, Validate validate, Issues &is, int origin_tag) {
    (void)origin_tag;
    const Value *a = doc.find(key); if (!a) return;
    if (a->type != Value::Array) { E(is, std::string("$.") + key, "expected an array"); return; }
    std::set<std::string> ids;
    for (size_t i = 0; i < a->arr.size(); i++) {
        const std::string p = std::string("$.") + key + "[" + std::to_string(i) + "]"; T item; Issues local;
        bool ok = from(a->arr[i], item, p, local);
        if (ok) validate(item, p, local);
        bool bad = !ok || has_errors(local);
        is.insert(is.end(), local.begin(), local.end());
        if (bad) { E(is, p, "entry dropped (see the errors above)"); continue; }
        if (!ids.insert(item.id).second) { W(is, p + ".id", "duplicate id '" + item.id + "': later entry ignored"); continue; }
        out.push_back(item);
    }
}
void read_lists(const Value &doc, std::vector<Profile> &pr, std::vector<Combo> &co, const ValidationContext &ctx, Issues &is) {
    read_list(doc, "profiles", pr, [](const Value &v, Profile &p, const std::string &path, Issues &i) { return profile_from_json(v, p, path, i); },
              [](const Profile &p, const std::string &path, Issues &i) { validate_profile(p, path, i); }, is, 0);
    read_list(doc, "combos", co, [](const Value &v, Combo &c, const std::string &path, Issues &i) { return combo_from_json(v, c, path, i); },
              [&](const Combo &c, const std::string &path, Issues &i) { validate_combo(c, ctx, path, i); }, is, 0);
}
Value doc_header(const char *kind) { Value d = Value::make_object(); d.set("kind", Value::make_string(kind)); d.set("schema_version", Value::make_int(kSchemaVersion)); return d; }
}  // namespace

LoadResult load_defaults(const std::string &text, Defaults &out, const ValidationContext &ctx, const MigrationTable &mt) {
    LoadResult r; Value doc; Defaults d;
    r.status = open_doc(text, "aicoach.defaults", doc, r.issues, mt);
    if (r.status != LoadStatus::Ok) { out = Defaults(); return r; }
    read_lists(doc, d.profiles, d.combos, ctx, r.issues);
    for (auto &c : d.combos) c.origin = Combo::Default;
    out = d; return r;
}
LoadResult load_user(const std::string &text, UserData &out, const ValidationContext &ctx, const MigrationTable &mt) {
    LoadResult r; Value doc; UserData u;
    r.status = open_doc(text, "aicoach.user", doc, r.issues, mt);
    if (r.status != LoadStatus::Ok) { out = UserData(); return r; }
    read_lists(doc, u.profiles, u.combos, ctx, r.issues);
    if (const Value *p = doc.find("patches")) {
        if (p->type != Value::Object) E(r.issues, "$.patches", "expected an object");
        else for (auto &kv : p->obj) {
            const std::string path = "$.patches." + kv.first; const Value &v = kv.second; Patch pt;
            if (v.type != Value::Object) { E(r.issues, path, "expected an object"); continue; }
            bool bad = false;
            if (const Value *e = v.find("enabled")) { if (e->type == Value::Bool) { pt.has_enabled = true; pt.enabled = e->b; } else { E(r.issues, path + ".enabled", "expected true or false"); bad = true; } }
            if (const Value *q = v.find("priority")) { if (q->type == Value::Number && q->is_int && q->num >= 0 && q->num <= 1000) { pt.has_priority = true; pt.priority = (int)q->num; } else { E(r.issues, path + ".priority", "expected an integer 0..1000"); bad = true; } }
            if (!bad) u.patches[kv.first] = pt;
        }
    }
    if (const Value *d = doc.find("deleted")) {
        if (d->type != Value::Array) E(r.issues, "$.deleted", "expected an array");
        else for (auto &x : d->arr) { if (x.type == Value::String) u.deleted.push_back(x.str); else E(r.issues, "$.deleted", "expected strings"); }
    }
    for (auto &c : u.combos) c.origin = Combo::UserNew;   /* merge() decides override vs new */
    out = u; return r;
}
std::string serialize_defaults(const Defaults &d) {
    Value doc = doc_header("aicoach.defaults"); Value p = Value::make_array(), c = Value::make_array();
    for (auto &x : d.profiles) p.push(to_json(x)); for (auto &x : d.combos) c.push(to_json(x));
    doc.set("profiles", p); doc.set("combos", c); return json::dump(doc);
}
std::string serialize_user(const UserData &u) {
    Value doc = doc_header("aicoach.user"); Value p = Value::make_array(), c = Value::make_array(), pa = Value::make_object(), de = Value::make_array();
    for (auto &x : u.profiles) p.push(to_json(x)); for (auto &x : u.combos) c.push(to_json(x));
    for (auto &kv : u.patches) { Value o = Value::make_object(); if (kv.second.has_enabled) o.set("enabled", Value::make_bool(kv.second.enabled)); if (kv.second.has_priority) o.set("priority", Value::make_int(kv.second.priority)); pa.set(kv.first, o); }
    for (auto &x : u.deleted) de.push(Value::make_string(x));
    doc.set("profiles", p); doc.set("combos", c); doc.set("patches", pa); doc.set("deleted", de); return json::dump(doc);
}

Effective merge(const Defaults &d, const UserData &u, Issues &is) {
    Effective e; std::set<std::string> del(u.deleted.begin(), u.deleted.end()), known;
    std::map<std::string, const Combo *> over; for (auto &c : u.combos) over[c.id] = &c;
    std::map<std::string, const Profile *> pover; for (auto &p : u.profiles) pover[p.id] = &p;
    for (auto &c : d.combos) {
        known.insert(c.id); if (del.count(c.id)) continue;
        Combo x = c; auto o = over.find(c.id); if (o != over.end()) { x = *o->second; x.origin = Combo::UserOverride; }
        auto p = u.patches.find(c.id); if (p != u.patches.end()) { if (p->second.has_enabled) x.enabled = p->second.enabled; if (p->second.has_priority) x.priority = p->second.priority; }
        e.combos.push_back(x);
    }
    for (auto &c : u.combos) if (!known.count(c.id) && !del.count(c.id)) {
        Combo x = c; x.origin = Combo::UserNew; auto p = u.patches.find(c.id); if (p != u.patches.end()) { if (p->second.has_enabled) x.enabled = p->second.enabled; if (p->second.has_priority) x.priority = p->second.priority; }
        e.combos.push_back(x);
    }
    for (auto &kv : u.patches) { bool found = known.count(kv.first) != 0; for (auto &c : u.combos) found = found || c.id == kv.first; if (!found) W(is, "patches." + kv.first, "patch for a combo that does not exist (ignored)"); }
    for (auto &p : d.profiles) { auto o = pover.find(p.id); e.profiles.push_back(o != pover.end() ? *o->second : p); }
    std::set<std::string> dp; for (auto &p : d.profiles) dp.insert(p.id);
    for (auto &p : u.profiles) if (!dp.count(p.id)) e.profiles.push_back(p);
    return e;
}

bool import_user(const std::string &text, UserData &into, const ValidationContext &ctx, Issues &issues) {
    UserData in; LoadResult r = load_user(text, in, ctx); issues.insert(issues.end(), r.issues.begin(), r.issues.end());
    if (r.status != LoadStatus::Ok) return false;
    for (auto &c : in.combos) { auto it = std::find_if(into.combos.begin(), into.combos.end(), [&](const Combo &x) { return x.id == c.id; }); if (it != into.combos.end()) *it = c; else into.combos.push_back(c); }
    for (auto &p : in.profiles) { auto it = std::find_if(into.profiles.begin(), into.profiles.end(), [&](const Profile &x) { return x.id == p.id; }); if (it != into.profiles.end()) *it = p; else into.profiles.push_back(p); }
    for (auto &kv : in.patches) into.patches[kv.first] = kv.second;
    for (auto &d : in.deleted) if (std::find(into.deleted.begin(), into.deleted.end(), d) == into.deleted.end()) into.deleted.push_back(d);
    return true;
}

bool read_file(const std::filesystem::path &p, std::string &out) {
    std::ifstream f(p, std::ios::binary); if (!f) return false; std::ostringstream s; s << f.rdbuf(); out = s.str(); return !f.bad();
}
bool write_file_atomic(const std::filesystem::path &p, const std::string &text, std::string &error) {
    namespace fs = std::filesystem; std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p; tmp += ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) { error = "cannot create " + tmp.string(); return false; } f.write(text.data(), (std::streamsize)text.size()); f.flush(); if (!f) { error = "write failed: " + tmp.string(); fs::remove(tmp, ec); return false; } }
    if (fs::exists(p, ec)) { fs::path bak = p; bak += ".bak"; fs::copy_file(p, bak, fs::copy_options::overwrite_existing, ec); }
    fs::rename(tmp, p, ec); if (ec) { error = "rename failed: " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

LoadResult UserStore::load(const ValidationContext &ctx) {
    std::string text; LoadResult r;
    if (!std::filesystem::exists(file_)) { data = UserData(); status_ = LoadStatus::Missing; r.status = LoadStatus::Missing; return r; }
    if (!read_file(file_, text)) { E(r.issues, "$", "cannot read " + file_.string()); r.status = LoadStatus::Malformed; data = UserData(); status_ = r.status; return r; }
    r = load_user(text, data, ctx); status_ = r.status; return r;
}
bool UserStore::save(std::string &error) {
    if (status_ == LoadStatus::TooNew) { error = "user file was written by a newer version; refusing to overwrite it"; return false; }
    if (status_ == LoadStatus::Malformed) {   /* keep the unreadable file for the player instead of silently destroying it */
        std::error_code ec; std::filesystem::path bad = file_; bad += ".corrupt";
        std::filesystem::rename(file_, bad, ec); if (ec) { error = "cannot move the corrupt file aside: " + ec.message(); return false; }
    }
    if (!write_file_atomic(file_, serialize_user(data), error)) return false;
    status_ = LoadStatus::Ok; return true;
}
void UserStore::reset(std::string &error) {
    if (status_ == LoadStatus::TooNew) { error = "user file was written by a newer version; refusing to delete it"; return; }
    data = UserData(); std::error_code ec; std::filesystem::remove(file_, ec); if (ec) error = ec.message(); status_ = LoadStatus::Missing;
}

}  // namespace
