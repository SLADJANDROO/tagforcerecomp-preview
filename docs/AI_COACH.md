# AI Coach — architecture audit, plan and status

AI Coach turns the game's generic CPU duelists into characters that follow data-defined strategies (combos, profiles) which the player
can edit without recompiling. This document is the **Milestone 1 audit** (what exists, what was verified, what is missing), the plan,
and the live status table. Every claim is tagged:

- **[verified]** read in this checkout's code or docs, or executed here
- **[inferred]** follows from verified facts but was not observed
- **[unknown]** not determinable from this checkout

Audited checkout: `tagforcerecomp-preview`, single commit `960b479`, European game `ULES-00600`, plugin API version 6.
Reference checkout: `tag-force-1-ai-research` (see 1.4).

---

## 1. Findings

### 1.1 How the game's AI is organised

- **[verified]** Duel state is at fixed PSP addresses. Player `p` is at `0x09f79cec + p*0xfac`: LP at `+0`, hand at `+0x120 + i*4`
  (card id = `u16 & 0x1fff`), hand count at `0x09f79cf8 + p*0xfac`, deck count at `0x09f79cfc + p*0xfac`. Field slots are 0x14 bytes at
  `0x09f79d1c + p*0xfac + i*0x14` (0..4 monsters, 5..9 spell/trap, 10 field spell): `+0` id, `+6` position, `+0x10` flags
  (`0x80` face-up, `0x08` defense). Source: `plugins/src/BetterAI/ia_aprimorada.cpp`, `plugins/src/CPUvsCPU/cpu_vs_cpu.cpp`.
