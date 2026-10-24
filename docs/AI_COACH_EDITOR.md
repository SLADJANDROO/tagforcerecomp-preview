# AI Coach — in-game combo editor

Status, stated plainly:

| Part | Implemented | Compiled (Linux g++) | Unit-tested | Compiled with MSYS2 | Works in game |
|---|---|---|---|---|---|
| Editor logic (`plugins/aicoach_core/editor.*`): screens, card picker, step / condition editing, validation, saving | yes | yes | yes (`test_editor.cpp`) | **no** | **no** |
| Pad reader (held mask → presses with auto-repeat) | yes | yes | yes | no | no |
| Card name index (resolved a few ids per frame) | yes | yes | yes | no | no |
| Reading the real pad **without the game also reacting** | **no** | – | – | – | – |
| Drawing the editor on screen | **no** (only the `Line` list exists) | – | – | – | – |
| Opening the editor from the game | **no** | – | – | – | – |

So the editor *logic* is finished and tested, but there is **no way to see or use it in the game yet**. Two things are missing, and both need the runtime
(not just this plugin):

1. **Input capture.** `api->buttons()` is read-only: the game still receives every press, so pressing Cross in the editor would also confirm things in the duel.
   The runtime needs a small, default-off addition (proposed API v7): `capture_input(bool)`; while on, `display_buttons()` seen by the guest is 0 and the plugin
   gets the real mask from `buttons()`. The place to do it is `runtime/ui/display.cpp` (`g_latched`, used by `display_buttons()` and by the guest's
   `sceCtrl` read in `runtime/hle/hle_misc.cpp`). I have not written it: I cannot build the runtime here, and a mistake would break the input of *every* mod.
2. **Drawing.** `api->panel()` shows up to ~40-character text lines in one corner, which is enough for these screens (`Editor::render()` already wraps and
   truncates to that), but nobody has tried it with ~14 lines changing on every press.

Until then, `user.json` can be edited by hand (see `AI_COACH_TESTING.md`, section 5); the editor writes exactly the same file.

## What the editor does

Buttons (PSP layout): Up/Down move · Cross open/choose/toggle · Circle back · Square add · Triangle delete or toggle · Left/Right adjust · L/R move or page ·
Start duplicate/accept · Select delete/revert.

| Screen | What you can do |
|---|---|
| Combo list | See every effective combo (`[x]` on, `*edited` = your replacement of a shipped combo, `*mine` = yours). Cross opens it, Left/Right priority ±10, Triangle on/off, Square new, Start duplicate, Select hide (shipped) / revert (edited) / delete (yours), "Restore hidden defaults". |
| Edit combo | Name (on-screen text entry), enabled, priority, character, decks, required / optional cards, start conditions, steps, success conditions, interruption policy, preferred phase, **Check** (lists every validation finding), **Save**, Back (asks before dropping unsaved changes). |
| Steps | Add (only actions the engine can execute, from `Capabilities`), edit, delete, move up/down. Per step: action, card, position, target policy, conditions before, expected state after, what to do on failure, jump target. |
| Conditions | All thirteen condition types with defaults that validate; only the fields relevant to the type are shown. |
| Card picker | Search by name (case-insensitive) or id, page with L/R, or press Triangle and type a card id. Names are resolved gradually (60 ids per frame); until they are loaded, use the id entry. |

Safety properties (all covered by tests):

- Saving goes through `validate_combo`, with the same `Capabilities` the engine reports. A combo with errors is **not** saved; Check says why.
- Shipped defaults are never modified. Editing a shipped combo stores a replacement in `user.json`; saving it back to exactly the shipped content removes the replacement.
- A failed write leaves the previous data in memory and on disk and says "Not saved: …". A `user.json` from a newer schema is never overwritten.
- Nothing is written until you press Save (priority / on-off shortcuts in the list write a small patch immediately).
- Leaving with unsaved changes asks first.

## Known limits (honest list)

- Not editable on screen, but **preserved** when you save: step `branches`, `tags`, `preferred_turn`, `revision`, the combo id. Edit those in JSON.
- No undo; discard the draft instead (Back → "Leave without saving").
- Text entry is ASCII only (A–Z, a–z, 0–9 and `-_.:'!?&/() `). Card names in the picker may be any Unicode the game's name function returns.
- The card picker needs a working name function. In the plugin it will be the existing `card_name()` wrapper (game function `0x0884b46c`), whose behaviour for ids that do
  not exist is **unknown**: the index treats an empty string as "no such card". If it returns placeholder text for every id, search by name will show everything; use ids.
- Character numbers are shown with the profile name only if a profile for that character exists in the effective data (4 shipped).
- Priority gate reminder: new combos start at priority 300 because the provisional `signature_bias` gate hides lower ones for the shipped profiles.

## What I need from you to finish it

1. Build and run `sh tools/test/aicoach/run_tests.sh` on MSYS2 (adds an "editor" section). A failure there is useful on its own: the editor uses `std::filesystem`
   and `std::towlower`.
2. Decide whether I may add the input-capture API (v7) to the runtime. I would write it small, default off, and keep `buttons()` unchanged, but it cannot be tested here.
3. After that: a plugin hotkey (F8 proposed) opens the editor, `PadReader` feeds it from `api->buttons()`, `render()` goes to `api->panel()`, the user store is shared with
   the observer (it already hot-reloads `user.json`).
