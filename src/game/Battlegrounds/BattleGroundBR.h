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

    // --- Open-world map hosting (see BattleRoyale.md「分层设计草案」) ---
    // For OPEN_WORLD templates there is no dedicated BattleGroundMap: instead we attach to
    // whatever persistent Map the server already has loaded for that mapId (e.g. Kalimdor).
    // GetBgMap()/m_map (base class) stay untouched/null in this mode — GetHostMap() is the
    // one accessor BattleRoyale.cpp should use instead of GetBgMap() everywhere.
    Map* GetHostMap() { return m_openWorldMap ? m_openWorldMap : GetBgMap(); }
    void SetOpenWorldMap(Map* map, uint32 instanceId) { m_openWorldMap = map; m_openWorldInstanceId = instanceId; }
    bool IsOpenWorldHosted() const { return m_openWorldMap != nullptr; }

    // GetInstanceID() (base class) derives from m_map, which stays null in open-world mode —
    // override so registration with BattleGroundMgr uses a real synthetic id instead of 0.
    uint32 GetInstanceID() override { return m_openWorldMap ? m_openWorldInstanceId : BattleGround::GetInstanceID(); }

private:
    BattleRoyale* m_owner = nullptr;
    Map* m_openWorldMap = nullptr;
    uint32 m_openWorldInstanceId = 0;
};

#endif // MANGOS_BATTLEGROUNDBR_H
