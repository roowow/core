/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "BattleGroundAfkMgr.h"
#include "BattleGround.h"
#include "BattleGroundAB.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"

#include <algorithm>
#include <ctime>
#include <vector>

namespace
{
uint32 const AFK_CHECK_INTERVAL = 30 * IN_MILLISECONDS;
uint32 const AFK_WARNING_COOLDOWN = 5 * MINUTE * IN_MILLISECONDS;
uint32 const AFK_RECOVERY_NOTICE_COOLDOWN = 2 * MINUTE * IN_MILLISECONDS;
float const AFK_MIN_MOVE_DISTANCE = 5.0f;
float const AFK_AB_OBJECTIVE_RADIUS = 60.0f;
float const AFK_AB_START_RADIUS = 70.0f;
int32 const AFK_AB_OBJECTIVE_SCORE_REDUCE = 3;
int32 const AFK_AB_START_IDLE_SCORE = 4;
uint32 const AFK_EFFECTIVE_DAMAGE_THRESHOLD = 100;
uint32 const AFK_EFFECTIVE_HEALING_THRESHOLD = 100;
int32 const AFK_DAMAGE_DONE_SCORE_REDUCE = 3;
int32 const AFK_DAMAGE_TAKEN_SCORE_REDUCE = 2;
int32 const AFK_HEALING_DONE_SCORE_REDUCE = 3;
int32 const AFK_OBJECTIVE_SCORE_REDUCE = 6;
uint32 const AFK_DEAD_GRACE_CHECKS = 4;
int32 const AFK_DEAD_IDLE_SCORE = 2;

int32 ClampAfkScore(int32 score)
{
    return std::max<int32>(0, std::min<int32>(100, score));
}

float GetDistanceSq(float x1, float y1, float z1, float x2, float y2, float z2)
{
    float const dx = x1 - x2;
    float const dy = y1 - y2;
    float const dz = z1 - z2;
    return dx * dx + dy * dy + dz * dz;
}

bool IsNearPoint(Player const* player, float x, float y, float z, float radius)
{
    return GetDistanceSq(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z) <= radius * radius;
}

bool IsNearABObjective(Player const* player)
{
    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
        if (IsNearPoint(player, BG_AB_BuffPositions[i][0], BG_AB_BuffPositions[i][1], BG_AB_BuffPositions[i][2], AFK_AB_OBJECTIVE_RADIUS))
            return true;

    return false;
}

bool IsNearTeamStart(BattleGround* bg, Player* player, ObjectGuid guid, float radius)
{
    Team team = bg->GetPlayerTeam(guid);
    if (!team)
        team = player->GetBGTeam();

    float x, y, z, o;
    bg->GetTeamStartLoc(team, x, y, z, o);
    return IsNearPoint(player, x, y, z, radius);
}
}

void BattleGroundAfkMgr::Update(BattleGround* bg, uint32 diff)
{
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
    {
        m_updateTimer = 0;
        return;
    }

    BattleGroundTypeId const bgType = bg->GetTypeID();
    if (bgType != BATTLEGROUND_WS && bgType != BATTLEGROUND_AB && bgType != BATTLEGROUND_AV)
    {
        m_updateTimer = 0;
        return;
    }

    m_elapsedTime += diff;
    m_updateTimer += diff;
    if (m_updateTimer < AFK_CHECK_INTERVAL)
        return;

    m_updateTimer = 0;

    std::vector<ObjectGuid> activePlayers;
    activePlayers.reserve(bg->GetPlayers().size());
    for (BattleGround::BattleGroundPlayerMap::const_iterator itr = bg->GetPlayers().begin(); itr != bg->GetPlayers().end(); ++itr)
        activePlayers.push_back(itr->first);

    for (ObjectGuid const& guid : activePlayers)
        UpdatePlayer(bg, guid);

    for (std::map<ObjectGuid, BattleGroundAfkPlayerState>::iterator itr = m_playerStates.begin(); itr != m_playerStates.end();)
    {
        if (bg->GetPlayers().find(itr->first) == bg->GetPlayers().end())
            m_playerStates.erase(itr++);
        else
            ++itr;
    }
}

void BattleGroundAfkMgr::RemovePlayer(ObjectGuid guid)
{
    m_playerStates.erase(guid);
}

void BattleGroundAfkMgr::Reset()
{
    m_playerStates.clear();
    m_updateTimer = 0;
    m_elapsedTime = 0;
}

void BattleGroundAfkMgr::RecordDamageDone(ObjectGuid guid, uint32 amount)
{
    if (!amount)
        return;

    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    state.recentDamageDone = std::min<uint32>(state.recentDamageDone + amount, 1000000);
}

