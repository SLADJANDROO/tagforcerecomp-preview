# Destiny Draw (Tag Force 2 -> Tag Force 1): research notes

Source: PPSSPP save states of **Tag Force 2 USA v1.02 (ULUS10302)**, unpacked offline (zstd payload at file offset 176, 32 MB RAM block at offset 0x48 of the
decompressed data, guest address = 0x08000000 + offset). Nothing here was observed in Tag Force 1 yet. Addresses below are **TF2 USA only**.

States compared (same duel): turn 8 draw-phase menu showing "Draw / Destiny Draw" (A); turn 8 main phase right after using Destiny Draw (B);
turn 10 draw phase, conditions still met, menu shows only "Draw" (C); turn 10 main phase after a normal draw (D).

## Confirmed by matching the screenshots
| What | Where (TF2 USA) | Evidence |
|---|---|---|
| Duel player block, stride 0xfac (same stride as TF1) | player 0 = 0x09f47e60, player 1 = +0xfac | LP 2000/8000 and 5300/8000 match the screen |
| Hand count | block+0x0c | 6 / 7 / 5 / 6 cards, matches screenshots |
| Deck count | block+0x10 | 32 / 31 / 31 / 30, matches the on-screen counter |
| Hand entries (u32, low 16 bits = card id, `& 0x1fff` like TF1) | block+0x120 + 4*i | new card appears at the end after drawing |
| Deck order array (u32 entries, same format), top card = index 0 | block+0x3a0 (0x09f48200) | a normal draw shifts the array left by one and decrements the count; the old last entry stays as a stale duplicate |

## Destiny Draw effect as it actually happened (A -> B)
The chosen card (id 0x171a) was removed from the **middle** of the deck array (index 20), the entries after it shifted left, deck count 32 -> 31,
and the same 32-bit entry was appended to the hand (hand count 6 -> 7). So the "put on top, then draw" rule ends up as: remove entry k, shift, count--, append to hand.

## Candidate "used" flag (one sample, not proven)
0x09f4e450 (u32): 0 before use, 1 after, stays 1 on the next turn while the option is gone. 0x09f4e45c: 0 -> 7 at the same time (meaning unknown).
0x09f4e44c: 1 while the menu showed the option and after use, 0 otherwise (meaning unknown). Needs a second duel / the opponent's side to confirm.

## Still unknown
- How a card is marked as a Destiny Card (not found in the duel block; probably deck data from deck edit).
- The trigger check, the menu code that adds the "Destiny Draw" command, and the random pick (all code, not data).
- Everything on the Tag Force 1 side.

## Where the Destiny mark is stored: NOT found (and not needed for the port)
Deck Edit states with a card marked / cleared differ only in screen-drawing data; the deck structure (name at 0x08ab2f40, count 0x28, 40 ids) is identical
in both. The PSP save files (DATA0) are encrypted, so diffing them is useless. The TF1 port does not need TF2's storage format anyway: TF1 has no marking
UI, so marks will live in the mod's own file (keyed by deck), and the effect only needs the deck array and the hand (above).

## Tag Force 1 probe (read-only)
`AICoach` F7 mark now also logs `[probe]` lines: both sides' LP, hand/deck counts, the raw hand words (+0x120) and the first deck-array words (+0x3a0)
plus the words around the end of the deck. If TF1 uses the TF2 layout, the hand ids should match what the F6 panel shows and the deck array should be
a list of u32 entries (id in the low 13 bits) whose first entry is the top card.

## Tag Force 1 side: what the installed recomp (C:\Users\Admin\Desktop\TagForceRecomp) confirms
* It is the **European** build (`ULES-00600`, `gen/`, `docs/VERSIONS.md`), the same one the AI Coach targets. The duel modules are at the same
  bases as in the repo (`libduel_eng` at `0x09E6B800`).
* `decomp/libduel_eng/duel_data.h` and its README confirm the field-slot layout (`0x09F79D1C`, 0x14 bytes per zone) and the player stride
  (`0xFAC`, base `0x09F79CEC`). The hand (`+0x120`) and the counts (`+0x0C`, `+0x10`) are already used by the repo's own observer code and docs.
* No function or table named "Destiny" exists in the generated code, which fits the wiki: Destiny Draw is not in the original GX Tag Force.
* **Not confirmed yet:** the duel's deck order array (`+0x3A0`) and whether a draw takes entry 0. This is what the F7 `[probe]` lines test.
* The generated code has only addresses, no names, so the draw routine can't be found there by search. Finding it needs a trace or the probe.

## Next evidence wanted
Deck Edit states with one card marked as Destiny vs unmarked; the same duel menu in Tag Force 1 (to confirm the TF1 layout is what the AI Coach already assumes).
