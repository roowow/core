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

#include <cstdio>

static uint32 const BR_FINISH_DELAY_MS = 10000;

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
        ChatHandler(player).PSendSysMessage("[Battle Royale] 欢迎！正在准备空降，共 %u 名参与者。", m_totalCount);
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

void BattleRoyale::OnPlayerDied(ObjectGuid guid)
{
    auto it = m_players.find(guid);
    if (it != m_players.end() && it->second.alive)
        Eliminate(guid);
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
                ChatHandler(player).PSendSysMessage("[Battle Royale] 空降开始。");
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
    else if (player && brPlayer.bot)
    {
        // Bot landed via taxi normally (teleportToLandingPoint = false).
        // The taxi endpoint may be slightly above the server terrain mesh — real players
        // fall via client physics, bots don't. Snap to actual ground here.
        Map* bgMap = m_host ? m_host->GetBgMap() : nullptr;
        if (bgMap)
        {
            float gx = player->GetPositionX();
            float gy = player->GetPositionY();
            float gz = player->GetPositionZ();
            float groundZ = bgMap->GetHeight(gx, gy, MAX_HEIGHT, false, MAX_HEIGHT);
            if (groundZ > INVALID_HEIGHT && gz > groundZ + 0.5f)
                player->TeleportTo(m_tmpl->mapId, gx, gy, groundZ, player->GetOrientation());
        }
    }

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

    BroadcastToAll("[Battle Royale] 空降完成！30 秒后对局开始。");
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
        ChatHandler(player).PSendSysMessage("[Battle Royale] 对局开始！最后存活者获胜！");
    }

    // Announce initial zone damage so players know the starting pressure.
    BroadcastPhaseChange(0);
}

void BattleRoyale::Eliminate(ObjectGuid guid, bool notify)
{
    auto it = m_players.find(guid);
    if (it == m_players.end() || !it->second.alive)
        return;

    it->second.alive         = false;
    it->second.placementRank = m_aliveCount;
    --m_aliveCount;

    BRRankEntry entry;
    entry.guid        = guid;
    entry.rank        = it->second.placementRank;
    entry.survivalSec = m_runningTime / 1000;
    m_ranks.push_back(entry);

    Map* map = m_host ? m_host->GetBgMap() : nullptr;
    Player* player = map ? map->GetPlayer(guid) : nullptr;

    if (player)
        player->SendMirrorTimerStop(MirrorTimer::FATIGUE);

    if (notify && player)
        ChatHandler(player).PSendSysMessage("[Battle Royale] 你已被淘汰！排名第 %u。", it->second.placementRank);

    // Broadcast to survivors
    {
        char buf[128];
        std::string name = player ? player->GetName() : "玩家";
        snprintf(buf, sizeof(buf), "[Battle Royale] %s 已被淘汰，剩余 %u 名存活者。", name.c_str(), m_aliveCount);
        BroadcastToAll(buf);
    }

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
                ChatHandler(winner).PSendSysMessage("[Battle Royale] 恭喜！你是最后的存活者！");
        }

        Finish();
    }
}

void BattleRoyale::Finish()
{
    // Guard against double-call (can happen if two players die in the same Update frame)
    if (m_status == BattleRoyaleStatus::FINISHED || m_status == BattleRoyaleStatus::CANCELLED)
        return;

    // Assign rank 1 to the last survivor
    for (auto it = m_players.begin(); it != m_players.end(); ++it)
    {
        if (it->second.alive)
            it->second.placementRank = 1;
    }

    m_status      = BattleRoyaleStatus::FINISHED;
    m_finishTimer = BR_FINISH_DELAY_MS;
    m_zone.Cleanup(m_host ? m_host->GetBgMap() : nullptr);
    BroadcastToAll("[Battle Royale] 对局结束！10 秒后返回原位置。");

    sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "[BattleRoyale] Instance %u finished. Total players: %u",
             m_host ? m_host->GetInstanceID() : 0u, m_totalCount);
}

void BattleRoyale::Cancel()
{
    m_status = BattleRoyaleStatus::CANCELLED;
    m_zone.Cleanup(m_host ? m_host->GetBgMap() : nullptr);

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
        snprintf(buf, sizeof(buf), "[Battle Royale] 决赛圈已形成！圈外伤害 %.0f%%/秒，快速回圈！",
                 m_zone.GetCurrentDamagePercent());
    }
    else
    {
        snprintf(buf, sizeof(buf), "[Battle Royale] 毒圈进入第 %u 阶段，圈外伤害 %.0f%%/秒！",
                 phase + 1, m_zone.GetCurrentDamagePercent());
    }
    BroadcastToAll(buf);
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
