#include "BattleRoyale.h"
#include "BattleGroundBR.h"

#include "Player.h"
#include "Map.h"
#include "MirrorTimer.h"
#include "BattleGround.h"
#include "BattleGroundMgr.h"
#include "Chat.h"
#include "CustomTaxiMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "PlayerBotMgr.h"

#include "Mail.h"
#include "GameObject.h"

#include <cstdio>

static uint32 const BR_FINISH_DELAY_MS     = 10000;
static uint32 const BR_DEPLOYMENT_START_DELAY_MS = 3000;
static uint32 const BR_COMMON_CHEST_ENTRY  = 900110;
static float  const BR_LANDING_CORRECTION_DISTANCE = 5.0f;

// All custom BR item entries live in this range; used for inventory cleanup on player exit.
static uint32 const BR_ITEM_ENTRIES[] = { 900200, 900210, 900211, 900212, 900213 };

BattleRoyale::BattleRoyale(BattleRoyaleTemplate const* tmpl, BattleGroundBR* host)
    : m_status(BattleRoyaleStatus::DEPLOYING), m_tmpl(tmpl), m_host(host),
      m_landedCount(0), m_deploymentTimer(30000),
      m_prepareTimer(30000), m_aliveCount(0), m_totalCount(0), m_finishTimer(0), m_runningTime(0)
{
    m_zone.Init(tmpl);
}

void BattleRoyale::AddPlayer(Player* player, BRSpawnPoint const& landingPoint, uint32 deploymentPathId, bool isBot)
{
    ObjectGuid guid = player->GetObjectGuid();

    BattleRoyalePlayer brPlayer;
    brPlayer.guid             = guid;
    brPlayer.alive            = true;
    brPlayer.bot              = isBot;
    brPlayer.outsideZone      = false;
    brPlayer.zoneWarnTimer    = 0;
    brPlayer.placementRank    = 0;
    brPlayer.landingPoint     = landingPoint;
    brPlayer.deploymentPathId = deploymentPathId;
    brPlayer.deploymentStartDelayTimer = BR_DEPLOYMENT_START_DELAY_MS;
    brPlayer.savedPosition    = WorldLocation(player->GetMapId(),
                                              player->GetPositionX(),
                                              player->GetPositionY(),
                                              player->GetPositionZ(),
                                              player->GetOrientation());
    brPlayer.savedFFAPvP = player->IsFFAPvP();

    m_players[guid] = brPlayer;
    ++m_totalCount;
    ++m_aliveCount;
    if (isBot && m_pendingBotCount > 0)
        --m_pendingBotCount;

    player->SetFFAPvP(true);
    player->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
    player->SetBattleGroundId(m_host->GetInstanceID(), BATTLEGROUND_BR);
    player->SetBGTeam(TEAM_NONE);

    if (!isBot)
        ChatHandler(player).PSendSysMessage("[孤胆称雄] 号角已响，你将空降入场。此局已有 %u 名独行者。", m_totalCount);
}

