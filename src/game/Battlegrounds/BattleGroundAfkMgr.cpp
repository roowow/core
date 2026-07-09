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
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace
{
uint32 const AFK_CHECK_INTERVAL           = 30 * IN_MILLISECONDS;
uint32 const SPELL_DESERTER               = 26013;
uint32 const AFK_WARNING_COOLDOWN = 5 * MINUTE * IN_MILLISECONDS;
uint32 const AFK_RECOVERY_NOTICE_COOLDOWN = 2 * MINUTE * IN_MILLISECONDS;
uint32 const AFK_RECOVERY_PENDING_WARNING_GRACE = 2 * MINUTE * IN_MILLISECONDS;
uint32 const AFK_ACTIVITY_NOTICE_NORMAL_COOLDOWN = 10 * MINUTE * IN_MILLISECONDS;
uint32 const AFK_ACTIVITY_NOTICE_LOW_COOLDOWN = 3 * MINUTE * IN_MILLISECONDS;
uint32 const AFK_ACTIVITY_LEVEL_CHANGE_COOLDOWN = 90 * IN_MILLISECONDS;
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
int32 const AFK_BOT_KILL_SCORE_REDUCE = 4;
int32 const AFK_PLAYER_KILL_SCORE_REDUCE = 10;
int32 const AFK_WSG_FLAG_CARRIER_SCORE_REDUCE = 6;
uint32 const AFK_RECOVERY_NORMAL_MARGIN = 4;
uint32 const AFK_DEAD_GRACE_CHECKS = 4;
int32 const AFK_DEAD_IDLE_SCORE = 2;

enum AfkReasonMask
{
    AFK_REASON_NONE               = 0,
    AFK_REASON_MOVED              = 1 << 0,
    AFK_REASON_NO_MOVE            = 1 << 1,
    AFK_REASON_COMBAT             = 1 << 2,
    AFK_REASON_NO_COMBAT          = 1 << 3,
    AFK_REASON_NO_CONTRIBUTION    = 1 << 4,
    AFK_REASON_DAMAGE_DONE        = 1 << 5,
    AFK_REASON_DAMAGE_TAKEN       = 1 << 6,
    AFK_REASON_HEALING_DONE       = 1 << 7,
    AFK_REASON_OBJECTIVE_EVENT    = 1 << 8,
    AFK_REASON_NEAR_OBJECTIVE     = 1 << 9,
    AFK_REASON_NEAR_START         = 1 << 10,
    AFK_REASON_NO_OBJECTIVE_CHAIN = 1 << 11,
    AFK_REASON_DEAD_GRACE         = 1 << 12,
    AFK_REASON_DEAD_IDLE          = 1 << 13
};

int32 ClampAfkScore(int32 score)
{
    return std::max<int32>(0, std::min<int32>(100, score));
}

char const* BoolText(bool value)
{
    return value ? "1" : "0";
}

char const* GetAfkBattleGroundName(uint32 bgType)
{
    switch (bgType)
    {
        case BATTLEGROUND_AV:
            return "AV";
        case BATTLEGROUND_WS:
            return "WSG";
        case BATTLEGROUND_AB:
            return "AB";
        default:
            return "UNKNOWN";
    }
}

void NotifyTeamAfkKick(BattleGround* bg, ObjectGuid kickedGuid, char const* kickedName)
{
    if (!bg || !kickedName)
        return;

    Team kickedTeam = bg->GetPlayerTeam(kickedGuid);
    if (!kickedTeam)
    {
        if (Player* kickedPlayer = sObjectMgr.GetPlayer(kickedGuid))
            kickedTeam = kickedPlayer->GetBGTeam();
    }

    if (!kickedTeam)
        return;

    for (BattleGround::BattleGroundPlayerMap::const_iterator itr = bg->GetPlayers().begin(); itr != bg->GetPlayers().end(); ++itr)
    {
        if (itr->first == kickedGuid)
            continue;

        Player* player = sObjectMgr.GetPlayer(itr->first);
        if (!player || player->IsBot() || player->GetBattleGround() != bg)
            continue;

        Team team = bg->GetPlayerTeam(itr->first);
        if (!team)
            team = player->GetBGTeam();

        if (team == kickedTeam)
            player->PSendSysMessage("玩家 %s 因战场活跃度不足已被移出战场。", kickedName);
    }
}

void AppendAfkReason(std::string& reasons, char const* reason)
{
    if (!reasons.empty())
        reasons += ",";

    reasons += reason;
}

std::string FormatAfkReasons(uint32 mask)
{
    std::string reasons;

    if (mask & AFK_REASON_MOVED)
        AppendAfkReason(reasons, "moved");
    if (mask & AFK_REASON_NO_MOVE)
        AppendAfkReason(reasons, "noMove");
    if (mask & AFK_REASON_COMBAT)
        AppendAfkReason(reasons, "combat");
    if (mask & AFK_REASON_NO_COMBAT)
        AppendAfkReason(reasons, "noCombat");
    if (mask & AFK_REASON_NO_CONTRIBUTION)
        AppendAfkReason(reasons, "noContribution");
    if (mask & AFK_REASON_DAMAGE_DONE)
        AppendAfkReason(reasons, "damageDone");
    if (mask & AFK_REASON_DAMAGE_TAKEN)
        AppendAfkReason(reasons, "damageTaken");
    if (mask & AFK_REASON_HEALING_DONE)
        AppendAfkReason(reasons, "healingDone");
    if (mask & AFK_REASON_OBJECTIVE_EVENT)
        AppendAfkReason(reasons, "objectiveEvent");
    if (mask & AFK_REASON_NEAR_OBJECTIVE)
        AppendAfkReason(reasons, "nearObjective");
    if (mask & AFK_REASON_NEAR_START)
        AppendAfkReason(reasons, "nearStart");
    if (mask & AFK_REASON_NO_OBJECTIVE_CHAIN)
        AppendAfkReason(reasons, "noObjectiveChain");
    if (mask & AFK_REASON_DEAD_GRACE)
        AppendAfkReason(reasons, "deadGrace");
    if (mask & AFK_REASON_DEAD_IDLE)
        AppendAfkReason(reasons, "deadIdle");

    return reasons.empty() ? "none" : reasons;
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

char const* GetAfkTrackStopReasonForInvalidPlayer(Player* player, BattleGround* bg)
{
    if (!player)
        return "offline";

    if (player->IsBot())
        return "bot";

    if (!player->GetBattleGround())
        return "not_in_bg";

    if (player->GetBattleGround() != bg)
        return "other_bg";

    return "unknown_remove";
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
        {
            Player* player = sObjectMgr.GetPlayer(itr->first);
            char const* reason = GetAfkTrackStopReasonForInvalidPlayer(player, bg);
            if (player && player->GetBattleGround() == bg)
                reason = "left_roster";

            SendTrackStop(bg, itr->first, itr->second, itr->second.kicked ? "kicked" : reason);
            m_playerStates.erase(itr++);
        }
        else
            ++itr;
    }
}

void BattleGroundAfkMgr::RemovePlayer(ObjectGuid guid, char const* reason)
{
    std::map<ObjectGuid, BattleGroundAfkPlayerState>::iterator itr = m_playerStates.find(guid);
    if (itr != m_playerStates.end())
    {
        SendTrackStop(nullptr, guid, itr->second, itr->second.kicked ? "kicked" : (reason ? reason : "unknown_remove"));
        m_playerStates.erase(itr);
    }
}

void BattleGroundAfkMgr::Reset()
{
    for (std::map<ObjectGuid, BattleGroundAfkPlayerState>::const_iterator itr = m_playerStates.begin(); itr != m_playerStates.end(); ++itr)
        SendTrackStop(nullptr, itr->first, itr->second, "reset");

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

void BattleGroundAfkMgr::RecordCrowdControl(ObjectGuid guid)
{
    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    state.recentCrowdControl = std::min<uint32>(state.recentCrowdControl + 1, 1000);
}

void BattleGroundAfkMgr::RecordKill(ObjectGuid guid, bool realPlayer)
{
    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    if (realPlayer)
        state.recentPlayerKills = std::min<uint32>(state.recentPlayerKills + 1, 100);
    else
        state.recentBotKills = std::min<uint32>(state.recentBotKills + 1, 100);
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
    if (!player || player->IsBot() || player->GetBattleGround() != bg)
    {
        RemovePlayer(guid, GetAfkTrackStopReasonForInvalidPlayer(player, bg));
        return;
    }

    BattleGroundAfkPlayerState& state = m_playerStates[guid];
    if (!state.initialized)
    {
        state.initialized = true;
        state.lastX = player->GetPositionX();
        state.lastY = player->GetPositionY();
        state.lastZ = player->GetPositionZ();
        Team team = bg->GetPlayerTeam(guid);
        if (!team)
            team = player->GetBGTeam();
        state.trackBgType = uint32(bg->GetTypeID());
        state.trackInstanceId = bg->GetInstanceID();
        state.trackTeam = uint32(team);
        sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] track start player %s (%s) bgType %u bgName %s instance %u team %u elapsed %u pos %.2f %.2f %.2f.",
            player->GetName(), guid.GetString().c_str(), state.trackBgType, GetAfkBattleGroundName(state.trackBgType), state.trackInstanceId, state.trackTeam, m_elapsedTime,
            state.lastX, state.lastY, state.lastZ);
        return;
    }

    if (!player->IsAlive())
    {
        BattleGroundAfkScoreRule const rule = GetRule(bg);
        uint32 const previousScore = state.score;
        uint32 reasonMask = AFK_REASON_DEAD_GRACE;

        ++state.deadChecks;
        if (state.deadChecks > AFK_DEAD_GRACE_CHECKS)
        {
            state.score = uint32(ClampAfkScore(int32(state.score) + AFK_DEAD_IDLE_SCORE));
            reasonMask = AFK_REASON_DEAD_IDLE;
        }

        state.lastX = player->GetPositionX();
        state.lastY = player->GetPositionY();
        state.lastZ = player->GetPositionZ();
        state.lastPreviousScore = previousScore;
        state.lastReasonMask = reasonMask;
        state.lastMoveDistance = 0.0f;
        state.lastMoved = false;
        state.lastInCombat = false;
        state.lastNearObjective = false;
        state.lastDamageDone = 0;
        state.lastDamageTaken = 0;
        state.lastHealingDone = 0;
        state.lastObjectiveEvents = 0;
        state.recentDamageDone = 0;
        state.recentDamageTaken = 0;
        state.recentHealingDone = 0;
        state.recentObjectiveEvents = 0;
        state.recentCrowdControl = 0;
        ApplyStage(bg, guid, state, rule, previousScore, false, false);
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
    float const movedDistance = std::sqrt(movedDistanceSq);
    bool const moved = movedDistanceSq >= AFK_MIN_MOVE_DISTANCE * AFK_MIN_MOVE_DISTANCE;
    bool const inCombat = player->IsInCombat();
    uint32 const recentDamageDone = state.recentDamageDone;
    uint32 const recentDamageTaken = state.recentDamageTaken;
    uint32 const recentHealingDone = state.recentHealingDone;
    uint32 const recentObjectiveEvents = state.recentObjectiveEvents;
    uint32 const recentCrowdControl = state.recentCrowdControl;
    bool const effectiveDamageDone = state.recentDamageDone >= AFK_EFFECTIVE_DAMAGE_THRESHOLD;
    bool const effectiveDamageTaken = state.recentDamageTaken >= AFK_EFFECTIVE_DAMAGE_THRESHOLD;
    bool const effectiveHealingDone = state.recentHealingDone >= AFK_EFFECTIVE_HEALING_THRESHOLD;
    bool const objectiveEvent = state.recentObjectiveEvents > 0;
    bool const crowdControlEvent = state.recentCrowdControl > 0;
    bool const botKill = state.recentBotKills > 0;
    bool const playerKill = state.recentPlayerKills > 0;
    bool nearObjective = false;
    uint32 reasonMask = AFK_REASON_NONE;
    bool const wsgFlagCarrier = (bg->GetTypeID() == BATTLEGROUND_WS) && IsWSGFlagCarrier(player);

    if (wsgFlagCarrier)
    {
        // Carrying the flag counts as active contribution: skip all penalties, apply reduction.
        score -= AFK_WSG_FLAG_CARRIER_SCORE_REDUCE;
        nearObjective = true;
        reasonMask |= AFK_REASON_NEAR_OBJECTIVE;
    }
    else
    {
        if (!moved)
        {
            score += rule.noMovementScore;
            reasonMask |= AFK_REASON_NO_MOVE;
        }
        else
        {
            score -= rule.movementReduce;
            reasonMask |= AFK_REASON_MOVED;
        }

        if (!inCombat)
        {
            score += rule.noCombatScore;
            reasonMask |= AFK_REASON_NO_COMBAT;
        }
        else
        {
            score -= rule.combatReduce;
            reasonMask |= AFK_REASON_COMBAT;
        }

        if (!moved && !inCombat)
        {
            score += rule.noContributionScore;
            reasonMask |= AFK_REASON_NO_CONTRIBUTION;
        }
    }

    if (effectiveDamageDone)
    {
        score -= AFK_DAMAGE_DONE_SCORE_REDUCE;
        reasonMask |= AFK_REASON_DAMAGE_DONE;
    }

    if (effectiveDamageTaken)
    {
        score -= AFK_DAMAGE_TAKEN_SCORE_REDUCE;
        reasonMask |= AFK_REASON_DAMAGE_TAKEN;
    }

    if (effectiveHealingDone)
    {
        score -= AFK_HEALING_DONE_SCORE_REDUCE;
        reasonMask |= AFK_REASON_HEALING_DONE;
    }

    if (objectiveEvent)
    {
        score -= AFK_OBJECTIVE_SCORE_REDUCE;
        reasonMask |= AFK_REASON_OBJECTIVE_EVENT;
    }

    if (crowdControlEvent)
    {
        score -= AFK_OBJECTIVE_SCORE_REDUCE;
        reasonMask |= AFK_REASON_OBJECTIVE_EVENT;
    }

    if (botKill)
        score -= int32(std::min(state.recentBotKills, 3u)) * AFK_BOT_KILL_SCORE_REDUCE;
    if (playerKill)
        score -= int32(std::min(state.recentPlayerKills, 2u)) * AFK_PLAYER_KILL_SCORE_REDUCE;

    bool const activeContribution = inCombat || effectiveDamageDone || effectiveDamageTaken || effectiveHealingDone || objectiveEvent || crowdControlEvent || botKill || playerKill || wsgFlagCarrier;
    // Initiated contribution: player-originated actions only (excludes passive inCombat / damage taken).
    // Used at kick threshold so AFK players being hit by enemies cannot indefinitely avoid removal.
    bool const initiatedContribution = effectiveDamageDone || effectiveHealingDone || objectiveEvent || crowdControlEvent || botKill || playerKill || wsgFlagCarrier;

    if (bg->GetTypeID() == BATTLEGROUND_AB)
    {
        nearObjective = IsNearABObjective(player);
        if (nearObjective && activeContribution)
        {
            score -= AFK_AB_OBJECTIVE_SCORE_REDUCE;
            reasonMask |= AFK_REASON_NEAR_OBJECTIVE;
        }

        if (!moved && !inCombat && IsNearTeamStart(bg, player, guid, AFK_AB_START_RADIUS))
        {
            score += AFK_AB_START_IDLE_SCORE;
            reasonMask |= AFK_REASON_NEAR_START;
        }
    }
    else if (bg->GetTypeID() == BATTLEGROUND_WS && !wsgFlagCarrier)
    {
        // Flag carriers are already handled above; only evaluate others here.
        // nearObjective score reduction requires active contribution, same as AB —
        // just running near the flag carrier without fighting does not count.
        nearObjective = IsNearWSGObjective(bg, player);
        if (nearObjective)
        {
            reasonMask |= AFK_REASON_NEAR_OBJECTIVE;
            if (activeContribution)
                score -= AFK_WSG_OBJECTIVE_SCORE_REDUCE;
        }
    }
    else if (bg->GetTypeID() == BATTLEGROUND_AV)
    {
        nearObjective = IsNearAVObjective(player);
        if (nearObjective)
        {
            score -= AFK_AV_OBJECTIVE_SCORE_REDUCE;
            reasonMask |= AFK_REASON_NEAR_OBJECTIVE;
        }
    }

    // AB/WSG: nearObjective only counts as effective contribution when actively contributing
    // (combat/damage/heal/objective), preventing idle walkers from resetting the chain penalty.
    // AV: movement near an objective is sufficient given its large map and long travel times.
    // WSG flag carrier: always effective regardless of movement.
    bool const nearObjectiveEffective = nearObjective && (
        wsgFlagCarrier ? true :
        bg->GetTypeID() == BATTLEGROUND_AV ? (moved || inCombat) : activeContribution);
    bool const effectiveContribution = effectiveDamageDone || effectiveDamageTaken || effectiveHealingDone ||
        objectiveEvent || crowdControlEvent || nearObjectiveEffective || botKill || playerKill || wsgFlagCarrier;
    if (effectiveContribution)
        state.noObjectiveContributionChecks = 0;
    else
    {
        ++state.noObjectiveContributionChecks;
        if (state.noObjectiveContributionChecks > AFK_NO_OBJECTIVE_GRACE_CHECKS)
        {
            score += AFK_NO_OBJECTIVE_CHAIN_SCORE;
            reasonMask |= AFK_REASON_NO_OBJECTIVE_CHAIN;
        }
    }

    state.recentDamageDone = 0;
    state.recentDamageTaken = 0;
    state.recentHealingDone = 0;
    state.recentObjectiveEvents = 0;
    state.recentCrowdControl = 0;
    state.recentBotKills = 0;
    state.recentPlayerKills = 0;

    state.score = uint32(ClampAfkScore(score));
    state.lastPreviousScore = previousScore;
    state.lastReasonMask = reasonMask;
    state.lastMoveDistance = movedDistance;
    state.lastMoved = moved;
    state.lastInCombat = inCombat;
    state.lastNearObjective = nearObjective;
    state.lastDamageDone = recentDamageDone;
    state.lastDamageTaken = recentDamageTaken;
    state.lastHealingDone = recentHealingDone;
    state.lastObjectiveEvents = recentObjectiveEvents;
    state.lastX = player->GetPositionX();
    state.lastY = player->GetPositionY();
    state.lastZ = player->GetPositionZ();

    // AV has a large map where players legitimately spend long periods moving or defending
    // objectives without immediate combat. AB/WSG are small enough that real combat should
    // always be available, so proximity alone does not count as recovery at kick threshold.
    // At the kick threshold we use initiatedContribution (not activeContribution) so that
    // passive behavior — being attacked, standing near an AV objective without acting —
    // cannot indefinitely block removal.
    bool const kickThresholdRecovery = (bg->GetTypeID() == BATTLEGROUND_AV)
        ? (initiatedContribution || (nearObjective && moved))
        : initiatedContribution;
    ApplyStage(bg, guid, state, rule, previousScore, kickThresholdRecovery, activeContribution);
    MaybeSendActivityNotice(bg, guid, state, rule);
}

void BattleGroundAfkMgr::ApplyStage(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState& state, BattleGroundAfkScoreRule const& rule, uint32 previousScore, bool kickThresholdRecovery, bool activeContribution)
{
    uint8 targetStage = 0;
    if (state.score >= rule.warning3Score)
        targetStage = 3;
    else if (state.score >= rule.warning2Score)
        targetStage = 2;
    else if (state.score >= rule.warning1Score)
        targetStage = 1;

    uint32 const effectiveCooldown = (state.score >= rule.kickScore * 2u)
        ? AFK_WARNING_COOLDOWN / 2
        : AFK_WARNING_COOLDOWN;
    bool const cooldownReady = !state.lastWarnTime || m_elapsedTime >= state.lastWarnTime + effectiveCooldown;
    bool const scoreDecreasing = state.score < previousScore;
    bool const recoveryNoticeReady = !state.lastRecoveryNoticeTime || m_elapsedTime >= state.lastRecoveryNoticeTime + AFK_RECOVERY_NOTICE_COOLDOWN;

    bool const recoveredNormal = state.score + AFK_RECOVERY_NORMAL_MARGIN < rule.warning1Score;

    // sustainedKickChecks accumulates every tick the score stays at kick threshold.
    // It resets only when the score genuinely drops below warning2 (real recovery),
    // so minor fluctuations while sitting at the kick ceiling cannot prevent removal.
    if (state.score >= rule.kickScore)
        ++state.sustainedKickChecks;
    else if (state.score < rule.warning2Score)
        state.sustainedKickChecks = 0;

    if (recoveredNormal)
    {
        if (state.stage > 0)
        {
            state.stage = 0;
            state.lastWarnTime = 0;
            state.lastRecoveryNoticeTime = m_elapsedTime;
            state.lastRecoveryPendingTime = 0;
            state.lastActivityNoticeTime = m_elapsedTime;
            state.lastActivityNoticeLevel = 0;
            state.noObjectiveContributionChecks = 0;
            state.sustainedKickChecks = 0;
            SendRecoveryNotice(bg, guid, state, true, false, false);
            return;
        }

        state.stage = 0;
        state.noObjectiveContributionChecks = 0;
        state.sustainedKickChecks = 0;
    }
    else if (state.score < rule.warning1Score)
        targetStage = 0;
    else if (targetStage < state.stage)
        state.stage = targetStage;

    if (state.stage > 0 && targetStage > state.stage && scoreDecreasing)
    {
        if (recoveryNoticeReady)
        {
            state.lastRecoveryNoticeTime = m_elapsedTime;
            state.lastRecoveryPendingTime = m_elapsedTime;
            SendRecoveryNotice(bg, guid, state, false, false, false);
        }
        return;
    }

    if (targetStage > state.stage &&
        state.lastRecoveryPendingTime &&
        m_elapsedTime < state.lastRecoveryPendingTime + AFK_RECOVERY_PENDING_WARNING_GRACE &&
        state.score <= previousScore)
        return;

    if (targetStage > state.stage && cooldownReady)
    {
        state.stage = state.stage + 1;
        state.lastWarnTime = m_elapsedTime;
        SendWarning(bg, guid, state);
        return;
    }

    if (state.score >= rule.kickScore && state.stage >= 3)
    {
        // forcedKick fires when the player has spent 3+ minutes at the kick threshold
        // without a genuine recovery (score dropping below warning2). It bypasses the
        // scoreDecreasing+kickThresholdRecovery grace so passive activity (being hit,
        // standing near an objective) cannot perpetually stall removal.
        bool const forcedKick = (state.sustainedKickChecks >= 6);

        if (!forcedKick && scoreDecreasing && kickThresholdRecovery)
        {
            state.maxScoreChecks = 0;
            if (recoveryNoticeReady)
            {
                state.lastRecoveryNoticeTime = m_elapsedTime;
                state.lastRecoveryPendingTime = m_elapsedTime;
                SendRecoveryNotice(bg, guid, state, false, true, activeContribution);
            }
            return;
        }

        ++state.maxScoreChecks;
        if (state.maxScoreChecks >= 3 || cooldownReady || forcedKick)
        {
            if (Player* player = sObjectMgr.GetPlayer(guid))
            {
                if (player->IsGameMaster())
                    return;

                std::string const reasons = FormatAfkReasons(state.lastReasonMask);
                int32 const delta = int32(state.score) - int32(state.lastPreviousScore);
                uint32 const bgType = bg ? uint32(bg->GetTypeID()) : state.trackBgType;
                sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] kick player %s (%s) bgType %u bgName %s instance %u elapsed %u stage %u score %u previousScore %u delta %+d reason %s moved %s moveDist %.1f combat %s nearObjective %s damage %u taken %u heal %u objective %u maxScoreChecks %u sustainedKickChecks %u forcedKick %s.",
                    player->GetName(), guid.GetString().c_str(), bgType, GetAfkBattleGroundName(bgType), bg ? bg->GetInstanceID() : state.trackInstanceId,
                    m_elapsedTime, uint32(state.stage), state.score, state.lastPreviousScore, delta, reasons.c_str(),
                    BoolText(state.lastMoved), state.lastMoveDistance, BoolText(state.lastInCombat), BoolText(state.lastNearObjective),
                    state.lastDamageDone, state.lastDamageTaken, state.lastHealingDone, state.lastObjectiveEvents,
                    state.maxScoreChecks, state.sustainedKickChecks, BoolText(forcedKick));
                state.kicked = true;
                player->SendSysMessage("您因长时间未有效参与战场，已被移出战场。");
                NotifyTeamAfkKick(bg, guid, player->GetName());
                player->LeaveBattleground();
            }
            return;
        }
    }
    else
        state.maxScoreChecks = 0;
}

