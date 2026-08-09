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
#include "BattleBotWaypoints2.h"
#include "Geometry.h"
#include <cstddef>
#include "Utilities/Random.h"
#include "Log.h"
#include "World.h"

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

#define SPELL_CAPTURE_BANNER 21651

std::vector<uint32> const vFlagsAV = { GO_AV_HORDE_BANNER1 , GO_AV_HORDE_BANNER2 , GO_AV_ALLIANCE_BANNER1 , GO_AV_ALLIANCE_BANNER2 ,
                                       GO_AV_CONTESTED_BANNER1 , GO_AV_CONTESTED_BANNER2 , GO_AV_CONTESTED_BANNER3 ,
                                       GO_AV_CONTESTED_BANNER4 , GO_AV_SNOWFALL_BANNER };

std::vector<uint32> const vFlagsAB = { GO_AB_ALLIANCE_BANNER , GO_AB_CONTESTED_BANNER1 , GO_AB_HORDE_BANNER , GO_AB_CONTESTED_BANNER2 ,
                                       GO_AB_STABLE_BANNER, GO_AB_BLACKSMITH_BANNER, GO_AB_FARM_BANNER, GO_AB_LUMBER_MILL_BANNER,
                                       GO_AB_GOLD_MINE_BANNER };

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

static bool TryRogueBlindBeforeCapture(BattleBotAI* pAI, GameObject* pGo)
{
    if (!pAI || !pGo || pAI->me->GetClass() != CLASS_ROGUE || !pAI->m_spells.rogue.pBlind)
        return false;

    SpellEntry const* blind = pAI->m_spells.rogue.pBlind;
    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 15.0f);

    Player* bestTarget = nullptr;
    float bestDistanceToFlag = FLT_MAX;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            (blind->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS) && pAI->AreOthersOnSameTarget(player->GetObjectGuid())) ||
            !pAI->CanTryToCastSpell(player, blind))
            continue;

        float const distanceToFlag = player->GetDistance(pGo);
        if (!bestTarget || distanceToFlag < bestDistanceToFlag)
        {
            bestTarget = player;
            bestDistanceToFlag = distanceToFlag;
        }
    }

    if (!bestTarget)
        return false;

    if (pAI->me->IsMounted())
        pAI->me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    if (pAI->me->IsInDisallowedMountForm())
        pAI->me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

    if (pAI->DoCastSpell(bestTarget, blind) == SPELL_CAST_OK)
    {
        pAI->me->AttackStop();
        pAI->ClearPath();
        pAI->StopMoving();
        return true;
    }

    return false;
}

bool AtFlag(BattleBotAI* pAI, std::vector<uint32> const& vFlagIds)
{
    if (Spell* currentSpell = pAI->me->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        if (currentSpell->m_spellInfo->Id == SPELL_CAPTURE_BANNER)
            return true;
    }

    if (Player* pFriend = pAI->me->FindNearestFriendlyPlayer(INTERACTION_DISTANCE))
    {
        if (pFriend != pAI->me &&
            pFriend->GetCurrentSpell(CURRENT_GENERIC_SPELL) &&
            pFriend->GetCurrentSpell(CURRENT_GENERIC_SPELL)->m_spellInfo->Id == SPELL_CAPTURE_BANNER)
        {
            pAI->ClearPath();
            if (BattleGround* bg = pAI->me->GetBattleGround())
            {
                if (bg->GetTypeID() == BATTLEGROUND_AB)
                    pAI->StopMoving();
                else
                    pAI->StartNewPathFromBeginning();
            }
            return true;
        }
    }

    for (const auto& bannerId : vFlagIds)
    {
        if (GameObject* pGo = pAI->me->FindNearestGameObject(bannerId, INTERACTION_DISTANCE))
        {
            if (IsABFlagOpenable(pAI, pGo))
            {
                if (TryRogueBlindBeforeCapture(pAI, pGo))
                    return true;

                if (pAI->me->IsMounted())
                    pAI->me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

                if (pAI->me->IsInDisallowedMountForm())
                    pAI->me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

                pAI->ClearPath();
                if (sWorld.getConfig(CONFIG_BOOL_BATTLEGROUND_MOVEMENT_DEBUG))
                {
                    BattleGround* dbgBg = pAI->me->GetBattleGround();
                    sLog.Out(LOG_BG, LOG_LVL_BASIC,
                             "[BattleGroundObjective] node-capture-attempt bot %s guid %u bg %u entry %u pos %.1f %.1f %.1f.",
                             pAI->me->GetName(), pAI->me->GetGUIDLow(),
                             dbgBg ? dbgBg->GetInstanceID() : 0u, bannerId,
                             pAI->me->GetPositionX(), pAI->me->GetPositionY(), pAI->me->GetPositionZ());
                }
                pAI->me->CastSpell(pGo, SPELL_CAPTURE_BANNER, false);
                return true;
            }
        }
    }

    return false;
}

bool BattleBotTryCaptureNearbyObjective(BattleBotAI* pAI)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_AB:
            return BattleBotTryCaptureNearbyABObjective(pAI);
        default:
            return false;
    }
}

void AV_AtFlag(BattleBotAI* pAI)
{
    if (AtFlag(pAI, vFlagsAV))
        return;

    pAI->MoveToNextPoint();
}

void AtCaveExit(BattleBotAI* pAI)
{
    pAI->me->StopMoving();

    if (pAI->UseMount())
    {
        pAI->ClearPath();
        return;
    }

    pAI->MoveToNextPoint();
}

extern BattleBotPath vPath_AV_Stormpike_to_Irondeep_Morloch;
extern BattleBotPath vPath_AV_TowerPoint_to_Coldtooth_Snivvle;

void MoveToNextPointSpecial(BattleBotAI* pAI)
{
    if (!pAI->m_currentPath || pAI->m_currentPath->empty())
    {
        pAI->ClearPath();
        return;
    }

    uint32 const lastPointInPath = pAI->m_movingInReverse ? 0 : ((*pAI->m_currentPath).size() - 1);

    if ((pAI->m_currentPoint == lastPointInPath) ||
        (pAI->me->IsInCombat() && !pAI->ShouldIgnoreCombat()) || !pAI->me->IsAlive())
    {
        // Path is over.
        pAI->ClearPath();
        return;
    }

    if (pAI->m_movingInReverse)
        pAI->m_currentPoint--;
    else
        pAI->m_currentPoint++;

    // Defensive: m_currentPoint can be stale (e.g. set to closestPoint-1 with
    // closestPoint==0 underflows to UINT32_MAX before wraparound on ++). Drop the
    // path instead of throwing out_of_range from at() and aborting the world thread.
    if (pAI->m_currentPoint >= pAI->m_currentPath->size())
    {
        pAI->ClearPath();
        return;
    }

    BattleBotWaypoint& nextPoint = (*pAI->m_currentPath)[pAI->m_currentPoint];

    uint32 moveFlags = MOVE_RUN_MODE;
    if (pAI->m_currentPath == &vPath_AV_Stormpike_to_Irondeep_Morloch ||
        pAI->m_currentPath == &vPath_AV_TowerPoint_to_Coldtooth_Snivvle)
        moveFlags = MOVE_PATHFINDING | MOVE_RUN_MODE;

    pAI->me->GetMotionMaster()->MovePoint(pAI->m_currentPoint, nextPoint.x + frand(-1, 1), nextPoint.y + frand(-1, 1), nextPoint.z, moveFlags);
}

BattleBotPath vPath_AV_Horde_Cave_to_Tower_Point_Crossroad =
{
    { -885.928f, -536.612f, 55.1936f, &AtCaveExit },
    { -880.957f, -525.119f, 53.6791f, nullptr },
    { -839.408f, -499.746f, 49.7505f, nullptr },
    { -820.21f, -469.193f, 49.4085f,  nullptr },
    { -812.602f, -460.45f, 54.0872f,  nullptr },
    { -789.646f, -441.864f, 57.8833f, nullptr },
    { -776.405f, -432.056f, 61.9256f, nullptr },
    { -760.773f, -430.154f, 64.8376f, nullptr },
    { -734.801f, -419.622f, 67.5354f, nullptr },
    { -718.313f, -404.674f, 67.5994f, nullptr },
    { -711.436f, -362.86f, 66.7543f,  nullptr },
};

