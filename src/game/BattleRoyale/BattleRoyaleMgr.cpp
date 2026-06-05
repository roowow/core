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
#include "MirrorTimer.h"
#include "MotionMaster.h"
#include "World.h"
#include "WorldPacket.h"
#include "PlayerBotMgr.h"

#include <algorithm>

INSTANTIATE_SINGLETON_1(BattleRoyaleMgr);

namespace
{
// Each template gets its own 10000-wide path ID block:
//   template 1 (AB): 910000 + spawnIndex
//   template 2 (AV): 920000 + spawnIndex
//   template N:      (910000 + (N-1)*10000) + spawnIndex
uint32 BRDefaultPathBase(uint32 templateId)
{
    return 910000u + (templateId - 1u) * 10000u;
}

bool IsBattleRoyaleDeploymentPathLoaded(uint32 pathId)
{
    if (!pathId)
        return false;

    auto const& paths = sCustomTaxiMgr.GetPaths();
    auto pathItr = paths.find(pathId);
    return pathItr != paths.end() && !pathItr->second.nodes.empty();
}

uint32 ResolveBattleRoyaleDeploymentPath(uint32 templateId, uint32 spawnIndex,
                                         std::map<uint32, uint32> const& deploymentPaths)
{
    auto pathItr = deploymentPaths.find(spawnIndex);
    if (pathItr != deploymentPaths.end() && IsBattleRoyaleDeploymentPathLoaded(pathItr->second))
        return pathItr->second;

    uint32 const defaultPathId = BRDefaultPathBase(templateId) + spawnIndex;
    if (IsBattleRoyaleDeploymentPathLoaded(defaultPathId))
        return defaultPathId;

    if (pathItr != deploymentPaths.end() && pathItr->second)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[BattleRoyaleMgr] Deployment path %u for spawn %u (template %u) is not loaded; "
                 "default path %u is unavailable.",
                 pathItr->second, spawnIndex, templateId, defaultPathId);
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
    // Load persisted enabled state; default true if no DB entry yet.
    bool exists = false;
    uint32 val = sObjectMgr.GetSavedVariable(VAR_BATTLE_ROYALE_ENABLED, 1, &exists);
    if (exists)
        m_enabled = val != 0;

    LoadSpawnPoints();
}

void BattleRoyaleMgr::LoadSpawnPoints()
{
    for (BattleRoyaleTemplate* tmpl : GetAllBRTemplates())
    {
        tmpl->spawnPoints.clear();

        std::unique_ptr<QueryResult> result(WorldDatabase.PQuery(
            "SELECT `position_x`, `position_y`, `position_z`, `orientation` "
            "FROM `battle_royale_spawn_point` "
            "WHERE `template_id` = %u ORDER BY `id`", tmpl->id));

        if (!result)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                     "[BattleRoyaleMgr] No spawn points in DB for template %u. "
                     "Use '.br spawn add' in-game to record positions.", tmpl->id);
            continue;
        }

        do
        {
            Field* fields = result->Fetch();
            BRSpawnPoint pt;
            pt.x = fields[0].GetFloat();
            pt.y = fields[1].GetFloat();
            pt.z = fields[2].GetFloat();
            pt.o = fields[3].GetFloat();
            tmpl->spawnPoints.push_back(pt);
        }
        while (result->NextRow());

        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                 "[BattleRoyaleMgr] Loaded %u spawn points for template %u.",
                 uint32(tmpl->spawnPoints.size()), tmpl->id);
    }
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

    // Only one BR instance runs at a time. Hold the countdown until the current game is
    // fully torn down (CANCELLED). FINISHED still has players being returned and the map
    // being cleaned up, so it also counts as active.
    bool hasActiveInstance = false;
    for (auto const& kv : m_instances)
    {
        if (kv.second->GetStatus() != BattleRoyaleStatus::CANCELLED)
        {
            hasActiveInstance = true;
            break;
        }
    }

    // Start countdown when queue fills
    if (!hasActiveInstance && !m_countdownActive && m_queue.size() >= MIN_PLAYERS)
    {
        m_countdownActive = true;
        m_countdownTimer  = COUNTDOWN_SEC * 1000;
        m_nextReminderSec = COUNTDOWN_SEC - REMINDER_INTERVAL_SEC;

        char buf[128];
        snprintf(buf, sizeof(buf), "[孤胆称雄] 论剑帖已发，%u 秒后封场开局。欲赴此局者，速至令使处留名。", COUNTDOWN_SEC);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, buf);
        sWorld.SendGlobalMessage(&data);
    }

    if (m_countdownActive)
    {
        if (hasActiveInstance)
        {
            // Previous game still running — pause the countdown
        }
        else if (m_countdownTimer <= diff)
        {
            m_countdownActive = false;
            m_countdownTimer  = 0;
            TryCreateGame();
        }
        else
        {
            m_countdownTimer -= diff;

            // Periodic reminders every REMINDER_INTERVAL_SEC seconds
            uint32 const remainSec = m_countdownTimer / 1000;
            if (remainSec <= m_nextReminderSec && m_nextReminderSec > 0)
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "[孤胆称雄] 论剑帖将于 %u 秒后封场，尚未报名者速来。", remainSec);
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, buf);
                sWorld.SendGlobalMessage(&data);

                m_nextReminderSec = (m_nextReminderSec > REMINDER_INTERVAL_SEC)
                    ? m_nextReminderSec - REMINDER_INTERVAL_SEC : 0;
            }
        }
    }
}

