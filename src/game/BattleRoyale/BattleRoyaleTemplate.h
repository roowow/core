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
    float  damagePercent; // % of max health per second, bypasses all mitigation
};

struct BattleRoyaleTemplate
{
    uint32 id;
    uint32 mapId;
    float  centerX;
    float  centerY;
    uint32 maxPlayers;
    bool   enabled;
    BRSpawnPoint deploymentStart;

    std::vector<BRSpawnPoint> spawnPoints;
    std::vector<BRZonePhase>  phases;
    std::vector<BRSpawnPoint> commonChestPoints; // 普通箱生成点，需 GM 现场验证
};

// Arathi Basin MVP template – spawn coordinates must be verified in-game by a GM.
// Returns a mutable reference so BattleRoyaleMgr::LoadChestPoints() can populate
// commonChestPoints from the database after server startup.
inline BattleRoyaleTemplate& GetABTemplate()
{
    // C++14 guarantees thread-safe initialization of static local variables (§6.7)
    static BattleRoyaleTemplate tmpl = []() -> BattleRoyaleTemplate
    {
        BattleRoyaleTemplate t;
        t.id         = 1;
        t.mapId      = 529; // MAP_ARATHI_BASIN
        t.centerX    = 990.0f;  // Gold Mine / geometric center of all 5 nodes
        t.centerY    = 1008.0f;
        t.maxPlayers = 30;
        t.enabled    = true;
        t.deploymentStart = { 990.0f, 1008.0f, 250.0f, 0.0f };

        // spawnPoints 由 BattleRoyaleMgr::LoadSpawnPoints() 从数据库加载，此处留空。
        // 使用 .br spawn add 命令在游戏内站到目标位置后记录坐标。

        BRZonePhase const phases[] = {
            { 520.0f, 380.0f, 3 * 60 * 1000,  2.0f  }, // phase 1:  0:00,  2%/s (~50s to die)
            { 380.0f, 230.0f, 3 * 60 * 1000,  4.0f  }, // phase 2:  3:00,  4%/s (~25s to die)
            { 230.0f, 120.0f, 3 * 60 * 1000,  8.0f  }, // phase 3:  6:00,  8%/s (~12s to die)
            { 120.0f,  50.0f, 3 * 60 * 1000, 15.0f  }, // phase 4:  9:00, 15%/s (~7s to die)
            {  50.0f,  50.0f, 0,             25.0f  }, // final ring: 12:00, 25%/s (~4s to die)
        }; // total: 12 min
        for (BRZonePhase const& ph : phases)
            t.phases.push_back(ph);

        // commonChestPoints 由 BattleRoyaleMgr::LoadChestPoints() 从数据库加载，此处留空。
        // 使用 .br chest add 命令在游戏内站到目标位置后记录坐标。

        return t;
    }();
    return tmpl;
}

#endif // MANGOS_BATTLEROYALETEMPLATE_H