void BattleGroundAfkMgr::MaybeSendActivityNotice(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState& state, BattleGroundAfkScoreRule const& rule) const
{
    if (state.lastWarnTime == m_elapsedTime || state.lastRecoveryNoticeTime == m_elapsedTime)
        return;

    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player || player->IsBot() || player->GetBattleGround() != bg)
        return;

    uint8 level = 0;
    if (state.score >= rule.warning3Score)
        level = 3;
    else if (state.score >= rule.warning2Score)
        level = 2;
    else if (state.score + AFK_RECOVERY_NORMAL_MARGIN >= rule.warning1Score)
        level = 1;

    bool const levelChanged = state.lastActivityNoticeLevel == 255 || state.lastActivityNoticeLevel != level;
    uint32 const cooldown = level == 0 ? AFK_ACTIVITY_NOTICE_NORMAL_COOLDOWN : AFK_ACTIVITY_NOTICE_LOW_COOLDOWN;
    if (levelChanged && state.lastActivityNoticeTime &&
        m_elapsedTime < state.lastActivityNoticeTime + AFK_ACTIVITY_LEVEL_CHANGE_COOLDOWN)
        return;

    if (!levelChanged && state.lastActivityNoticeTime && m_elapsedTime < state.lastActivityNoticeTime + cooldown)
        return;

    bool const firstNotice = state.lastActivityNoticeTime == 0;
    state.lastActivityNoticeTime = m_elapsedTime;
    state.lastActivityNoticeLevel = level;
    if (level == 0 && !firstNotice)
        return;
    bool const logNotice = level > 0 && (level < 3 || levelChanged);
    SendActivityNotice(bg, guid, level, state.stage, state.score, logNotice);
}

