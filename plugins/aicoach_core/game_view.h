/* AI Coach - DuelView over the game's duel memory (Milestone 3b, read-only). No platform headers: guest memory is reached through GuestMemory,
 * which the plugin implements with api->mem() and the tests implement with a byte array.
 *
 * Layout used (EU ULES-00600), taken from plugins/src/BetterAI/ia_aprimorada.cpp, plugins/src/CPUvsCPU and runtime/mods/cheats.cpp. It has NOT been
 * checked by this code against a running game: the tests prove the code matches this documented layout, nothing more.
 *   player p base = 0x09f79cec + p*0xfac : +0 LP (s32), +0x0c hand count, +0x10 deck count, +0x120 + i*4 hand card (u16 & 0x1fff)
 *   slot = 0x09f79d1c + p*0xfac + i*0x14 : i 0..4 monsters, 5..9 spell/trap, 10 field spell; +0 id (u16 & 0x1fff), +0x10 flags (u32: 0x80 face-up, 0x08 defense)
 *   turn owner = u32(0x09f7d460) & 1
 * Not exposed by the game as far as known, so they answer Unknown: graveyard, extra deck, duel phase, turn number, action legality. */
#pragma once
#include "engine.h"

namespace aicoach {

struct GuestMemory {
    virtual ~GuestMemory() {}
    virtual uint32_t u32(uint32_t addr) const = 0;
    virtual uint16_t u16(uint32_t addr) const = 0;
};

class GameDuelView : public DuelView {
public:
    GameDuelView(const GuestMemory &m, int self) : m_(m), self_(self & 1) {}
    /* False when the memory does not look like a duel in progress (implausible counts or LP): every question then answers Unknown. */
    bool plausible() const;
    bool my_turn() const;                                 /* the acting side owns the current turn */

    Tri has_card(Where w, uint32_t card) const override;
    Tri monster_position(uint32_t card, const std::string &position) const override;
    int free_zones(const std::string &zone) const override;
    int life_points(bool self) const override;
    int opponent_monster_count() const override;
    std::string phase() const override { return std::string(); }
    int turn() const override { return 0; }
    Tri action_legal(const Step &) const override { return Tri::Unknown; }   /* only the game (through the executor) can judge */
private:
    uint32_t base(int p) const;
    uint32_t slot(int p, int i) const;
    uint32_t slot_id(int p, int i) const;
    const GuestMemory &m_; int self_;
};

}  // namespace
