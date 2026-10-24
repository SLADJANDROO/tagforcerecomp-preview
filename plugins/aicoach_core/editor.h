/* AI Coach - in-game combo editor, LOGIC LAYER (Milestone 6a). Pure C++17, no platform headers, no game access.
 *
 * What this is: a screen state machine driven by pad buttons that renders to plain text lines. It edits combos through the same
 * Combo / validate_combo / UserStore code the rest of the mod uses, so it cannot produce a file the loader would reject.
 * What it is NOT (yet): nothing here reads the real pad, draws on screen or swallows input from the game. That needs the runtime
 * addition described in docs/AI_COACH_EDITOR.md (input capture) and has never been compiled against the game.
 *
 * Buttons (PSP layout, see PadReader):  Up/Down move   Cross = open/toggle/OK   Circle = back   Square = add   Triangle = delete/toggle
 *                                       Left/Right = adjust   L/R = move item / page   Start = duplicate/accept   Select = remove/revert
 * Not editable here (kept untouched when a combo is saved): step `branches`, `tags`, `preferred_turn`, `revision`, the id. Use JSON for those. */
#pragma once
#include "store.h"
#include <memory>

namespace aicoach {

enum class Btn { Up, Down, Left, Right, Cross, Circle, Triangle, Square, L, R, Start, Select };

/* Standard PSP button bits (SCE_CTRL_*). Same layout the runtime hands to guests; not verified against a live pad by me. */
enum PspMask : uint32_t { kSelect = 0x1, kStart = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80, kLTrigger = 0x100, kRTrigger = 0x200,
                          kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000 };

/* Turns the held-button mask of each frame into discrete presses: one on the frame a button goes down, then auto-repeat (directions, L, R only). */
class PadReader {
public:
    PadReader(int first_delay = 20, int repeat = 4) : delay_(first_delay), repeat_(repeat) {}
    std::vector<Btn> update(uint32_t held);
    void reset() { for (int &h : age_) h = -1; }
private:
    int delay_, repeat_, age_[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};

/* Card names, resolved a few per frame (the game's name function is slow to call thousands of times at once). */
class CardIndex {
public:
    /* `name` returns the card's name, or an empty string if there is no such card. It is only ever called from pump(). */
    CardIndex(std::function<std::wstring(uint32_t)> name, uint32_t max_id = kMaxCardId) : name_(std::move(name)), max_(max_id), names_(max_id + 1) {}
    void pump(int budget);                               /* resolves up to `budget` more ids */
    bool complete() const { return next_ > max_; }
    uint32_t resolved() const { return next_ > max_ ? max_ : (next_ ? next_ - 1 : 0); }
    uint32_t max_id() const { return max_; }
    std::wstring name(uint32_t id) const { return id <= max_ ? names_[id] : std::wstring(); }
    /* `q` empty: every resolved card. Digits: that id first. Otherwise a case-insensitive substring of the name. Ascending ids, at most `limit`. */
    std::vector<uint32_t> search(const std::wstring &q, size_t limit = 2000) const;
private:
    std::function<std::wstring(uint32_t)> name_; uint32_t max_, next_ = 1; std::vector<std::wstring> names_;
};

struct Line { std::wstring text; bool selected = false, header = false; };

class Editor {
public:
    /* All references must outlive the editor. `ctx.caps`, when set, limits the actions the editor offers to those the engine can execute. */
    Editor(const Defaults &defaults, UserStore &store, const ValidationContext &ctx, CardIndex &cards);
    ~Editor();
    void open();                      /* shows the combo list (does nothing if already open) */
    void close();                     /* leaves the editor; unsaved draft changes are discarded */
    bool active() const;
    void press(Btn b);
    void pump(int card_budget = 60);  /* call once per frame while active */
    std::vector<Line> render() const; /* header, up to 10 body lines, one hint line */
    std::string message() const;      /* last status / error text ("Saved", "Not saved: ...") */
    bool dirty() const;               /* a combo is open in the editor with unsaved changes */
    Effective effective() const;      /* defaults merged with the user's data, as the engine sees them */
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}  // namespace