- **[verified]** Turn owner: `u32(0x09f7d460) & 1` (read by CPUvsCPU's monitor). RNG seed `0x09f77880`, first player `0x09f77890`.
- **[verified]** Control type per player: `u32(0x09f77888 + 4*p)`; `1` = CPU, `0` = local human. Written by the duel manager through
  `libduel_eng_5A41E0A6(player, type)` at `0x09f626dc`. Every decision point is `if (control[p]==1) CPU decides else open the UI` (`docs/AI.md`).
- **[verified]** Hooked AI functions (EU addresses, all already used by BetterAI / CPUvsCPU, see `docs/HOOKS.md`):
  `09e74f60` attack decision, `09e762e0` hand decision, `09e76a14` which monster to summon, `09e75258` attack planner (arg `-1` = simulation),
  `09e6cff0` / `09e6c4a0` position decision / set position, `09e6bad4(player, hand_idx, mode)` play a monster, `09e87760` execute the chosen
  spell action from the rule tables (`09f6a478`, `09f6b470`), `09e8133c` / `09e82bd4` simulate / Dark Hole rule, `09e75cc0` board score,
  `09e9ccb8(player, zone, idx, type)` card selection (spell activation, targets), `09f56da8` monster stats, `09f5060c` "can this monster attack".
- **[verified]** Hook semantics: return `0` = original keeps running; `1` = handled, result in `r2`. `call_original` wraps. Several mods may hook one
  address; the first to return 1 ends the chain. Hooks run on the game thread inside the frame: nothing may block.
- **[verified]** Per-frame callback: hook `08833fc4`.
- **[verified]** Engine data lookups available to a plugin by calling game functions: card name `0884b46c`, description `0884b4b8`, ATK/DEF/level
  `0884b7ec/0884b830/0884b77c`, is-monster `0884c0d8`, `CARD_Prop` entry `0884b4f8`.
- **[verified, partly reconstructed]** Spells/traps are played from a rule table `{card id, predicate}`; cards not in the table are never played by the
  original AI. The AI is a staged checklist (early priorities, summon/effect passes, position, battle preparation, late priorities) with inner counters
  that can restart after an action, so it is **re-entered repeatedly during a turn**, not decided once per turn (research repo `docs/FINDINGS.md`, USA build).
- **[unknown]** The EU address of the AI's top-level per-tick update. The USA research build calls it at `0x09EA4998`; EU addresses differ. Finding it needs
  `TF_HOOK_TRACE` on a running EU build (see risk R2).
- **[unknown]** A duel **phase** variable. No plugin reads one; `model.h` phase names are provisional until it is located.
- **[unknown]** The in-memory format of a "pending action record" the AI submits to the engine (research repo mentions action records, event and
  chain descriptors in the USA build). Needed to submit actions without going through the UI or the AI's own functions.
- **[unknown]** Chain / response prompt state. BetterAI does not touch chains.

Reconstruction status: the engine is mostly **generated C** (mechanical translation). `decomp/libduel_eng` has 25 readable swaps (card-slot flags,
effect-list queries, legality helpers); the AI itself is not decompiled in this repo. The research repo has readable Python models of hand choice,
position, phase plan, battle, targets and responses with 49,929 differential cases, but for the **USA v1.03 build** and with no blanket licence
(`RIGHTS_AND_PROVENANCE.md`): treat it as a behavioural specification, **do not copy code or addresses** into this repo.

### 1.2 The opponent-turn control mod — NOT FOUND

- **[verified]** There is no such mod in this checkout. Searched `plugins/`, `runtime/`, `decomp/`, `tools/`, `docs/` for the control array, opponent
  control, input injection. The only related code is **CPUvsCPU**, which does the *opposite*: it rewrites `control[0]` from human to CPU so the AI plays your side.
  The only input-synthesis facilities are test-only (`TF_CTL` file protocol, `AUTO_PRESS`, `TF_MOUSE_AT`), not exposed to plugins.
- **[verified]** Your Desktop on the linked computer has `TagForceRecomp` and `Yu-Gi-Oh-Tag-Force-main`. Access to them was requested and **denied**, so they
  were not read. The latest control mod is probably there.
- **[inferred, unconfirmed]** It probably works by setting `control[1]` to human so the engine opens the UI for the opponent's decision points
  (the `if (control==1)` structure). If so, it is an input-driven path: a combo executor would have to press buttons through that UI.

Consequence: Milestones 1–4 do not depend on it (they are pure data, pure logic and read-only state). **Milestone 5 (first end-to-end combo) does.** The two possible
execution backends are described in 2.3; I will not choose between them until I have read the control mod.

### 1.3 UI, input and persistence

- **[verified]** Drawing available to a plugin (API v6): `panel` (translucent text/table panel in a screen corner, ~40 chars wide, colour-coded card
  rows, never blocks), `message_box`, `sprite` (RGBA image at 480x272 PSP coordinates, GPU renderers only, 1–3 frames latency, "not meant for per-frame
  animation"). No buttons, lists, cursors or text-entry widgets. No card-image API.
- **[verified]** Input: `buttons()` returns the pad state the game itself sees (`g_latched`). It is **read-only**: a plugin cannot consume a press, so an overlay menu
  would also drive the game underneath. `add_toggle` can bind a few free keys. The window menu bar was removed (`add_slider` is a no-op).
- **[verified]** The game's own menus live in `libtitle`, which this repo has hand-decompiled (192 swaps incl. the main menu list `MainMenuItem`).
  A main-menu entry is therefore technically reachable but means editing decomp swaps, i.e. the core repo, not a plugin.
- **[verified]** Persistence: plugins are Windows DLLs and already use `_wfopen` in their own folder (`api->mod_dir`): CPUvsCPU `config.ini`, BetterDecks reads
  `decks/RCPnnn.txt`. `plugins/aicoach_core/store.cpp` already adds atomic writes (temp file + rename, `.bak`), corrupt-file quarantine and newer-schema refusal.
  Reloading at run time only needs a re-read. **[unknown]** whether a duel is the right moment to write files (not an issue for a menu outside duels).
- **[verified]** No JSON parser exists in the runtime or plugins (only a log writer); `aicoach_core/json.*` is a small strict parser with no dependencies.
- **[verified]** Card database: the game owns it. Names, descriptions, ATK/DEF/level and `CARD_Prop` are reachable through the game functions above, so a card
  browser needs no external database. Card **images** (`card/cardm_*.cip`, 64x64 DXT1) are disc files read by sector; whether a plugin can obtain them is **[unknown]**.
- **[verified]** Character and deck identity: the duel setup structure has 0x18c bytes per duelist at base `0x091c3d6c`; `+0x120` = duelist id, `+0x124` = deck
  index (`RCPnnn`; values `0x15f..0x162` are the player's own decks). CPUvsCPU already hooks `libehsys_F11F0D9A` (`0883f4f8`) and receives that base in `r4`.
  Deck → owner mapping is in `decomp/common/duelists.h`. The same character has up to six decks, so **deck id must be part of every lookup** (it is, in the data model).
- **[verified]** Environment: the plugin is built with MSYS2 UCRT64 g++ (`tools/build/build_plugins.sh`); this cloud box has Linux g++ 13 only, so **plugin DLLs cannot be built or
  run here**. Anything not touching `tfmod.h` can be (and is) unit-tested here.

### 1.4 Reference repositories

| | This checkout | Research repo |
|---|---|---|
| Game build | EU `ULES-00600` | USA `ULUS10136` v1.03 |
| Engine base | EU addresses `0x09e....` | `0x09E65C00` |
| What it is | working native port + plugin API | Python models + generated C replay harness, needs private game state |
| Licence | MIT | none assigned |
| Use for AI Coach | the host: hooks, state, UI, persistence | behavioural spec only; verify every address on EU |

---

## 2. Architecture

```
 data (JSON, versioned)          pure logic (no game, no Windows)           game adapter (plugin, EU only)
 ┌────────────────────┐   ┌─────────────────────────────────────┐   ┌────────────────────────────────┐
 │ defaults.json      │──▶│ model   validate / to_json          │   │ DuelView  <- reads PSP memory  │
 │ user.json          │   │ store   merge, migrate, atomic save │   │ Executor  -> backend A or B    │
 └────────────────────┘   │ engine  evaluate / rank / run       │◀─▶│ hooks: AI tick, setup, frame   │
                          └─────────────────────────────────────┘   │ UI: panel / overlay            │
                                                                    └────────────────────────────────┘
```

### 2.1 Layers

1. **model/store** (`plugins/aicoach_core/model.*`, `store.*`, `json.*`): done, tested. Closed vocabulary of actions and conditions, so a combo can never carry code.
2. **engine** (`plugins/aicoach_core/engine.*`): condition evaluation with a three-valued result, deterministic combo ranking, the step runner state machine,
   an attempt ledger. It talks to the game only through two abstract interfaces, `DuelView` (read state) and `ActionExecutor` (submit one action, poll its result).
3. **game adapter** (new, plugin side): implements those two interfaces over the real game.

### 2.2 Execution model (matches Phase 2 of the spec)

State-driven, one bounded step per tick, never blocking:
`preconditions → legality → submit → (poll until resolved, with timeout) → verify expected state → branch / next / failure policy`.
Unknown state is explicit (`Tri::Unknown`): an unknown **precondition** is not satisfied (fail safe), an unknown **expectation** is accepted but traced as unverified.

### 2.3 Execution backends (decision deferred to Milestone 5)

- **A — AI-side injection.** Keep `control[p]==1` (CPU) and, inside the AI's own decision hooks (the points BetterAI already hooks), return the combo's next intended
  choice. Pros: proven hook points, the game computes legality, no UI timing. Cons: only the decisions the AI asks for can be steered; cards outside its rule tables are
  never offered; the AI's checklist may re-enter in an order that does not match a combo.
- **B — UI-side driving.** Hand the side to the UI (what the control mod seems to do) and press buttons. Pros: reaches every legal action a human can. Cons: needs reliable
  menu-state detection and frame-accurate input; plugin API has no input injection today (would need an API v7 addition).
- A hybrid (A for summon/position/attack, B for the rest) is possible. **This is decided after reading the control mod and tracing the EU AI tick.**

### 2.4 In-game editor feasibility

Feasible, but needs two small **runtime** additions, which is why it is late in the plan:
1. an input-capture call (API v7) so an overlay menu can consume presses instead of the game receiving them; and
2. a way to draw lists and a cursor: either build them from `panel` rows / a generated `sprite`, or add a text-drawing call.
Card thumbnails are the one open question (1.3). The first editor version can use the existing colour-coded card rows (frame colour, attribute, level, ATK/DEF)
with no images. A text-only or external editor will **not** be substituted unless these additions prove impossible.

---

## 3. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Control mod not available to me; execution backend unknown | Engine is behind interfaces; ask for the mod; Milestones 2–4 proceed |
| R2 | EU address of the AI tick / pending-action format unknown | `TF_HOOK_TRACE` session on EU build; research repo for structure only |
| R3 | No duel phase variable | Locate with trace; until then `phase_is` evaluates `Unknown` (never true) |
| R4 | Overlay input leaks to the game | API v7 `capture_input` before any editor work |
| R5 | Cannot build/run DLLs in this cloud workspace | Pure logic unit-tested here; adapter compiled and tested on the user's MSYS2 machine; status table says which |
| R6 | Chain/response prompts interfere with a running combo | Runner treats prompts as `NeedsInput`; fails safe to the normal AI; chain handling is its own milestone |
| R7 | Plugin version coupling (EU-only addresses) | `tfmod_game_ids` stays EU; per-version address table is a later task |
| R8 | Hook ordering with BetterAI on the same addresses | Coach hooks return 0 unless a combo step is active; document load order |

## 4. Milestones and acceptance tests

| M | Deliverable | Acceptance |
|---|---|---|
| 1 | This audit | reviewed |
| 2 | Data model, validation, store, tests | 23 unit tests pass under ASan/UBSan |
| 3a | **Engine core** (this change): evaluation, ranking, runner, ledger, with fakes | engine unit tests pass; behaviour list in `test_engine.cpp` |
| 3b | Game adapter `DuelView` (read-only) — code and unit tests done (section 9); plugin wiring + panel diagnostics still to do | on the user's machine: panel shows hand/field/LP matching the game during a duel |
| 4 | Profiles + deck assignment + shared combos wired to the real character/deck ids | log line "char X deck Y -> N combos" at duel start for 3 known duelists |
| 5 | First end-to-end combo through the real game (needs the control mod / chosen backend) | duel log shows steps, state verification, one forced failure recovered; duel finishes |
| 6 | Menu: browse characters/decks/combos, enable/disable, priority | in-game navigation with pad only; input does not leak |
| 7 | Combo editor | create/edit/save/reload a combo with no JSON typing |
| 8 | Character strategies | signature combos per verified deck |
| 9 | Diagnostics, import/export, docs | trace view explains a skipped combo |
| 10 | Compatibility and release | AI Coach off ⇒ identical to the original; BetterAI/CPUvsCPU coexist |

## 5. Files

Added (this change): `plugins/aicoach_core/engine.h`, `engine.cpp`, `tools/test/aicoach/test_engine.cpp`, this document; modified: `tools/test/aicoach/run_tests.sh`.
Planned: `plugins/src/AICoach/` (adapter, hooks, UI; `mod.txt`, `config.ini`), `plugins/aicoach_core/data/defaults.json` (real combos), possibly `plugins/sdk/tfmod.h` v7 (input capture).
Nothing in `runtime/`, `decomp/`, existing mods or the SDK has been changed.

## 6. Status (per feature)

Legend: **I** implemented · **C** compiled · **U** unit-tested · **G** integration-tested against the game · **V** verified in the running game.

| Feature | I | C | U | G | V |
|---|---|---|---|---|---|
| JSON parser, model, validation, defaults/user store, migration, import/export | yes | yes (Linux g++) | yes (23 tests) | no | no |
| Condition evaluation, combo ranking, runner, attempt ledger (section 7) | yes | yes (Linux g++, `-Wall -Wextra -Wpedantic` clean) | yes (18 tests, ASan/UBSan; 3 deliberate bugs injected and caught) | no | no |
| Read-only `GameDuelView` over the documented memory layout (`game_view.*`, section 9) | yes | yes (Linux g++, clean) | yes (7 tests, against a fake memory image) | no | no |
| Plugin wiring (`tfmod_init`, hooks, `api->mem` bridge), action executor, menu, editor | no | no | no | no | no |

Nothing is verified in the running game. This workspace cannot run the game (no Windows, no ISO).

## 7. Engine core (Milestone 3a)

Files: `plugins/aicoach_core/engine.h|cpp`; tests `tools/test/aicoach/test_engine.cpp`; run everything with `sh tools/test/aicoach/run_tests.sh`.
The tests use a fake duel and a fake executor, so they prove the logic only. Nothing here touches the game.

**Interfaces the game adapter must implement** (`DuelView`, `ActionExecutor`): every query can answer *unknown*; an executor submits exactly one action and reports
`Pending / Succeeded / Failed / Cancelled / NeedsInput` over later ticks.

**Rules implemented and tested**

| Area | Behaviour |
|---|---|
| Unknown state | Unknown precondition or start condition = not satisfied (fail safe). Unknown expectation = accepted, traced as `unverified`. Unknown legality = submit anyway, the game decides. `NOT unknown` stays unknown. |
| Ranking | Disabled, other character, other deck, blocked this turn, below the profile's signature-bias gate, wrong turn/phase, missing required card, failed start condition, goal already achieved are all ineligible **with a stated reason** and the checks that produced it. Order: score desc, then id asc (deterministic). Score = priority + 5 per available optional card. |
| Signature bias | `min priority = 5 * (100 - signature_bias)`: bias 100 allows every combo, 50 needs priority >= 250, 0 needs >= 500. **Provisional heuristic**; tune it in game. |
| Sequencing | One action in flight. A step is submitted only after the previous one reported success **and a further tick passed**. Never two submissions in one tick. |
| Failure policies | `abort`, `skip` (later steps can test `step_failed`), `goto` (forward only), `retry_once` (exactly one more attempt), `fallback` (hand back to the normal AI). A failed combo is never re-selected in the same turn. |
| Unexpected state change | The action DID happen, so it is a failure that is never retried. |
| Stalls | Per-step and per-combo tick budgets; on expiry the action is cancelled and the outcome is `timed_out`. |
| Prompts | `NeedsInput` means a selection prompt is open: the next step must be `select_card`, otherwise the combo stops. |
| Interruption | `abort` (blocked), `reevaluate` (not blocked, asks for re-selection, at most `max_starts_per_turn` = 2 starts), `resume` (cancels, re-checks and re-sends the current step). |
| Invalid input | `begin()` re-validates the combo and refuses an invalid one; it also refuses to start while another is running. |
| Diagnostics | Bounded trace (`begin/submit/ok/fail/retry/skip/goto/branch/prompt/unverified/interrupt/end`) with a stable text form; two runs on identical inputs produce identical traces. |

**Deliberately not done yet:** a `phase` source (until located, `phase_is` is Unknown and phase-gated combos are never chosen), chain/response handling,
reading real game memory, target-selection policies (the executor receives `target_policy` and must honour it), and any in-game UI.

## 8. Next step

1. Obtain the opponent-control mod (see 1.2) and a short `TF_HOOK_TRACE` session on the EU build to find the AI tick and the pending-action layout. Decide backend A/B.
2. Milestone 3b: read-only `DuelView` over the memory map in 1.1, shown in a `panel`. Compile with MSYS2 on your machine; a diagnostic log line at duel start
   ("character X, deck Y, N combos") is the first thing to verify in the running game.

## 9. Game view (Milestone 3b, part 1)

`plugins/aicoach_core/game_view.h|cpp`: `GameDuelView` implements `DuelView` over a `GuestMemory` interface (the plugin will implement it with `api->mem()`).
It reads hand, field (monsters, spell/trap, field spell), positions from the face-up/defense flags, free zones, LP, opponent monster count and turn owner, using the
offsets listed at the top of `game_view.h`. Everything the game is not known to expose answers Unknown: graveyard, extra deck, phase, turn number, action legality.
If the counts or LP look implausible (not in a duel, stale memory) every answer is Unknown / -1.

Caveats: the offsets come from reading the existing mods' code, not from running the game; the tests build a fake memory image from the same list, so they cannot
catch a wrong offset. The first in-game check is the panel/log of hand and field against what the screen shows. The graveyard and extra deck need their addresses found.