void BattleRoyale::Update(uint32 diff)
{
    Map* map = m_host ? m_host->GetBgMap() : nullptr;

    if (m_status == BattleRoyaleStatus::DEPLOYING)
    {
        UpdateDeploying(diff, map);
        return;
    }

    if (m_status == BattleRoyaleStatus::PREPARING)
    {
        // Drain the marker spawn queue during prep so the circle is fully visible
        // before the game starts — not after.
        if (map)
            m_zone.DrainSpawnQueue(map);

        if (m_prepareTimer <= diff)
            StartRunning();
        else
            m_prepareTimer -= diff;
        return;
    }

    if (m_status == BattleRoyaleStatus::RUNNING)
    {
        m_runningTime += diff;

        if (map)
        {
            uint32 phaseBefore = m_zone.GetPhase();
            m_zone.Update(diff, m_players, map);
            uint32 phaseAfter = m_zone.GetPhase();
            if (phaseAfter != phaseBefore)
                BroadcastPhaseChange(phaseAfter);
        }

        // Check for player deaths and re-enforce FFA.
        // Player::UpdateArea() clears PLAYER_FLAGS_FFA_PVP on every sub-zone change (AB has
        // several sub-zones), so we re-apply it each update if it was cleared.
        if (map)
        {
            std::vector<ObjectGuid> toEliminate;
            for (auto it = m_players.begin(); it != m_players.end(); ++it)
            {
                if (!it->second.alive)
                    continue;
                Player* player = map->GetPlayer(it->first);
                if (!player || !player->IsAlive())
                {
                    toEliminate.push_back(it->first);
                    continue;
                }
                if (!player->IsFFAPvP())
                    player->SetFFAPvP(true);
            }
            for (ObjectGuid const& guid : toEliminate)
                Eliminate(guid);
        }
        return;
    }

    if (m_status == BattleRoyaleStatus::FINISHED)
    {
        if (m_finishTimer <= diff)
        {
            if (map)
            {
                for (auto it = m_players.begin(); it != m_players.end(); ++it)
                {
                    Player* player = map->GetPlayer(it->first);
                    if (player)
                        ReturnPlayer(player, it->second);
                }
            }
            m_status = BattleRoyaleStatus::CANCELLED;
        }
        else
            m_finishTimer -= diff;
    }
}

void BattleRoyale::OnPlayerDied(ObjectGuid victim, ObjectGuid killer)
{
    auto it = m_players.find(victim);
    if (it != m_players.end() && it->second.alive)
        Eliminate(victim, true, killer);
}

void BattleRoyale::OnPlayerLeftMap(ObjectGuid guid)
{
    auto it = m_players.find(guid);
    if (it == m_players.end())
        return;

    // If the player leaves during deployment (e.g. disconnect), count them as landed
    // so the remaining players are not stuck waiting forever.
    if (m_status == BattleRoyaleStatus::DEPLOYING && !it->second.landed)
    {
        it->second.landed = true;
        ++m_landedCount;
    }

    if (it->second.alive)
        Eliminate(guid, false);
}

uint32 BattleRoyale::GetAliveCount() const
{
    return m_aliveCount;
}

bool BattleRoyale::IsAlive(ObjectGuid guid) const
{
    auto it = m_players.find(guid);
    return it != m_players.end() && it->second.alive;
}

void BattleRoyale::ForceSetPhase(uint32 phase)
{
    m_zone.ForcePhase(phase);
    if (Map* map = m_host ? m_host->GetBgMap() : nullptr)
        m_zone.RefreshMarkers(map);
}

void BattleRoyale::ForceSetRadius(float radius)
{
    m_zone.ForceRadius(radius);
    if (Map* map = m_host ? m_host->GetBgMap() : nullptr)
        m_zone.RefreshMarkers(map);
}

// --- private ---

void BattleRoyale::UpdateDeploying(uint32 diff, Map* map)
{
    if (map)
    {
        for (auto it = m_players.begin(); it != m_players.end(); ++it)
        {
            BattleRoyalePlayer& brPlayer = it->second;
            if (brPlayer.landed)
                continue;

            Player* player = map->GetPlayer(it->first);
            if (!player)
                continue;

            if (brPlayer.deploymentStartDelayTimer > 0)
            {
                if (brPlayer.deploymentStartDelayTimer <= diff)
                    brPlayer.deploymentStartDelayTimer = 0;
                else
                    brPlayer.deploymentStartDelayTimer -= diff;
                continue;
            }

            if (brPlayer.deploymentStarted)
            {
                if (!player->IsTaxiFlying())
                    CompleteDeployment(player, brPlayer, false);
                continue;
            }

            if (!brPlayer.deploymentPathId)
            {
                CompleteDeployment(player, brPlayer, true);
                continue;
            }

            std::string error;
            if (sCustomTaxiMgr.Play(player, brPlayer.deploymentPathId, error))
            {
                brPlayer.deploymentStarted = true;
                ChatHandler(player).PSendSysMessage("[孤胆称雄] 风声掠过耳畔，空降开始。落地后，只有自己可信。");
                sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
                         "[BattleRoyale] Deployment started player %s path %u instance %u.",
                         player->GetName(), brPlayer.deploymentPathId,
                         m_host ? m_host->GetInstanceID() : 0u);
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                         "[BattleRoyale] Deployment path %u failed for player %s: %s. Falling back to landing point.",
                         brPlayer.deploymentPathId, player->GetName(), error.c_str());
                CompleteDeployment(player, brPlayer, true);
            }
        }
    }

    if (m_landedCount >= m_totalCount && m_pendingBotCount == 0)
    {
        StartPreparing();
        return;
    }

    if (m_deploymentTimer <= diff)
        StartPreparing();
    else
        m_deploymentTimer -= diff;
}

