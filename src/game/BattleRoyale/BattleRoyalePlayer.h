#ifndef MANGOS_BATTLEROYALEPLAYER_H
#define MANGOS_BATTLEROYALEPLAYER_H

#include "ObjectGuid.h"
#include "SharedDefines.h"

struct BattleRoyalePlayer
{
    ObjectGuid    guid;
    bool          alive         = true;
    bool          outsideZone   = false;
    uint32        zoneWarnTimer = 0;   // countdown to next direction warning (ms)
    uint32        placementRank = 0;   // 0 = still in game, 1 = winner, N = Nth eliminated

    // Saved on enter, restored on leave
    WorldLocation savedPosition;
    bool          savedFFAPvP   = false;

    bool IsBot() const { return guid.IsEmpty(); } // bots will have their guid set normally; this is a placeholder
};

#endif // MANGOS_BATTLEROYALEPLAYER_H
