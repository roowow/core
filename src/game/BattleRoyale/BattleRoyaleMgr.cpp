#include "BattleRoyaleMgr.h"
#include "BattleGroundBR.h"

#include "Policies/SingletonImp.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Corpse.h"
#include "MapManager.h"
#include "Map.h"
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

#include "Utilities/Random.h"
#include <algorithm>

INSTANTIATE_SINGLETON_1(BattleRoyaleMgr);

namespace
{
// Once the pre-match countdown drops to this many ms remaining, the queued
// players + template for the upcoming match are locked in and CreateInstance()
// runs right away (bots start logging in and circling) — instead of waiting for
// the countdown to hit zero, which is when real players actually get teleported
// in (see BattleRoyaleMgr::Update()/AdmitPendingRealPlayers()). Gives bots the
// rest of the countdown to join, no matter which template ends up selected.
uint32 const BR_LOCK_THRESHOLD_MS = 60000;

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

void SetBattleRoyaleStartError(std::string* outError, char const* message)
{
    if (outError)
        *outError = message;
}

bool IsBattleRoyaleTemplateBattlegroundMap(BattleRoyaleTemplate const& tmpl)
{
    MapEntry const* mapEntry = sMapStorage.LookupEntry<MapEntry>(tmpl.mapId);
    return mapEntry && mapEntry->IsBattleGround();
}

// INSTANCED 模板的地图必须是战场类型（走 CreateBgMap()）；OPEN_WORLD 模板的地图
// 天生就不是战场类型（比如 Kalimdor 大陆），改成检查目标地图当前是否已经常驻加载
// （sMapMgr.FindMap 找已存在地图，不创建新的）。
bool IsBattleRoyaleTemplateMapReady(BattleRoyaleTemplate const& tmpl)
{
    if (tmpl.hostMode == BRMapHostMode::OPEN_WORLD)
        return sMapMgr.FindMap(tmpl.mapId, 0) != nullptr;
    return IsBattleRoyaleTemplateBattlegroundMap(tmpl);
}

bool IsBattleRoyaleTemplateStartable(BattleRoyaleTemplate const& tmpl)
{
    return tmpl.enabled && !tmpl.spawnPoints.empty() && IsBattleRoyaleTemplateMapReady(tmpl);
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

} // anonymous namespace

void BattleRoyaleMgr::SendMsgToParticipants(char const* msg) const
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);

    for (ObjectGuid const& guid : m_queue)
        if (Player* p = sObjectMgr.GetPlayer(guid))
            p->SendDirectMessage(&data);

    for (auto const& kv : m_playerInstMap)
        if (Player* p = sObjectMgr.GetPlayer(kv.first))
            if (!p->IsBot())
                p->SendDirectMessage(&data);
}

BattleRoyaleMgr::BattleRoyaleMgr()
{
    // Load persisted enabled state; default true if no DB entry yet.
    bool exists = false;
    uint32 val = sObjectMgr.GetSavedVariable(VAR_BATTLE_ROYALE_ENABLED, 1, &exists);
    if (exists)
        m_enabled = val != 0;

    LoadSpawnPoints();
    LoadDeploymentPaths();
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

void BattleRoyaleMgr::LoadDeploymentPaths()
{
    for (BattleRoyaleTemplate* tmpl : GetAllBRTemplates())
    {
        tmpl->deploymentPaths.clear();

        std::unique_ptr<QueryResult> result(WorldDatabase.PQuery(
            "SELECT `spawn_index`, `custom_taxi_path_id` FROM `battle_royale_deployment_path` "
            "WHERE `template_id` = %u", tmpl->id));

        if (!result)
            continue;

        do
        {
            Field* fields = result->Fetch();
            tmpl->deploymentPaths[fields[0].GetUInt32()] = fields[1].GetUInt32();
        }
        while (result->NextRow());

        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                 "[BattleRoyaleMgr] Loaded %u deployment paths for template %u.",
                 uint32(tmpl->deploymentPaths.size()), tmpl->id);
    }
}

void BattleRoyaleMgr::ScheduleCorpseCleanup(ObjectGuid guid, uint32 delayMs)
{
    m_pendingCorpseCleanup.emplace_back(guid, int32(delayMs));
}