void BattleRoyale::CompleteDeployment(Player* player, BattleRoyalePlayer& brPlayer, bool teleportToLandingPoint)
{
    if (brPlayer.landed)
        return;

    if (player && teleportToLandingPoint)
    {
        if (player->IsTaxiFlying())
        {
            player->GetMotionMaster()->MovementExpired();
            player->GetTaxi().ClearTaxiDestinations();
        }

        BRSpawnPoint const& landing = brPlayer.landingPoint;
        float landZ = landing.z;
        // Bots have no client-side physics. Snap their Z to actual terrain so they
        // don't float at spawn points that are slightly above the ground mesh.
        if (brPlayer.bot)
        {
            Map* bgMap = m_host ? m_host->GetBgMap() : nullptr;
            if (bgMap)
            {
                float groundZ = bgMap->GetHeight(landing.x, landing.y, MAX_HEIGHT, false, MAX_HEIGHT);
                if (groundZ > INVALID_HEIGHT)
                    landZ = groundZ;
            }
        }
        player->TeleportTo(m_tmpl->mapId, landing.x, landing.y, landZ, landing.o);
    }
    else if (player)
    {
        // Keep DB spawn points authoritative. If an old or hand-written route ends away
        // from the assigned spawn point, snap the participant to the recorded landing.
        // Bots also need a terrain-Z snap because they do not fall via client physics.
        BRSpawnPoint const& landing = brPlayer.landingPoint;
        float landZ = landing.z;
        Map* bgMap = m_host ? m_host->GetBgMap() : nullptr;
        if (brPlayer.bot && bgMap)
        {
            float groundZ = bgMap->GetHeight(landing.x, landing.y, MAX_HEIGHT, false, MAX_HEIGHT);
            if (groundZ > INVALID_HEIGHT)
                landZ = groundZ;
        }

        bool const farFromLanding = player->GetDistance(landing.x, landing.y, landZ) > BR_LANDING_CORRECTION_DISTANCE;
        bool const botFloating = brPlayer.bot && player->GetPositionZ() > landZ + 0.5f;
        if (farFromLanding || botFloating)
            player->TeleportTo(m_tmpl->mapId, landing.x, landing.y, landZ, landing.o);
    }

    if (player && player->IsMounted())
        player->Unmount();

    brPlayer.deploymentStarted = false;
    brPlayer.landed = true;
    ++m_landedCount;
}

void BattleRoyale::StartPreparing()
{
    if (m_status != BattleRoyaleStatus::DEPLOYING)
        return;

    m_status = BattleRoyaleStatus::PREPARING;
    m_prepareTimer = 30000;

    // Re-apply FFA PvP now that players are on the BG map.
    // Player::UpdateArea() clears the flag when entering a non-arena area,
    // so we must re-set it after teleport completes.
    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    if (map)
    {
        for (auto it = m_players.begin(); it != m_players.end(); ++it)
        {
            if (!it->second.alive)
                continue;
            if (Player* player = map->GetPlayer(it->first))
                player->SetFFAPvP(true);
        }
    }

    // Spawn zone markers now so players can see the boundary during prep
    if (map)
        m_zone.RefreshMarkers(map);

    BroadcastToAll("[孤胆称雄] 众人已落地，猎场即将苏醒。30 秒后解除庇护。");
}

