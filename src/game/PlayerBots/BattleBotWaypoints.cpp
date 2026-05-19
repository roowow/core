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

    pAI->me->GetMotionMaster()->MovePoint(pAI->m_currentPoint, nextPoint.x + frand(-1, 1), nextPoint.y + frand(-1, 1), nextPoint.z, MOVE_PATHFINDING | MOVE_RUN_MODE);
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

// Mine path uses MoveToNextPointSpecial callbacks (MOVE_PATHFINDING | MOVE_RUN_MODE,
// no MOVE_EXCLUDE_STEEP_SLOPES) so the pathfinder can handle the steep ascent
// through the mine interior without rerouting through terrain walls.
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
    { 747.2754f, -313.9810f, 55.0999f, &MoveToNextPointSpecial },
    { 751.7374f, -319.3745f, 56.5670f, &MoveToNextPointSpecial },
    { 756.1774f, -324.7858f, 58.0850f, &MoveToNextPointSpecial },
    { 760.5327f, -330.2660f, 59.4319f, &MoveToNextPointSpecial },
    { 765.0652f, -335.5873f, 60.7855f, &MoveToNextPointSpecial },
    { 770.1476f, -340.3909f, 61.4104f, &MoveToNextPointSpecial },
    { 775.4432f, -343.7472f, 61.4104f, &MoveToNextPointSpecial },
    { 781.0787f, -343.5684f, 61.4104f, &MoveToNextPointSpecial },
    { 786.3533f, -339.4595f, 61.9130f, &MoveToNextPointSpecial },
    { 789.2404f, -332.4202f, 62.9791f, &MoveToNextPointSpecial },
    { 796.4823f, -335.3680f, 63.0614f, &MoveToNextPointSpecial },
    { 803.1797f, -338.5370f, 63.7152f, &MoveToNextPointSpecial },
    { 810.5118f, -336.4881f, 64.5936f, &MoveToNextPointSpecial },
    { 815.9957f, -330.8441f, 64.3564f, &MoveToNextPointSpecial },
    { 821.3481f, -328.4308f, 64.0906f, &MoveToNextPointSpecial },
    { 827.8917f, -330.4461f, 64.3465f, &MoveToNextPointSpecial },
    { 833.1855f, -333.5173f, 64.6426f, &MoveToNextPointSpecial },
    { 838.7567f, -337.7537f, 65.3710f, &MoveToNextPointSpecial },
    { 843.8019f, -340.3744f, 65.7461f, &MoveToNextPointSpecial },
    { 850.0931f, -343.4436f, 66.0264f, &MoveToNextPointSpecial },
    { 856.5757f, -346.0758f, 65.5041f, &MoveToNextPointSpecial },
    { 862.4332f, -347.8371f, 64.5341f, &MoveToNextPointSpecial },
    { 870.0270f, -348.7738f, 64.5399f, &MoveToNextPointSpecial },
    { 876.3622f, -347.7039f, 65.6523f, &MoveToNextPointSpecial },
    { 882.1097f, -344.6449f, 66.6220f, &MoveToNextPointSpecial },
    { 887.0400f, -339.6767f, 67.3502f, &MoveToNextPointSpecial },
    { 891.6273f, -335.4755f, 67.4392f, &MoveToNextPointSpecial },
    { 896.4573f, -333.1840f, 67.5219f, &MoveToNextPointSpecial },
    { 904.6970f, -331.6394f, 67.2410f, &MoveToNextPointSpecial },
    { 910.7316f, -332.3532f, 66.6613f, &MoveToNextPointSpecial },
    { 917.2830f, -334.8167f, 66.2733f, &MoveToNextPointSpecial },
    { 924.8119f, -337.8008f, 65.8492f, &MoveToNextPointSpecial },
    { 928.6079f, -342.4911f, 65.4707f, &MoveToNextPointSpecial },
    { 927.7592f, -350.2041f, 65.9241f, &MoveToNextPointSpecial },
    { 926.0995f, -357.0023f, 65.6402f, &MoveToNextPointSpecial },
    { 924.1185f, -363.7159f, 66.1701f, &MoveToNextPointSpecial },
    { 922.2449f, -370.4605f, 65.4642f, &MoveToNextPointSpecial },
    { 921.3293f, -376.6117f, 63.8428f, &MoveToNextPointSpecial },
    { 920.9985f, -383.6017f, 61.6317f, &MoveToNextPointSpecial },
    { 921.3293f, -390.5931f, 60.8913f, &MoveToNextPointSpecial },
    { 921.8428f, -397.5742f, 60.1568f, &MoveToNextPointSpecial },
    { 922.2695f, -404.5612f, 58.2709f, &MoveToNextPointSpecial },
    { 922.6887f, -411.5469f, 56.4949f, &MoveToNextPointSpecial },
    { 922.0705f, -417.4118f, 56.0405f, &MoveToNextPointSpecial },
    { 919.4290f, -425.1350f, 56.6067f, &MoveToNextPointSpecial },
    { 916.4502f, -431.3454f, 57.1667f, &MoveToNextPointSpecial },
    { 911.2559f, -434.7462f, 57.5335f, &MoveToNextPointSpecial },
    { 906.2648f, -435.2204f, 58.0658f, &MoveToNextPointSpecial },
    { 898.4221f, -433.2715f, 58.0214f, &MoveToNextPointSpecial },
    { 891.7562f, -431.1346f, 55.4655f, &MoveToNextPointSpecial },
    { 885.0898f, -428.9992f, 53.7119f, &MoveToNextPointSpecial },
    { 878.3228f, -427.2171f, 52.4257f, &MoveToNextPointSpecial },
    { 873.2521f, -426.1608f, 51.1973f, &MoveToNextPointSpecial },
    { 868.8007f, -431.5623f, 50.7682f, &MoveToNextPointSpecial },
    { 865.9803f, -437.0531f, 50.6978f, &MoveToNextPointSpecial },
    { 864.3466f, -443.8597f, 50.8458f, &MoveToNextPointSpecial },
};

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
    { -960.2833f, -262.7704f, 64.3421f, nullptr },
    { -959.1378f, -256.7336f, 64.9495f, nullptr },
    { -954.3208f, -251.2252f, 65.3379f, nullptr },
    { -950.5594f, -246.2899f, 65.6322f, nullptr },
    { -945.4386f, -244.1082f, 65.9180f, nullptr },
    { -939.8275f, -241.9550f, 66.4189f, nullptr },
    { -933.3918f, -237.4376f, 67.1937f, nullptr },
    { -928.5602f, -232.3808f, 67.8597f, nullptr },
    { -925.4470f, -225.2723f, 69.3805f, nullptr },
    { -924.5667f, -220.0994f, 70.5566f, nullptr },
    { -925.4279f, -214.7184f, 71.6138f, nullptr },
    { -928.6381f, -207.9140f, 73.0227f, nullptr },
    { -933.3755f, -204.6613f, 74.3951f, nullptr },
    { -939.8578f, -202.0197f, 76.4508f, nullptr },
    { -944.5110f, -199.4895f, 77.0072f, nullptr },
    { -944.0008f, -193.2953f, 76.8878f, nullptr },
    { -941.1964f, -187.4930f, 77.5361f, nullptr },
    { -942.8546f, -182.5168f, 78.4077f, nullptr },
    { -947.9466f, -177.2496f, 78.4463f, nullptr },
    { -953.4483f, -173.4925f, 78.1010f, nullptr },
    { -954.7617f, -167.4946f, 78.1104f, nullptr },
    { -953.0056f, -162.7873f, 78.3515f, nullptr },
    { -950.2720f, -156.3431f, 78.7969f, nullptr },
    { -947.6274f, -149.8627f, 79.4500f, nullptr },
    { -945.1370f, -143.3210f, 79.9146f, nullptr },
    { -943.8690f, -138.2334f, 79.8593f, nullptr },
    { -943.3285f, -131.4956f, 79.1605f, nullptr },
    { -944.6203f, -124.6160f, 78.4408f, nullptr },
    { -945.6746f, -117.6968f, 78.8467f, nullptr },
    { -946.9941f, -112.5348f, 79.7423f, nullptr },
    { -950.8286f, -107.5055f, 80.7873f, nullptr },
    { -954.6636f, -104.1033f, 81.2618f, nullptr },
    { -960.0722f, -99.6634f, 81.4109f, nullptr },
    { -964.3049f, -95.1076f, 81.4145f, nullptr },
    { -967.2874f, -88.5635f, 81.1775f, nullptr },
    { -967.0710f, -81.5658f, 80.5754f, nullptr },
    { -965.7562f, -74.6904f, 80.1361f, nullptr },
    { -964.3475f, -67.8336f, 79.8033f, nullptr },
    { -963.5542f, -61.6764f, 78.7310f, nullptr },
    { -966.6141f, -55.0845f, 77.4820f, nullptr },
    { -960.8079f, -55.9332f, 78.9160f, nullptr },
    { -954.6627f, -57.7340f, 79.7014f, nullptr },
    { -948.1019f, -60.1717f, 79.7711f, nullptr },
    { -941.5959f, -62.7547f, 80.0057f, nullptr },
    { -936.2493f, -64.6331f, 79.8637f, nullptr },
    { -929.0509f, -64.8926f, 78.8937f, nullptr },
    { -922.1203f, -64.0126f, 76.7897f, nullptr },
    { -915.3257f, -62.3382f, 75.0193f, nullptr },
    { -908.5811f, -60.4647f, 74.7523f, nullptr },
    { -901.8364f, -58.5913f, 73.3433f, nullptr },
    { -895.4684f, -57.0057f, 71.4998f, nullptr },
    { -888.8160f, -56.6577f, 70.1055f, nullptr },
    { -881.5418f, -57.5201f, 70.3146f, nullptr },
    { -874.8353f, -59.5260f, 71.0993f, nullptr },
    { -869.5035f, -60.8172f, 71.3211f, nullptr },
    { -866.8955f, -65.3491f, 71.7876f, nullptr },
    { -867.7218f, -72.2987f, 72.4164f, nullptr },
    { -867.9849f, -82.7911f, 69.3123f, nullptr },
    { -867.6506f, -89.7821f, 67.6387f, nullptr },
    { -867.3318f, -96.7746f, 66.3379f, nullptr },
    { -867.4017f, -103.2480f, 64.9578f, nullptr },
    { -862.2948f, -105.1149f, 64.7026f, nullptr },
    { -856.1605f, -102.3480f, 65.2198f, nullptr },
    { -852.5985f, -97.6946f, 68.0407f, nullptr },
    { -850.7347f, -92.2076f, 68.5046f, nullptr },
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
    &vPath_AV_Stormpike_to_Irondeep_Morloch,
    &vPath_AV_TowerPoint_to_Coldtooth_Snivvle,
};

