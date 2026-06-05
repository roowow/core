#ifndef MANGOS_BATTLEROYALETEMPLATE_H
#define MANGOS_BATTLEROYALETEMPLATE_H

#include "Common.h"
#include <vector>
#include <array>

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
};

// Arathi Basin MVP template – spawn coordinates must be verified in-game by a GM.
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
        // Teleport destination = first node of the shared orbit path (br_ab_orbit, ID 909999).
        // The orbit is a circle of radius 60 centred on (990, 1008); node 0 is at angle 0°
        // (east), so x = 990 + 60 = 1050. CustomTaxiMgr::Play() requires the player to be
        // within 20 yards of node 0, so the staging point must equal this position.
        t.deploymentStart = { 1050.0f, 1008.0f, 250.0f, 0.0f };

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

        return t;
    }();
    return tmpl;
}

// Alterac Valley template – spawn coordinates must be verified in-game by a GM.
inline BattleRoyaleTemplate& GetAVTemplate()
{
    static BattleRoyaleTemplate tmpl = []() -> BattleRoyaleTemplate
    {
        BattleRoyaleTemplate t;
        t.id         = 2;
        t.mapId      = 30; // MAP_ALTERAC_VALLEY
        t.centerX    = -256.68f; // NPC 12159 (Korrak) — verified AV map center
        t.centerY    = -301.7f;
        t.maxPlayers = 40;
        t.enabled    = true;
        // High staging point above AV center; first orbit node at angle 0° (east)
        t.deploymentStart = { -196.68f, -301.7f, 500.0f, 0.0f }; // center + 60 east (orbit node 0)

        BRZonePhase const phases[] = {
            { 450.0f, 320.0f, 3 * 60 * 1000,  2.0f  }, // phase 1:  0:00,  2%/s (~50s to die)
            { 320.0f, 200.0f, 2 * 60 * 1000,  4.0f  }, // phase 2:  3:00,  4%/s (~25s to die)
            { 200.0f,  90.0f, 2 * 60 * 1000,  8.0f  }, // phase 3:  5:00,  8%/s (~12s to die)
            {  90.0f,  35.0f, 1 * 60 * 1000, 15.0f  }, // phase 4:  7:00, 15%/s (~7s to die)
            {  35.0f,  35.0f, 0,             25.0f  }, // final ring:  8:00, 25%/s (~4s to die)
        }; // total: 8 min
        for (BRZonePhase const& ph : phases)
            t.phases.push_back(ph);

        return t;
    }();
    return tmpl;
}

// All enabled templates in random-selection order.
// Add new templates here when they are ready.
inline std::array<BattleRoyaleTemplate*, 2> GetAllBRTemplates()
{
    return { &GetABTemplate(), &GetAVTemplate() };
}

#endif // MANGOS_BATTLEROYALETEMPLATE_H