void BattleGroundAfkMgr::RecordDamageTaken(ObjectGuid guid, uint32 amount)
{
    if (!amount)
        return;

    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    state.recentDamageTaken = std::min<uint32>(state.recentDamageTaken + amount, 1000000);
}

void BattleGroundAfkMgr::RecordHealingDone(ObjectGuid guid, uint32 amount)
{
    if (!amount)
        return;

    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    state.recentHealingDone = std::min<uint32>(state.recentHealingDone + amount, 1000000);
}

void BattleGroundAfkMgr::RecordObjective(ObjectGuid guid)
{
    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    state.recentObjectiveEvents = std::min<uint32>(state.recentObjectiveEvents + 1, 1000);
}

BattleGroundAfkScoreRule BattleGroundAfkMgr::GetRule(BattleGround const* bg) const
{
    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_WS:
            return { 8, 13, 18, 24, 3, 3, 2, 1, 5 };
        case BATTLEGROUND_AB:
            return { 10, 16, 22, 30, 2, 2, 1, 1, 5 };
        case BATTLEGROUND_AV:
            return { 14, 22, 30, 40, 2, 2, 1, 1, 4 };
        default:
            return { 10, 16, 22, 30, 2, 2, 1, 1, 5 };
    }
}

void BattleGroundAfkMgr::UpdatePlayer(BattleGround* bg, ObjectGuid guid)
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player || player->IsBot() || player->IsGameMaster() || player->GetBattleGround() != bg)
    {
        RemovePlayer(guid);
        return;
    }

    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    if (!state.initialized)
    {
        state.initialized = true;
        state.lastX = player->GetPositionX();
        state.lastY = player->GetPositionY();
        state.lastZ = player->GetPositionZ();
        return;
    }

    if (!player->IsAlive())
    {
        BattleGroundAfkScoreRule const rule = GetRule(bg);
        uint32 const previousScore = state.score;

        ++state.deadChecks;
        if (state.deadChecks > AFK_DEAD_GRACE_CHECKS)
            state.score = uint32(ClampAfkScore(int32(state.score) + AFK_DEAD_IDLE_SCORE));

        state.lastX = player->GetPositionX();
        state.lastY = player->GetPositionY();
        state.lastZ = player->GetPositionZ();
        state.recentDamageDone = 0;
        state.recentDamageTaken = 0;
        state.recentHealingDone = 0;
        state.recentObjectiveEvents = 0;
        ApplyStage(bg, guid, state, rule, previousScore);
        return;
    }

    state.deadChecks = 0;

    if (state.graceChecks)
    {
        --state.graceChecks;
        state.lastX = player->GetPositionX();
        state.lastY = player->GetPositionY();
        state.lastZ = player->GetPositionZ();
        return;
    }

    BattleGroundAfkScoreRule const rule = GetRule(bg);
    uint32 const previousScore = state.score;
    int32 score = int32(state.score);
    float const movedDistanceSq = GetDistanceSq(state.lastX, state.lastY, state.lastZ, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    bool const moved = movedDistanceSq >= AFK_MIN_MOVE_DISTANCE * AFK_MIN_MOVE_DISTANCE;
    bool const inCombat = player->IsInCombat();

    if (!moved)
        score += rule.noMovementScore;
    else
        score -= rule.movementReduce;

    if (!inCombat)
        score += rule.noCombatScore;
    else
        score -= rule.combatReduce;

    if (!moved && !inCombat)
        score += rule.noContributionScore;

    if (state.recentDamageDone >= AFK_EFFECTIVE_DAMAGE_THRESHOLD)
        score -= AFK_DAMAGE_DONE_SCORE_REDUCE;

    if (state.recentDamageTaken >= AFK_EFFECTIVE_DAMAGE_THRESHOLD)
        score -= AFK_DAMAGE_TAKEN_SCORE_REDUCE;

    if (state.recentHealingDone >= AFK_EFFECTIVE_HEALING_THRESHOLD)
        score -= AFK_HEALING_DONE_SCORE_REDUCE;

    if (state.recentObjectiveEvents)
        score -= AFK_OBJECTIVE_SCORE_REDUCE;

    if (bg->GetTypeID() == BATTLEGROUND_AB)
    {
        if (IsNearABObjective(player))
            score -= AFK_AB_OBJECTIVE_SCORE_REDUCE;

        if (!moved && !inCombat && IsNearTeamStart(bg, player, guid, AFK_AB_START_RADIUS))
            score += AFK_AB_START_IDLE_SCORE;
    }

    state.recentDamageDone = 0;
    state.recentDamageTaken = 0;
    state.recentHealingDone = 0;
    state.recentObjectiveEvents = 0;

    state.score = uint32(ClampAfkScore(score));
    state.lastX = player->GetPositionX();
    state.lastY = player->GetPositionY();
    state.lastZ = player->GetPositionZ();

    ApplyStage(bg, guid, state, rule, previousScore);
}

