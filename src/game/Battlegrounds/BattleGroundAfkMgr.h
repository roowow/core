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

#ifndef MANGOS_BATTLEGROUNDAFKMGR_H
#define MANGOS_BATTLEGROUNDAFKMGR_H

#include "Common.h"
#include "ObjectGuid.h"

#include <map>

class BattleGround;

struct BattleGroundAfkScoreRule
{
    uint32 warning1Score;
    uint32 warning2Score;
    uint32 warning3Score;
    uint32 kickScore;

    int32 noContributionScore;
    int32 noMovementScore;
    int32 noCombatScore;
    int32 movementReduce;
    int32 combatReduce;
};

struct BattleGroundAfkPlayerState
{
    uint32 score = 0;
    uint8 stage = 0;
    uint32 graceChecks = 2;
    uint32 lastWarnTime = 0;
    bool initialized = false;
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastZ = 0.0f;
};

class BattleGroundAfkMgr
{
public:
    void Update(BattleGround* bg, uint32 diff);
    void RemovePlayer(ObjectGuid guid);
    void Reset();

private:
    BattleGroundAfkScoreRule GetRule(BattleGround const* bg) const;
    void UpdatePlayer(BattleGround* bg, ObjectGuid guid);
    void ApplyStage(BattleGround* bg, ObjectGuid guid, BattleGroundAfkPlayerState& state, BattleGroundAfkScoreRule const& rule);
    void SendWarning(ObjectGuid guid, uint8 stage, uint32 score) const;

    std::map<ObjectGuid, BattleGroundAfkPlayerState> m_playerStates;
    uint32 m_updateTimer = 0;
    uint32 m_elapsedTime = 0;
};

#endif
