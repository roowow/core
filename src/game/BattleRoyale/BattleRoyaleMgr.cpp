#include "BattleRoyaleMgr.h"
#include "BattleGroundBR.h"

#include "Policies/SingletonImp.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "MapManager.h"
#include "BattleGroundMgr.h"
#include "Chat.h"
#include "CustomTaxiMgr.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "World.h"
#include "WorldPacket.h"
#include "PlayerBotMgr.h"

#include <algorithm>

INSTANTIATE_SINGLETON_1(BattleRoyaleMgr);

namespace
{
uint32 const BR_DEFAULT_DEPLOYMENT_PATH_BASE = 910000;

BRSpawnPoint GetCompactDeploymentStart(BRSpawnPoint const& start, uint32 index)
{
    static float const offsets[][2] =
    {
        {   0.0f,   0.0f },
        {   6.0f,   0.0f }, {   0.0f,   6.0f }, {  -6.0f,   0.0f }, {   0.0f,  -6.0f },
        {   4.5f,   4.5f }, {  -4.5f,   4.5f }, {  -4.5f,  -4.5f }, {   4.5f,  -4.5f },
        {  12.0f,   0.0f }, {   0.0f,  12.0f }, { -12.0f,   0.0f }, {   0.0f, -12.0f },
        {   8.5f,   8.5f }, {  -8.5f,   8.5f }, {  -8.5f,  -8.5f }, {   8.5f,  -8.5f },
        {  18.0f,   0.0f }, {   0.0f,  18.0f }, { -18.0f,   0.0f }, {   0.0f, -18.0f },
        {  13.0f,  13.0f }, { -13.0f,  13.0f }, { -13.0f, -13.0f }, {  13.0f, -13.0f },
        {  16.0f,   8.0f }, {  -8.0f,  16.0f }, { -16.0f,  -8.0f }, {   8.0f, -16.0f },
        {  16.0f,  -8.0f }, {   8.0f,  16.0f }
    };

    BRSpawnPoint point = start;
    float const* offset = offsets[index % (sizeof(offsets) / sizeof(offsets[0]))];
    point.x += offset[0];
    point.y += offset[1];
    return point;
}

bool IsBattleRoyaleDeploymentPathLoaded(uint32 pathId)
{
    if (!pathId)
        return false;

    auto const& paths = sCustomTaxiMgr.GetPaths();
    auto pathItr = paths.find(pathId);
    return pathItr != paths.end() && !pathItr->second.nodes.empty();
}

uint32 ResolveBattleRoyaleDeploymentPath(uint32 spawnIndex, std::map<uint32, uint32> const& deploymentPaths)
{
    auto pathItr = deploymentPaths.find(spawnIndex);
    if (pathItr != deploymentPaths.end() && IsBattleRoyaleDeploymentPathLoaded(pathItr->second))
        return pathItr->second;

    uint32 const defaultPathId = BR_DEFAULT_DEPLOYMENT_PATH_BASE + spawnIndex;
    if (IsBattleRoyaleDeploymentPathLoaded(defaultPathId))
        return defaultPathId;

    if (pathItr != deploymentPaths.end() && pathItr->second)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[BattleRoyaleMgr] Deployment path %u for spawn %u is not loaded; default path %u is unavailable.",
                 pathItr->second, spawnIndex, defaultPathId);
    }

    return 0;
}

uint32 GetBattleRoyaleStagingMountDisplayId(Player* player, uint32 deploymentPathId)
{
    if (!player)
        return 0;

    if (deploymentPathId)
    {
        auto const& paths = sCustomTaxiMgr.GetPaths();
        auto pathItr = paths.find(deploymentPathId);
        if (pathItr != paths.end() && pathItr->second.mountDisplayId)
            return pathItr->second.mountDisplayId;
    }

    uint32 const sourceTaxiNode = player->GetTeam() == ALLIANCE ? 2 : 23;
    return sObjectMgr.GetTaxiMountDisplayId(sourceTaxiNode, player->GetTeam(), true);
}

void ApplyBattleRoyaleStagingMount(Player* player, uint32 deploymentPathId)
{
    uint32 const mountDisplayId = GetBattleRoyaleStagingMountDisplayId(player, deploymentPathId);
    if (!mountDisplayId)
        return;

    player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    if (player->IsInDisallowedMountForm())
    {
        player->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        player->RemoveSpellsCausingAura(SPELL_AURA_TRANSFORM);
    }

    player->Mount(mountDisplayId);
}
}

BattleRoyaleMgr::BattleRoyaleMgr()
{
    LoadSpawnPoints();
}