void BattleGroundAfkMgr::ApplyStage(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState& state, BattleGroundAfkScoreRule const& rule, uint32 previousScore)
{
    uint8 targetStage = 0;
    if (state.score >= rule.warning3Score)
        targetStage = 3;
    else if (state.score >= rule.warning2Score)
        targetStage = 2;
    else if (state.score >= rule.warning1Score)
        targetStage = 1;

    bool const cooldownReady = !state.lastWarnTime || m_elapsedTime >= state.lastWarnTime + AFK_WARNING_COOLDOWN;
    bool const scoreDecreasing = state.score < previousScore;
    bool const recoveryNoticeReady = !state.lastRecoveryNoticeTime || m_elapsedTime >= state.lastRecoveryNoticeTime + AFK_RECOVERY_NOTICE_COOLDOWN;

    if (state.score < rule.warning1Score)
    {
        if (state.stage > 0)
        {
            state.stage = 0;
            state.lastWarnTime = 0;
            state.lastRecoveryNoticeTime = 0;
            SendRecoveryNotice(bg, guid, state.score, true);
            return;
        }

        state.stage = 0;
    }
    else if (targetStage < state.stage)
        state.stage = targetStage;

    if (state.stage > 0 && targetStage > state.stage && scoreDecreasing)
    {
        if (recoveryNoticeReady)
        {
            state.lastRecoveryNoticeTime = m_elapsedTime;
            SendRecoveryNotice(bg, guid, state.score, false);
        }
        return;
    }

    if (targetStage > state.stage && cooldownReady)
    {
        state.stage = state.stage + 1;
        state.lastWarnTime = m_elapsedTime;
        SendWarning(bg, guid, state.stage, state.score);
        return;
    }

    if (state.score >= rule.kickScore && state.stage >= 3 && cooldownReady)
    {
        if (scoreDecreasing)
        {
            if (recoveryNoticeReady)
            {
                state.lastRecoveryNoticeTime = m_elapsedTime;
                SendRecoveryNotice(bg, guid, state.score, false);
            }
            return;
        }

        if (Player* player = sObjectMgr.GetPlayer(guid))
        {
            player->SendSysMessage("您因长时间未有效参与战场，已被移出战场。");
            player->LeaveBattleground();
        }
        return;
    }
}

void BattleGroundAfkMgr::SendWarning(BattleGround* bg, ObjectGuid guid, uint8 stage, uint32 score) const
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;

    time_t const now = time(nullptr);
    tm const* localTime = localtime(&now);
    char currentTime[6];
    snprintf(currentTime, sizeof(currentTime), "%02u:%02u", uint32(localTime->tm_hour), uint32(localTime->tm_min));

    sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] warning stage %u player %s (%s) bgType %u instance %u score %u.",
        uint32(stage), player->GetName(), guid.GetString().c_str(), bg ? uint32(bg->GetTypeID()) : 0, bg ? bg->GetInstanceID() : 0, score);

    switch (stage)
    {
        case 1:
            player->PSendSysMessage("[%s] 战场挂机警告 1：您在战场中的有效活动较少。", currentTime);
            break;
        case 2:
            player->PSendSysMessage("[%s] 战场挂机警告 2：请您参与战斗或战场目标，否则将被移出。", currentTime);
            break;
        case 3:
            player->PSendSysMessage("[%s] 战场挂机警告 3：最后警告，请您立即参与战场。", currentTime);
            break;
        default:
            break;
    }
}

void BattleGroundAfkMgr::SendRecoveryNotice(BattleGround* bg, ObjectGuid guid, uint32 score, bool normal) const
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;

    time_t const now = time(nullptr);
    tm const* localTime = localtime(&now);
    char currentTime[6];
    snprintf(currentTime, sizeof(currentTime), "%02u:%02u", uint32(localTime->tm_hour), uint32(localTime->tm_min));

    sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] recovery %s player %s (%s) bgType %u instance %u score %u.",
        normal ? "normal" : "pending", player->GetName(), guid.GetString().c_str(), bg ? uint32(bg->GetTypeID()) : 0, bg ? bg->GetInstanceID() : 0, score);

    if (normal)
        player->PSendSysMessage("[%s] 您的战场活动已恢复正常。", currentTime);
    else
        player->PSendSysMessage("[%s] 您的战场活动正在恢复，请持续参与战斗或战场目标，以免被移出。", currentTime);
}
