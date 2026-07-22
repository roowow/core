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
    uint32 orbitPathId; // shared orbit taxi path ridden by all players before branching
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
        t.id          = 1;
        t.mapId       = 529; // MAP_ARATHI_BASIN
        t.orbitPathId = 909999; // br_ab_orbit
        t.centerX     = 990.0f;  // Gold Mine / geometric center of all 5 nodes
        t.centerY     = 1008.0f;
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
            {  50.0f,   0.0f, 3 * 60 * 1000, 25.0f  }, // phase 5: 12:00, keeps shrinking to 0
        }; // total: 15 min
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
        t.id          = 2;
        t.mapId       = 30; // MAP_ALTERAC_VALLEY
        t.orbitPathId = 909998; // br_av_orbit
        t.centerX     = -256.68f; // NPC 12159 (Korrak) — verified AV map center
        t.centerY     = -301.7f;
        t.maxPlayers = 40;
        t.enabled    = true;
        // High staging point above AV center; first orbit node at angle 0° (east)
        t.deploymentStart = { -196.68f, -301.7f, 500.0f, 0.0f }; // center + 60 east (orbit node 0)

        BRZonePhase const phases[] = {
            { 450.0f, 320.0f, 3 * 60 * 1000,  2.0f  }, // phase 1:  0:00,  2%/s (~50s to die)
            { 320.0f, 200.0f, 2 * 60 * 1000,  4.0f  }, // phase 2:  3:00,  4%/s (~25s to die)
            { 200.0f,  90.0f, 2 * 60 * 1000,  8.0f  }, // phase 3:  5:00,  8%/s (~12s to die)
            {  90.0f,  35.0f, 1 * 60 * 1000, 15.0f  }, // phase 4:  7:00, 15%/s (~7s to die)
            {  35.0f,   0.0f, 3 * 60 * 1000, 25.0f  }, // phase 5:  8:00, keeps shrinking to 0
        }; // total: 11 min
        for (BRZonePhase const& ph : phases)
            t.phases.push_back(ph);

        return t;
    }();
    return tmpl;
}

// Azshara Crater template. Its deployment orbit/routes are generated from
// battle_royale_spawn_point rows by BattleRoyale.sql.
inline BattleRoyaleTemplate& GetAzsharaCraterTemplate()
{
    static BattleRoyaleTemplate tmpl = []() -> BattleRoyaleTemplate
    {
        BattleRoyaleTemplate t;
        t.id          = 3;
        t.mapId       = 37; // MAP_AZSHARA_CRATER
        t.orbitPathId = 909997; // br_azshara_crater_orbit, to be created with deployment routes
        t.centerX     = 157.216248f; // GM-verified Azshara Crater center
        t.centerY     = 74.673859f;
        t.maxPlayers  = 30;
        // BattleRoyale.sql sets map_template.map_type = 3 (MAP_BATTLEGROUND)
        // for map 37 so CreateBgMap() and IsBattleRoyaleTemplateBattlegroundMap()
        // accept it.  Re-run BattleRoyale.sql if map 37 still shows a wrong type.
        t.enabled     = true;
        // First orbit node should be center + 60 yards east, high above the arena center.
        t.deploymentStart = { 217.216248f, 74.673859f, 430.783401f, 0.0f };

        BRZonePhase const phases[] = {
            { 460.0f, 320.0f, 3 * 60 * 1000,  2.0f  }, // phase 1:  0:00,  2%/s (~50s to die)
            { 320.0f, 200.0f, 3 * 60 * 1000,  4.0f  }, // phase 2:  3:00,  4%/s (~25s to die)
            { 200.0f,  90.0f, 3 * 60 * 1000,  8.0f  }, // phase 3:  6:00,  8%/s (~12s to die)
            {  90.0f,  35.0f, 3 * 60 * 1000, 15.0f  }, // phase 4:  9:00, 15%/s (~7s to die)
            {  35.0f,   0.0f, 3 * 60 * 1000, 25.0f  }, // phase 5: 12:00, keeps shrinking to 0
        }; // total: 15 min
        for (BRZonePhase const& ph : phases)
            t.phases.push_back(ph);

        return t;
    }();
    return tmpl;
}

// Mount Hyjal (World Tree area, Map:1 Kalimdor) — 野外开放世界地图，占位模板。
// mapId=1 是大陆地图，不是战场类型地图，IsBattleRoyaleTemplateBattlegroundMap() 会拒绝它，
// 所以 enabled=false，且无论如何都进不了真实对局的随机/指定选择（见 BattleRoyaleMgr.cpp
// IsBattleRoyaleTemplateStartable() / CreateInstance() 里的多处硬性检查）。
// 这个占位模板存在的唯一目的：让 .br spawn add / .br spawn list 命令能识别 map 1，
// 方便在真正的 OPEN_WORLD 地图承载层（见 BattleRoyale.md「分层设计草案」）落地之前，
// 先把出生点坐标录进 battle_royale_spawn_point 表。orbitPathId/deploymentStart/phases
// 都是未定的占位值，承载层实现完之后再回来补齐、把 enabled 改成 true。
inline BattleRoyaleTemplate& GetHyjalTemplate()
{
    static BattleRoyaleTemplate tmpl = []() -> BattleRoyaleTemplate
    {
        BattleRoyaleTemplate t;
        t.id          = 4;
        t.mapId       = 1; // Kalimdor（海加尔山 世界之树）
        t.orbitPathId = 0; // TODO: 需要新建一条 taxi 环绕路线，承载层落地后再补
        t.centerX     = 5502.959961f; // 实测边界框中心（BattleRoyale.md 已记录）
        t.centerY     = -3611.911377f;
        t.maxPlayers  = 30; // TODO: 待定，先用跟 AB/Azshara Crater 一样的规模占位
        t.enabled     = false; // 承载层没实现完之前绝不能进正式轮换
        t.deploymentStart = { 5502.959961f, -3611.911377f, 1700.0f, 0.0f }; // TODO: 待定占位

        // spawnPoints 由 BattleRoyaleMgr::LoadSpawnPoints() 从数据库加载，此处留空。
        // 使用 .br spawn add 命令在游戏内站到目标位置后记录坐标。

        // TODO: 缩圈阶段数据，等地图承载层确定能用之后再定，这里先留空。

        return t;
    }();
    return tmpl;
}

// All enabled templates in random-selection order.
// Add new templates here when they are ready.
inline std::array<BattleRoyaleTemplate*, 4> GetAllBRTemplates()
{
    return { &GetABTemplate(), &GetAVTemplate(), &GetAzsharaCraterTemplate(), &GetHyjalTemplate() };
}

#endif // MANGOS_BATTLEROYALETEMPLATE_H