BattleBotPath vPath_AV_Horde_Cave_to_Frostwolf_Graveyard_Flag =
{
    { -885.928f, -536.612f, 55.1936f, &AtCaveExit },
    { -892.195f, -527.288f, 54.3716f, nullptr },
    { -901.886f, -506.521f, 54.2415f, nullptr },
    { -908.843f, -494.624f, 46.3098f, nullptr },
    { -919.567f, -480.127f, 43.6671f, nullptr },
    { -939.133f, -456.123f, 43.1962f, nullptr },
    { -965.027f, -425.647f, 47.8552f, nullptr },
    { -980.983f, -417.492f, 49.7504f, nullptr },
    { -1011.67f, -415.94f, 51.0241f, nullptr },
    { -1035.81f, -410.183f, 51.3285f, nullptr },
    { -1049.87f, -400.511f, 51.4724f, nullptr },
    { -1054.03f, -379.162f, 51.4679f, nullptr },
    { -1081.32f, -348.198f, 54.5837f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Frostwolf_Graveyard_Flag_to_Horde_Base_First_Crossroads =
{
    { -1081.32f, -348.198f, 54.5837f, &AV_AtFlag },
    { -1085.54f, -349.316f, 54.5448f, nullptr },
    { -1096.08f, -339.028f, 54.5433f, nullptr },
    { -1112.8f, -338.773f, 53.4886f, nullptr },
    { -1137.95f, -349.136f, 51.1539f, nullptr },
    { -1160.92f, -352.495f, 51.7134f, nullptr },
    { -1181.12f, -360.f, 52.4494f, nullptr },
    { -1191.79f, -365.559f, 52.4952f, nullptr },
    { -1210.16f, -366.523f, 55.6316f, nullptr },
    { -1238.08f, -364.752f, 59.3705f, nullptr },
    { -1242.44f, -361.619f, 59.7433f, nullptr },
    { -1246.25f, -350.16f, 59.4911f, nullptr },
    { -1245.71f, -338.859f, 59.2882f, nullptr },
    { -1238.46f, -325.945f, 60.084f, nullptr },
    { -1227.58f, -312.249f, 63.2087f, nullptr },
    { -1216.36f, -296.148f, 70.0471f, nullptr },
    { -1210.f, -276.949f, 73.7288f, nullptr },
    { -1207.09f, -259.721f, 72.6882f, nullptr },
    { -1208.37f, -254.367f, 72.5688f, nullptr },
    { -1211.83f, -252.648f, 72.7613f, nullptr },
    { -1236.28f, -251.079f, 73.3273f, nullptr },
    { -1243.33f, -253.936f, 73.3273f, nullptr },
    { -1249.37f, -260.819f, 73.3704f, nullptr },
    { -1257.24f, -272.391f, 72.9263f, nullptr },
    { -1266.31f, -281.263f, 76.3849f, nullptr },
    { -1272.81f, -285.884f, 82.5902f, nullptr },
    { -1278.52f, -288.089f, 86.4665f, nullptr },
    { -1288.71f, -289.227f, 89.7414f, nullptr },
    { -1306.2f, -289.35f, 90.7095f, nullptr },
    { -1320.18f, -289.806f, 90.4719f, nullptr },
};

BattleBotPath vPath_AV_Horde_Base_First_Crossroads_to_East_Frostwolf_Tower_Flag =
{
    { -1320.18f, -289.806f, 90.4719f, nullptr },
    { -1315.93f, -293.794f, 90.4653f, nullptr },
    { -1312.71f, -300.242f, 90.9343f, nullptr },
    { -1309.39f, -305.389f, 91.7743f, nullptr },
    { -1306.77f, -310.068f, 91.7371f, nullptr },
    { -1300.2f, -322.303f, 91.3631f, nullptr },
    { -1295.93f, -320.154f, 91.3596f, nullptr },
    { -1301.5f, -309.995f, 95.7382f, nullptr },
    { -1309.02f, -315.629f, 99.4271f, nullptr },
    { -1304.13f, -321.638f, 102.323f, nullptr },
    { -1298.43f, -317.239f, 104.769f, nullptr },
    { -1304.62f, -310.076f, 107.328f, nullptr },
    { -1306.73f, -316.046f, 107.328f, nullptr },
    { -1314.61f, -321.14f, 107.316f, nullptr },
    { -1311.92f, -324.548f, 109.202f, nullptr },
    { -1304.72f, -328.225f, 113.563f, nullptr },
    { -1301.77f, -326.848f, 113.84f, nullptr },
    { -1294.77f, -321.092f, 113.792f, nullptr },
    { -1294.33f, -312.247f, 113.794f, nullptr },
    { -1302.25f, -307.602f, 113.856f, nullptr },
    { -1305.22f, -311.212f, 113.864f, nullptr },
    { -1303.38f, -315.877f, 113.867f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Horde_Base_First_Crossroads_to_West_Frostwolf_Tower_Flag =
{
    { -1320.18f, -289.806f, 90.4719f, nullptr },
    { -1318.43f, -283.088f, 90.5913f, nullptr },
    { -1315.58f, -275.043f, 91.1062f, nullptr },
    { -1311.21f, -271.424f, 91.8402f, nullptr },
    { -1306.39f, -269.65f, 92.0506f, nullptr },
    { -1292.18f, -264.937f, 91.6452f, nullptr },
    { -1292.76f, -261.18f, 91.6437f, nullptr },
    { -1296.92f, -261.369f, 92.6981f, nullptr },
    { -1303.73f, -263.629f, 95.987f, nullptr },
    { -1300.5f, -272.465f, 99.691f, nullptr },
    { -1293.33f, -269.5f, 102.51f, nullptr },
    { -1295.96f, -263.022f, 105.042f, nullptr },
    { -1304.46f, -266.371f, 107.612f, nullptr },
    { -1299.74f, -270.916f, 107.612f, nullptr },
    { -1296.95f, -279.413f, 107.585f, nullptr },
    { -1291.49f, -276.799f, 110.059f, nullptr },
    { -1286.89f, -269.783f, 114.142f, nullptr },
    { -1290.87f, -260.594f, 114.151f, nullptr },
    { -1299.88f, -257.96f, 114.111f, nullptr },
    { -1306.39f, -263.798f, 114.147f, nullptr },
    { -1305.91f, -269.074f, 114.066f, nullptr },
    { -1298.82f, -267.215f, 114.151f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Horde_Base_First_Crossroads_to_Horde_Base_Second_Crossroads =
{
    { -1320.18f, -289.806f, 90.4719f, nullptr },
    { -1334.14f, -290.898f, 90.8243f, nullptr },
    { -1344.61f, -291.546f, 90.8375f, nullptr },
};

BattleBotPath vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_Entrance_DrekThar =
{
    { -1344.61f, -291.546f, 90.8375f, nullptr },
    { -1348.33f, -287.291f, 91.1178f, nullptr },
    { -1350.16f, -282.141f, 92.7105f, nullptr },
    { -1355.48f, -265.233f, 98.9033f, nullptr },
    { -1360.94f, -247.529f, 99.3667f, nullptr },
};

BattleBotPath vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_DrekThar1 =
{
    { -1360.94f, -247.529f, 99.3667f, nullptr },
    { -1354.74f, -242.226f, 99.3695f, nullptr },
    { -1348.1f, -235.612f, 99.3657f, nullptr },
    { -1349.98f, -229.222f, 99.3699f, nullptr },
    { -1366.23f, -223.049f, 98.4174f, nullptr },
};

BattleBotPath vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_DrekThar2 =
{
    { -1360.94f, -247.529f, 99.3667f, nullptr },
    { -1368.08f, -247.832f, 99.3703f, nullptr },
    { -1377.83f, -245.929f, 99.3629f, nullptr },
    { -1381.21f, -239.565f, 99.3698f, nullptr },
    { -1372.52f, -225.593f, 98.4269f, nullptr },
};

BattleBotPath vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_Graveyard_Flag =
{
    { -1344.61f, -291.546f, 90.8375f, nullptr },
    { -1353.15f, -296.183f, 90.6364f, nullptr },
    { -1360.12f, -302.321f, 91.5223f, nullptr },
    { -1372.69f, -308.451f, 91.2869f, nullptr },
    { -1392.43f, -318.109f, 88.7925f, nullptr },
    { -1400.8f, -314.327f, 89.0501f, nullptr },
    { -1401.94f, -310.103f, 89.3816f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Tower_Point_Crossroads_to_Tower_Point_Bottom =
{
    { -711.436f, -362.86f, 66.7543f,  nullptr },
    { -713.433f, -357.847f, 66.6605f, nullptr },
    { -726.362f, -345.477f, 66.8089f, nullptr },
    { -748.788f, -344.154f, 66.7348f, nullptr },
    { -759.771f, -342.304f, 67.2223f, nullptr },
};

BattleBotPath vPath_AV_TowerPoint_Bottom_to_Tower_Point_Flag =
{
    { -759.771f, -342.304f, 67.2223f, &MoveToNextPointSpecial },
    { -762.294f, -343.172f, 67.3607f, &MoveToNextPointSpecial },
    { -764.151f, -350.571f, 68.7991f, nullptr },
    { -766.112f, -357.945f, 68.6996f, nullptr },
    { -770.997f, -370.089f, 68.3956f, nullptr },
    { -763.765f, -368.338f, 69.1196f, nullptr },
    { -761.735f, -359.76f, 72.7363f, nullptr },
    { -771.43f, -357.941f, 76.4841f, nullptr },
    { -773.925f, -365.214f, 79.2135f, nullptr },
    { -766.473f, -365.891f, 81.9322f, nullptr },
    { -765.147f, -355.981f, 84.3558f, nullptr },
    { -771.041f, -360.772f, 84.3558f, nullptr },
    { -779.831f, -356.316f, 84.3425f, nullptr },
    { -780.107f, -362.818f, 87.4599f, nullptr },
    { -775.392f, -371.248f, 90.8508f, nullptr },
    { -767.966f, -372.722f, 90.8949f, nullptr },
    { -759.167f, -366.147f, 90.8259f, nullptr },
    { -760.11f, -357.787f, 90.8949f, nullptr },
    { -764.399f, -355.108f, 90.8013f, nullptr },
    { -767.9f, -362.019f, 90.8949f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Tower_Point_Bottom_to_Frostwolf_Graveyard_Flag =
{
    { -759.771f, -342.304f, 67.2223f, nullptr },
    { -764.971f, -339.278f, 67.6875f, nullptr },
    { -773.394f, -335.633f, 66.4157f, nullptr },
    { -796.758f, -340.437f, 61.5754f, nullptr },
    { -828.745f, -348.592f, 50.1022f, nullptr },
    { -846.826f, -355.181f, 50.0754f, nullptr },
    { -869.897f, -359.01f, 50.9404f, nullptr },
    { -888.679f, -365.688f, 49.3732f, nullptr },
    { -908.082f, -381.24f, 48.9888f, nullptr },
    { -934.234f, -388.41f, 48.9912f, nullptr },
    { -960.683f, -395.321f, 49.028f, nullptr },
    { -970.161f, -397.02f, 49.2312f, nullptr },
    { -993.784f, -397.619f, 50.0896f, nullptr },
    { -1018.82f, -393.742f, 50.703f, nullptr },
    { -1047.38f, -380.337f, 51.1403f, nullptr },
    { -1066.7f, -361.097f, 51.3909f, nullptr },
    { -1079.61f, -345.548f, 55.1131f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Frostwolf_Graveyard_to_Frostwolf_Graveyard_Flag =
{
    { -1089.6f, -268.375f, 57.038f, nullptr },
    { -1087.23f, -285.712f, 56.625f, nullptr },
    { -1084.83f, -307.023f, 56.5773f, nullptr },
    { -1082.81f, -327.926f, 54.863f, nullptr },
    { -1082.08f, -333.784f, 54.885f, nullptr },
    { -1079.61f, -345.548f, 55.1131f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Tower_Point_Crossroads_to_Iceblood_Graveyard_Flag =
{
    { -711.436f, -362.86f, 66.7543f,  nullptr },
    { -703.169f, -363.672f, 66.3514f, nullptr },
    { -682.166f, -374.453f, 65.6513f, nullptr },
    { -653.798f, -387.044f, 62.0839f, nullptr },
    { -625.062f, -397.093f, 59.0311f, nullptr },
    { -614.138f, -396.501f, 60.8585f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Iceblood_Graveyard_Flag_to_Iceblood_Tower_Crossroad =
{
    { -614.138f, -396.501f, 60.8585f, nullptr },
    { -620.856f, -389.074f, 58.4251f, nullptr },
    { -622.361f, -366.641f, 56.862f, nullptr },
    { -618.134f, -346.849f, 55.2242f, nullptr },
    { -586.744f, -317.327f, 48.149f, nullptr },
    { -579.697f, -315.037f, 46.345f, nullptr },
};

BattleBotPath vPath_AV_Iceblood_Tower_Crossroad_to_Iceblood_Tower =
{
    { -579.697f, -315.037f, 46.345f, nullptr },
    { -579.747f, -308.986f, 46.6127f, nullptr },
    { -575.92f, -296.013f, 48.4349f, nullptr },
    { -567.029f, -286.975f, 50.7703f, nullptr },
    { -557.697f, -276.264f, 52.1503f, nullptr },
};

BattleBotPath vPath_AV_Iceblood_Tower_to_Iceblood_Tower_Flag =
{
    { -557.697f, -276.264f, 52.1503f, nullptr },
    { -562.505f, -271.251f, 52.9165f, nullptr },
    { -575.986f, -258.447f, 52.5129f, nullptr },
    { -580.103f, -261.305f, 52.5013f, nullptr },
    { -571.844f, -269.038f, 56.8539f, nullptr },
    { -565.86f, -261.478f, 60.5514f, nullptr },
    { -572.102f, -256.66f, 63.3275f, nullptr },
    { -576.45f, -262.642f, 65.8959f, nullptr },
    { -568.831f, -268.036f, 68.4696f, nullptr },
    { -568.091f, -260.214f, 68.4696f, nullptr },
    { -561.978f, -254.631f, 68.4482f, nullptr },
    { -570.682f, -250.791f, 73.0299f, nullptr },
    { -576.069f, -252.266f, 74.9855f, nullptr },
    { -581.294f, -260.533f, 74.9366f, nullptr },
    { -579.309f, -268.79f, 74.9984f, nullptr },
    { -570.275f, -271.346f, 75.0083f, nullptr },
    { -566.436f, -268.102f, 74.9324f, nullptr },
    { -571.044f, -263.753f, 75.0087f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Iceblood_Tower_to_Iceblood_Garrison =
{
    { -557.697f, -276.264f, 52.1503f, nullptr },
    { -550.396f, -269.326f, 52.4539f, nullptr },
    { -542.628f, -254.901f, 54.7447f, nullptr },
    { -536.969f, -242.614f, 57.233f, nullptr },
    { -532.204f, -230.96f, 56.7126f, nullptr },
    { -499.532f, -204.606f, 57.4507f, nullptr },
    { -492.17f, -187.077f, 57.1342f, nullptr },
};

BattleBotPath vPath_AV_Iceblood_Garrison_to_Captain_Galvangar =
{
    { -492.17f, -187.077f, 57.1342f, nullptr },
    { -496.52f, -186.015f, 57.4265f, nullptr },
    { -508.094f, -180.795f, 57.9437f, nullptr },
    { -519.813f, -179.116f, 57.9527f, nullptr },
    { -528.962f, -186.573f, 57.9513f, nullptr },
    { -538.064f, -186.984f, 57.9556f, nullptr },
    { -540.747f, -169.935f, 57.0124f, nullptr },
};

BattleBotPath vPath_AV_Iceblood_Garrison_to_Snowfall_Flag =
{
    { -492.17f, -187.077f, 57.1342f, nullptr },
    { -478.362f, -193.882f, 55.0986f, nullptr },
    { -463.702f, -196.051f, 48.5213f, nullptr },
    { -456.512f, -196.217f, 43.158f, nullptr },
    { -429.951f, -198.515f, 26.8893f, nullptr },
    { -412.975f, -190.921f, 25.7659f, nullptr },
    { -387.531f, -186.251f, 15.2001f, nullptr },
    { -370.088f, -186.351f, 12.8474f, nullptr },
    { -353.23f, -179.161f, 10.3823f, nullptr },
    { -319.906f, -162.842f, 9.26139f, nullptr },
    { -316.616f, -151.198f, 10.5439f, nullptr },
    { -307.702f, -133.042f, 14.3502f, nullptr },
    { -293.423f, -117.684f, 19.8246f, nullptr },
    { -274.446f, -97.1283f, 40.3783f, nullptr },
    { -256.993f, -83.0377f, 54.9012f, nullptr },
    { -249.129f, -80.3578f, 58.6962f, nullptr },
    { -237.627f, -84.7628f, 63.982f, nullptr },
    { -226.054f, -92.6308f, 75.1362f, nullptr },
    { -205.464f, -114.337f, 78.6167f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Iceblood_Graveyard_to_Iceblood_Graveyard_Flag =
{
    { -536.289f, -397.294f, 49.7357f, nullptr },
    { -549.254f, -386.331f, 50.1129f, nullptr },
    { -558.918f, -388.345f, 50.7527f, nullptr },
    { -571.161f, -397.302f, 52.6324f, nullptr },
    { -590.297f, -409.179f, 56.239f, nullptr },
    { -602.969f, -411.808f, 59.1209f, nullptr },
    { -613.748f, -407.195f, 59.6702f, nullptr },
    { -614.138f, -396.501f, 60.8585f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Iceblood_Tower_Crossroad_to_Field_of_Strife_Stoneheart_Snowfall_Crossroad =
{
    { -579.697f, -315.037f, 46.345f, nullptr },
    { -572.416f, -317.133f, 43.9943f, nullptr },
    { -559.201f, -326.073f, 39.5171f, nullptr },
    { -544.32f, -335.203f, 37.6494f, nullptr },
    { -520.032f, -342.158f, 34.3473f, nullptr },
    { -486.033f, -344.504f, 29.4772f, nullptr },
    { -462.603f, -340.414f, 31.3555f, nullptr },
    { -455.422f, -336.249f, 30.5176f, nullptr },
    { -450.551f, -332.547f, 25.4498f, nullptr },
    { -440.977f, -326.828f, 18.9782f, nullptr },
    { -421.08f, -321.656f, 17.7909f, nullptr },
    { -398.601f, -318.181f, 18.5789f, nullptr },
    { -387.651f, -312.289f, 21.504f, nullptr },
    { -360.257f, -299.908f, 9.75685f, nullptr },
    { -294.669f, -283.616f, 6.66756f, nullptr },
    { -244.919f, -272.52f, 6.66754f, nullptr },
    { -206.645f, -264.661f, 6.66755f, nullptr },
    { -171.291f, -266.565f, 9.06823f, nullptr },
    { -147.596f, -253.494f, 6.78363f, nullptr },
};

BattleBotPath vPath_AV_Snowfall_Flag_to_Field_of_Strife_Stoneheart_Snowfall_Crossroad =
{
    { -205.464f, -114.337f, 78.6167f, &AV_AtFlag },
    { -196.784f, -127.119f, 78.1071f, nullptr },
    { -193.525f, -137.091f, 75.7275f, nullptr },
    { -180.605f, -153.454f, 63.4406f, nullptr },
    { -175.934f, -158.667f, 58.9224f, nullptr },
    { -169.015f, -173.639f, 46.3033f, nullptr },
    { -164.039f, -186.72f, 36.9312f, nullptr },
    { -159.523f, -203.286f, 27.413f, nullptr },
    { -154.95f, -220.108f, 16.6193f, nullptr },
    { -147.742f, -233.671f, 9.55408f, nullptr },
    { -134.536f, -243.891f, 7.96268f, nullptr },
    { -147.596f, -253.494f, 6.78363f, nullptr },
};

BattleBotPath vPath_AV_Field_of_Strife_Stoneheart_Snowfall_Crossroad_to_Stonehearth_Outpost =
{
    { -147.596f, -253.494f, 6.78363f, nullptr },
    { -119.031f, -245.732f, 9.12747f, nullptr },
    { -69.9914f, -241.707f, 8.7055f, nullptr },
    { -41.7233f, -231.875f, 10.2956f, nullptr },
    { -11.2685f, -237.666f, 10.7819f, nullptr },
    { 22.1935f, -244.842f, 14.09f, nullptr },
    { 33.7178f, -258.971f, 14.8231f, nullptr },
    { 29.4264f, -283.735f, 16.3003f, nullptr },
    { 28.1264f, -302.593f, 15.076f, nullptr },
};

BattleBotPath vPath_AV_Stonehearth_Outpost_to_Captain_Balinda_Stonehearth =
{
    { 28.1264f, -302.593f, 15.076f, nullptr },
    { 19.509f, -300.648f, 14.02f, nullptr },
    { 11.9925f, -299.182f, 15.0127f, nullptr },
    { -2.23151f, -296.73f, 15.5632f, nullptr },
    { -20.1345f, -296.292f, 15.5632f, nullptr },
    { -25.8903f, -306.347f, 15.5632f, nullptr },
    { -31.9783f, -309.925f, 15.5632f, nullptr },
    { -46.5667f, -294.841f, 15.0786f, nullptr },
};

BattleBotPath vPath_AV_Stonehearth_Outpost_to_Stonehearth_Graveyard_Crossroad =
{
    { 28.1264f, -302.593f, 15.076f, nullptr },
    { 26.4279f, -302.732f, 14.7797f, nullptr },
    { 43.9172f, -312.202f, 18.0643f, nullptr },
    { 57.9471f, -317.071f, 25.175f, nullptr },
    { 82.0413f, -321.368f, 33.9446f, nullptr },
    { 102.051f, -333.675f, 39.3055f, nullptr },
    { 117.624f, -352.173f, 42.6668f, nullptr },
    { 121.462f, -367.206f, 43.1468f, nullptr },
    { 123.153f, -375.134f, 42.8991f, nullptr },
};

BattleBotPath vPath_AV_Stonehearth_Graveyard_Crossroad_to_Stonehearth_Graveyard_Flag =
{
    { 123.153f, -375.134f, 42.8991f, nullptr },
    { 101.9f, -389.081f, 45.0974f, nullptr },
    { 89.1516f, -393.047f, 45.1475f, nullptr },
    { 79.8805f, -401.379f, 46.516f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Stonehearth_Graveyard_to_Stonehearth_Graveyard_Flag =
{
    { 73.8433f, -485.163f, 48.7233f, nullptr },
    { 74.4106f, -469.205f, 48.5722f, nullptr },
    { 74.0364f, -450.204f, 48.7063f, nullptr },
    { 73.3269f, -433.156f, 49.0149f, nullptr },
    { 73.6789f, -417.638f, 48.9345f, nullptr },
    { 79.8805f, -401.379f, 46.516f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Stonehearth_Graveyard_Flag_to_Stonehearth_Graveyard_Second_Crossroad =
{
    { 79.8805f, -401.379f, 46.516f, &AV_AtFlag },
    { 59.3904f, -396.459f, 46.3454f, nullptr },
    { 38.213f, -391.053f, 45.6625f, nullptr },
    { 18.563f, -398.416f, 45.6217f, nullptr },
    { -14.0184f, -419.609f, 44.4167f, nullptr },
};

BattleBotPath vPath_AV_Stonehearth_Graveyard_Second_Crossroad_to_Stonehearth_Bunker_First_Crossroad =
{
    { -14.0184f, -419.609f, 44.4167f, nullptr },
    { -16.2311f, -430.739f, 45.9019f, nullptr },
    { -23.0513f, -442.558f, 46.1862f, nullptr },
    { -36.0353f, -454.259f, 45.1569f, nullptr },
    { -47.8724f, -462.469f, 41.2528f, nullptr },
    { -89.1045f, -457.978f, 24.1971f, nullptr },
    { -104.292f, -455.243f, 22.3564f, nullptr },
};

BattleBotPath vPath_AV_Stonehearth_Graveyard_Second_Crossroad_to_Iceblood_Garrison =
{
    { -14.0184f, -419.609f, 44.4167f, nullptr },
    { -23.0625f, -414.929f, 39.5583f, nullptr },
    { -35.4078f, -408.859f, 30.7036f, nullptr },
    { -43.2854f, -396.271f, 21.1548f, nullptr },
    { -52.1465f, -377.477f, 13.6804f, nullptr },
    { -63.422f, -348.549f, 12.6189f, nullptr },
    { -70.7889f, -334.456f, 12.3143f, nullptr },
    { -87.3986f, -321.764f, 10.286f, nullptr },
    { -107.322f, -315.203f, 9.54238f, nullptr },
    { -127.844f, -310.833f, 13.4444f, nullptr },
    { -155.644f, -305.004f, 10.38f, nullptr },
    { -187.766f, -302.183f, 6.66806f, nullptr },
    { -227.675f, -300.517f, 6.66806f, nullptr },
    { -244.583f, -291.433f, 6.66806f, nullptr },
    { -265.603f, -272.943f, 6.66806f, nullptr },
    { -287.897f, -256.006f, 6.71878f, nullptr },
    { -310.325f, -239.243f, 11.8723f, nullptr },
    { -325.404f, -233.386f, 17.9203f, nullptr },
    { -345.71f, -232.887f, 12.1903f, nullptr },
    { -360.847f, -211.82f, 12.329f, nullptr },
    { -372.245f, -196.784f, 12.5248f, nullptr },
    { -391.209f, -185.399f, 15.9205f, nullptr },
    { -404.248f, -184.159f, 20.8763f, nullptr },
    { -425.522f, -195.322f, 26.2692f, nullptr },
    { -436.78f, -199.258f, 29.3854f, nullptr },
    { -464.585f, -196.566f, 49.1431f, nullptr },
    { -492.17f, -187.077f, 57.1342f, nullptr },
};

BattleBotPath vPath_AV_Stonehearth_Graveyard_Second_Crossroad_to_Iceblood_Tower_Crossroad =
{
    { -104.292f, -455.243f, 22.3564f, nullptr },
    { -95.7107f, -437.38f, 18.6408f, nullptr },
    { -96.426f, -395.254f, 14.7414f, nullptr },
    { -117.9f, -376.108f, 12.2937f, nullptr },
    { -170.065f, -344.859f, 10.1405f, nullptr },
    { -220.336f, -329.639f, 8.81655f, nullptr },
    { -273.738f, -342.925f, 6.66864f, nullptr },
    { -307.056f, -315.782f, 6.66756f, nullptr },
    { -329.567f, -296.801f, 6.66756f, nullptr },
    { -354.044f, -289.519f, 9.62051f, nullptr },
    { -384.954f, -283.819f, 12.0969f, nullptr },
    { -411.689f, -272.086f, 16.7252f, nullptr },
    { -432.235f, -273.793f, 20.6022f, nullptr },
    { -452.713f, -277.187f, 21.7067f, nullptr },
    { -489.158f, -284.974f, 28.429f, nullptr },
    { -499.066f, -302.407f, 31.48f, nullptr },
    { -505.393f, -314.896f, 31.9995f, nullptr },
    { -522.97f, -340.616f, 34.5491f, nullptr },
    { -542.124f, -333.426f, 37.6958f, nullptr },
    { -561.111f, -324.477f, 40.0344f, nullptr },
    { -579.697f, -315.037f, 46.345f, nullptr },
};

BattleBotPath vPath_AV_Stonehearth_Bunker_First_Crossroad_to_Stonehearth_Bunker_Flag =
{
    { -104.292f, -455.243f, 22.3564f, nullptr },
    { -111.899f, -466.777f, 24.0451f, nullptr },
    { -122.548f, -480.921f, 26.5748f, nullptr },
    { -133.831f, -478.232f, 28.1725f, nullptr },
    { -128.332f, -462.402f, 26.4943f, nullptr },
    { -156.579f, -449.815f, 29.0267f, nullptr },
    { -168.009f, -444.6f, 33.2796f, nullptr },
    { -160.378f, -440.192f, 33.2796f, nullptr },
    { -154.387f, -445.423f, 33.2796f, nullptr },
    { -159.655f, -458.512f, 40.395f, nullptr },
    { -165.724f, -454.853f, 40.403f, nullptr },
    { -165.652f, -447.139f, 40.403f, nullptr },
    { -161.038f, -440.504f, 40.403f, nullptr },
    { -153.491f, -441.386f, 40.3957f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Stonehearth_Graveyard_Crossroad_to_Icewing_Bunker_Crossroad =
{
    { 123.153f, -375.134f, 42.8991f, nullptr },
    { 132.848f, -385.475f, 42.2487f, nullptr },
    { 147.934f, -393.887f, 42.6617f, nullptr },
    { 170.279f, -400.056f, 42.802f, nullptr },
    { 209.274f, -410.47f, 42.15f, nullptr },
    { 232.206f, -406.171f, 41.2464f, nullptr },
};

BattleBotPath vPath_AV_Icewing_Bunker_Crossroad_to_Icewing_Bunker_Flag =
{
    { 232.206f, -406.171f, 41.2464f, nullptr },
    { 232.415f, -399.32f, 43.0377f, nullptr },
    { 241.705f, -378.726f, 43.2973f, nullptr },
    { 243.685f, -361.498f, 43.2563f, &MoveToNextPointSpecial },
    { 233.604f, -348.561f, 42.4664f, &MoveToNextPointSpecial },
    { 208.557f, -367.638f, 44.8858f, nullptr },
    { 199.002f, -374.949f, 49.2678f, nullptr },
    { 197.929f, -366.972f, 49.2678f, nullptr },
    { 204.639f, -363.321f, 49.2678f, nullptr },
    { 214.635f, -374.753f, 56.3819f, nullptr },
    { 206.689f, -377.633f, 56.3917f, nullptr },
    { 199.606f, -370.834f, 56.3917f, nullptr },
    { 200.792f, -361.881f, 56.3798f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Icewing_Bunker_Crossroad_to_Alliance_Slope_Crossroad =
{
    { 232.206f, -406.171f, 41.2464f, nullptr },
    { 245.764f, -414.729f, 34.8094f, nullptr },
    { 260.329f, -406.891f, 26.2452f, nullptr },
    { 277.874f, -393.876f, 11.3203f, nullptr },
    { 292.624f, -385.478f, 3.80607f, nullptr },
    { 315.119f, -384.083f, -0.803525f, nullptr },
    { 352.975f, -389.716f, -0.510339f, nullptr },
    { 383.883f, -393.12f, -1.07409f, nullptr },
    { 401.915f, -389.568f, -1.24385f, nullptr },
};

BattleBotPath vPath_AV_Alliance_Slope_Crossroad_to_Stormpike_Crossroad =
{
    { 401.915f, -389.568f, -1.24385f, nullptr },
    { 411.259f, -385.402f, -1.24337f, nullptr },
    { 424.079f, -380.069f, -1.24337f, nullptr },
    { 462.535f, -368.534f, -1.24387f, nullptr },
    { 508.895f, -330.261f, -1.08467f, nullptr },
    { 517.577f, -324.732f, -1.03504f, nullptr },
    { 536.967f, -321.273f, 3.75218f, nullptr },
    { 557.529f, -324.368f, 15.726f, nullptr },
    { 578.914f, -330.968f, 28.2467f, nullptr },
    { 597.588f, -336.744f, 30.2853f, nullptr },
    { 608.74f, -333.816f, 30.5787f, nullptr },
    { 621.331f, -324.856f, 30.1337f, nullptr },
    { 629.341f, -313.696f, 30.1337f, nullptr },
    { 638.087f, -287.84f, 30.1471f, nullptr },
};

BattleBotPath vPath_AV_Stormpike_Crossroad_to_Alliance_Base_Bunker_First_Crossroad =
{
    { 638.087f, -287.84f, 30.1471f, nullptr },
    { 635.381f, -271.761f, 30.1326f, nullptr },
    { 633.779f, -257.854f, 33.1093f, nullptr },
    { 631.23f, -233.502f, 37.2848f, nullptr },
    { 625.312f, -191.934f, 38.782f, nullptr },
    { 622.182f, -167.638f, 36.3214f, nullptr },
    { 619.956f, -150.28f, 33.3684f, nullptr },
    { 620.254f, -135.032f, 33.4412f, nullptr },
    { 629.777f, -99.9175f, 40.6453f, nullptr },
};

BattleBotPath vPath_AV_Alliance_Base_Bunker_First_Crossroad_to_Alliance_Base_North_Bunker =
{
    { 629.777f, -99.9175f, 40.6453f, nullptr },
    { 635.309f, -97.7424f, 41.9851f, nullptr },
    { 642.07f, -93.7443f, 46.1184f, nullptr },
    { 659.709f, -104.838f, 51.5034f, nullptr },
    { 654.41f, -118.753f, 49.7697f, nullptr },
    { 661.432f, -124.145f, 49.6422f, nullptr },
    { 679.011f, -135.631f, 51.9985f, nullptr },
    { 689.794f, -142.705f, 56.5425f, nullptr },
    { 683.375f, -145.993f, 56.5425f, nullptr },
    { 676.456f, -140.675f, 56.5425f, nullptr },
    { 684.145f, -127.197f, 63.6535f, nullptr },
    { 678.008f, -125.249f, 63.6667f, nullptr },
    { 671.246f, -128.806f, 63.665f, nullptr },
    { 669.384f, -135.545f, 63.6574f, nullptr },
    { 672.685f, -142.49f, 63.6571f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Alliance_Base_Bunker_First_Crossroad_to_Alliance_Base_Bunker_Second_Crossroad =
{
    { 629.777f, -99.9175f, 40.6453f, nullptr },
    { 633.117f, -67.768f, 41.3917f, nullptr },
};

BattleBotPath vPath_AV_Alliance_Base_Bunker_Second_Crossroad_to_Alliance_Base_South_Bunker =
{
    { 633.117f, -67.768f, 41.3917f, nullptr },
    { 624.951f, -67.4683f, 40.4152f, nullptr },
    { 616.973f, -73.0334f, 38.8073f, nullptr },
    { 596.24f, -89.2897f, 38.855f, nullptr },
    { 591.941f, -86.7649f, 39.5782f, nullptr },
    { 585.983f, -74.2185f, 38.0143f, nullptr },
    { 557.244f, -87.113f, 40.4615f, nullptr },
    { 546.247f, -91.6955f, 44.8191f, nullptr },
    { 548.186f, -82.8881f, 44.8191f, nullptr },
    { 555.216f, -83.2067f, 44.8191f, nullptr },
    { 561.528f, -94.9507f, 51.9364f, nullptr },
    { 567.204f, -90.1402f, 51.9291f, nullptr },
    { 566.935f, -81.8903f, 51.9429f, nullptr },
    { 560.568f, -77.4604f, 51.9305f, nullptr },
    { 555.018f, -77.9842f, 51.9347f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Alliance_Base_Bunker_Second_Crossroad_to_Alliance_Base_Bunker_Third_Crossroad =
{
    { 633.117f, -67.768f, 41.3917f, nullptr },
    { 635.133f, -51.7416f, 42.3031f, nullptr },
    { 648.593f, -33.8686f, 47.1592f, nullptr },
};

BattleBotPath vPath_AV_Alliance_Base_Bunker_Third_Crossroad_to_Alliance_Base_Flag =
{
    { 648.593f, -33.8686f, 47.1592f, nullptr },
    { 640.404f, -32.0183f, 46.2328f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Alliance_Base_Bunker_Third_Crossroad_to_Alliance_Base_Vanndar_Stormpike =
{
    { 648.593f, -33.8686f, 47.1592f, nullptr },
    { 664.325f, -28.0147f, 50.6198f, nullptr },
    { 690.605f, -20.6846f, 50.6198f, nullptr },
    { 696.331f, -27.5629f, 50.6198f, nullptr },
    { 699.006f, -31.7397f, 50.6198f, nullptr },
    { 704.958f, -34.5659f, 50.6198f, nullptr },
    { 717.709f, -16.6861f, 50.1354f, nullptr },
};

BattleBotPath vPath_AV_Stormpike_Crossroad_to_Stormpike_Flag =
{
    { 638.087f, -287.84f, 30.1471f, nullptr },
    { 667.173f, -295.225f, 30.29f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Stormpike_Graveyard_to_Stormpike_Flag =
{
    { 676.0f, -374.0f, 29.782f, nullptr },
    { 665.922f, -347.777f, 29.493f, nullptr },
    { 667.173f, -295.225f, 30.29f, &AV_AtFlag },
};

BattleBotPath vPath_AV_Alliance_Cave_Slop_Crossroad_to_Alliance_Slope_Crossroad =
{
    { 450.8f, -434.864f, 30.5126f, nullptr },
    { 442.575f, -430.266f, 26.6539f, nullptr },
    { 422.36f, -412.85f, 12.285f, nullptr },
    { 401.915f, -389.568f, -1.24385f, nullptr },
};

BattleBotPath vPath_AV_Alliance_Cave_to_Alliance_Cave_Slop_Crossroad =
{
    { 769.016f, -491.165f, 97.7772f, &AtCaveExit },
    { 758.026f, -489.447f, 95.9521f, nullptr },
    { 742.169f, -480.684f, 85.9649f, nullptr },
    { 713.063f, -467.311f, 71.0884f, nullptr },
    { 694.957f, -434.171f, 62.8627f, nullptr },
    { 686.386f, -420.276f, 64.5562f, nullptr },
    { 650.413f, -401.791f, 67.9546f, nullptr },
    { 626.412f, -384.308f, 67.5032f, nullptr },
    { 608.756f, -385.851f, 66.5105f, nullptr },
    { 576.021f, -395.976f, 63.5599f, nullptr },
    { 546.555f, -397.812f, 52.655f, nullptr },
    { 514.201f, -416.131f, 42.4508f, nullptr },
    { 450.8f, -434.864f, 30.5126f, nullptr },
};

// AV mine paths: start from existing vPaths_AV waypoints so bots can chain onto them naturally.

// Mine paths keep pathfinding in MoveToNextPointSpecial so the pathfinder can
// handle the steep ascent through the mine interior without rerouting through
// terrain walls. Non-mine special tower/bunker steps keep the old direct run.
// WPs 0-18: outdoor section (Stormpike Crossroad → mine entrance).
// WPs 19-192: full mine interior patrol recorded 20260520_233444.
BattleBotPath vPath_AV_Stormpike_to_Irondeep_Morloch =
{
    { 638.0870f, -287.8400f, 30.1471f, &MoveToNextPointSpecial },
    { 644.7953f, -285.8409f, 30.3311f, &MoveToNextPointSpecial },
    { 652.8074f, -285.3333f, 30.4394f, &MoveToNextPointSpecial },
    { 660.1908f, -285.0237f, 30.0100f, &MoveToNextPointSpecial },
    { 666.2333f, -282.7752f, 29.0930f, &MoveToNextPointSpecial },
    { 672.6602f, -280.0033f, 26.9573f, &MoveToNextPointSpecial },
    { 679.0343f, -277.1199f, 24.4300f, &MoveToNextPointSpecial },
    { 683.7335f, -275.3425f, 23.8273f, &MoveToNextPointSpecial },
    { 689.2477f, -272.4332f, 23.2755f, &MoveToNextPointSpecial },
    { 695.7837f, -271.5105f, 25.3758f, &MoveToNextPointSpecial },
    { 702.5557f, -271.7004f, 28.8104f, &MoveToNextPointSpecial },
    { 709.0307f, -273.7076f, 32.7204f, &MoveToNextPointSpecial },
    { 715.5093f, -276.3286f, 36.2839f, &MoveToNextPointSpecial },
    { 721.7195f, -280.7448f, 39.4063f, &MoveToNextPointSpecial },
    { 726.5405f, -285.8130f, 42.1494f, &MoveToNextPointSpecial },
    { 730.7940f, -291.3660f, 45.2236f, &MoveToNextPointSpecial },
    { 734.8254f, -297.0817f, 47.8769f, &MoveToNextPointSpecial },
    { 738.8505f, -302.8087f, 50.4943f, &MoveToNextPointSpecial },
    { 742.8948f, -308.5220f, 53.1261f, &MoveToNextPointSpecial },
    { 749.9741f, -313.0966f, 55.2926f, nullptr },
    { 757.1210f, -320.5660f, 57.5585f, nullptr },
    { 757.6140f, -327.3914f, 58.6334f, nullptr },
    { 762.2471f, -332.6387f, 59.8886f, nullptr },
    { 766.9348f, -337.8362f, 61.4114f, nullptr },
    { 771.8456f, -342.0069f, 61.4114f, nullptr },
    { 776.7856f, -343.9720f, 61.4114f, nullptr },
    { 782.6708f, -343.0394f, 61.4114f, nullptr },
    { 788.0584f, -336.5657f, 62.6441f, nullptr },
    { 790.6700f, -332.0114f, 63.0328f, nullptr },
    { 797.3383f, -334.8674f, 62.9938f, nullptr },
    { 801.7383f, -338.1904f, 63.4820f, nullptr },
    { 808.1951f, -338.0619f, 64.4889f, nullptr },
    { 812.2214f, -334.4272f, 64.5452f, nullptr },
    { 815.8193f, -329.9876f, 64.3131f, nullptr },
    { 821.9169f, -328.8660f, 64.1093f, nullptr },
    { 827.7714f, -330.2855f, 64.3336f, nullptr },
    { 833.4594f, -334.3627f, 64.7016f, nullptr },
    { 839.1688f, -338.4121f, 65.4207f, nullptr },
    { 845.1281f, -342.0790f, 65.9065f, nullptr },
    { 851.2982f, -344.5918f, 65.9788f, nullptr },
    { 857.9470f, -346.7812f, 65.2467f, nullptr },
    { 863.2656f, -348.4494f, 64.3609f, nullptr },
    { 869.2357f, -348.0496f, 64.5193f, nullptr },
    { 874.9837f, -346.2871f, 65.4428f, nullptr },
    { 881.2501f, -343.1736f, 66.7161f, nullptr },
    { 887.3643f, -339.7654f, 67.3662f, nullptr },
    { 893.4669f, -336.3365f, 67.5765f, nullptr },
    { 899.4143f, -331.9313f, 67.5117f, nullptr },
    { 905.8758f, -329.8451f, 67.3239f, nullptr },
    { 910.8824f, -331.6965f, 66.6906f, nullptr },
    { 917.2162f, -334.6771f, 66.2647f, nullptr },
    { 923.5309f, -337.6971f, 66.0783f, nullptr },
    { 928.4184f, -339.8672f, 65.2748f, nullptr },
    { 933.7018f, -339.7291f, 64.3335f, nullptr },
    { 939.2121f, -336.6114f, 63.2957f, nullptr },
    { 945.1691f, -332.9358f, 62.4234f, nullptr },
    { 951.9389f, -329.9099f, 62.0142f, nullptr },
    { 958.3546f, -327.4232f, 61.8992f, nullptr },
    { 952.9962f, -332.6707f, 62.6635f, nullptr },
    { 946.2765f, -334.6309f, 63.0563f, nullptr },
    { 939.6093f, -336.7635f, 63.2882f, nullptr },
    { 932.6397f, -340.4206f, 64.5885f, nullptr },
    { 930.6841f, -345.8302f, 65.6047f, nullptr },
    { 932.0636f, -352.6909f, 66.0368f, nullptr },
    { 927.6450f, -359.0630f, 65.7745f, nullptr },
    { 922.5593f, -362.6730f, 66.4072f, nullptr },
    { 923.9247f, -367.8446f, 65.8885f, nullptr },
    { 930.0101f, -370.9507f, 66.3248f, nullptr },
    { 934.0178f, -374.6649f, 65.9521f, nullptr },
    { 934.5198f, -379.9965f, 64.2827f, nullptr },
    { 938.4604f, -384.9354f, 63.6908f, nullptr },
    { 938.5239f, -390.3865f, 62.6524f, nullptr },
    { 935.8464f, -395.7358f, 61.4821f, nullptr },
    { 929.3687f, -397.9735f, 60.5762f, nullptr },
    { 923.7055f, -396.7885f, 60.3456f, nullptr },
    { 920.4825f, -402.5423f, 58.8938f, nullptr },
    { 920.6826f, -409.4213f, 57.1628f, nullptr },
    { 924.3575f, -415.5543f, 55.9128f, nullptr },
    { 928.8966f, -417.6603f, 56.3921f, nullptr },
    { 927.8540f, -425.4566f, 56.6616f, nullptr },
    { 930.9374f, -430.1976f, 55.5799f, nullptr },
    { 935.9874f, -432.6905f, 55.4507f, nullptr },
    { 940.8868f, -437.3302f, 55.2866f, nullptr },
    { 945.0168f, -441.5298f, 55.1237f, nullptr },
    { 951.1625f, -443.5104f, 55.6050f, nullptr },
    { 959.0090f, -441.7212f, 55.7698f, nullptr },
    { 964.3408f, -441.2900f, 56.0055f, nullptr },
    { 969.7030f, -442.5423f, 56.9035f, nullptr },
    { 969.9769f, -448.7678f, 57.3856f, nullptr },
    { 967.9329f, -453.5570f, 57.5247f, nullptr },
    { 961.5345f, -455.3502f, 56.7296f, nullptr },
    { 955.2478f, -452.4745f, 56.9069f, nullptr },
    { 951.2103f, -446.7579f, 56.3420f, nullptr },
    { 946.8184f, -442.0459f, 55.1497f, nullptr },
    { 941.0389f, -438.3454f, 55.3832f, nullptr },
    { 934.5074f, -435.8312f, 55.7185f, nullptr },
    { 927.7856f, -433.8958f, 55.7789f, nullptr },
    { 922.3573f, -433.5148f, 56.4266f, nullptr },
    { 915.8545f, -436.4665f, 57.1670f, nullptr },
    { 911.9377f, -441.2616f, 57.3735f, nullptr },
    { 909.4339f, -447.2007f, 57.5979f, nullptr },
    { 907.5444f, -453.9388f, 58.0954f, nullptr },
    { 906.0735f, -462.2965f, 58.9123f, nullptr },
    { 906.5991f, -468.4090f, 58.7244f, nullptr },
    { 903.4094f, -472.6341f, 58.4722f, nullptr },
    { 903.0264f, -466.1058f, 58.7565f, nullptr },
    { 905.4053f, -459.5250f, 58.7587f, nullptr },
    { 907.1579f, -452.7517f, 58.0913f, nullptr },
    { 908.1906f, -445.8306f, 57.8903f, nullptr },
    { 907.0223f, -438.1575f, 57.9183f, nullptr },
    { 902.0360f, -434.8175f, 58.3467f, nullptr },
    { 895.3373f, -432.7959f, 57.1080f, nullptr },
    { 889.1259f, -429.2722f, 54.5588f, nullptr },
    { 883.8253f, -425.8016f, 53.4364f, nullptr },
    { 878.2059f, -422.4061f, 52.5859f, nullptr },
    { 872.4256f, -420.4440f, 51.4132f, nullptr },
    { 866.6716f, -423.0157f, 50.8839f, nullptr },
    { 864.5043f, -429.0670f, 50.5117f, nullptr },
    { 862.7550f, -434.3589f, 50.2492f, nullptr },
    { 863.1865f, -439.6559f, 50.5190f, nullptr },
    { 867.9703f, -435.7375f, 50.8621f, nullptr },
    { 872.9225f, -438.6662f, 52.4538f, nullptr },
    { 877.4988f, -444.3521f, 54.6103f, nullptr },
    { 882.3982f, -442.0868f, 54.6513f, nullptr },
    { 875.0012f, -442.5977f, 54.3540f, nullptr },
    { 870.5634f, -437.4556f, 51.2488f, nullptr },
    { 867.6713f, -431.0822f, 50.6324f, nullptr },
    { 866.0455f, -425.4446f, 50.6998f, nullptr },
    { 861.5696f, -421.9699f, 51.0493f, nullptr },
    { 855.8695f, -420.4463f, 50.6229f, nullptr },
    { 848.9927f, -419.2355f, 49.8845f, nullptr },
    { 843.2914f, -417.6939f, 48.2993f, nullptr },
    { 836.8456f, -418.4646f, 47.3612f, nullptr },
    { 831.8464f, -413.8861f, 47.7531f, nullptr },
    { 827.6541f, -408.2002f, 48.3558f, nullptr },
    { 826.8242f, -400.0729f, 47.6965f, nullptr },
    { 827.5244f, -393.9712f, 47.6674f, nullptr },
    { 827.5768f, -388.5997f, 47.7292f, nullptr },
    { 829.6581f, -380.7886f, 47.6118f, nullptr },
    { 832.6170f, -375.1532f, 48.0625f, nullptr },
    { 835.6160f, -369.9710f, 48.2821f, nullptr },
    { 841.8552f, -369.1906f, 48.2812f, nullptr },
    { 849.4988f, -368.9225f, 47.6919f, nullptr },
    { 845.3161f, -365.4579f, 47.5076f, nullptr },
    { 840.3666f, -367.0207f, 47.9496f, nullptr },
    { 834.9725f, -363.5636f, 47.9857f, nullptr },
    { 831.9970f, -357.2296f, 47.5389f, nullptr },
    { 829.9485f, -349.8815f, 46.9633f, nullptr },
    { 826.4904f, -345.6635f, 48.0302f, nullptr },
    { 821.4036f, -341.5811f, 49.3061f, nullptr },
    { 815.6425f, -337.6053f, 49.5992f, nullptr },
    { 811.4964f, -334.0757f, 50.6470f, nullptr },
    { 806.9940f, -329.1862f, 51.9886f, nullptr },
    { 802.8763f, -325.4006f, 52.3027f, nullptr },
    { 799.8839f, -320.5759f, 52.6741f, nullptr },
    { 795.9949f, -316.3070f, 54.0191f, nullptr },
    { 800.8972f, -321.7757f, 52.3870f, nullptr },
    { 805.3230f, -327.1974f, 52.1261f, nullptr },
    { 809.9796f, -332.4237f, 51.0680f, nullptr },
    { 814.9395f, -337.3608f, 49.7216f, nullptr },
    { 820.0442f, -342.1493f, 49.0618f, nullptr },
    { 825.3945f, -346.6621f, 47.8123f, nullptr },
    { 830.2791f, -351.7588f, 46.9812f, nullptr },
    { 832.2368f, -356.5119f, 47.5205f, nullptr },
    { 832.9245f, -362.1476f, 47.8918f, nullptr },
    { 832.2316f, -367.8243f, 48.0283f, nullptr },
    { 829.7338f, -373.3723f, 47.8687f, nullptr },
    { 827.9733f, -379.3525f, 47.5795f, nullptr },
    { 826.8942f, -386.2657f, 47.7607f, nullptr },
    { 826.5557f, -393.2550f, 47.7490f, nullptr },
    { 826.4606f, -400.2542f, 47.7532f, nullptr },
    { 826.5320f, -408.1244f, 48.5734f, nullptr },
    { 829.2873f, -413.4566f, 48.2818f, nullptr },
    { 831.7641f, -418.7356f, 47.6215f, nullptr },
    { 828.7030f, -424.1270f, 48.0091f, nullptr },
    { 827.1041f, -430.6973f, 48.1797f, nullptr },
    { 826.2712f, -438.9842f, 49.0085f, nullptr },
    { 824.8983f, -445.0700f, 49.4648f, nullptr },
    { 822.2959f, -452.4495f, 48.7827f, nullptr },
    { 826.5121f, -456.5996f, 48.7336f, nullptr },
    { 830.4738f, -452.9874f, 48.4066f, nullptr },
    { 828.0916f, -446.1214f, 48.9109f, nullptr },
    { 828.7092f, -439.8293f, 48.4782f, nullptr },
    { 829.1599f, -433.9692f, 47.9110f, nullptr },
    { 831.1500f, -429.2800f, 47.6688f, nullptr },
    { 834.0696f, -422.2478f, 47.6638f, nullptr },
    { 838.5847f, -418.4668f, 47.4341f, nullptr },
    { 843.6133f, -416.7885f, 48.3666f, nullptr },
    { 849.0623f, -418.3384f, 49.8665f, nullptr },
    { 854.1639f, -420.6887f, 50.4827f, nullptr },
    { 859.1237f, -427.0638f, 50.5451f, nullptr },
    { 861.6859f, -433.5741f, 50.1260f, nullptr },
    { 863.7015f, -440.2736f, 50.6062f, nullptr },
};

// WPs 0-22: outdoor section (TowerPoint crossroad → mine entrance).
// WPs 23-241: full mine interior patrol recorded 20260520_234012.
BattleBotPath vPath_AV_TowerPoint_to_Coldtooth_Snivvle =
{
    { -846.8260f, -355.1810f, 50.0754f, nullptr },
    { -853.7689f, -356.0296f, 50.7901f, nullptr },
    { -860.7067f, -356.9613f, 50.6432f, nullptr },
    { -867.6421f, -357.8927f, 51.0649f, nullptr },
    { -873.5925f, -357.9070f, 50.7679f, nullptr },
    { -879.5068f, -354.8615f, 50.5226f, nullptr },
    { -885.4896f, -351.2273f, 50.3338f, nullptr },
    { -891.4723f, -347.5932f, 50.1704f, nullptr },
    { -897.4550f, -343.9590f, 50.1674f, nullptr },
    { -903.4377f, -340.3249f, 52.2986f, nullptr },
    { -909.6059f, -337.0194f, 55.4422f, nullptr },
    { -915.6081f, -333.4230f, 58.0387f, nullptr },
    { -921.3315f, -329.3953f, 61.7148f, nullptr },
    { -926.8960f, -325.1484f, 65.0706f, nullptr },
    { -932.5949f, -319.1186f, 66.5863f, nullptr },
    { -937.7026f, -314.3339f, 66.6110f, nullptr },
    { -941.9382f, -307.9189f, 65.8326f, nullptr },
    { -944.8809f, -301.5678f, 65.1650f, nullptr },
    { -947.9769f, -295.2897f, 64.5595f, nullptr },
    { -950.9139f, -288.9386f, 64.1245f, nullptr },
    { -953.5179f, -282.4456f, 63.6195f, nullptr },
    { -956.5792f, -276.1507f, 63.5245f, nullptr },
    { -959.2665f, -269.6910f, 63.7585f, nullptr },
    { -958.0262f, -267.3844f, 63.9660f, nullptr },
    { -958.4664f, -262.3926f, 64.4046f, nullptr },
    { -957.0034f, -256.2473f, 65.0218f, nullptr },
    { -954.2914f, -251.9035f, 65.3252f, nullptr },
    { -948.9855f, -247.8890f, 65.7690f, nullptr },
    { -943.1499f, -244.0231f, 66.0588f, nullptr },
    { -937.3189f, -240.1602f, 66.7761f, nullptr },
    { -931.7636f, -236.4554f, 67.3808f, nullptr },
    { -926.6116f, -231.7229f, 68.1387f, nullptr },
    { -921.2256f, -225.7400f, 70.3748f, nullptr },
    { -921.2448f, -219.1600f, 71.2780f, nullptr },
    { -924.2188f, -212.8233f, 72.1173f, nullptr },
    { -928.0421f, -206.6777f, 73.1019f, nullptr },
    { -935.9578f, -203.6708f, 75.5265f, nullptr },
    { -941.0697f, -202.0344f, 76.7069f, nullptr },
    { -947.9166f, -200.6016f, 76.9914f, nullptr },
    { -944.6596f, -194.5357f, 76.9694f, nullptr },
    { -942.4153f, -189.9558f, 77.2564f, nullptr },
    { -944.3124f, -183.0910f, 78.4790f, nullptr },
    { -949.2844f, -178.7640f, 78.3343f, nullptr },
    { -951.5369f, -173.1046f, 78.2042f, nullptr },
    { -951.3700f, -165.9680f, 78.6602f, nullptr },
    { -950.1734f, -158.1762f, 78.6425f, nullptr },
    { -947.6752f, -151.6373f, 79.3271f, nullptr },
    { -946.3480f, -146.6797f, 79.6631f, nullptr },
    { -945.3856f, -138.8983f, 79.9083f, nullptr },
    { -943.4861f, -133.0375f, 79.3952f, nullptr },
    { -943.8383f, -126.4054f, 78.4719f, nullptr },
    { -942.2822f, -121.6436f, 78.1914f, nullptr },
    { -943.1924f, -116.4762f, 79.0995f, nullptr },
    { -946.3169f, -112.0375f, 79.7562f, nullptr },
    { -951.3091f, -109.1263f, 80.6905f, nullptr },
    { -954.9819f, -105.0732f, 81.2087f, nullptr },
    { -960.0182f, -98.9004f, 81.4630f, nullptr },
    { -964.3766f, -92.4264f, 81.4304f, nullptr },
    { -969.5428f, -88.8461f, 81.3587f, nullptr },
    { -971.3616f, -83.1058f, 80.6514f, nullptr },
    { -968.1661f, -75.8115f, 79.9334f, nullptr },
    { -964.1345f, -69.6919f, 80.0251f, nullptr },
    { -963.3066f, -63.9375f, 79.1705f, nullptr },
    { -966.1097f, -58.7191f, 77.8783f, nullptr },
    { -968.2316f, -54.0880f, 77.1291f, nullptr },
    { -972.1962f, -50.2777f, 76.2404f, nullptr },
    { -976.3509f, -43.0637f, 75.9518f, nullptr },
    { -978.3452f, -37.8310f, 75.8187f, nullptr },
    { -973.0225f, -39.3237f, 77.2176f, nullptr },
    { -970.2410f, -45.2428f, 77.6381f, nullptr },
    { -967.9938f, -50.2206f, 77.6783f, nullptr },
    { -963.6252f, -54.5048f, 78.7189f, nullptr },
    { -958.8996f, -56.7868f, 79.0756f, nullptr },
    { -953.3061f, -57.5785f, 79.8634f, nullptr },
    { -948.4694f, -56.2615f, 79.9092f, nullptr },
    { -943.3876f, -62.9281f, 79.7095f, nullptr },
    { -938.1875f, -65.3834f, 80.2135f, nullptr },
    { -933.1954f, -61.0548f, 79.8231f, nullptr },
    { -931.1484f, -56.4638f, 80.0582f, nullptr },
    { -928.4746f, -50.7514f, 79.5639f, nullptr },
    { -924.5546f, -47.0789f, 78.5143f, nullptr },
    { -919.5144f, -45.0371f, 77.5098f, nullptr },
    { -912.5443f, -43.2746f, 76.1364f, nullptr },
    { -908.5737f, -47.4050f, 75.3328f, nullptr },
    { -904.6812f, -54.0991f, 74.0286f, nullptr },
    { -903.4513f, -60.0849f, 73.7683f, nullptr },
    { -906.9292f, -65.6850f, 74.0121f, nullptr },
    { -907.1702f, -71.5634f, 74.2805f, nullptr },
    { -904.6023f, -77.4272f, 74.5471f, nullptr },
    { -899.8743f, -81.6577f, 74.1212f, nullptr },
    { -898.9984f, -88.1269f, 74.3444f, nullptr },
    { -897.8920f, -93.6501f, 74.5764f, nullptr },
    { -897.6190f, -98.8714f, 74.9345f, nullptr },
    { -898.5678f, -104.5814f, 75.1668f, nullptr },
    { -898.7932f, -109.9793f, 75.2446f, nullptr },
    { -896.1526f, -115.0747f, 75.7175f, nullptr },
    { -895.7001f, -120.3225f, 75.5120f, nullptr },
    { -897.3519f, -125.1866f, 74.9976f, nullptr },
    { -899.6663f, -131.0912f, 74.7799f, nullptr },
    { -902.4193f, -135.6834f, 74.9487f, nullptr },
    { -903.3541f, -141.7113f, 75.6109f, nullptr },
    { -908.5148f, -146.4506f, 76.6521f, nullptr },
    { -914.8497f, -148.5027f, 76.9636f, nullptr },
    { -908.6008f, -144.8061f, 76.6648f, nullptr },
    { -905.1782f, -138.7060f, 75.4483f, nullptr },
    { -901.9520f, -133.1742f, 74.8083f, nullptr },
    { -900.2200f, -127.4110f, 75.0022f, nullptr },
    { -897.4749f, -122.1850f, 75.2973f, nullptr },
    { -893.7896f, -116.2339f, 75.8101f, nullptr },
    { -890.1310f, -109.9049f, 75.9451f, nullptr },
    { -889.5350f, -103.4125f, 75.9082f, nullptr },
    { -893.0192f, -96.6152f, 74.9999f, nullptr },
    { -896.6045f, -90.6039f, 74.3748f, nullptr },
    { -900.3971f, -84.7206f, 74.3495f, nullptr },
    { -903.6666f, -78.7883f, 74.5309f, nullptr },
    { -906.2255f, -73.6893f, 74.3782f, nullptr },
    { -908.8524f, -69.2275f, 74.2432f, nullptr },
    { -906.9767f, -62.0069f, 74.3351f, nullptr },
    { -901.9204f, -58.2554f, 73.3702f, nullptr },
    { -895.9319f, -57.0817f, 71.6221f, nullptr },
    { -888.6734f, -56.2531f, 70.0344f, nullptr },
    { -883.7520f, -55.0271f, 69.9786f, nullptr },
    { -876.8777f, -53.0992f, 70.2030f, nullptr },
    { -875.2785f, -47.4192f, 69.8982f, nullptr },
    { -873.2711f, -41.9683f, 69.3706f, nullptr },
    { -870.6176f, -34.6212f, 69.1455f, nullptr },
    { -868.2780f, -28.6154f, 68.7580f, nullptr },
    { -866.7368f, -22.9221f, 69.0853f, nullptr },
    { -868.8026f, -16.2417f, 69.4659f, nullptr },
    { -868.7053f, -10.6551f, 69.8751f, nullptr },
    { -863.8822f, -6.2685f, 70.7067f, nullptr },
    { -859.8013f, -2.4118f, 71.6447f, nullptr },
    { -856.1391f, -6.7193f, 71.0237f, nullptr },
    { -853.6738f, -13.9860f, 70.4332f, nullptr },
    { -859.3860f, -20.1693f, 70.6999f, nullptr },
    { -863.8373f, -24.8841f, 69.4740f, nullptr },
    { -864.6612f, -30.4740f, 69.3611f, nullptr },
    { -865.8622f, -35.5748f, 70.3591f, nullptr },
    { -868.1232f, -40.4532f, 69.9533f, nullptr },
    { -869.6132f, -45.6944f, 69.6896f, nullptr },
    { -868.4382f, -53.5580f, 70.4874f, nullptr },
    { -870.3230f, -58.3560f, 71.0317f, nullptr },
    { -865.8256f, -61.1102f, 71.2723f, nullptr },
    { -859.5298f, -61.3595f, 71.2873f, nullptr },
    { -851.1862f, -61.9063f, 71.5038f, nullptr },
    { -845.3550f, -62.3956f, 72.0334f, nullptr },
    { -839.2603f, -60.7806f, 72.7859f, nullptr },
    { -834.1891f, -60.1509f, 72.9528f, nullptr },
    { -829.0896f, -62.0853f, 72.3183f, nullptr },
    { -826.5691f, -66.6731f, 72.6021f, nullptr },
    { -833.9263f, -66.8847f, 72.6789f, nullptr },
    { -840.8912f, -66.1959f, 72.5903f, nullptr },
    { -847.2831f, -66.6002f, 72.4345f, nullptr },
    { -854.0241f, -67.7552f, 72.7765f, nullptr },
    { -859.3011f, -68.7990f, 72.4025f, nullptr },
    { -864.7905f, -71.1847f, 72.1895f, nullptr },
    { -869.0612f, -74.3124f, 72.3893f, nullptr },
    { -865.5281f, -80.5923f, 70.4016f, nullptr },
    { -868.8170f, -86.7670f, 68.1529f, nullptr },
    { -871.7861f, -93.1056f, 67.3047f, nullptr },
    { -873.4398f, -99.2777f, 66.3087f, nullptr },
    { -872.7027f, -106.2755f, 65.0578f, nullptr },
    { -869.8052f, -111.0749f, 64.8469f, nullptr },
    { -864.2855f, -111.5721f, 64.4058f, nullptr },
    { -856.9188f, -110.2722f, 64.2302f, nullptr },
    { -855.4336f, -105.2469f, 64.8580f, nullptr },
    { -854.3452f, -99.2238f, 67.2187f, nullptr },
    { -850.1052f, -95.9145f, 68.5014f, nullptr },
    { -852.6627f, -90.5634f, 68.5526f, nullptr },
    { -852.6336f, -96.1762f, 68.5832f, nullptr },
    { -854.3484f, -100.8989f, 66.4341f, nullptr },
    { -857.0208f, -105.1530f, 64.8217f, nullptr },
    { -861.5042f, -110.5248f, 64.2614f, nullptr },
    { -866.1697f, -116.1414f, 64.6379f, nullptr },
    { -870.2924f, -119.7013f, 64.5695f, nullptr },
    { -874.2936f, -125.2442f, 64.1393f, nullptr },
    { -871.5055f, -131.4679f, 62.7613f, nullptr },
    { -868.3973f, -135.4443f, 61.6424f, nullptr },
    { -868.5043f, -142.4357f, 61.3941f, nullptr },
    { -871.6622f, -147.8361f, 62.2462f, nullptr },
    { -875.9318f, -151.0173f, 62.5615f, nullptr },
    { -881.2260f, -152.7749f, 62.0661f, nullptr },
    { -888.4754f, -152.3926f, 61.5388f, nullptr },
    { -895.3615f, -151.1348f, 61.4920f, nullptr },
    { -902.2600f, -149.9507f, 61.7487f, nullptr },
    { -909.6881f, -149.3220f, 61.9902f, nullptr },
    { -916.4929f, -146.4249f, 62.2129f, nullptr },
    { -917.5007f, -140.4414f, 62.1153f, nullptr },
    { -918.4023f, -133.5360f, 61.8556f, nullptr },
    { -922.8866f, -137.5430f, 61.2437f, nullptr },
    { -923.1592f, -143.7832f, 61.5751f, nullptr },
    { -924.0225f, -149.2767f, 61.8780f, nullptr },
    { -927.8495f, -153.0869f, 61.6019f, nullptr },
    { -932.9203f, -155.6496f, 61.0010f, nullptr },
    { -937.5087f, -159.9830f, 61.2187f, nullptr },
    { -941.0455f, -166.0211f, 63.0277f, nullptr },
    { -944.4594f, -172.1290f, 63.4874f, nullptr },
    { -948.0247f, -178.1518f, 64.7075f, nullptr },
    { -952.0921f, -183.4381f, 65.7220f, nullptr },
    { -956.7747f, -188.6405f, 66.3722f, nullptr },
    { -961.2820f, -194.6123f, 67.1811f, nullptr },
    { -955.5977f, -194.4117f, 67.0453f, nullptr },
    { -952.0972f, -188.3499f, 66.2930f, nullptr },
    { -948.6029f, -182.2846f, 65.1494f, nullptr },
    { -944.8737f, -176.3638f, 63.8845f, nullptr },
    { -941.0584f, -170.4951f, 63.0485f, nullptr },
    { -937.0164f, -164.7811f, 61.7956f, nullptr },
    { -932.5897f, -159.2725f, 60.9509f, nullptr },
    { -927.3860f, -154.2337f, 61.5418f, nullptr },
    { -922.6979f, -151.9650f, 61.8396f, nullptr },
    { -916.4889f, -151.8197f, 62.0379f, nullptr },
    { -909.1931f, -154.6437f, 61.6565f, nullptr },
    { -903.5311f, -157.5129f, 61.4907f, nullptr },
    { -895.6739f, -158.1805f, 62.2065f, nullptr },
    { -888.7617f, -157.0911f, 61.9305f, nullptr },
    { -882.0114f, -155.2503f, 62.3239f, nullptr },
    { -875.8168f, -153.9659f, 63.1851f, nullptr },
    { -870.3460f, -152.9317f, 63.9738f, nullptr },
    { -865.1218f, -150.6951f, 62.6446f, nullptr },
    { -860.4359f, -148.9337f, 62.3263f, nullptr },
    { -854.1095f, -148.8653f, 62.5218f, nullptr },
    { -847.1732f, -149.7830f, 63.4186f, nullptr },
    { -841.7355f, -149.3583f, 63.6466f, nullptr },
    { -836.1990f, -151.4686f, 63.1030f, nullptr },
    { -829.7960f, -150.7334f, 62.2363f, nullptr },
    { -826.5248f, -146.2442f, 62.7029f, nullptr },
    { -829.7138f, -140.0741f, 62.6917f, nullptr },
    { -833.9691f, -136.5332f, 61.9869f, nullptr },
    { -840.1450f, -140.6282f, 62.7205f, nullptr },
    { -847.5126f, -141.8634f, 61.9770f, nullptr },
    { -853.3090f, -141.8069f, 61.4654f, nullptr },
    { -858.7864f, -142.5569f, 61.6271f, nullptr },
    { -863.9399f, -143.0960f, 61.6453f, nullptr },
    { -866.6136f, -138.1031f, 61.4067f, nullptr },
    { -867.9744f, -133.2425f, 62.1494f, nullptr },
    { -870.1982f, -128.0217f, 63.6288f, nullptr },
    { -870.0323f, -122.6509f, 64.3347f, nullptr },
    { -867.6614f, -118.0945f, 64.5997f, nullptr },
    { -863.3998f, -111.6357f, 64.3327f, nullptr },
    { -858.9880f, -106.1409f, 64.6680f, nullptr },
    { -854.8686f, -100.4918f, 66.4264f, nullptr },
    { -851.2466f, -95.6850f, 68.5368f, nullptr },
};

// WSG and AB paths defined in BattleBotWaypoints2.cpp; extern declared in BattleBotWaypoints.h.

// Forward declarations for AB paths used by vPaths_NoReverseAllowed.
extern BattleBotPath vPath_AB_AllianceBase_to_Stables;
extern BattleBotPath vPath_AB_AllianceBase_to_GoldMine;
extern BattleBotPath vPath_AB_AllianceBase_to_LumberMill;
extern BattleBotPath vPath_AB_HordeBase_to_Farm;
extern BattleBotPath vPath_AB_HordeBase_to_GoldMine;
extern BattleBotPath vPath_AB_HordeBase_to_LumberMill;

std::vector<BattleBotPath*> const vPaths_AV =
{
    &vPath_AV_Horde_Cave_to_Tower_Point_Crossroad,
    &vPath_AV_Tower_Point_Crossroads_to_Tower_Point_Bottom,
    &vPath_AV_TowerPoint_Bottom_to_Tower_Point_Flag,
    &vPath_AV_Tower_Point_Bottom_to_Frostwolf_Graveyard_Flag,
    &vPath_AV_Frostwolf_Graveyard_to_Frostwolf_Graveyard_Flag,
    &vPath_AV_Tower_Point_Crossroads_to_Iceblood_Graveyard_Flag,
    &vPath_AV_Iceblood_Graveyard_Flag_to_Iceblood_Tower_Crossroad,
    &vPath_AV_Iceblood_Tower_to_Iceblood_Garrison,
    &vPath_AV_Iceblood_Garrison_to_Captain_Galvangar,
    &vPath_AV_Iceblood_Graveyard_to_Iceblood_Graveyard_Flag,
    &vPath_AV_Iceblood_Tower_Crossroad_to_Field_of_Strife_Stoneheart_Snowfall_Crossroad,
    &vPath_AV_Stonehearth_Outpost_to_Captain_Balinda_Stonehearth,
    &vPath_AV_Stonehearth_Outpost_to_Stonehearth_Graveyard_Crossroad,
    &vPath_AV_Stonehearth_Graveyard_Crossroad_to_Stonehearth_Graveyard_Flag,
    &vPath_AV_Stonehearth_Graveyard_to_Stonehearth_Graveyard_Flag,
    &vPath_AV_Stonehearth_Graveyard_Flag_to_Stonehearth_Graveyard_Second_Crossroad,
    &vPath_AV_Stonehearth_Graveyard_Second_Crossroad_to_Stonehearth_Bunker_First_Crossroad,
    &vPath_AV_Stonehearth_Graveyard_Second_Crossroad_to_Iceblood_Garrison,
    &vPath_AV_Stonehearth_Graveyard_Second_Crossroad_to_Iceblood_Tower_Crossroad,
    &vPath_AV_Stonehearth_Bunker_First_Crossroad_to_Stonehearth_Bunker_Flag,
    &vPath_AV_Stonehearth_Graveyard_Crossroad_to_Icewing_Bunker_Crossroad,
    &vPath_AV_Icewing_Bunker_Crossroad_to_Icewing_Bunker_Flag,
    &vPath_AV_Icewing_Bunker_Crossroad_to_Alliance_Slope_Crossroad,
    &vPath_AV_Alliance_Cave_Slop_Crossroad_to_Alliance_Slope_Crossroad,
    &vPath_AV_Alliance_Cave_to_Alliance_Cave_Slop_Crossroad,
    &vPath_AV_Horde_Cave_to_Frostwolf_Graveyard_Flag,
    &vPath_AV_Frostwolf_Graveyard_Flag_to_Horde_Base_First_Crossroads,
    &vPath_AV_Horde_Base_First_Crossroads_to_Horde_Base_Second_Crossroads,
    &vPath_AV_Horde_Base_First_Crossroads_to_East_Frostwolf_Tower_Flag,
    &vPath_AV_Horde_Base_First_Crossroads_to_West_Frostwolf_Tower_Flag,
    &vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_Entrance_DrekThar,
    &vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_DrekThar1,
    &vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_DrekThar2,
    &vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_Graveyard_Flag,
    &vPath_AV_Iceblood_Garrison_to_Snowfall_Flag,
    &vPath_AV_Snowfall_Flag_to_Field_of_Strife_Stoneheart_Snowfall_Crossroad,
    &vPath_AV_Field_of_Strife_Stoneheart_Snowfall_Crossroad_to_Stonehearth_Outpost,
    &vPath_AV_Alliance_Slope_Crossroad_to_Stormpike_Crossroad,
    &vPath_AV_Stormpike_Crossroad_to_Alliance_Base_Bunker_First_Crossroad,
    &vPath_AV_Stormpike_Crossroad_to_Stormpike_Flag,
    &vPath_AV_Stormpike_Graveyard_to_Stormpike_Flag,
    &vPath_AV_Alliance_Base_Bunker_First_Crossroad_to_Alliance_Base_North_Bunker,
    &vPath_AV_Alliance_Base_Bunker_First_Crossroad_to_Alliance_Base_Bunker_Second_Crossroad,
    &vPath_AV_Alliance_Base_Bunker_Second_Crossroad_to_Alliance_Base_South_Bunker,
    &vPath_AV_Alliance_Base_Bunker_Second_Crossroad_to_Alliance_Base_Bunker_Third_Crossroad,
    &vPath_AV_Alliance_Base_Bunker_Third_Crossroad_to_Alliance_Base_Flag,
    &vPath_AV_Alliance_Base_Bunker_Third_Crossroad_to_Alliance_Base_Vanndar_Stormpike,
    &vPath_AV_Iceblood_Tower_Crossroad_to_Iceblood_Tower,
    &vPath_AV_Iceblood_Tower_to_Iceblood_Tower_Flag,
    &vPath_AV_Stormpike_to_Irondeep_Morloch,
    &vPath_AV_TowerPoint_to_Coldtooth_Snivvle,
};

std::vector<BattleBotPath*> const vPaths_NoReverseAllowed =
{
    &vPath_AB_AllianceBase_to_Stables,
    &vPath_AB_AllianceBase_to_GoldMine,
    &vPath_AB_AllianceBase_to_LumberMill,
    &vPath_AB_HordeBase_to_Farm,
    &vPath_AB_HordeBase_to_GoldMine,
    &vPath_AB_HordeBase_to_LumberMill,
    &vPath_AV_Horde_Cave_to_Tower_Point_Crossroad,
    &vPath_AV_Frostwolf_Graveyard_to_Frostwolf_Graveyard_Flag,
    &vPath_AV_Iceblood_Graveyard_to_Iceblood_Graveyard_Flag,
    &vPath_AV_Stonehearth_Graveyard_to_Stonehearth_Graveyard_Flag,
    &vPath_AV_Alliance_Cave_to_Alliance_Cave_Slop_Crossroad,
    &vPath_AV_Horde_Cave_to_Frostwolf_Graveyard_Flag,
    &vPath_AV_Alliance_Cave_Slop_Crossroad_to_Alliance_Slope_Crossroad,
    &vPath_AV_Stormpike_Graveyard_to_Stormpike_Flag,
    &vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_DrekThar1,
    &vPath_AV_Horde_Base_Second_Crossroads_to_Horde_Base_DrekThar2,
    &vPath_AV_Alliance_Base_Bunker_Third_Crossroad_to_Alliance_Base_Vanndar_Stormpike,
    &vPath_AV_Stormpike_to_Irondeep_Morloch,
    &vPath_AV_TowerPoint_to_Coldtooth_Snivvle,
};

// Paths excluded from StartNewPathToPosition objective routing.
// StartNewPathFromBeginning / StartNewPathFromAnywhere still use them.
//
// Cave-exit paths: their WP 0 carries an AtCaveExit callback. From inside
// the cave they are the only paths within the 50-yard search radius, so
// without this exclusion they get selected as proxy routes for unrelated
// objectives, trapping bots in a WP-0 loop.
//
// GY_Flag_to_GY_Second_Crossroad: this southward path (GY Flag → Second
// Crossroad) causes a navigation loop when bots try to reach Balinda or
// other northern objectives from the Stonehearth GY/Bunker area. The
// algorithm picks it because Second Crossroad is geometrically closer to
// Balinda (~129 yd) than GY Crossroad (~188 yd), but the correct route
// to Balinda goes north through GY Crossroad → Outpost. Excluding it
// forces bots to take the northward leg instead. StartNewPathFromBeginning
// can still use it as a fallback for bots that need to exit toward the
// bunker crossroad.
std::vector<BattleBotPath*> const vPaths_ObjectiveExcluded =
{
    &vPath_AV_Alliance_Cave_to_Alliance_Cave_Slop_Crossroad,
    &vPath_AV_Horde_Cave_to_Tower_Point_Crossroad,
    &vPath_AV_Horde_Cave_to_Frostwolf_Graveyard_Flag,
    &vPath_AV_Stonehearth_Graveyard_Flag_to_Stonehearth_Graveyard_Second_Crossroad,
};

// Mine paths: only selected by StartNewPathFromBeginning/Anywhere when bot is a mine bot.
// Their start is near the Alliance base (638, -287), so without this guard non-mine bots
// exiting the Alliance cave would be routed into the mine.
std::vector<BattleBotPath*> const vPaths_MineExclusive =
{
    &vPath_AV_Stormpike_to_Irondeep_Morloch,
    &vPath_AV_TowerPoint_to_Coldtooth_Snivvle,
};

std::vector<BattleBotPath*> const vPaths_ObjectiveOnly =
{
    &vPath_AV_Stonehearth_Bunker_First_Crossroad_to_Stonehearth_Bunker_Flag,
    &vPath_AV_Icewing_Bunker_Crossroad_to_Icewing_Bunker_Flag,
    &vPath_AV_Stormpike_Crossroad_to_Alliance_Base_Bunker_First_Crossroad,
    &vPath_AV_Alliance_Base_Bunker_First_Crossroad_to_Alliance_Base_North_Bunker,
    &vPath_AV_Alliance_Base_Bunker_First_Crossroad_to_Alliance_Base_Bunker_Second_Crossroad,
    &vPath_AV_Alliance_Base_Bunker_Second_Crossroad_to_Alliance_Base_South_Bunker,
    &vPath_AV_Alliance_Base_Bunker_Second_Crossroad_to_Alliance_Base_Bunker_Third_Crossroad,
    &vPath_AV_Alliance_Base_Bunker_Third_Crossroad_to_Alliance_Base_Flag,
    &vPath_AV_Alliance_Base_Bunker_Third_Crossroad_to_Alliance_Base_Vanndar_Stormpike,
    &vPath_AV_TowerPoint_Bottom_to_Tower_Point_Flag,
    &vPath_AV_Horde_Base_First_Crossroads_to_East_Frostwolf_Tower_Flag,
    &vPath_AV_Horde_Base_First_Crossroads_to_West_Frostwolf_Tower_Flag,
    &vPath_AV_Iceblood_Tower_to_Iceblood_Tower_Flag,
};

void BattleBotAI::MovementInform(uint32 movementType, uint32 data)
{
    if (movementType == POINT_MOTION_TYPE)
    {
        bool const debugLog = sWorld.getConfig(CONFIG_BOOL_BATTLEGROUND_MOVEMENT_DEBUG) &&
            me->GetBattleGround() && me->GetBattleGround()->GetTypeID() == BATTLEGROUND_WS;

        // Guard against stale callbacks: a queued PointMovementGenerator can fire its
        // Finalize() after the path has been swapped (e.g. MotionMaster::Clear() from
        // class AI). The `data` index refers to the *previous* path, so it may be out
        // of range for the current one. Without this check, vector::at() throws and
        // aborts the world thread.
        if (m_currentPath && data < m_currentPath->size())
        {
            if (debugLog)
            {
                sLog.Out(LOG_BG, LOG_LVL_BASIC,
                         "[BattleGroundMount] arrived bot %s guid %u bg %u point %u/%u hasFunc %u mounted %u pos %.1f %.1f %.1f.",
                         me->GetName(), me->GetGUIDLow(), me->GetBattleGroundId(),
                         data, uint32(m_currentPath->size() - 1),
                         (*m_currentPath)[data].pFunc ? 1u : 0u, me->IsMounted() ? 1u : 0u,
                         me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
            }

            if ((*m_currentPath)[data].pFunc)
                (*(*m_currentPath)[data].pFunc)(this);
            else
                MoveToNextPoint();
        }
        else
        {
            if (debugLog)
            {
                sLog.Out(LOG_BG, LOG_LVL_BASIC,
                         "[BattleGroundMount] arrived-stale bot %s guid %u bg %u point %u hasPath %u pos %.1f %.1f %.1f.",
                         me->GetName(), me->GetGUIDLow(), me->GetBattleGroundId(), data,
                         m_currentPath ? 1u : 0u,
                         me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
            }
            MoveToNextPoint();
        }

        ActivateNearbyAreaTrigger();
    }
}

void BattleBotAI::MoveToNextPoint()
{
    if (!m_currentPath || m_currentPath->empty())
    {
        ClearPath();
        return;
    }

    uint32 const lastPointInPath = m_movingInReverse ? 0 : ((*m_currentPath).size() - 1);

    if ((m_currentPoint == lastPointInPath) ||
        (me->IsInCombat() && !ShouldIgnoreCombat()) || !me->IsAlive())
    {
        // Path is over.
        ClearPath();
        return;
    }

    if (m_movingInReverse)
        m_currentPoint--;
    else
        m_currentPoint++;

    // Defensive: m_currentPoint can be stale (e.g. set to closestPoint-1 with
    // closestPoint==0 underflows to UINT32_MAX before wraparound on ++). Drop the
    // path instead of throwing out_of_range from at() and aborting the world thread.
    if (m_currentPoint >= m_currentPath->size())
    {
        ClearPath();
        return;
    }

    BattleBotWaypoint& nextPoint = (*m_currentPath)[m_currentPoint];

    // Trail of every leg a bot travels while unmounted, to tell "never got a chance
    // to mount on this whole trip" apart from "briefly unmounted near a dead zone".
    if (!me->IsMounted() && me->GetBattleGround() && me->GetBattleGround()->GetTypeID() == BATTLEGROUND_WS &&
        sWorld.getConfig(CONFIG_BOOL_BATTLEGROUND_MOVEMENT_DEBUG))
    {
        sLog.Out(LOG_BG, LOG_LVL_BASIC,
                 "[BattleGroundMount] hop-unmounted bot %s guid %u bg %u point %u/%u from %.1f %.1f %.1f to %.1f %.1f %.1f.",
                 me->GetName(), me->GetGUIDLow(), me->GetBattleGroundId(),
                 m_currentPoint, uint32(m_currentPath->size() - 1),
                 me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                 nextPoint.x, nextPoint.y, nextPoint.z);
    }

    me->GetMotionMaster()->MovePoint(m_currentPoint, nextPoint.x + frand(-1, 1), nextPoint.y + frand(-1, 1), nextPoint.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE);
}

bool BattleBotAI::StartNewPathFromBeginning()
{
    struct AvailablePath
    {
        AvailablePath(BattleBotPath* pPath_, bool reverse_) : pPath(pPath_), reverse(reverse_) {}
        BattleBotPath* pPath = nullptr;
        bool reverse = false;
    };
    std::vector<AvailablePath> availablePaths;

    std::vector<BattleBotPath*> const* vPaths;
    switch (me->GetBattleGround()->GetTypeID())
    {
        case BATTLEGROUND_AB:
        case BATTLEGROUND_BR:   // BR uses AB map, reuse the same recorded paths
        {
            vPaths = &vPaths_AB;
            break;
        }
        case BATTLEGROUND_AV:
        {
            vPaths = &vPaths_AV;
            break;
        }
        case BATTLEGROUND_WS:
        {
            vPaths = &vPaths_WS;
            break;
        }
        default:
            return false;
    }

    for (const auto& pPath : *vPaths)
    {
        // Mine paths are reserved for designated mine bots.
        if (!m_avIsMineBot && std::find(vPaths_MineExclusive.begin(), vPaths_MineExclusive.end(), pPath) != vPaths_MineExclusive.end())
            continue;

        BattleBotWaypoint* pStart = &((*pPath)[0]);
        if (me->GetDistance(pStart->x, pStart->y, pStart->z) < INTERACTION_DISTANCE)
            availablePaths.emplace_back(AvailablePath(pPath, false));

        // Some paths are not allowed backwards.
        if (std::find(vPaths_NoReverseAllowed.begin(), vPaths_NoReverseAllowed.end(), pPath) != vPaths_NoReverseAllowed.end())
            continue;

        BattleBotWaypoint* pEnd = &((*pPath)[(*pPath).size() - 1]);
        if (me->GetDistance(pEnd->x, pEnd->y, pEnd->z) < INTERACTION_DISTANCE)
            availablePaths.emplace_back(AvailablePath(pPath, true));
    }

    if (availablePaths.empty())
        return false;

    AvailablePath const* chosenPath = &SelectRandomContainerElement(availablePaths);
    m_currentPath = chosenPath->pPath;
    m_movingInReverse = chosenPath->reverse;
    m_currentPoint = m_movingInReverse ? m_currentPath->size() - 1 : 0;
    MoveToNextPoint();
    return true;
}

void BattleBotAI::StartNewPathFromAnywhere()
{
    BattleBotPath* pClosestPath = nullptr;
    uint32 closestPoint = 0;
    float closestDistance = FLT_MAX;

    std::vector<BattleBotPath*> const* vPaths;
    switch (me->GetBattleGround()->GetTypeID())
    {
        case BATTLEGROUND_AB:
        case BATTLEGROUND_BR:   // BR uses AB map, reuse the same recorded paths
        {
            vPaths = &vPaths_AB;
            break;
        }
        case BATTLEGROUND_AV:
        {
            vPaths = &vPaths_AV;
            break;
        }
        case BATTLEGROUND_WS:
        {
            vPaths = &vPaths_WS;
            break;
        }
        default:
            return;
    }

    for (const auto& pPath : *vPaths)
    {
        // Mine paths are reserved for designated mine bots.
        if (!m_avIsMineBot && std::find(vPaths_MineExclusive.begin(), vPaths_MineExclusive.end(), pPath) != vPaths_MineExclusive.end())
            continue;

        for (uint32 i = 0; i < pPath->size(); i++)
        {
            BattleBotWaypoint& waypoint = ((*pPath)[i]);
            float const distanceToPoint = me->GetDistance(waypoint.x, waypoint.y, waypoint.z);
            if (distanceToPoint < closestDistance)
            {
                pClosestPath = pPath;
                closestPoint = i;
                closestDistance = distanceToPoint;
            }
        }
    }

    if (!pClosestPath)
        return;

    m_currentPath = pClosestPath;
    m_movingInReverse = false;
    m_currentPoint = (closestPoint > 0) ? closestPoint - 1 : static_cast<uint32>(-1);
    MoveToNextPoint();
}

bool BattleBotAI::StartNewPathToPosition(Position const& targetPosition, std::vector<BattleBotPath*> const& vPaths)
{
    BattleBotPath* pClosestPath = nullptr;
    uint32 closestPoint = 0;
    float closestDistanceToTarget = FLT_MAX;
    bool reverse = false;

    for (const auto& pPath : vPaths)
    {
        // Cave-exit paths must not be selected as proxy routes for objectives.
        if (std::find(vPaths_ObjectiveExcluded.begin(), vPaths_ObjectiveExcluded.end(), pPath) != vPaths_ObjectiveExcluded.end())
            continue;

        {
            BattleBotWaypoint& lastPoint = ((*pPath)[pPath->size() - 1]);
            float const distanceFromPathEndToTarget = GetDistance3D(lastPoint, targetPosition);
            if (closestDistanceToTarget > distanceFromPathEndToTarget)
            {
                float closestDistanceFromMeToPoint = FLT_MAX;

                for (uint32 i = 0; i < pPath->size(); i++)
                {
                    BattleBotWaypoint& waypoint = ((*pPath)[i]);
                    float const distanceFromMeToPoint = me->GetDistance(waypoint.x, waypoint.y, waypoint.z);
                    if (distanceFromMeToPoint < 50.0f && closestDistanceFromMeToPoint > distanceFromMeToPoint)
                    {
                        reverse = false;
                        pClosestPath = pPath;
                        closestPoint = i;
                        closestDistanceToTarget = distanceFromPathEndToTarget;
                        closestDistanceFromMeToPoint = distanceFromMeToPoint;
                    }
                }
            }
        }
        
        if (std::find(vPaths_NoReverseAllowed.begin(), vPaths_NoReverseAllowed.end(), pPath) != vPaths_NoReverseAllowed.end())
            continue;

        {
            BattleBotWaypoint& firstPoint = ((*pPath)[0]);
            float const distanceFromPathBeginToTarget = GetDistance3D(firstPoint, targetPosition);
            if (closestDistanceToTarget > distanceFromPathBeginToTarget)
            {
                float closestDistanceFromMeToPoint = FLT_MAX;

                for (uint32 i = 0; i < pPath->size(); i++)
                {
                    BattleBotWaypoint& waypoint = ((*pPath)[i]);
                    float const distanceFromMeToPoint = me->GetDistance(waypoint.x, waypoint.y, waypoint.z);
                    if (distanceFromMeToPoint < 50.0f && closestDistanceFromMeToPoint > distanceFromMeToPoint)
                    {
                        reverse = true;
                        pClosestPath = pPath;
                        closestPoint = i;
                        closestDistanceToTarget = distanceFromPathBeginToTarget;
                        closestDistanceFromMeToPoint = distanceFromMeToPoint;
                    }
                }
            }
        }
    }

    if (!pClosestPath)
        return false;

    // Prevent picking last point of path.
    // It means we are already there.
    if (reverse)
    {
        if (closestPoint == 0)
            return false;
            
    }
    else
    {
        if (closestPoint == pClosestPath->size() - 1)
            return false;
    }

    m_currentPath = pClosestPath;
    m_movingInReverse = reverse;
    if (m_movingInReverse)
        m_currentPoint = closestPoint + 1;
    else
        m_currentPoint = (closestPoint > 0) ? closestPoint - 1 : static_cast<uint32>(-1);
    MoveToNextPoint();
    return true;
}

static uint32 AV_KeyDefenseObjectives[] =
{
    BG_AV_STORMPIKE_AID_STATION_GY,
    BG_AV_STORMPIKE_GY,
    BG_AV_STONEHEARTH_GY,
    BG_AV_SNOWFALL_GY,
    BG_AV_ICEBLOOD_GY,
    BG_AV_FROSTWOLF_GY,
    BG_AV_FROSTWOLF_RELIEF_HUT_GY,
};

static uint32 GetAVControlledStateForTeam(Team team)
{
    return team == HORDE ? HORDE_CONTROLLED : ALLIANCE_CONTROLLED;
}

static uint32 GetAVAssaultedStateForTeam(Team team)
{
    return team == HORDE ? HORDE_ASSAULTED : ALLIANCE_ASSAULTED;
}

static uint32 GetAVEnemyAssaultedStateForTeam(Team team)
{
    return team == HORDE ? ALLIANCE_ASSAULTED : HORDE_ASSAULTED;
}

static bool GetAVNativeGraveyardFallbackPosition(uint32 node, Position& outPosition)
{
    switch (node)
    {
        case BG_AV_STORMPIKE_AID_STATION_GY:
            outPosition.x = 640.404f; outPosition.y = -32.0183f; outPosition.z = 46.2328f; outPosition.o = 0.0f;
            return true;
        case BG_AV_STORMPIKE_GY:
            outPosition.x = 667.173f; outPosition.y = -295.225f; outPosition.z = 30.29f; outPosition.o = 0.0f;
            return true;
        case BG_AV_STONEHEARTH_GY:
            outPosition.x = 79.8805f; outPosition.y = -401.379f; outPosition.z = 46.516f; outPosition.o = 0.0f;
            return true;
        case BG_AV_ICEBLOOD_GY:
            outPosition.x = -614.138f; outPosition.y = -396.501f; outPosition.z = 60.8585f; outPosition.o = 0.0f;
            return true;
        case BG_AV_FROSTWOLF_GY:
            outPosition.x = -1079.61f; outPosition.y = -345.548f; outPosition.z = 55.1131f; outPosition.o = 0.0f;
            return true;
        case BG_AV_FROSTWOLF_RELIEF_HUT_GY:
            outPosition.x = -1401.94f; outPosition.y = -310.103f; outPosition.z = 89.3816f; outPosition.o = 0.0f;
            return true;
        default:
            return false;
    }
}

bool BattleBotIsInAVGyCaptureHold(BattleBotAI const* pAI)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return false;

    uint32 const capturingState = GetAVAssaultedStateForTeam(pAI->me->GetTeam());
    for (uint32 const objective : AV_KeyDefenseObjectives)
    {
        if (objective == BG_AV_SNOWFALL_GY)
            continue;
        if (!bg->IsActiveEvent(objective, capturingState))
            continue;
        Position gyPos;
        if (GameObject* pGO = pAI->me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(objective, capturingState)))
            gyPos = pGO->GetPosition();
        else if (!GetAVNativeGraveyardFallbackPosition(objective, gyPos))
            continue;
        if (pAI->me->IsWithinDist3d(gyPos.x, gyPos.y, gyPos.z, AV_RESCUE_RADIUS))
            return true;
    }
    return false;
}

bool BattleBotIsNearAVFlag(BattleBotAI const* pAI, float radius)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return false;

    for (uint32 const bannerId : vFlagsAV)
        if (pAI->me->FindNearestGameObject(bannerId, radius))
            return true;

    return false;
}

bool BattleBotIsNearAVCaptain(BattleBotAI const* pAI, float radius)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return false;

    // Enemy captain: Balinda for Horde attackers, Galvangar for Alliance attackers.
    uint32 const captainType = (pAI->me->GetTeam() == HORDE) ? BG_AV_CAPTAIN_A : BG_AV_CAPTAIN_H;
    if (Creature* pCaptain = pAI->me->GetMap()->GetCreature(bg->GetSingleCreatureGuid(captainType, 0)))
        if (pCaptain->IsAlive() && pAI->me->IsWithinDist(pCaptain, radius))
            return true;

    return false;
}

Unit* BattleBotSelectAVGeneralTarget(BattleBotAI const* pAI, Unit* pExcept, float radius)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return nullptr;

    uint32 const generalType = (pAI->me->GetTeam() == HORDE) ? BG_AV_BOSS_A : BG_AV_BOSS_H;
    Creature* pGeneral = pAI->me->GetMap()->GetCreature(bg->GetSingleCreatureGuid(generalType, 0));
    if (!pGeneral || pGeneral == pExcept || !pGeneral->IsAlive() ||
        !pAI->IsValidHostileTarget(pGeneral) ||
        !pAI->me->IsWithinDist(pGeneral, radius) ||
        !pAI->me->IsWithinLOSInMap(pGeneral))
        return nullptr;

    return pGeneral;
}

bool BattleBotIsNearAVGeneral(BattleBotAI const* pAI, float radius)
{
    return BattleBotSelectAVGeneralTarget(pAI, nullptr, radius) != nullptr;
}

bool BattleBotIsNearOpenObjectiveFlag(BattleBotAI const* pAI, float radius)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    std::vector<uint32> const* flags = nullptr;
    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_AB:
            flags = &vFlagsAB;
            break;
        case BATTLEGROUND_AV:
            flags = &vFlagsAV;
            break;
        default:
            return false;
    }

    for (uint32 const bannerId : *flags)
        if (GameObject* pGo = pAI->me->FindNearestGameObject(bannerId, radius))
            if (IsABFlagOpenable(pAI, pGo))
                return true;

    return false;
}

template<std::size_t N>
static GameObject* FindNearbyAVKeyDefenseObject(BattleBotAI const* pAI, uint32 const (&objectives)[N], float radius)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    Map* map = pAI->me->GetMap();
    if (!bg || !map || bg->GetTypeID() != BATTLEGROUND_AV)
        return nullptr;

    GameObject* bestObject = nullptr;
    float bestDistance = FLT_MAX;
    uint32 const controlledState = GetAVControlledStateForTeam(pAI->me->GetTeam());
    uint32 const assaultedState = GetAVAssaultedStateForTeam(pAI->me->GetTeam());
    uint32 const enemyAssaultedState = GetAVEnemyAssaultedStateForTeam(pAI->me->GetTeam());

    for (uint32 const objective : objectives)
    {
        uint32 activeState = 0;
        if (bg->IsActiveEvent(objective, assaultedState))
            activeState = assaultedState;
        else if (bg->IsActiveEvent(objective, enemyAssaultedState))
            activeState = enemyAssaultedState;
        else if (bg->IsActiveEvent(objective, controlledState))
            activeState = controlledState;
        else
            continue;

        GameObject* pGO = map->GetGameObject(bg->GetSingleGameObjectGuid(objective, activeState));
        if (!pGO)
            continue;

        float const distance = pAI->me->GetDistance(pGO);
        if (distance <= radius && distance < bestDistance)
        {
            bestObject = pGO;
            bestDistance = distance;
        }
    }

    return bestObject;
}

static GameObject* FindNearbyAVKeyDefenseObject(BattleBotAI const* pAI, float radius)
{
    return FindNearbyAVKeyDefenseObject(pAI, AV_KeyDefenseObjectives, radius);
}

Unit* BattleBotSelectAVFlagDefenseTarget(BattleBotAI const* pAI, Unit* pExcept)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return nullptr;

    CombatBotRoles const role = pAI->GetRole();
    if (role != ROLE_MELEE_DPS && role != ROLE_RANGE_DPS && role != ROLE_TANK)
        return nullptr;

    GameObject* pDefenseObject = FindNearbyAVKeyDefenseObject(pAI, AV_FLAG_DEFENSE_RADIUS);
    if (!pDefenseObject)
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

        if (player->GetDistance(pDefenseObject) > AV_FLAG_DEFENSE_RADIUS)
            continue;

        if (pAI->me->GetDistanceZ(player) > 10.0f)
            continue;

        if (!pAI->me->IsWithinLOSInMap(player))
            continue;

        float const distanceToFlag = player->GetDistance(pDefenseObject);
        if (!bestTarget || distanceToFlag < bestDistanceToFlag)
        {
            bestTarget = player;
            bestDistanceToFlag = distanceToFlag;
        }
    }

    return bestTarget;
}

// -----------------------------------------------------------------------
// AV graveyard guard system
// -----------------------------------------------------------------------

static uint32 const AV_AllianceNativeGYs[] =
{
    BG_AV_STORMPIKE_AID_STATION_GY,
    BG_AV_STORMPIKE_GY,
    BG_AV_STONEHEARTH_GY,
};

static uint32 const AV_HordeNativeGYs[] =
{
    BG_AV_FROSTWOLF_RELIEF_HUT_GY,
    BG_AV_FROSTWOLF_GY,
    BG_AV_ICEBLOOD_GY,
};

static bool IsAVNativeGY(Team team, uint32 node)
{
    uint32 const* arr = (team == HORDE) ? AV_HordeNativeGYs : AV_AllianceNativeGYs;
    for (uint32 i = 0; i < 3; ++i)
        if (arr[i] == node)
            return true;
    return false;
}

static uint8 CountAVBotsAssignedToGY(Map* map, Team team, uint32 node)
{
    uint8 count = 0;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            if (player->GetTeam() != team || !player->IsBot() || !player->IsAlive())
                continue;
            if (BattleBotAI* pBotAI = dynamic_cast<BattleBotAI*>(player->AI()))
                if (pBotAI->m_avAssignedGY == node)
                    ++count;
        }
    }
    return count;
}

static bool IsAVExcessGuardForGY(BattleBotAI const* pAI, uint32 node, uint8 keepCount)
{
    Map* map = pAI->me->GetMap();
    uint8 lowerGuidGuards = 0;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            if (player == pAI->me || player->GetTeam() != pAI->me->GetTeam() ||
                !player->IsBot() || !player->IsAlive())
                continue;
            if (BattleBotAI* pBotAI = dynamic_cast<BattleBotAI*>(player->AI()))
                if (pBotAI->m_avAssignedGY == node &&
                    player->GetObjectGuid().GetCounter() < pAI->me->GetObjectGuid().GetCounter())
                    ++lowerGuidGuards;
        }
    }
    return lowerGuidGuards >= keepCount;
}

static bool GetAVGYPosition(BattleGround* bg, Map* map, Team team, uint32 node, Position& outPos)
{
    uint32 const states[] = {
        GetAVControlledStateForTeam(team),
        GetAVAssaultedStateForTeam(team),
        GetAVEnemyAssaultedStateForTeam(team),
    };
    for (uint32 state : states)
    {
        if (!bg->IsActiveEvent(node, state))
            continue;
        if (GameObject* pGO = map->GetGameObject(bg->GetSingleGameObjectGuid(node, state)))
        {
            outPos = pGO->GetPosition();
            return true;
        }
    }
    return GetAVNativeGraveyardFallbackPosition(node, outPos);
}

static bool FindAVGYToGuard(BattleBotAI* pAI, uint32& outNode)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    Map* map = pAI->me->GetMap();
    if (!bg || !map || bg->GetTypeID() != BATTLEGROUND_AV)
        return false;

    Team const team = pAI->me->GetTeam();
    uint32 const ownControlled = GetAVControlledStateForTeam(team);
    uint32 const ownAssaulted  = GetAVAssaultedStateForTeam(team);
    uint32 const* enemyNative  = (team == HORDE) ? AV_AllianceNativeGYs : AV_HordeNativeGYs;
    uint32 const* ownNative    = (team == HORDE) ? AV_HordeNativeGYs    : AV_AllianceNativeGYs;

    // Priority 1: We are actively capturing an enemy GY — up to 5 bots converge as guards.
    // Capped to prevent all bots being permanently assigned here while the GY is assaulting;
    // holdCaptureUntilControlled handles the "everyone waits" behaviour for the rest.
    for (uint32 i = 0; i < 3; ++i)
    {
        if (bg->IsActiveEvent(enemyNative[i], ownAssaulted) &&
            CountAVBotsAssignedToGY(map, team, enemyNative[i]) < 5)
        {
            outNode = enemyNative[i];
            return true;
        }
    }

    // Priority 2: We fully control a captured GY — keep 3 guards
    for (uint32 i = 0; i < 3; ++i)
    {
        if (bg->IsActiveEvent(enemyNative[i], ownControlled) &&
            CountAVBotsAssignedToGY(map, team, enemyNative[i]) < 3)
        {
            outNode = enemyNative[i];
            return true;
        }
    }

    // Priority 3: Own native GYs after 5 minutes — 2 guards each, GUID-spread
    if (bg->GetStartTime() < 5 * 60 * 1000u)
        return false;

    uint32 const guidBase = pAI->me->GetObjectGuid().GetCounter();
    for (uint32 attempt = 0; attempt < 3; ++attempt)
    {
        uint32 const node = ownNative[(guidBase + attempt) % 3];
        if (bg->IsActiveEvent(node, ownControlled) &&
            CountAVBotsAssignedToGY(map, team, node) < 2)
        {
            outNode = node;
            return true;
        }
    }
    return false;
}

// Stable GUID-based selection: returns true if this bot should be a mine bot for its team.
// Called once per bot after the 5-minute delay. The lowest m_avMineMissionCount GUIDs among
// eligible DPS bots (non-healer, no fixed GY guard, not in temp GY hold) become mine bots.
static bool ShouldBeAVMineBot(BattleBotAI const* pAI)
{
    uint32 const MINE_MISSION_COUNT = pAI->m_avMineMissionCount;
    if (MINE_MISSION_COUNT == 0)
        return false;
    Team const team = pAI->me->GetTeam();
    Map* map = pAI->me->GetMap();
    if (!map)
        return false;

    // Count already-assigned mine bots and exclude them from the eligible pool.
    // Without this, a late-deciding bot (e.g. one that was in GY capture hold or
    // in sustained combat when the 3-minute window opened) sees the full pool and
    // can land in the lowest-N set even after N slots are already filled.
    uint32 alreadyAssigned = 0;
    std::vector<uint32> eligibleGuids;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player || player->GetTeam() != team || !player->IsBot())
            continue;
        BattleBotAI const* pBotAI = dynamic_cast<BattleBotAI const*>(player->AI());
        if (!pBotAI)
            continue;
        if (pBotAI->m_avIsMineBot)
        {
            ++alreadyAssigned;
            continue;
        }
        if (CombatBotBaseAI::IsHealerClass(player->GetClass()))
            continue;
        if (pBotAI->m_avAssignedGY != 0)
            continue;
        if (BattleBotIsInAVGyCaptureHold(pBotAI))
            continue;
        eligibleGuids.push_back(player->GetGUIDLow());
    }

    if (alreadyAssigned >= MINE_MISSION_COUNT)
        return false;

    uint32 const remainingSlots = MINE_MISSION_COUNT - alreadyAssigned;
    std::sort(eligibleGuids.begin(), eligibleGuids.end());

    uint32 const myGuid = pAI->me->GetGUIDLow();
    uint32 count = 0;
    for (uint32 guid : eligibleGuids)
    {
        if (count >= remainingSlots)
            break;
        if (guid == myGuid)
            return true;
        ++count;
    }
    return false;
}

static bool BattleBotSelectAVGuardObjective(BattleBotAI* pAI)
{
    BattleGround* bg = pAI->me->GetBattleGround();
    Map* map = pAI->me->GetMap();
    if (!bg || !map)
        return false;

    Team const team = pAI->me->GetTeam();
    uint32 const ownControlled = GetAVControlledStateForTeam(team);
    uint32 const ownAssaulted  = GetAVAssaultedStateForTeam(team);
    uint32 const node          = pAI->m_avAssignedGY;
    bool const isNative        = IsAVNativeGY(team, node);

    bool const weControl    = bg->IsActiveEvent(node, ownControlled);
    bool const weCapturing  = bg->IsActiveEvent(node, ownAssaulted);

    if (weControl)
    {
        // Behavior 1: bots that decided to advance after capture leave once GY is fully ours
        if (!isNative && !pAI->m_avStayGuardAfterCapture)
        {
            pAI->m_avAssignedGY = 0;
            return false;
        }
        // Release excess guards once GY is fully controlled
        uint8 const keepCount = isNative ? 2 : 3;
        if (IsAVExcessGuardForGY(pAI, node, keepCount))
        {
            pAI->m_avAssignedGY = 0;
            return false;
        }
    }
    else if (!weCapturing)
    {
        // GY no longer ours
        if (!isNative)
        {
            // Captured enemy GY lost — release and return to attack
            pAI->m_avAssignedGY = 0;
            return false;
        }
        // Native GY lost — keep assignment and go recapture
    }

    Position gyPos;
    if (!GetAVGYPosition(bg, map, team, node, gyPos))
    {
        pAI->m_avAssignedGY = 0;
        return false;
    }

    if (pAI->me->GetDistance(gyPos) > 25.0f)
        return pAI->StartNewPathToPosition(gyPos, vPaths_AV);

    // At the GY — attempt to capture flag if it's capturable
    AtFlag(pAI, vFlagsAV);
    return true;
}

void BattleBotUpdateAVGuardBehavior(BattleBotAI* pAI)
{
    if (pAI->m_avAssignedGY == 0 || !pAI->me->IsInCombat() || !pAI->me->GetVictim())
        return;

    BattleGround* bg = pAI->me->GetBattleGround();
    Map* map = pAI->me->GetMap();
    if (!bg || !map)
        return;

    Position gyPos;
    if (!GetAVGYPosition(bg, map, pAI->me->GetTeam(), pAI->m_avAssignedGY, gyPos))
        return;

    // Break off combat if enemy moves too far from the guarded GY
    if (pAI->me->GetVictim()->GetDistance(gyPos) > AV_FLAG_DEFENSE_RADIUS + 20.0f)
    {
        pAI->me->AttackStop();
        pAI->ClearPath();
        pAI->me->GetMotionMaster()->MovePoint(0, gyPos.x, gyPos.y, gyPos.z,
            MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE);
    }
}

// Behavior 3: after respawning at a non-home GY, randomly become its guard (max 2).
void TryAssignAVRespawnGuard(BattleBotAI* pAI)
{
    if (pAI->m_avAssignedGY != 0)
        return;

    BattleGround* bg = pAI->me->GetBattleGround();
    Map* map = pAI->me->GetMap();
    if (!bg || !map || bg->GetTypeID() != BATTLEGROUND_AV)
        return;

    Team const team = pAI->me->GetTeam();
    uint32 const homeGY = (team == HORDE) ? BG_AV_FROSTWOLF_RELIEF_HUT_GY : BG_AV_STORMPIKE_AID_STATION_GY;
    uint32 const ownControlled = GetAVControlledStateForTeam(team);

    uint32 nearestNode = 0;
    float nearestDist = FLT_MAX;

    for (uint32 const node : AV_KeyDefenseObjectives)
    {
        if (node == homeGY)
            continue;
        if (!bg->IsActiveEvent(node, ownControlled))
            continue;
        Position gyPos;
        if (!GetAVGYPosition(bg, map, team, node, gyPos))
            continue;
        float const dist = pAI->me->GetDistance(gyPos);
        if (dist < nearestDist)
        {
            nearestDist = dist;
            nearestNode = node;
        }
    }

    if (nearestNode == 0 || nearestDist > AV_RESCUE_RADIUS)
        return;

    if (CountAVBotsAssignedToGY(map, team, nearestNode) >= 3)
        return;

    if (roll_chance_u(50))
    {
        pAI->m_avAssignedGY = nearestNode;
        pAI->m_avStayGuardAfterCapture = true;
    }
}

static std::pair<uint32, uint32> AV_HordeAttackObjectives[] =
{
    // Attack (array order = priority; retake objectives go last)
    { BG_AV_STONEHEARTH_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_STONEHEARTH_GY, ALLIANCE_CONTROLLED },
    { BG_AV_ICEWING_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_STORMPIKE_GY, ALLIANCE_CONTROLLED },
    { BG_AV_DUN_BALDAR_SOUTH_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_DUN_BALDAR_NORTH_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_STORMPIKE_AID_STATION_GY, ALLIANCE_CONTROLLED },
    { BG_AV_ICEBLOOD_GY, ALLIANCE_CONTROLLED },      // retake if Alliance captured it (lowest priority)
};

static std::pair<uint32, uint32> AV_HordeDefendObjectives[] =
{
    // Defend
    { BG_AV_ICEBLOOD_GY, ALLIANCE_ASSAULTED },    // closest Horde GY to frontline
    { BG_AV_FROSTWOLF_GY, ALLIANCE_ASSAULTED },
    { BG_AV_EAST_FROSTWOLF_TOWER, ALLIANCE_ASSAULTED },
    { BG_AV_WEST_FROSTWOLF_TOWER, ALLIANCE_ASSAULTED },
    { BG_AV_TOWER_POINT_TOWER, ALLIANCE_ASSAULTED },
    { BG_AV_ICEBLOOD_TOWER, ALLIANCE_ASSAULTED },
};

static std::pair<uint32, uint32> AV_AllianceAttackObjectives[] =
{
    // Attack
    { BG_AV_STONEHEARTH_GY, HORDE_CONTROLLED },  // retake if Horde captured it
    { BG_AV_ICEBLOOD_TOWER, HORDE_CONTROLLED },
    { BG_AV_ICEBLOOD_GY, HORDE_CONTROLLED },
    { BG_AV_TOWER_POINT_TOWER, HORDE_CONTROLLED },
    { BG_AV_FROSTWOLF_GY, HORDE_CONTROLLED },
    { BG_AV_EAST_FROSTWOLF_TOWER, HORDE_CONTROLLED },
    { BG_AV_WEST_FROSTWOLF_TOWER, HORDE_CONTROLLED },
    { BG_AV_FROSTWOLF_RELIEF_HUT_GY, HORDE_CONTROLLED },
};

static std::pair<uint32, uint32> AV_AllianceDefendObjectives[] =
{
    // Defend
    { BG_AV_STONEHEARTH_GY, HORDE_ASSAULTED },
    { BG_AV_STORMPIKE_GY, HORDE_ASSAULTED },
    { BG_AV_DUN_BALDAR_SOUTH_BUNKER, HORDE_ASSAULTED },
    { BG_AV_DUN_BALDAR_NORTH_BUNKER, HORDE_ASSAULTED },
    { BG_AV_ICEWING_BUNKER, HORDE_ASSAULTED },
    { BG_AV_STONEHEARTH_BUNKER, HORDE_ASSAULTED },
};

// Dedicated cavalry hunter selection (GUID-stable, 2 per faction from DPS pool).
// Excludes healers, fixed GY guards, mine bots, and bots already in GY capture hold.
static constexpr uint32 CAVALRY_HUNTER_COUNT = 2;

static bool ShouldBeAVCavalryHunter(BattleBotAI const* pAI)
{
    Team const team = pAI->me->GetTeam();
    Map* map = pAI->me->GetMap();
    if (!map)
        return false;

    uint32 alreadyAssigned = 0;
    std::vector<uint32> eligibleGuids;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player || player->GetTeam() != team || !player->IsBot())
            continue;
        BattleBotAI const* pBotAI = dynamic_cast<BattleBotAI const*>(player->AI());
        if (!pBotAI)
            continue;
        if (pBotAI->m_avIsCavalryHunter)
        {
            ++alreadyAssigned;
            continue;
        }
        if (CombatBotBaseAI::IsHealerClass(player->GetClass()))
            continue;
        if (pBotAI->m_avAssignedGY != 0)
            continue;
        if (pBotAI->m_avIsMineBot)
            continue;
        if (BattleBotIsInAVGyCaptureHold(pBotAI))
            continue;
        eligibleGuids.push_back(player->GetGUIDLow());
    }

    if (alreadyAssigned >= CAVALRY_HUNTER_COUNT)
        return false;

    uint32 const remainingSlots = CAVALRY_HUNTER_COUNT - alreadyAssigned;
    std::sort(eligibleGuids.begin(), eligibleGuids.end());

    uint32 const myGuid = pAI->me->GetGUIDLow();
    uint32 count = 0;
    for (uint32 guid : eligibleGuids)
    {
        if (count >= remainingSlots)
            break;
        if (guid == myGuid)
            return true;
        ++count;
    }
    return false;
}

bool BattleBotAI::StartNewPathToObjective()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg)
        return false;

    if (bg->GetStatus() == STATUS_WAIT_JOIN)
        return false;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_AB:
            return BattleBotSelectABObjective(this);
        case BATTLEGROUND_AV:
        {
            // Guard assignment takes priority over everything.
            if (m_avAssignedGY != 0)
                return BattleBotSelectAVGuardObjective(this);

            if (m_avGuardGraveyards)
            {
                uint32 guardGY = 0;
                if (FindAVGYToGuard(this, guardGY))
                {
                    m_avAssignedGY = guardGY;
                    m_avStayGuardAfterCapture = roll_chance_u(50);
                    return BattleBotSelectAVGuardObjective(this);
                }
            }

            // Mine bot decision: assigned once per BG after a 3-minute delay, then permanent.
            // Skipped entirely when mineMissionCount == 0 (e.g. Mode 2 / Push).
            if (m_avMineMissionCount > 0)
            {
                constexpr uint32 MINE_BOT_DELAY_MS = 3 * 60 * 1000;
                if ((!m_avMineBotDecided || m_avMineBotBgInstance != bg->GetInstanceID()) &&
                    bg->GetStartTime() >= MINE_BOT_DELAY_MS)
                {
                    m_avMineBotDecided = true;
                    m_avMineBotBgInstance = bg->GetInstanceID();
                    m_avIsMineBot = ShouldBeAVMineBot(this);
                }
            }

            // Mine bots patrol their mine indefinitely; boss kill contribution in HandleKillUnit.
            // Alliance: WPs 0-18 outdoor → mine entrance, WPs 19-192 interior patrol loop.
            // Horde:    WPs 0-22 outdoor → mine entrance, WPs 23-241 interior patrol loop.
            if (m_avIsMineBot)
            {
                Team const myTeam = me->GetTeam();

                // Release if own key GY is taken by the enemy — bot is needed on the frontline.
                // Alliance key GY: Stonehearth; Horde key GY: Frostwolf.
                bool const ownKeyGyLost = (myTeam == ALLIANCE)
                    ? (bg->IsActiveEvent(BG_AV_STONEHEARTH_GY, HORDE_ASSAULTED) || bg->IsActiveEvent(BG_AV_STONEHEARTH_GY, HORDE_CONTROLLED))
                    : (bg->IsActiveEvent(BG_AV_FROSTWOLF_GY, ALLIANCE_ASSAULTED) || bg->IsActiveEvent(BG_AV_FROSTWOLF_GY, ALLIANCE_CONTROLLED));

                if (!ownKeyGyLost)
                {
                    uint8 const mineIdx = (myTeam == ALLIANCE) ? BG_AV_NORTH_MINE : BG_AV_SOUTH_MINE;

                    if (m_avMineState == AV_MINE_NONE)
                    {
                        m_avMineIndex = mineIdx;
                        m_avMineState = AV_MINE_GOING;
                    }

                    BattleBotPath* const pMinePath = (myTeam == ALLIANCE)
                        ? &vPath_AV_Stormpike_to_Irondeep_Morloch
                        : &vPath_AV_TowerPoint_to_Coldtooth_Snivvle;

                    // Already traveling — movement driven by MovementInform callbacks.
                    if (m_currentPath == pMinePath)
                        return true;

                    uint32 const interiorStart = (myTeam == ALLIANCE) ? 19 : 23;
                    uint32 const pathLast = static_cast<uint32>(pMinePath->size() - 1);
                    BattleBotWaypoint const& lastWP = (*pMinePath)[pathLast];

                    if (me->GetDistance(lastWP.x, lastWP.y, lastWP.z) < 50.0f)
                    {
                        // Finished a forward pass — loop back to interior start.
                        m_currentPath = pMinePath;
                        m_movingInReverse = false;
                        m_currentPoint = interiorStart - 1;
                        MoveToNextPoint();
                    }
                    else
                    {
                        // First entry or resumed after revival/combat far from boss.
                        m_currentPath = pMinePath;
                        m_movingInReverse = false;
                        m_currentPoint = static_cast<uint32>(-1);
                        MoveToNextPoint();
                    }
                    return true;
                }

                // Own key GY lost: release back to normal routing.
                m_avIsMineBot = false;
                m_avMineState = AV_MINE_NONE;
            }

            // Cavalry hunter decision: assigned once per BG after 3-minute delay, then permanent.
            // Skip while this bot is an active mine bot (mine mission has higher priority).
            // A released mine bot may re-evaluate here, but cavalry slots are almost always
            // already full by then, so ShouldBeAVCavalryHunter will simply return false.
            if (!m_avIsMineBot)
            {
                constexpr uint32 CAVALRY_HUNTER_DELAY_MS = 3 * 60 * 1000;
                if ((!m_avCavalryHunterDecided || m_avCavalryHunterBgInstance != bg->GetInstanceID()) &&
                    bg->GetStartTime() >= CAVALRY_HUNTER_DELAY_MS)
                {
                    m_avCavalryHunterDecided = true;
                    m_avCavalryHunterBgInstance = bg->GetInstanceID();
                    m_avIsCavalryHunter = ShouldBeAVCavalryHunter(this);
                }
            }

            // Cavalry hunters seek faction-specific animals and kill them.
            // Kill contribution handled in HandleKillUnit → BotContributeCavalryAssault.
            //   Horde: Frostwolf Wolf (10981) ~18 spawns, 300s respawn, area diagonal ~1031 yd
            //   Alliance: Alterac Ram (10990) ~35 spawns, 430s respawn, area diagonal ~944 yd
            // 400-yard radius covers the full spawn cluster from the centre point.
            if (m_avIsCavalryHunter)
            {
                constexpr float ATTACK_RADIUS   = 400.0f;
                constexpr uint32 FROSTWOLF_WOLF = 10981;
                constexpr uint32 ALTERAC_RAM    = 10990;
                uint32 const animalEntry = (me->GetTeam() == HORDE) ? FROSTWOLF_WOLF : ALTERAC_RAM;

                if (Creature* pAnimal = me->FindNearestCreature(animalEntry, ATTACK_RADIUS, true))
                {
                    AttackStart(pAnimal);
                    return true;
                }

                // No animal nearby — walk to the spawn cluster so we arrive ready to hunt.
                Position const spawnCentre = (me->GetTeam() == HORDE)
                    ? Position(-848.0f, -340.0f, 57.0f, 0.0f)   // Frostwolf Wolf area centre
                    : Position(297.0f,  -252.0f,   5.0f, 0.0f); // Alterac Ram area centre
                return StartNewPathToPosition(spawnCentre, vPaths_AV);
            }

            // holdCaptureUntilControlled: non-mine bots stay at a GY being assaulted until
            // it's fully controlled. Mine bots bypass this — their mission has higher priority.
            // Returning true without setting a path keeps the bot at its current position.
            if (m_avHoldCaptureUntilControlled && BattleBotIsInAVGyCaptureHold(this))
                return true;

            // Alliance and Horde code is intentionally different.
            // Horde bots are more united and always go together.
            // Alliance bots can pick random objective.

            if (me->GetTeam() == HORDE)
            {
                // End Boss: keep the old clear sequence; captain death alone must not
                // make Horde skip Stonehearth/Icewing/Dun Baldar objectives.
                if (!bg->IsActiveEvent(BG_AV_DUN_BALDAR_SOUTH_BUNKER, ALLIANCE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_DUN_BALDAR_NORTH_BUNKER, ALLIANCE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_ICEWING_BUNKER, ALLIANCE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_STONEHEARTH_BUNKER, ALLIANCE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_STORMPIKE_AID_STATION_GY, ALLIANCE_CONTROLLED))
                {
                    if (Creature* pVanndar = me->GetMap()->GetCreature(bg->GetSingleCreatureGuid(BG_AV_BOSS_A, 0)))
                        return StartNewPathToPosition(pVanndar->GetPosition(), vPaths_AV);
                }

                // Only go to Snowfall Graveyard if already close to it.
                // 之前这里不管命中三种状态里的哪一种，GetSingleGameObjectGuid都写死查NEUTRAL_CONTROLLED
                // 这个event2——旗子一旦被冲击/占领过，中立状态对应的那个GameObject实例早被移除，查出来
                // 是nullptr，导致机器人只有在旗子还是中立状态时才能找到路径，一旦被冲击过就再也找不到路，
                // 旗子被反复触发重置5分钟计时器但永远占不满（对应玩家反馈"一直去开旗但一直开不成功"）。
                // 现在按实际命中的状态去查对应的旗子对象。
                {
                    uint8 snowfallEvent2 = 0xFF;
                    if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, ALLIANCE_ASSAULTED))
                        snowfallEvent2 = ALLIANCE_ASSAULTED;
                    else if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, ALLIANCE_CONTROLLED))
                        snowfallEvent2 = ALLIANCE_CONTROLLED;
                    else if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, NEUTRAL_CONTROLLED))
                        snowfallEvent2 = NEUTRAL_CONTROLLED;

                    if (snowfallEvent2 != 0xFF)
                    {
                        if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(BG_AV_SNOWFALL_GY, snowfallEvent2)))
                            if (me->IsWithinDist(pGO, VISIBILITY_DISTANCE_LARGE))
                                return StartNewPathToPosition(pGO->GetPosition(), vPaths_AV);
                    }
                }

                if (!bg->IsActiveEvent(BG_AV_NodeEventCaptainDead_A, 0))
                {
                    if (Creature* pBalinda = me->GetMap()->GetCreature(bg->GetSingleCreatureGuid(BG_AV_CAPTAIN_A, 0)))
                        return StartNewPathToPosition(pBalinda->GetPosition(), vPaths_AV);
                }

                for (const auto& objective : AV_HordeDefendObjectives)
                {
                    if (bg->IsActiveEvent(objective.first, ALLIANCE_ASSAULTED))
                    {
                        if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(objective.first, objective.second)))
                            if (me->IsWithinDist(pGO, VISIBILITY_DISTANCE_LARGE))
                                return StartNewPathToPosition(pGO->GetPosition(), vPaths_AV);
                    }
                }

                // Random mode may split a few bots deeper; Native/Push keep the old ordered push.
                bool skipFirst = (m_avMode == AV_MODE_RANDOM) && roll_chance_u(30);
                GameObject* pHordeFallback = nullptr;
                for (const auto& objective : AV_HordeAttackObjectives)
                {
                    if (bg->IsActiveEvent(objective.first, ALLIANCE_ASSAULTED) || bg->IsActiveEvent(objective.first, ALLIANCE_CONTROLLED) || bg->IsActiveEvent(objective.first, NEUTRAL_CONTROLLED))
                    {
                        if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(objective.first, objective.second)))
                        {
                            if (skipFirst)
                            {
                                if (!pHordeFallback) pHordeFallback = pGO;
                                skipFirst = false;
                                continue;
                            }
                            return StartNewPathToPosition(pGO->GetPosition(), vPaths_AV);
                        }
                    }
                }
                if (pHordeFallback)
                    return StartNewPathToPosition(pHordeFallback->GetPosition(), vPaths_AV);
            }
            else // ALLIANCE
            {
                // End boss
                if (!bg->IsActiveEvent(BG_AV_ICEBLOOD_TOWER, HORDE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_TOWER_POINT_TOWER, HORDE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_EAST_FROSTWOLF_TOWER, HORDE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_WEST_FROSTWOLF_TOWER, HORDE_CONTROLLED) &&
                    !bg->IsActiveEvent(BG_AV_FROSTWOLF_RELIEF_HUT_GY, HORDE_CONTROLLED))
                {
                    if (Creature* pDrek = me->GetMap()->GetCreature(bg->GetSingleCreatureGuid(BG_AV_BOSS_H, 0)))
                        return StartNewPathToPosition(pDrek->GetPosition(), vPaths_AV);
                }

                // Only go to Snowfall Graveyard if already close to it.
                // 同Horde分支的坑：不管命中三种状态里的哪一种，之前都写死查NEUTRAL_CONTROLLED，
                // 旗子被冲击/占领过之后中立状态的GameObject实例已被移除，查出来是nullptr，导致
                // Alliance机器人同样只有在旗子还是中立状态时才能找到路径。现在按实际命中的状态查。
                {
                    uint8 snowfallEvent2 = 0xFF;
                    if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, HORDE_ASSAULTED))
                        snowfallEvent2 = HORDE_ASSAULTED;
                    else if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, HORDE_CONTROLLED))
                        snowfallEvent2 = HORDE_CONTROLLED;
                    else if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, NEUTRAL_CONTROLLED))
                        snowfallEvent2 = NEUTRAL_CONTROLLED;

                    if (snowfallEvent2 != 0xFF)
                    {
                        if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(BG_AV_SNOWFALL_GY, snowfallEvent2)))
                            if (me->IsWithinDist(pGO, VISIBILITY_DISTANCE_LARGE))
                                return StartNewPathToPosition(pGO->GetPosition(), vPaths_AV);
                    }
                }
                
                // Chance to defend.
                if (roll_chance_u(25))
                {
                    for (const auto& objective : AV_AllianceDefendObjectives)
                    {
                        if (bg->IsActiveEvent(objective.first, HORDE_ASSAULTED))
                        {
                            if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(objective.first, objective.second)))
                                return StartNewPathToPosition(pGO->GetPosition(), vPaths_AV);
                        }
                    }
                }

                // Attack closest objective; behavior 4 tracks second-closest for skip.
                WorldObject* pAttackObjectiveObject = nullptr;
                float attackObjectiveDistance = FLT_MAX;
                WorldObject* pSecondObjective = nullptr;
                float secondObjectiveDistance = FLT_MAX;

                if (!bg->IsActiveEvent(BG_AV_NodeEventCaptainDead_H, 0))
                {
                    if (Creature* pGalvangar = me->GetMap()->GetCreature(bg->GetSingleCreatureGuid(BG_AV_CAPTAIN_H, 0)))
                    {
                        pAttackObjectiveObject = pGalvangar;
                        attackObjectiveDistance = me->GetDistance(pGalvangar);
                    }
                }

                for (const auto& objective : AV_AllianceAttackObjectives)
                {
                    if (bg->IsActiveEvent(objective.first, HORDE_ASSAULTED) || bg->IsActiveEvent(objective.first, HORDE_CONTROLLED) || bg->IsActiveEvent(objective.first, NEUTRAL_CONTROLLED))
                    {
                        if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(objective.first, objective.second)))
                        {
                            float const distance = me->GetDistance(pGO);
                            if (attackObjectiveDistance > distance)
                            {
                                pSecondObjective = pAttackObjectiveObject;
                                secondObjectiveDistance = attackObjectiveDistance;
                                pAttackObjectiveObject = pGO;
                                attackObjectiveDistance = distance;
                            }
                            else if (secondObjectiveDistance > distance)
                            {
                                pSecondObjective = pGO;
                                secondObjectiveDistance = distance;
                            }
                        }
                    }
                }

                // Random mode may split a few bots deeper; Native/Push keep the old closest-objective push.
                if (m_avMode == AV_MODE_RANDOM && pAttackObjectiveObject && pSecondObjective && roll_chance_u(30))
                    pAttackObjectiveObject = pSecondObjective;

                if (pAttackObjectiveObject)
                    return StartNewPathToPosition(pAttackObjectiveObject->GetPosition(), vPaths_AV);
            }
            break;
        }
        case BATTLEGROUND_WS:
            return BattleBotSelectWSGObjective(this);
        default:
            break;
    }

    return false;
}

void BattleBotAI::ClearPath()
{
    m_currentPath = nullptr;
    m_currentPoint = 0;
    m_movingInReverse = false;
}
