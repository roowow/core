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
        t.centerX    = 990.0f;  // Gold Mine / geometric center of all 5 nodes
        t.centerY    = 1008.0f;
        t.maxPlayers = 20;
        t.enabled    = true;

        // 20 spawn points across AB.
        // Z source: db = derived from database (guard/flag/trigger), est = estimated from nearby terrain.
        // All must be verified in-game with .go xyz before use in production.
        BRSpawnPoint const sp[] = {
            { 1313.0f, 1310.0f,  -7.7f, 0.0f  }, // db: alliance base trigger
            {  684.0f,  681.0f, -12.9f, 3.14f }, // db: horde base trigger
            { 1182.0f, 1183.0f, -45.3f, 0.0f  }, // db: stables guard
            { 1200.0f, 1160.0f, -56.4f, 3.14f }, // db: stables flag ref
            {  820.0f,  815.0f, -57.7f, 1.57f }, // db: blacksmith guard
            {  840.0f,  858.0f, -56.5f, 4.71f }, // db: blacksmith flag ref
            {  810.0f, 1185.0f,  11.9f, 0.0f  }, // db: farm flag
            {  790.0f, 1165.0f,  11.9f, 3.14f }, // db: farm flag ref
            { 1147.0f,  820.0f, -98.4f, 1.57f }, // db: lumber mill flag
            { 1175.0f,  832.0f,-106.6f, 0.0f  }, // db: lumber mill guard
            {  820.0f, 1178.0f,  36.4f, 3.14f }, // db: lumber mill area guard
            {  990.0f, 1010.0f, -42.6f, 1.57f }, // db: gold mine flag
            {  997.0f, 1003.0f, -31.4f, 0.0f  }, // db: gold mine guard
            { 1290.0f, 1230.0f, -30.0f, 3.14f }, // est: alliance-stables road
            { 1100.0f, 1100.0f, -42.0f, 0.0f  }, // est: northeast center
            {  900.0f, 1100.0f, -25.0f, 1.57f }, // est: farm-gold mine road
            {  720.0f,  950.0f, -20.0f, 0.0f  }, // est: horde-farm road
            {  740.0f,  790.0f, -30.0f, 4.71f }, // est: horde-blacksmith road
            {  900.0f,  930.0f, -42.0f, 3.14f }, // est: blacksmith-gold mine road
            { 1060.0f,  910.0f, -65.0f, 0.0f  }, // est: lumber mill-gold mine road
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
