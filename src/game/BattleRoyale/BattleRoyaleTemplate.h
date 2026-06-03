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

        // 30 verified spawn points across AB (maxPlayers = 30; no modulo reuse).
        // Z source: db = derived from database, est = GM-verified in-game.
        BRSpawnPoint const sp[] = {
            { 1365.0f, 1280.0f,  -8.0f, 0.0f  }, // alliance base exterior
            {  700.0f,  730.0f, -20.0f, 3.14f }, // horde base exterior
            { 1182.0f, 1183.0f, -45.3f, 0.0f  }, // stables guard
            { 1200.0f, 1160.0f, -56.4f, 3.14f }, // stables area
            {  820.0f,  815.0f, -57.7f, 1.57f }, // blacksmith guard
            {  840.0f,  858.0f, -56.5f, 4.71f }, // blacksmith area
            {  860.0f, 1150.0f,  20.0f, 0.0f  }, // farm exterior road
            {  900.0f, 1090.0f,  -5.0f, 3.14f }, // farm south road
            { 1147.0f,  820.0f, -98.4f, 1.57f }, // lumber mill flag
            { 1155.0f,  840.0f, -98.4f, 0.0f  }, // lumber mill approach
            {  820.0f, 1178.0f,  36.4f, 3.14f }, // lumber mill platform
            { 1010.0f,  985.0f, -42.0f, 1.57f }, // gold mine exterior
            {  997.0f, 1003.0f, -31.4f, 0.0f  }, // gold mine guard
            { 1280.0f, 1220.0f, -20.0f, 3.14f }, // alliance-stables road
            { 1100.0f, 1100.0f, -42.0f, 0.0f  }, // northeast center
            {  900.0f, 1100.0f, -25.0f, 1.57f }, // farm-gold mine road
            {  750.0f,  965.0f, -22.0f, 0.0f  }, // horde-farm road
            {  740.0f,  790.0f, -30.0f, 4.71f }, // horde-blacksmith road
            {  900.0f,  930.0f, -42.0f, 3.14f }, // blacksmith-gold mine road
            { 1050.0f,  905.0f, -50.0f, 0.0f  }, // lumber mill-gold mine road
            { 1050.0f, 1200.0f, -30.0f, 0.0f  }, // alliance-farm road north
            {  800.0f,  700.0f, -35.0f, 3.14f }, // horde-blacksmith south
            { 1300.0f, 1350.0f,  -5.0f, 0.0f  }, // alliance base interior
            {  920.0f, 1250.0f,  10.0f, 3.14f }, // farm north road
            {  720.0f,  900.0f, -25.0f, 1.57f }, // horde-farm west
            {  990.0f,  870.0f, -48.0f, 0.0f  }, // gold mine-lumber mill south
            {  700.0f,  800.0f, -22.0f, 1.57f }, // horde base-blacksmith road
            { 1320.0f, 1160.0f, -30.0f, 3.14f }, // alliance base south road
            { 1100.0f,  900.0f, -55.0f, 4.71f }, // northeast-gold mine road
            { 1000.0f, 1150.0f, -20.0f, 0.0f  }, // gold mine-farm road north
        };
        for (BRSpawnPoint const& s : sp)
            t.spawnPoints.push_back(s);

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
