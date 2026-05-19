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

#ifndef MANGOS_BattleBotAI_H
#define MANGOS_BattleBotAI_H

#include "CombatBotBaseAI.h"
#include "BattleBotWaypoints.h"

#include <ctime>

enum BattleBotWsgWaitSpot
{
    BB_WSG_WAIT_SPOT_SPAWN,
    BB_WSG_WAIT_SPOT_LEFT,
    BB_WSG_WAIT_SPOT_RIGHT
};

enum FlagSpellsWS
{
    AURA_WARSONG_FLAG    = 23333,
    AURA_SILVERWING_FLAG = 23335
};

class BattleBotAI : public CombatBotBaseAI
{
public:
    BattleBotAI(uint8 race, uint8 class_, uint8 level, uint32 mapId, uint32 instanceId, float x, float y, float z, float o, uint8 bgId, bool temporary)
        : CombatBotBaseAI(),  m_race(race), m_class(class_), m_level(level), m_mapId(mapId), m_instanceId(instanceId), m_x(x), m_y(y), m_z(z), m_o(o), m_battlegroundId(bgId), m_temporary(temporary)
    {
        m_updateTimer.Reset(2000);
    }
    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) override
    {
        return SpawnNewPlayer(sess, m_class, m_race, m_mapId, m_instanceId, m_x, m_y, m_z, m_o);
    }
    uint32 GetBotNameThemeKey(uint32 /*instanceId*/) const override { return m_battlegroundId; }
  
    void OnPlayerLogin() final;
    void UpdateAI(uint32 const diff) final;
    void OnPacketReceived(WorldPacket const* packet) final;
    void MovementInform(uint32 MovementType, uint32 Data = 0) final;

    bool ShouldIgnoreCombat() const;
    bool DrinkAndEat();
    bool UseMount();
    uint32 GetMountSpellId() const;
    bool CheckForUnreachableTarget();
    bool IsBadPlayer(Unit const* pTarget) const;
    float GetMaxAggroDistanceForMap() const;
    bool ShouldUseAVOpeningPassiveCombat() const;
    Unit* SelectAVOpeningObjectiveNpcTarget(Unit* pExcept = nullptr) const;
    Unit* SelectAVOpeningRetaliationTarget(Unit* pExcept = nullptr) const;
    uint32 GetAVPhase1WaveDelay() const;
    bool ShouldWaitForAVPhase1Wave();
    bool UpdateAVPhase1WaitingAI();
    bool AttackStart(Unit* pVictim);
    Unit* SelectAttackTarget(Unit* pExcept = nullptr) const;
    Unit* SelectHealerOffensiveTarget() const;
    Unit* SelectFollowTarget() const;

    void OnJustRevived();
    void OnJustDied();
    void OnEnterBattleGround();
    void OnLeaveBattleGround();

    void UpdateFlagCarrierAI();
    bool UpdateBattleGroundAI();
    bool TryUseBattleGroundFlag(uint32 entry);
    void UpdateInCombatAI() final;
    void UpdateOutOfCombatAI() final;
    void UpdateInCombatAI_Paladin() final;
    void UpdateOutOfCombatAI_Paladin() final;
    void UpdateInCombatAI_Shaman() final;
    void UpdateOutOfCombatAI_Shaman() final;
    void UpdateInCombatAI_Hunter() final;
    void UpdateOutOfCombatAI_Hunter() final;
    void UpdateInCombatAI_Mage() final;
    void UpdateOutOfCombatAI_Mage() final;
    void UpdateInCombatAI_Priest() final;
    void UpdateOutOfCombatAI_Priest() final;
    void UpdateInCombatAI_Warlock() final;
    void UpdateOutOfCombatAI_Warlock() final;
    void UpdateInCombatAI_Warrior() final;
    void UpdateOutOfCombatAI_Warrior() final;
    void UpdateInCombatAI_Rogue() final;
    void UpdateOutOfCombatAI_Rogue() final;
    void UpdateInCombatAI_Druid() final;
    void UpdateOutOfCombatAI_Druid() final;

    uint8 m_battlegroundId = 0;
    ShortTimeTracker m_updateTimer;
    uint8 m_race = 0;
    uint8 m_class = 0;
    uint8 m_level = 0;
    uint32 m_mapId = 0;
    uint32 m_instanceId = 0;
    float m_x = 0.0f;
    float m_y = 0.0f;
    float m_z = 0.0f;
    float m_o = 0.0f;
    bool m_temporary = false;
    bool m_wasDead = false;
    bool m_wasInBG = false;

    // Movement System
    void UpdateWaypointMovement();
    void DoGraveyardJump();
    void MoveToNextPoint();
    bool StartNewPathFromBeginning();
    void StartNewPathFromAnywhere();
    bool StartNewPathToObjective();
    bool StartNewPathToPosition(Position const& position, std::vector<BattleBotPath*> const& vPaths);
    void ClearPath();
    void StopMoving();
    bool m_doingGraveyardJump = false;
    bool m_movingInReverse = false;
    uint32 m_currentPoint = 0;
    BattleBotPath* m_currentPath = nullptr;
    uint8 m_waitingSpot = BB_WSG_WAIT_SPOT_SPAWN;
    bool m_avStartWaveInitialized = false;
    uint32 m_avStartWaveBgInstance = 0;
    time_t m_avStartWaveReleaseTime = 0;

    // BG melee stuck detection: bots unable to reach target inside buildings
    uint8 m_bgStuckCounter = 0;
    float m_bgStuckLastX = 0.0f;
    float m_bgStuckLastY = 0.0f;

    // AV tower stuck detection: timeout + skip for non-key tower objectives
    uint32 m_avCurrentObjective = 0;
    time_t m_avObjectiveTime = 0;
    uint32 m_avSkipObjective = 0;
    time_t m_avSkipObjectiveExpiry = 0;
    uint32 m_avAssignedGY = 0;

    // AV mine bot identity (set once after 5-minute delay, stable for the BG session)
    bool m_avIsMineBot = false;
    bool m_avMineBotDecided = false;
    uint32 m_avMineBotBgInstance = 0;

    // AV randomness state (per-bot decisions rolled on login/respawn)
    bool m_avAggressiveMode = false;       // true = attack enemies while traveling (30% chance)
    bool m_avStayGuardAfterCapture = false; // true = remain as guard after capture completes

    // Movement progress checkpoint: detects outdoor pathfinding loops
    uint8 m_bgProgressTicks = 0;
    float m_bgProgressX = 0.0f;
    float m_bgProgressY = 0.0f;
};

#endif
