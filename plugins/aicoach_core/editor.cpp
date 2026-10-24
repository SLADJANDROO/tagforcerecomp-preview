/* AI Coach - in-game combo editor, logic layer. See editor.h for scope and limits. */
#include "editor.h"
#include <algorithm>
#include <cwctype>

namespace aicoach {

namespace {
std::wstring W(const std::string &s) { return std::wstring(s.begin(), s.end()); }
std::wstring N(long long v) { return W(std::to_string(v)); }
constexpr int kBody = 10;
constexpr size_t kTextWidth = 40;
}

/* ---------------- PadReader ---------------- */
std::vector<Btn> PadReader::update(uint32_t held) {
    static const struct { uint32_t mask; Btn b; bool repeats; } map[12] = {
        {kUp, Btn::Up, true}, {kDown, Btn::Down, true}, {kLeft, Btn::Left, true}, {kRight, Btn::Right, true},
        {kCross, Btn::Cross, false}, {kCircle, Btn::Circle, false}, {kTriangle, Btn::Triangle, false}, {kSquare, Btn::Square, false},
        {kLTrigger, Btn::L, true}, {kRTrigger, Btn::R, true}, {kStart, Btn::Start, false}, {kSelect, Btn::Select, false}};
    std::vector<Btn> out;
    for (int i = 0; i < 12; i++) {
        if (!(held & map[i].mask)) { age_[i] = -1; continue; }
        const int a = ++age_[i];
        if (a == 0) out.push_back(map[i].b);
        else if (map[i].repeats && a >= delay_ && (a - delay_) % (repeat_ > 0 ? repeat_ : 1) == 0) out.push_back(map[i].b);
    }
    return out;
}

/* ---------------- CardIndex ---------------- */
void CardIndex::pump(int budget) {
    while (budget-- > 0 && next_ <= max_) { names_[next_] = name_ ? name_(next_) : std::wstring(); next_++; }
}
static std::wstring fold(std::wstring s) { for (auto &c : s) c = (wchar_t)std::towlower((wint_t)c); return s; }
std::vector<uint32_t> CardIndex::search(const std::wstring &q, size_t limit) const {
    std::vector<uint32_t> out; const std::wstring f = fold(q);
    bool digits = !f.empty(); uint32_t want = 0;
    for (wchar_t c : f) { if (c < L'0' || c > L'9') { digits = false; break; } if (want < 100000) want = want * 10 + (uint32_t)(c - L'0'); }
    for (uint32_t id = 1; id <= max_ && out.size() < limit; id++) {
        if (names_[id].empty()) continue;
        if (f.empty() || (digits && id == want) || fold(names_[id]).find(f) != std::wstring::npos) out.push_back(id);
    }
    return out;
}

/* ---------------- screens ---------------- */
struct Screen {
    int cur = 0;
    virtual ~Screen() {}
    virtual void press(Editor::Impl &, Btn) = 0;
    virtual void render(const Editor::Impl &, std::wstring &title, std::vector<Line> &body, std::wstring &hint) const = 0;
    virtual void pump(Editor::Impl &) {}
};

struct Editor::Impl {
    const Defaults &defs; UserStore &store; ValidationContext ctx; CardIndex &cards;
    std::vector<std::unique_ptr<Screen>> st;
    std::string msg;
    Combo draft; bool has_draft = false, dirty = false;
    Impl(const Defaults &d, UserStore &s, const ValidationContext &c, CardIndex &ci) : defs(d), store(s), ctx(c), cards(ci) {}

    void push(std::unique_ptr<Screen> s) { st.push_back(std::move(s)); }
    void pop() { if (!st.empty()) st.pop_back(); }
    Effective eff() const { Issues is; return merge(defs, store.data, is); }