void BattleRoyaleMgr::LoadSpawnPoints()
{
    BattleRoyaleTemplate& tmpl = GetABTemplate();
    tmpl.spawnPoints.clear();

    std::unique_ptr<QueryResult> result(WorldDatabase.PQuery(
        "SELECT `position_x`, `position_y`, `position_z`, `orientation` "
        "FROM `battle_royale_spawn_point` "
        "WHERE `template_id` = %u ORDER BY `id`", tmpl.id));

    if (!result)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                 "[BattleRoyaleMgr] No spawn points in DB for template %u. "
                 "Use '.br spawn add' in-game to record positions.", tmpl.id);
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        BRSpawnPoint pt;
        pt.x = fields[0].GetFloat();
        pt.y = fields[1].GetFloat();
        pt.z = fields[2].GetFloat();
        pt.o = fields[3].GetFloat();
        tmpl.spawnPoints.push_back(pt);
    }
    while (result->NextRow());

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
             "[BattleRoyaleMgr] Loaded %u spawn points for template %u.",
             uint32(tmpl.spawnPoints.size()), tmpl.id);
}

void BattleRoyaleMgr::Update(uint32 diff)
{
    // BattleRoyale::Update is driven by BattleGroundMap::Update -> BattleGroundBR::Update.
    // Here we only check for completed instances and clean them up.
    for (auto it = m_instances.begin(); it != m_instances.end(); )
    {
        BattleRoyale* br = it->second;

        if (br->GetStatus() == BattleRoyaleStatus::CANCELLED)
        {
            uint32 instanceId = it->first;
            BattleGroundBR* host = br->GetHost();

            // Null the owner pointer BEFORE deleting to prevent use-after-free
            // (BattleGroundBR may still receive Update calls from the map thread)
            if (host)
                host->SetOwner(nullptr);

            // Remove player -> instance mappings
            for (auto jt = m_playerInstMap.begin(); jt != m_playerInstMap.end(); )
            {
                if (jt->second == instanceId)
                    jt = m_playerInstMap.erase(jt);
                else
                    ++jt;
            }

            delete br;
            // ~BattleGround() calls RemoveBattleGround() and m_map->SetUnload()+SetBG(nullptr),
            // which unregisters the host and schedules the BG map for unloading.
            delete host;
            m_botSpawnIndexes.erase(instanceId);
            it = m_instances.erase(it);
        }
        else
            ++it;
    }

    // Start countdown when queue fills
    if (!m_countdownActive && m_queue.size() >= MIN_PLAYERS)
    {
        m_countdownActive = true;
        m_countdownTimer  = COUNTDOWN_SEC * 1000;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, "[孤胆称雄] 猎场将启，60 秒后封场开局！想称雄者，速去令使处报名。");
        sWorld.SendGlobalMessage(&data);
    }

    if (m_countdownActive)
    {
        if (m_countdownTimer <= diff)
        {
            m_countdownActive = false;
            m_countdownTimer  = 0;
            TryCreateGame();
        }
        else
            m_countdownTimer -= diff;
    }
}

bool BattleRoyaleMgr::EnqueuePlayer(Player* player, std::string& outError)
{
    if (!CanEnqueue(player, outError))
        return false;

    ObjectGuid guid = player->GetObjectGuid();
    m_queue.push_back(guid);
    ChatHandler(player).PSendSysMessage("[孤胆称雄] 你已立下孤胆战约，当前候战 %u 人。", uint32(m_queue.size()));
    return true;
}

bool BattleRoyaleMgr::DequeuePlayer(Player* player)
{
    ObjectGuid guid = player->GetObjectGuid();
    auto it = std::find(m_queue.begin(), m_queue.end(), guid);
    if (it == m_queue.end())
        return false;

    m_queue.erase(it);

    if (m_queue.size() < MIN_PLAYERS)
    {
        m_countdownActive = false;
        m_countdownTimer  = 0;
    }

    ChatHandler(player).PSendSysMessage("[孤胆称雄] 你已收起战约，离开候战队列。");
    return true;
}

bool BattleRoyaleMgr::IsPlayerInQueue(ObjectGuid guid) const
{
    return std::find(m_queue.begin(), m_queue.end(), guid) != m_queue.end();
}

bool BattleRoyaleMgr::IsPlayerInGame(ObjectGuid guid) const
{
    return m_playerInstMap.find(guid) != m_playerInstMap.end();
}

void BattleRoyaleMgr::ForceStartNow()
{
    m_countdownActive = false;
    m_countdownTimer  = 0;
    TryCreateGame(true); // ignore MIN_PLAYERS for GM testing
}

BattleRoyale* BattleRoyaleMgr::GetInstanceForPlayer(ObjectGuid guid)
{
    auto it = m_playerInstMap.find(guid);
    if (it == m_playerInstMap.end())
        return nullptr;
    auto jt = m_instances.find(it->second);
    return jt != m_instances.end() ? jt->second : nullptr;
}