void BattleRoyale::StartRunning()
{
    m_status = BattleRoyaleStatus::RUNNING;

    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        if (!it->second.alive)
            continue;
        Player* player = map ? map->GetPlayer(it->first) : nullptr;
        if (!player)
            continue;
        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
        ChatHandler(player).PSendSysMessage("[孤胆称雄] 庇护散去，猎场开战！活到最后，方可称雄。");
    }

    // Announce initial zone damage so players know the starting pressure.
    BroadcastPhaseChange(0);

    SpawnChests(map);
}

void BattleRoyale::Eliminate(ObjectGuid guid, bool notify, ObjectGuid killerGuid)
{
    auto it = m_players.find(guid);
    if (it == m_players.end() || !it->second.alive)
        return;

    it->second.alive         = false;
    it->second.placementRank = m_aliveCount;
    --m_aliveCount;

    uint32 const survivalSec = m_runningTime / 1000;

    BRRankEntry entry;
    entry.guid        = guid;
    entry.rank        = it->second.placementRank;
    entry.survivalSec = survivalSec;
    m_ranks.push_back(entry);

    // Credit kill to the killer (works for both real players and bots)
    if (killerGuid && killerGuid != guid)
    {
        auto killerIt = m_players.find(killerGuid);
        if (killerIt != m_players.end())
            ++killerIt->second.killCount;
    }

    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    Player* player = map ? map->GetPlayer(guid) : nullptr;

    if (player)
        player->SendMirrorTimerStop(MirrorTimer::FATIGUE);

    if (notify && player)
        ChatHandler(player).PSendSysMessage("[孤胆称雄] 你的征途止步于此，最终排名第 %u。", it->second.placementRank);

    // Broadcast to survivors
    {
        char buf[192];
        std::string victimName = player ? player->GetName() : "一名试炼者";
        std::string victimTitle = it->second.bot ? "机器人" : "玩家";

        if (killerGuid && killerGuid != guid)
        {
            Player* killer = map ? map->GetPlayer(killerGuid) : nullptr;
            auto killerRecord = m_players.find(killerGuid);
            bool const killerIsBot = killerRecord != m_players.end() && killerRecord->second.bot;
            std::string killerName = killer ? killer->GetName() : "未知猎手";
            std::string killerTitle = killerIsBot ? "机器人" : "玩家";
            snprintf(buf, sizeof(buf), "[孤胆称雄] %s %s 被 %s %s 击倒，猎场还剩 %u 人。",
                     victimTitle.c_str(), victimName.c_str(), killerTitle.c_str(), killerName.c_str(), m_aliveCount);
        }
        else if (!notify)
        {
            snprintf(buf, sizeof(buf), "[孤胆称雄] %s %s 离开猎场，剩余 %u 名独行者。",
                     victimTitle.c_str(), victimName.c_str(), m_aliveCount);
        }
        else
        {
            snprintf(buf, sizeof(buf), "[孤胆称雄] %s %s 倒在毒圈边缘，猎场还剩 %u 人。",
                     victimTitle.c_str(), victimName.c_str(), m_aliveCount);
        }
        BroadcastToAll(buf);
    }

    // Send battle report mail before returning the player
    if (!it->second.bot)
        SendBattleReport(guid, it->second, survivalSec);

    // MVP: return player immediately (no spectating)
    if (player)
    {
        ReturnPlayer(player, it->second);
        it->second.outsideZone = false;
    }

    // Win condition
    if (m_aliveCount <= 1)
    {
        ObjectGuid winnerGuid;
        for (auto jt = m_players.begin(); jt != m_players.end(); ++jt)
        {
            if (jt->second.alive)
            {
                winnerGuid = jt->first;
                break;
            }
        }

        if (map && winnerGuid)
        {
            Player* winner = map->GetPlayer(winnerGuid);
            if (winner)
                ChatHandler(winner).PSendSysMessage("[孤胆称雄] 冠军诞生！你独自站到最后，猎场记住了你的名字。");
        }

        Finish();
    }
}

