# Destiny Draw

Tag Force 2's Destiny Draw, added to Tag Force 1 (European version).

**Rule:** available while your LP is below 4000, the opponent controls a monster and none of your monsters has higher ATK than the
opponent's strongest. Once per duel. A small "Destiny Draw / Ready" panel shows when you can use it.
**Use:** press **F2** (or L+R+Select on the pad with `combo=1`). A random Destiny Card from your deck is put on top of your deck, so your
next normal draw is that card - like in Tag Force 2, where the card is placed on top and then drawn.

**Destiny Cards:** Tag Force 1 has no screen to mark them, so list them in `config.ini`, by name (`names=Card One; Card Two`) or by id
(`cards=1234, 5678`). The AI Coach panel and the Cheats mod show card ids.

**Safety:** the only thing changed is the order of your own deck, once per duel. After the first use the mod checks that the card you drew
is the Destiny Card; if not, it disables itself for the session and writes `MISMATCH` in the log (`logs/sessao_*.log`). `dry_run=1` only
shows the choice.
