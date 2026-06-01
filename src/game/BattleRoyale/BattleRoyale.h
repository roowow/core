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
class Map;

enum class BattleRoyaleStatus : uint8
{
    WAITING   = 0,
    COUNTDOWN = 1,
    DEPLOYING = 2,
    PREPARING = 3,
    RUNNING   = 4,
    FINISHED  = 5,
    CANCELLED = 6,
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
    void AddPlayer(Player* player, BRSpawnPoint const& landingPoint, uint32 deploymentPathId, bool isBot = false);
    void SetPendingBotCount(uint32 count) { m_pendingBotCount = count; }
    void Update(uint32 diff);
    void Cancel();

    // Called by BattleGroundBR when a player leaves the map
    void OnPlayerLeftMap(ObjectGuid guid);
    // Called by BattleGroundBR::OnPlayerKilled hook (see BattleGroundBR)
    void OnPlayerDied(ObjectGuid guid);

    // Queries
    BattleRoyaleStatus GetStatus()         const { return m_status; }
    uint32             GetAliveCount()     const;
    uint32             GetTotalCount()     const { return m_totalCount; }
    uint32             GetPendingBotCount() const { return m_pendingBotCount; }
    uint32             GetRunningTimeSecs() const { return m_runningTime / 1000; }
    bool               IsAlive(ObjectGuid guid) const;
    BattleRoyaleZone const& GetZone() const { return m_zone; }
    BattleGroundBR*    GetHost()      const { return m_host; }

    // GM helpers
    void ForceSetPhase(uint32 phase);
    void ForceSetRadius(float r);
    std::map<ObjectGuid, BattleRoyalePlayer> const& GetPlayers() const { return m_players; }

private:
    void UpdateDeploying(uint32 diff, Map* map);
    void CompleteDeployment(Player* player, BattleRoyalePlayer& brPlayer, bool teleportToLandingPoint);
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

    uint32  m_deploymentTimer  = 30000;
    uint32  m_pendingBotCount  = 0;   // bots created but not yet added via AddPlayer
    uint32  m_landedCount      = 0;
    uint32  m_prepareTimer     = 30000;
    uint32  m_aliveCount       = 0;
    uint32  m_totalCount       = 0;
    uint32  m_finishTimer      = 0;
    uint32  m_runningTime      = 0;
};

#endif // MANGOS_BATTLEROYALE_H