void BattleRoyaleMgr::Update(uint32 diff)
{
    // Delete BR corpses once their loot window expires. Tracked here rather than on
    // the BattleRoyale instance itself, since that object can be destroyed almost
    // immediately after match end.
    for (auto it = m_pendingCorpseCleanup.begin(); it != m_pendingCorpseCleanup.end(); )
    {
        it->second -= int32(diff);
        if (it->second > 0)
        {
            ++it;
            continue;
        }

        if (Corpse* corpse = sObjectAccessor.GetCorpseForPlayerGUID(it->first))
        {
            sObjectAccessor.RemoveCorpse(corpse);
            corpse->DeleteFromDB();
            delete corpse;
        }
        it = m_pendingCorpseCleanup.erase(it);
    }

    // BattleRoyale::Update 正常情况下是靠 BattleGroundMap::Update -> BattleGroundBR::Update
    // 这条链驱动的（每个战场地图每帧自动调用挂在它身上的 BattleGround）。OPEN_WORLD 模式
    // 没有专属 BattleGroundMap（挂的是服务器常驻地图，那张地图完全不知道 host 这个对象存在），
    // 这条驱动链不存在，所以这里手动补上——对每个还在进行中的 OPEN_WORLD 对局直接调用
    // host->Update(diff)。INSTANCED 对局不受影响，仍然只靠地图自己驱动，这里跳过它们。
    for (auto const& kv : m_instances)
    {
        BattleRoyale* br = kv.second;
        if (br->GetStatus() == BattleRoyaleStatus::CANCELLED)
            continue;
        if (BattleGroundBR* host = br->GetHost())
            if (host->IsOpenWorldHosted())
                host->Update(diff);
    }

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
            m_pendingRealPlayerJoins.erase(instanceId);
            // Locked instances are normally admitted (see Update()'s countdown-zero
            // branch) before ever reaching CANCELLED, but a GM can cancel one
            // directly (.br cancel) while it's still mid-lock — don't leave m_locked
            // pointing at a now-deleted instance.
            if (m_locked && instanceId == m_lockedInstanceId)
            {
                m_locked = false;
                m_lockedInstanceId = 0;
            }
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
    uint32 const minPlayers = sWorld.getConfig(CONFIG_UINT32_BATTLE_ROYALE_MIN_PLAYERS);
    if (!hasActiveInstance && !m_countdownActive && m_queue.size() >= minPlayers)
    {
        uint32 const countdownSec = sWorld.getConfig(CONFIG_UINT32_BATTLE_ROYALE_COUNTDOWN_SEC);
        m_countdownActive = true;
        m_countdownTimer  = countdownSec * 1000;
        // First reminder threshold: depends on which frequency zone the countdown starts in.
        if (countdownSec > REMINDER_INTERVAL_SEC)
            m_nextReminderSec = countdownSec - REMINDER_INTERVAL_SEC; // 60s interval zone
        else if (countdownSec > 10)
            m_nextReminderSec = countdownSec - 10;                    // 10s interval zone
        else if (countdownSec > 1)
            m_nextReminderSec = countdownSec - 1;                     // 1s interval zone
        else
            m_nextReminderSec = 0;

        char buf[128];
        snprintf(buf, sizeof(buf), "[孤胆称雄] 论剑帖已发，%u 秒后封场开局。欲赴此局者，速至令使处留名。", countdownSec);
        SendMsgToParticipants(buf);
    }

    // Distinct from hasActiveInstance above: once locked, the just-created instance
    // (m_lockedInstanceId) is itself "active" in m_instances (DEPLOYING, with bots
    // already circling) for the rest of THIS SAME countdown — that must not pause
    // its own countdown, only some genuinely separate previous match should.
    bool hasOtherActiveInstance = false;
    for (auto const& kv : m_instances)
    {
        if (m_locked && kv.first == m_lockedInstanceId)
            continue;
        if (kv.second->GetStatus() != BattleRoyaleStatus::CANCELLED)
        {
            hasOtherActiveInstance = true;
            break;
        }
    }

    if (m_countdownActive)
    {
        if (hasOtherActiveInstance)
        {
            // Previous game still running — pause the countdown
        }
        else if (m_countdownTimer <= diff)
        {
            m_countdownActive = false;
            m_countdownTimer  = 0;
            if (m_locked)
            {
                // Bots have had the rest of the countdown to log in and circle
                // (see the lock branch below) — teleport the real players in now.
                auto it = m_instances.find(m_lockedInstanceId);
                if (it != m_instances.end())
                    AdmitPendingRealPlayers(m_lockedInstanceId, it->second);
                m_locked = false;
                m_lockedInstanceId = 0;
            }
            else
            {
                // Countdown was configured shorter than BR_LOCK_THRESHOLD_MS (or the
                // lock attempt below never succeeded) — create and admit back to
                // back right now instead of waiting any further.
                BattleRoyale* br = nullptr;
                if (TryCreateGame(m_forcedTemplateId, nullptr, &br))
                {
                    m_forcedTemplateId = 0;
                    AdmitPendingRealPlayers(br->GetHost()->GetInstanceID(), br);
                }
            }
        }
        else
        {
            if (!m_locked && m_countdownTimer <= BR_LOCK_THRESHOLD_MS)
            {
                // Lock this batch in: select + validate real players and a template
                // now, and create the instance (bots start logging in and circling
                // immediately) — but don't teleport real players in yet, that
                // happens once the countdown actually reaches zero, above.
                BattleRoyale* br = nullptr;
                if (TryCreateGame(m_forcedTemplateId, nullptr, &br))
                {
                    m_locked = true;
                    m_lockedInstanceId = br->GetHost()->GetInstanceID();
                    m_forcedTemplateId = 0;
                }
                // If it fails (e.g. the queue emptied out at the last moment), just
                // don't lock — this check retries every tick until the countdown
                // reaches zero and falls through to the immediate-fallback branch.
            }

            // Skip the "not enough online players" cancellation once locked: the
            // match is already committed with its own player list at that point,
            // independent of how many (if any) are left in m_queue afterward.
            if (!m_locked)
            {
                // Cancel countdown if online queue drops below minimum mid-tick.
                uint32 const minPlayers = sWorld.getConfig(CONFIG_UINT32_BATTLE_ROYALE_MIN_PLAYERS);
                uint32 onlineCount = 0;
                for (ObjectGuid const& g : m_queue)
                    if (Player* p = sObjectMgr.GetPlayer(g))
                        if (p->IsInWorld())
                            ++onlineCount;
                if (onlineCount < minPlayers)
                {
                    m_countdownActive = false;
                    m_countdownTimer  = 0;
                    SendMsgToParticipants("[孤胆称雄] 候战人数不足，本局取消，重新等待。");
                    return;
                }
            }

            m_countdownTimer -= diff;

            // Variable-frequency reminders:
            //   > 60s remaining  → every 60s
            //   10–60s remaining → every 10s
            //   1–10s remaining  → every 1s
            uint32 const remainSec = m_countdownTimer / 1000;
            if (remainSec <= m_nextReminderSec && m_nextReminderSec > 0)
            {
                char buf[128];
                if (remainSec <= 10)
                    snprintf(buf, sizeof(buf), "[孤胆称雄] %u！", remainSec);
                else if (remainSec <= 60)
                    snprintf(buf, sizeof(buf), "[孤胆称雄] 封场倒计时 %u 秒！", remainSec);
                else
                    snprintf(buf, sizeof(buf), "[孤胆称雄] 论剑帖将于 %u 秒后封场，尚未报名者速来。", remainSec);
                SendMsgToParticipants(buf);

                // Schedule next reminder at the appropriate interval for the next zone.
                if (remainSec <= 1)
                    m_nextReminderSec = 0;
                else if (remainSec <= 10)
                    m_nextReminderSec = remainSec - 1;
                else if (remainSec <= 60)
                    m_nextReminderSec = remainSec - 10;
                else
                    m_nextReminderSec = remainSec - REMINDER_INTERVAL_SEC;
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

    uint32 const queued     = uint32(m_queue.size());
    uint32 const minNeeded  = sWorld.getConfig(CONFIG_UINT32_BATTLE_ROYALE_MIN_PLAYERS);
    if (queued < minNeeded)
    {
        uint32 const needed = minNeeded - queued;
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
                queued, sWorld.getConfig(CONFIG_UINT32_BATTLE_ROYALE_COUNTDOWN_SEC));
    }

    // 通知队列中其他等待的玩家（GM账号混入观察时跳过，不暴露其在场，见 BattleRoyalePlayer::isGM）
    bool const isGMJoin = player->GetSession() && player->GetSession()->GetSecurity() > SEC_PLAYER;
    if (!isGMJoin)
    {
        for (ObjectGuid const& other : m_queue)
        {
            if (other == guid)
                continue;
            if (Player* p = sObjectMgr.GetPlayer(other))
                ChatHandler(p).PSendSysMessage(
                    "[孤胆称雄] %s 接下论剑帖，当前候战 %u 人。",
                    player->GetName(), queued);
        }
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

    if (m_queue.size() < sWorld.getConfig(CONFIG_UINT32_BATTLE_ROYALE_MIN_PLAYERS))
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

bool BattleRoyaleMgr::ForceStartNow(uint32 templateId, std::string* outError)
{
    // No longer bypasses the minimum-player/countdown gate — just forces which
    // template the next lock (or immediate fallback, see Update()) will use
    // instead of a random pick. The match itself still only actually starts once
    // enough real players are queued and the countdown reaches zero.
    if (templateId)
    {
        bool valid = false;
        for (BattleRoyaleTemplate* t : GetAllBRTemplates())
        {
            if (t->id == templateId)
            {
                valid = true;
                break;
            }
        }
        if (!valid)
        {
            SetBattleRoyaleStartError(outError, "指定模板不存在。");
            return false;
        }
    }

    m_forcedTemplateId = templateId;
    return true;
}

BattleRoyale* BattleRoyaleMgr::GetInstanceForPlayer(ObjectGuid guid)
{
    auto it = m_playerInstMap.find(guid);
    if (it == m_playerInstMap.end())
        return nullptr;
    auto jt = m_instances.find(it->second);
    return jt != m_instances.end() ? jt->second : nullptr;
}

bool BattleRoyaleMgr::TryGetAnonName(ObjectGuid guid, std::string& outName)
{
    BattleRoyale* br = GetInstanceForPlayer(guid);
    if (!br)
        return false;

    auto const& players = br->GetPlayers();
    auto it = players.find(guid);
    if (it == players.end() || it->second.bot || it->second.anonName.empty())
        return false;

    outName = it->second.anonName;
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

    // Static reference data, loaded once at startup (BattleRoyaleMgr::LoadDeploymentPaths())
    // instead of queried per bot join - this table doesn't change at runtime.
    uint32 deploymentPathId = ResolveBattleRoyaleDeploymentPath(tmpl.id, spawnIndex, tmpl.deploymentPaths);

    BRSpawnPoint const& start = tmpl.deploymentStart;
    bot->SetBattleGroundEntryPoint();
    br->AddPlayer(bot, sp, deploymentPathId, true /*isBot*/);
    m_playerInstMap[bot->GetObjectGuid()] = instanceId;

    ApplyBattleRoyaleStagingMount(bot, deploymentPathId);
    if (bot->GetMapId() == tmpl.mapId)
    {
        // 同地图（OPEN_WORLD 模板，比如海加尔山：机器人登录用的GM岛暂存点和BR比赛
        // 本身都在 map 1）。Player::TeleportTo() 的"近传送"分支依赖客户端一来一回
        // 确认（MSG_MOVE_TELEPORT_ACK），机器人这边确认包能收发但不生效（具体卡在
        // 引擎哪一层没有继续深挖），实测机器人位置最终完全不变。机器人不需要那套
        // "客户端平滑过渡"的仪式，直接调用 TeleportPositionRelocation() 强制定位，
        // 跳过整个确认流程。跨地图（AV/AB/Azshara Crater）不受影响，走原来的 TeleportTo()。
        bot->DisableSpline();
        bot->SetFallInformation(0.0f);
        bot->TeleportPositionRelocation(start.x, start.y, start.z, start.o);
    }
    else if (!bot->TeleportTo(tmpl.mapId, start.x, start.y, start.z, start.o))
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

bool BattleRoyaleMgr::TryCreateGame(uint32 templateId, std::string* outError, BattleRoyale** outInstance)
{
    if (m_queue.empty())
    {
        SetBattleRoyaleStartError(outError, "候战席无人。");
        return false;
    }

    // Pick a random enabled template, unless a GM explicitly requested one.
    auto allTmpls = GetAllBRTemplates();
    BattleRoyaleTemplate const* selectedTemplate = nullptr;
    if (templateId)
    {
        for (BattleRoyaleTemplate* t : allTmpls)
        {
            if (t->id == templateId)
            {
                selectedTemplate = t;
                break;
            }
        }

        if (!selectedTemplate)
        {
            SetBattleRoyaleStartError(outError, "指定模板不存在。");
            return false;
        }

        if (selectedTemplate->spawnPoints.empty())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Template %u has no spawn points.", templateId);
            SetBattleRoyaleStartError(outError, "指定模板没有出生点。");
            return false;
        }

        if (!IsBattleRoyaleTemplateMapReady(*selectedTemplate))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[BattleRoyaleMgr] Template %u map %u is not ready (not a battleground map, or open-world map not loaded).",
                     selectedTemplate->id, selectedTemplate->mapId);
            SetBattleRoyaleStartError(outError, "指定模板地图未就绪（不是战场类型，或野外地图未加载），当前不能创建 BR 对局。");
            return false;
        }
    }
    else
    {
        std::vector<BattleRoyaleTemplate*> eligible;
        for (BattleRoyaleTemplate* t : allTmpls)
            if (IsBattleRoyaleTemplateStartable(*t))
                eligible.push_back(t);

        if (eligible.empty())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] No enabled BR templates with spawn points.");
            SetBattleRoyaleStartError(outError, "没有可用的已开放模板。");
            return false;
        }

        selectedTemplate = eligible[urand(0, uint32(eligible.size()) - 1)];
    }

    BattleRoyaleTemplate const& tmpl = *selectedTemplate;

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

        // Re-validate: player state may have changed since they joined the queue.
        // Skip (and drop from queue) anyone now in a BG, instance, combat, or dead.
        if (p->InBattleGround() || p->GetMap()->Instanceable() ||
            p->IsInCombat() || p->IsDead() || p->IsTaxiFlying())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                     "[BattleRoyaleMgr] Dropping player %s (%u) from BR queue: state changed (bg=%u inst=%u combat=%u dead=%u taxi=%u).",
                     p->GetName(), p->GetGUIDLow(),
                     p->InBattleGround() ? 1 : 0,
                     p->GetMap()->Instanceable() ? 1 : 0,
                     p->IsInCombat() ? 1 : 0,
                     p->IsDead() ? 1 : 0,
                     p->IsTaxiFlying() ? 1 : 0);
            ChatHandler(p).PSendSysMessage("[孤胆称雄] 你的状态已不符合入局条件，已从候战席中移除。");
            continue;
        }

        // uncapRealPlayers templates (e.g. Hyjal) take every eligible queued real
        // player into this same match — maxPlayers there only sizes the bot fill-in
        // below, it doesn't cap real admission. Other templates keep the original
        // cap: overflow stays queued for the next game.
        if (tmpl.uncapRealPlayers || uint32(players.size()) < tmpl.maxPlayers)
            players.push_back(p);
        else
            remaining.push_back(guid);
    }

    m_queue = remaining;

    uint32 const brMinPlayers = sWorld.getConfig(CONFIG_UINT32_BATTLE_ROYALE_MIN_PLAYERS);
    if (uint32(players.size()) < brMinPlayers)
    {
        // Not enough online players - put them back in their original order and wait.
        std::deque<ObjectGuid> retryQueue;
        for (Player* p : players)
            retryQueue.push_back(p->GetObjectGuid());
        retryQueue.insert(retryQueue.end(), remaining.begin(), remaining.end());
        m_queue = retryQueue;
        if (m_queue.size() < brMinPlayers)
        {
            m_countdownActive = false;
            m_countdownTimer  = 0;
        }
        SetBattleRoyaleStartError(outError, "在线候战人数不足。");
        return false;
    }

    if (players.empty())
    {
        SetBattleRoyaleStartError(outError, "没有在线候战玩家。");
        return false;
    }

    BattleRoyale* br = CreateInstance(players, tmpl);
    if (!br)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Failed to create BR instance.");
        SetBattleRoyaleStartError(outError, "创建对局失败。");
        return false;
    }

    if (outInstance)
        *outInstance = br;
    return true;
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

    if (tmpl.hostMode == BRMapHostMode::OPEN_WORLD)
    {
        // 挂到服务器常驻加载的那张地图上（continent/persistent map 的 instanceId 恒为0），
        // 不走 CreateBgMap()——那条路径内部会 new 一个 BattleGroundMap 并
        // MANGOS_ASSERT(map->IsBattleGround())，对 Kalimdor 这种非战场类型地图必炸。
        Map* map = sMapMgr.FindMap(tmpl.mapId, 0);
        if (!map)
        {
            delete host;
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Template %u: open-world map %u is not currently loaded.", tmpl.id, tmpl.mapId);
            return nullptr;
        }
        // GetInstanceID() 需要一个非0的合成ID，不能直接用地图自己的instanceId=0
        // （0在这套注册表体系里另有含义，见 BattleGround.h::GetInstanceID() 的注释）。
        host->SetOpenWorldMap(map, sMapMgr.GenerateInstanceId());
    }
    else
    {
        if (!IsBattleRoyaleTemplateBattlegroundMap(tmpl))
        {
            delete host;
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[BattleRoyaleMgr] Template %u map %u is not a battleground map; refusing to create BattleGroundMap.",
                     tmpl.id, tmpl.mapId);
            return nullptr;
        }

        Map* map = sMapMgr.CreateBgMap(tmpl.mapId, host);
        if (!map)
        {
            delete host;
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[BattleRoyaleMgr] Failed to create BG map %u.", tmpl.mapId);
            return nullptr;
        }
    }

    sBattleGroundMgr.AddBattleGround(host->GetInstanceID(), BATTLEGROUND_BR, host);

    auto* br = new BattleRoyale(&tmpl, host);
    host->SetOwner(br);
    // uncapRealPlayers templates can end up with more participants than maxPlayers
    // (real players alone already over the bot-fill target, so no bots join) — lock
    // in the real final headcount now so orbit entry-angle slots don't collide.
    br->SetOrbitTotalSlots(std::max(tmpl.maxPlayers, uint32(players.size())));

    uint32 instanceId = host->GetInstanceID();
    m_instances[instanceId] = br;

    std::vector<BRSpawnPoint> const& spawns = tmpl.spawnPoints;
    // Static reference data, loaded once at startup (BattleRoyaleMgr::LoadDeploymentPaths())
    // instead of queried per match start - this table doesn't change at runtime.
    std::map<uint32, uint32> const& deploymentPaths = tmpl.deploymentPaths;

    std::vector<uint32> spawnIndexes;
    spawnIndexes.reserve(spawns.size());
    for (uint32 i = 0; i < uint32(spawns.size()); ++i)
        spawnIndexes.push_back(i);
    for (uint32 i = uint32(spawnIndexes.size()); i > 1; --i)
        std::swap(spawnIndexes[i - 1], spawnIndexes[urand(0, i - 1)]);

    // Defer real players' actual join (AddPlayer + teleport) until the pre-match
    // countdown reaches zero — see BattleRoyaleMgr::Update()'s lock mechanism and
    // AdmitPendingRealPlayers(). Bots (requested below) get the time in between to
    // log in and start circling (BattleRoyale::UpdateDeploying()'s holding-loop
    // flight). Register real players in m_playerInstMap right away regardless, so
    // IsPlayerInGame() correctly blocks them from re-queueing during the wait.
    uint32 const realCount = uint32(players.size());
    std::vector<BRPendingRealPlayerJoin> pending;
    pending.reserve(realCount);
    for (uint32 i = 0; i < realCount; ++i)
    {
        Player* player = players[i];
        uint32 spawnIndex = spawnIndexes[i % spawnIndexes.size()];
        uint32 deploymentPathId = ResolveBattleRoyaleDeploymentPath(tmpl.id, spawnIndex, deploymentPaths);
        pending.push_back({player->GetObjectGuid(), spawns[spawnIndex], deploymentPathId});
        m_playerInstMap[player->GetObjectGuid()] = instanceId;
    }
    m_pendingRealPlayerJoins[instanceId] = std::move(pending);
    br->BeginAwaitingRealPlayers();

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

