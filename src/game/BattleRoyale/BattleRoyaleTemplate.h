#ifndef MANGOS_BATTLEROYALETEMPLATE_H
#define MANGOS_BATTLEROYALETEMPLATE_H

#include "Common.h"
#include <vector>

struct BRSpawnPoint
{
    float x, y, z, o;
};

struct BRZonePhase
{
    float  startRadius;
    float  endRadius;
    uint32 durationMs;
    uint32 damagePerSec;
};

struct BattleRoyaleTemplate
{
    uint32 id;
    uint32 mapId;
    float  centerX;
    float  centerY;
    uint32 maxPlayers;
    bool   enabled;

    std::vector<BRSpawnPoint> spawnPoints;
    std::vector<BRZonePhase>  phases;
};

// Arathi Basin MVP template – spawn coordinates must be verified in-game by a GM.
inline BattleRoyaleTemplate const& GetABTemplate()
{
    // C++14 guarantees thread-safe initialization of static local variables (§6.7)
    static BattleRoyaleTemplate const tmpl = []() -> BattleRoyaleTemplate
    {
        BattleRoyaleTemplate t;
        t.id         = 1;
        t.mapId      = 529; // MAP_ARATHI_BASIN
        t.centerX    = -867.0f;
        t.centerY    = -427.0f;
        t.maxPlayers = 20;
        t.enabled    = true;

        // 20 spawn points spread across AB (approximate, must be verified in-game)
        BRSpawnPoint const sp[] = {
            { -910.0f, -301.0f, -73.6f, 0.0f   },
            { -716.0f, -415.0f, -72.8f, 3.14f  },
            { -787.0f, -575.0f, -82.6f, 1.57f  },
            { -948.0f, -575.0f, -82.2f, 4.71f  },
            { -1040.0f,-415.0f, -73.5f, 0.0f   },
            { -867.0f, -370.0f, -73.0f, 1.57f  },
            { -867.0f, -490.0f, -73.0f, 4.71f  },
            { -800.0f, -427.0f, -73.0f, 3.14f  },
            { -940.0f, -427.0f, -73.0f, 0.0f   },
            { -820.0f, -340.0f, -72.0f, 2.35f  },
            { -780.0f, -500.0f, -80.0f, 0.78f  },
            { -960.0f, -500.0f, -79.0f, 5.49f  },
            { -960.0f, -340.0f, -71.5f, 3.92f  },
            { -680.0f, -360.0f, -68.0f, 2.0f   },
            { -1060.0f,-360.0f, -69.0f, 4.3f   },
            { -690.0f, -490.0f, -76.0f, 1.2f   },
            { -1050.0f,-490.0f, -76.0f, 5.0f   },
            { -867.0f, -300.0f, -71.0f, 1.57f  },
            { -760.0f, -550.0f, -84.0f, 1.0f   },
            { -980.0f, -550.0f, -84.0f, 5.2f   },
        };
        for (BRSpawnPoint const& s : sp)
            t.spawnPoints.push_back(s);

        BRZonePhase const phases[] = {
            { 600.0f, 400.0f, 2 * 60 * 1000,  80   }, // phase 1: 2 min
            { 400.0f, 230.0f, 90 * 1000,       200  }, // phase 2: 1.5 min
            { 230.0f, 120.0f, 90 * 1000,       400  }, // phase 3: 1.5 min
            { 120.0f,  50.0f, 1 * 60 * 1000,   800  }, // phase 4: 1 min
            {  50.0f,  50.0f, 0,               2000  }, // final ring
        };
        for (BRZonePhase const& ph : phases)
            t.phases.push_back(ph);

        return t;
    }();
    return tmpl;
}

#endif // MANGOS_BATTLEROYALETEMPLATE_H
