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

#include "BattleBotAI.h"
#include "BattleBotWaypoints.h"
#include "BattleBotWaypoints2.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Player.h"
#include "GameObject.h"
#include "MotionMaster.h"
#include "Spell.h"
#include "Battlegrounds/BattleGround.h"
#include "BattleGroundAB.h"
#include "BattleGroundAV.h"
#include "BattleGroundWS.h"
#include "DBCStores.h"
#include "Geometry.h"
#include <cstddef>

using namespace Geometry;

enum GameObjectsAB
{
    GO_AB_ALLIANCE_BANNER    = 180058,
    GO_AB_CONTESTED_BANNER1  = 180059,
    GO_AB_HORDE_BANNER       = 180060,
    GO_AB_CONTESTED_BANNER2  = 180061,
    GO_AB_STABLE_BANNER      = 180087,
    GO_AB_BLACKSMITH_BANNER  = 180088,
    GO_AB_FARM_BANNER        = 180089,
    GO_AB_LUMBER_MILL_BANNER = 180090,
    GO_AB_GOLD_MINE_BANNER   = 180091
};

enum GameObjectsAV
{
    GO_AV_HORDE_BANNER1     = 178364,
    GO_AV_HORDE_BANNER2     = 178943,
    GO_AV_ALLIANCE_BANNER1  = 178365,
    GO_AV_ALLIANCE_BANNER2  = 178925,
    GO_AV_CONTESTED_BANNER1 = 178940, // usable by horde
    GO_AV_CONTESTED_BANNER2 = 179286, // usable by horde
    GO_AV_CONTESTED_BANNER3 = 179287, // usable by alliance
    GO_AV_CONTESTED_BANNER4 = 179435, // usable by alliance
    GO_AV_SNOWFALL_BANNER   = 180418
};

enum CreaturesAV
{
    NPC_AV_GALVANGAR = 11947,
    NPC_AV_BALINDA   = 11949
};

enum GameObjectsWS
{
    GO_WS_SILVERWING_FLAG = 179830,
    GO_WS_WARSONG_FLAG    = 179831
};

enum AreaTriggersWS
{
    AT_SILVERWING_FLAG = 3646,
    AT_WARSONG_FLAG    = 3647
};

static Position const WSG_GuardPositions[BG_TEAMS_COUNT] =
{
    { 1519.53f, 1481.87f, 352.024f, 0.0f },  // Alliance flag room
    { 933.331f, 1433.72f, 345.536f, 0.0f }   // Horde flag room
};

#define WSG_GUARD_REQUIRED_BOTS 2

static bool HasEnemyFlagAura(Player* player)
{
    if (player->GetTeam() == ALLIANCE)
        return player->HasAura(AURA_WARSONG_FLAG);

    return player->HasAura(AURA_SILVERWING_FLAG);
}

bool BattleBotIsWSGHomeGuardCandidate(BattleBotAI const* pAI)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_WS)
        return false;

    // Flag carriers are runners, not home guards
    if (HasEnemyFlagAura(pAI->me))
        return false;

    Map* map = pAI->me->GetMap();
    if (!map)
        return false;

    uint8 higherGuidCandidates = 0;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            if (player == pAI->me)
                continue;

            if (player->GetTeam() != pAI->me->GetTeam() || !player->IsBot())
                continue;

            // Dead bots and flag carriers cannot actually guard — skip them
            // so a living bot gets promoted to fill the slot
            if (!player->IsAlive() || HasEnemyFlagAura(player))
                continue;

            if (player->GetObjectGuid().GetCounter() > pAI->me->GetObjectGuid().GetCounter())
                ++higherGuidCandidates;
        }
    }

    return higherGuidCandidates < WSG_GUARD_REQUIRED_BOTS;
}

static bool StartWSGHomeGuardObjective(BattleBotAI* pAI, BattleGroundWS* bgWS)
{
    if (!bgWS || !BattleBotIsWSGHomeGuardCandidate(pAI))
        return false;

    Team const team = pAI->me->GetTeam();
    uint8 const ownFlagState = bgWS->GetFlagState(team);

    // Own flag dropped on ground: go recover it immediately
    if (ownFlagState == BG_WS_FLAG_STATE_ON_GROUND)
    {
        if (GameObject* pFlag = pAI->me->GetMap()->GetGameObject(bgWS->GetDroppedFlagGuid(team)))
        {
            if (pAI->StartNewPathToPosition(pFlag->GetPosition(), vPaths_WS))
                return true;

            pAI->me->GetMotionMaster()->MovePoint(0, pFlag->GetPositionX(), pFlag->GetPositionY(), pFlag->GetPositionZ(), MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
            return true;
        }
    }

    // Own flag safe or carried by enemy: hold the guard position.
    // When the enemy carries our flag they run to their own base to score,
    // not ours — chasing them mid-field achieves nothing and leaves home empty.
    if (ownFlagState == BG_WS_FLAG_STATE_ON_BASE ||
        ownFlagState == BG_WS_FLAG_STATE_ON_PLAYER)
    {
        Position const& guardPosition = WSG_GuardPositions[BattleGround::GetTeamIndexByTeamId(team)];
        if (pAI->StartNewPathToPosition(guardPosition, vPaths_WS))
            return true;

        if (pAI->me->GetDistance(guardPosition) <= 25.0f)
            return true;

        pAI->me->GetMotionMaster()->MovePoint(0, guardPosition.x, guardPosition.y, guardPosition.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES);
        return true;
    }

    return false;
}

void WSG_AtAllianceFlag(BattleBotAI* pAI)
{
    if (GameObject* pFlag = pAI->me->FindNearestGameObject(GO_WS_SILVERWING_FLAG, 25.0f))
    {
        if (pFlag->isSpawned())
        {
            if (pAI->me->GetTeam() == HORDE)
            {
                if (pAI->me->IsWithinDistInMap(pFlag, INTERACTION_DISTANCE))
                {
                    pAI->ClearPath();
                    WorldPackets::Misc::GameObjectUse packet;
                    packet.guid = pFlag->GetObjectGuid();
                    pAI->me->GetSession()->HandleGameObjectUseOpcode(packet);
                    return;
                }
                else
                {
                    pAI->ClearPath();
                    pAI->me->GetMotionMaster()->MovePoint(0, pFlag->GetPositionX(), pFlag->GetPositionY(), pFlag->GetPositionZ());
                    return;
                }
            }
            else if (pAI->me->HasAura(AURA_WARSONG_FLAG))
            {
                pAI->ClearPath();
                pAI->me->GetMotionMaster()->MovePoint(0, pFlag->GetPositionX(), pFlag->GetPositionY(), pFlag->GetPositionZ());
                return;
            }
        }
    }

    pAI->MoveToNextPoint();
}

void WSG_AtHordeFlag(BattleBotAI* pAI)
{
    if (GameObject* pFlag = pAI->me->FindNearestGameObject(GO_WS_WARSONG_FLAG, 25.0f))
    {
        if (pFlag->isSpawned())
        {
            if (pAI->me->GetTeam() == ALLIANCE)
            {
                if (pAI->me->IsWithinDistInMap(pFlag, INTERACTION_DISTANCE))
                {
                    pAI->ClearPath();
                    WorldPackets::Misc::GameObjectUse packet;
                    packet.guid = pFlag->GetObjectGuid();
                    pAI->me->GetSession()->HandleGameObjectUseOpcode(packet);
                    return;
                }
                else
                {
                    pAI->ClearPath();
                    pAI->me->GetMotionMaster()->MovePoint(0, pFlag->GetPositionX(), pFlag->GetPositionY(), pFlag->GetPositionZ());
                    return;
                }
            }
            else if (pAI->me->HasAura(AURA_SILVERWING_FLAG))
            {
                pAI->ClearPath();
                pAI->me->GetMotionMaster()->MovePoint(0, pFlag->GetPositionX(), pFlag->GetPositionY(), pFlag->GetPositionZ());
                return;
            }
        }
    }

    pAI->MoveToNextPoint();
}

void WSG_AtAllianceGraveyard(BattleBotAI* pAI)
{
    if ((pAI->me->GetTeam() == ALLIANCE) && !pAI->me->IsMounted() && urand(0, 1))
    {
        pAI->ClearPath();
        pAI->DoGraveyardJump();
    }
    else
        pAI->MoveToNextPoint();
}

void WSG_AtHordeGraveyard(BattleBotAI* pAI)
{
    if ((pAI->me->GetTeam() == HORDE) && !pAI->me->IsMounted() && urand(0, 1))
    {
        pAI->ClearPath();
        pAI->DoGraveyardJump();
    }
    else
        pAI->MoveToNextPoint();
}

#define SPELL_CAPTURE_BANNER 21651
#define BB_SPELL_FOOD 1131
#define BB_SPELL_DRINK 1137

#define AB_GUARD_REQUIRED_BOTS 2
#define AB_GUARD_SEARCH_RADIUS 35.0f
#define AB_GUARD_KEEP_RADIUS   20.0f
#define AB_GUARD_EXCESS_RADIUS 45.0f
#define AB_GUARD_ASSIGN_RADIUS 20.0f
#define AV_FLAG_DEFENSE_RADIUS    55.0f
static Position const AB_GuardPositions[5] =
{
    { 1167.98f, 1202.9f, -56.4743f, 0.0f },   // Stables
    { 978.269f, 1043.84f, -44.4588f, 0.0f },  // Blacksmith
    { 804.429f, 874.961f, -55.2691f, 0.0f },  // Farm
    { 853.921f, 1150.92f, 11.543f, 0.0f },    // Lumber Mill
    { 1144.9f, 850.049f, -110.522f, 0.0f }   // Gold Mine
};

static bool IsABSettledGuardBot(Player* player, Player* currentBot)
{
    if (player->IsInCombat() || player->GetVictim())
        return false;

    if (player == currentBot)
        return true;

    if (!player->IsMoving())
        return true;

    switch (player->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        case POINT_MOTION_TYPE:
        case FOLLOW_MOTION_TYPE:
            return false;
        default:
            return true;
    }
}

static bool IsABAssignedToGuardPosition(Player* player, Position const& pos)
{
    if (BattleBotAI* pBotAI = dynamic_cast<BattleBotAI*>(player->AI()))
    {
        // Guard against empty path: front()/back() on an empty vector is UB.
        if (pBotAI->m_currentPath && !pBotAI->m_currentPath->empty())
        {
            BattleBotWaypoint const& targetPoint = pBotAI->m_movingInReverse ? pBotAI->m_currentPath->front() : pBotAI->m_currentPath->back();
            if (GetDistance3D(targetPoint, pos) <= AB_GUARD_ASSIGN_RADIUS)
                return true;
        }
    }

    float x, y, z;
    if (player->GetMotionMaster()->GetDestination(x, y, z) &&
        GetDistance3D(x, y, z, pos.x, pos.y, pos.z) <= AB_GUARD_ASSIGN_RADIUS)
        return true;

    return false;
}

static uint8 CountABGuardBots(BattleBotAI* pAI, Position const& pos, bool includeAssigned)
{
    Map* map = pAI->me->GetMap();
    if (!map)
        return 0;

    uint8 count = 0;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            if (player->GetTeam() != pAI->me->GetTeam() || !player->IsBot() || !player->IsAlive())
                continue;

            if (player->GetDistance(pos) <= AB_GUARD_SEARCH_RADIUS &&
                IsABSettledGuardBot(player, pAI->me))
            {
                ++count;
                continue;
            }

            if (includeAssigned &&
                !player->IsInCombat() &&
                !player->GetVictim() &&
                IsABAssignedToGuardPosition(player, pos))
                ++count;
        }
    }

    return count;
}

