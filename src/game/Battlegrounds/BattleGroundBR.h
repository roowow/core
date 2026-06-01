#ifndef MANGOS_BATTLEGROUNDBR_H
#define MANGOS_BATTLEGROUNDBR_H

#include "Battlegrounds/BattleGround.h"

class BattleRoyale;

// Lightweight BattleGround host for Battle Royale.
// Only manages the BattleGroundMap lifetime.
// All game logic lives in BattleRoyale.
class BattleGroundBR : public BattleGround
{
    friend class BattleGroundMgr;
public:
    BattleGroundBR();
    ~BattleGroundBR() override;

    void SetOwner(BattleRoyale* owner) { m_owner = owner; }
    BattleRoyale* GetOwner() const { return m_owner; }

    // Override all BG mechanics to no-ops — BR handles everything
    bool SetupBattleGround() override { return true; }
    void Reset() override {}
    void Update(uint32 diff) override;
    void EndBattleGround(Team /*winner*/) override {}
    bool HandleAreaTrigger(Player* /*player*/, uint32 /*trigger*/) override { return false; }

    // Do NOT call base AddPlayer — it builds faction BG teams
    void AddPlayer(Player* player) override;
    void RemovePlayerAtLeave(ObjectGuid guid, bool transport, bool sendPacket) override;
    void HandleKillPlayer(Player* victim, Player* killer) override;

private:
    BattleRoyale* m_owner = nullptr;
};

#endif // MANGOS_BATTLEGROUNDBR_H
