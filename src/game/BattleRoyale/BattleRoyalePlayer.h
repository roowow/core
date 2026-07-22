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
    bool          isGM              = false;  // 账号安全等级 > SEC_PLAYER 的真人玩家（典型场景：.br join 进场观察）；
                                               // 不参与队列加入播报、不计入 AwardSeasonScore / character_log_battle_royale，
                                               // 也不广播自己的淘汰消息，方便GM混入对局观察其他玩家而不暴露/不干扰排行榜
    bool          outsideZone       = false;
    uint32        zoneWarnTimer     = 0;
    uint32        placementRank     = 0;      // 0 = still in game, 1 = winner, N = Nth eliminated
    BRSpawnPoint  landingPoint      = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32        deploymentPathId  = 0;
    uint32        orbitSlot         = 0;      // join order (0-based); spreads entry angles evenly around the orbit
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