static bool IsABGuardingPosition(Player* player, Position const& pos, bool includeAssigned)
{
    if (player->IsInCombat() || player->GetVictim())
        return false;

    if (player->GetDistance(pos) <= AB_GUARD_EXCESS_RADIUS)
        return true;

    if (includeAssigned && IsABAssignedToGuardPosition(player, pos))
        return true;

    return false;
}

static uint8 GetABRequiredGuardBots(BattleGround* bg, Team team);

static bool IsABExcessGuardBot(BattleBotAI* pAI, Position const& pos)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    Map* map = pAI->me->GetMap();
    if (!bg || !map)
        return false;

    if (!IsABGuardingPosition(pAI->me, pos, true))
        return false;

    uint8 const requiredGuards = GetABRequiredGuardBots(bg, pAI->me->GetTeam());
    bool const currentIsHealer = pAI->GetRole() == ROLE_HEALER;
    uint8 preferredGuards = 0;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            if (player == pAI->me)
                continue;

            if (player->GetTeam() != pAI->me->GetTeam() || !player->IsBot() || !player->IsAlive())
                continue;

            if (player->IsInCombat() || player->GetVictim())
                continue;

            if (!IsABGuardingPosition(player, pos, true))
                continue;

            bool playerIsHealer = false;
            if (BattleBotAI* pBotAI = dynamic_cast<BattleBotAI*>(player->AI()))
                playerIsHealer = pBotAI->GetRole() == ROLE_HEALER;

            if (currentIsHealer)
            {
                if (!playerIsHealer ||
                    player->GetObjectGuid().GetCounter() < pAI->me->GetObjectGuid().GetCounter())
                    ++preferredGuards;
            }
            else if (!playerIsHealer &&
                     player->GetObjectGuid().GetCounter() < pAI->me->GetObjectGuid().GetCounter())
                ++preferredGuards;
        }
    }

    return preferredGuards >= requiredGuards;
}

static bool IsABNodeOccupiedByTeam(BattleGround* bg, Team team, uint8 node)
{
    if (!bg)
        return false;

    BattleGroundTeamIndex const teamIndex = BattleGround::GetTeamIndexByTeamId(team);
    return bg->IsActiveEvent(node, teamIndex + BG_AB_NODE_TYPE_OCCUPIED);
}

bool BattleBotIsABGuardingOwnedNode(BattleBotAI const* pAI)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AB)
        return false;

    if (pAI->me->IsInCombat() || pAI->me->GetVictim())
        return false;

    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
    {
        if (!IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), i))
            continue;

        if (IsABGuardingPosition(pAI->me, AB_GuardPositions[i], true))
            return true;
    }

    return false;
}

static bool IsABNodeContestedByTeam(BattleGround* bg, Team team, uint8 node)
{
    if (!bg)
        return false;

    BattleGroundTeamIndex const teamIndex = BattleGround::GetTeamIndexByTeamId(team);
    return bg->IsActiveEvent(node, teamIndex + BG_AB_NODE_TYPE_CONTESTED);
}

Unit* BattleBotSelectABFlagDefenseTarget(BattleBotAI const* pAI, Unit* pExcept)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AB)
        return nullptr;

    CombatBotRoles const role = pAI->GetRole();
    if (role != ROLE_MELEE_DPS && role != ROLE_RANGE_DPS && role != ROLE_TANK)
        return nullptr;

    Position const* defendPosition = nullptr;
    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
    {
        if (!IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), i))
            continue;

        if (pAI->me->GetDistance2d(AB_GuardPositions[i]) <= AB_GUARD_EXCESS_RADIUS)
        {
            defendPosition = &AB_GuardPositions[i];
            break;
        }
    }

    if (!defendPosition)
        return nullptr;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, VISIBILITY_DISTANCE_NORMAL);

    Player* bestTarget = nullptr;
    float bestDistanceToFlag = FLT_MAX;
    for (Player* player : players)
    {
        if (player == pExcept)
            continue;

        if (!pAI->IsValidHostileTarget(player) || pAI->IsBadPlayer(player))
            continue;

        if (player->GetDistance(*defendPosition) > AB_GUARD_EXCESS_RADIUS)
            continue;

        if (pAI->me->GetDistanceZ(player) > 10.0f)
            continue;

        if (!pAI->me->IsWithinLOSInMap(player))
            continue;

        float const distanceToFlag = player->GetDistance(*defendPosition);
        if (!bestTarget || distanceToFlag < bestDistanceToFlag)
        {
            bestTarget = player;
            bestDistanceToFlag = distanceToFlag;
        }
    }

    return bestTarget;
}

static uint8 CountABOccupiedNodesByTeam(BattleGround* bg, Team team)
{
    if (!bg)
        return 0;

    uint8 count = 0;
    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
        if (IsABNodeOccupiedByTeam(bg, team, i))
            ++count;

    return count;
}

static uint8 GetABRequiredGuardBots(BattleGround* bg, Team team)
{
    return CountABOccupiedNodesByTeam(bg, team) >= 3 ? 1 : AB_GUARD_REQUIRED_BOTS;
}

static uint8 GetABHomeNode(Team team)
{
    return team == ALLIANCE ? BG_AB_NODE_STABLES : BG_AB_NODE_FARM;
}

static Position const& SelectABPositionForBot(BattleBotAI* pAI, std::vector<uint8> const& nodes)
{
    return AB_GuardPositions[nodes[pAI->me->GetObjectGuid().GetCounter() % nodes.size()]];
}

static bool FindABAssaultPosition(BattleBotAI* pAI, Position& outPosition)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg)
        return false;

    uint8 const homeNode = GetABHomeNode(pAI->me->GetTeam());
    if (!IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), homeNode) &&
        CountABGuardBots(pAI, AB_GuardPositions[homeNode], true) < AB_GUARD_REQUIRED_BOTS)
    {
        outPosition = AB_GuardPositions[homeNode];
        return true;
    }

    uint8 bestCount = AB_GUARD_REQUIRED_BOTS;
    std::vector<uint8> bestNodes;

    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
    {
        if (IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), i))
            continue;

        uint8 count = CountABGuardBots(pAI, AB_GuardPositions[i], true);
        if (count >= AB_GUARD_REQUIRED_BOTS)
            continue;

        if (count < bestCount)
        {
            bestCount = count;
            bestNodes.clear();
        }

        if (count == bestCount)
        {
            bestNodes.push_back(i);
        }
    }

    if (bestNodes.empty())
        return false;

    outPosition = SelectABPositionForBot(pAI, bestNodes);
    return true;
}

static bool FindABOwnedGuardPosition(BattleBotAI* pAI, Position& outPosition)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg)
        return false;

    uint8 const requiredGuards = GetABRequiredGuardBots(bg, pAI->me->GetTeam());

    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
    {
        if (!IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), i))
            continue;

        if (pAI->me->GetDistance(AB_GuardPositions[i]) <= AB_GUARD_EXCESS_RADIUS &&
            !IsABExcessGuardBot(pAI, AB_GuardPositions[i]))
        {
            outPosition = AB_GuardPositions[i];
            return true;
        }
    }

    uint8 bestCount = requiredGuards;
    std::vector<uint8> bestNodes;

    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
    {
        if (!IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), i))
            continue;

        uint8 count = CountABGuardBots(pAI, AB_GuardPositions[i], true);
        if (count >= requiredGuards)
            continue;

        if (count < bestCount)
        {
            bestCount = count;
            bestNodes.clear();
        }

        if (count == bestCount)
        {
            bestNodes.push_back(i);
        }
    }

    if (bestNodes.empty())
        return false;

    outPosition = SelectABPositionForBot(pAI, bestNodes);
    return true;
}

static bool IsABFlagOpenable(BattleBotAI const* pAI, GameObject* pGo)
{
    if (!pGo || !pGo->isSpawned())
        return false;

    if (pAI->me->GetReactionTo(pGo) < REP_NEUTRAL)
        return false;

    if (pGo->GetGoState() != GO_STATE_READY)
        return false;

    return true;
}

static bool MoveToNearbyABOpenFlag(BattleBotAI* pAI)
{
    GameObject* pBestFlag = nullptr;
    float bestDistance = FLT_MAX;

    for (uint32 const bannerId : vFlagsAB)
    {
        if (GameObject* pGo = pAI->me->FindNearestGameObject(bannerId, AB_GUARD_EXCESS_RADIUS))
        {
            if (!IsABFlagOpenable(pAI, pGo))
                continue;

            float const distance = pAI->me->GetDistance(pGo);
            if (distance < bestDistance)
            {
                pBestFlag = pGo;
                bestDistance = distance;
            }
        }
    }

    if (!pBestFlag)
        return false;

    pAI->me->RemoveAurasDueToSpellByCancel(BB_SPELL_FOOD);
    pAI->me->RemoveAurasDueToSpellByCancel(BB_SPELL_DRINK);
    if (pAI->me->GetStandState() != UNIT_STAND_STATE_STAND)
        pAI->me->SetStandState(UNIT_STAND_STATE_STAND);

    pAI->ClearPath();
    pAI->me->GetMotionMaster()->MovePoint(0, pBestFlag->GetPositionX(), pBestFlag->GetPositionY(), pBestFlag->GetPositionZ(), MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE);
    return true;
}

void AB_AtFlag(BattleBotAI* pAI)
{
    if (AtFlag(pAI, vFlagsAB))
        return;

    BattleGround* bg = pAI->me->GetBattleGround();
    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
    {
        if (pAI->me->GetDistance(AB_GuardPositions[i]) > AB_GUARD_EXCESS_RADIUS)
            continue;

        if (!IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), i) &&
            !IsABNodeContestedByTeam(bg, pAI->me->GetTeam(), i))
            continue;

        if (!IsABExcessGuardBot(pAI, AB_GuardPositions[i]))
        {
            pAI->ClearPath();
            pAI->StopMoving();
            return;
        }
    }

    pAI->MoveToNextPoint();
}

static bool ReleaseABExcessGuard(BattleBotAI* pAI)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AB)
        return false;

    if (pAI->m_currentPath || pAI->me->IsMoving() || !pAI->me->IsStopped())
        return false;

    for (uint8 i = 0; i < BG_AB_NODES_MAX; ++i)
    {
        if (!IsABNodeOccupiedByTeam(bg, pAI->me->GetTeam(), i) &&
            !IsABNodeContestedByTeam(bg, pAI->me->GetTeam(), i))
            continue;

        if (!IsABGuardingPosition(pAI->me, AB_GuardPositions[i], true))
            continue;

        if (!IsABExcessGuardBot(pAI, AB_GuardPositions[i]))
            return false;

        pAI->ClearPath();
        if (pAI->StartNewPathToObjective())
            return true;

        if (pAI->StartNewPathFromBeginning())
            return true;

        pAI->StartNewPathFromAnywhere();
        return true;
    }

    return false;
}