bool BattleRoyaleMgr::EnqueuePlayer(Player* player, std::string& outError)
{
    if (!CanEnqueue(player, outError))
        return false;

    ObjectGuid guid = player->GetObjectGuid();
    m_queue.push_back(guid);

    uint32 const queued = uint32(m_queue.size());
    if (queued < MIN_PLAYERS)
    {
        uint32 const needed = MIN_PLAYERS - queued;
        ChatHandler(player).PSendSysMessage(
            "[孤胆称雄] 你已接下论剑帖，当前候战 %u 人。还需 %u 人方可开局。",
            queued, needed);
    }
    else
    {
        bool hasActiveInstance = false;
        for (auto const& kv : m_instances)
        {
            if (kv.second->GetStatus() != BattleRoyaleStatus::CANCELLED)
            {
                hasActiveInstance = true;
                break;
            }
        }

        if (hasActiveInstance)
            ChatHandler(player).PSendSysMessage(
                "[孤胆称雄] 你已接下论剑帖，当前候战 %u 人。等待上一局结束后开局。",
                queued);
        else if (m_countdownActive)
            ChatHandler(player).PSendSysMessage(
                "[孤胆称雄] 你已接下论剑帖，当前候战 %u 人。%u 秒后开局。",
                queued, m_countdownTimer / 1000);
        else
            ChatHandler(player).PSendSysMessage(
                "[孤胆称雄] 你已接下论剑帖，当前候战 %u 人。%u 秒后开局。",
                queued, COUNTDOWN_SEC);
    }

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

    ChatHandler(player).PSendSysMessage("[孤胆称雄] 你收起论剑帖，暂离此番江湖局。");
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

void BattleRoyaleMgr::SavePendingRestore(Player const* player, uint32 instanceId) const
{
    if (!player || player->IsSavingDisabled())
        return;

    CharacterDatabase.PExecute(
        "REPLACE INTO `battle_royale_pending_restore` "
        "  (`guid`, `instance_id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `ffa_pvp`) "
        "VALUES (%u, %u, %u, %f, %f, %f, %f, %u)",
        player->GetGUIDLow(), instanceId, player->GetMapId(),
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation(),
        player->IsFFAPvP() ? 1u : 0u);
}

void BattleRoyaleMgr::ClearPendingRestore(ObjectGuid guid) const
{
    if (!guid.IsPlayer())
        return;

    CharacterDatabase.PExecute("DELETE FROM `battle_royale_pending_restore` WHERE `guid` = %u",
                               guid.GetCounter());
}

bool BattleRoyaleMgr::RestorePendingPlayer(Player* player) const
{
    if (!player || player->IsInWorld())
        return false;

    uint32 const guid = player->GetGUIDLow();
    std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
        "SELECT `instance_id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `ffa_pvp` "
        "FROM `battle_royale_pending_restore` WHERE `guid` = %u", guid));
    if (!result)
        return false;

    Field* fields = result->Fetch();
    uint32 const instanceId = fields[0].GetUInt32();
    uint32 const mapId      = fields[1].GetUInt32();
    float const x           = fields[2].GetFloat();
    float const y           = fields[3].GetFloat();
    float const z           = fields[4].GetFloat();
    float const o           = fields[5].GetFloat();
    bool const savedFfa     = fields[6].GetBool();

    if (!MapManager::IsValidMapCoord(mapId, x, y, z, o))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                 "[BattleRoyaleMgr] Invalid pending restore for player %u from instance %u: map %u %.2f %.2f %.2f %.2f.",
                 guid, instanceId, mapId, x, y, z, o);
        ClearPendingRestore(player->GetObjectGuid());
        return false;
    }

    if (player->IsTaxiFlying())
    {
        player->GetMotionMaster()->MovementExpired();
        player->GetTaxi().ClearTaxiDestinations();
    }

    if (player->IsHovering())
    {
        player->SetHover(false);
        player->SetHoverReal(false);
    }

    player->SendMirrorTimerStop(MirrorTimer::FATIGUE);
    player->SetFallInformation(0);
    BattleRoyale::CleanupBRItems(player);
    player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
    player->SetFFAPvP(savedFfa);
    player->SetBGTeam(TEAM_NONE);
    player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
    player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
    // Clear stale BG entry so login flow doesn't try to send the player back to
    // the now-gone BR instance. _SaveBGData() is private; replicate its DELETE.
    CharacterDatabase.PExecute("DELETE FROM `character_battleground_data` WHERE `guid` = %u",
                               player->GetGUIDLow());

    player->SetLocationMapId(mapId);
    player->Relocate(x, y, z, o);
    if (mapId <= MAX_CONTINENT_ID)
        player->SetLocationInstanceId(sMapMgr.GetContinentInstanceId(mapId, x, y));
    else
        player->SetLocationInstanceId(0);
    player->SetMap(sMapMgr.CreateMap(mapId, player));

    CharacterDatabase.PExecute(
        "UPDATE `characters` SET `map` = %u, `instance` = 0, "
        "`position_x` = %f, `position_y` = %f, `position_z` = %f, `orientation` = %f, "
        "`transport_guid` = 0, `transport_x` = 0, `transport_y` = 0, `transport_z` = 0, `transport_o` = 0, "
        "`current_taxi_path` = '' WHERE `guid` = %u",
        mapId, x, y, z, o, guid);
    ClearPendingRestore(player->GetObjectGuid());

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
             "[BattleRoyaleMgr] Restored player %s (%u) from pending BR instance %u to map %u %.2f %.2f %.2f.",
             player->GetName(), guid, instanceId, mapId, x, y, z);
    return true;
}