void BattleRoyale::Finish()
{
    // Guard against double-call (can happen if two players die in the same Update frame)
    if (m_status == BattleRoyaleStatus::FINISHED || m_status == BattleRoyaleStatus::CANCELLED)
        return;

    // Assign rank 1 to the last survivor and send battle report
    uint32 const finishSurvivalSec = m_runningTime / 1000;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        if (!it->second.alive)
            continue;
        it->second.placementRank = 1;
        if (!it->second.bot)
            SendBattleReport(it->first, it->second, finishSurvivalSec);
    }

    m_status      = BattleRoyaleStatus::FINISHED;
    m_finishTimer = BR_FINISH_DELAY_MS;
    m_zone.Cleanup(m_host ? m_host->GetBgMap() : nullptr);
    CleanupChests(m_host ? m_host->GetBgMap() : nullptr);
    BroadcastToAll("[孤胆称雄] 尘埃落定，孤胆之战结束。10 秒后送回原地。");

    sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[BattleRoyale] Instance %u finished. Total players: %u",
             m_host ? m_host->GetInstanceID() : 0u, m_totalCount);
}

void BattleRoyale::Cancel()
{
    m_status = BattleRoyaleStatus::CANCELLED;
    m_zone.Cleanup(m_host ? m_host->GetBgMap() : nullptr);
    CleanupChests(m_host ? m_host->GetBgMap() : nullptr);

    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        Player* player = map ? map->GetPlayer(it->first) : nullptr;
        if (player)
        {
            player->SendMirrorTimerStop(MirrorTimer::FATIGUE);
            player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);
            ReturnPlayer(player, it->second);
        }
    }
}

void BattleRoyale::ReturnPlayer(Player* player, BattleRoyalePlayer const& brPlayer)
{
    if (player->IsTaxiFlying())
    {
        player->GetMotionMaster()->MovementExpired();
        player->GetTaxi().ClearTaxiDestinations();
    }

    CleanupBRItems(player);

    player->SetFFAPvP(brPlayer.savedFFAPvP);
    player->SetBGTeam(TEAM_NONE);
    player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
    player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_IMMUNE_TO_NPC);

    if (brPlayer.bot)
    {
        // Bots are removed from the world when the session is cleaned up
        if (PlayerBotEntry* entry = player->GetSession() ? player->GetSession()->GetBot() : nullptr)
            entry->requestRemoval = true;
        return;
    }

    WorldLocation const& pos = brPlayer.savedPosition;
    player->TeleportTo(pos.mapId, pos.x, pos.y, pos.z, pos.o);
}

void BattleRoyale::BroadcastPhaseChange(uint32 phase)
{
    uint32 totalPhases = m_tmpl ? uint32(m_tmpl->phases.size()) : 0;
    bool isFinal = (totalPhases > 0 && phase == totalPhases - 1 &&
                    m_tmpl->phases[phase].durationMs == 0);

    char buf[128];
    if (isFinal)
    {
        snprintf(buf, sizeof(buf), "[孤胆称雄] 决赛圈已形成！圈外伤害 %.0f%%/秒，快速回圈！",
                 m_zone.GetCurrentDamagePercent());
    }
    else
    {
        snprintf(buf, sizeof(buf), "[孤胆称雄] 毒圈进入第 %u 阶段，圈外伤害 %.0f%%/秒！",
                 phase + 1, m_zone.GetCurrentDamagePercent());
    }
    BroadcastToAll(buf);
}

