# AI Coach — what to test on your PC

Everything below was written and unit-tested on Linux **without the game**. The plugin has **never been compiled with MSYS2 or run in Tag Force**. This page is the checklist for
the first real session. Tick items in order; stop at the first failure and send me the log lines quoted in that item.

Stage reached: **observer**. The plugin reads the duel, ranks combos and shows what it *would* pick. It does not change how the CPU plays, writes nothing to the game
and every hook returns 0. Combos are not executed yet (that needs the opponent-control mod and the AI tick address, see `docs/AI_COACH.md` sections 1.2 and 2.3).

## 0. Get the code

```sh
git fetch https://github.com/SLADJANDROO/tagforcerecomp-preview aicoach-engine-core
git checkout -b aicoach-engine-core FETCH_HEAD     # or add your fork as a remote
```

## 1. Build (MSYS2 UCRT64)

1. `sh tools/test/aicoach/run_tests.sh` — needs only g++; expect four sections (data model, engine, game view, plugin glue), each ending in `0 failed checks`. (If it fails on Windows only, that is useful: send the output.)
2. `sh tools/build/build_plugins.sh AICoach` — expect `plugin: gen/mods/AICoach/plugin.dll`.
   - Watch for: errors about `std::filesystem` or `-lstdc++fs` (add `-lstdc++fs` only if the linker asks), name clashes between the `core_*.cpp` files (they are separate translation units, so none expected).
3. `sh tools/test/sdk_check.sh` — the SDK guard; AICoach must build with the same flags as every other mod.
4. Check `gen/mods/AICoach/` contains `plugin.dll`, `mod.txt`, `config.ini`, `data/defaults.json`.

## 2. Load

1. Launcher → Mods → enable **AI Coach (observer)**. Start the game.
2. Session log (`gen/logs/sessao_*.log`) must contain, in this order:
   - `[mods] AICoach: plugin carregado`
   - `[mod AICoach] loaded 4 profile(s), 0 combo(s) (0 enabled)`
   - `[mod AICoach] AI Coach observer loaded (it does not change how the CPU plays)`
   - If the toggle key is refused you will also see `tecla 115 reservada`; the panel can then only be switched in `config.ini`.
3. **Compare the game with and without the mod** in one Free Duel (same deck, same opponent): nothing should look or feel different. Turn it off again if anything does.

## 3. Identity — are the memory assumptions right?

Start a **Free Duel against a known duelist** (not CPU vs CPU yet).

- Log line `duel setup: side 0 = character A, deck B ...` and `side 1 = character C, deck D ...`.
- Check: one of the sides is the opponent you chose. Is its character id the one in `decomp/common/duelist_ids.inc` (e.g. Jaden = 2, Zane = 8)? Is its deck id one that belongs to that character (ladder: `1 + 6*(character-2) + level-1`)?
- **Unknown to settle:** is "record j = duel player j" true? Note which side number the opponent got and whether the opponent is the CPU side (next section).

## 4. State — does the view match the screen?

1. With the panel on (F4 toggles it; `config.ini` `panel=1`, `corner=0..3`), start a duel. During the **CPU's turn** a panel appears: title `AI Coach (observer) side N`, then `Character .. deck ..`, `No eligible combo: normal AI`, then `Hand (card ids)` with one row per card in the CPU's hand: `1234 Card name`.
2. Verify against the real game (the Cheats "See the opponent's cards" panel, F6, shows the same hand): the card ids and names must match, count must match.
3. Play until the CPU has monsters. Then press **F7** (or L3+R3) and enter any note: the log gets a `[mark #n] AI Coach ranking` block per side with `cpu=1/0`.
   Things to check and report: LP values, which side is `cpu=1`, who the "turn" belongs to (the panel shows only on the CPU's own turn; if it appears on *your* turn the turn-owner bit is inverted).
4. **Graveyard, extra deck, phase, turn number** answer *unknown* by design (addresses not found). Combos using them are never selected; the reason says "unavailable".

## 5. Your first combo (by hand, until the in-game editor exists)

1. During a CPU turn read three card ids from the panel's hand section.
2. Create `gen/mods/AICoach/user.json` (UTF-8):

```json
{
  "kind": "aicoach.user",
  "schema_version": 1,
  "combos": [
    {
      "id": "test.summon_it",
      "name": "Test: summon card",
      "description": "Observer test",
      "character": 2,
      "decks": [1],
      "priority": 300,
      "required_cards": [1001],
      "steps": [ { "id": "s1", "action": "normal_summon", "card": 1001 } ]
    }
  ]
}
```

   Use the real character id, deck id (from section 3) and a card id from the CPU's hand. For a quick test leave `character` at `0` and remove `decks` — then it applies to anyone.
   **Priority gate:** a profile's `signature_bias` hides combos with a low priority: minimum priority = `5 * (100 - signature_bias)`. The shipped profiles have bias 45-70, so use
   priority 300 or more for tests, or the log will say "priority below the profile's signature bias". (Provisional rule; tell me if it feels wrong in play.)
   (Field names are exactly those of `to_json(Combo)` in `plugins/aicoach_core/model.cpp`; unknown keys only produce warnings in the log.)
3. Within ~2 seconds the log says `user.json changed: reloading` and `loaded 4 profile(s), 1 combo(s) (1 enabled)`. No restart needed.
4. When the CPU holds that card on its turn: log `side N (...): would select 'test.summon_it' (score 300)` and the panel row `> Test: summon card`. When it does not: `... missing required card 1001`.
5. Break the file on purpose (delete a brace). Expected: log `user.json is MALFORMED`, the game keeps running, **the file is not touched**. Fix it → picked up again.

## 6. Things I could not know — please report

| Question | How to see |
|---|---|
| Does `std::filesystem` rename replace an existing file on your MSYS2 (needed for future saving)? | not exercised by the observer; will be by the editor milestone |
| Is F4 free (not a window hotkey)? | log line `tecla 115 reservada ...` means no |
| Does the setup hook (`0883f4f8`) fire for Free Duel **and** Story duels, and only there? | one `duel setup:` pair per duel in the log |
| Tag/2-vs-2 duels | how many `duel setup` sides are logged; the plugin assumes 2 |
| Cost | `[perf]` lines in the log should not change with the mod on |

## 7. To let me finish the real thing (needs your PC)

- The **opponent-control mod** (source or folder). It decides how combos get executed.
- A `TF_HOOK_TRACE` capture to find the AI's per-tick update and the pending-action layout: run a Free Duel with
  `TF_HOOK_TRACE=09e87760,09e6bad4,09e9ccb8,09e74f60,09e762e0,09e76a14` and send the log (frame number + arguments of each call). The EU address of the AI tick is the function that calls these repeatedly; its caller shows up when the same frame logs several of them.
- The addresses of the graveyard and extra deck arrays, if you can find them (Cheat Engine-style search while the card count changes).