void BattleRoyaleMgr::AdmitPendingRealPlayers(uint32 instanceId, BattleRoyale* br)
{
    BattleRoyaleTemplate const* tmpl = br->GetTemplate();

    auto it = m_pendingRealPlayerJoins.find(instanceId);
    if (it != m_pendingRealPlayerJoins.end())
    {
        for (BRPendingRealPlayerJoin const& pending : it->second)
        {
            Player* player = sObjectMgr.GetPlayer(pending.guid);
            if (!player)
            {
                // Logged out during the wait — undo the m_playerInstMap registration
                // from CreateInstance() so they aren't stuck unable to re-queue.
                RemovePlayerFromInstance(pending.guid);
                continue;
            }

            player->SetBattleGroundEntryPoint();
            br->AddPlayer(player, pending.landingPoint, pending.deploymentPathId);

            if (tmpl)
            {
                BRSpawnPoint const& start = tmpl->deploymentStart;
                bool const teleported = player->TeleportTo(tmpl->mapId, start.x, start.y, start.z, start.o, TELE_TO_FORCE_MAP_CHANGE);
                ApplyBattleRoyaleStagingMount(player, pending.deploymentPathId);
                if (!teleported && player->IsMounted())
                    player->Unmount();
            }
        }
        m_pendingRealPlayerJoins.erase(it);
    }

    Map* map = br->GetHost() ? br->GetHost()->GetHostMap() : nullptr;
    br->ReleaseHoldingBots(map);
    br->MarkRealPlayersAdmitted();
}