std::vector<RecordedMovementPacket> vAllianceGraveyardJumpPath =
{
    { MSG_MOVE_START_FORWARD, 0, 1, 1415.33f, 1554.79f, 343.156f, 2.34205f },
    { MSG_MOVE_START_TURN_LEFT, 187, 17, 1414.42f, 1555.73f, 343.121f, 2.34205f },
    { MSG_MOVE_HEARTBEAT, 500, 17, 1411.19f, 1556.42f, 343.355f, 3.52015f },
    { MSG_MOVE_STOP_TURN, 124, 1, 1410.44f, 1555.99f, 343.451f, 3.81232f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1407.7f, 1553.81f, 343.604f, 3.81232f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1404.96f, 1551.63f, 343.158f, 3.81232f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1402.22f, 1549.46f, 340.935f, 3.81232f },
    { MSG_MOVE_HEARTBEAT, 500, 8193, 1399.47f, 1547.28f, 338.344f, 3.81232f },
    { MSG_MOVE_HEARTBEAT, 500, 24577, 1396.73f, 1545.11f, 333.791f, 3.81232f },
    { MSG_MOVE_FALL_LAND, 402, 1, 1394.27f, 1543.15f, 326.633f, 3.81232f },
    { MSG_MOVE_HEARTBEAT, 500, 8193, 1391.53f, 1540.97f, 324.186f, 3.81232f },
    { MSG_MOVE_FALL_LAND, 197, 1, 1390.44f, 1540.12f, 321.966f, 3.81232f },
    { MSG_MOVE_STOP, 473, 0, 1387.85f, 1538.06f, 321.855f, 3.81232f },
};

std::vector<RecordedMovementPacket> vHordeGraveyardJumpPath =
{
    { MSG_MOVE_START_FORWARD, 0, 1, 1029.14f, 1387.49f, 340.836f, 6.23605f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1032.64f, 1387.33f, 340.57f, 6.23605f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1036.13f, 1387.16f, 340.638f, 6.23605f },
    { MSG_MOVE_START_TURN_LEFT, 154, 17, 1037.21f, 1387.11f, 340.699f, 6.23605f },
    { MSG_MOVE_STOP_TURN, 63, 1, 1037.65f, 1387.12f, 340.705f, 0.101309f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1041.13f, 1387.48f, 340.679f, 0.101309f },
    { MSG_MOVE_START_TURN_LEFT, 311, 17, 1043.3f, 1387.7f, 340.672f, 0.101309f },
    { MSG_MOVE_STOP_TURN, 93, 1, 1043.93f, 1387.83f, 340.67f, 0.320435f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1047.25f, 1388.93f, 340.558f, 0.320435f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1050.58f, 1390.04f, 340.305f, 0.320435f },
    { MSG_MOVE_START_TURN_LEFT, 451, 17, 1053.57f, 1391.03f, 340.038f, 0.320435f },
    { MSG_MOVE_STOP_TURN, 31, 1, 1053.78f, 1391.11f, 340.02f, 0.393477f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1057.01f, 1392.45f, 339.548f, 0.393477f },
    { MSG_MOVE_HEARTBEAT, 500, 8193, 1060.24f, 1393.79f, 337.843f, 0.393477f },
    { MSG_MOVE_HEARTBEAT, 500, 24577, 1063.47f, 1395.13f, 333.618f, 0.393477f },
    { MSG_MOVE_FALL_LAND, 497, 1, 1066.69f, 1396.47f, 324.635f, 0.393477f },
    { MSG_MOVE_START_TURN_LEFT, 297, 17, 1068.61f, 1397.26f, 324.331f, 0.393477f },
    { MSG_MOVE_STOP_TURN, 47, 1, 1068.9f, 1397.41f, 324.296f, 0.504218f },
    { MSG_MOVE_HEARTBEAT, 500, 1, 1071.97f, 1399.1f, 323.823f, 0.504218f },
    { MSG_MOVE_START_TURN_RIGHT, 124, 33, 1072.73f, 1399.52f, 323.799f, 0.504218f },
    { MSG_MOVE_STOP_TURN, 124, 1, 1073.54f, 1399.82f, 323.78f, 0.21205f },
    { MSG_MOVE_START_TURN_RIGHT, 219, 33, 1075.04f, 1400.14f, 323.761f, 0.21205f },
    { MSG_MOVE_STOP_TURN, 78, 1, 1075.58f, 1400.21f, 323.651f, 0.0282667f },
    { MSG_MOVE_STOP, 327, 0, 1077.87f, 1400.27f, 323.154f, 0.0282667f },
};