void BattleRoyaleMgr::OnInstanceEnd(uint32 /*instanceId*/)
{
    // Cleanup handled in Update when status == CANCELLED
}

void BattleRoyaleMgr::OnBotReady(Player* bot, uint32 instanceId)
{
    auto instIt = m_instances.find(instanceId);
    if (instIt == m_instances.end())
    {
        // Instance already gone (cancelled before bot finished loading)
        if (PlayerBotEntry* e = bot->GetSession() ? bot->GetSession()->GetBot() : nullptr)
            e->requestRemoval = true;
        return;
    }

    BattleRoyale* br = instIt->second;
    if (br->GetStatus() == BattleRoyaleStatus::FINISHED ||
        br->GetStatus() == BattleRoyaleStatus::CANCELLED)
    {
        br->DecrementPendingBotCount();
        if (PlayerBotEntry* e = bot->GetSession() ? bot->GetSession()->GetBot() : nullptr)
            e->requestRemoval = true;
        return;
    }

    // Allocate a spawn index for this bot
    auto idxIt = m_botSpawnIndexes.find(instanceId);
    if (idxIt == m_botSpawnIndexes.end() || idxIt->second.empty())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] OnBotReady: no spawn index left for instance %u.", instanceId);
        br->DecrementPendingBotCount();
        if (PlayerBotEntry* e = bot->GetSession() ? bot->GetSession()->GetBot() : nullptr)
            e->requestRemoval = true;
        return;
    }

    BattleRoyaleTemplate const& tmpl = GetABTemplate();
    uint32 spawnIndex = idxIt->second.back();
    idxIt->second.pop_back();

    BRSpawnPoint const& sp = tmpl.spawnPoints[spawnIndex % tmpl.spawnPoints.size()];

    std::map<uint32, uint32> deploymentPaths;
    std::unique_ptr<QueryResult> pathResult = WorldDatabase.PQuery(
        "SELECT `spawn_index`, `custom_taxi_path_id` FROM `battle_royale_deployment_path` "
        "WHERE `template_id` = %u AND `spawn_index` = %u", tmpl.id, spawnIndex);
    if (pathResult)
    {
        Field* fields = pathResult->Fetch();
        deploymentPaths[fields[0].GetUInt32()] = fields[1].GetUInt32();
    }
    uint32 deploymentPathId = ResolveBattleRoyaleDeploymentPath(spawnIndex, deploymentPaths);

    bot->SetBattleGroundEntryPoint();
    br->AddPlayer(bot, sp, deploymentPathId, true /*isBot*/);
    m_playerInstMap[bot->GetObjectGuid()] = instanceId;

    BRSpawnPoint const start = GetCompactDeploymentStart(tmpl.deploymentStart, spawnIndex);
    ApplyBattleRoyaleStagingMount(bot, deploymentPathId);
    if (!bot->TeleportTo(tmpl.mapId, start.x, start.y, start.z, start.o) && bot->IsMounted())
        bot->Unmount();

    sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[BattleRoyaleMgr] Bot %s ready for instance %u (spawn %u).",
             bot->GetName(), instanceId, spawnIndex);
}

// --- private ---

bool BattleRoyaleMgr::CanEnqueue(Player* player, std::string& outError) const
{
    ObjectGuid guid = player->GetObjectGuid();

    if (IsPlayerInQueue(guid))    { outError = "你已在队列中。";       return false; }
    if (IsPlayerInGame(guid))     { outError = "你已在对局中。";       return false; }
    if (player->GetLevel() < 60)  { outError = "需要 60 级才能参与。"; return false; }
    if (player->GetGroup())       { outError = "请先退出队伍再排队。"; return false; }
    if (player->InBattleGround()) { outError = "请先离开当前战场。";   return false; }
    if (player->IsInCombat())     { outError = "请先脱离战斗状态。";   return false; }
    if (player->IsDead())         { outError = "请在存活状态下排队。"; return false; }
    if (player->IsTaxiFlying())   { outError = "飞行途中无法排队。";   return false; }
    if (player->GetMap() && player->GetMap()->IsDungeon())
                                  { outError = "请先离开副本再排队。"; return false; }

    return true;
}

void BattleRoyaleMgr::TryCreateGame(bool ignoreMinPlayers)
{
    if (m_queue.empty())
        return;

    BattleRoyaleTemplate const& tmpl = GetABTemplate();

    // Collect online players up to maxPlayers
    std::vector<Player*> players;
    std::deque<ObjectGuid> remaining;

    for (auto it = m_queue.begin(); it != m_queue.end(); ++it)
    {
        ObjectGuid const& guid = *it;
        if (uint32(players.size()) >= tmpl.maxPlayers)
        {
            remaining.push_back(guid);
            continue;
        }
        Player* p = sObjectMgr.GetPlayer(guid);
        if (p && p->IsInWorld())
            players.push_back(p);
        else
            remaining.push_back(guid); // offline, preserve in queue
    }

    m_queue = remaining;

    if (!ignoreMinPlayers && uint32(players.size()) < MIN_PLAYERS)
    {
        // Not enough online players – put them back and wait
        for (Player* p : players)
            m_queue.push_front(p->GetObjectGuid());
        return;
    }

    if (players.empty())
        return;

    BattleRoyale* br = CreateInstance(players);
    if (!br)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Failed to create BR instance.");
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[BattleRoyaleMgr] Created BR instance with %u players.", uint32(players.size()));
}