void BattleGroundAfkMgr::SendActivityNotice(BattleGround* bg, ObjectGuid guid, uint8 level, uint8 stage, uint32 score, bool logNotice) const
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;

    time_t const now = time(nullptr);
    tm const* localTime = localtime(&now);
    char currentTime[6];
    snprintf(currentTime, sizeof(currentTime), "%02u:%02u", uint32(localTime->tm_hour), uint32(localTime->tm_min));

    if (logNotice)
    {
        uint32 const bgType = bg ? uint32(bg->GetTypeID()) : 0;
        sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] activity level %u player %s (%s) bgType %u bgName %s instance %u elapsed %u stage %u score %u.",
            uint32(level), player->GetName(), guid.GetString().c_str(), bgType, GetAfkBattleGroundName(bgType), bg ? bg->GetInstanceID() : 0,
            m_elapsedTime, uint32(stage), score);
    }

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

void BattleGroundAfkMgr::SendTrackStop(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState const& state, char const* reason) const
{
    if (!state.initialized)
        return;

    Player* player = sObjectMgr.GetPlayer(guid);
    char const* playerName = player ? player->GetName() : "unknown";
    std::string const reasons = FormatAfkReasons(state.lastReasonMask);
    int32 const delta = int32(state.score) - int32(state.lastPreviousScore);
    uint32 const bgType = bg ? uint32(bg->GetTypeID()) : state.trackBgType;
    uint32 const instanceId = bg ? bg->GetInstanceID() : state.trackInstanceId;

    sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] track stop reason %s player %s (%s) bgType %u bgName %s instance %u team %u elapsed %u stage %u score %u previousScore %u delta %+d reasonMask %s moved %s moveDist %.1f combat %s nearObjective %s damage %u taken %u heal %u objective %u.",
        reason ? reason : "unknown", playerName, guid.GetString().c_str(), bgType, GetAfkBattleGroundName(bgType), instanceId, state.trackTeam, m_elapsedTime,
        uint32(state.stage), state.score, state.lastPreviousScore, delta, reasons.c_str(),
        BoolText(state.lastMoved), state.lastMoveDistance, BoolText(state.lastInCombat), BoolText(state.lastNearObjective),
        state.lastDamageDone, state.lastDamageTaken, state.lastHealingDone, state.lastObjectiveEvents);
}

