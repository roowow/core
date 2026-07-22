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

// INSTANCED：现有做法，走 CreateBgMap() 建一个专属 BattleGroundMap（AB/AV/Azshara Crater 都是这个）。
// OPEN_WORLD：挂到服务器本来就常驻加载的那张地图上（sMapMgr.FindMap(mapId, 0)），不新建实例。
// 见 BattleRoyale.md「分层设计草案」。
enum class BRMapHostMode : uint8
{
    INSTANCED,
    OPEN_WORLD,
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
    BRMapHostMode hostMode = BRMapHostMode::INSTANCED;

    // 硬边界（仅 hostMode==OPEN_WORLD 使用）：越出这个矩形范围立即淘汰传送回原位。
    // 平面判断 x < minX || x > maxX || y < minY || y > maxY，不判高度。
    float openWorldMinX = 0.0f;
    float openWorldMaxX = 0.0f;
    float openWorldMinY = 0.0f;
    float openWorldMaxY = 0.0f;

    // 盘旋入场轨道的中心点，默认跟毒圈中心(centerX/centerY)一致。如果想让盘旋路线
    // 落在别的风景点（不影响毒圈缩圈的实际中心），设置 orbitCenterX/Y 并把
    // hasCustomOrbitCenter 置 true；用 GetOrbitCenterX()/GetOrbitCenterY() 取值。
    float orbitCenterX = 0.0f;
    float orbitCenterY = 0.0f;
    bool  hasCustomOrbitCenter = false;
    float GetOrbitCenterX() const { return hasCustomOrbitCenter ? orbitCenterX : centerX; }
    float GetOrbitCenterY() const { return hasCustomOrbitCenter ? orbitCenterY : centerY; }

    // 盘旋入场绕几圈轨道再切入个人下降航线，默认1圈（原有行为，AB/AV/Azshara Crater不受影响）。
    uint32 orbitLapCount = 1;

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
        t.orbitLapCount = 2;

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

// Mount Hyjal (World Tree area, Map:1 Kalimdor) — 第一个野外开放世界BR地图。
// hostMode=OPEN_WORLD：走 BattleRoyaleMgr::CreateInstance() 里的 OPEN_WORLD 分支，
// 挂到常驻的 Kalimdor 地图上，不新建 BattleGroundMap，不受 IsBattleRoyaleTemplateBattlegroundMap()
// 那道"必须是战场类型地图"检查约束。
// enabled 暂时=false：出生点还在录制中（.br spawn add），且这条 OPEN_WORLD 分支本身刚实现完还
// 没有实测过，等出生点录够、真人测试确认没问题后再手动改成 true。
//
// orbitPathId 曾经是0（占位），实测发现这会导致一个真实bug：没有环绕飞行路线时，
// 真人玩家会在部署阶段第一个tick就直接完成落地，这时候机器人还没来得及异步登录
// 加入 m_players，UpdateDeploying() 里 m_landedCount>=m_totalCount 就已经成立，
// 直接转入RUNNING——等机器人登录完成再想加入，会被"迟到机器人拒收"逻辑全部拒绝
// （BattleRoyaleMgr.cpp::OnBotReady() 检查 GetStatus()!=DEPLOYING）。
// 已在 BattleRoyale.sql 里补了真实的环绕轨道（909996 br_hyjal_orbit）+ 每个出生点的
// 下降螺旋落地路线（940000+spawn_index），deploymentStart 对齐轨道节点0的位置。
inline BattleRoyaleTemplate& GetHyjalTemplate()
{
    static BattleRoyaleTemplate tmpl = []() -> BattleRoyaleTemplate
    {
        BattleRoyaleTemplate t;
        t.id          = 4;
        t.mapId       = 1; // Kalimdor（海加尔山 世界之树）
        t.orbitPathId = 909996; // br_hyjal_orbit，见 BattleRoyale.sql
        t.centerX     = 5502.959961f; // 实测边界框中心（BattleRoyale.md 已记录）
        t.centerY     = -3611.911377f;
        t.maxPlayers  = 30; // TODO: 待定，先用跟 AB/Azshara Crater 一样的规模占位
        t.enabled     = false; // 出生点录制中 + 未实测，先不进正式轮换
        // 轨道节点0在圆心正东60码（角度0：cos0=1,sin0=0），跟 BattleRoyale.sql 里
        // 生成轨道节点的公式对齐；staging高度1700，跟轨道节点/落地螺旋起点保持一致。
        t.deploymentStart = { 5562.959961f, -3611.911377f, 1700.0f, 0.0f };
        t.hostMode    = BRMapHostMode::OPEN_WORLD;
        // 绕2圈再切入个人下降航线，比默认1圈（约11-12秒）长一倍，给玩家多看看
        // 海加尔山的风景；轨道中心暂时还是用 centerX/Y（没有单独设置风景点）。
        t.orbitLapCount = 2;
        // 实测边界框（BattleRoyale.md「硬边界数据格式」），Y方向在出生点录制完成后
        // 发现比最初四方向探测的范围更宽（有6个出生点落在原边界外），已按实际出生点
        // 范围外扩重新调整，留出安全余量，不贴着出生点边缘。
        t.openWorldMinX = 5199.093262f; // 最南点.X（X方向出生点范围5217.87~5757.23，原边界够用，未调整）
        t.openWorldMaxX = 5806.826172f; // 最北点.X
        t.openWorldMinY = -3900.0f; // 原-3842.36，出生点128实测到-3875.23，外扩到-3900留余量
        t.openWorldMaxY = -3270.0f; // 原-3381.46，出生点117实测到-3298.06，外扩到-3270留余量

        // spawnPoints 由 BattleRoyaleMgr::LoadSpawnPoints() 从数据库加载，此处留空。
        // 使用 .br spawn add 命令在游戏内站到目标位置后记录坐标。

        // 缩圈阶段数据（之前留空导致毒圈完全不生效：BattleRoyaleZone::Update() 一开头
        // 就检查 phases.empty() 直接返回，缩圈/伤害/圈标记GameObject全部跳过不执行）。
        // 起始半径600码：实测圆心到硬边界四个角最远约457码，600码留了足够余量把出生点
        // 全部包进圈内。节奏跟AB/艾萨拉环形山同款，15分钟5阶段。
        BRZonePhase const phases[] = {
            { 600.0f, 450.0f, 3 * 60 * 1000,  2.0f  }, // phase 1:  0:00,  2%/s (~50s to die)
            { 450.0f, 300.0f, 3 * 60 * 1000,  4.0f  }, // phase 2:  3:00,  4%/s (~25s to die)
            { 300.0f, 180.0f, 3 * 60 * 1000,  8.0f  }, // phase 3:  6:00,  8%/s (~12s to die)
            { 180.0f,  80.0f, 3 * 60 * 1000, 15.0f  }, // phase 4:  9:00, 15%/s (~7s to die)
            {  80.0f,   0.0f, 3 * 60 * 1000, 25.0f  }, // phase 5: 12:00, keeps shrinking to 0
        }; // total: 15 min
        for (BRZonePhase const& ph : phases)
            t.phases.push_back(ph);

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
