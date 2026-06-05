#ifndef MANGOS_BATTLEROYALEPLAYER_H
#define MANGOS_BATTLEROYALEPLAYER_H

#include "BattleRoyaleTemplate.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <string>

struct BattleRoyalePlayer
{
    ObjectGuid    guid;
    bool          alive             = true;
    bool          bot               = false;  // true = AI bot, does not receive rewards
    bool          outsideZone       = false;
    uint32        zoneWarnTimer     = 0;
    uint32        placementRank     = 0;      // 0 = still in game, 1 = winner, N = Nth eliminated
    BRSpawnPoint  landingPoint      = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32        deploymentPathId  = 0;
    bool          orbitStarted      = false;  // currently riding the shared orbit taxi
    bool          deploymentStarted = false;  // currently riding the individual drop taxi
    bool          landed            = false;
    uint32        killCount         = 0;
    bool          pendingReturn     = false;  // defer teleport out until Unit::Kill has finished

    // Captured just before ReturnPlayer() while the player is still in the BR map.
    // Used for character_log_battle_royale at match end.
    std::string   logName;
    std::string   logIp;
    uint32        logZone           = 0;
    uint32        logMap            = 0;

    // Saved on enter, restored on leave
    WorldLocation savedPosition;
    bool          savedFFAPvP = false;
};

#endif // MANGOS_BATTLEROYALEPLAYER_H
