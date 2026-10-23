/* AI Coach - minimal strict JSON (no dependencies, no exceptions, deterministic output).
 * Why it exists: the runtime has no JSON parser (verified: only the BetterAI training log writes JSON lines). Plugins are static C++ DLLs, so a small
 * header-only parser is the lowest-risk way to read user-editable data. Objects keep insertion order so saved files are stable and diffable. */
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <stdint.h>

namespace aicoach { namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false; double num = 0; bool is_int = false;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value make_null() { return Value(); }
    static Value make_bool(bool v) { Value x; x.type = Bool; x.b = v; return x; }
    static Value make_int(int64_t v) { Value x; x.type = Number; x.num = (double)v; x.is_int = true; return x; }
    static Value make_string(const std::string &s) { Value x; x.type = String; x.str = s; return x; }
    static Value make_array() { Value x; x.type = Array; return x; }
    static Value make_object() { Value x; x.type = Object; return x; }

    const Value *find(const std::string &k) const { if (type != Object) return nullptr; for (auto &p : obj) if (p.first == k) return &p.second; return nullptr; }
    void set(const std::string &k, Value v) { for (auto &p : obj) if (p.first == k) { p.second = std::move(v); return; } obj.emplace_back(k, std::move(v)); }
    void push(Value v) { arr.push_back(std::move(v)); }
};

/* Parses `text`. On failure returns false and fills `err` ("line L col C: message"). Rejects trailing garbage, depth > 32, NaN/Inf, bad escapes. */
bool parse(const std::string &text, Value &out, std::string &err);
/* Deterministic pretty printer (2 spaces). */
std::string dump(const Value &v);

}}  // namespace