// Horde Flag Room to Horde Graveyard
BattleBotPath vPath_WSG_HordeFlagRoom_to_HordeGraveyard =
{
    { 933.331f, 1433.72f, 345.536f, &WSG_AtHordeFlag },
    { 944.859f, 1423.05f, 345.437f, nullptr },
    { 966.691f, 1422.53f, 345.223f, nullptr },
    { 979.588f, 1422.84f, 345.46f, nullptr },
    { 997.806f, 1422.52f, 344.623f, nullptr },
    { 1008.53f, 1417.02f, 343.206f, nullptr },
    { 1016.42f, 1402.33f, 341.352f, nullptr },
    { 1029.14f, 1387.49f, 340.836f, &WSG_AtHordeGraveyard },
};
// Horde Graveyard to Horde Tunnel
BattleBotPath vPath_WSG_HordeGraveyard_to_HordeTunnel =
{
    { 1029.14f, 1387.49f, 340.836f, nullptr },
    { 1034.95f, 1392.62f, 340.856f, nullptr },
    { 1038.21f, 1406.43f, 341.562f, nullptr },
    { 1043.87f, 1426.9f, 339.197f, nullptr },
    { 1054.53f, 1441.47f, 339.725f, nullptr },
    { 1056.33f, 1456.03f, 341.463f, nullptr },
    { 1057.39f, 1469.98f, 342.148f, nullptr },
    { 1057.67f, 1487.55f, 342.537f, nullptr },
    { 1048.7f, 1505.37f, 341.117f, nullptr },
    { 1042.19f, 1521.69f, 338.003f, nullptr },
    { 1050.01f, 1538.22f, 332.43f, nullptr },
    { 1068.15f, 1548.1f, 321.446f, nullptr },
    { 1088.14f, 1538.45f, 316.398f, nullptr },
    { 1101.26f, 1522.79f, 314.918f, nullptr },
    { 1114.67f, 1503.18f, 312.947f, nullptr },
    { 1126.45f, 1487.4f, 314.136f, nullptr },
    { 1124.37f, 1462.28f, 315.853f, nullptr },
};
// Horde Tunnel to Horde Flag Room
BattleBotPath vPath_WSG_HordeTunnel_to_HordeFlagRoom =
{
    { 1124.37f, 1462.28f, 315.853f, nullptr },
    { 1106.87f, 1462.13f, 316.558f, nullptr },
    { 1089.44f, 1461.04f, 316.332f, nullptr },
    { 1072.07f, 1459.46f, 317.449f, nullptr },
    { 1051.09f, 1459.89f, 323.126f, nullptr },
    { 1030.1f, 1459.58f, 330.204f, nullptr },
    { 1010.76f, 1457.49f, 334.896f, nullptr },
    { 1005.47f, 1448.19f, 335.864f, nullptr },
    { 999.974f, 1458.49f, 335.632f, nullptr },
    { 982.632f, 1459.18f, 336.127f, nullptr },
    { 965.049f, 1459.15f, 338.076f, nullptr },
    { 944.526f, 1459.0f, 344.207f, nullptr },
    { 937.479f, 1451.12f, 345.553f, nullptr },
    { 933.331f, 1433.72f, 345.536f, &WSG_AtHordeFlag },
};
// Horde Tunnel to Alliance Tunnel 1
BattleBotPath vPath_WSG_HordeTunnel_to_AllianceTunnel_1 =
{
    { 1124.37f, 1462.28f, 315.853f, nullptr },
    { 1135.07f, 1462.43f, 315.569f, nullptr },
    { 1152.2f, 1465.51f, 311.056f, nullptr },
    { 1172.62f, 1470.34f, 306.812f, nullptr },
    { 1193.1f, 1475.0f, 305.155f, nullptr },
    { 1212.99f, 1477.94f, 306.929f, nullptr },
    { 1233.88f, 1476.29f, 308.015f, nullptr },
    { 1250.52f, 1470.94f, 309.8f, nullptr },
    { 1266.09f, 1465.75f, 312.242f, nullptr },
    { 1283.31f, 1463.55f, 311.819f, nullptr },
    { 1297.11f, 1461.2f, 315.485f, nullptr },
    { 1314.31f, 1460.76f, 317.926f, nullptr },
    { 1329.8f, 1461.24f, 320.267f, nullptr },
    { 1348.02f, 1461.06f, 323.167f, nullptr },
};
// Horde Tunnel to Alliance Tunnel 2
BattleBotPath vPath_WSG_HordeTunnel_to_AllianceTunnel_2 =
{
    { 1124.37f, 1462.28f, 315.853f, nullptr },
    { 1138.61f, 1452.12f, 312.988f, nullptr },
    { 1154.35f, 1442.42f, 310.728f, nullptr },
    { 1171.29f, 1438.04f, 307.462f, nullptr },
    { 1185.03f, 1435.43f, 309.484f, nullptr },
    { 1202.24f, 1432.26f, 310.193f, nullptr },
    { 1219.48f, 1429.2f, 310.301f, nullptr },
    { 1235.94f, 1429.97f, 309.727f, nullptr },
    { 1249.3f, 1434.12f, 312.37f, nullptr },
    { 1265.88f, 1439.71f, 314.373f, nullptr },
    { 1282.87f, 1443.85f, 314.907f, nullptr },
    { 1300.06f, 1447.16f, 316.737f, nullptr },
    { 1313.79f, 1449.86f, 317.651f, nullptr },
    { 1329.76f, 1457.36f, 320.37f, nullptr },
    { 1348.02f, 1461.06f, 323.167f, nullptr },
};
// Horde GY Jump to Horde Tunnel
BattleBotPath vPath_WSG_HordeGYJump_to_HordeTunnel =
{
    { 1077.87f, 1400.27f, 323.153f, nullptr },
    { 1088.42f, 1402.68f, 319.605f, nullptr },
    { 1104.34f, 1409.4f, 315.304f, nullptr },
    { 1115.4f, 1418.91f, 313.772f, nullptr },
    { 1122.83f, 1430.74f, 312.765f, nullptr },
    { 1125.26f, 1442.56f, 313.996f, nullptr },
    { 1124.37f, 1462.28f, 315.853f, nullptr },
};
// Horde GY Jump to Alliance Tunnel
BattleBotPath vPath_WSG_HordeGYJump_to_AllianceTunnel =
{
    { 1077.87f, 1400.27f, 323.153f, nullptr },
    { 1091.57f, 1397.37f, 317.739f, nullptr },
    { 1113.14f, 1398.07f, 314.937f, nullptr },
    { 1133.88f, 1401.36f, 314.333f, nullptr },
    { 1151.25f, 1403.39f, 310.679f, nullptr },
    { 1172.17f, 1405.13f, 308.046f, nullptr },
    { 1192.63f, 1409.01f, 306.914f, nullptr },
    { 1212.59f, 1415.38f, 308.805f, nullptr },
    { 1228.5f, 1422.68f, 309.404f, nullptr },
    { 1242.89f, 1431.01f, 310.664f, nullptr },
    { 1259.33f, 1436.99f, 314.488f, nullptr },
    { 1276.1f, 1442.0f, 314.162f, nullptr },
    { 1299.13f, 1450.26f, 317.148f, nullptr },
    { 1315.54f, 1456.24f, 318.449f, nullptr },
    { 1330.63f, 1460.27f, 320.435f, nullptr },
    { 1348.02f, 1461.06f, 323.167f, nullptr },
};
// Alliance Flag Room to Alliance Graveyard
BattleBotPath vPath_WSG_AllianceFlagRoom_to_AllianceGraveyard =
{
    { 1519.53f, 1481.87f, 352.024f, &WSG_AtAllianceFlag },
    { 1508.27f, 1493.17f, 352.005f, nullptr },
    { 1490.78f, 1493.51f, 352.141f, nullptr },
    { 1469.79f, 1494.13f, 351.774f, nullptr },
    { 1453.65f, 1494.39f, 350.614f, nullptr },
    { 1443.51f, 1501.75f, 348.317f, nullptr },
    { 1443.33f, 1517.78f, 345.534f, nullptr },
    { 1443.55f, 1533.4f, 343.148f, nullptr },
    { 1441.47f, 1548.12f, 342.752f, nullptr },
    { 1433.79f, 1552.67f, 342.763f, nullptr },
    { 1422.88f, 1552.37f, 342.751f, nullptr },
    { 1415.33f, 1554.79f, 343.156f, &WSG_AtAllianceGraveyard },
};
// Alliance Graveyard to Alliance Tunnel
BattleBotPath vPath_WSG_AllianceGraveyard_to_AllianceTunnel =
{
    { 1415.33f, 1554.79f, 343.156f, nullptr },
    { 1428.29f, 1551.79f, 342.751f, nullptr },
    { 1441.51f, 1545.79f, 342.757f, nullptr },
    { 1441.15f, 1530.35f, 343.712f, nullptr },
    { 1435.53f, 1517.29f, 346.698f, nullptr },
    { 1424.81f, 1499.24f, 349.486f, nullptr },
    { 1416.31f, 1483.94f, 348.536f, nullptr },
    { 1408.83f, 1468.4f, 347.648f, nullptr },
    { 1404.64f, 1449.79f, 347.279f, nullptr },
    { 1405.34f, 1432.33f, 345.792f, nullptr },
    { 1406.38f, 1416.18f, 344.755f, nullptr },
    { 1400.22f, 1401.87f, 340.496f, nullptr },
    { 1385.96f, 1394.15f, 333.829f, nullptr },
    { 1372.38f, 1390.75f, 328.722f, nullptr },
    { 1362.93f, 1390.02f, 327.034f, nullptr },
    { 1357.91f, 1398.07f, 325.674f, nullptr },
    { 1354.17f, 1411.56f, 324.327f, nullptr },
    { 1351.44f, 1430.38f, 323.506f, nullptr },
    { 1350.36f, 1444.43f, 323.388f, nullptr },
    { 1348.02f, 1461.06f, 323.167f, nullptr },
};
// Alliance Tunnel to Alliance Flag Room
BattleBotPath vPath_WSG_AllianceTunnel_to_AllianceFlagRoom =
{
    { 1348.02f, 1461.06f, 323.167f, nullptr },
    { 1359.8f, 1461.49f, 324.527f, nullptr },
    { 1372.47f, 1461.61f, 324.354f, nullptr },
    { 1389.08f, 1461.12f, 325.913f, nullptr },
    { 1406.57f, 1460.48f, 330.615f, nullptr },
    { 1424.04f, 1459.57f, 336.029f, nullptr },
    { 1442.5f, 1459.7f, 342.024f, nullptr },
    { 1449.59f, 1469.14f, 342.65f, nullptr },
    { 1458.03f, 1458.43f, 342.746f, nullptr },
    { 1469.4f, 1458.14f, 342.794f, nullptr },
    { 1489.06f, 1457.86f, 342.794f, nullptr },
    { 1502.27f, 1457.52f, 347.589f, nullptr },
    { 1512.87f, 1457.81f, 352.039f, nullptr },
    { 1517.53f, 1468.79f, 352.033f, nullptr },
    { 1519.53f, 1481.87f, 352.024f, &WSG_AtAllianceFlag },
};
// Alliance GY Jump to Alliance Tunnel
BattleBotPath vPath_WSG_AllianceGYJump_to_AllianceTunnel =
{
    { 1387.85f, 1538.06f, 321.854f, nullptr },
    { 1376.87f, 1529.48f, 321.66f, nullptr },
    { 1369.76f, 1521.76f, 318.544f, nullptr },
    { 1360.97f, 1508.68f, 320.007f, nullptr },
    { 1355.78f, 1495.7f, 323.959f, nullptr },
    { 1351.58f, 1482.36f, 324.189f, nullptr },
    { 1348.02f, 1461.06f, 323.167f, nullptr },
};
// Alliance GY Jump to Horde Tunnel
BattleBotPath vPath_WSG_AllianceGYJump_to_HordeTunnel =
{
    { 1387.85f, 1538.06f, 321.855f, nullptr },
    { 1377.58f, 1535.88f, 321.053f, nullptr },
    { 1363.98f, 1532.59f, 319.913f, nullptr },
    { 1353.94f, 1529.5f, 316.643f, nullptr },
    { 1340.71f, 1524.94f, 315.246f, nullptr },
    { 1330.75f, 1521.6f, 314.868f, nullptr },
    { 1320.73f, 1518.48f, 316.097f, nullptr },
    { 1307.28f, 1514.6f, 318.134f, nullptr },
    { 1297.12f, 1511.95f, 318.073f, nullptr },
    { 1283.61f, 1508.28f, 316.707f, nullptr },
    { 1273.51f, 1505.39f, 314.615f, nullptr },
    { 1263.49f, 1502.27f, 311.343f, nullptr },
    { 1250.22f, 1497.81f, 309.106f, nullptr },
    { 1237.31f, 1492.4f, 307.577f, nullptr },
    { 1224.04f, 1487.97f, 306.302f, nullptr },
    { 1213.89f, 1485.29f, 305.739f, nullptr },
    { 1203.69f, 1482.8f, 306.177f, nullptr },
    { 1190.05f, 1479.62f, 303.89f, nullptr },
    { 1179.83f, 1477.22f, 303.686f, nullptr },
    { 1169.65f, 1474.65f, 305.842f, nullptr },
    { 1156.05f, 1471.33f, 310.002f, nullptr },
    { 1142.54f, 1467.68f, 311.727f, nullptr },
    { 1135.4f, 1465.54f, 315.622f, nullptr },
    { 1124.37f, 1462.28f, 315.853f, nullptr },
};
// Horde GY Jump to Alliance Flag Room through Side Entrance
BattleBotPath vPath_WSG_HordeGYJump_to_AllianceFlagRoom =
{
    { 1077.87f, 1400.27f, 323.153f, nullptr },
    { 1084.45f, 1388.76f, 319.724f, nullptr },
    { 1088.27f, 1371.39f, 319.17f, nullptr },
    { 1090.71f, 1350.54f, 316.097f, nullptr },
    { 1098.71f, 1332.2f, 317.792f, nullptr },
    { 1109.45f, 1320.92f, 318.267f, nullptr },
    { 1123.49f, 1311.38f, 317.472f, nullptr },
    { 1145.46f, 1302.64f, 317.741f, nullptr },
    { 1168.4f, 1288.85f, 318.053f, nullptr },
    { 1186.49f, 1284.27f, 316.972f, nullptr },
    { 1199.4f, 1286.83f, 317.377f, nullptr },
    { 1215.89f, 1304.02f, 312.822f, nullptr },
    { 1232.18f, 1324.73f, 312.345f, nullptr },
    { 1247.16f, 1329.97f, 315.095f, nullptr },
    { 1269.9f, 1335.18f, 311.879f, nullptr },
    { 1289.97f, 1341.28f, 318.625f, nullptr },
    { 1305.99f, 1347.63f, 321.123f, nullptr },
    { 1325.81f, 1361.58f, 319.39f, nullptr },
    { 1337.48f, 1371.68f, 318.706f, nullptr },
    { 1342.62f, 1390.03f, 321.435f, nullptr },
    { 1352.23f, 1397.97f, 325.547f, nullptr },
    { 1366.38f, 1385.61f, 328.196f, nullptr },
    { 1382.67f, 1380.56f, 332.371f, nullptr },
    { 1395.17f, 1393.12f, 336.183f, nullptr },
    { 1409.03f, 1411.5f, 344.626f, nullptr },
    { 1405.12f, 1438.81f, 346.533f, nullptr },
    { 1409.95f, 1460.93f, 347.687f, nullptr },
    { 1430.87f, 1461.08f, 353.992f, nullptr },
    { 1449.36f, 1459.44f, 358.499f, nullptr },
    { 1471.4f, 1458.48f, 362.557f, nullptr },
    { 1488.64f, 1464.01f, 362.454f, nullptr },
    { 1488.75f, 1474.6f, 358.79f, nullptr },
    { 1490.44f, 1485.99f, 352.112f, nullptr },
    { 1502.97f, 1493.87f, 352.199f, nullptr },
    { 1519.53f, 1481.87f, 352.024f, &WSG_AtAllianceFlag },
};
// Alliance GY Jump to Horde Flag Room through Side Entrance
BattleBotPath vPath_WSG_AllianceGYJump_to_HordeFlagRoom =
{
    { 1387.85f, 1538.06f, 321.855f, nullptr },
    { 1370.13f, 1549.33f, 321.122f, nullptr },
    { 1346.7f, 1564.64f, 316.708f, nullptr },
    { 1324.23f, 1574.24f, 317.11f, nullptr },
    { 1304.03f, 1576.06f, 314.625f, nullptr },
    { 1277.44f, 1569.2f, 312.201f, nullptr },
    { 1249.2f, 1555.53f, 309.172f, nullptr },
    { 1229.95f, 1558.21f, 306.936f, nullptr },
    { 1209.65f, 1573.56f, 308.95f, nullptr },
    { 1189.93f, 1587.73f, 309.608f, nullptr },
    { 1173.76f, 1592.66f, 309.805f, nullptr },
    { 1147.86f, 1590.75f, 310.37f, nullptr },
    { 1124.1f, 1579.89f, 314.881f, nullptr },
    //{ 1102.61f, 1573.98f, 315.804f, nullptr },
    { 1091.28f, 1558.56f, 316.451f, nullptr },
    { 1092.6f, 1547.71f, 316.709f, nullptr },
    { 1086.22f, 1541.5f, 316.924f, nullptr },
    { 1071.64f, 1548.25f, 319.88f, nullptr },
    { 1054.86f, 1544.78f, 328.415f, nullptr },
    { 1043.08f, 1528.67f, 336.984f, nullptr },
    { 1043.21f, 1512.43f, 339.099f, nullptr },
    { 1050.71f, 1485.48f, 342.852f, nullptr },
    { 1042.67f, 1461.07f, 342.305f, nullptr },
    { 1023.13f, 1457.49f, 345.535f, nullptr },
    { 992.797f, 1458.42f, 354.84f, nullptr },
    { 967.257f, 1458.84f, 356.131f, nullptr },
    { 964.566f, 1450.29f, 354.865f, nullptr },
    { 963.586f, 1432.46f, 345.206f, nullptr },
    { 953.017f, 1423.3f, 345.835f, nullptr },
    { 933.331f, 1433.72f, 345.536f, &WSG_AtHordeFlag },
};
// Horde Tunnel to Horde Base Roof
BattleBotPath vPath_WSG_HordeTunnel_to_HordeBaseRoof =
{
    { 1124.37f, 1462.28f, 315.853f, nullptr },
    { 1106.87f, 1462.13f, 316.558f, nullptr },
    { 1089.44f, 1461.04f, 316.332f, nullptr },
    { 1072.07f, 1459.46f, 317.449f, nullptr },
    { 1051.09f, 1459.89f, 323.126f, nullptr },
    { 1030.1f, 1459.58f, 330.204f, nullptr },
    { 1010.76f, 1457.49f, 334.896f, nullptr },
    { 981.948f, 1459.07f, 336.154f, nullptr },
    { 981.768f, 1480.46f, 335.976f, nullptr },
    { 974.664f, 1495.9f, 340.837f, nullptr },
    { 964.661f, 1510.21f, 347.509f, nullptr },
    { 951.188f, 1520.99f, 356.377f, nullptr },
    { 937.37f, 1513.27f, 362.589f, nullptr },
    { 935.947f, 1499.58f, 364.199f, nullptr },
    { 935.9f, 1482.08f, 366.396f, nullptr },
    { 937.564f, 1462.81f, 367.287f, nullptr },
    { 945.871f, 1458.65f, 367.287f, nullptr },
    { 956.972f, 1459.48f, 367.291f, nullptr },
    { 968.317f, 1459.71f, 367.291f, nullptr },
    { 979.934f, 1454.58f, 367.078f, nullptr },
    { 979.99f, 1442.87f, 367.093f, nullptr },
    { 978.632f, 1430.71f, 367.125f, nullptr },
    { 970.395f, 1422.32f, 367.289f, nullptr },
    { 956.338f, 1425.09f, 367.293f, nullptr },
    { 952.778f, 1433.0f, 367.604f, nullptr },
    { 952.708f, 1445.01f, 367.604f, nullptr },
};
// Alliance Tunnel to Alliance Base Roof
BattleBotPath vPath_WSG_AllianceTunnel_to_AllianceBaseRoof =
{
    { 1348.02f, 1461.06f, 323.167f, nullptr },
    { 1359.8f, 1461.49f, 324.527f, nullptr },
    { 1372.47f, 1461.61f, 324.354f, nullptr },
    { 1389.08f, 1461.12f, 325.913f, nullptr },
    { 1406.57f, 1460.48f, 330.615f, nullptr },
    { 1424.04f, 1459.57f, 336.029f, nullptr },
    { 1442.5f, 1459.7f, 342.024f, nullptr },
    { 1471.86f, 1456.65f, 342.794f, nullptr },
    { 1470.93f, 1440.5f, 342.794f, nullptr },
    { 1472.24f, 1427.49f, 342.06f, nullptr },
    { 1476.86f, 1412.46f, 341.426f, nullptr },
    { 1484.42f, 1396.69f, 346.117f, nullptr },
    { 1490.7f, 1387.59f, 351.861f, nullptr },
    { 1500.79f, 1382.98f, 357.784f, nullptr },
    { 1511.08f, 1391.29f, 364.444f, nullptr },
    { 1517.85f, 1403.18f, 370.336f, nullptr },
    { 1517.99f, 1417.59f, 371.636f, nullptr },
    { 1517.07f, 1431.56f, 372.106f, nullptr },
    { 1516.66f, 1445.55f, 372.939f, nullptr },
    { 1514.23f, 1457.37f, 373.689f, nullptr },
    { 1503.73f, 1457.67f, 373.684f, nullptr },
    { 1486.24f, 1457.8f, 373.718f, nullptr },
    { 1476.78f, 1460.35f, 373.711f, nullptr },
    { 1477.37f, 1470.83f, 373.709f, nullptr },
    { 1477.5f, 1484.83f, 373.715f, nullptr },
    { 1480.53f, 1495.26f, 373.721f, nullptr },
    { 1492.61f, 1494.72f, 373.721f, nullptr },
    { 1499.37f, 1489.02f, 373.718f, nullptr },
    { 1500.63f, 1472.89f, 373.707f, nullptr },
};
// Alliance Base to Stables
BattleBotPath vPath_AB_AllianceBase_to_Stables =
{
    { 1285.67f, 1282.14f, -15.8466f, nullptr },
    { 1272.52f, 1267.83f, -21.7811f, nullptr },
    { 1250.44f, 1248.09f, -33.3028f, nullptr },
    { 1232.56f, 1233.05f, -41.5241f, nullptr },
    { 1213.25f, 1224.93f, -47.5513f, nullptr },
    { 1189.29f, 1219.49f, -53.119f, nullptr },
    { 1177.17f, 1210.21f, -56.4593f, nullptr },
    { 1167.98f, 1202.9f, -56.4743f, &AB_AtFlag },
};
// Alliance Base to Gold Mine
BattleBotPath vPath_AB_AllianceBase_to_GoldMine =
{
    { 1285.67f, 1282.14f, -15.8466f, nullptr },
    { 1276.41f, 1267.11f, -20.775f, nullptr },
    { 1261.34f, 1241.52f, -31.2971f, nullptr },
    { 1244.91f, 1219.03f, -41.9658f, nullptr },
    { 1232.25f, 1184.41f, -50.3348f, nullptr },
    { 1226.89f, 1150.82f, -55.7935f, nullptr },
    { 1224.09f, 1120.38f, -57.0633f, nullptr },
    { 1220.03f, 1092.72f, -59.1744f, nullptr },
    { 1216.05f, 1060.86f, -67.2771f, nullptr },
    { 1213.77f, 1027.96f, -74.429f, nullptr },
    { 1208.56f, 998.394f, -81.9493f, nullptr },
    { 1197.42f, 969.73f, -89.9385f, nullptr },
    { 1185.23f, 944.531f, -97.2433f, nullptr },
    { 1166.29f, 913.945f, -107.214f, nullptr },
    { 1153.17f, 887.863f, -112.34f, nullptr },
    { 1148.89f, 871.391f, -111.96f, nullptr },
    { 1145.24f, 850.82f, -110.514f, &AB_AtFlag },
};
// Alliance Base to Lumber Mill
BattleBotPath vPath_AB_AllianceBase_to_LumberMill =
{
    { 1285.67f, 1282.14f, -15.8466f, nullptr },
    { 1269.13f, 1267.89f, -22.7764f, nullptr },
    { 1247.79f, 1249.77f, -33.2518f, nullptr },
    { 1226.29f, 1232.02f, -43.9193f, nullptr },
    { 1196.68f, 1230.15f, -50.4644f, nullptr },
    { 1168.72f, 1228.98f, -53.9329f, nullptr },
    { 1140.82f, 1226.7f, -53.6318f, nullptr },
    { 1126.85f, 1225.77f, -47.98f, nullptr },
    { 1096.5f, 1226.57f, -53.1769f, nullptr },
    { 1054.52f, 1226.14f, -49.2011f, nullptr },
    { 1033.52f, 1226.08f, -45.5968f, nullptr },
    { 1005.52f, 1226.08f, -43.2912f, nullptr },
    { 977.53f, 1226.68f, -40.16f, nullptr },
    { 957.242f, 1227.94f, -34.1487f, nullptr },
    { 930.689f, 1221.57f, -18.9588f, nullptr },
    { 918.202f, 1211.98f, -12.2494f, nullptr },
    { 880.329f, 1192.63f, 7.61168f, nullptr },
    { 869.965f, 1178.52f, 10.9678f, nullptr },
    { 864.74f, 1163.78f, 12.385f, nullptr },
    { 859.165f, 1148.84f, 11.5289f, &AB_AtFlag },
};
// Stables to Blacksmith
BattleBotPath vPath_AB_Stables_to_Blacksmith =
{
    { 1169.52f, 1198.71f, -56.2742f, &AB_AtFlag },
    { 1166.93f, 1185.2f, -56.3634f, nullptr },
    { 1173.84f, 1170.6f, -56.4094f, nullptr },
    { 1186.7f, 1163.92f, -56.3961f, nullptr },
    { 1189.7f, 1150.68f, -55.8664f, nullptr },
    { 1185.18f, 1129.31f, -58.1044f, nullptr },
    { 1181.7f, 1108.6f, -62.1797f, nullptr },
    { 1177.92f, 1087.95f, -63.5768f, nullptr },
    { 1174.52f, 1067.23f, -64.402f, nullptr },
    { 1171.27f, 1051.09f, -65.0833f, nullptr },
    { 1163.22f, 1031.7f, -64.954f, nullptr },
    { 1154.25f, 1010.25f, -63.5299f, nullptr },
    { 1141.07f, 999.479f, -63.3713f, nullptr },
    { 1127.12f, 1000.37f, -60.628f, nullptr },
    { 1106.17f, 1001.66f, -61.7457f, nullptr },
    { 1085.64f, 1005.62f, -58.5932f, nullptr },
    { 1064.88f, 1008.65f, -52.3547f, nullptr },
    { 1044.16f, 1011.96f, -47.2647f, nullptr },
    { 1029.72f, 1014.88f, -45.3546f, nullptr },
    { 1013.94f, 1028.7f, -43.9786f, nullptr },
    { 990.89f, 1039.3f, -42.7659f, nullptr },
    { 978.269f, 1043.84f, -44.4588f, &AB_AtFlag },
};
// Horde Base to Farm
BattleBotPath vPath_AB_HordeBase_to_Farm =
{
    { 707.259f, 707.839f, -17.5318f, nullptr },
    { 712.063f, 712.928f, -20.1802f, nullptr },
    { 725.941f, 728.682f, -29.7536f, nullptr },
    { 734.715f, 739.591f, -35.2144f, nullptr },
    { 747.607f, 756.161f, -40.899f, nullptr },
    { 753.994f, 766.668f, -43.3049f, nullptr },
    { 758.715f, 787.106f, -46.7014f, nullptr },
    { 762.077f, 807.831f, -48.4721f, nullptr },
    { 764.132f, 821.68f, -49.656f, nullptr },
    { 767.947f, 839.274f, -50.8574f, nullptr },
    { 773.745f, 852.013f, -52.6226f, nullptr },
    { 785.123f, 869.103f, -54.2089f, nullptr },
    { 804.429f, 874.961f, -55.2691f, &AB_AtFlag },
};
// Horde Base to Gold Mine
BattleBotPath vPath_AB_HordeBase_to_GoldMine =
{
    { 707.259f, 707.839f, -17.5318f, nullptr },
    { 717.935f, 716.874f, -23.3941f, nullptr },
    { 739.195f, 732.483f, -34.5791f, nullptr },
    { 757.087f, 742.008f, -38.1123f, nullptr },
    { 776.946f, 748.775f, -42.7346f, nullptr },
    { 797.138f, 754.539f, -46.3237f, nullptr },
    { 817.37f, 760.167f, -48.9235f, nullptr },
    { 837.638f, 765.664f, -49.7374f, nullptr },
    { 865.092f, 774.738f, -51.9831f, nullptr },
    { 878.86f, 777.149f, -47.2361f, nullptr },
    { 903.911f, 780.212f, -53.1424f, nullptr },
    { 923.454f, 787.888f, -54.7937f, nullptr },
    { 946.218f, 798.93f, -59.0904f, nullptr },
    { 978.1f, 813.321f, -66.7268f, nullptr },
    { 1002.94f, 817.895f, -77.3119f, nullptr },
    { 1030.77f, 820.92f, -88.7717f, nullptr },
    { 1058.61f, 823.889f, -94.1623f, nullptr },
    { 1081.6f, 828.32f, -99.4137f, nullptr },
    { 1104.64f, 844.773f, -106.387f, nullptr },
    { 1117.56f, 853.686f, -110.716f, nullptr },
    { 1144.9f, 850.049f, -110.522f, &AB_AtFlag },
};
// Horde Base to Lumber Mill
BattleBotPath vPath_AB_HordeBase_to_LumberMill =
{
    { 707.259f, 707.839f, -17.5318f, nullptr },
    { 721.611f, 726.507f, -27.9646f, nullptr },
    { 733.846f, 743.573f, -35.8633f, nullptr },
    { 746.201f, 760.547f, -40.838f, nullptr },
    { 758.937f, 787.565f, -46.741f, nullptr },
    { 761.289f, 801.357f, -48.0037f, nullptr },
    { 764.341f, 822.128f, -49.6908f, nullptr },
    { 769.766f, 842.244f, -51.1239f, nullptr },
    { 775.322f, 855.093f, -53.1161f, nullptr },
    { 783.995f, 874.216f, -55.0822f, nullptr },
    { 789.917f, 886.902f, -56.2935f, nullptr },
    { 798.03f, 906.259f, -57.1162f, nullptr },
    { 803.183f, 919.266f, -57.6692f, nullptr },
    { 813.248f, 937.688f, -57.7106f, nullptr },
    { 820.412f, 958.712f, -56.1492f, nullptr },
    { 814.247f, 973.692f, -50.4602f, nullptr },
    { 807.697f, 985.502f, -47.2383f, nullptr },
    { 795.672f, 1002.69f, -44.9382f, nullptr },
    { 784.653f, 1020.77f, -38.6278f, nullptr },
    { 784.826f, 1037.34f, -31.5719f, nullptr },
    { 786.083f, 1051.28f, -24.0793f, nullptr },
    { 787.314f, 1065.23f, -16.8918f, nullptr },
    { 788.892f, 1086.17f, -6.42608f, nullptr },
    { 792.077f, 1106.53f, 4.81124f, nullptr },
    { 800.398f, 1119.48f, 8.5814f, nullptr },
    { 812.476f, 1131.1f, 10.439f, nullptr },
    { 829.704f, 1142.52f, 10.738f, nullptr },
    { 842.646f, 1143.51f, 11.9984f, nullptr },
    { 857.674f, 1146.16f, 11.529f, &AB_AtFlag },
};
// Farm to Blacksmith
BattleBotPath vPath_AB_Farm_to_Blacksmith =
{
    { 803.826f, 874.909f, -55.2547f, &AB_AtFlag },
    { 808.763f, 887.991f, -57.4437f, nullptr },
    { 818.33f, 906.674f, -59.3554f, nullptr },
    { 828.634f, 924.972f, -60.5664f, nullptr },
    { 835.255f, 937.308f, -60.2915f, nullptr },
    { 845.244f, 955.78f, -60.4208f, nullptr },
    { 852.125f, 967.965f, -61.3135f, nullptr },
    { 863.232f, 983.109f, -62.6402f, nullptr },
    { 875.413f, 989.245f, -61.2916f, nullptr },
    { 895.765f, 994.41f, -63.6287f, nullptr },
    { 914.16f, 1001.09f, -58.37f, nullptr },
    { 932.418f, 1011.44f, -51.9225f, nullptr },
    { 944.244f, 1018.92f, -49.1438f, nullptr },
    { 961.55f, 1030.81f, -45.814f, nullptr },
    { 978.122f, 1043.87f, -44.4682f, &AB_AtFlag },
};
// Stables to Gold Mine
BattleBotPath vPath_AB_Stables_to_GoldMine =
{
    { 1169.52f, 1198.71f, -56.2742f, &AB_AtFlag },
    { 1166.72f, 1183.58f, -56.3633f, nullptr },
    { 1172.14f, 1170.99f, -56.4735f, nullptr },
    { 1185.18f, 1164.02f, -56.4269f, nullptr },
    { 1193.98f, 1155.85f, -55.924f, nullptr },
    { 1201.51f, 1145.65f, -56.4733f, nullptr },
    { 1205.39f, 1134.81f, -56.2366f, nullptr },
    { 1207.57f, 1106.9f, -58.4748f, nullptr },
    { 1209.4f, 1085.98f, -63.4022f, nullptr },
    { 1212.68f, 1065.25f, -66.514f, nullptr },
    { 1216.42f, 1037.52f, -72.0457f, nullptr },
    { 1215.4f, 1011.56f, -78.3338f, nullptr },
    { 1209.8f, 992.293f, -83.2433f, nullptr },
    { 1201.23f, 973.121f, -88.5661f, nullptr },
    { 1192.16f, 954.183f, -94.2209f, nullptr },
    { 1181.88f, 935.894f, -99.5239f, nullptr },
    { 1169.86f, 918.68f, -105.588f, nullptr },
    { 1159.36f, 900.497f, -110.461f, nullptr },
    { 1149.32f, 874.429f, -112.142f, nullptr },
    { 1145.34f, 849.824f, -110.523f, &AB_AtFlag },
};
// Stables to Lumber Mill
BattleBotPath vPath_AB_Stables_to_LumberMill =
{
    { 1169.52f, 1198.71f, -56.2742f, &AB_AtFlag },
    { 1169.33f, 1203.43f, -56.5457f, nullptr },
    { 1164.77f, 1208.73f, -56.1907f, nullptr },
    { 1141.52f, 1224.99f, -53.8204f, nullptr },
    { 1127.54f, 1224.82f, -48.2081f, nullptr },
    { 1106.56f, 1225.58f, -50.5154f, nullptr },
    { 1085.6f, 1226.54f, -53.1863f, nullptr },
    { 1064.6f, 1226.82f, -50.4381f, nullptr },
    { 1043.6f, 1227.27f, -46.5439f, nullptr },
    { 1022.61f, 1227.72f, -44.7157f, nullptr },
    { 1001.61f, 1227.62f, -42.6876f, nullptr },
    { 980.623f, 1226.93f, -40.4687f, nullptr },
    { 959.628f, 1227.1f, -35.3838f, nullptr },
    { 938.776f, 1226.34f, -23.5399f, nullptr },
    { 926.138f, 1217.21f, -16.2176f, nullptr },
    { 911.966f, 1205.99f, -9.69655f, nullptr },
    { 895.135f, 1198.85f, -0.546275f, nullptr },
    { 873.419f, 1189.27f, 9.3466f, nullptr },
    { 863.821f, 1181.72f, 9.76912f, nullptr },
    { 851.803f, 1166.3f, 10.4423f, nullptr },
    { 853.921f, 1150.92f, 11.543f, &AB_AtFlag },
};
// Farm to Gold Mine
BattleBotPath vPath_AB_Farm_to_GoldMine =
{
    { 803.826f, 874.909f, -55.2547f, &AB_AtFlag },
    { 801.662f, 865.689f, -56.9445f, nullptr },
    { 806.433f, 860.776f, -57.5899f, nullptr },
    { 816.236f, 857.397f, -57.7029f, nullptr },
    { 826.717f, 855.846f, -57.9914f, nullptr },
    { 836.128f, 851.257f, -57.8321f, nullptr },
    { 847.933f, 843.837f, -58.1296f, nullptr },
    { 855.08f, 832.688f, -57.7373f, nullptr },
    { 864.513f, 813.663f, -57.574f, nullptr },
    { 864.229f, 797.762f, -54.2057f, nullptr },
    { 862.967f, 787.372f, -53.0276f, nullptr },
    { 864.163f, 776.33f, -52.0372f, nullptr },
    { 872.583f, 777.391f, -48.5342f, nullptr },
    { 893.575f, 777.922f, -49.1826f, nullptr },
    { 915.941f, 783.534f, -53.6598f, nullptr },
    { 928.105f, 789.929f, -55.4802f, nullptr },
    { 946.263f, 800.46f, -59.166f, nullptr },
    { 958.715f, 806.845f, -62.1494f, nullptr },
    { 975.79f, 811.913f, -65.9648f, nullptr },
    { 989.468f, 814.883f, -71.3089f, nullptr },
    { 1010.13f, 818.643f, -80.0817f, nullptr },
    { 1023.97f, 820.667f, -86.1114f, nullptr },
    { 1044.84f, 823.011f, -92.0583f, nullptr },
    { 1058.77f, 824.482f, -94.1937f, nullptr },
    { 1079.13f, 829.402f, -99.1207f, nullptr },
    { 1092.85f, 836.986f, -102.755f, nullptr },
    { 1114.75f, 851.21f, -109.782f, nullptr },
    { 1128.22f, 851.928f, -111.078f, nullptr },
    { 1145.14f, 849.895f, -110.523f, &AB_AtFlag },
};
// Farm to Lumber Mill
BattleBotPath vPath_AB_Farm_to_LumberMill =
{
    { 803.826f, 874.909f, -55.2547f, &AB_AtFlag },
    { 802.874f, 894.28f, -56.4661f, nullptr },
    { 806.844f, 920.39f, -57.3157f, nullptr },
    { 814.003f, 934.161f, -57.6065f, nullptr },
    { 824.594f, 958.47f, -58.4916f, nullptr },
    { 820.434f, 971.184f, -53.201f, nullptr },
    { 808.339f, 987.79f, -47.5705f, nullptr },
    { 795.98f, 1004.76f, -44.9189f, nullptr },
    { 785.497f, 1019.18f, -39.2806f, nullptr },
    { 783.94f, 1032.46f, -33.5692f, nullptr },
    { 784.956f, 1053.41f, -22.8368f, nullptr },
    { 787.499f, 1074.25f, -12.4232f, nullptr },
    { 789.406f, 1088.11f, -5.28606f, nullptr },
    { 794.617f, 1109.17f, 6.1966f, nullptr },
    { 801.514f, 1120.77f, 8.81455f, nullptr },
    { 817.3f, 1134.59f, 10.6064f, nullptr },
    { 828.961f, 1142.98f, 10.7354f, nullptr },
    { 841.63f, 1147.75f, 11.6916f, nullptr },
    { 854.326f, 1150.55f, 11.537f, &AB_AtFlag },
};

