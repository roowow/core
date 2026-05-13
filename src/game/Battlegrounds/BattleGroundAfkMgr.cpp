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
#include "BattleGroundAV.h"
#include "BattleGroundWS.h"
#include "DBCStores.h"
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
uint32 const AFK_ACTIVITY_NOTICE_NORMAL_COOLDOWN = 10 * MINUTE * IN_MILLISECONDS;
uint32 const AFK_ACTIVITY_NOTICE_LOW_COOLDOWN = 3 * MINUTE * IN_MILLISECONDS;
float const AFK_MIN_MOVE_DISTANCE = 5.0f;
float const AFK_AB_OBJECTIVE_RADIUS = 60.0f;
float const AFK_AB_START_RADIUS = 70.0f;
int32 const AFK_AB_OBJECTIVE_SCORE_REDUCE = 3;
int32 const AFK_AB_START_IDLE_SCORE = 4;
float const AFK_WSG_OBJECTIVE_RADIUS = 70.0f;
int32 const AFK_WSG_OBJECTIVE_SCORE_REDUCE = 4;
float const AFK_AV_OBJECTIVE_RADIUS = 65.0f;
int32 const AFK_AV_OBJECTIVE_SCORE_REDUCE = 3;
uint32 const AFK_NO_OBJECTIVE_GRACE_CHECKS = 3;
int32 const AFK_NO_OBJECTIVE_CHAIN_SCORE = 2;
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

bool IsNearUnit(Player const* player, Unit const* target, float radius)
{
    return target && player->GetMapId() == target->GetMapId() &&
        IsNearPoint(player, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(), radius);
}

bool IsWSGFlagCarrier(Player const* player)
{
    return player && (player->HasAura(BG_WS_SPELL_WARSONG_FLAG) || player->HasAura(BG_WS_SPELL_SILVERWING_FLAG));
}

bool IsNearWSGObjective(BattleGround* bg, Player* player)
{
    BattleGroundWS* wsg = dynamic_cast<BattleGroundWS*>(bg);
    if (!wsg)
        return false;

    if (IsWSGFlagCarrier(player))
        return true;

    if (IsNearUnit(player, sObjectMgr.GetPlayer(wsg->GetAllianceFlagPickerGuid()), AFK_WSG_OBJECTIVE_RADIUS))
        return true;

    if (IsNearUnit(player, sObjectMgr.GetPlayer(wsg->GetHordeFlagPickerGuid()), AFK_WSG_OBJECTIVE_RADIUS))
        return true;

    if (Unit* victim = player->GetVictim())
        if (Player const* victimPlayer = victim->ToPlayer())
            if (IsWSGFlagCarrier(victimPlayer))
                return true;

    return false;
}

struct AfkObjectivePoint
{
    float x;
    float y;
    float z;
};

AfkObjectivePoint const AFK_AV_OBJECTIVE_POINTS[] =
{
    { 640.404f, -32.0183f, 46.2328f },     // Stormpike Aid Station
    { 667.173f, -295.225f, 30.29f },       // Stormpike Graveyard
    { 79.8805f, -401.379f, 46.516f },      // Stonehearth Graveyard
    { -205.464f, -114.337f, 78.6167f },    // Snowfall Graveyard
    { -614.138f, -396.501f, 60.8585f },    // Iceblood Graveyard
    { -1081.32f, -348.198f, 54.5837f },    // Frostwolf Graveyard
    { -1401.94f, -310.103f, 89.3816f },    // Frostwolf Relief Hut
    { 555.018f, -77.9842f, 51.9347f },     // Dun Baldar South Bunker
    { 672.685f, -142.49f, 63.6571f },      // Dun Baldar North Bunker
    { 200.792f, -361.881f, 56.3798f },     // Icewing Bunker
    { -153.491f, -441.386f, 40.3957f },    // Stonehearth Bunker
    { -571.044f, -263.753f, 75.0087f },    // Iceblood Tower
    { -767.9f, -362.019f, 90.8949f },      // Tower Point
    { -1303.38f, -315.877f, 113.867f },    // East Frostwolf Tower
    { -1298.82f, -267.215f, 114.151f },    // West Frostwolf Tower
    { -46.5667f, -294.841f, 15.0786f },    // Captain Balinda
    { -540.747f, -169.935f, 57.0124f },    // Captain Galvangar
    { 717.709f, -16.6861f, 50.1354f },     // Vanndar room
    { -1366.23f, -223.049f, 98.4174f },    // Drek'Thar room
};