void BattleRoyaleMgr::RemovePlayerFromInstance(ObjectGuid guid)
{
    m_playerInstMap.erase(guid);
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
    if (br->GetStatus() != BattleRoyaleStatus::DEPLOYING)
    {
        // Game already past deployment (running, finished, or cancelled).
        // A late bot arriving now would bypass the deployment flow and land
        // mid-match with immunities that are never removed. Reject it.
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

    BattleRoyaleTemplate const* tmplPtr = br->GetTemplate();
    if (!tmplPtr)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] OnBotReady: instance %u has no template.", instanceId);
        br->DecrementPendingBotCount();
        if (PlayerBotEntry* e = bot->GetSession() ? bot->GetSession()->GetBot() : nullptr)
            e->requestRemoval = true;
        return;
    }
    BattleRoyaleTemplate const& tmpl = *tmplPtr;
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
    uint32 deploymentPathId = ResolveBattleRoyaleDeploymentPath(tmpl.id, spawnIndex, deploymentPaths);

    BRSpawnPoint const& start = tmpl.deploymentStart;
    bot->SetBattleGroundEntryPoint();
    br->AddPlayer(bot, sp, deploymentPathId, true /*isBot*/);
    m_playerInstMap[bot->GetObjectGuid()] = instanceId;

    ApplyBattleRoyaleStagingMount(bot, deploymentPathId);
    if (!bot->TeleportTo(tmpl.mapId, start.x, start.y, start.z, start.o))
    {
        if (bot->IsMounted())
            bot->Unmount();
    }
}