std::vector<BattleBotPath*> const vPaths_WS =
{
    &vPath_WSG_HordeFlagRoom_to_HordeGraveyard,
    &vPath_WSG_HordeGraveyard_to_HordeTunnel,
    &vPath_WSG_HordeTunnel_to_HordeFlagRoom,
    &vPath_WSG_HordeTunnel_to_AllianceTunnel_1,
    &vPath_WSG_HordeTunnel_to_AllianceTunnel_2,
    &vPath_WSG_HordeGYJump_to_HordeTunnel,
    &vPath_WSG_HordeGYJump_to_AllianceTunnel,
    &vPath_WSG_AllianceFlagRoom_to_AllianceGraveyard,
    &vPath_WSG_AllianceGraveyard_to_AllianceTunnel,
    &vPath_WSG_AllianceTunnel_to_AllianceFlagRoom,
    &vPath_WSG_AllianceGYJump_to_AllianceTunnel,
    &vPath_WSG_AllianceGYJump_to_HordeTunnel,
    &vPath_WSG_HordeGYJump_to_AllianceFlagRoom,
    &vPath_WSG_AllianceGYJump_to_HordeFlagRoom,
    &vPath_WSG_HordeTunnel_to_HordeBaseRoof,
    &vPath_WSG_AllianceTunnel_to_AllianceBaseRoof,
};