bool IsNearAVObjective(Player const* player)
{
    for (uint8 i = BG_AV_NODES_FIRSTAID_STATION; i <= BG_AV_NODES_FROSTWOLF_HUT; ++i)
    {
        if (WorldSafeLocsEntry const* entry = sWorldSafeLocsStore.LookupEntry(BG_AV_GraveyardIds[i]))
            if (IsNearPoint(player, entry->x, entry->y, entry->z, AFK_AV_OBJECTIVE_RADIUS))
                return true;
    }

    for (AfkObjectivePoint const& point : AFK_AV_OBJECTIVE_POINTS)
        if (IsNearPoint(player, point.x, point.y, point.z, AFK_AV_OBJECTIVE_RADIUS))
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
            return { 8, 13, 18, 24, 3, 3, 2, 1, 3 };
        case BATTLEGROUND_AB:
            return { 10, 16, 22, 30, 2, 2, 1, 1, 3 };
        case BATTLEGROUND_AV:
            return { 14, 22, 30, 40, 2, 2, 1, 1, 2 };
        default:
            return { 10, 16, 22, 30, 2, 2, 1, 1, 3 };
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
        MaybeSendActivityNotice(bg, guid, state, rule);
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
    bool const effectiveDamageDone = state.recentDamageDone >= AFK_EFFECTIVE_DAMAGE_THRESHOLD;
    bool const effectiveDamageTaken = state.recentDamageTaken >= AFK_EFFECTIVE_DAMAGE_THRESHOLD;
    bool const effectiveHealingDone = state.recentHealingDone >= AFK_EFFECTIVE_HEALING_THRESHOLD;
    bool const objectiveEvent = state.recentObjectiveEvents > 0;
    bool nearObjective = false;

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

    if (effectiveDamageDone)
        score -= AFK_DAMAGE_DONE_SCORE_REDUCE;

    if (effectiveDamageTaken)
        score -= AFK_DAMAGE_TAKEN_SCORE_REDUCE;

    if (effectiveHealingDone)
        score -= AFK_HEALING_DONE_SCORE_REDUCE;

    if (objectiveEvent)
        score -= AFK_OBJECTIVE_SCORE_REDUCE;

    if (bg->GetTypeID() == BATTLEGROUND_AB)
    {
        nearObjective = IsNearABObjective(player);
        if (nearObjective)
            score -= AFK_AB_OBJECTIVE_SCORE_REDUCE;

        if (!moved && !inCombat && IsNearTeamStart(bg, player, guid, AFK_AB_START_RADIUS))
            score += AFK_AB_START_IDLE_SCORE;
    }
    else if (bg->GetTypeID() == BATTLEGROUND_WS)
    {
        nearObjective = IsNearWSGObjective(bg, player);
        if (nearObjective)
            score -= AFK_WSG_OBJECTIVE_SCORE_REDUCE;
    }
    else if (bg->GetTypeID() == BATTLEGROUND_AV)
    {
        nearObjective = IsNearAVObjective(player);
        if (nearObjective)
            score -= AFK_AV_OBJECTIVE_SCORE_REDUCE;
    }

    bool const effectiveContribution = effectiveDamageDone || effectiveDamageTaken || effectiveHealingDone ||
        objectiveEvent || (nearObjective && (moved || inCombat));
    if (effectiveContribution)
        state.noObjectiveContributionChecks = 0;
    else
    {
        ++state.noObjectiveContributionChecks;
        if (state.noObjectiveContributionChecks > AFK_NO_OBJECTIVE_GRACE_CHECKS)
            score += AFK_NO_OBJECTIVE_CHAIN_SCORE;
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
    MaybeSendActivityNotice(bg, guid, state, rule);
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
            state.lastRecoveryNoticeTime = m_elapsedTime;
            state.lastActivityNoticeTime = m_elapsedTime;
            state.lastActivityNoticeLevel = 0;
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

void BattleGroundAfkMgr::MaybeSendActivityNotice(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState& state, BattleGroundAfkScoreRule const& rule) const
{
    if (state.lastWarnTime == m_elapsedTime || state.lastRecoveryNoticeTime == m_elapsedTime)
        return;

    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player || player->IsBot() || player->IsGameMaster() || player->GetBattleGround() != bg)
        return;

    uint8 level = 0;
    if (state.score >= rule.warning3Score)
        level = 3;
    else if (state.score >= rule.warning2Score)
        level = 2;
    else if (state.score >= rule.warning1Score)
        level = 1;

    bool const levelChanged = state.lastActivityNoticeLevel == 255 || state.lastActivityNoticeLevel != level;
    uint32 const cooldown = level == 0 ? AFK_ACTIVITY_NOTICE_NORMAL_COOLDOWN : AFK_ACTIVITY_NOTICE_LOW_COOLDOWN;
    if (!levelChanged && state.lastActivityNoticeTime && m_elapsedTime < state.lastActivityNoticeTime + cooldown)
        return;

    state.lastActivityNoticeTime = m_elapsedTime;
    state.lastActivityNoticeLevel = level;
    SendActivityNotice(bg, guid, level);
}

void BattleGroundAfkMgr::SendActivityNotice(BattleGround* bg, ObjectGuid guid, uint8 level) const
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;

    time_t const now = time(nullptr);
    tm const* localTime = localtime(&now);
    char currentTime[6];
    snprintf(currentTime, sizeof(currentTime), "%02u:%02u", uint32(localTime->tm_hour), uint32(localTime->tm_min));

    if (level > 0)
        sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] activity level %u player %s (%s) bgType %u instance %u.",
            uint32(level), player->GetName(), guid.GetString().c_str(), bg ? uint32(bg->GetTypeID()) : 0, bg ? bg->GetInstanceID() : 0);

    switch (level)
    {
        case 0:
            player->PSendSysMessage("[%s] 您当前战场活跃度正常。", currentTime);
            break;
        case 1:
            player->PSendSysMessage("[%s] 您当前战场活跃度偏低，请参与战斗、治疗队友或争夺战场目标。", currentTime);
            break;
        case 2:
            player->PSendSysMessage("[%s] 您当前战场活跃度不足，请尽快提升战场参与。", currentTime);
            break;
        default:
            player->PSendSysMessage("[%s] 您当前战场活跃度危险，请立即参与战斗或战场目标。", currentTime);
            break;
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
