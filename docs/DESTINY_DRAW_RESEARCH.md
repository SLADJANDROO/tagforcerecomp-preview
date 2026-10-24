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

## Next evidence wanted
Deck Edit states with one card marked as Destiny vs unmarked; the same duel menu in Tag Force 1 (to confirm the TF1 layout is what the AI Coach already assumes).