// Blacksmith to Stables
BattleBotPath vPath_AB_Blacksmith_to_Stables =
{
    { 978.0571f, 1043.0000f, -44.4109f, &AB_AtFlag },
    { 983.6119f, 1040.1910f, -43.7337f, nullptr },
    { 991.3457f, 1037.7684f, -42.8333f, nullptr },
    { 997.8841f, 1037.5361f, -42.8318f, nullptr },
    { 1004.0986f, 1037.3127f, -43.1018f, nullptr },
    { 1010.7835f, 1033.1281f, -43.7374f, nullptr },
    { 1015.8127f, 1028.2643f, -44.1043f, nullptr },
    { 1020.7307f, 1023.2830f, -44.6280f, nullptr },
    { 1024.8656f, 1019.2150f, -45.0386f, nullptr },
    { 1029.6281f, 1016.3548f, -45.3577f, nullptr },
    { 1034.8937f, 1014.8676f, -45.9056f, nullptr },
    { 1041.6956f, 1013.2142f, -46.8829f, nullptr },
    { 1048.4982f, 1011.5632f, -48.2800f, nullptr },
    { 1055.3243f, 1010.0143f, -49.6164f, nullptr },
    { 1062.1580f, 1008.4973f, -51.5530f, nullptr },
    { 1068.9916f, 1006.9802f, -53.7095f, nullptr },
    { 1075.8252f, 1005.4631f, -56.2210f, nullptr },
    { 1082.6588f, 1003.9460f, -57.9498f, nullptr },
    { 1089.4948f, 1002.4400f, -59.4998f, nullptr },
    { 1096.4270f, 1001.4749f, -60.7221f, nullptr },
    { 1103.3939f, 1000.8220f, -61.3886f, nullptr },
    { 1110.3790f, 1000.3651f, -61.7335f, nullptr },
    { 1117.3641f, 999.9082f, -60.6289f, nullptr },
    { 1124.3492f, 999.4514f, -60.4475f, nullptr },
    { 1131.3342f, 998.9945f, -61.0967f, nullptr },
    { 1137.2102f, 999.2244f, -62.5208f, nullptr },
    { 1143.9512f, 999.9784f, -63.7019f, nullptr },
    { 1148.8907f, 999.0661f, -63.7880f, nullptr },
    { 1153.3779f, 1002.9023f, -63.9264f, nullptr },
    { 1156.9161f, 1007.3471f, -63.8866f, nullptr },
    { 1160.0549f, 1013.5645f, -63.8768f, nullptr },
    { 1162.1479f, 1020.2434f, -64.2108f, nullptr },
    { 1164.0912f, 1026.9683f, -64.8316f, nullptr },
    { 1166.0344f, 1033.6931f, -65.1194f, nullptr },
    { 1167.9777f, 1040.4180f, -65.3254f, nullptr },
    { 1169.9209f, 1047.1428f, -65.2136f, nullptr },
    { 1171.8634f, 1053.8652f, -65.0067f, nullptr },
    { 1173.8066f, 1060.5902f, -64.8289f, nullptr },
    { 1175.3979f, 1067.4026f, -64.4933f, nullptr },
    { 1176.9407f, 1074.2305f, -64.2891f, nullptr },
    { 1178.6691f, 1081.0134f, -63.9791f, nullptr },
    { 1180.4652f, 1087.7788f, -63.5526f, nullptr },
    { 1182.6963f, 1094.4126f, -63.1850f, nullptr },
    { 1185.1036f, 1100.9857f, -62.9805f, nullptr },
    { 1187.4760f, 1107.5712f, -62.3779f, nullptr },
    { 1189.7078f, 1114.2058f, -60.9543f, nullptr },
    { 1191.7677f, 1120.8943f, -59.3935f, nullptr },
    { 1193.4540f, 1127.6879f, -58.0266f, nullptr },
    { 1195.1050f, 1134.4904f, -56.7955f, nullptr },
    { 1196.4174f, 1141.3632f, -56.4489f, nullptr },
    { 1196.5293f, 1146.5991f, -56.2016f, nullptr },
    { 1193.3751f, 1154.1067f, -55.8396f, nullptr },
    { 1189.6846f, 1159.2571f, -56.1894f, nullptr },
    { 1184.6456f, 1164.1118f, -56.4343f, nullptr },
    { 1179.4800f, 1168.8357f, -56.3987f, nullptr },
    { 1174.5780f, 1173.8301f, -56.3827f, nullptr },
    { 1169.8561f, 1178.9976f, -56.4511f, nullptr },
    { 1166.5594f, 1183.3514f, -56.3632f, nullptr },
    { 1163.8148f, 1190.4664f, -56.3484f, nullptr },
    { 1163.6088f, 1197.9747f, -56.5324f, nullptr },
    { 1166.9785f, 1203.6083f, -56.5134f, &AB_AtFlag },
};

