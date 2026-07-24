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

    // 盘旋入场绕几圈轨道再切入个人下降航线，默认2圈（原有行为，AB/AV/Azshara Crater不受影响）。
    uint32 orbitLapCount = 2;

    // 默认false：TryCreateGame() 收报名玩家时最多收maxPlayers个真人，超出的留在队列等下一局
    // （AB/AV/Azshara Crater这种小地图容量本来就有限，不适合超员）。
    // true时：maxPlayers只用来算"真人不够时补几个机器人"（realCount<maxPlayers时补齐），
    // 不再限制真人数量——报名超过maxPlayers也全部放进这一局，出生点不够就按spawnIndex取模
    // 重叠使用（已有逻辑天然支持，不用额外处理）。海加尔山这种开放世界地图设为true。
    bool uncapRealPlayers = false;

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
        t.orbitLapCount = 3;

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
        t.maxPlayers  = 30; // 已确定：只用来控制机器人补位数量，见 uncapRealPlayers
        t.enabled     = true; // 出生点录制完成，多轮实测确认地图承载/传送/机器人加入没问题，纳入正式随机轮换
        // 真人报名不设上限：30人只是机器人补位的目标基数（真人不够30个才补机器人），
        // 报名超过30人也全部放进这一局，不会把多出来的人留在队列等下一局。出生点不够
        // 时靠spawnIndex取模自动重叠使用，不需要额外处理。
        t.uncapRealPlayers = true;
        // 绕圈半径120码比其它模板大一倍，单人绕圈+下降总时长接近1分钟——机器人异步登录的
        // 延迟不再是问题，因为 BattleRoyaleMgr 现在对所有模板都是"候战倒计时剩60秒时锁定+
        // 提前登录机器人绕圈，倒计时归零真人才进场"，机器人早就有充足时间登录并开始绕圈了。
        // 轨道节点0在盘旋圆心(orbitCenterX/Y，见下面)正东120码（角度0：cos0=1,sin0=0；
        // 海加尔山盘旋圈半径是其它模板的2倍：60->120），跟 BattleRoyale.sql 里生成
        // 轨道节点的公式对齐。高度用用户实地飞行验证过的GPS读数1850.940063（之前试过
        // 直接把1700翻倍到3400，实测太高看不清风景，改成这个校验过的高度）。落地航线的
        // node0/1/2高度也跟着同步改了（1850.940063 / 1835.940063，维持原来staging比
        // 30%混合点高15码的关系），保持衔接连续；node3往后的下降螺旋是相对落点地面高度
        // 的偏移量，没有改。注意这里不是 centerX/centerY（那是毒圈中心）。
        t.deploymentStart = { 5581.397949f, -3512.822510f, 1850.940063f, 0.0f };
        t.hostMode    = BRMapHostMode::OPEN_WORLD;
        // 绕2圈再切入个人下降航线，比默认1圈（约11-12秒）长一倍，给玩家多看看
        // 海加尔山的风景。
        t.orbitLapCount = 2;
        // 盘旋入场单独设置了风景点（世界之树附近），跟毒圈真正的中心点
        // （centerX/centerY）分开，缩圈范围不受影响。
        t.hasCustomOrbitCenter = true;
        t.orbitCenterX = 5461.397949f;
        t.orbitCenterY = -3512.822510f;
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