void BattleGroundAfkMgr::SendWarning(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState const& state) const
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;

    time_t const now = time(nullptr);
    tm const* localTime = localtime(&now);
    char currentTime[6];
    snprintf(currentTime, sizeof(currentTime), "%02u:%02u", uint32(localTime->tm_hour), uint32(localTime->tm_min));

    std::string const reasons = FormatAfkReasons(state.lastReasonMask);
    int32 const delta = int32(state.score) - int32(state.lastPreviousScore);
    uint32 const bgType = bg ? uint32(bg->GetTypeID()) : state.trackBgType;
    sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] warning stage %u player %s (%s) bgType %u bgName %s instance %u elapsed %u score %u previousScore %u delta %+d reason %s moved %s moveDist %.1f combat %s nearObjective %s damage %u taken %u heal %u objective %u.",
        uint32(state.stage), player->GetName(), guid.GetString().c_str(), bgType, GetAfkBattleGroundName(bgType), bg ? bg->GetInstanceID() : state.trackInstanceId,
        m_elapsedTime, state.score, state.lastPreviousScore, delta, reasons.c_str(),
        BoolText(state.lastMoved), state.lastMoveDistance, BoolText(state.lastInCombat), BoolText(state.lastNearObjective),
        state.lastDamageDone, state.lastDamageTaken, state.lastHealingDone, state.lastObjectiveEvents);

    switch (state.stage)
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