// Blacksmith to Farm
BattleBotPath vPath_AB_Blacksmith_to_Farm =
{
    { 978.0000f, 1043.0000f, -44.4136f, &AB_AtFlag },
    { 973.6752f, 1035.9166f, -43.4421f, nullptr },
    { 971.6692f, 1029.8945f, -43.8791f, nullptr },
    { 966.7489f, 1024.4343f, -44.8338f, nullptr },
    { 962.3774f, 1020.1698f, -45.3716f, nullptr },
    { 958.9289f, 1014.4613f, -45.8917f, nullptr },
    { 955.1345f, 1009.7889f, -46.9859f, nullptr },
    { 951.0858f, 1006.4722f, -47.8565f, nullptr },
    { 944.9265f, 1003.1562f, -49.2848f, nullptr },
    { 938.3211f, 1000.8506f, -51.1514f, nullptr },
    { 931.4842f, 999.3743f, -52.8905f, nullptr },
    { 924.5352f, 998.5622f, -54.9523f, nullptr },
    { 917.5762f, 997.8118f, -57.1427f, nullptr },
    { 910.6666f, 996.6919f, -59.6239f, nullptr },
    { 903.7587f, 995.5598f, -62.4756f, nullptr },
    { 896.8658f, 994.3464f, -63.7230f, nullptr },
    { 890.0185f, 992.8922f, -62.3768f, nullptr },
    { 883.2240f, 991.2242f, -61.3594f, nullptr },
    { 876.6078f, 988.9500f, -61.3617f, nullptr },
    { 870.0863f, 986.4077f, -62.3143f, nullptr },
    { 863.6147f, 983.7459f, -62.7174f, nullptr },
    { 858.1229f, 980.6586f, -61.6248f, nullptr },
    { 852.8593f, 976.0525f, -61.0891f, nullptr },
    { 847.7943f, 971.2209f, -60.9012f, nullptr },
    { 842.8646f, 966.2513f, -60.7623f, nullptr },
    { 837.9415f, 961.2750f, -60.7306f, nullptr },
    { 833.0217f, 956.3022f, -60.3636f, nullptr },
    { 828.1304f, 951.2949f, -59.7396f, nullptr },
    { 823.4440f, 946.0997f, -58.7521f, nullptr },
    { 819.3036f, 940.4562f, -58.0571f, nullptr },
    { 815.1973f, 934.7872f, -57.5740f, nullptr },
    { 811.1725f, 929.0630f, -57.5143f, nullptr },
    { 807.5568f, 923.0693f, -57.3165f, nullptr },
    { 803.9549f, 917.0671f, -57.3517f, nullptr },
    { 801.5123f, 912.0548f, -57.2261f, nullptr },
    { 798.5981f, 905.6909f, -57.0580f, nullptr },
    { 796.1498f, 899.1385f, -56.6290f, nullptr },
    { 794.7695f, 894.0766f, -56.5687f, nullptr },
    { 794.7698f, 887.9856f, -56.1391f, nullptr },
    { 796.4748f, 882.9484f, -55.1992f, nullptr },
    { 801.0659f, 878.1399f, -55.4924f, nullptr },
    { 805.1912f, 874.2695f, -55.3920f, &AB_AtFlag },
};

// Lumber Mill to Blacksmith
BattleBotPath vPath_AB_LumberMill_to_Blacksmith =
{
    { 853.0000f, 1150.0000f, 11.5482f, &AB_AtFlag },
    { 846.6965f, 1149.4784f, 11.8048f, nullptr },
    { 839.7022f, 1149.1937f, 11.2617f, nullptr },
    { 833.7174f, 1147.7544f, 10.9033f, nullptr },
    { 827.4684f, 1144.6104f, 10.7350f, nullptr },
    { 821.4099f, 1141.1101f, 10.6660f, nullptr },
    { 815.7768f, 1136.9749f, 10.5681f, nullptr },
    { 809.7383f, 1131.1328f, 10.2445f, nullptr },
    { 805.1579f, 1125.8395f, 9.5738f, nullptr },
    { 800.8204f, 1120.3518f, 8.7075f, nullptr },
    { 796.7669f, 1114.6453f, 7.7643f, nullptr },
    { 793.7538f, 1107.1805f, 5.2485f, nullptr },
    { 792.0230f, 1100.4011f, 1.4933f, nullptr },
    { 790.8270f, 1093.5042f, -2.2194f, nullptr },
    { 789.6381f, 1086.6057f, -6.1592f, nullptr },
    { 788.8583f, 1079.6500f, -9.8517f, nullptr },
    { 788.1545f, 1072.6855f, -13.2066f, nullptr },
    { 787.4506f, 1065.7209f, -16.6364f, nullptr },
    { 786.7468f, 1058.7565f, -20.1814f, nullptr },
    { 786.0430f, 1051.7920f, -23.7752f, nullptr },
    { 785.3392f, 1044.8274f, -27.8713f, nullptr },
    { 784.6473f, 1037.8617f, -31.3345f, nullptr },
    { 783.9982f, 1030.8918f, -34.2154f, nullptr },
    { 783.9387f, 1025.8713f, -36.3576f, nullptr },
    { 785.6446f, 1020.5839f, -38.8318f, nullptr },
    { 789.0717f, 1014.8658f, -41.5380f, nullptr },
    { 795.2316f, 1009.7592f, -44.0052f, nullptr },
    { 801.2617f, 1006.2041f, -46.4115f, nullptr },
    { 807.3894f, 1002.8260f, -48.6309f, nullptr },
    { 813.6630f, 999.7249f, -50.3783f, nullptr },
    { 820.0469f, 996.8534f, -52.3870f, nullptr },
    { 826.6205f, 994.4538f, -54.3685f, nullptr },
    { 833.3067f, 992.3834f, -56.2219f, nullptr },
    { 840.1157f, 990.7637f, -58.1047f, nullptr },
    { 846.9370f, 989.1924f, -59.3941f, nullptr },
    { 853.7850f, 987.7476f, -60.4551f, nullptr },
    { 858.9025f, 987.4193f, -61.3304f, nullptr },
    { 863.9248f, 987.5729f, -62.5132f, nullptr },
    { 870.8471f, 988.5775f, -61.8803f, nullptr },
    { 877.6768f, 990.1121f, -61.2269f, nullptr },
    { 884.5027f, 991.6630f, -61.4470f, nullptr },
    { 891.2877f, 993.3843f, -62.6715f, nullptr },
    { 898.0723f, 995.1077f, -63.7754f, nullptr },
    { 904.8768f, 996.7106f, -61.9482f, nullptr },
    { 912.9575f, 997.0508f, -58.8050f, nullptr },
    { 918.8525f, 997.2437f, -56.7385f, nullptr },
    { 925.8206f, 997.8917f, -54.5483f, nullptr },
    { 932.6587f, 999.3828f, -52.6284f, nullptr },
    { 939.1466f, 1001.3581f, -50.8376f, nullptr },
    { 944.3173f, 1003.7301f, -49.3373f, nullptr },
    { 950.1145f, 1007.6470f, -47.8043f, nullptr },
    { 955.7177f, 1011.8428f, -46.6183f, nullptr },
    { 961.1299f, 1017.2438f, -45.6432f, nullptr },
    { 966.4512f, 1021.7886f, -45.0418f, nullptr },
    { 971.8815f, 1026.2050f, -44.3456f, nullptr },
    { 975.9669f, 1030.6169f, -44.0101f, nullptr },
    { 979.2751f, 1035.8699f, -43.8887f, nullptr },
    { 976.7523f, 1043.1355f, -44.4748f, &AB_AtFlag },
};