// --- private ---

bool BattleRoyaleMgr::CanEnqueue(Player* player, std::string& outError) const
{
    if (!m_enabled)
    {
        outError = "孤胆称雄暂未开放。";
        return false;
    }

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

    // Pick a random enabled template that has spawn points.
    auto allTmpls = GetAllBRTemplates();
    std::vector<BattleRoyaleTemplate*> eligible;
    for (BattleRoyaleTemplate* t : allTmpls)
        if (t->enabled && !t->spawnPoints.empty())
            eligible.push_back(t);

    if (eligible.empty())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] No enabled BR templates with spawn points.");
        return;
    }

    BattleRoyaleTemplate const& tmpl = *eligible[urand(0, uint32(eligible.size()) - 1)];

    // Collect online players up to maxPlayers. Offline entries are dropped so a
    // stale queued character cannot keep the counter alive or block the next game.
    std::vector<Player*> players;
    std::deque<ObjectGuid> remaining;

    for (auto it = m_queue.begin(); it != m_queue.end(); ++it)
    {
        ObjectGuid const& guid = *it;
        Player* p = sObjectMgr.GetPlayer(guid);
        if (!p || !p->IsInWorld())
        {
            continue;
        }

        if (uint32(players.size()) < tmpl.maxPlayers)
            players.push_back(p);
        else
            remaining.push_back(guid);
    }

    m_queue = remaining;

    if (!ignoreMinPlayers && uint32(players.size()) < MIN_PLAYERS)
    {
        // Not enough online players - put them back in their original order and wait.
        std::deque<ObjectGuid> retryQueue;
        for (Player* p : players)
            retryQueue.push_back(p->GetObjectGuid());
        retryQueue.insert(retryQueue.end(), remaining.begin(), remaining.end());
        m_queue = retryQueue;
        if (m_queue.size() < MIN_PLAYERS)
        {
            m_countdownActive = false;
            m_countdownTimer  = 0;
        }
        return;
    }

    if (players.empty())
        return;

    BattleRoyale* br = CreateInstance(players, tmpl);
    if (!br)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Failed to create BR instance.");
        return;
    }
}

BattleRoyale* BattleRoyaleMgr::CreateInstance(std::vector<Player*> const& players,
                                               BattleRoyaleTemplate const& tmpl)
{
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
        uint32 deploymentPathId = ResolveBattleRoyaleDeploymentPath(tmpl.id, spawnIndex, deploymentPaths);

        BRSpawnPoint const& start = tmpl.deploymentStart;

        // Register player BEFORE TeleportTo so BattleGroundMap::CanEnter()
        // finds the correct instanceId when the transfer is processed.
        player->SetBattleGroundEntryPoint();
        br->AddPlayer(player, sp, deploymentPathId);
        m_playerInstMap[player->GetObjectGuid()] = instanceId;

        ApplyBattleRoyaleStagingMount(player, deploymentPathId);
        if (!player->TeleportTo(tmpl.mapId, start.x, start.y, start.z, start.o))
        {
            if (player->IsMounted())
                player->Unmount();
        }
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

        // Set pending before spawning. Bot creation is async, but a very fast
        // ready callback can still arrive before this loop completes.
        br->SetPendingBotCount(botCount);

        for (uint32 i = 0; i < botCount; ++i)
        {
            if (!sPlayerBotMgr.AddBattleRoyaleBot(instanceId))
                br->DecrementPendingBotCount();
        }
    }

    return br;
}