void BattleGroundAfkMgr::SendRecoveryNotice(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState const& state, bool normal, bool atKickThreshold, bool activeContribution) const
{
    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;

    time_t const now = time(nullptr);
    tm const* localTime = localtime(&now);
    char currentTime[6];
    snprintf(currentTime, sizeof(currentTime), "%02u:%02u", uint32(localTime->tm_hour), uint32(localTime->tm_min));

    std::string const reasons = FormatAfkReasons(state.lastReasonMask);
    int32 const delta = int32(state.score) - int32(state.lastPreviousScore);
    uint32 const bgType = bg ? uint32(bg->GetTypeID()) : state.trackBgType;

    if (normal)
    {
        sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] recovery normal player %s (%s) bgType %u bgName %s instance %u elapsed %u stage %u score %u previousScore %u delta %+d reason %s moved %s moveDist %.1f combat %s nearObjective %s damage %u taken %u heal %u objective %u.",
            player->GetName(), guid.GetString().c_str(), bgType, GetAfkBattleGroundName(bgType), bg ? bg->GetInstanceID() : state.trackInstanceId,
            m_elapsedTime, uint32(state.stage), state.score, state.lastPreviousScore, delta, reasons.c_str(),
            BoolText(state.lastMoved), state.lastMoveDistance, BoolText(state.lastInCombat), BoolText(state.lastNearObjective),
            state.lastDamageDone, state.lastDamageTaken, state.lastHealingDone, state.lastObjectiveEvents);
        player->PSendSysMessage("[%s] 您的战场活动已恢复正常。", currentTime);
    }
    else if (atKickThreshold)
    {
        // nearObjHeld: recovery was granted by nearObjective alone (AV), not by real combat contribution
        bool const nearObjHeld = !activeContribution && state.lastNearObjective;
        sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] recovery kickhold player %s (%s) bgType %u bgName %s instance %u elapsed %u stage %u score %u previousScore %u delta %+d reason %s moved %s moveDist %.1f combat %s nearObjective %s damage %u taken %u heal %u objective %u activeCont %s nearObjHeld %s.",
            player->GetName(), guid.GetString().c_str(), bgType, GetAfkBattleGroundName(bgType), bg ? bg->GetInstanceID() : state.trackInstanceId,
            m_elapsedTime, uint32(state.stage), state.score, state.lastPreviousScore, delta, reasons.c_str(),
            BoolText(state.lastMoved), state.lastMoveDistance, BoolText(state.lastInCombat), BoolText(state.lastNearObjective),
            state.lastDamageDone, state.lastDamageTaken, state.lastHealingDone, state.lastObjectiveEvents,
            BoolText(activeContribution), BoolText(nearObjHeld));
        player->PSendSysMessage("[%s] 您的战场活动正在恢复，请持续参与战斗或战场目标，以免被移出。", currentTime);
    }
    else
    {
        sLog.Out(LOG_BG, LOG_LVL_BASIC, "[BattleGroundAfk] recovery pending player %s (%s) bgType %u bgName %s instance %u elapsed %u stage %u score %u previousScore %u delta %+d reason %s moved %s moveDist %.1f combat %s nearObjective %s damage %u taken %u heal %u objective %u.",
            player->GetName(), guid.GetString().c_str(), bgType, GetAfkBattleGroundName(bgType), bg ? bg->GetInstanceID() : state.trackInstanceId,
            m_elapsedTime, uint32(state.stage), state.score, state.lastPreviousScore, delta, reasons.c_str(),
            BoolText(state.lastMoved), state.lastMoveDistance, BoolText(state.lastInCombat), BoolText(state.lastNearObjective),
            state.lastDamageDone, state.lastDamageTaken, state.lastHealingDone, state.lastObjectiveEvents);
        player->PSendSysMessage("[%s] 您的战场活动正在恢复，请持续参与战斗或战场目标，以免被移出。", currentTime);
    }
}

