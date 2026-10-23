#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

namespace aicoach { namespace json {
namespace {
struct P {
    const std::string &s; size_t i = 0; std::string &err; bool failed = false;
    P(const std::string &t, std::string &e) : s(t), err(e) {}
    bool fail(const char *m) {
        if (!failed) { failed = true; int line = 1, col = 1; for (size_t k = 0; k < i && k < s.size(); k++) { if (s[k] == '\n') { line++; col = 1; } else col++; }
            char b[160]; snprintf(b, sizeof b, "line %d col %d: %s", line, col, m); err = b; }
        return false;
    }
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    static void utf8(std::string &o, uint32_t c) {
        if (c < 0x80) o += (char)c; else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | c >> 18); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
    }
    bool hex4(uint32_t &v) { v = 0; for (int k = 0; k < 4; k++) { if (i >= s.size()) return fail("truncated \\u escape"); char c = s[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return fail("bad \\u escape"); } return true; }
    bool str(std::string &o) {
        i++;  /* opening quote */
        for (;;) {
            if (i >= s.size()) return fail("unterminated string");
            unsigned char c = (unsigned char)s[i++];
            if (c == '"') return true;
            if (c < 0x20) { i--; return fail("control character in string"); }
            if (c != '\\') { o += (char)c; continue; }
            if (i >= s.size()) return fail("unterminated escape");
            char e = s[i++];
            switch (e) {
                case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break; case 'b': o += '\b'; break;
                case 'f': o += '\f'; break; case 'n': o += '\n'; break; case 'r': o += '\r'; break; case 't': o += '\t'; break;
                case 'u': { uint32_t v; if (!hex4(v)) return false;
                    if (v >= 0xD800 && v < 0xDC00) { if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') { i += 2; uint32_t lo; if (!hex4(lo)) return false; if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair"); v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); } else return fail("lone surrogate"); }
                    else if (v >= 0xDC00 && v <= 0xDFFF) return fail("lone surrogate");
                    utf8(o, v); break; }
                default: i--; return fail("bad escape");
            }
        }
    }
    bool val(Value &v, int depth) {
        if (depth > 32) return fail("nesting too deep");
        ws(); if (i >= s.size()) return fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            i++; v = Value::make_object(); ws();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            for (;;) {
                ws(); if (i >= s.size() || s[i] != '"') return fail("expected object key");
                std::string k; if (!str(k)) return false;
                if (v.find(k)) return fail("duplicate object key");
                ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':'"); i++;
                Value x; if (!val(x, depth + 1)) return false; v.obj.emplace_back(std::move(k), std::move(x));
                ws(); if (i >= s.size()) return fail("unterminated object");
                if (s[i] == ',') { i++; continue; } if (s[i] == '}') { i++; return true; } return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i++; v = Value::make_array(); ws();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            for (;;) {
                Value x; if (!val(x, depth + 1)) return false; v.arr.push_back(std::move(x));
                ws(); if (i >= s.size()) return fail("unterminated array");
                if (s[i] == ',') { i++; continue; } if (s[i] == ']') { i++; return true; } return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v = Value(); v.type = Value::String; return str(v.str); }
        if (!s.compare(i, 4, "true")) { i += 4; v = Value::make_bool(true); return true; }
        if (!s.compare(i, 5, "false")) { i += 5; v = Value::make_bool(false); return true; }
        if (!s.compare(i, 4, "null")) { i += 4; v = Value(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t st = i; bool isint = true; if (s[i] == '-') i++;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
            if (s[i] == '0') { i++; if (i < s.size() && s[i] >= '0' && s[i] <= '9') return fail("leading zero"); } else while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
            if (i < s.size() && s[i] == '.') { isint = false; i++; if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad fraction"); while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++; }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) { isint = false; i++; if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++; if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad exponent"); while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++; }
            double d = strtod(s.substr(st, i - st).c_str(), nullptr);
            if (!isfinite(d)) return fail("number out of range");
            v = Value(); v.type = Value::Number; v.num = d; v.is_int = isint && fabs(d) < 9007199254740992.0; return true;
        }
        return fail("unexpected character");
    }
};
void esc(std::string &o, const std::string &s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) { case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default: if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += (char)c; }
    }
    o += '"';
}
void out(std::string &o, const Value &v, int ind) {
    auto pad = [&](int n) { o.append((size_t)n * 2, ' '); };
    switch (v.type) {
        case Value::Null: o += "null"; break;
        case Value::Bool: o += v.b ? "true" : "false"; break;
        case Value::Number: { char b[40]; if (v.is_int) snprintf(b, sizeof b, "%lld", (long long)v.num); else snprintf(b, sizeof b, "%.17g", v.num); o += b; break; }
        case Value::String: esc(o, v.str); break;
        case Value::Array:
            if (v.arr.empty()) { o += "[]"; break; }
            o += "[\n"; for (size_t k = 0; k < v.arr.size(); k++) { pad(ind + 1); out(o, v.arr[k], ind + 1); o += k + 1 < v.arr.size() ? ",\n" : "\n"; } pad(ind); o += ']'; break;
        case Value::Object:
            if (v.obj.empty()) { o += "{}"; break; }
            o += "{\n"; for (size_t k = 0; k < v.obj.size(); k++) { pad(ind + 1); esc(o, v.obj[k].first); o += ": "; out(o, v.obj[k].second, ind + 1); o += k + 1 < v.obj.size() ? ",\n" : "\n"; } pad(ind); o += '}'; break;
    }
}
}  // namespace

bool parse(const std::string &text, Value &out_v, std::string &err) {
    P p(text, err); Value v;
    size_t st = (text.size() >= 3 && !memcmp(text.data(), "\xEF\xBB\xBF", 3)) ? 3 : 0; p.i = st;   /* tolerate a UTF-8 BOM (Notepad adds one) */
    if (!p.val(v, 0)) return false;
    p.ws(); if (p.i != text.size()) { p.fail("trailing characters"); return false; }
    out_v = std::move(v); return true;
}
std::string dump(const Value &v) { std::string o; out(o, v, 0); o += '\n'; return o; }
}}  // namespace