// Horde central GY to Blacksmith
BattleBotPath vPath_AB_HordeGY_to_Blacksmith =
{
    { 1016.0000f, 955.0000f, -43.0000f, nullptr },
    { 1015.1665f, 961.9457f, -43.1131f, nullptr },
    { 1015.3206f, 968.9413f, -43.6918f, nullptr },
    { 1013.4579f, 976.4611f, -43.9023f, nullptr },
    { 1011.5610f, 983.1973f, -43.8936f, nullptr },
    { 1010.8018f, 989.0358f, -44.0713f, nullptr },
    { 1012.1433f, 994.9338f, -44.4553f, nullptr },
    { 1015.8508f, 1000.8707f, -44.5165f, nullptr },
    { 1019.6274f, 1006.7645f, -44.6318f, nullptr },
    { 1022.8658f, 1012.9299f, -44.8824f, nullptr },
    { 1022.7216f, 1018.4158f, -45.0546f, nullptr },
    { 1018.9384f, 1024.4022f, -44.5235f, nullptr },
    { 1013.8049f, 1029.5848f, -43.9398f, nullptr },
    { 1008.6189f, 1034.2860f, -43.5381f, nullptr },
    { 1003.8159f, 1036.5104f, -43.1339f, nullptr },
    { 998.1559f, 1037.7864f, -42.8235f, nullptr },
    { 991.1607f, 1037.9363f, -42.8306f, nullptr },
    { 985.4360f, 1038.8873f, -43.4215f, nullptr },
    { 981.0193f, 1042.7747f, -44.1746f, &AB_AtFlag },
};

// Alliance NW GY (near LumberMill) to LumberMill
BattleBotPath vPath_AB_AllianceNWGY_to_LumberMill =
{
    { 772.0000f, 1213.0000f, 15.7974f, nullptr },
    { 773.9371f, 1206.2734f, 15.7974f, nullptr },
    { 775.8870f, 1199.5505f, 15.6212f, nullptr },
    { 777.8370f, 1192.8275f, 14.7980f, nullptr },
    { 779.6102f, 1186.0590f, 13.7765f, nullptr },
    { 781.0338f, 1179.2056f, 13.0062f, nullptr },
    { 783.3770f, 1172.7410f, 12.7134f, nullptr },
    { 786.0655f, 1167.9785f, 12.6439f, nullptr },
    { 790.1926f, 1163.3259f, 12.4911f, nullptr },
    { 795.1788f, 1158.4128f, 12.1032f, nullptr },
    { 800.0654f, 1153.4014f, 11.8117f, nullptr },
    { 804.4689f, 1149.6541f, 11.5540f, nullptr },
    { 808.9541f, 1147.1547f, 11.4720f, nullptr },
    { 816.4813f, 1145.3860f, 10.8988f, nullptr },
    { 822.8158f, 1145.4310f, 10.7236f, nullptr },
    { 827.8175f, 1146.5604f, 10.7703f, nullptr },
    { 833.0098f, 1149.9618f, 10.9759f, nullptr },
    { 838.2422f, 1153.1292f, 11.1878f, nullptr },
    { 843.6024f, 1154.4913f, 11.2567f, nullptr },
    { 850.0904f, 1152.7222f, 11.5520f, nullptr },
    { 855.9973f, 1150.1223f, 11.5296f, &AB_AtFlag },
};

std::vector<BattleBotPath*> const vPaths_AB =
{
    &vPath_AB_AllianceBase_to_Stables,
    &vPath_AB_AllianceBase_to_GoldMine,
    &vPath_AB_AllianceBase_to_LumberMill,
    &vPath_AB_Stables_to_Blacksmith,
    &vPath_AB_HordeBase_to_Farm,
    &vPath_AB_HordeBase_to_GoldMine,
    &vPath_AB_HordeBase_to_LumberMill,
    &vPath_AB_Farm_to_Blacksmith,
    &vPath_AB_Stables_to_GoldMine,
    &vPath_AB_Stables_to_LumberMill,
    &vPath_AB_Farm_to_GoldMine,
    &vPath_AB_Farm_to_LumberMill,
    &vPath_AB_Blacksmith_to_Stables,
    &vPath_AB_Blacksmith_to_Farm,
    &vPath_AB_LumberMill_to_Blacksmith,
    &vPath_AB_HordeGY_to_Blacksmith,
    &vPath_AB_AllianceNWGY_to_LumberMill,
};

static bool BattleBotNeedsRecovery(BattleBotAI* pAI)
{
    bool const needToEat = pAI->me->GetHealthPercent() < 90.0f;
    bool needToDrink = (pAI->me->GetPowerType() == POWER_MANA) && (pAI->me->GetPowerPercent(POWER_MANA) < 90.0f);
    if (needToDrink &&
        pAI->me->GetClass() == CLASS_DRUID &&
        pAI->me->GetShapeshiftForm() != FORM_NONE &&
       (pAI->GetRole() == ROLE_MELEE_DPS || pAI->GetRole() == ROLE_TANK))
        needToDrink = false;

    return needToEat || needToDrink || pAI->me->HasAura(BB_SPELL_FOOD) || pAI->me->HasAura(BB_SPELL_DRINK);
}

static bool MoveGuardBackBeforeRecovery(BattleBotAI* pAI, Position const& guardPosition, float readyRadius, std::vector<BattleBotPath*> const* paths, bool use2dDistance = false)
{
    float const distance = use2dDistance ? pAI->me->GetDistance2d(guardPosition) : pAI->me->GetDistance(guardPosition);
    if (distance <= readyRadius)
        return false;

    pAI->me->RemoveAurasDueToSpellByCancel(BB_SPELL_FOOD);
    pAI->me->RemoveAurasDueToSpellByCancel(BB_SPELL_DRINK);
    if (pAI->me->GetStandState() != UNIT_STAND_STATE_STAND)
        pAI->me->SetStandState(UNIT_STAND_STATE_STAND);

    pAI->ClearPath();
    if (paths && pAI->StartNewPathToPosition(guardPosition, *paths))
        return true;

    pAI->me->GetMotionMaster()->MovePoint(0, guardPosition.x, guardPosition.y, guardPosition.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE);
    return true;
}

bool BattleBotReturnToGuardPositionBeforeRecovery(BattleBotAI* pAI)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    if (pAI->me->IsInCombat() || pAI->me->GetVictim() || pAI->me->IsMounted())
        return false;

    if (!BattleBotNeedsRecovery(pAI))
        return false;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_AB:
        {
            Position guardPosition;
            if (FindABOwnedGuardPosition(pAI, guardPosition) &&
                pAI->me->GetDistance2d(guardPosition) <= 80.0f)
                return MoveGuardBackBeforeRecovery(pAI, guardPosition, AB_GUARD_SEARCH_RADIUS, &vPaths_AB, true);

            return false;
        }
        case BATTLEGROUND_AV:
            return false;
        default:
            return false;
    }
}

bool BattleBotTryCaptureNearbyABObjective(BattleBotAI* pAI)
{
    if (AtFlag(pAI, vFlagsAB))
        return true;
    if (MoveToNearbyABOpenFlag(pAI))
        return true;
    if (pAI->me->HasAura(BB_SPELL_FOOD) || pAI->me->HasAura(BB_SPELL_DRINK))
        return false;
    bool needToEat = pAI->me->GetHealthPercent() < 90.0f;
    bool needToDrink = (pAI->me->GetPowerType() == POWER_MANA) && (pAI->me->GetPowerPercent(POWER_MANA) < 90.0f);
    if (needToDrink && pAI->me->GetClass() == CLASS_DRUID &&
        pAI->me->GetShapeshiftForm() != FORM_NONE &&
       (pAI->GetRole() == ROLE_MELEE_DPS || pAI->GetRole() == ROLE_TANK))
        needToDrink = false;
    if (needToEat || needToDrink)
        return false;
    return ReleaseABExcessGuard(pAI);
}

bool BattleBotSelectABObjective(BattleBotAI* pAI)
{
    Position targetPosition;
    if (FindABOwnedGuardPosition(pAI, targetPosition))
    {
        if (pAI->StartNewPathToPosition(targetPosition, vPaths_AB))
            return true;
        return pAI->me->GetDistance(targetPosition) <= AB_GUARD_EXCESS_RADIUS;
    }
    if (FindABAssaultPosition(pAI, targetPosition))
    {
        if (pAI->StartNewPathToPosition(targetPosition, vPaths_AB))
            return true;
        return pAI->me->GetDistance(targetPosition) <= AB_GUARD_EXCESS_RADIUS;
    }
    return false;
}

bool BattleBotSelectWSGObjective(BattleBotAI* pAI)
{
    BattleGroundWS* bgWS = static_cast<BattleGroundWS*>(pAI->me->GetBattleGround());
    if (StartWSGHomeGuardObjective(pAI, bgWS))
        return true;
    if (pAI->me->GetTeam() == HORDE)
    {
        if (pAI->me->HasAura(AURA_SILVERWING_FLAG))
            return pAI->StartNewPathToPosition(WS_FLAG_POS_HORDE, vPaths_WS);
        if (!bgWS->IsAllianceFlagPickedup())
        {
            float const distance = pAI->me->GetDistance(WS_FLAG_POS_ALLIANCE);
            if (distance > 20.0f)
                return pAI->StartNewPathToPosition(WS_FLAG_POS_ALLIANCE, vPaths_WS);
        }
    }
    else
    {
        if (pAI->me->HasAura(AURA_WARSONG_FLAG))
            return pAI->StartNewPathToPosition(WS_FLAG_POS_ALLIANCE, vPaths_WS);
        if (!bgWS->IsHordeFlagPickedup())
        {
            float const distance = pAI->me->GetDistance(WS_FLAG_POS_HORDE);
            if (distance > 20.0f)
                return pAI->StartNewPathToPosition(WS_FLAG_POS_HORDE, vPaths_WS);
        }
    }
    return false;
}
