#ifndef MANGOS_BATTLEROYALE_H
#define MANGOS_BATTLEROYALE_H

#include "BattleRoyaleTemplate.h"
#include "BattleRoyalePlayer.h"
#include "BattleRoyaleZone.h"

#include "ObjectGuid.h"

#include <map>
#include <vector>

class Player;
class BattleGroundBR;

enum class BattleRoyaleStatus : uint8
{
    WAITING   = 0,
    COUNTDOWN = 1,
    PREPARING = 2,
    RUNNING   = 3,
    FINISHED  = 4,
    CANCELLED = 5,
};

struct BRRankEntry
{
    ObjectGuid guid;
    uint32     rank;
    uint32     survivalSec;
};

class BattleRoyale
{
public:
    explicit BattleRoyale(BattleRoyaleTemplate const* tmpl, BattleGroundBR* host);

    // Called by BattleRoyaleMgr
    void AddPlayer(Player* player);
    void Update(uint32 diff);
    void Cancel();

    // Called by BattleGroundBR when a player leaves the map
    void OnPlayerLeftMap(ObjectGuid guid);
    // Called by BattleGroundBR::OnPlayerKilled hook (see BattleGroundBR)
    void OnPlayerDied(ObjectGuid guid);

    // Queries
    BattleRoyaleStatus GetStatus()    const { return m_status; }
    uint32             GetAliveCount() const;
    bool               IsAlive(ObjectGuid guid) const;
    BattleRoyaleZone const& GetZone() const { return m_zone; }
    BattleGroundBR*    GetHost()      const { return m_host; }

    // GM helpers
    void ForceSetPhase(uint32 phase) { m_zone.ForcePhase(phase); }
    void ForceSetRadius(float r)     { m_zone.ForceRadius(r); }
    std::map<ObjectGuid, BattleRoyalePlayer> const& GetPlayers() const { return m_players; }

private:
    void StartPreparing();
    void StartRunning();
    void Finish();
    void Eliminate(ObjectGuid guid, bool notify = true);
    void ReturnPlayer(Player* player, BattleRoyalePlayer const& brPlayer);
    void BroadcastToAll(std::string const& msg);

    BattleRoyaleStatus  m_status;
    BattleRoyaleZone    m_zone;
    BattleRoyaleTemplate const* m_tmpl;
    BattleGroundBR*     m_host;

    std::map<ObjectGuid, BattleRoyalePlayer> m_players;
    std::vector<BRRankEntry>                 m_ranks;

    uint32  m_prepareTimer  = 30000; // 30 s protection period
    uint32  m_aliveCount    = 0;
    uint32  m_totalCount    = 0;
    uint32  m_finishTimer   = 0;     // delay before cleanup after FINISHED
    uint32  m_runningTime   = 0;     // seconds elapsed since RUNNING
};

#endif // MANGOS_BATTLEROYALE_H