BattleRoyale* BattleRoyaleMgr::CreateInstance(std::vector<Player*> const& players)
{
    BattleRoyaleTemplate const& tmpl = GetABTemplate();
    if (tmpl.spawnPoints.empty())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Template %u has no spawn points.", tmpl.id);
        return nullptr;
    }

    auto* host = new BattleGroundBR();
    host->SetMaxPlayers(tmpl.maxPlayers);
    host->SetMinPlayers(1);
    host->SetMapId(tmpl.mapId);

    Map* map = sMapMgr.CreateBgMap(tmpl.mapId, host);
    if (!map)
    {
        delete host;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Failed to create BG map %u.", tmpl.mapId);
        return nullptr;
    }

    sBattleGroundMgr.AddBattleGround(host->GetInstanceID(), BATTLEGROUND_BR, host);

    auto* br = new BattleRoyale(&tmpl, host);
    host->SetOwner(br);

    uint32 instanceId = host->GetInstanceID();
    m_instances[instanceId] = br;

    std::vector<BRSpawnPoint> const& spawns = tmpl.spawnPoints;
    std::map<uint32, uint32> deploymentPaths;
    std::unique_ptr<QueryResult> pathResult = WorldDatabase.PQuery(
        "SELECT `spawn_index`, `custom_taxi_path_id` FROM `battle_royale_deployment_path` "
        "WHERE `template_id` = %u", tmpl.id);
    if (pathResult)
    {
        do
        {
            Field* fields = pathResult->Fetch();
            deploymentPaths[fields[0].GetUInt32()] = fields[1].GetUInt32();
        }
        while (pathResult->NextRow());
    }

    std::vector<uint32> spawnIndexes;
    spawnIndexes.reserve(spawns.size());
    for (uint32 i = 0; i < uint32(spawns.size()); ++i)
        spawnIndexes.push_back(i);
    for (uint32 i = uint32(spawnIndexes.size()); i > 1; --i)
        std::swap(spawnIndexes[i - 1], spawnIndexes[urand(0, i - 1)]);

    uint32 const realCount = uint32(players.size());
    for (uint32 i = 0; i < realCount; ++i)
    {
        Player* player = players[i];
        uint32 spawnIndex = spawnIndexes[i % spawnIndexes.size()];
        BRSpawnPoint const& sp = spawns[spawnIndex];
        uint32 deploymentPathId = ResolveBattleRoyaleDeploymentPath(spawnIndex, deploymentPaths);

        // Register player BEFORE TeleportTo so BattleGroundMap::CanEnter()
        // finds the correct instanceId when the transfer is processed.
        player->SetBattleGroundEntryPoint();
        br->AddPlayer(player, sp, deploymentPathId);
        m_playerInstMap[player->GetObjectGuid()] = instanceId;

        BRSpawnPoint const deploymentStart = GetCompactDeploymentStart(tmpl.deploymentStart, spawnIndex);
        ApplyBattleRoyaleStagingMount(player, deploymentPathId);
        if (!player->TeleportTo(tmpl.mapId, deploymentStart.x, deploymentStart.y,
                                deploymentStart.z, deploymentStart.o) && player->IsMounted())
            player->Unmount();
    }

    // Fill remaining slots with bots
    uint32 const botCount = (tmpl.maxPlayers > realCount) ? (tmpl.maxPlayers - realCount) : 0;
    if (botCount > 0)
    {
        // Save remaining shuffled spawn indexes for bots arriving asynchronously
        std::vector<uint32> botIndexes;
        botIndexes.reserve(botCount);
        for (uint32 i = realCount; i < realCount + botCount; ++i)
            botIndexes.push_back(spawnIndexes[i % spawnIndexes.size()]);
        m_botSpawnIndexes[instanceId] = botIndexes;

        uint32 actualBotCount = 0;
        for (uint32 i = 0; i < botCount; ++i)
            if (sPlayerBotMgr.AddBattleRoyaleBot(instanceId))
                ++actualBotCount;
        br->SetPendingBotCount(actualBotCount);

        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[BattleRoyaleMgr] Requested %u/%u bots for instance %u.", actualBotCount, botCount, instanceId);
    }

    return br;
}