    std::wstring card_label(uint32_t id) const {
        if (!id) return L"(none)";
        std::wstring n = cards.name(id); return n.empty() ? N(id) : N(id) + L" " + n;
    }
    std::vector<std::string> allowed_actions() const {
        std::vector<std::string> v; for (auto &a : action_specs()) if (!ctx.caps || ctx.caps->actions.count(a.name)) v.push_back(a.name); return v;
    }
    std::string profile_name(int character) const {
        for (auto &p : eff().profiles) if (p.character == character && p.enabled) return p.name; return "";
    }
    bool persist(const UserData &backup, const std::string &ok) {
        std::string err; if (store.save(err)) { msg = ok; return true; }
        store.data = backup; msg = "Not saved: " + err; return false;
    }
};

typedef Editor::Impl Impl;

struct Row { std::wstring text; std::function<void(Btn)> act; };
struct Rows : Screen {
    Impl &im; std::wstring title, hint;
    std::function<void(std::vector<Row> &)> build;
    std::function<bool(Btn)> extra;        /* handled? */
    std::function<void()> on_back;         /* default: pop */
    explicit Rows(Impl &i) : im(i) {}
    void press(Impl &, Btn b) override {
        std::vector<Row> rows; if (build) build(rows);
        const int n = (int)rows.size(); if (cur >= n) cur = n ? n - 1 : 0; if (cur < 0) cur = 0;
        if (b == Btn::Up) { if (n) cur = (cur + n - 1) % n; return; }
        if (b == Btn::Down) { if (n) cur = (cur + 1) % n; return; }
        if (extra) { auto ex = extra; if (ex(b)) return; }
        if (b == Btn::Circle) { if (on_back) { auto f = on_back; f(); } else im.pop(); return; }
        if (cur < n && rows[cur].act) { auto f = rows[cur].act; f(b); }
    }
    void render(const Impl &, std::wstring &t, std::vector<Line> &body, std::wstring &h) const override {
        std::vector<Row> rows; if (build) build(rows);
        t = title; h = hint;
        const int n = (int)rows.size(); int c = std::min(cur, n ? n - 1 : 0);
        int top = std::max(0, std::min(c - kBody / 2, n - kBody));
        for (int i = top; i < n && i < top + kBody; i++) { Line l; l.text = rows[i].text; l.selected = (i == c); body.push_back(l); }
        if (!n) body.push_back(Line{L"(empty)", false, false});
    }
};

/* ---- number entry ---- */
struct Num : Screen {
    Impl &im; std::wstring title; int v, lo, hi; std::function<void(int)> done;
    Num(Impl &i, std::wstring t, int val, int l, int h, std::function<void(int)> d) : im(i), title(std::move(t)), v(val), lo(l), hi(h), done(std::move(d)) { v = std::max(lo, std::min(hi, v)); }
    void press(Impl &, Btn b) override {
        auto add = [&](int d) { long long x = (long long)v + d; v = (int)std::max<long long>(lo, std::min<long long>(hi, x)); };
        switch (b) {
            case Btn::Right: add(1); break; case Btn::Left: add(-1); break; case Btn::Up: add(10); break; case Btn::Down: add(-10); break;
            case Btn::R: add(100); break; case Btn::L: add(-100); break;
            case Btn::Cross: { auto f = done; int val = v; im.pop(); if (f) f(val); break; }
            case Btn::Circle: im.pop(); break;
            default: break;
        }
    }
    void render(const Impl &, std::wstring &t, std::vector<Line> &body, std::wstring &h) const override {
        t = title; body.push_back(Line{L"Value: " + N(v), true, false}); body.push_back(Line{L"allowed " + N(lo) + L" .. " + N(hi), false, false});
        h = L"L/R +-1  U/D +-10  L1/R1 +-100  X ok  O cancel";
    }
};

/* ---- text entry ---- */
struct Txt : Screen {
    Impl &im; std::wstring title; std::string val; size_t maxlen; int row = 0, col = 0; std::function<void(const std::string &)> done;
    Txt(Impl &i, std::wstring t, std::string v, size_t m, std::function<void(const std::string &)> d) : im(i), title(std::move(t)), val(std::move(v)), maxlen(m), done(std::move(d)) {}
    static const std::vector<std::string> &grid() {
        static const std::vector<std::string> g = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz", "0123456789", "-_.:'!?&/() "};
        return g;
    }
    void press(Impl &, Btn b) override {
        const auto &g = grid(); const int rn = (int)g.size();
        switch (b) {
            case Btn::Up: row = (row + rn - 1) % rn; break; case Btn::Down: row = (row + 1) % rn; break;
            case Btn::Left: col--; break; case Btn::Right: col++; break;
            case Btn::Cross: if (val.size() < maxlen) val.push_back(g[row][col % (int)g[row].size()]); break;
            case Btn::Square: if (!val.empty()) val.pop_back(); break;
            case Btn::Start: { auto f = done; std::string v = val; im.pop(); if (f) f(v); return; }
            case Btn::Circle: im.pop(); return;
            default: break;
        }
        const int len = (int)g[row].size(); col = ((col % len) + len) % len;
    }
    void render(const Impl &, std::wstring &t, std::vector<Line> &body, std::wstring &h) const override {
        t = title; body.push_back(Line{W(val) + L"_", false, false});
        const auto &g = grid();
        for (int r = 0; r < (int)g.size(); r++) {
            std::wstring s; for (int c = 0; c < (int)g[r].size(); c++) { const bool sel = r == row && c == col; s += sel ? L'[' : L' '; s += g[r][c]; s += sel ? L']' : L' '; }
            body.push_back(Line{s, r == row, false});
        }
        h = L"X add  Sq delete  Start ok  O cancel";
    }
};

/* ---- card picker ---- */
struct CardPick : Screen {
    Impl &im; std::wstring title, query; std::function<void(uint32_t)> done; std::vector<uint32_t> results; uint32_t seen = ~0u; std::wstring seen_q = L"?";
    CardPick(Impl &i, std::wstring t, std::function<void(uint32_t)> d) : im(i), title(std::move(t)), done(std::move(d)) { refresh(); }
    void refresh() {
        if (seen == im.cards.resolved() && seen_q == query) return;
        seen = im.cards.resolved(); seen_q = query; results = im.cards.search(query, 500); if (cur >= (int)results.size()) cur = 0;
    }
    void pump(Impl &) override { refresh(); }
    void press(Impl &, Btn b) override {
        refresh(); const int n = (int)results.size();
        switch (b) {
            case Btn::Up: if (n) cur = (cur + n - 1) % n; break; case Btn::Down: if (n) cur = (cur + 1) % n; break;
            case Btn::L: cur = std::max(0, cur - kBody); break; case Btn::R: if (n) cur = std::min(n - 1, cur + kBody); break;
            case Btn::Cross: if (n) { auto f = done; uint32_t id = results[cur]; im.pop(); if (f) f(id); } break;
            case Btn::Circle: im.pop(); break;
            case Btn::Square: {   /* edit the search text */
                Impl *p = &im; CardPick *self = this;
                std::string q; for (wchar_t c : query) q.push_back((char)c);
                im.push(std::unique_ptr<Screen>(new Txt(im, L"Search card name or id", q, 24, [p, self](const std::string &s) { (void)p; self->query = W(s); self->cur = 0; self->refresh(); })));
                break;
            }
            case Btn::Triangle: {   /* type an id directly: works even when names are not available */
                auto f = done; Impl *p = &im;
                im.push(std::unique_ptr<Screen>(new Num(im, L"Card id", 1000, 1, (int)kMaxCardId, [p, f](int v) { p->pop(); if (f) f((uint32_t)v); })));
                break;
            }
            default: break;
        }
    }
    void render(const Impl &, std::wstring &t, std::vector<Line> &body, std::wstring &h) const override {
        t = title;
        body.push_back(Line{L"Search: " + (query.empty() ? std::wstring(L"(all)") : query) + L"  " + N((long long)results.size()) + L" found", false, false});
        if (!im.cards.complete()) body.push_back(Line{L"names loaded " + N(im.cards.resolved()) + L"/" + N(im.cards.max_id()), false, false});
        const int n = (int)results.size(); const int rows = kBody - (int)body.size();
        int top = std::max(0, std::min(cur - rows / 2, n - rows));
        for (int i = top; i < n && i < top + rows; i++) body.push_back(Line{N(results[i]) + L" " + im.cards.name(results[i]), i == cur, false});
        if (!n) body.push_back(Line{L"no match (Triangle: enter an id)", false, false});
        h = L"X pick  Sq search  Tri id  L1/R1 page  O back";
    }
};

/* ---------------- helpers that build screens ---------------- */
static Rows *new_rows(Impl &im, const std::wstring &title, const std::wstring &hint) { Rows *r = new Rows(im); r->title = title; r->hint = hint; return r; }

static void push_choice(Impl &im, const std::wstring &title, const std::vector<std::string> &opts, const std::string &current, std::function<void(const std::string &)> done) {
    Rows *r = new_rows(im, title, L"X choose  O back"); Impl *p = &im;
    r->build = [opts, current, done, p](std::vector<Row> &rows) {
        for (auto &o : opts) rows.push_back(Row{(o == current ? L"* " : L"  ") + (o.empty() ? std::wstring(L"(none)") : W(o)), [o, done, p](Btn b) { if (b == Btn::Cross) { p->pop(); if (done) done(o); } }});
    };
    for (size_t i = 0; i < opts.size(); i++) if (opts[i] == current) r->cur = (int)i;
    im.push(std::unique_ptr<Screen>(r));
}
static void push_menu(Impl &im, const std::wstring &title, std::vector<std::pair<std::wstring, std::function<void()>>> items) {
    Rows *r = new_rows(im, title, L"X choose  O back"); Impl *p = &im;
    r->build = [items, p](std::vector<Row> &rows) {
        for (auto &it : items) { auto fn = it.second; rows.push_back(Row{it.first, [fn, p](Btn b) { if (b == Btn::Cross) { p->pop(); if (fn) fn(); } }}); }
    };
    im.push(std::unique_ptr<Screen>(r));
}
static std::vector<std::string> to_vec(const std::set<std::string> &s) { return std::vector<std::string>(s.begin(), s.end()); }

/* ---- conditions ---- */
typedef std::function<std::vector<Condition> &()> CondVec;
typedef std::function<std::vector<std::string>()> IdsFn;

static void init_condition(Condition &c, const std::string &type) {
    const bool neg = c.negate; c = Condition(); c.type = type; c.negate = neg;
    if (type == "monster_position") c.position = "attack";
    else if (type == "free_zones") { c.zone = "monster"; c.op = "ge"; c.value = 1; }
    else if (type == "lp_compare") { c.who = "self"; c.op = "ge"; c.value = 1000; }
    else if (type == "opponent_monster_count") { c.op = "ge"; c.value = 0; }
    else if (type == "phase_is") c.phase = "main1";
}
static std::wstring describe(const Impl &im, const Condition &c) {
    std::wstring s = c.negate ? L"not " : L""; s += W(c.type);
    const std::string &t = c.type;
    if (t == "card_in_hand" || t == "card_on_field" || t == "card_in_graveyard" || t == "card_in_extra" || t == "card_unused") s += L" " + im.card_label(c.card);
    else if (t == "monster_position") s += L" " + N(c.card) + L" " + W(c.position);
    else if (t == "free_zones") s += L" " + W(c.zone) + L" " + W(c.op) + L" " + N(c.value);
    else if (t == "lp_compare") s += L" " + W(c.who) + L" " + W(c.op) + L" " + N(c.value);
    else if (t == "opponent_monster_count") s += L" " + W(c.op) + L" " + N(c.value);
    else if (t == "phase_is") s += L" " + W(c.phase);
    else if (t == "step_succeeded" || t == "step_failed") s += L" " + W(c.step);
    return s;
}
static void push_cond_edit(Impl &im, CondVec get, size_t idx, IdsFn earlier) {
    Rows *r = new_rows(im, L"Condition", L"X change  L/R toggle  O back"); Impl *p = &im;
    r->build = [p, get, idx, earlier](std::vector<Row> &rows) {
        auto &v = get(); if (idx >= v.size()) return;
        const Condition c = v[idx]; const std::string t = c.type;
        auto set = [p, get, idx](std::function<void(Condition &)> f) { auto &vv = get(); if (idx < vv.size()) { f(vv[idx]); p->dirty = true; } };
        rows.push_back(Row{L"Type: " + W(t), [p, set](Btn b) { if (b != Btn::Cross) return; std::vector<std::string> o; for (auto *x : condition_types()) o.push_back(x);
            push_choice(*p, L"Condition type", o, "", [set](const std::string &s) { set([&](Condition &x) { init_condition(x, s); }); }); }});
        rows.push_back(Row{std::wstring(L"Negate (not): ") + (c.negate ? L"yes" : L"no"), [set](Btn b) { if (b == Btn::Cross || b == Btn::Left || b == Btn::Right) set([](Condition &x) { x.negate = !x.negate; }); }});
        const bool card = t == "card_in_hand" || t == "card_on_field" || t == "card_in_graveyard" || t == "card_in_extra" || t == "card_unused" || t == "monster_position";
        if (card) rows.push_back(Row{L"Card: " + p->card_label(c.card), [p, set](Btn b) { if (b == Btn::Cross) { Impl *q = p; q->push(std::unique_ptr<Screen>(new CardPick(*q, L"Condition card", [set](uint32_t id) { set([&](Condition &x) { x.card = id; }); }))); } }});
        if (t == "monster_position") rows.push_back(Row{L"Position: " + W(c.position), [p, set](Btn b) { if (b == Btn::Cross) push_choice(*p, L"Position", to_vec(positions()), "", [set](const std::string &s) { set([&](Condition &x) { x.position = s; }); }); }});
        if (t == "free_zones") rows.push_back(Row{L"Zone: " + W(c.zone), [p, set](Btn b) { if (b == Btn::Cross) push_choice(*p, L"Zone", to_vec(zones()), "", [set](const std::string &s) { set([&](Condition &x) { x.zone = s; }); }); }});
        if (t == "lp_compare") rows.push_back(Row{L"Whose LP: " + W(c.who), [p, set](Btn b) { if (b == Btn::Cross) push_choice(*p, L"Whose LP", to_vec(whos()), "", [set](const std::string &s) { set([&](Condition &x) { x.who = s; }); }); }});
        if (t == "free_zones" || t == "lp_compare" || t == "opponent_monster_count") {
            rows.push_back(Row{L"Compare: " + W(c.op), [p, set](Btn b) { if (b == Btn::Cross) push_choice(*p, L"Compare (lt le eq ge gt ne)", {"lt", "le", "eq", "ge", "gt", "ne"}, "", [set](const std::string &s) { set([&](Condition &x) { x.op = s; }); }); }});
            const int hi = t == "lp_compare" ? 99999 : 5;
            const int val = c.value;
            rows.push_back(Row{L"Value: " + N(c.value), [p, set, hi, val](Btn b) { if (b == Btn::Cross) p->push(std::unique_ptr<Screen>(new Num(*p, L"Value", val, 0, hi, [set](int v) { set([&](Condition &x) { x.value = v; }); }))); }});
        }
        if (t == "phase_is") rows.push_back(Row{L"Phase: " + W(c.phase), [p, set](Btn b) { if (b == Btn::Cross) push_choice(*p, L"Phase", {"draw", "standby", "main1", "battle", "main2", "end"}, "", [set](const std::string &s) { set([&](Condition &x) { x.phase = s; }); }); }});
        if (t == "step_succeeded" || t == "step_failed") rows.push_back(Row{L"Step: " + (c.step.empty() ? std::wstring(L"(none)") : W(c.step)), [p, set, earlier](Btn b) { if (b == Btn::Cross) { auto ids = earlier(); if (ids.empty()) { p->msg = "No earlier step to refer to"; return; } push_choice(*p, L"Earlier step", ids, "", [set](const std::string &s) { set([&](Condition &x) { x.step = s; }); }); } }});
    };
    im.push(std::unique_ptr<Screen>(r));
}
static void push_cond_list(Impl &im, const std::wstring &title, CondVec get, IdsFn earlier) {
    Rows *r = new_rows(im, title, L"X edit  Sq add  Tri/Sel delete  O back"); Impl *p = &im;
    r->build = [p, get, earlier](std::vector<Row> &rows) {
        auto &v = get();
        for (size_t i = 0; i < v.size(); i++) rows.push_back(Row{N((long long)i + 1) + L". " + describe(*p, v[i]), [p, get, earlier, i](Btn b) {
            if (b == Btn::Cross) push_cond_edit(*p, get, i, earlier);
            else if (b == Btn::Triangle || b == Btn::Select) { auto &vv = get(); if (i < vv.size()) { vv.erase(vv.begin() + (long)i); p->dirty = true; } }
        }});
    };
    r->extra = [p, get, earlier](Btn b) {
        if (b != Btn::Square) return false;
        if (get().size() >= kMaxConditions) { p->msg = "Too many conditions"; return true; }
        std::vector<std::string> o; for (auto *x : condition_types()) o.push_back(x);
        push_choice(*p, L"New condition type", o, "", [p, get, earlier](const std::string &t) {
            Condition c; init_condition(c, t); get().push_back(c); p->dirty = true; push_cond_edit(*p, get, get().size() - 1, earlier);
        });
        return true;
    };
    im.push(std::unique_ptr<Screen>(r));
}

/* ---- card list / deck list ---- */
static void push_card_list(Impl &im, const std::wstring &title, std::function<std::vector<uint32_t> &()> get) {
    Rows *r = new_rows(im, title, L"X replace  Sq add  Tri/Sel delete  O back"); Impl *p = &im;
    r->build = [p, get](std::vector<Row> &rows) {
        auto &v = get();
        for (size_t i = 0; i < v.size(); i++) rows.push_back(Row{p->card_label(v[i]), [p, get, i](Btn b) {
            if (b == Btn::Cross) p->push(std::unique_ptr<Screen>(new CardPick(*p, L"Replace card", [p, get, i](uint32_t id) { auto &vv = get(); if (i < vv.size()) { vv[i] = id; p->dirty = true; } })));
            else if (b == Btn::Triangle || b == Btn::Select) { auto &vv = get(); if (i < vv.size()) { vv.erase(vv.begin() + (long)i); p->dirty = true; } }
        }});
    };
    r->extra = [p, get](Btn b) {
        if (b != Btn::Square) return false;
        p->push(std::unique_ptr<Screen>(new CardPick(*p, L"Add card", [p, get](uint32_t id) {
            auto &vv = get(); if (std::find(vv.begin(), vv.end(), id) != vv.end()) { p->msg = "Already in the list"; return; } vv.push_back(id); p->dirty = true; })));
        return true;
    };
    im.push(std::unique_ptr<Screen>(r));
}
static void push_deck_list(Impl &im) {
    Rows *r = new_rows(im, L"Decks (empty = every deck of the character)", L"Sq add  Tri/Sel delete  O back"); Impl *p = &im;
    r->build = [p](std::vector<Row> &rows) {
        auto &v = p->draft.decks;
        for (size_t i = 0; i < v.size(); i++) {
            const bool ok = p->draft.character == 0 || deck_belongs_to_character(p->draft.character, v[i]);
            rows.push_back(Row{L"deck " + N(v[i]) + (ok ? L"" : L"  (not this character's?)"), [p, i](Btn b) {
                if (b == Btn::Triangle || b == Btn::Select) { auto &vv = p->draft.decks; if (i < vv.size()) { vv.erase(vv.begin() + (long)i); p->dirty = true; } }
            }});
        }
    };
    r->extra = [p](Btn b) {
        if (b != Btn::Square) return false;
        p->push(std::unique_ptr<Screen>(new Num(*p, L"Deck id", 1, 1, kMaxDeckId, [p](int v) {
            auto &vv = p->draft.decks; if (std::find(vv.begin(), vv.end(), v) != vv.end()) { p->msg = "Already in the list"; return; } vv.push_back(v); p->dirty = true; })));
        return true;
    };
    im.push(std::unique_ptr<Screen>(r));
}

/* ---- steps ---- */
static std::vector<std::string> step_ids_before(const Impl &im, size_t idx) { std::vector<std::string> v; for (size_t i = 0; i < idx && i < im.draft.steps.size(); i++) v.push_back(im.draft.steps[i].id); return v; }
static std::string new_step_id(const Combo &c) {
    for (int n = 1;; n++) { std::string id = "s" + std::to_string(n); bool used = false; for (auto &s : c.steps) used = used || s.id == id; if (!used) return id; }
}
static const ActionSpec *spec_of(const std::string &a) { for (auto &s : action_specs()) if (a == s.name) return &s; return nullptr; }
static bool needs(const std::string &action, const char *param) { if (auto *s = spec_of(action)) for (auto *r : s->required) if (std::string(r) == param) return true; return false; }

static void push_step_edit(Impl &im, size_t idx) {
    Rows *r = new_rows(im, L"Step " + N((long long)idx + 1), L"X change  O back"); Impl *p = &im;
    r->build = [p, idx](std::vector<Row> &rows) {
        if (idx >= p->draft.steps.size()) return;
        const Step s = p->draft.steps[idx];
        auto set = [p, idx](std::function<void(Step &)> f) { if (idx < p->draft.steps.size()) { f(p->draft.steps[idx]); p->dirty = true; } };
        rows.push_back(Row{L"Action: " + W(s.action), [p, set](Btn b) { if (b != Btn::Cross) return;
            push_choice(*p, L"Action", p->allowed_actions(), "", [set](const std::string &a) { set([&](Step &x) {
                x.action = a; if (!needs(a, "position")) x.position.clear();
                const bool takes_card = needs(a, "card") || a == "select_card"; if (!takes_card) x.card = 0;
                if (a != "select_card" && a != "declare_attack") { x.target_policy.clear(); x.target_card = 0; } }); }); }});
        if (needs(s.action, "card") || s.action == "select_card")
            rows.push_back(Row{L"Card: " + p->card_label(s.card), [p, set](Btn b) { if (b == Btn::Cross) p->push(std::unique_ptr<Screen>(new CardPick(*p, L"Step card", [set](uint32_t id) { set([&](Step &x) { x.card = id; }); }))); }});
        if (needs(s.action, "position"))
            rows.push_back(Row{L"Position: " + (s.position.empty() ? std::wstring(L"(none)") : W(s.position)), [p, set](Btn b) { if (b == Btn::Cross) push_choice(*p, L"Position", to_vec(positions()), "", [set](const std::string &v) { set([&](Step &x) { x.position = v; }); }); }});
        if (s.action == "select_card" || s.action == "declare_attack") {
            rows.push_back(Row{L"Target: " + (s.target_policy.empty() ? std::wstring(L"(none)") : W(s.target_policy)), [p, set](Btn b) { if (b == Btn::Cross) { std::vector<std::string> o = {""}; for (auto &x : target_policies()) o.push_back(x);
                push_choice(*p, L"Target policy", o, "", [set](const std::string &v) { set([&](Step &x) { x.target_policy = v; if (v != "specific_card") x.target_card = 0; }); }); } }});
            if (s.target_policy == "specific_card") rows.push_back(Row{L"Target card: " + p->card_label(s.target_card), [p, set](Btn b) { if (b == Btn::Cross) p->push(std::unique_ptr<Screen>(new CardPick(*p, L"Target card", [set](uint32_t id) { set([&](Step &x) { x.target_card = id; }); }))); }});
        }
        auto cv = [p, idx]() -> std::vector<Condition> & { return p->draft.steps[idx].conditions; };
        auto ev = [p, idx]() -> std::vector<Condition> & { return p->draft.steps[idx].expect; };
        IdsFn before = [p, idx]() { return step_ids_before(*p, idx); };
        IdsFn upto = [p, idx]() { return step_ids_before(*p, idx + 1); };
        rows.push_back(Row{L"Before: " + N((long long)s.conditions.size()) + L" condition(s)", [p, cv, before](Btn b) { if (b == Btn::Cross) push_cond_list(*p, L"Conditions before the step", cv, before); }});
        rows.push_back(Row{L"Expect after: " + N((long long)s.expect.size()) + L" condition(s)", [p, ev, upto](Btn b) { if (b == Btn::Cross) push_cond_list(*p, L"Expected after the step", ev, upto); }});
        rows.push_back(Row{L"If it fails: " + W(s.on_fail.policy), [p, set](Btn b) { if (b == Btn::Cross) push_choice(*p, L"On failure", to_vec(fail_policies()), "", [set](const std::string &v) { set([&](Step &x) { x.on_fail.policy = v; if (v != "goto") x.on_fail.goto_step.clear(); }); }); }});
        if (s.on_fail.policy == "goto") rows.push_back(Row{L"Jump to: " + (s.on_fail.goto_step.empty() ? std::wstring(L"(none)") : W(s.on_fail.goto_step)), [p, set, idx](Btn b) {
            if (b != Btn::Cross) return; std::vector<std::string> later; for (size_t i = idx + 1; i < p->draft.steps.size(); i++) later.push_back(p->draft.steps[i].id);
            if (later.empty()) { p->msg = "No later step to jump to"; return; } push_choice(*p, L"Jump to (forward only)", later, "", [set](const std::string &v) { set([&](Step &x) { x.on_fail.goto_step = v; }); }); }});
    };
    im.push(std::unique_ptr<Screen>(r));
}
static void push_step_list(Impl &im) {
    Rows *r = new_rows(im, L"Steps", L"X edit  Sq add  Tri/Sel delete  L1/R1 move  O back"); Impl *p = &im;
    r->build = [p](std::vector<Row> &rows) {
        auto &v = p->draft.steps;
        for (size_t i = 0; i < v.size(); i++) {
            std::wstring t = N((long long)i + 1) + L". " + W(v[i].action); if (v[i].card) t += L" " + p->card_label(v[i].card);
            rows.push_back(Row{t, [p, i](Btn b) {
                auto &vv = p->draft.steps;
                if (b == Btn::Cross) push_step_edit(*p, i);
                else if ((b == Btn::Triangle || b == Btn::Select) && i < vv.size()) { vv.erase(vv.begin() + (long)i); p->dirty = true; p->msg = "Step removed (use Check to find references to it)"; }
            }});
        }
    };
    r->extra = [p](Btn b) {
        auto &v = p->draft.steps; Screen *self = p->st.back().get();
        if (b == Btn::Square) {
            if (v.size() >= kMaxSteps) { p->msg = "Too many steps"; return true; }
            push_choice(*p, L"New step: action", p->allowed_actions(), "", [p](const std::string &a) {
                Step s; s.id = new_step_id(p->draft); s.action = a; if (needs(a, "position")) s.position = "attack"; p->draft.steps.push_back(s); p->dirty = true; push_step_edit(*p, p->draft.steps.size() - 1); });
            return true;
        }
        if ((b == Btn::L || b == Btn::R) && !v.empty()) {
            size_t i = (size_t)std::max(0, self->cur); if (i >= v.size()) return true;
            if (b == Btn::L && i > 0) { std::swap(v[i], v[i - 1]); self->cur--; p->dirty = true; }
            if (b == Btn::R && i + 1 < v.size()) { std::swap(v[i], v[i + 1]); self->cur++; p->dirty = true; }
            return true;
        }
        return false;
    };
    im.push(std::unique_ptr<Screen>(r));
}

/* ---- saving ---- */
static const Combo *default_combo(const Impl &im, const std::string &id) { for (auto &c : im.defs.combos) if (c.id == id) return &c; return nullptr; }
static void push_issues(Impl &im) {
    Issues is; validate_combo(im.draft, im.ctx, "combo", is);
    Rows *r = new_rows(im, L"Check: " + N((long long)is.size()) + L" finding(s)", L"O back");
    std::vector<std::wstring> lines;
    if (is.empty()) lines.push_back(L"No problems found.");
    for (auto &i : is) {
        std::wstring t = std::wstring(i.sev == Issue::Error ? L"[E] " : L"[W] ") + W(i.path.size() > 6 && i.path.compare(0, 6, "combo.") == 0 ? i.path.substr(6) : i.path) + L": " + W(i.msg);
        while (t.size() > kTextWidth) { size_t cut = t.rfind(L' ', kTextWidth); if (cut == std::wstring::npos || cut < 10) cut = kTextWidth; lines.push_back(t.substr(0, cut)); t = L"   " + t.substr(cut == kTextWidth ? cut : cut + 1); }
        lines.push_back(t);
    }
    r->build = [lines](std::vector<Row> &rows) { for (auto &l : lines) rows.push_back(Row{l, nullptr}); };
    im.push(std::unique_ptr<Screen>(r));
}
static bool save_draft(Impl &im) {
    Issues is; validate_combo(im.draft, im.ctx, "combo", is);
    int errs = 0; for (auto &i : is) if (i.sev == Issue::Error) errs++;
    if (errs) { im.msg = "Not saved: " + std::to_string(errs) + " error(s), open Check"; return false; }
    const UserData backup = im.store.data; UserData &u = im.store.data;
    u.combos.erase(std::remove_if(u.combos.begin(), u.combos.end(), [&](const Combo &c) { return c.id == im.draft.id; }), u.combos.end());
    u.patches.erase(im.draft.id);
    u.deleted.erase(std::remove(u.deleted.begin(), u.deleted.end(), im.draft.id), u.deleted.end());
    const Combo *d = default_combo(im, im.draft.id);
    const bool same = d && json::dump(to_json(*d)) == json::dump(to_json(im.draft));
    if (!same) u.combos.push_back(im.draft);
    if (!im.persist(backup, same ? "Saved (identical to the shipped default)" : "Saved")) return false;
    im.draft.origin = d ? (same ? Combo::Default : Combo::UserOverride) : Combo::UserNew; im.dirty = false;
    return true;
}
static void leave_editor(Impl &im) { im.has_draft = false; im.dirty = false; im.pop(); }

static void push_combo_edit(Impl &im) {
    Rows *r = new_rows(im, L"Edit combo", L"X change  L/R adjust  O back"); Impl *p = &im;
    r->build = [p](std::vector<Row> &rows) {
        Combo &c = p->draft; auto mark = [p]() { p->dirty = true; };
        rows.push_back(Row{L"Name: " + W(c.name), [p, mark](Btn b) { if (b == Btn::Cross) p->push(std::unique_ptr<Screen>(new Txt(*p, L"Combo name", p->draft.name, 40, [p, mark](const std::string &s) { p->draft.name = s; mark(); }))); }});
        rows.push_back(Row{std::wstring(L"Enabled: ") + (c.enabled ? L"yes" : L"no"), [p, mark](Btn b) { if (b == Btn::Cross || b == Btn::Left || b == Btn::Right) { p->draft.enabled = !p->draft.enabled; mark(); } }});
        rows.push_back(Row{L"Priority: " + N(c.priority), [p, mark](Btn b) {
            if (b == Btn::Left || b == Btn::Right) { p->draft.priority = std::max(0, std::min(1000, p->draft.priority + (b == Btn::Right ? 10 : -10))); mark(); }
            else if (b == Btn::Cross) p->push(std::unique_ptr<Screen>(new Num(*p, L"Priority (0..1000)", p->draft.priority, 0, 1000, [p, mark](int v) { p->draft.priority = v; mark(); })));
        }});
        std::string pn = p->profile_name(c.character);
        rows.push_back(Row{L"Character: " + (c.character ? N(c.character) + (pn.empty() ? L"" : L" " + W(pn)) : std::wstring(L"any")), [p, mark](Btn b) {
            if (b == Btn::Cross) p->push(std::unique_ptr<Screen>(new Num(*p, L"Character id (0 = any)", p->draft.character, 0, kMaxCharacterId, [p, mark](int v) { if (v == 1) v = kMinCharacterId; p->draft.character = v; mark(); })));
        }});
        std::wstring ds; for (size_t i = 0; i < c.decks.size(); i++) ds += (i ? L"," : L"") + N(c.decks[i]);
        rows.push_back(Row{L"Decks: " + (ds.empty() ? std::wstring(L"all") : ds), [p](Btn b) { if (b == Btn::Cross) push_deck_list(*p); }});
        rows.push_back(Row{L"Required cards: " + N((long long)c.required_cards.size()), [p](Btn b) { if (b == Btn::Cross) push_card_list(*p, L"Required cards", [p]() -> std::vector<uint32_t> & { return p->draft.required_cards; }); }});
        rows.push_back(Row{L"Optional cards: " + N((long long)c.optional_cards.size()), [p](Btn b) { if (b == Btn::Cross) push_card_list(*p, L"Optional cards", [p]() -> std::vector<uint32_t> & { return p->draft.optional_cards; }); }});
        rows.push_back(Row{L"Start conditions: " + N((long long)c.conditions.size()), [p](Btn b) { if (b == Btn::Cross) push_cond_list(*p, L"Conditions to start", [p]() -> std::vector<Condition> & { return p->draft.conditions; }, []() { return std::vector<std::string>(); }); }});
        rows.push_back(Row{L"Steps: " + N((long long)c.steps.size()), [p](Btn b) { if (b == Btn::Cross) push_step_list(*p); }});
        rows.push_back(Row{L"Success conditions: " + N((long long)c.success.size()), [p](Btn b) { if (b == Btn::Cross) push_cond_list(*p, L"Combo succeeded when", [p]() -> std::vector<Condition> & { return p->draft.success; }, [p]() { return step_ids_before(*p, p->draft.steps.size()); }); }});
        rows.push_back(Row{L"If interrupted: " + W(c.interruption), [p, mark](Btn b) { if (b == Btn::Cross) push_choice(*p, L"Interruption", to_vec(interruptions()), p->draft.interruption, [p, mark](const std::string &v) { p->draft.interruption = v; mark(); }); }});
        rows.push_back(Row{L"Preferred phase: " + (c.preferred_phase.empty() ? std::wstring(L"any") : W(c.preferred_phase)), [p, mark](Btn b) { if (b == Btn::Cross) { std::vector<std::string> o = {""}; for (auto &x : phases()) o.push_back(x);
            push_choice(*p, L"Preferred phase", o, p->draft.preferred_phase, [p, mark](const std::string &v) { p->draft.preferred_phase = v; mark(); }); } }});
        rows.push_back(Row{L"[ Check for problems ]", [p](Btn b) { if (b == Btn::Cross) push_issues(*p); }});
        rows.push_back(Row{p->dirty ? L"[ Save ]  (unsaved changes)" : L"[ Save ]", [p](Btn b) { if (b == Btn::Cross) save_draft(*p); }});
        rows.push_back(Row{L"[ Back ]", [p](Btn b) { if (b == Btn::Cross) { Screen *s = p->st.back().get(); (void)s; if (!p->dirty) leave_editor(*p); else
            push_menu(*p, L"Unsaved changes", {{L"Save and leave", [p]() { if (save_draft(*p)) leave_editor(*p); }}, {L"Leave without saving", [p]() { leave_editor(*p); }}, {L"Keep editing", nullptr}}); } }});
    };
    r->on_back = [p]() {
        if (!p->dirty) { leave_editor(*p); return; }
        push_menu(*p, L"Unsaved changes", {{L"Save and leave", [p]() { if (save_draft(*p)) leave_editor(*p); }}, {L"Leave without saving", [p]() { leave_editor(*p); }}, {L"Keep editing", nullptr}});
    };
    im.push(std::unique_ptr<Screen>(r));
}
static void open_draft(Impl &im, const Combo &c, bool dirty) { im.draft = c; im.has_draft = true; im.dirty = dirty; push_combo_edit(im); }

/* ---- combo list ---- */
static std::string unique_id(const Impl &im, const std::string &base) {
    std::set<std::string> used; for (auto &c : im.defs.combos) used.insert(c.id); for (auto &c : im.store.data.combos) used.insert(c.id);
    for (auto &d : im.store.data.deleted) used.insert(d);
    for (int n = 1;; n++) { std::string id = base + std::to_string(n); if (!used.count(id)) return id; }
}
static void set_patch(Impl &im, const Combo &c, bool enabled_change, bool enabled, int priority) {
    const UserData backup = im.store.data; Patch &p = im.store.data.patches[c.id];
    if (enabled_change) { p.has_enabled = true; p.enabled = enabled; } else { p.has_priority = true; p.priority = priority; }
    im.persist(backup, enabled_change ? (enabled ? "Enabled" : "Disabled") : "Priority " + std::to_string(priority));
}
static void delete_combo(Impl &im, const Combo &c) {
    const UserData backup = im.store.data; UserData &u = im.store.data;
    u.patches.erase(c.id);
    if (c.origin == Combo::Default) { u.deleted.push_back(c.id); im.persist(backup, "Hidden (see 'Restore hidden defaults')"); }
    else { u.combos.erase(std::remove_if(u.combos.begin(), u.combos.end(), [&](const Combo &x) { return x.id == c.id; }), u.combos.end()); im.persist(backup, c.origin == Combo::UserOverride ? "Reverted to the shipped default" : "Deleted"); }
}
static void push_combo_list(Impl &im) {
    Rows *r = new_rows(im, L"AI Coach - combos", L"X edit Sq new Start copy Sel delete Tri on/off L/R prio O exit"); Impl *p = &im;
    r->build = [p](std::vector<Row> &rows) {
        Effective e = p->eff();
        for (auto &c : e.combos) {
            const wchar_t *tag = c.origin == Combo::Default ? L"" : c.origin == Combo::UserOverride ? L" *edited" : L" *mine";
            rows.push_back(Row{std::wstring(c.enabled ? L"[x] " : L"[ ] ") + W(c.name) + L"  p" + N(c.priority) + tag, nullptr});
        }
        rows.push_back(Row{L"+ New combo", nullptr});
        if (!p->store.data.deleted.empty()) rows.push_back(Row{L"Restore hidden defaults (" + N((long long)p->store.data.deleted.size()) + L")", nullptr});
    };
    r->extra = [p](Btn b) {
        Effective e = p->eff(); Screen *self = p->st.back().get(); const int i = self->cur;
        const bool on_combo = i >= 0 && i < (int)e.combos.size();
        auto make_new = [p]() { Combo c; c.id = unique_id(*p, "user."); c.name = "New combo"; c.priority = 300; c.origin = Combo::UserNew; open_draft(*p, c, true); };
        if (b == Btn::Square) { make_new(); return true; }
        if (b == Btn::Circle) { p->st.clear(); return true; }
        if (!on_combo) {
            if (b == Btn::Cross) {
                if (i == (int)e.combos.size()) make_new();
                else if (!p->store.data.deleted.empty()) { const UserData bk = p->store.data; p->store.data.deleted.clear(); p->persist(bk, "Hidden defaults restored"); }
            }
            return b == Btn::Cross;
        }
        const Combo &c = e.combos[(size_t)i];
        switch (b) {
            case Btn::Cross: open_draft(*p, c, false); return true;
            case Btn::Left: case Btn::Right: set_patch(*p, c, false, false, std::max(0, std::min(1000, c.priority + (b == Btn::Right ? 10 : -10)))); return true;
            case Btn::Triangle: set_patch(*p, c, true, !c.enabled, 0); return true;
            case Btn::Start: { Combo d = c; d.id = unique_id(*p, c.id + ".copy"); d.name = c.name + " (copy)"; d.origin = Combo::UserNew; open_draft(*p, d, true); return true; }
            case Btn::Select: {
                Combo cc = c; const wchar_t *what = c.origin == Combo::Default ? L"Hide this default combo?" : c.origin == Combo::UserOverride ? L"Revert to the shipped default?" : L"Delete this combo?";
                push_menu(*p, what, {{L"Yes", [p, cc]() { delete_combo(*p, cc); }}, {L"No", nullptr}}); return true;
            }
            default: return false;
        }
    };
    im.push(std::unique_ptr<Screen>(r));
}

/* ---------------- Editor ---------------- */
Editor::Editor(const Defaults &d, UserStore &s, const ValidationContext &c, CardIndex &ci) : p_(new Impl(d, s, c, ci)) {}
Editor::~Editor() {}
void Editor::open() { if (!p_->st.empty()) return; p_->msg.clear(); p_->has_draft = false; p_->dirty = false; push_combo_list(*p_); }
void Editor::close() { p_->st.clear(); p_->has_draft = false; p_->dirty = false; }
bool Editor::active() const { return !p_->st.empty(); }
void Editor::press(Btn b) { if (p_->st.empty()) return; p_->msg.clear(); p_->st.back()->press(*p_, b); if (p_->st.empty()) { p_->has_draft = false; p_->dirty = false; } }
void Editor::pump(int budget) { p_->cards.pump(budget); if (!p_->st.empty()) p_->st.back()->pump(*p_); }
std::string Editor::message() const { return p_->msg; }
bool Editor::dirty() const { return p_->has_draft && p_->dirty; }
Effective Editor::effective() const { return p_->eff(); }
std::vector<Line> Editor::render() const {
    std::vector<Line> out; if (p_->st.empty()) return out;
    std::wstring title, hint; std::vector<Line> body; p_->st.back()->render(*p_, title, body, hint);
    if (title.size() > kTextWidth) title.resize(kTextWidth);
    out.push_back(Line{title, false, true});
    for (auto &l : body) { if (l.text.size() > kTextWidth) l.text.resize(kTextWidth); out.push_back(l); }
    if (!p_->msg.empty()) out.push_back(Line{W(p_->msg.substr(0, kTextWidth)), false, false});
    while (!hint.empty() && out.size() < 14) {   /* the panel is ~40 columns wide: wrap on spaces */
        if (hint.size() <= kTextWidth) { out.push_back(Line{hint, false, false}); break; }
        size_t cut = hint.rfind(L' ', kTextWidth); if (cut == std::wstring::npos || cut == 0) cut = kTextWidth;
        out.push_back(Line{hint.substr(0, cut), false, false}); hint = hint.substr(cut < hint.size() && hint[cut] == L' ' ? cut + 1 : cut);
    }
    return out;
}

}  // namespace