// GM岛（Map:1 Kalimdor，Zone/Area:876）— 第二个野外开放世界BR地图，复用海加尔山的
// OPEN_WORLD 分层设计（BRMapHostMode/GetHostMap()/CreateInstance() 的 OPEN_WORLD 分支），
// 不需要再验证一遍架构可行性。
//
// GM岛本身也是海加尔山模板机器人异步登录期间的暂存点（同map 1，见
// BattleRoyaleMgr.cpp::OnBotReady() 里"同地图直接TeleportPositionRelocation"那段注释）——
// 两个OPEN_WORLD模板可以在同一张常驻地图上同时开局互不冲突：CreateInstance() 用
// sMapMgr.GenerateInstanceId() 给每个OPEN_WORLD对局生成独立的合成instanceId，
// m_instances 按这个ID区分，不依赖mapId唯一性；GM岛本来就是玩家/生物到不了的隔离小岛，
// 跟海加尔山那边的比赛不会有视觉/移动上的交集。
//
// enabled=false：centerX/Y（用户截图中心点）、硬边界（openWorldMinX/MaxX/MinY/MaxY，
// 用户实地探路四个方向测出）、绕圈高度（deploymentStart.z=130.120071，用户实地飞行
// 找的GPS高度）都已经是实测值，phases 的起始半径也已经按实测边界重新核算过。
// 还剩下面几件事做完再改成true：
//   1. 用 .br spawn add 站到各个预定出生点记录 spawnPoints（数据库表，这里留空）。
//   2. 出生点录完后，核对是否有出生点落在220码的缩圈起始半径之外，超出的话
//      调大 phases 第一阶段的 startRadius（同步调整后续阶段维持相对节奏）。
//   3.（可选）绕圈半径目前沿用默认60码没有特殊调整，如果实地看着不满意可以
//      参照海加尔山放大到120码的做法调整，同步改下面 SQL 里909995轨道路径的半径。
inline BattleRoyaleTemplate& GetGMIslandTemplate()
{
    static BattleRoyaleTemplate tmpl = []() -> BattleRoyaleTemplate
    {
        BattleRoyaleTemplate t;
        t.id          = 5;
        t.mapId       = 1; // Kalimdor（GM岛）
        t.orbitPathId = 909995; // br_gm_island_orbit，见 BattleRoyale.sql
        t.centerX     = 16283.500000f; // 用户截图读出的GM岛中心点GPS坐标
        t.centerY     = 16297.299805f;
        t.maxPlayers  = 20; // 用户指定：小岛容量按20人左右设计
        t.enabled     = false; // 出生点还没录、绕圈高度/半径还没实地验证，见上方注释的3步清单
        t.hostMode    = BRMapHostMode::OPEN_WORLD;

        // 绕圈高度130.120071是用户实地飞行找的合适高度（GPS实测值，代替之前"地面+150"
        // 的占位估计）。半径仍沿用默认60码（未像海加尔山那样特殊放大，如果之后觉得
        // 60码看不清风景/太拥挤，可以参照海加尔山的做法单独放大）。
        // 轨道节点0 = 圆心正东60码。
        t.deploymentStart = { 16343.500000f, 16297.299805f, 130.120071f, 0.0f };

        // 实测边界：用户实地探路四个方向站到岛边缘读的GPS坐标（北 16384.062500/16275.951172，
        // 南 16141.498047/16254.906250，东 16235.166992/16172.987305，西 16218.019531/16348.754883）。
        // 北/南两点在X上差异最大（242.6码），东/西两点在Y上差异最大（175.8码）——
        // 确认这张地图跟海加尔山一样是标准WoW坐标系（X轴对应南北，Y轴对应东西）。
        // 直接取四点的X/Y极值做矩形边界，没有额外加安全余量（用户站的就是边缘，
        // 如果实测发现某个方向卡得太紧/太松，再单独调整那一侧）。
        // 注意：北/南/东三个点液位数据显示是站在深水区上方（地面在水面以下15~72码），
        // 说明这三个方向的"边缘"是往外海延伸了一段距离才停下，不是贴着陆地边缘；
        // 只有西点是站在实地上（GroundZ跟玩家Z几乎相同，30.72）。
        t.openWorldMinX = 16141.498047f; // 南
        t.openWorldMaxX = 16384.062500f; // 北
        t.openWorldMinY = 16172.987305f; // 东
        t.openWorldMaxY = 16348.754883f; // 西

        // spawnPoints 由 BattleRoyaleMgr::LoadSpawnPoints() 从数据库加载，此处留空。
        // 使用 .br spawn add 命令在游戏内站到目标位置后记录坐标。

        // 缩圈阶段：中心点(16283.5, 16297.3)到矩形边界四个角最远约188.7码（东南角），
        // 起始半径220码留了约30码余量（参照海加尔山"留足余量把出生点全部包进圈内"的
        // 做法）；出生点录完后如果发现有出生点落在220码圈外，要相应调大。中心点跟
        // 边界框的几何中心并不完全重合（框中心约16262.8/16260.9，跟用户截图给的
        // 16283.5/16297.3 相差20~36码），毒圈范围以centerX/Y（截图中心点）为准，
        // 目前留了足够余量，这点偏差不影响220码起始半径的覆盖结论。
        BRZonePhase const phases[] = {
            { 220.0f, 165.0f, 3 * 60 * 1000,  2.0f  }, // phase 1: 0:00,  2%/s
            { 165.0f, 110.0f, 2 * 60 * 1000,  4.0f  }, // phase 2: 3:00,  4%/s
            { 110.0f,  65.0f, 2 * 60 * 1000,  8.0f  }, // phase 3: 5:00,  8%/s
            {  65.0f,  25.0f, 1 * 60 * 1000, 15.0f  }, // phase 4: 7:00, 15%/s
            {  25.0f,   0.0f, 2 * 60 * 1000, 25.0f  }, // phase 5: 8:00, keeps shrinking to 0
        }; // total: 10 min（小岛+少人数，比大地图短；出生点录完后如有需要再微调半径）
        for (BRZonePhase const& ph : phases)
            t.phases.push_back(ph);

        return t;
    }();
    return tmpl;
}

// All enabled templates in random-selection order.
// Add new templates here when they are ready.
inline std::array<BattleRoyaleTemplate*, 5> GetAllBRTemplates()
{
    return { &GetABTemplate(), &GetAVTemplate(), &GetAzsharaCraterTemplate(), &GetHyjalTemplate(), &GetGMIslandTemplate() };
}

#endif // MANGOS_BATTLEROYALETEMPLATE_H