// Paths excluded from StartNewPathToPosition objective routing.
// These are cave-exit paths whose WP 0 carries an AtCaveExit callback.
// From inside the cave they are the only paths with waypoints in the
// 50-yard search radius, so without this exclusion they get selected as
// "proxy" routes for unrelated objectives, trapping bots in a WP-0 loop.
// StartNewPathFromBeginning / StartNewPathFromAnywhere still use them.
std::vector<BattleBotPath*> const vPaths_ObjectiveExcluded =
{
    &vPath_AV_Alliance_Cave_to_Alliance_Cave_Slop_Crossroad,
    &vPath_AV_Horde_Cave_to_Tower_Point_Crossroad,
    &vPath_AV_Horde_Cave_to_Frostwolf_Graveyard_Flag,
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
        // Guard against stale callbacks: a queued PointMovementGenerator can fire its
        // Finalize() after the path has been swapped (e.g. MotionMaster::Clear() from
        // class AI). The `data` index refers to the *previous* path, so it may be out
        // of range for the current one. Without this check, vector::at() throws and
        // aborts the world thread.
        if (m_currentPath && data < m_currentPath->size())
        {
            if ((*m_currentPath)[data].pFunc)
                (*(*m_currentPath)[data].pFunc)(this);
            else
                MoveToNextPoint();
        }
        else
        {
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
    m_currentPoint = closestPoint-1;
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
    m_currentPoint = m_movingInReverse ? closestPoint + 1 : closestPoint - 1;
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

    // Priority 1: We are actively capturing an enemy GY — all bots converge
    for (uint32 i = 0; i < 3; ++i)
    {
        if (bg->IsActiveEvent(enemyNative[i], ownAssaulted))
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
// Called once per bot after the 5-minute delay. The lowest MINE_MISSION_COUNT GUIDs among
// eligible DPS bots (non-healer, no fixed GY guard, not in temp GY hold) become mine bots.
static bool ShouldBeAVMineBot(BattleBotAI const* pAI)
{
    constexpr uint32 MINE_MISSION_COUNT = 5;
    Team const team = pAI->me->GetTeam();
    Map* map = pAI->me->GetMap();
    if (!map)
        return false;

    std::vector<uint32> eligibleGuids;
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player || player->GetTeam() != team || !player->IsBot())
            continue;
        BattleBotAI const* pBotAI = dynamic_cast<BattleBotAI const*>(player->AI());
        if (!pBotAI)
            continue;
        if (CombatBotBaseAI::IsHealerClass(player->GetClass()))
            continue;
        if (pBotAI->m_avAssignedGY != 0)
            continue;
        if (BattleBotIsInAVGyCaptureHold(pBotAI))
            continue;
        eligibleGuids.push_back(player->GetGUIDLow());
    }

    std::sort(eligibleGuids.begin(), eligibleGuids.end());

    uint32 const myGuid = pAI->me->GetGUIDLow();
    uint32 count = 0;
    for (uint32 guid : eligibleGuids)
    {
        if (count >= MINE_MISSION_COUNT)
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
    // Attack
    { BG_AV_STONEHEARTH_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_STONEHEARTH_GY, ALLIANCE_CONTROLLED },
    { BG_AV_ICEWING_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_STORMPIKE_GY, ALLIANCE_CONTROLLED },
    { BG_AV_DUN_BALDAR_SOUTH_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_DUN_BALDAR_NORTH_BUNKER, ALLIANCE_CONTROLLED },
    { BG_AV_STORMPIKE_AID_STATION_GY, ALLIANCE_CONTROLLED }
};

static std::pair<uint32, uint32> AV_HordeDefendObjectives[] =
{
    // Defend
    { BG_AV_FROSTWOLF_GY, ALLIANCE_ASSAULTED },
    { BG_AV_EAST_FROSTWOLF_TOWER, ALLIANCE_ASSAULTED },
    { BG_AV_WEST_FROSTWOLF_TOWER, ALLIANCE_ASSAULTED },
    { BG_AV_TOWER_POINT_TOWER, ALLIANCE_ASSAULTED },
    { BG_AV_ICEBLOOD_TOWER, ALLIANCE_ASSAULTED },
};

static std::pair<uint32, uint32> AV_AllianceAttackObjectives[] =
{
    // Attack
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
    { BG_AV_STORMPIKE_GY, HORDE_ASSAULTED },
    { BG_AV_DUN_BALDAR_SOUTH_BUNKER, HORDE_ASSAULTED },
    { BG_AV_DUN_BALDAR_NORTH_BUNKER, HORDE_ASSAULTED },
    { BG_AV_ICEWING_BUNKER, HORDE_ASSAULTED },
    { BG_AV_STONEHEARTH_BUNKER, HORDE_ASSAULTED },
};

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
            // Guard assignment takes priority over normal attack routing.
            if (m_avAssignedGY != 0)
                return BattleBotSelectAVGuardObjective(this);

            uint32 guardGY = 0;
            if (FindAVGYToGuard(this, guardGY))
            {
                m_avAssignedGY = guardGY;
                // Behavior 1: randomly decide now whether to guard after capture or advance
                m_avStayGuardAfterCapture = roll_chance_u(50);
                return BattleBotSelectAVGuardObjective(this);
            }

            // Mine bot decision: determined once per bot after a 5-minute delay.
            // The lowest MINE_MISSION_COUNT GUIDs among eligible DPS bots become mine bots.
            // This avoids routing mine bots before they have exited their starting cave.
            {
                constexpr uint32 MINE_BOT_DELAY_MS = 5 * 60 * 1000;
                if ((!m_avMineBotDecided || m_avMineBotBgInstance != bg->GetInstanceID()) &&
                    bg->GetStartTime() >= MINE_BOT_DELAY_MS)
                {
                    m_avMineBotDecided = true;
                    m_avMineBotBgInstance = bg->GetInstanceID();
                    m_avIsMineBot = ShouldBeAVMineBot(this);
                }
            }

            // Mine bot release: once the mine boss is dead the bot returns to normal routing.
            if (m_avIsMineBot)
            {
                bool const mineBossAlive = (me->GetTeam() == ALLIANCE)
                    ? bg->IsActiveEvent(BG_AV_MINE_BOSSES_NORTH, BG_AV_TEAM_NEUTRAL)
                    : bg->IsActiveEvent(BG_AV_MINE_BOSSES_SOUTH, BG_AV_TEAM_NEUTRAL);
                if (!mineBossAlive)
                    m_avIsMineBot = false;
            }

            // Mine bot routing (both teams handled here, before team-specific push logic).
            if (m_avIsMineBot)
            {
                if (me->GetTeam() == ALLIANCE)
                {
                    static Position const morlochPos = { 864.3466f, -443.8597f, 50.8458f, 0.0f };
                    if (me->GetDistance(morlochPos.x, morlochPos.y, morlochPos.z) > 30.0f)
                    {
                        if (StartNewPathToPosition(morlochPos, vPaths_AV))
                            return true;
                        // Fallback: directly assign mine path so pathfinder navigates to WP 0.
                        m_currentPath = &vPath_AV_Stormpike_to_Irondeep_Morloch;
                        m_movingInReverse = false;
                        m_currentPoint = static_cast<uint32>(-1);
                        MoveToNextPoint();
                        return true;
                    }
                    return true; // Already at mine — stay and let combat AI engage Morloch.
                }
                else // HORDE
                {
                    static Position const snivvlePos = { -850.7347f, -92.2076f, 68.5046f, 0.0f };
                    if (me->GetDistance(snivvlePos.x, snivvlePos.y, snivvlePos.z) > 30.0f)
                    {
                        if (StartNewPathToPosition(snivvlePos, vPaths_AV))
                            return true;
                        // Fallback: directly assign mine path.
                        m_currentPath = &vPath_AV_TowerPoint_to_Coldtooth_Snivvle;
                        m_movingInReverse = false;
                        m_currentPoint = static_cast<uint32>(-1);
                        MoveToNextPoint();
                        return true;
                    }
                    return true; // Already at mine — stay and let combat AI engage Snivvle.
                }
            }

            // Alliance and Horde code is intentionally different.
            // Horde bots are more united and always go together.
            // Alliance bots can pick random objective.

            if (me->GetTeam() == HORDE)
            {
                // End Boss
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
                if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, ALLIANCE_ASSAULTED) || bg->IsActiveEvent(BG_AV_SNOWFALL_GY, ALLIANCE_CONTROLLED) || bg->IsActiveEvent(BG_AV_SNOWFALL_GY, NEUTRAL_CONTROLLED))
                {
                    if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(BG_AV_SNOWFALL_GY, NEUTRAL_CONTROLLED)))
                        if (me->IsWithinDist(pGO, VISIBILITY_DISTANCE_LARGE))
                            return StartNewPathToPosition(pGO->GetPosition(), vPaths_AV);  
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

                // Behavior 4: 30% chance to skip the first eligible objective and go deeper
                bool skipFirst = roll_chance_u(30);
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
                if (bg->IsActiveEvent(BG_AV_SNOWFALL_GY, HORDE_ASSAULTED) || bg->IsActiveEvent(BG_AV_SNOWFALL_GY, HORDE_CONTROLLED) || bg->IsActiveEvent(BG_AV_SNOWFALL_GY, NEUTRAL_CONTROLLED))
                {
                    if (GameObject* pGO = me->GetMap()->GetGameObject(bg->GetSingleGameObjectGuid(BG_AV_SNOWFALL_GY, NEUTRAL_CONTROLLED)))
                        if (me->IsWithinDist(pGO, VISIBILITY_DISTANCE_LARGE))
                            return StartNewPathToPosition(pGO->GetPosition(), vPaths_AV);
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

                // Behavior 4: 30% chance to skip closest objective and attack the second closest
                if (pAttackObjectiveObject && pSecondObjective && roll_chance_u(30))
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