void BattleRoyale::SpawnChests(Map* map)
{
    if (!map || !m_tmpl || m_tmpl->commonChestPoints.empty())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
                 "[BattleRoyale] SpawnChests skipped: map=%s tmpl=%s points=%u",
                 map ? "ok" : "null",
                 m_tmpl ? "ok" : "null",
                 m_tmpl ? uint32(m_tmpl->commonChestPoints.size()) : 0u);
        BroadcastToAll("[孤胆称雄] 本局没有补给箱投放，猎场只留下脚步与刀锋。");
        return;
    }

    for (BRSpawnPoint const& pos : m_tmpl->commonChestPoints)
    {
        GameObject* go = new GameObject;
        if (!go->Create(map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT),
                        BR_COMMON_CHEST_ENTRY, map,
                        pos.x, pos.y, pos.z, pos.o,
                        0.0f, 0.0f, 0.0f, 0.0f,
                        GO_ANIMPROGRESS_DEFAULT, GO_STATE_READY))
        {
            delete go;
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                     "[BattleRoyale] Failed to spawn common chest at %.1f,%.1f,%.1f",
                     pos.x, pos.y, pos.z);
            continue;
        }
        go->SetRespawnTime(0);
        // Set loot state to GO_READY before Add() so the initial object update
        // packet sent to nearby clients already carries the interactable DYN_FLAGS.
        // Without this, the chest spawns as GO_NOT_READY; the server transitions it
        // to GO_READY on the next tick but never sends a DYN_FLAGS update, leaving
        // the client permanently showing the chest as non-interactable.
        go->SetLootState(GO_READY);
        map->Add(go);
        m_chestGuids.push_back(go->GetObjectGuid());
    }
    uint32 const spawnedCount = uint32(m_chestGuids.size());
    uint32 const totalPoints  = uint32(m_tmpl->commonChestPoints.size());
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
             "[BattleRoyale] Spawned %u / %u common chests for instance %u.",
             spawnedCount, totalPoints, m_host ? m_host->GetInstanceID() : 0u);
    char broadcastBuf[128];
    snprintf(broadcastBuf, sizeof(broadcastBuf), "[孤胆称雄] %u 个补给箱已散落猎场，先到者先得。", spawnedCount);
    BroadcastToAll(broadcastBuf);
}

void BattleRoyale::CleanupChests(Map* map)
{
    if (map)
    {
        for (ObjectGuid const& guid : m_chestGuids)
        {
            if (GameObject* go = map->GetGameObject(guid))
            {
                go->SetRespawnTime(0);
                go->Delete();
            }
        }
    }
    m_chestGuids.clear();
}

void BattleRoyale::CleanupBRItems(Player* player)
{
    for (uint32 entry : BR_ITEM_ENTRIES)
        player->DestroyItemCount(entry, 200, true, false);
}

void BattleRoyale::SendBattleReport(ObjectGuid playerGuid, BattleRoyalePlayer const& brPlayer, uint32 survivalSec) const
{
    uint32 const rank  = brPlayer.placementRank;
    uint32 const kills = brPlayer.killCount;
    uint32 const total = m_totalCount;
    uint32 const mm    = survivalSec / 60;
    uint32 const ss    = survivalSec % 60;
    char const* closing = rank == 1
        ? "你是最后的存活者。此战过后，孤胆称雄。"
        : "下一次落地，猎场仍会等待新的名字。";

    char body[768];
    snprintf(body, sizeof(body),
             "孤胆称雄战报\n\n"
             "猎场已经沉寂，属于你的这一局写入战册。\n\n"
             "最终名次：第 %u 名 / 共 %u 人\n"
             "击倒对手：%u 人\n"
             "存活时间：%02u:%02u\n\n"
             "%s",
             rank, total, kills, mm, ss, closing);

    const char* subject = (rank == 1) ? "「孤胆称雄」冠军战报" : "「孤胆称雄」猎场战报";

    MailDraft(subject, std::string(body))
        .SendMailTo(MailReceiver(playerGuid),
                    MailSender(MAIL_NORMAL, uint32(0), MAIL_STATIONERY_DEFAULT));
}

void BattleRoyale::BroadcastToAll(std::string const& msg)
{
    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    if (!map)
        return;
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        Player* player = map->GetPlayer(it->first);
        if (player)
            ChatHandler(player).PSendSysMessage("%s", msg.c_str());
    }
}
