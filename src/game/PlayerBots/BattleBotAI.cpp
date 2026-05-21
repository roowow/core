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
#include "OO/OOMgr.h"
#include "BattleGround.h"
#include "BattleGroundAV.h"
#include "Player.h"
#include "SpellAuras.h"
#include "Group.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Chat.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "World.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "TargetedMovementGenerator.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

#include <cfloat>
#include <list>

enum BattleBotSpells
{
    BB_SPELL_FOOD = 1131,
    BB_SPELL_DRINK = 1137,
    BB_SPELL_AUTO_SHOT = 75,
    BB_SPELL_SHOOT_WAND = 5019,

    BB_SPELL_MOUNT_40_HUMAN = 470,
    BB_SPELL_MOUNT_40_NELF = 10787,
    BB_SPELL_MOUNT_40_DWARF = 6896,
    BB_SPELL_MOUNT_40_GNOME = 17456,
    BB_SPELL_MOUNT_40_TROLL = 10795,
    BB_SPELL_MOUNT_40_ORC = 581,
    BB_SPELL_MOUNT_40_TAUREN = 18363,
    BB_SPELL_MOUNT_40_UNDEAD = 8980,

    BB_SPELL_MOUNT_60_HUMAN = 22717,
    BB_SPELL_MOUNT_60_NELF = 22723,
    BB_SPELL_MOUNT_60_DWARF = 22720,
    BB_SPELL_MOUNT_60_GNOME = 22719,
    BB_SPELL_MOUNT_60_TROLL = 22721,
    BB_SPELL_MOUNT_60_ORC = 22724,
    BB_SPELL_MOUNT_60_TAUREN = 22718,
    BB_SPELL_MOUNT_60_UNDEAD = 22722,

    BB_SPELL_MOUNT_40_PALADIN = 13819,
    BB_SPELL_MOUNT_60_PALADIN = 23214,

    BB_SPELL_MOUNT_40_WARLOCK = 5784,
    BB_SPELL_MOUNT_60_WARLOCK = 23161,

    BB_ITEM_ARROW  = 2512,
    BB_ITEM_BULLET = 2516,
};

#define BB_UPDATE_INTERVAL 1000

#define GO_WSG_DROPPED_SILVERWING_FLAG 179785
#define GO_WSG_DROPPED_WARSONG_FLAG 179786
#define GO_WSG_SILVERWING_FLAG 179830
#define GO_WSG_WARSONG_FLAG 179831

static Unit* SelectHunterRogueMarkTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.hunter.pHuntersMark)
        return nullptr;

    Unit* currentVictim = pAI->me->GetVictim();
    if (currentVictim &&
        currentVictim->GetClass() == CLASS_ROGUE &&
        pAI->IsValidHostileTarget(currentVictim) &&
        pAI->CanTryToCastSpell(currentVictim, pAI->m_spells.hunter.pHuntersMark))
        return currentVictim;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 35.0f);

    Player* bestTarget = nullptr;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (player->GetClass() != CLASS_ROGUE ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.hunter.pHuntersMark))
            continue;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || distance < bestDistance)
        {
            bestTarget = player;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectHunterConcussiveShotTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.hunter.pConcussiveShot)
        return nullptr;

    Unit* currentVictim = pAI->me->GetVictim();
    if (currentVictim &&
        currentVictim->IsMoving() &&
        pAI->IsValidHostileTarget(currentVictim) &&
        (currentVictim->HasAura(AURA_WARSONG_FLAG) ||
         currentVictim->HasAura(AURA_SILVERWING_FLAG) ||
         currentVictim->GetVictim() == pAI->me ||
         (CombatBotBaseAI::IsMeleeDamageClass(currentVictim->GetClass()) && pAI->me->GetDistance(currentVictim) < 20.0f)) &&
        pAI->CanTryToCastSpell(currentVictim, pAI->m_spells.hunter.pConcussiveShot))
        return currentVictim;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);

    Player* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!player->IsMoving() ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.hunter.pConcussiveShot))
            continue;

        uint8 priority = 0;
        if (player->HasAura(AURA_WARSONG_FLAG) || player->HasAura(AURA_SILVERWING_FLAG))
            priority = 4;
        else if (player->GetVictim() == pAI->me && CombatBotBaseAI::IsMeleeDamageClass(player->GetClass()))
            priority = 3;
        else if (player == currentVictim)
            priority = 2;

        if (!priority)
            continue;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectHunterPetProtectTarget(BattleBotAI const* pAI)
{
    if (!pAI)
        return nullptr;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 12.0f);

    Player* bestTarget = nullptr;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (player->GetVictim() != pAI->me ||
            !CombatBotBaseAI::IsMeleeDamageClass(player->GetClass()) ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player))
            continue;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || distance < bestDistance)
        {
            bestTarget = player;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectHunterNearbyDpsTarget(BattleBotAI const* pAI, Unit const* pExcept)
{
    if (!pAI)
        return nullptr;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);

    Player* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me || player == pExcept)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player))
            continue;

        uint8 priority = 1;
        if (player->HasAura(AURA_WARSONG_FLAG) || player->HasAura(AURA_SILVERWING_FLAG))
            priority = 5;
        else if (player->GetVictim() == pAI->me)
            priority = 4;
        else if (CombatBotBaseAI::IsHealerClass(player->GetClass()))
            priority = 3;
        else if (player->GetVictim() && player->GetVictim()->IsFriendlyTo(pAI->me))
            priority = 2;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectShamanInterruptTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.shaman.pEarthShock)
        return nullptr;

    Unit* currentVictim = pAI->me->GetVictim();
    if (currentVictim &&
        currentVictim->IsNonMeleeSpellCasted(false, false, true) &&
        pAI->CanTryToCastSpell(currentVictim, pAI->m_spells.shaman.pEarthShock))
        return currentVictim;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 25.0f);

    Unit* bestTarget = nullptr;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me || player == currentVictim)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !player->IsNonMeleeSpellCasted(false, false, true) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.shaman.pEarthShock))
            continue;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || distance < bestDistance)
        {
            bestTarget = player;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectShamanPurgeTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.shaman.pPurge)
        return nullptr;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->IsValidDispelTarget(player, pAI->m_spells.shaman.pPurge) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.shaman.pPurge))
            continue;

        uint8 priority = 1;
        if (player->HasAura(AURA_WARSONG_FLAG) || player->HasAura(AURA_SILVERWING_FLAG))
            priority = 4;
        else if (CombatBotBaseAI::IsHealerClass(player->GetClass()))
            priority = 3;
        else if (CombatBotBaseAI::IsRangedDamageClass(player->GetClass()))
            priority = 2;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    Unit* currentVictim = pAI->me->GetVictim();
    if (!bestTarget && currentVictim &&
        pAI->IsValidDispelTarget(currentVictim, pAI->m_spells.shaman.pPurge) &&
        pAI->CanTryToCastSpell(currentVictim, pAI->m_spells.shaman.pPurge))
        return currentVictim;

    return bestTarget;
}

static bool BattleBotCanUseCrowdControl(BattleBotAI const* pAI, SpellEntry const* pSpellEntry, Unit* pTarget)
{
    if (!pAI || !pSpellEntry || !pTarget)
        return false;

    if (pAI->IsInDuel())
        return true;

    if (pSpellEntry->HasAuraInterruptFlag(AURA_INTERRUPT_DAMAGE_CANCELS) &&
        pAI->AreOthersOnSameTarget(pTarget->GetObjectGuid()))
        return false;

    if (pSpellEntry->HasSingleTargetAura())
    {
        auto const& singleAuras = pAI->me->GetSingleCastSpellTargets();
        if (singleAuras.find(pSpellEntry) != singleAuras.end())
            return false;
    }

    return true;
}

static bool BattleBotHasEntanglingRoots(Unit const* pTarget)
{
    if (!pTarget)
        return false;

    Unit::AuraList const& rootAuras = pTarget->GetAurasByType(SPELL_AURA_MOD_ROOT);
    for (Aura const* aura : rootAuras)
    {
        if (!aura)
            continue;

        SpellEntry const* spellInfo = aura->GetSpellProto();
        if (spellInfo && spellInfo->SpellName[0].find("Entangling Roots") != std::string::npos)
            return true;
    }

    return false;
}

static bool BattleBotHasBlind(Unit const* pTarget)
{
    if (!pTarget)
        return false;

    Unit::AuraList const& confuseAuras = pTarget->GetAurasByType(SPELL_AURA_MOD_CONFUSE);
    for (Aura const* aura : confuseAuras)
    {
        if (!aura)
            continue;

        SpellEntry const* spellInfo = aura->GetSpellProto();
        if (spellInfo && spellInfo->SpellName[0].find("Blind") != std::string::npos)
            return true;
    }

    return false;
}

static bool BattleBotIsHardControlled(Unit const* pTarget)
{
    return !pTarget ||
        BattleBotHasBlind(pTarget) ||
        BattleBotHasEntanglingRoots(pTarget) ||
        pTarget->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ||
        pTarget->HasAuraType(SPELL_AURA_MOD_FEAR) ||
        pTarget->HasAuraType(SPELL_AURA_MOD_CONFUSE);
}

static bool BattleBotShouldUsePsychicScream(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.priest.pPsychicScream ||
        !pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.priest.pPsychicScream))
        return false;

    uint8 nearbyThreats = 0;
    for (Unit* attacker : pAI->me->GetAttackers())
    {
        if (!attacker ||
            !pAI->IsValidHostileTarget(attacker) ||
            pAI->IsBadPlayer(attacker) ||
            BattleBotIsHardControlled(attacker) ||
            !pAI->m_spells.priest.pPsychicScream->IsTargetInRange(pAI->me, attacker))
            continue;

        ++nearbyThreats;
    }

    if (!nearbyThreats)
        return false;

    if (pAI->GetRole() == ROLE_HEALER)
    {
        if (nearbyThreats > 1 || pAI->me->GetHealthPercent() < 80.0f)
            return true;

        return BattleBotIsNearOpenObjectiveFlag(pAI, 18.0f);
    }

    return nearbyThreats > 1 || pAI->me->GetHealthPercent() < 80.0f;
}

static bool BattleBotHasBattlegroundFlag(Unit const* pTarget)
{
    return pTarget &&
       (pTarget->HasAura(AURA_WARSONG_FLAG) ||
        pTarget->HasAura(AURA_SILVERWING_FLAG));
}

static bool BattleBotNeedsFreedom(Unit const* pTarget)
{
    return pTarget &&
       (pTarget->HasUnitState(UNIT_STATE_ROOT) ||
        pTarget->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED));
}

static Unit* SelectMageCounterspellTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.mage.pCounterspell)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            !target->ToPlayer() ||
            !target->IsNonMeleeSpellCasted(false, false, true) ||
            !pAI->IsValidHostileTarget(target) ||
            pAI->IsBadPlayer(target) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.mage.pCounterspell))
            return;

        uint8 priority = target->GetClass() == CLASS_MAGE ? 6 :
            CombatBotBaseAI::IsHealerClass(target->GetClass()) ? 5 : 3;
        if (BattleBotHasBattlegroundFlag(target) && priority < 5)
            priority = 5;
        if (target == currentVictim)
            ++priority;

        float const distance = pAI->me->GetDistance(target);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = target;
            bestPriority = priority;
            bestDistance = distance;
        }
    };

    considerTarget(currentVictim);

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);
    for (Player* player : players)
        considerTarget(player);

    return bestTarget;
}

static Unit* SelectMagePolymorphTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.mage.pPolymorph)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);
    for (Player* player : players)
    {
        if (!player || player == pAI->me || player == currentVictim)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            BattleBotIsHardControlled(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !BattleBotCanUseCrowdControl(pAI, pAI->m_spells.mage.pPolymorph, player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.mage.pPolymorph))
            continue;

        bool const isMeleeThreat = CombatBotBaseAI::IsMeleeDamageClass(player->GetClass()) && player->GetCombatDistance(pAI->me) < 12.0f;
        Unit* victim = player->GetVictim();
        bool const isThreateningFriendly = victim && pAI->me->IsValidHelpfulTarget(victim);
        bool const isThreateningCarrier = isThreateningFriendly && BattleBotHasBattlegroundFlag(victim);
        bool const isThreateningHealer = isThreateningFriendly && CombatBotBaseAI::IsHealerClass(victim->GetClass());

        uint8 priority = 0;
        if (isMeleeThreat && victim == pAI->me)
            priority = 6;
        else if (isMeleeThreat && isThreateningCarrier)
            priority = 5;
        else if (isMeleeThreat && isThreateningHealer)
            priority = 4;
        else if (CombatBotBaseAI::IsHealerClass(player->GetClass()) && player->IsNonMeleeSpellCasted(false, false, true))
            priority = 3;
        else if (player->IsCaster() && player->GetVictim() == pAI->me)
            priority = 2;

        if (!priority)
            continue;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static bool BattleBotShouldUseMageBurst(BattleBotAI const* pAI, Unit* target)
{
    if (!pAI || !target)
        return false;

    if (BattleBotHasBattlegroundFlag(target))
        return true;

    if (target->GetHealthPercent() < 45.0f)
        return true;

    if (target->ToPlayer() &&
       (target->GetClass() == CLASS_MAGE || target->GetClass() == CLASS_WARLOCK || CombatBotBaseAI::IsHealerClass(target->GetClass())))
        return true;

    return pAI->GetAttackersInRangeCount(10.0f) > 0 && target->GetVictim() == pAI->me;
}

static bool BattleBotMageHasDamageSpellInRange(BattleBotAI const* pAI, Unit const* target)
{
    if (!pAI || !target)
        return false;

    auto isInRange = [&](SpellEntry const* spell)
    {
        return spell && spell->IsTargetInRange(pAI->me, target);
    };

    return isInRange(pAI->m_spells.mage.pPyroblast) ||
        isInRange(pAI->m_spells.mage.pFrostbolt) ||
        isInRange(pAI->m_spells.mage.pFireball) ||
        isInRange(pAI->m_spells.mage.pFireBlast) ||
       (target->GetHealthPercent() < 20.0f && isInRange(pAI->m_spells.mage.pScorch));
}

static bool BattleBotMageHasPhysicalPressure(BattleBotAI const* pAI, Unit const* currentVictim)
{
    if (!pAI)
        return false;

    if (currentVictim &&
        currentVictim->GetVictim() == pAI->me &&
        CombatBotBaseAI::IsPhysicalDamageClass(currentVictim->GetClass()))
        return true;

    for (Unit* attacker : pAI->me->GetAttackers())
    {
        if (!attacker ||
            attacker->GetVictim() != pAI->me ||
            !CombatBotBaseAI::IsMeleeDamageClass(attacker->GetClass()) ||
            pAI->me->GetCombatDistance(attacker) > 12.0f)
            continue;

        return true;
    }

    return false;
}

static Unit* SelectWarlockPetProtectTarget(BattleBotAI const* pAI)
{
    if (!pAI)
        return nullptr;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 12.0f);

    Player* bestTarget = nullptr;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (player->GetVictim() != pAI->me ||
            !CombatBotBaseAI::IsMeleeDamageClass(player->GetClass()) ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            BattleBotIsHardControlled(player) ||
            !pAI->me->IsWithinLOSInMap(player))
            continue;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || distance < bestDistance)
        {
            bestTarget = player;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectWarlockDeathCoilTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.warlock.pDeathCoil)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            !pAI->IsValidHostileTarget(target) ||
            pAI->IsBadPlayer(target) ||
            BattleBotIsHardControlled(target) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.warlock.pDeathCoil))
            return;

        bool const isCasting = target->IsNonMeleeSpellCasted(false, false, true);
        bool const isMeleeThreat = CombatBotBaseAI::IsMeleeDamageClass(target->GetClass()) && target->GetCombatDistance(pAI->me) < 10.0f;
        Unit* victim = target->GetVictim();
        bool const isThreateningFriendly = victim && pAI->me->IsValidHelpfulTarget(victim);
        bool const isThreateningCarrier = isThreateningFriendly && BattleBotHasBattlegroundFlag(victim);
        bool const isThreateningHealer = isThreateningFriendly && CombatBotBaseAI::IsHealerClass(victim->GetClass());

        uint8 priority = 0;
        if (isMeleeThreat && victim == pAI->me)
            priority = 8;
        else if (isCasting && target->GetClass() == CLASS_MAGE)
            priority = 7;
        else if (isCasting && CombatBotBaseAI::IsHealerClass(target->GetClass()))
            priority = 6;
        else if (isMeleeThreat && isThreateningCarrier)
            priority = 6;
        else if (isMeleeThreat && isThreateningHealer)
            priority = 5;
        else if (BattleBotHasBattlegroundFlag(target))
            priority = 4;
        else if (target == currentVictim && (isMeleeThreat || isCasting))
            priority = 3;

        if (!priority)
            return;

        float const distance = pAI->me->GetDistance(target);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = target;
            bestPriority = priority;
            bestDistance = distance;
        }
    };

    considerTarget(currentVictim);

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 20.0f);
    for (Player* player : players)
        considerTarget(player);

    return bestTarget;
}

static Unit* SelectWarlockFearTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.warlock.pFear)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            !pAI->IsValidHostileTarget(target) ||
            pAI->IsBadPlayer(target) ||
            BattleBotIsHardControlled(target) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !BattleBotCanUseCrowdControl(pAI, pAI->m_spells.warlock.pFear, target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.warlock.pFear))
            return;

        bool const isMeleeThreat = CombatBotBaseAI::IsMeleeDamageClass(target->GetClass()) && target->GetCombatDistance(pAI->me) < 12.0f;
        Unit* victim = target->GetVictim();
        bool const isThreateningFriendly = victim && pAI->me->IsValidHelpfulTarget(victim);
        bool const isThreateningCarrier = isThreateningFriendly && BattleBotHasBattlegroundFlag(victim);
        bool const isThreateningHealer = isThreateningFriendly && CombatBotBaseAI::IsHealerClass(victim->GetClass());

        uint8 priority = 0;
        if (isMeleeThreat && victim == pAI->me)
            priority = 7;
        else if (isMeleeThreat && isThreateningCarrier)
            priority = 6;
        else if (isMeleeThreat && isThreateningHealer)
            priority = 5;
        else if (CombatBotBaseAI::IsHealerClass(target->GetClass()) && target->IsNonMeleeSpellCasted(false, false, true))
            priority = 4;
        else if (target == currentVictim && isMeleeThreat)
            priority = 3;

        if (!priority)
            return;

        float const distance = pAI->me->GetDistance(target);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = target;
            bestPriority = priority;
            bestDistance = distance;
        }
    };

    considerTarget(currentVictim);

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 25.0f);
    for (Player* player : players)
        considerTarget(player);

    return bestTarget;
}

static Unit* SelectWarlockCurseOfTonguesTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.warlock.pCurseofTongues)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            !target->ToPlayer() ||
            !pAI->IsValidHostileTarget(target) ||
            pAI->IsBadPlayer(target) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.warlock.pCurseofTongues))
            return;

        uint8 priority = 0;
        if (target->GetClass() == CLASS_MAGE)
            priority = 5;
        else if (CombatBotBaseAI::IsHealerClass(target->GetClass()))
            priority = 4;
        else if (target->GetClass() == CLASS_WARLOCK || target->IsNonMeleeSpellCasted(false, false, true))
            priority = 3;
        else if (target == currentVictim && target->IsCaster())
            priority = 2;

        if (!priority)
            return;

        float const distance = pAI->me->GetDistance(target);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = target;
            bestPriority = priority;
            bestDistance = distance;
        }
    };

    considerTarget(currentVictim);

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);
    for (Player* player : players)
        considerTarget(player);

    return bestTarget;
}

static Unit* SelectWarlockCurseOfExhaustionTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.warlock.pCurseofExhaustion)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            !target->ToPlayer() ||
            !CombatBotBaseAI::IsMeleeDamageClass(target->GetClass()) ||
            !pAI->IsValidHostileTarget(target) ||
            pAI->IsBadPlayer(target) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.warlock.pCurseofExhaustion))
            return;

        Unit* victim = target->GetVictim();
        bool const isThreateningFriendly = victim && pAI->me->IsValidHelpfulTarget(victim);
        bool const isThreateningCarrier = isThreateningFriendly && BattleBotHasBattlegroundFlag(victim);
        bool const isThreateningHealer = isThreateningFriendly && CombatBotBaseAI::IsHealerClass(victim->GetClass());

        uint8 priority = 0;
        if (victim == pAI->me)
            priority = 5;
        else if (isThreateningCarrier)
            priority = 4;
        else if (isThreateningHealer)
            priority = 3;
        else if (target == currentVictim && pAI->me->GetDistance(target) < 20.0f)
            priority = 2;

        if (!priority)
            return;

        float const distance = pAI->me->GetDistance(target);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = target;
            bestPriority = priority;
            bestDistance = distance;
        }
    };

    considerTarget(currentVictim);

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);
    for (Player* player : players)
        considerTarget(player);

    return bestTarget;
}

static Unit* SelectPaladinEmergencyHealTarget(BattleBotAI const* pAI, SpellEntry const* pSpellEntry, float healthPercent)
{
    if (!pAI || !pSpellEntry)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestHealth = 100.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            target->GetHealthPercent() >= healthPercent ||
            !pAI->me->IsValidHelpfulTarget(target) ||
            !pAI->me->IsWithinDist(target, 30.0f) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pSpellEntry))
            return;

        float const targetHealth = target->GetHealthPercent();
        uint8 priority = 1;
        if (BattleBotHasBattlegroundFlag(target))
            priority = 6;
        else if (targetHealth < 20.0f)
            priority = 5;
        else if (pAI->GetIncomingdamage(target) > int32(target->GetMaxHealth() / 4))
            priority = 4;
        else if (target == pAI->me)
            priority = 3;
        else if (targetHealth < 40.0f)
            priority = 2;

        if (!bestTarget || priority > bestPriority || (priority == bestPriority && targetHealth < bestHealth))
        {
            bestTarget = target;
            bestPriority = priority;
            bestHealth = targetHealth;
        }
    };

    considerTarget(pAI->me);

    if (Group* group = pAI->me->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* member = itr->getSource())
                considerTarget(member);
        }
    }

    return bestTarget;
}

static Unit* SelectPaladinFreedomTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.paladin.pBlessingOfFreedom)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestHealth = 100.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            !BattleBotNeedsFreedom(target) ||
            !pAI->me->IsValidHelpfulTarget(target) ||
            !pAI->me->IsWithinDist(target, 30.0f) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.paladin.pBlessingOfFreedom))
            return;

        uint8 priority = 1;
        if (BattleBotHasBattlegroundFlag(target))
            priority = 6;
        else if (target != pAI->me && CombatBotBaseAI::IsPhysicalDamageClass(target->GetClass()))
            priority = 4;
        else if (target == pAI->me)
            priority = 3;

        float const health = target->GetHealthPercent();
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && health < bestHealth))
        {
            bestTarget = target;
            bestPriority = priority;
            bestHealth = health;
        }
    };

    considerTarget(pAI->me);

    if (Group* group = pAI->me->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* member = itr->getSource())
                considerTarget(member);
        }
    }

    return bestTarget;
}

static Unit* SelectPaladinHammerOfJusticeTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.paladin.pHammerOfJustice)
        return nullptr;

    Player* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    auto considerTarget = [&](Player* player)
    {
        if (!player ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            BattleBotIsHardControlled(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.paladin.pHammerOfJustice))
            return;

        bool const isCasting = player->IsNonMeleeSpellCasted(false, false, true);
        bool const isMeleeThreat = CombatBotBaseAI::IsMeleeDamageClass(player->GetClass()) && pAI->me->GetCombatDistance(player) < 10.0f;
        Unit* victim = player->GetVictim();
        bool const isThreateningFriendly = victim && pAI->me->IsValidHelpfulTarget(victim);
        bool const isThreateningCarrier = isThreateningFriendly && BattleBotHasBattlegroundFlag(victim);
        bool const isThreateningHealer = isThreateningFriendly && CombatBotBaseAI::IsHealerClass(victim->GetClass());
        bool const isThreateningSelf = victim == pAI->me && (pAI->GetRole() == ROLE_HEALER || BattleBotHasBattlegroundFlag(pAI->me));

        if (!isCasting && !(isMeleeThreat && (isThreateningCarrier || isThreateningHealer || isThreateningSelf)))
            return;

        uint8 priority = 0;
        if (isCasting)
        {
            priority = player->GetClass() == CLASS_MAGE ? 8 :
                CombatBotBaseAI::IsHealerClass(player->GetClass()) ? 7 : 4;
            if (BattleBotHasBattlegroundFlag(player) && priority < 6)
                priority = 6;
        }
        else if (isThreateningCarrier)
            priority = 6;
        else if (isThreateningHealer || isThreateningSelf)
            priority = 5;

        if (player == currentVictim)
            ++priority;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    };

    if (currentVictim)
        considerTarget(currentVictim->ToPlayer());

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 15.0f);
    for (Player* player : players)
        considerTarget(player);

    return bestTarget;
}

static Unit* SelectPaladinSacrificeTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.paladin.pBlessingOfSacrifice)
        return nullptr;

    Unit* bestTarget = nullptr;
    float bestHealth = 100.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            target == pAI->me ||
            !BattleBotHasBattlegroundFlag(target) ||
            !pAI->me->IsValidHelpfulTarget(target) ||
            !pAI->me->IsWithinDist(target, 30.0f) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.paladin.pBlessingOfSacrifice))
            return;

        if (!target->IsInCombat() && target->GetAttackers().empty() && target->GetHealthPercent() > 90.0f)
            return;

        float const health = target->GetHealthPercent();
        if (!bestTarget || health < bestHealth)
        {
            bestTarget = target;
            bestHealth = health;
        }
    };

    if (Group* group = pAI->me->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* member = itr->getSource())
                considerTarget(member);
        }
    }

    return bestTarget;
}

static Unit* SelectPaladinProtectionTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.paladin.pBlessingOfProtection)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestHealth = 100.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            BattleBotHasBattlegroundFlag(target) ||
            CombatBotBaseAI::IsPhysicalDamageClass(target->GetClass()) ||
            target->GetHealthPercent() > 70.0f ||
            !pAI->me->IsValidHelpfulTarget(target) ||
            !pAI->me->IsWithinDist(target, 30.0f) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.paladin.pBlessingOfProtection))
            return;

        bool meleeThreat = false;
        for (Unit* attacker : target->GetAttackers())
        {
            if (attacker && CombatBotBaseAI::IsMeleeDamageClass(attacker->GetClass()) && target->GetCombatDistance(attacker) < 10.0f)
            {
                meleeThreat = true;
                break;
            }
        }

        if (!meleeThreat && target->GetHealthPercent() > 35.0f)
            return;

        uint8 priority = CombatBotBaseAI::IsHealerClass(target->GetClass()) ? 5 : 3;
        if (target->GetHealthPercent() < 35.0f)
            ++priority;

        float const health = target->GetHealthPercent();
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && health < bestHealth))
        {
            bestTarget = target;
            bestPriority = priority;
            bestHealth = health;
        }
    };

    considerTarget(pAI->me);

    if (Group* group = pAI->me->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* member = itr->getSource())
                considerTarget(member);
        }
    }

    return bestTarget;
}

static Unit* SelectPriestPowerWordShieldTarget(BattleBotAI const* pAI)
{
    if (!pAI || !pAI->m_spells.priest.pPowerWordShield)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestHealth = 100.0f;

    auto considerTarget = [&](Unit* target)
    {
        if (!target ||
            !pAI->me->IsValidHelpfulTarget(target) ||
            !pAI->me->IsWithinDist(target, 30.0f) ||
            !pAI->me->IsWithinLOSInMap(target) ||
            !pAI->CanTryToCastSpell(target, pAI->m_spells.priest.pPowerWordShield))
            return;

        bool const isSelf = target == pAI->me;
        bool const hasFlag = BattleBotHasBattlegroundFlag(target);
        int32 const incomingDamage = pAI->GetIncomingdamage(target);
        float const healthPercent = target->GetHealthPercent();

        uint8 priority = 0;
        if (hasFlag && (target->IsInCombat() || healthPercent < 95.0f || incomingDamage > 0))
            priority = 6;
        else if (incomingDamage > int32(target->GetMaxHealth() / 4))
            priority = 5;
        else if (healthPercent < 45.0f)
            priority = 4;
        else if (pAI->GetRole() == ROLE_HEALER && incomingDamage > 0)
            priority = 3;
        else if (pAI->GetRole() == ROLE_HEALER && healthPercent < 80.0f)
            priority = 2;
        else if (isSelf && (pAI->GetAttackersInRangeCount(10.0f) > 0 || healthPercent < 85.0f))
            priority = 1;

        if (!priority)
            return;

        if (!bestTarget || priority > bestPriority || (priority == bestPriority && healthPercent < bestHealth))
        {
            bestTarget = target;
            bestPriority = priority;
            bestHealth = healthPercent;
        }
    };

    considerTarget(pAI->me);

    if (Group* group = pAI->me->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* member = itr->getSource())
                considerTarget(member);
        }
    }

    return bestTarget;
}

static Unit* SelectPriestSilenceTarget(BattleBotAI const* pAI, Unit* currentVictim)
{
    if (!pAI || !pAI->m_spells.priest.pSilence)
        return nullptr;

    Player* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    auto considerTarget = [&](Player* player)
    {
        if (!player ||
            !player->IsNonMeleeSpellCasted(false, false, true) ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            BattleBotIsHardControlled(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.priest.pSilence))
            return;

        uint8 priority = player->GetClass() == CLASS_MAGE ? 6 :
            CombatBotBaseAI::IsHealerClass(player->GetClass()) ? 5 : 3;
        if (player == currentVictim)
            ++priority;
        if (BattleBotHasBattlegroundFlag(player) && priority < 5)
            priority = 5;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    };

    if (currentVictim)
        considerTarget(currentVictim->ToPlayer());

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);
    for (Player* player : players)
        considerTarget(player);

    return bestTarget;
}

static Unit* SelectRogueInterruptTarget(BattleBotAI const* pAI, SpellEntry const* pSpellEntry)
{
    if (!pAI || !pSpellEntry)
        return nullptr;

    Unit* currentVictim = pAI->me->GetVictim();
    if (currentVictim &&
        currentVictim->IsNonMeleeSpellCasted(false, false, true) &&
        pAI->CanTryToCastSpell(currentVictim, pSpellEntry))
        return currentVictim;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 10.0f);

    Player* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!player->IsNonMeleeSpellCasted(false, false, true) ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pSpellEntry))
            continue;

        uint8 priority = CombatBotBaseAI::IsHealerClass(player->GetClass()) ? 3 : 2;
        if (player == currentVictim)
            priority = 4;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectRogueBlindTarget(BattleBotAI const* pAI, Unit* pVictim, bool preferObjectiveTarget)
{
    if (!pAI || !pAI->m_spells.rogue.pBlind)
        return nullptr;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, preferObjectiveTarget ? 15.0f : 12.0f);
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !BattleBotCanUseCrowdControl(pAI, pAI->m_spells.rogue.pBlind, player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.rogue.pBlind))
            continue;

        uint8 priority = 1;
        if (preferObjectiveTarget)
        {
            if (player == pVictim)
                priority = 5;
            else if (player->GetVictim() == pAI->me)
                priority = 4;
            else if (player->GetVictim() && player->GetVictim()->IsFriendlyTo(pAI->me))
                priority = 3;
            else
                priority = 2;
        }
        else
        {
            if (player == pVictim)
                continue;

            if (player->GetVictim() == pAI->me)
                priority = 3;
            else if (player->GetVictim() && player->GetVictim()->IsFriendlyTo(pAI->me))
                priority = 2;
        }

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static bool TryRogueEviscerate(BattleBotAI* pAI, Unit* pVictim, bool useColdBlood)
{
    if (!pAI || !pVictim || !pAI->m_spells.rogue.pEviscerate)
        return false;

    if (useColdBlood &&
        pAI->m_spells.rogue.pColdBlood &&
        pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.rogue.pColdBlood))
    {
        if (pAI->DoCastSpell(pAI->me, pAI->m_spells.rogue.pColdBlood) == SPELL_CAST_OK)
            return true;
    }

    if (pAI->CanTryToCastSpell(pVictim, pAI->m_spells.rogue.pEviscerate))
    {
        if (pAI->DoCastSpell(pVictim, pAI->m_spells.rogue.pEviscerate) == SPELL_CAST_OK)
            return true;
    }

    return false;
}

static bool BattleBotEnterDruidCombatForm(BattleBotAI* pAI)
{
    if (!pAI || pAI->me->GetShapeshiftForm() != FORM_NONE)
        return false;

    if (pAI->GetRole() == ROLE_TANK)
    {
        if (pAI->m_spells.druid.pBearForm &&
            pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.druid.pBearForm))
        {
            if (pAI->DoCastSpell(pAI->me, pAI->m_spells.druid.pBearForm) == SPELL_CAST_OK)
                return true;
        }
    }
    else if (pAI->GetRole() == ROLE_MELEE_DPS)
    {
        if (pAI->m_spells.druid.pCatForm &&
            !pAI->me->IsMounted() &&
            pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.druid.pCatForm))
        {
            if (pAI->DoCastSpell(pAI->me, pAI->m_spells.druid.pCatForm) == SPELL_CAST_OK)
                return true;
        }

        if (pAI->m_spells.druid.pBearForm &&
            pAI->me->GetHealthPercent() < 35.0f &&
            pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.druid.pBearForm))
        {
            if (pAI->DoCastSpell(pAI->me, pAI->m_spells.druid.pBearForm) == SPELL_CAST_OK)
                return true;
        }
    }
    else if (pAI->GetRole() == ROLE_RANGE_DPS)
    {
        if (pAI->m_spells.druid.pMoonkinForm &&
            pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.druid.pMoonkinForm))
        {
            if (pAI->DoCastSpell(pAI->me, pAI->m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return true;
        }
    }

    return false;
}

static Unit* SelectDruidFaerieFireTarget(BattleBotAI const* pAI, SpellEntry const* pSpellEntry)
{
    if (!pAI || !pSpellEntry)
        return nullptr;

    Unit* currentVictim = pAI->me->GetVictim();
    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pSpellEntry))
            continue;

        uint8 priority = 1;
        if (player->HasAura(AURA_WARSONG_FLAG) || player->HasAura(AURA_SILVERWING_FLAG))
            priority = 4;
        else if (player->GetClass() == CLASS_ROGUE || player->GetClass() == CLASS_DRUID)
            priority = 3;
        else if (player == currentVictim)
            priority = 2;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    if (!bestTarget && currentVictim &&
        pAI->IsValidHostileTarget(currentVictim) &&
        pAI->CanTryToCastSpell(currentVictim, pSpellEntry))
        return currentVictim;

    return bestTarget;
}

static Unit* SelectDruidEntanglingRootsTarget(BattleBotAI const* pAI, SpellEntry const* pSpellEntry)
{
    if (!pAI || !pSpellEntry)
        return nullptr;

    Unit* currentVictim = pAI->me->GetVictim();
    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!CombatBotBaseAI::IsMeleeDamageClass(player->GetClass()) ||
            BattleBotHasEntanglingRoots(player) ||
            player->CanReachWithMeleeAutoAttack(pAI->me) ||
            !pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pSpellEntry))
            continue;

        uint8 priority = 1;
        Unit* victim = player->GetVictim();
        if (victim == pAI->me)
            priority = 5;
        else if (victim && victim->ToPlayer() && victim->IsFriendlyTo(pAI->me) &&
                 CombatBotBaseAI::IsHealerClass(victim->GetClass()))
            priority = 4;
        else if (victim && victim->IsFriendlyTo(pAI->me))
            priority = 3;
        else if (player == currentVictim)
            priority = 2;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectDruidInterruptTarget(BattleBotAI const* pAI, SpellEntry const* pSpellEntry)
{
    if (!pAI || !pSpellEntry)
        return nullptr;

    Unit* currentVictim = pAI->me->GetVictim();
    if (currentVictim &&
        currentVictim->IsNonMeleeSpellCasted(false, false, true) &&
        pAI->CanTryToCastSpell(currentVictim, pSpellEntry))
        return currentVictim;

    Unit* bestTarget = nullptr;
    uint8 bestPriority = 0;
    float bestDistance = 0.0f;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 25.0f);
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (!pAI->IsValidHostileTarget(player) ||
            pAI->IsBadPlayer(player) ||
            !player->IsNonMeleeSpellCasted(false, false, true) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pSpellEntry))
            continue;

        uint8 priority = CombatBotBaseAI::IsHealerClass(player->GetClass()) ? 3 : 2;
        if (player->HasAura(AURA_WARSONG_FLAG) || player->HasAura(AURA_SILVERWING_FLAG))
            priority = 4;

        float const distance = pAI->me->GetDistance(player);
        if (!bestTarget || priority > bestPriority || (priority == bestPriority && distance < bestDistance))
        {
            bestTarget = player;
            bestPriority = priority;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

static Unit* SelectDruidInnervateTarget(BattleBotAI* pAI)
{
    if (!pAI || !pAI->m_spells.druid.pInnervate)
        return nullptr;

    if (pAI->me->GetPowerType() == POWER_MANA &&
        pAI->me->GetPowerPercent(POWER_MANA) < 15.0f &&
        pAI->me->GetHealthPercent() > 35.0f &&
        pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.druid.pInnervate))
        return pAI->me;

    std::list<Player*> players;
    pAI->me->GetAlivePlayerListInRange(pAI->me, players, 30.0f);

    Player* bestTarget = nullptr;
    float bestMana = 100.0f;
    for (Player* player : players)
    {
        if (!player || player == pAI->me)
            continue;

        if (player->GetTeam() != pAI->me->GetTeam() ||
            player->GetPowerType() != POWER_MANA ||
            !CombatBotBaseAI::IsHealerClass(player->GetClass()) ||
            !pAI->me->IsWithinLOSInMap(player) ||
            !pAI->CanTryToCastSpell(player, pAI->m_spells.druid.pInnervate))
            continue;

        float const manaPercent = player->GetPowerPercent(POWER_MANA);
        if (manaPercent < 20.0f && (!bestTarget || manaPercent < bestMana))
        {
            bestTarget = player;
            bestMana = manaPercent;
        }
    }

    return bestTarget;
}

static bool TryDruidNatureSwiftnessHeal(BattleBotAI* pAI)
{
    if (!pAI || !pAI->m_spells.druid.pNaturesSwiftness)
        return false;

    Unit* target = pAI->SelectHealTarget(35.0f, 50.0f);
    if (!target)
        return false;

    if (pAI->CanTryToCastSpell(pAI->me, pAI->m_spells.druid.pNaturesSwiftness))
    {
        if (pAI->DoCastSpell(pAI->me, pAI->m_spells.druid.pNaturesSwiftness) == SPELL_CAST_OK)
        {
            pAI->HealInjuredTargetDirect(target);
            return true;
        }
    }

    return false;
}

uint32 BattleBotAI::GetMountSpellId() const
{
    if (me->GetLevel() >= 60)
    {
        if (me->GetClass() == CLASS_PALADIN)
            return BB_SPELL_MOUNT_60_PALADIN;
        if (me->GetClass() == CLASS_WARLOCK)
            return BB_SPELL_MOUNT_60_WARLOCK;

        switch (me->GetRace())
        {
            case RACE_HUMAN:
                return BB_SPELL_MOUNT_60_HUMAN;
            case RACE_NIGHTELF:
                return BB_SPELL_MOUNT_60_NELF;
            case RACE_DWARF:
                return BB_SPELL_MOUNT_60_DWARF;
            case RACE_GNOME:
                return BB_SPELL_MOUNT_60_GNOME;
            case RACE_TROLL:
                return BB_SPELL_MOUNT_60_TROLL;
            case RACE_ORC:
                return BB_SPELL_MOUNT_60_ORC;
            case RACE_TAUREN:
                return BB_SPELL_MOUNT_60_TAUREN;
            case RACE_UNDEAD:
                return BB_SPELL_MOUNT_60_UNDEAD;
        }
    }
    else if (me->GetLevel() >= 40)
    {
        if (me->GetClass() == CLASS_PALADIN)
            return BB_SPELL_MOUNT_40_PALADIN;
        if (me->GetClass() == CLASS_WARLOCK)
            return BB_SPELL_MOUNT_40_WARLOCK;

        switch (me->GetRace())
        {
            case RACE_HUMAN:
                return BB_SPELL_MOUNT_40_HUMAN;
            case RACE_NIGHTELF:
                return BB_SPELL_MOUNT_40_NELF;
            case RACE_DWARF:
                return BB_SPELL_MOUNT_40_DWARF;
            case RACE_GNOME:
                return BB_SPELL_MOUNT_40_GNOME;
            case RACE_TROLL:
                return BB_SPELL_MOUNT_40_TROLL;
            case RACE_ORC:
                return BB_SPELL_MOUNT_40_ORC;
            case RACE_TAUREN:
                return BB_SPELL_MOUNT_40_TAUREN;
            case RACE_UNDEAD:
                return BB_SPELL_MOUNT_40_UNDEAD;
        }
    }

    return 0;
}

bool BattleBotAI::UseMount()
{
    if (me->IsMounted())
        return false;

    if (BattleGround* bg = me->GetBattleGround())
    {
        if (bg->GetStatus() == STATUS_WAIT_JOIN)
            return false;
        // Don't strip shapeshift form and then fail to mount because we're moving in AV/WSG.
        if (me->IsMoving() && (bg->GetTypeID() == BATTLEGROUND_AV || bg->GetTypeID() == BATTLEGROUND_WS))
            return false;
    }

    if (me->HasAura(AURA_WARSONG_FLAG) ||
        me->HasAura(AURA_SILVERWING_FLAG))
        return false;

    // Druid: remove shapeshift before checking display ID and mounting,
    // but only after all blocking conditions have already passed.
    if (me->GetClass() == CLASS_DRUID && me->GetShapeshiftForm() != FORM_NONE)
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);

    if (me->GetDisplayId() != me->GetNativeDisplayId())
        return false;

    if (me->HasAura(SPELL_AURA_MOD_STEALTH))
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

    uint32 spellId = GetMountSpellId();
    if (!spellId)
        return false;

    if (me->IsMoving())
    {
        ClearPath();
        StopMoving();
    }

    if (me->CastSpell(me, spellId, false) == SPELL_CAST_OK)
        return true;

    return false;
}

bool BattleBotAI::DrinkAndEat()
{
    if (m_isBuffing)
        return false;

    if (me->IsMounted())
        return false;

    if (me->GetVictim())
        return false;

    bool const isGuard = BattleBotIsWSGHomeGuardCandidate(this) || BattleBotIsABGuardingOwnedNode(this);
    BattleGround* bg = me->GetBattleGround();
    bool const isWaiting = bg && bg->GetStatus() == STATUS_WAIT_JOIN;
    float const recoveryThreshold = isWaiting ? 100.0f : (isGuard ? 90.0f : 60.0f);
    bool const needToEat = me->GetHealthPercent() < recoveryThreshold;
    bool needToDrink = (me->GetPowerType() == POWER_MANA) && (me->GetPowerPercent(POWER_MANA) < recoveryThreshold);

    if (needToDrink &&
        me->GetClass() == CLASS_DRUID &&
        me->GetShapeshiftForm() != FORM_NONE &&
       (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK))
        needToDrink = false;

    if (!needToEat && !needToDrink)
        return false;

    // Do not stop while carrying flag.
    if (me->HasAura(AURA_WARSONG_FLAG) ||
        me->HasAura(AURA_SILVERWING_FLAG))
        return false;

    bool const isEating = me->HasAura(BB_SPELL_FOOD);
    bool const isDrinking = me->HasAura(BB_SPELL_DRINK);

    if (!isEating && needToEat)
    {
        if (me->GetClass() == CLASS_DRUID && me->GetShapeshiftForm() != FORM_NONE)
        {
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            return true;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            ClearPath();
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(BB_SPELL_FOOD))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(*pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {
        if (me->GetClass() == CLASS_DRUID && me->GetShapeshiftForm() != FORM_NONE)
        {
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            return true;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {
            ClearPath();
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(BB_SPELL_DRINK))
        {
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(*pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

float BattleBotAI::GetMaxAggroDistanceForMap() const
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return 50.0f;

    // Guard bots and bots holding a capture keep full aggro range.
    if (m_avAssignedGY != 0 || BattleBotIsInAVGyCaptureHold(this))
        return 50.0f;

    // Non-aggressive travel: no new combat initiation outside objective radius.
    if (!m_avAggressiveTravelCombat && !BattleBotIsNearAVFlag(this, AV_FLAG_DEFENSE_RADIUS))
        return 0.0f;

    return 20.0f;
}

bool BattleBotAI::ShouldUseAVOpeningPassiveCombat() const
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV ||
        bg->GetStatus() != STATUS_IN_PROGRESS ||
        me->IsInCombat() ||
        m_avAssignedGY != 0 ||
        BattleBotIsNearAVFlag(this, 10.0f) ||
        BattleBotIsInAVGyCaptureHold(this))
        return false;

    // Mode with no passive opening (e.g. Native) never restricts target selection here.
    if (m_avOpeningPassiveMinutes == 0)
        return false;

    uint32 const t = bg->GetStartTime();
    uint32 const passiveMs = m_avOpeningPassiveMinutes * MINUTE * IN_MILLISECONDS;

    // Passive opening phase: only target objective NPCs or retaliators.
    if (t <= passiveMs)
        return true;

    // Post-opening: still passive unless within objective radius.
    return !BattleBotIsNearAVFlag(this, AV_FLAG_DEFENSE_RADIUS);
}

Unit* BattleBotAI::SelectAVOpeningObjectiveNpcTarget(Unit* pExcept) const
{
    float const range = 20.0f;
    std::list<Unit*> targets;

    CellPair pair(MaNGOS::ComputeCellPair(me->GetPositionX(), me->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck check(me, me, range);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(targets, check);
    TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck>, WorldTypeMapContainer> worldSearcher(searcher);
    TypeContainerVisitor<MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck>, GridTypeMapContainer> gridSearcher(searcher);

    cell.Visit(pair, worldSearcher, *me->GetMap(), *me, range);
    cell.Visit(pair, gridSearcher, *me->GetMap(), *me, range);

    Unit* bestTarget = nullptr;
    float bestDistance = FLT_MAX;
    for (Unit* target : targets)
    {
        if (target == pExcept)
            continue;

        if (!target->ToCreature())
            continue;

        if (!IsValidHostileTarget(target) || IsBadPlayer(target))
            continue;

        Unit* victim = target->GetVictim();
        if (!victim || !victim->ToPlayer() || !victim->IsFriendlyTo(me))
            continue;

        if (!me->IsWithinLOSInMap(target))
            continue;

        float const distance = me->GetDistance(target);
        if (!bestTarget || distance < bestDistance)
        {
            bestTarget = target;
            bestDistance = distance;
        }
    }

    return bestTarget;
}

Unit* BattleBotAI::SelectAVOpeningRetaliationTarget(Unit* pExcept) const
{
    HostileReference* pReference = me->GetHostileRefManager().getFirst();
    while (pReference)
    {
        if (Unit* pTarget = pReference->getSourceUnit())
        {
            if (pTarget != pExcept &&
                pTarget->GetVictim() == me &&
                IsValidHostileTarget(pTarget) &&
                !IsBadPlayer(pTarget) &&
                !BattleBotHasBlind(pTarget) &&
                me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL) &&
                me->GetDistanceZ(pTarget) < 10.0f &&
                me->IsWithinLOSInMap(pTarget))
                return pTarget;
        }

        pReference = pReference->next();
    }

    return nullptr;
}

uint32 BattleBotAI::GetAVPhase1WaveDelay() const
{
    uint32 const guid = me->GetObjectGuid().GetCounter();
    uint32 const bucket = guid % 10;

    if (bucket < 5)
        return 0;

    return 30;
}

bool BattleBotAI::ShouldWaitForAVPhase1Wave()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV)
        return false;

    if (bg->GetStatus() == STATUS_WAIT_JOIN)
    {
        m_avStartWaveInitialized = false;
        m_avStartWaveBgInstance = 0;
        m_avStartWaveReleaseTime = 0;
        return false;
    }

    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    uint32 const instanceId = bg->GetInstanceID();
    if (!m_avStartWaveInitialized || m_avStartWaveBgInstance != instanceId)
    {
        m_avStartWaveInitialized = true;
        m_avStartWaveBgInstance = instanceId;

        // GetStartTime includes the preparation phase. Skip the wave delay for late joins.
        if (bg->GetStartTime() > 3 * MINUTE * IN_MILLISECONDS)
            m_avStartWaveReleaseTime = sWorld.GetGameTime();
        else
            m_avStartWaveReleaseTime = sWorld.GetGameTime() + GetAVPhase1WaveDelay();
    }

    return sWorld.GetGameTime() < m_avStartWaveReleaseTime;
}

bool BattleBotAI::UpdateAVPhase1WaitingAI()
{
    if (!ShouldWaitForAVPhase1Wave())
        return false;

    ClearPath();

    if (me->GetVictim())
        me->AttackStop(false);

    if (me->IsMoving())
        StopMoving();

    return true;
}

bool BattleBotAI::AttackStart(Unit* pVictim)
{
    if (BattleBotHasBlind(pVictim))
        return false;

    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    if (me->Attack(pVictim, true))
    {
        ClearPath();

        if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
            IsRangedDamageClass(me->GetClass()) &&
            me->GetPowerPercent(POWER_MANA) > 10.0f &&
            me->GetCombatDistance(pVictim) > 8.0f)
            me->SetCasterChaseDistance(25.0f);
        else if (me->HasDistanceCasterMovement())
            me->SetCasterChaseDistance(0.0f);

        me->GetMotionMaster()->MoveChase(pVictim, 1.0f, m_role == ROLE_MELEE_DPS ? 3.0f : 0.0f);
        return true;
    }

    return false;
}

bool BattleBotAI::ShouldIgnoreCombat() const
{
    if (m_battlegroundId == BATTLEGROUND_QUEUE_WS && !me->IsRooted() &&
       (me->HasAura(AURA_SILVERWING_FLAG) || me->HasAura(AURA_WARSONG_FLAG)))
        return true;
    return false;
}

Unit* BattleBotAI::SelectHealerOffensiveTarget() const
{
    // Find an enemy within 30m that a nearby ally is already fighting.
    // Keeps healer close to the allied skirmish rather than wandering.
    float const maxRange = 30.0f;
    Unit* pBestTarget = nullptr;
    float bestDistance = maxRange + 1.0f;

    std::list<Player*> players;
    me->GetAlivePlayerListInRange(me, players, maxRange);

    for (Player* pAlly : players)
    {
        if (pAlly == me || me->GetTeam() != pAlly->GetTeam())
            continue;

        Unit* pAllyVictim = pAlly->GetVictim();
        if (!pAllyVictim || !IsValidHostileTarget(pAllyVictim))
            continue;

        float const dist = me->GetDistance(pAllyVictim);
        if (dist > maxRange)
            continue;

        if (BattleBotHasBlind(pAllyVictim) || BattleBotHasEntanglingRoots(pAllyVictim))
            continue;

        if (!me->IsWithinLOSInMap(pAllyVictim))
            continue;

        if (dist < bestDistance)
        {
            bestDistance = dist;
            pBestTarget = pAllyVictim;
        }
    }
    return pBestTarget;
}

Unit* BattleBotAI::SelectAttackTarget(Unit* pExcept) const
{
    // Ignore attackers while carrying flag, just keep running.
    if (ShouldIgnoreCombat())
        return nullptr;

    if (ShouldUseAVOpeningPassiveCombat())
    {
        if (Unit* pObjectiveNpcTarget = SelectAVOpeningObjectiveNpcTarget(pExcept))
            return pObjectiveNpcTarget;

        return SelectAVOpeningRetaliationTarget(pExcept);
    }

    Unit* pRootedFallback = nullptr;
    float rootedFallbackDistance = FLT_MAX;

    if (Unit* pFlagDefenseTarget = BattleBotSelectABFlagDefenseTarget(this, pExcept))
    {
        if (!BattleBotHasBlind(pFlagDefenseTarget) && !BattleBotHasEntanglingRoots(pFlagDefenseTarget))
            return pFlagDefenseTarget;

        if (!BattleBotHasBlind(pFlagDefenseTarget))
        {
            pRootedFallback = pFlagDefenseTarget;
            rootedFallbackDistance = me->GetDistance(pFlagDefenseTarget);
        }
    }

    if (Unit* pFlagDefenseTarget = BattleBotSelectAVFlagDefenseTarget(this, pExcept))
    {
        if (!BattleBotHasBlind(pFlagDefenseTarget) && !BattleBotHasEntanglingRoots(pFlagDefenseTarget))
            return pFlagDefenseTarget;

        if (!BattleBotHasBlind(pFlagDefenseTarget))
        {
            float const distance = me->GetDistance(pFlagDefenseTarget);
            if (!pRootedFallback || distance < rootedFallbackDistance)
            {
                pRootedFallback = pFlagDefenseTarget;
                rootedFallbackDistance = distance;
            }
        }
    }

    // 1. Check units we are currently in combat with.

    std::list<Unit*> targets;
    std::list<Unit*> rootedTargets;
    HostileReference* pReference = me->GetHostileRefManager().getFirst();

    while (pReference)
    {
        if (Unit* pTarget = pReference->getSourceUnit())
        {
            if (pTarget != pExcept &&
                IsValidHostileTarget(pTarget) &&
                !IsBadPlayer(pTarget) &&
                me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            {
                if (BattleBotHasBlind(pTarget))
                {
                    pReference = pReference->next();
                    continue;
                }

                if (BattleBotHasEntanglingRoots(pTarget))
                {
                    rootedTargets.push_back(pTarget);
                    pReference = pReference->next();
                    continue;
                }

                if (me->GetTeam() == HORDE)
                {
                    if (pTarget->HasAura(AURA_WARSONG_FLAG))
                        return pTarget;
                }
                else
                {
                    if (pTarget->HasAura(AURA_SILVERWING_FLAG))
                        return pTarget;
                }

                targets.push_back(pTarget);
            }
        }
        pReference = pReference->next();
    }

    if (!targets.empty())
    {
        targets.sort([this](Unit* pUnit1, const Unit* pUnit2)
        {
            return me->GetDistance(pUnit1) < me->GetDistance(pUnit2);
        });

        return *targets.begin();
    }

    if (!rootedTargets.empty())
    {
        rootedTargets.sort([this](Unit* pUnit1, const Unit* pUnit2)
        {
            return me->GetDistance(pUnit1) < me->GetDistance(pUnit2);
        });

        float const distance = me->GetDistance(*rootedTargets.begin());
        if (!pRootedFallback || distance < rootedFallbackDistance)
        {
            pRootedFallback = *rootedTargets.begin();
            rootedFallbackDistance = distance;
        }
    }

    // 2. Find enemy player in range.

    std::list<Player*> players;
    me->GetAlivePlayerListInRange(me, players, VISIBILITY_DISTANCE_NORMAL);
    float const maxAggroDistance = GetMaxAggroDistanceForMap();

    Player* bestStep2Target = nullptr;
    uint8 bestStep2Priority = 0;
    float bestStep2Distance = FLT_MAX;

    for (const auto& pTarget : players)
    {
        if (pTarget == pExcept)
            continue;

        if (!IsValidHostileTarget(pTarget) || IsBadPlayer(pTarget))
            continue;

        if (BattleBotHasBlind(pTarget))
            continue;

        if (!BattleBotHasEntanglingRoots(pTarget))
        {
            if (me->GetTeam() == HORDE)
            {
                if (pTarget->HasAura(AURA_WARSONG_FLAG))
                    return pTarget;
            }
            else
            {
                if (pTarget->HasAura(AURA_SILVERWING_FLAG))
                    return pTarget;
            }
        }

        // Aggro weak enemies from further away.
        uint32 const aggroDistance = me->GetHealth() > pTarget->GetHealth() ? maxAggroDistance : 20.0f;
        if (!me->IsWithinDist(pTarget, aggroDistance))
            continue;

        if (me->GetDistanceZ(pTarget) > 10.0f)
            continue;

        if (!me->IsWithinLOSInMap(pTarget))
            continue;

        if (BattleBotHasEntanglingRoots(pTarget))
        {
            float const distance = me->GetDistance(pTarget);
            if (!pRootedFallback || distance < rootedFallbackDistance)
            {
                pRootedFallback = pTarget;
                rootedFallbackDistance = distance;
            }
            continue;
        }

        // Priority: healer > attacking a friendly > low HP > closest
        uint8 priority = 1;
        if (IsHealerClass(pTarget->GetClass()))
            priority = 4;
        else if (pTarget->GetVictim() && me->IsValidHelpfulTarget(pTarget->GetVictim()))
            priority = 3;
        else if (pTarget->GetHealthPercent() < 30.0f)
            priority = 2;

        float const distance = me->GetDistance(pTarget);
        if (!bestStep2Target || priority > bestStep2Priority || (priority == bestStep2Priority && distance < bestStep2Distance))
        {
            bestStep2Target = pTarget;
            bestStep2Priority = priority;
            bestStep2Distance = distance;
        }
    }

    if (bestStep2Target)
        return bestStep2Target;

    // 3. Check party attackers.

    if (Group* pGroup = me->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {
                if (pMember == me)
                    continue;

                if (me->GetDistance(pMember) > 30.0f)
                    continue;

                if (Unit* pAttacker = pMember->GetAttackerForHelper())
                {
                    if (pAttacker != pExcept &&
                        IsValidHostileTarget(pAttacker) &&
                        !IsBadPlayer(pAttacker) &&
                        me->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) &&
                        me->GetDistanceZ(pAttacker) < 10.0f &&
                        me->IsWithinLOSInMap(pAttacker) &&
                        !BattleBotHasBlind(pAttacker))
                    {
                        if (BattleBotHasEntanglingRoots(pAttacker))
                        {
                            float const distance = me->GetDistance(pAttacker);
                            if (!pRootedFallback || distance < rootedFallbackDistance)
                            {
                                pRootedFallback = pAttacker;
                                rootedFallbackDistance = distance;
                            }
                            continue;
                        }

                        return pAttacker;
                    }
                }
            }
        }
    }

    if (pRootedFallback)
        return pRootedFallback;

    // Korrak the Bloodrager (12159): neutral hostile to both factions, spawns ~2h into AV.
    if (me->GetBattleGround() && me->GetBattleGround()->GetTypeID() == BATTLEGROUND_AV)
        if (Creature* pKorrak = me->FindNearestCreature(12159, 200.0f, true))
            if (me->IsValidAttackTarget(pKorrak))
                return pKorrak;

    return nullptr;
}

Unit* BattleBotAI::SelectFollowTarget() const
{
    if (me->HasAura(AURA_WARSONG_FLAG) ||
        me->HasAura(AURA_SILVERWING_FLAG))
        return nullptr;

    if (BattleBotIsWSGHomeGuardCandidate(this))
        return nullptr;

    BattleGround* bg = me->GetBattleGround();
    bool const isWSG = bg && bg->GetTypeID() == BATTLEGROUND_WS;
    bool const isAB = bg && bg->GetTypeID() == BATTLEGROUND_AB;

    if (isAB && m_role == ROLE_HEALER)
        return nullptr;

    std::list<Player*> players;
    me->GetAlivePlayerListInRange(me, players, VISIBILITY_DISTANCE_NORMAL);
    Player* pHealerFollowTarget = nullptr;

    for (const auto& pTarget : players)
    {
        if (pTarget == me)
            continue;

        if (me->GetTeam() != pTarget->GetTeam())
            continue;

        if (pTarget->IsGameMaster())
            continue;

        if (me->GetTeam() == ALLIANCE)
        {
            if (pTarget->HasAura(AURA_WARSONG_FLAG))
                return pTarget;
        }
        else
        {
            if (pTarget->HasAura(AURA_SILVERWING_FLAG))
                return pTarget;
        }

        if (isWSG)
        {
            if (BattleBotAI* pTargetAI = dynamic_cast<BattleBotAI*>(pTarget->AI()))
            {
                if (BattleBotIsWSGHomeGuardCandidate(pTargetAI))
                    continue;
            }
        }

        if (m_role == ROLE_HEALER &&
           !IsHealerClass(pTarget->GetClass()) &&
           !IsStealthClass(pTarget->GetClass()) &&
           (pTarget->IsMounted() == me->IsMounted()) &&
           (me->GetDistance2d(pTarget) <= 20.0f) &&
           (me->GetDistanceZ(pTarget) <= 3.0f))
        {
            if (!pHealerFollowTarget || me->GetDistance(pTarget) < me->GetDistance(pHealerFollowTarget))
                pHealerFollowTarget = pTarget;
        }
    }

    return pHealerFollowTarget;
}

void BattleBotAI::DoGraveyardJump()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_WS)
        return;

    m_doingGraveyardJump = true;
    uint32 timeOffset = 0;
    std::vector<RecordedMovementPacket>* pPath = me->GetTeam() == HORDE ? &vHordeGraveyardJumpPath : &vAllianceGraveyardJumpPath;
    for (uint32 i = 0; i < (*pPath).size(); i++)
    {
        RecordedMovementPacket* point = &((*pPath)[i]);
        Player* pBot = me;
        BattleBotAI* pAI = this;
        bool isLast = i == (*pPath).size() - 1;
        timeOffset += point->timeDiff;
        me->m_Events.AddLambdaEventAtOffset([pBot, pAI, point, isLast]
        {
            if (!pBot->HasUnitState(UNIT_STATE_NO_FREE_MOVE))
            {
                pBot->SetUnitMovementFlags(point->moveFlags);
                pBot->Relocate(point->position.x, point->position.y, point->position.z, point->position.o);
                pBot->SendMovementPacket(point->opcode, false);
            }

            if (isLast)
                pAI->m_doingGraveyardJump = false;
        }, timeOffset);
    }
}

void BattleBotAI::StopMoving()
{
    me->StopMoving();
    me->GetMotionMaster()->Clear();
    me->GetMotionMaster()->MoveIdle();
}

void BattleBotAI::OnPacketReceived(WorldPacket const* packet)
{
    //printf("Bot received %s\n", LookupOpcodeName(packet->GetOpcode()));
    switch (packet->GetOpcode())
    {
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_4_2
        case MSG_PVP_LOG_DATA:
        {
            if (!me)
                return;

            uint8 ended = *((uint8*)(*packet).contents());
            if (ended)
            {
                // Temporary battlebots are removed after bg ends.
                if (m_temporary)
                    botEntry->requestRemoval = true;
                else
                {
                    auto data = std::make_unique<WorldPackets::Battleground::LeaveBattlefield>();
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_8_4
                    data->mapId = me->GetMapId();
#endif
                    me->GetSession()->QueuePacket(std::move(data));
                }
            }
            return;
        }
#endif
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

void BattleBotAI::OnPlayerLogin()
{
    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
}

void BattleBotAI::UpdateWaypointMovement()
{
    // We already have a path.
    if (m_currentPath)
        return;

    if (me->IsMoving())
        return;

    if (!me->IsStopped())
        return;

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
        return;

    switch (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        case IDLE_MOTION_TYPE:
        case CHASE_MOTION_TYPE:
        case POINT_MOTION_TYPE:
            break;
        default:
            return;
    }

    if (BattleGround* bg = me->GetBattleGround())
        if (bg->GetStatus() == STATUS_WAIT_JOIN)
            return;

    if (StartNewPathToObjective())
        return;

    if (StartNewPathFromBeginning())
        return;

    StartNewPathFromAnywhere();
}

void BattleBotAI::OnJustDied()
{
    ClearPath();
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        StopMoving();
    m_avCurrentObjective = 0;
    m_avObjectiveTime = 0;
    m_avSkipObjective = 0;
    m_avSkipObjectiveExpiry = 0;
    m_bgProgressTicks = 0;
}

void BattleBotAI::OnJustRevived()
{
    //SpellAuraHolder* Bholder = me->AddAura(2479, 0, me); // 无荣誉目标
    //Bholder->SetAuraDuration(2*60*1000);
    //Bholder->UpdateAuraDuration();

    SummonPetIfNeeded();

    // AV guard bots immediately head back to their assigned GY after revive.
    if (m_avAssignedGY != 0)
    {
        ClearPath();
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            StopMoving();
        StartNewPathToObjective();
        return;
    }

    if (BattleBotIsWSGHomeGuardCandidate(this))
    {
        ClearPath();
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            StopMoving();
        StartNewPathToObjective();
        return;
    }

    // Behavior 3: randomly become guard of nearest non-home owned GY on respawn (only if mode enables guards)
    if (m_avGuardGraveyards)
        TryAssignAVRespawnGuard(this);
    if (m_avAssignedGY != 0)
    {
        // Stay at the GY; UpdateOutOfCombatAI will route us to guard position
        ClearPath();
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
            StopMoving();
        return;
    }

    if (!me->SelectRandomUnfriendlyTarget(nullptr, 30.0f))
        DoGraveyardJump();
}

void BattleBotAI::OnEnterBattleGround()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg)
        return;

    if (bg->GetStatus() != STATUS_WAIT_JOIN)
        return;

    SummonPetIfNeeded();

    if (bg->GetTypeID() == BATTLEGROUND_WS)
    {
        m_waitingSpot = urand(BB_WSG_WAIT_SPOT_SPAWN, BB_WSG_WAIT_SPOT_RIGHT);
        if (m_waitingSpot == BB_WSG_WAIT_SPOT_RIGHT)
        {
            if (me->GetTeam() == HORDE)
                me->GetMotionMaster()->MovePoint(0, WS_WAITING_POS_HORDE_1.x, WS_WAITING_POS_HORDE_1.y, WS_WAITING_POS_HORDE_1.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES, 0, WS_WAITING_POS_HORDE_1.o);
            else
                me->GetMotionMaster()->MovePoint(0, WS_WAITING_POS_ALLIANCE_1.x, WS_WAITING_POS_ALLIANCE_1.y, WS_WAITING_POS_ALLIANCE_1.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES, 0, WS_WAITING_POS_ALLIANCE_1.o);
        }
        else if (m_waitingSpot == BB_WSG_WAIT_SPOT_LEFT)
        {
            if (me->GetTeam() == HORDE)
                me->GetMotionMaster()->MovePoint(0, WS_WAITING_POS_HORDE_2.x, WS_WAITING_POS_HORDE_2.y, WS_WAITING_POS_HORDE_2.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES, 0, WS_WAITING_POS_HORDE_2.o);
            else
                me->GetMotionMaster()->MovePoint(0, WS_WAITING_POS_ALLIANCE_2.x, WS_WAITING_POS_ALLIANCE_2.y, WS_WAITING_POS_ALLIANCE_2.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES, 0, WS_WAITING_POS_ALLIANCE_2.o);
        }
    }
    else if (bg->GetTypeID() == BATTLEGROUND_AB)
    {
        if (me->GetTeam() == HORDE)
            me->GetMotionMaster()->MovePoint(0, AB_WAITING_POS_HORDE.x + frand(-2.0f, 2.0f), AB_WAITING_POS_HORDE.y + frand(-2.0f, 2.0f), AB_WAITING_POS_HORDE.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE, 0, AB_WAITING_POS_HORDE.o);
        else
            me->GetMotionMaster()->MovePoint(0, AB_WAITING_POS_ALLIANCE.x + frand(-2.0f, 2.0f), AB_WAITING_POS_ALLIANCE.y + frand(-2.0f, 2.0f), AB_WAITING_POS_ALLIANCE.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE, 0, AB_WAITING_POS_ALLIANCE.o);
    }
    else if (bg->GetTypeID() == BATTLEGROUND_AV)
    {
        if (me->GetTeam() == HORDE)
            me->GetMotionMaster()->MovePoint(0, AV_WAITING_POS_HORDE.x + frand(-6.0f, 6.0f), AV_WAITING_POS_HORDE.y + frand(-6.0f, 6.0f), AV_WAITING_POS_HORDE.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE, 0, AV_WAITING_POS_HORDE.o);
        else
            me->GetMotionMaster()->MovePoint(0, AV_WAITING_POS_ALLIANCE.x + frand(-6.0f, 6.0f), AV_WAITING_POS_ALLIANCE.y + frand(-6.0f, 6.0f), AV_WAITING_POS_ALLIANCE.z, MOVE_PATHFINDING | MOVE_EXCLUDE_STEEP_SLOPES | MOVE_RUN_MODE, 0, AV_WAITING_POS_ALLIANCE.o);
    }
}

void BattleBotAI::OnLeaveBattleGround()
{
    ClearPath();
    m_avStartWaveInitialized = false;
    m_avStartWaveBgInstance = 0;
    m_avStartWaveReleaseTime = 0;
    m_avCurrentObjective = 0;
    m_avObjectiveTime = 0;
    m_avSkipObjective = 0;
    m_avSkipObjectiveExpiry = 0;
    m_avAssignedGY = 0;
    m_avIsMineBot = false;
    m_avMineBotDecided = false;
    m_avMineBotBgInstance = 0;
    m_avMineState = AV_MINE_NONE;
    m_avMineIndex = 0;
    // AV strategy state: reset so InitAVStrategy() re-derives on next BG entry
    m_avStrategyDecided = false;           // allow re-init for next BG session
    m_avStrategyBgInstance = 0;           // clear stale instance ID
    m_avMode = 0;                         // 0 = undecided (AV_MODE_NATIVE/PUSH/RANDOM set on entry)
    m_avOpeningPassiveMinutes = 0;        // no passive opening until re-derived
    m_avGuardGraveyards = false;          // no GY guards until re-derived
    m_avHoldCaptureUntilControlled = false; // don't hold captures until re-derived
    m_avAggressiveTravelCombat = false;   // passive travel until re-derived
    m_avMineMissionCount = 0;             // no mine bots until re-derived
    m_avEnableThreePhase = false;         // three-phase off until re-derived
    m_avStayGuardAfterCapture = false;
    m_bgProgressTicks = 0;

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        StopMoving();

    // Release the theme slot so a new BG of this type can pick a fresh theme.
    sOOMgr.RemoveBgTheme(m_battlegroundId);

    // Temporary battlebots are removed after bg ends.
    if (m_temporary)
        botEntry->requestRemoval = true;
}

void BattleBotAI::InitAVStrategy()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_AV || bg->GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (m_avStrategyDecided && m_avStrategyBgInstance == bg->GetInstanceID())
        return;

    m_avStrategyDecided    = true;
    m_avStrategyBgInstance = bg->GetInstanceID();

    uint32 const instanceId = bg->GetInstanceID();
    uint32 const guidLow    = me->GetGUIDLow();

    // Mode is deterministic from instance ID — multiplicative hash breaks sequential patterns
    // from the shared global instance-ID pool (world maps, dungeons, BGs all share one counter).
    m_avMode = static_cast<AVBattleMode>((instanceId * 2654435761u) % 3 + 1);

    switch (m_avMode)
    {
        case AV_MODE_NATIVE:
            m_avOpeningPassiveMinutes      = 0;
            m_avGuardGraveyards            = false;
            m_avHoldCaptureUntilControlled = false;
            m_avAggressiveTravelCombat     = true;
            m_avMineMissionCount           = (instanceId * 7u + (me->GetTeam() == HORDE ? 3u : 0u)) % 11;
            m_avEnableThreePhase           = false;
            break;

        case AV_MODE_PUSH:
            m_avOpeningPassiveMinutes      = 30;
            m_avGuardGraveyards            = true;
            m_avHoldCaptureUntilControlled = true;
            m_avAggressiveTravelCombat     = false;
            m_avMineMissionCount           = 0;
            m_avEnableThreePhase           = true;
            break;

        case AV_MODE_RANDOM:
        default:
            m_avOpeningPassiveMinutes  = 30;
            // guardGraveyards: per-team stable random — Alliance uses bit 0, Horde uses bit 1
            m_avGuardGraveyards = (me->GetTeam() == ALLIANCE)
                ? ((instanceId & 1u) != 0)
                : ((instanceId & 2u) != 0);
            // holdCapture and aggressiveTravel: per-bot GUID-stable ~30%, different seeds
            m_avHoldCaptureUntilControlled = (guidLow ^ instanceId) % 10 < 3;
            m_avAggressiveTravelCombat     = (guidLow ^ (instanceId * 31337u)) % 10 < 3;
            m_avMineMissionCount           = (instanceId * 13u + (me->GetTeam() == HORDE ? 5u : 0u)) % 11;
            m_avEnableThreePhase           = false;
            break;
    }

    char const* const modeName = (m_avMode == AV_MODE_NATIVE) ? "Native" :
                                 (m_avMode == AV_MODE_PUSH)   ? "Push"   : "Random";
    char const* const teamName = (me->GetTeam() == ALLIANCE)  ? "Alliance" : "Horde";
    sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
        "[BattleBotAV] strategy instance=%u team=%s mode=%s openingPassive=%um guardGY=%s holdCapture=%s aggressiveTravel=%s mineBots=%u threePhase=%s",
        bg->GetInstanceID(), teamName, modeName,
        m_avOpeningPassiveMinutes,
        m_avGuardGraveyards            ? "true" : "false",
        m_avHoldCaptureUntilControlled ? "true" : "false",
        m_avAggressiveTravelCombat     ? "true" : "false",
        m_avMineMissionCount,
        m_avEnableThreePhase           ? "true" : "false");

    // Send one GM notification per team per BG: only the bot with the lowest GUID on its team sends it.
    // This avoids 20 identical messages while still guaranteeing exactly one message per team.
    uint32 const myGuid = me->GetGUIDLow();
    bool isLowestGuidBot = true;
    Map* map = me->GetMap();
    for (auto itr = map->GetPlayers().getFirst(); itr != nullptr && isLowestGuidBot; itr = itr->next())
    {
        Player* p = itr->getSource();
        if (!p || p == me || !p->IsBot() || p->GetTeam() != me->GetTeam()) continue;
        if (p->GetGUIDLow() < myGuid)
            isLowestGuidBot = false;
    }
    if (isLowestGuidBot)
    {
        for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
        {
            Player* p = itr->getSource();
            if (p && p->IsGameMaster())
                ChatHandler(p).PSendSysMessage(
                    "[BotAV] instance=%u team=%s mode=%s openingPassive=%um guardGY=%s holdCapture=%s aggressiveTravel=%s mineBots=%u threePhase=%s",
                    bg->GetInstanceID(), teamName, modeName,
                    m_avOpeningPassiveMinutes,
                    m_avGuardGraveyards            ? "true" : "false",
                    m_avHoldCaptureUntilControlled ? "true" : "false",
                    m_avAggressiveTravelCombat     ? "true" : "false",
                    m_avMineMissionCount,
                    m_avEnableThreePhase           ? "true" : "false");
        }
    }
}

bool BattleBotAI::CheckForUnreachableTarget()
{
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
       !me->GetMotionMaster()->GetCurrent()->IsReachable())
    {
        if (Unit* pTarget = static_cast<ChaseMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent())->GetTarget())
        {
            if (!me->CanReachWithMeleeAutoAttack(pTarget))
            {
                if (!me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
                {
                    me->AttackStop(false);
                    StopMoving();
                    return true;
                }

                if (pTarget->IsCreature() && !me->IsMoving())
                {
                    me->AttackStop(false);
                    StopMoving();
                    return true;
                }

                if (me->GetDistanceZ(pTarget) > 10.0f)
                {
                    me->AttackStop(false);
                    StopMoving();
                    return true;
                }
            }
        }
    }

    return false;
}

bool BattleBotAI::IsBadPlayer(Unit const* pTarget) const
{
    if (!pTarget->ToPlayer())
        return false;

    if (pTarget->HasAura(AURA_WARSONG_FLAG))
        return false;

    if (pTarget->HasAura(AURA_SILVERWING_FLAG))
        return false;

    if (pTarget->IsMounted())
        return true;

    if (pTarget->HasAura(783))   // Druid Travel Form
        return true;

    if (pTarget->HasAura(5421))  // Druid Aquatic Form
        return true;

    return false;
}

void BattleBotAI::UpdateAI(uint32 const diff)
{
    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(BB_UPDATE_INTERVAL);
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported() || m_doingGraveyardJump)
        return;

    if (!m_initialized)
    {
        if (m_level && m_level != me->GetLevel())
        {
            me->GiveLevel(m_level);
            me->InitTalentForLevel();
            me->SetUInt32Value(PLAYER_XP, 0);
        }

        LearnPremadeSpecForClass();

        if (m_role == ROLE_INVALID)
            AutoAssignRole();

        AutoEquipGear(sWorld.getConfig(CONFIG_UINT32_BATTLE_BOT_AUTO_EQUIP));
        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        me->UpdateSkillsToMaxSkillsForLevel();

        if (uint32 mountSpellId = GetMountSpellId())
            if (!me->HasSpell(mountSpellId))
                me->LearnSpell(mountSpellId, false, false);

        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        if (urand(0, 1))
        {
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM);
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK);
        }

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        m_initialized = true;
        return;
    }

    if (!me->InBattleGround())
    {
        if (botEntry && botEntry->requestRemoval)
            return;

        if (m_wasInBG)
        {
            m_wasInBG = false;
            OnLeaveBattleGround();
            return;
        }

        if (m_receivedBgInvite)
        {
            SendBattlefieldPortPacket();
            m_receivedBgInvite = false;
            return;
        }

        if (!me->InBattleGroundQueue())
        {
            bool canQueue;
            char args[] = "";
            switch (m_battlegroundId)
            {
                case BATTLEGROUND_QUEUE_AV:
                    canQueue = ChatHandler(me).HandleGoAlteracCommand(args);
                    break;
                case BATTLEGROUND_QUEUE_WS:
                    canQueue = ChatHandler(me).HandleGoWarsongCommand(args);
                    break;
                case BATTLEGROUND_QUEUE_AB:
                    canQueue = ChatHandler(me).HandleGoArathiCommand(args);
                    break;
                default:
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "BattleBot: Invalid BG queue type!");
                    botEntry->requestRemoval = true;
                    return;
            }

            if (!canQueue)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "BattleBot: Attempt to queue for BG failed! Bot is too low level or BG is not available in this patch.");
                botEntry->requestRemoval = true;
                return;
            }

            SendBattlemasterJoinPacket(m_battlegroundId);
            return;
        }

        // Remain idle until we can join battleground.
        return;
    }
    else
    {
        if (!m_wasInBG)
        {
            m_wasInBG = true;
            OnEnterBattleGround();
            return;
        }
        else if (m_temporary)
        {
            // Remove temporary battlebots if no real players in map.
            if (BattleGround* bg = me->GetBattleGround())
            {
                if (bg->GetStatus() == STATUS_IN_PROGRESS)
                {
                    if (!me->GetMap()->HaveRealPlayers())
                    {
                        botEntry->requestRemoval = true;
                        bg->EndBattleGround(TEAM_NONE); // 关闭战场
                        return;
                    }
                }
            }
        }
    }

    if (me->IsDead())
    {
        if (!m_wasDead)
        {
            m_wasDead = true;
            OnJustDied();
            return;
        }

        if (me->InBattleGround())
        {
            if (me->GetDeathState() == CORPSE)
            {
                me->BuildPlayerRepop();
                me->RepopAtGraveyard();
            }
        }
        else
        {
            if (me->GetDeathState() == DEAD)
            {
                me->ResurrectPlayer(0.5f);
                me->SpawnCorpseBones();
                //me->SendCreateUpdateToPlayer(pLeader);
            }
        }

        return;
    }
    else
    {
        if (m_wasDead)
        {
            m_wasDead = false;
            OnJustRevived();
            return;
        }
    }

    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        BreakCrowdControlEffects();
        return;
    }

    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        // Stop auto shot if no target.
        if (!me->GetVictim())
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        else if (me->GetClass() == CLASS_HUNTER)
        {
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                UpdateInCombatAI_Hunter();
        }

        return;
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    Unit* pVictim = me->GetVictim();

    // Prevent battelbot from chasing target entered stealth mode
    if (pVictim && !pVictim->IsVisibleForOrDetect(me, me, false))
    {
        me->AttackStop();
        me->ClearTarget();
        me->StopMoving();
        if (pVictim = SelectAttackTarget(pVictim))
            AttackStart(pVictim);
        return;
    }

    if (pVictim && IsBadPlayer(pVictim) && !pVictim->IsWithinDist(me, VISIBILITY_DISTANCE_SMALL))
    {
        me->AttackStop();
        me->ClearTarget();
        me->StopMoving();
        if (pVictim = SelectAttackTarget(pVictim))
            AttackStart(pVictim);
        return;
    }

    if (UpdateAVPhase1WaitingAI())
        return;

    if (!me->IsInCombat())
    {
        if (BattleBotTryCaptureNearbyObjective(this))
            return;

        if (BattleBotReturnToGuardPositionBeforeRecovery(this))
            return;

        if (DrinkAndEat())
            return;
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    if (UpdateBattleGroundAI())
        return;

    // BG melee stuck detection + WSG exploit prevention
    {
        BattleGround* currentBg = me->GetBattleGround();
        if (currentBg && currentBg->GetTypeID() == BATTLEGROUND_WS)
        {
            float const curX = me->GetPositionX();
            float const curY = me->GetPositionY();
            bool const inCombat = me->IsInCombat();
            bool dropCombat = false;

            if (inCombat)
            {
                // Home guard bots must not stray >80 yards from own flag room
                if (BattleBotIsWSGHomeGuardCandidate(this))
                {
                    Position const& flagPos = (me->GetTeam() == ALLIANCE) ? WS_FLAG_POS_ALLIANCE : WS_FLAG_POS_HORDE;
                    if (me->GetDistance2d(flagPos.x, flagPos.y) > 80.0f)
                        dropCombat = true;
                }

                // Stuck while chasing: can't reach victim. Stationary 10s or oscillating 30s.
                if (!dropCombat && me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                {
                    bool const cantReach = me->GetVictim() && !me->CanReachWithMeleeAutoAttack(me->GetVictim());
                    bool const notMoved = me->GetDistance2d(m_bgStuckLastX, m_bgStuckLastY) < 2.0f;
                    if (cantReach)
                        ++m_bgStuckCounter;
                    else
                        m_bgStuckCounter = 0;
                    uint8 const stuckThreshold = notMoved ? 5 : 15;
                    if (m_bgStuckCounter >= stuckThreshold)
                        dropCombat = true;
                }
                else
                {
                    // After CC (e.g. fear): attack state remains active but chase was
                    // interrupted. Detect: standing still, victim out of melee range,
                    // not rooted (rooted bots legitimately can't move).
                    if (!me->IsMoving() && !me->IsRooted() &&
                        me->GetVictim() && !me->CanReachWithMeleeAutoAttack(me->GetVictim()))
                    {
                        ++m_bgStuckCounter;
                        if (m_bgStuckCounter >= 3) // 3 * 2s = 6 seconds
                            dropCombat = true;
                    }
                    else
                        m_bgStuckCounter = 0;
                }
            }
            else
            {
                m_bgStuckCounter = 0;
                // Out of combat but physically stuck inside an unexpected building
                if (!me->IsMoving())
                {
                    bool const outdoors = me->GetMap()->GetTerrain()->IsOutdoors(curX, curY, me->GetPositionZ());
                    if (!outdoors)
                    {
                        Position const& ownFlag = (me->GetTeam() == ALLIANCE) ? WS_FLAG_POS_ALLIANCE : WS_FLAG_POS_HORDE;
                        Position const& enemyFlag = (me->GetTeam() == ALLIANCE) ? WS_FLAG_POS_HORDE : WS_FLAG_POS_ALLIANCE;
                        if (me->GetDistance2d(ownFlag.x, ownFlag.y) > 25.0f &&
                            me->GetDistance2d(enemyFlag.x, enemyFlag.y) > 25.0f)
                        {
                            ClearPath();
                            StartNewPathToObjective();
                        }
                    }
                }
            }

            if (dropCombat)
            {
                me->AttackStop(false);
                StopMoving();
                ClearPath();
                StartNewPathToObjective();
                m_bgStuckCounter = 0;
            }

            // Checkpoint-based movement progress detection; excludes FOLLOW to avoid
            // false positives for home guard bots patrolling near the flag room.
            if (me->IsMoving() &&
                me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                ++m_bgProgressTicks;
                if (m_bgProgressTicks >= 10) // 10 * 2s = 20 seconds
                {
                    if (me->GetDistance2d(m_bgProgressX, m_bgProgressY) < 15.0f)
                    {
                        if (inCombat)
                        {
                            me->AttackStop(false);
                            StopMoving();
                            m_bgStuckCounter = 0;
                        }
                        ClearPath();
                        StartNewPathToObjective();
                    }
                    m_bgProgressX = curX;
                    m_bgProgressY = curY;
                    m_bgProgressTicks = 0;
                }
            }
            else
            {
                m_bgProgressTicks = 0;
                m_bgProgressX = curX;
                m_bgProgressY = curY;
            }

            m_bgStuckLastX = curX;
            m_bgStuckLastY = curY;
        }
        else if (currentBg && (currentBg->GetTypeID() == BATTLEGROUND_AV || currentBg->GetTypeID() == BATTLEGROUND_AB))
        {
            float const curX = me->GetPositionX();
            float const curY = me->GetPositionY();

            if (me->IsInCombat() &&
                me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
            {
                bool const cantReach = me->GetVictim() && !me->CanReachWithMeleeAutoAttack(me->GetVictim());
                bool const notMoved = me->GetDistance2d(m_bgStuckLastX, m_bgStuckLastY) < 2.0f;
                if (cantReach)
                    ++m_bgStuckCounter;
                else
                    m_bgStuckCounter = 0;
                // Stationary + can't reach: 10s. Oscillating + can't reach: 30s.
                // The longer threshold avoids false positives when legitimately
                // chasing a running target that stays just out of melee range.
                uint8 const stuckThreshold = notMoved ? 5 : 15;
                if (m_bgStuckCounter >= stuckThreshold)
                {
                    me->AttackStop(false);
                    StopMoving();
                    ClearPath();
                    StartNewPathToObjective();
                    m_bgStuckCounter = 0;
                    m_bgProgressTicks = 0;
                    m_bgProgressX = curX;
                    m_bgProgressY = curY;
                }
            }
            else
            {
                // After CC (e.g. fear): attack state remains active but chase was
                // interrupted. Detect: standing still, victim out of melee range,
                // not rooted (rooted bots legitimately can't move).
                if (me->IsInCombat() && !me->IsMoving() && !me->IsRooted() &&
                    me->GetVictim() && !me->CanReachWithMeleeAutoAttack(me->GetVictim()))
                {
                    ++m_bgStuckCounter;
                    if (m_bgStuckCounter >= 3) // 3 * 2s = 6 seconds
                    {
                        me->AttackStop(false);
                        StopMoving();
                        ClearPath();
                        StartNewPathToObjective();
                        m_bgStuckCounter = 0;
                    }
                }
                else
                {
                    m_bgStuckCounter = 0;
                }
                // Out of combat but physically stuck inside a building (bunker in AV, tower in AB)
                if (!me->IsInCombat() && !me->IsMoving())
                {
                    bool const outdoors = me->GetMap()->GetTerrain()->IsOutdoors(curX, curY, me->GetPositionZ());
                    if (!outdoors)
                    {
                        bool shouldReroute = true;
                        if (currentBg->GetTypeID() == BATTLEGROUND_AV)
                        {
                            // Allow bots to remain indoors when fighting the AV boss
                            if (Creature* pBossA = me->GetMap()->GetCreature(currentBg->GetSingleCreatureGuid(BG_AV_BOSS_A, 0)))
                                if (me->GetDistance(pBossA) < 60.0f)
                                    shouldReroute = false;
                            if (shouldReroute)
                                if (Creature* pBossH = me->GetMap()->GetCreature(currentBg->GetSingleCreatureGuid(BG_AV_BOSS_H, 0)))
                                    if (me->GetDistance(pBossH) < 60.0f)
                                        shouldReroute = false;
                        }
                        if (shouldReroute)
                        {
                            ClearPath();
                            StartNewPathToObjective();
                        }
                    }
                }
            }

            // Checkpoint-based movement progress detection: catches both outdoor pathfinding
            // loops (out-of-combat) and in-combat terrain-pit loops where the bot oscillates
            // without making net progress (e.g. chasing an enemy above a pit).
            if (me->IsMoving())
            {
                ++m_bgProgressTicks;
                if (m_bgProgressTicks >= 10) // 10 * 2s = 20 seconds
                {
                    if (me->GetDistance2d(m_bgProgressX, m_bgProgressY) < 15.0f)
                    {
                        if (me->IsInCombat())
                        {
                            me->AttackStop(false);
                            StopMoving();
                        }
                        ClearPath();
                        StartNewPathToObjective();
                    }
                    m_bgProgressX = curX;
                    m_bgProgressY = curY;
                    m_bgProgressTicks = 0;
                }
            }
            else
            {
                m_bgProgressTicks = 0;
                m_bgProgressX = curX;
                m_bgProgressY = curY;
            }

            m_bgStuckLastX = curX;
            m_bgStuckLastY = curY;
        }
        else
        {
            m_bgStuckCounter = 0;
        }
    }

    if (!me->IsInCombat())
    {
        if (CheckForUnreachableTarget())
            return;

        UpdateOutOfCombatAI();

        if (m_isBuffing)
            return;

        // Can enter combat from UpdateOutOfCombatAI().
        if (me->IsInCombat())
            return;

        if (me->IsNonMeleeSpellCasted())
            return;

        if (!pVictim || !IsValidHostileTarget(pVictim))
        {
            // In AV phase 3 (total assault), don't initiate combat — only fight back if attacked.
            // Exception: bots holding a GY during active capture must still fight incoming defenders.
            // Only active when enableThreePhase is set (Mode 2 / Push); other modes skip this.
            BattleGround* bgForAssault = me->GetBattleGround();
            bool const avTotalAssault = m_avEnableThreePhase &&
                m_avAssignedGY == 0 && !BattleBotIsInAVGyCaptureHold(this) &&
                bgForAssault && bgForAssault->GetTypeID() == BATTLEGROUND_AV &&
                ((me->GetTeam() == HORDE    && (bgForAssault->IsActiveEvent(BG_AV_STORMPIKE_GY, HORDE_ASSAULTED)    || bgForAssault->IsActiveEvent(BG_AV_STORMPIKE_GY, HORDE_CONTROLLED)))    ||
                 (me->GetTeam() == ALLIANCE && (bgForAssault->IsActiveEvent(BG_AV_FROSTWOLF_GY, ALLIANCE_ASSAULTED) || bgForAssault->IsActiveEvent(BG_AV_FROSTWOLF_GY, ALLIANCE_CONTROLLED))));

            // Passive travelers don't initiate combat outside objectives.
            // Guards, bots inside a capture hold, and bots near any AV flag always fight.
            bool const avPassiveTraveler = !m_avAggressiveTravelCombat &&
                m_avAssignedGY == 0 && !BattleBotIsInAVGyCaptureHold(this) &&
                !BattleBotIsNearAVFlag(this, AV_FLAG_DEFENSE_RADIUS) &&
                bgForAssault && bgForAssault->GetTypeID() == BATTLEGROUND_AV;

            if (!avTotalAssault && !avPassiveTraveler)
            {
                if (m_role != ROLE_HEALER)
                {
                    if (pVictim = SelectAttackTarget(pVictim))
                    {
                        AttackStart(pVictim);
                        return;
                    }
                }
                else if (IsRangedDamageClass(me->GetClass()))
                {
                    // Healer: attack an enemy a nearby ally is already fighting.
                    // AttackStart sets caster chase distance so we stay at range.
                    if (Unit* pOffensiveTarget = SelectHealerOffensiveTarget())
                    {
                        AttackStart(pOffensiveTarget);
                        return;
                    }
                }
            }

            if (UseMount())
                return;

            BattleGround* bgForFollow = me->GetBattleGround();
            bool const isWaitJoin = bgForFollow && bgForFollow->GetStatus() == STATUS_WAIT_JOIN;
            if (isWaitJoin)
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                    StopMoving();
            }
            else if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                if (Unit* pFollowTarget = SelectFollowTarget())
                {
                    ClearPath();
                    me->GetMotionMaster()->MoveFollow(pFollowTarget, frand(3.0f, 5.0f), frand(0.0f, 3.0f));
                    return;
                }
            }
            else if (FollowMovementGenerator<Player> const* pMoveGen = dynamic_cast<FollowMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent()))
            {
                Unit* pTarget = pMoveGen->GetTarget();

                // Re-evaluate: switch to flag carrier if one appeared while following someone else.
                if (pTarget && !BattleBotHasBattlegroundFlag(pTarget))
                {
                    if (Unit* pNewFollowTarget = SelectFollowTarget())
                    {
                        if (pNewFollowTarget != pTarget)
                        {
                            ClearPath();
                            me->GetMotionMaster()->MoveFollow(pNewFollowTarget, frand(3.0f, 5.0f), frand(0.0f, 3.0f));
                            return;
                        }
                    }
                }

                if (!pTarget || !pTarget->IsAlive() || !pTarget->IsWithinDist(me, VISIBILITY_DISTANCE_NORMAL))
                {
                    StopMoving();
                    return;
                }

                if (BattleGround* bg = me->GetBattleGround())
                {
                    if (bg->GetTypeID() == BATTLEGROUND_WS &&
                       !pTarget->HasAura(AURA_WARSONG_FLAG) &&
                       !pTarget->HasAura(AURA_SILVERWING_FLAG))
                    {
                        if (Player* pTargetPlayer = pTarget->ToPlayer())
                        {
                            if (BattleBotAI* pTargetAI = dynamic_cast<BattleBotAI*>(pTargetPlayer->AI()))
                            {
                                if (BattleBotIsWSGHomeGuardCandidate(pTargetAI))
                                {
                                    StopMoving();
                                    return;
                                }
                            }
                        }
                    }

                }
            }

            UpdateWaypointMovement();
        }
        return;
    }

    if (ShouldIgnoreCombat())
    {
        UpdateFlagCarrierAI();
        UpdateWaypointMovement();
        return;
    }

    if (!pVictim || !IsValidHostileTarget(pVictim) ||
        BattleBotHasBlind(pVictim) ||
        BattleBotHasEntanglingRoots(pVictim) ||
        !pVictim->IsWithinDist(me, VISIBILITY_DISTANCE_SMALL))
    {
        if (m_role != ROLE_HEALER)
        {
            if (Unit* pNewVictim = SelectAttackTarget(pVictim))
            {
                if (pVictim != pNewVictim)
                {
                    AttackStart(pNewVictim);
                    return;
                }
            }
        }
        else if (IsRangedDamageClass(me->GetClass()))
        {
            if (Unit* pNewVictim = SelectHealerOffensiveTarget())
            {
                if (pVictim != pNewVictim)
                {
                    AttackStart(pNewVictim);
                    return;
                }
            }
        }

        if (me->GetVictim() && BattleBotHasBlind(me->GetVictim()))
        {
            me->AttackStop(false);
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                StopMoving();
            return;
        }

        if (me->GetVictim() &&
           me->GetVictim()->IsPlayer() &&
           (me != me->GetVictim()->GetVictim()))
        {
            me->AttackStop(false);
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                StopMoving();
            return;
        }
    }
    else
    {
        if (!me->HasInArc(pVictim, 2 * M_PI_F / 3) && !me->IsMoving())
        {
            me->SetInFront(pVictim);
            me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
        }

        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
           (m_role != ROLE_HEALER || IsRangedDamageClass(me->GetClass())) &&
            IsValidHostileTarget(pVictim) &&
            AttackStart(pVictim))
            return;
    }

    if (me->IsInCombat())
        UpdateInCombatAI();
}

bool BattleBotAI::UpdateBattleGroundAI()
{
    BattleGround* bg = me->GetBattleGround();
    if (!bg)
        return false;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_AV:
            InitAVStrategy();
            BattleBotUpdateAVGuardBehavior(this);
            break;
        case BATTLEGROUND_WS:
        {
            // Pick up dropped flags.
            if (TryUseBattleGroundFlag(GO_WSG_DROPPED_SILVERWING_FLAG) ||
                TryUseBattleGroundFlag(GO_WSG_DROPPED_WARSONG_FLAG))
                return true;

            // Pick up stationary flags from bases.
            if (me->GetTeam() == HORDE)
            {
                return TryUseBattleGroundFlag(GO_WSG_SILVERWING_FLAG);
            }
            else
            {
                return TryUseBattleGroundFlag(GO_WSG_WARSONG_FLAG);
            }
        }
    }

    return false;
}

bool BattleBotAI::TryUseBattleGroundFlag(uint32 entry)
{
    GameObject* pGo = me->FindNearestGameObject(entry, INTERACTION_DISTANCE);
    if (!pGo)
        return false;

    // Two-tick approach: removing mount/shapeshift triggers state changes that
    // wouldn't settle in time for an immediate pGo->Use() call this same tick.
    // Return true so the caller skips the rest of UpdateAI (prevents druids
    // from snapping back into combat form right after un-shifting). Next tick
    // the state is clean and the third branch actually picks up the flag.
    if (me->IsMounted())
    {
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        return true;
    }

    if (me->IsInDisallowedMountForm())
    {
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return true;
    }

    pGo->Use(me);
    return true;
}

void BattleBotAI::UpdateFlagCarrierAI()
{
    // First those that can be cast both in and out of combat.
    switch (me->GetClass())
    {
        case CLASS_PALADIN:
        {
            if (m_spells.paladin.pHolyShock && me->GetHealthPercent() < 70.0f &&
                CanTryToCastSpell(me, m_spells.paladin.pHolyShock))
            {
                me->CastSpell(me, m_spells.paladin.pHolyShock, false);
                return;
            }
            break;
        }
        case CLASS_SHAMAN:
        {
            if (m_spells.shaman.pGhostWolf && !me->IsMoving() &&
                CanTryToCastSpell(me, m_spells.shaman.pGhostWolf))
            {
                me->CastSpell(me, m_spells.shaman.pGhostWolf, false);
                return;
            }
            break;
        }
        case CLASS_MAGE:
        {
            if (m_spells.mage.pManaShield &&
                CanTryToCastSpell(me, m_spells.mage.pManaShield))
            {
                me->CastSpell(me, m_spells.mage.pManaShield, false);
                return;
            }
            if (m_spells.mage.pIceBarrier &&
                CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
            {
                me->CastSpell(me, m_spells.mage.pIceBarrier, false);
                return;
            }
            break;
        }
        case CLASS_PRIEST:
        {
            if (m_spells.priest.pPowerWordShield &&
                CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
            {
                me->CastSpell(me, m_spells.priest.pPowerWordShield, false);
                return;
            }
            if (m_spells.priest.pHolyNova && me->GetHealthPercent() < 90.0f &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                me->CastSpell(me, m_spells.priest.pHolyNova, false);
                return;
            }
            break;
        }
        case CLASS_WARRIOR:
        {
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {
                me->CastSpell(me, m_spells.warrior.pDefensiveStance, false);
                return;
            }
            break;
        }
        case CLASS_ROGUE:
        {
            if (m_spells.rogue.pSprint &&
                CanTryToCastSpell(me, m_spells.rogue.pSprint))
            {
                me->CastSpell(me, m_spells.rogue.pSprint, false);
                return;
            }
            break;
        }
        case CLASS_DRUID:
        {
            if (me->GetShapeshiftForm() == FORM_NONE)
            {
                if (m_spells.druid.pTravelForm &&
                    CanTryToCastSpell(me, m_spells.druid.pTravelForm))
                {
                    me->CastSpell(me, m_spells.druid.pTravelForm, false);
                    return;
                }
            }
            else if (me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
            {
                me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
            }
            break;
        }
    }

    Unit* pAttacker = me->GetAttackerForHelper();
    if (pAttacker)
    {
        switch (me->GetClass())
        {
            case CLASS_PALADIN:
            {
                if (Unit* pHammerTarget = SelectPaladinHammerOfJusticeTarget(this, pAttacker))
                {
                    me->CastSpell(pHammerTarget, m_spells.paladin.pHammerOfJustice, false);
                    return;
                }

                if (m_spells.paladin.pHammerOfJustice &&
                    CanTryToCastSpell(pAttacker, m_spells.paladin.pHammerOfJustice))
                {
                    me->CastSpell(pAttacker, m_spells.paladin.pHammerOfJustice, false);
                    return;
                }
                break;
            }
            case CLASS_SHAMAN:
            {
                if (m_spells.shaman.pFrostShock &&
                    CanTryToCastSpell(pAttacker, m_spells.shaman.pFrostShock))
                {
                    me->CastSpell(pAttacker, m_spells.shaman.pFrostShock, false);
                    return;
                }
                break;
            }
            case CLASS_HUNTER:
            {
                if (m_spells.hunter.pAspectOfTheCheetah &&
                    me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id))
                {
                    me->RemoveAurasDueToSpellByCancel(m_spells.hunter.pAspectOfTheCheetah->Id);
                    return;
                }
                break;
            }
            case CLASS_MAGE:
            {
                if (m_spells.mage.pFrostNova &&
                    CanTryToCastSpell(me, m_spells.mage.pFrostNova) &&
                    m_spells.mage.pFrostNova->IsTargetInRange(me, pAttacker))
                {
                    me->CastSpell(me, m_spells.mage.pFrostNova, false);
                    return;
                }
                break;
            }
            case CLASS_PRIEST:
            {
                if (BattleBotShouldUsePsychicScream(this))
                {
                    me->CastSpell(me, m_spells.priest.pPsychicScream, false);
                    return;
                }
                break;
            }
            case CLASS_WARLOCK:
            {
                if (m_spells.warlock.pDeathCoil &&
                    CanTryToCastSpell(pAttacker, m_spells.warlock.pDeathCoil))
                {
                    me->CastSpell(pAttacker, m_spells.warlock.pDeathCoil, false);
                    return;
                }
                break;
            }
            case CLASS_WARRIOR:
            {
                if (m_spells.warrior.pShieldWall && me->GetHealthPercent() < 50.0f &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {
                    me->CastSpell(me, m_spells.warrior.pShieldWall, false);
                    return;
                }
                break;
            }
            case CLASS_ROGUE:
            {
                if (m_spells.rogue.pEvasion && me->GetHealthPercent() < 50.0f &&
                    CanTryToCastSpell(me, m_spells.rogue.pEvasion))
                {
                    me->CastSpell(me, m_spells.rogue.pEvasion, false);
                    return;
                }
                break;
            }
            case CLASS_DRUID:
            {
                if (m_spells.druid.pBarkskin && me->GetHealthPercent() < 50.0f &&
                    CanTryToCastSpell(me, m_spells.druid.pBarkskin))
                {
                    me->CastSpell(me, m_spells.druid.pBarkskin, false);
                    return;
                }
                break;
            }
        }
    }
    else // no attackers
    {
        switch (me->GetClass())
        {
            case CLASS_HUNTER:
            {
                if (m_spells.hunter.pAspectOfTheCheetah &&
                    !me->InBattleGround() &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah))
                {
                    me->CastSpell(me, m_spells.hunter.pAspectOfTheCheetah, false);
                    return;
                }
                break;
            }
        }
    }
}

void BattleBotAI::UpdateOutOfCombatAI()
{
    // Undead: Cannibalize - channel HP recovery from nearby corpse
    if (m_racialSpells.pCannibalize && me->GetHealthPercent() < 70.0f &&
        CanTryToCastSpell(me, m_racialSpells.pCannibalize))
        DoCastSpell(me, m_racialSpells.pCannibalize);

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

void BattleBotAI::UpdateInCombatAI()
{
    // Gnome: Escape Artist - break root/snare
    if (m_racialSpells.pEscapeArtist && me->IsInRoots() &&
        CanTryToCastSpell(me, m_racialSpells.pEscapeArtist))
        DoCastSpell(me, m_racialSpells.pEscapeArtist);

    // Dwarf: Stoneform - remove poison/disease/bleed when HP threatened
    if (m_racialSpells.pStoneform && me->GetHealthPercent() < 60.0f)
    {
        bool hasDebuff = false;
        for (auto const& [id, holder] : me->GetSpellAuraHolderMap())
        {
            uint32 const dispelType = holder->GetSpellProto()->Dispel;
            if (!holder->IsPositive() && (dispelType == DISPEL_POISON || dispelType == DISPEL_DISEASE))
            {
                hasDebuff = true;
                break;
            }
        }
        if (!hasDebuff)
        {
            for (auto const* pAura : me->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE))
            {
                if (pAura->GetSpellProto()->Mechanic == MECHANIC_BLEED)
                {
                    hasDebuff = true;
                    break;
                }
            }
        }
        if (hasDebuff && CanTryToCastSpell(me, m_racialSpells.pStoneform))
            DoCastSpell(me, m_racialSpells.pStoneform);
    }

    // Tauren: War Stomp - AoE stun when 2+ attackers in melee range
    if (m_racialSpells.pWarStomp && me->GetVictim() &&
        GetAttackersInRangeCount(8.0f) >= 2 &&
        CanTryToCastSpell(me->GetVictim(), m_racialSpells.pWarStomp))
        DoCastSpell(me->GetVictim(), m_racialSpells.pWarStomp);

    // Troll: Berserking - speed boost when below 60% HP
    if (m_racialSpells.pBerserking && me->GetHealthPercent() < 60.0f &&
        CanTryToCastSpell(me, m_racialSpells.pBerserking))
        DoCastSpell(me, m_racialSpells.pBerserking);

    // Orc: Blood Fury - AP boost for physical classes (not healers, penalty reduces healing taken)
    if (m_racialSpells.pBloodFury && m_role != ROLE_HEALER &&
        IsPhysicalDamageClass(me->GetClass()) && me->GetVictim() &&
        CanTryToCastSpell(me, m_racialSpells.pBloodFury))
        DoCastSpell(me, m_racialSpells.pBloodFury);

    switch (me->GetClass())
    {
        case CLASS_PALADIN:
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();
}

void BattleBotAI::UpdateOutOfCombatAI_Paladin()
{
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.paladin.pBlessingBuff)
    {
        if (Player* pTarget = SelectBuffTarget(m_spells.paladin.pBlessingBuff))
        {
            if (CanTryToCastSpell(pTarget, m_spells.paladin.pBlessingBuff))
            {
                if (DoCastSpell(pTarget, m_spells.paladin.pBlessingBuff) == SPELL_CAST_OK)
                {
                    m_isBuffing = true;
                    return;
                }
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {
        m_isBuffing = false;
    }

}

void BattleBotAI::UpdateInCombatAI_Paladin()
{
    if (m_spells.paladin.pDivineShield &&
       (me->GetHealthPercent() < 20.0f) &&
       !BattleBotHasBattlegroundFlag(me) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pLayTarget = SelectPaladinEmergencyHealTarget(this, m_spells.paladin.pLayOnHands, 18.0f))
    {
        if (DoCastSpell(pLayTarget, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pShockHealTarget = SelectPaladinEmergencyHealTarget(this, m_spells.paladin.pHolyShock, m_role == ROLE_HEALER ? 70.0f : 50.0f))
    {
        if (m_spells.paladin.pDivineFavor &&
            CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            DoCastSpell(me, m_spells.paladin.pDivineFavor);

        if (DoCastSpell(pShockHealTarget, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pFreedomTarget = SelectPaladinFreedomTarget(this))
    {
        if (DoCastSpell(pFreedomTarget, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pProtectionTarget = SelectPaladinProtectionTarget(this))
    {
        if (DoCastSpell(pProtectionTarget, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pSacrificeTarget = SelectPaladinSacrificeTarget(this))
    {
        if (DoCastSpell(pSacrificeTarget, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.paladin.pCleanse)
    {
        if (Unit* pFriend = SelectDispelTarget(m_spells.paladin.pCleanse))
        {
            if (CanTryToCastSpell(pFriend, m_spells.paladin.pCleanse))
            {
                if (DoCastSpell(pFriend, m_spells.paladin.pCleanse) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (m_role == ROLE_HEALER &&
        FindAndHealInjuredAlly(me->IsTotalImmune() ? 80.0f : 65.0f, 85.0f))
        return;

    if (Unit* pHammerTarget = SelectPaladinHammerOfJusticeTarget(this, me->GetVictim()))
    {
        if (DoCastSpell(pHammerTarget, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
            return;
    }

    if (m_role == ROLE_HEALER)
        return;

    bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

    if (!hasSeal &&
        m_spells.paladin.pSeal &&
        CanTryToCastSpell(me, m_spells.paladin.pSeal))
    {
        me->CastSpell(me, m_spells.paladin.pSeal, false);
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.paladin.pHammerOfWrath &&
            pVictim->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                return;
        }
        if (hasSeal && m_spells.paladin.pJudgement &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield) &&
           (IsMeleeDamageClass(pVictim->GetClass()) || (GetAttackersInRangeCount(8.0f) > 1)))
        {
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pConsecration &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.paladin.pConsecration))
        {
            if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pHolyShock &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;
        }
        if (m_spells.paladin.pExorcism &&
            pVictim->IsCreature() &&
           (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
        {
            if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                return;
        }
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
           !me->CanReachWithMeleeAutoAttack(pVictim))
        {
            me->GetMotionMaster()->MoveChase(pVictim);
        }
    }

    FindAndHealInjuredAlly(me->IsTotalImmune() ? 80.0f : 40.0f, 50.0f);
}

void BattleBotAI::UpdateOutOfCombatAI_Shaman()
{
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;
    }

    if (me->GetVictim())
    {
        if (m_role != ROLE_HEALER && SummonShamanTotems())
            return;

        UpdateInCombatAI_Shaman();
    }
    else
    {
        if (m_spells.shaman.pGhostWolf &&
           !me->IsMoving() && !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.shaman.pGhostWolf))
        {
            if (DoCastSpell(me, m_spells.shaman.pGhostWolf) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateInCombatAI_Shaman()
{
    if (m_spells.shaman.pGhostWolf &&
        me->GetShapeshiftForm() == FORM_GHOSTWOLF)
        me->RemoveAurasDueToSpellByCancel(m_spells.shaman.pGhostWolf->Id);

    if (m_role == ROLE_HEALER)
    {
        // ── HEALER: dispel ──────────────────────────────────────────────────
        if (m_spells.shaman.pCureDisease)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCureDisease))
                if (CanTryToCastSpell(pFriend, m_spells.shaman.pCureDisease))
                    if (DoCastSpell(pFriend, m_spells.shaman.pCureDisease) == SPELL_CAST_OK)
                        return;
        }

        if (m_spells.shaman.pCurePoison)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.shaman.pCurePoison))
                if (CanTryToCastSpell(pFriend, m_spells.shaman.pCurePoison))
                    if (DoCastSpell(pFriend, m_spells.shaman.pCurePoison) == SPELL_CAST_OK)
                        return;
        }

        // ── HEALER: Chain Heal / direct heal — more aggressive threshold ────
        if (FindAndHealInjuredAlly(55.0f, 85.0f))
            return;

        if (FindAndPreHealTarget())
            return;

        // ── HEALER: interrupt and purge ─────────────────────────────────────
        if (Unit* pInterruptTarget = SelectShamanInterruptTarget(this))
        {
            if (DoCastSpell(pInterruptTarget, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pPurgeTarget = SelectShamanPurgeTarget(this))
        {
            if (DoCastSpell(pPurgeTarget, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                return;
        }

        // ── HEALER: control totems only (Grounding, Tremor, Earthbind) ──────
        // SummonShamanTotems in healer mode already skips fire / Windfury / SoE
        if (SummonShamanTotems())
            return;

        if (me->GetPowerPercent(POWER_MANA) < 30.0f)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_role != ROLE_HEALER)
        {
            // ── DPS: interrupt ──────────────────────────────────────────────
            if (Unit* pInterruptTarget = SelectShamanInterruptTarget(this))
            {
                if (DoCastSpell(pInterruptTarget, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                    return;
            }

            // ── DPS: Elemental Mastery before burst ─────────────────────────
            if (m_spells.shaman.pElementalMastery &&
                me->GetAttackers().empty() &&
                CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
            {
                if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                    return;
            }

            // ── DPS: purge ──────────────────────────────────────────────────
            if (Unit* pPurgeTarget = SelectShamanPurgeTarget(this))
            {
                if (DoCastSpell(pPurgeTarget, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                    return;
            }

            // ── DPS: melee rotation ─────────────────────────────────────────
            if (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
            {
                if (m_spells.shaman.pStormstrike &&
                    CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
                {
                    if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.shaman.pFlameShock &&
                    CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
                {
                    if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.shaman.pFrostShock &&
                    pVictim->IsMoving() &&
                    CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
                {
                    if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                        return;
                }
            }

            // ── DPS: range rotation ─────────────────────────────────────────
            if (m_role == ROLE_RANGE_DPS)
            {
                if (m_spells.shaman.pFlameShock &&
                    CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
                {
                    if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.shaman.pChainLightning &&
                    CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
                {
                    if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.shaman.pLightningBolt &&
                    CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
                {
                    if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                        return;
                }
            }
            else if (!me->CanReachWithMeleeAutoAttack(pVictim))
            {
                // Melee fallback: Lightning Bolt when can't reach
                if (m_spells.shaman.pLightningBolt &&
                    CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
                {
                    if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                        return;
                }
            }

            // ── DPS: totems AFTER damage rotation ──────────────────────────
            if (SummonShamanTotems())
                return;
        }
        else
        {
            // Healer: interrupt even on their own victim (fallback path)
            if (Unit* pInterruptTarget = SelectShamanInterruptTarget(this))
            {
                if (DoCastSpell(pInterruptTarget, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                    return;
            }
        }
    }

    if (m_spells.shaman.pCureDisease &&
        CanTryToCastSpell(me, m_spells.shaman.pCureDisease) &&
        IsValidDispelTarget(me, m_spells.shaman.pCureDisease))
    {
        if (DoCastSpell(me, m_spells.shaman.pCureDisease) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.shaman.pCurePoison &&
        CanTryToCastSpell(me, m_spells.shaman.pCurePoison) &&
        IsValidDispelTarget(me, m_spells.shaman.pCurePoison))
    {
        if (DoCastSpell(me, m_spells.shaman.pCurePoison) == SPELL_CAST_OK)
            return;
    }

    FindAndHealInjuredAlly(40.0f);
}

void BattleBotAI::UpdateOutOfCombatAI_Hunter()
{
    if (m_spells.hunter.pAspectOfTheCheetah &&
       !me->InBattleGround() &&
       !me->IsMounted() &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah))
    {
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheCheetah) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (Unit* pMarkTarget = SelectHunterRogueMarkTarget(this))
        {
            if (DoCastSpell(pMarkTarget, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        if (Pet* pPet = me->GetPet())
        {
            Unit* pPetTarget = SelectHunterPetProtectTarget(this);
            if (!pPetTarget)
                pPetTarget = pVictim;

            if (pPetTarget && pPet->GetVictim() != pPetTarget)
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pPetTarget);
            }
        }

        UpdateInCombatAI_Hunter();
    }
}

void BattleBotAI::UpdateInCombatAI_Hunter()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (Pet* pPet = me->GetPet())
        {
            Unit* pPetTarget = SelectHunterPetProtectTarget(this);
            if (!pPetTarget)
                pPetTarget = pVictim;

            if (pPetTarget && pPet->GetVictim() != pPetTarget)
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pPetTarget);
            }
        }

        if (me->GetDistance(pVictim) > 30.0f)
        {
            if (Unit* pNewVictim = SelectHunterNearbyDpsTarget(this, pVictim))
            {
                AttackStart(pNewVictim);
                return;
            }

            me->AttackStop(false);
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                StopMoving();
            return;
        }

        if (Unit* pMarkTarget = SelectHunterRogueMarkTarget(this))
        {
            if (DoCastSpell(pMarkTarget, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(BB_SPELL_AUTO_SHOT) &&
            !me->IsMoving() &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {
            switch (me->CastSpell(pVictim, BB_SPELL_AUTO_SHOT, false))
            {
                case SPELL_FAILED_NEED_AMMO:
                case SPELL_FAILED_NO_AMMO:
                {
                    AddHunterAmmo();
                    break;
                }
            }
        }

        if (Unit* pConcussiveTarget = SelectHunterConcussiveShotTarget(this))
        {
            if (DoCastSpell(pConcussiveTarget, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.hunter.pMultiShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {
            if (me->HasUnitState(UNIT_STATE_ROOT))
            {
                if (m_spells.hunter.pMongooseBite &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pMongooseBite) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.hunter.pRaptorStrike &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.hunter.pWingClip &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pWingClip))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pWingClip) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.hunter.pDisengage &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pDisengage))
                {
                    if (DoCastSpell(pVictim, m_spells.hunter.pDisengage) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (!me->HasUnitState(UNIT_STATE_ROOT) &&
            (me->GetCombatDistance(pVictim) < 8.0f) &&
             me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE)
        {
            if (!me->IsStopped())
                me->StopMoving();
            me->GetMotionMaster()->Clear();
            if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                return;
        }
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Mage()
{
    if (m_spells.mage.pArcaneBrilliance)
    {
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneBrilliance))
        {
            if (DoCastSpell(me, m_spells.mage.pArcaneBrilliance) == SPELL_CAST_OK)
                return;
        }
    }
    else if (m_spells.mage.pArcaneIntellect)
    {
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneIntellect))
        {
            if (DoCastSpell(me, m_spells.mage.pArcaneIntellect) == SPELL_CAST_OK)
                return;
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();
}

void BattleBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {
        bool const useBurst = BattleBotShouldUseMageBurst(this, pVictim);

        if (m_spells.mage.pArcanePower &&
            useBurst &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPresenceOfMind &&
            useBurst &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pCombustion &&
            useBurst &&
            m_spells.mage.pPresenceOfMind &&
           (me->HasAura(m_spells.mage.pPresenceOfMind->Id) || me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pPyroblast &&
            m_spells.mage.pPresenceOfMind &&
            me->HasAura(m_spells.mage.pPresenceOfMind->Id) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 25.0f) &&
           (!me->GetAttackers().empty() || GetIncomingdamage(me) > 0) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pManaShield &&
            BattleBotMageHasPhysicalPressure(this, pVictim) &&
           (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pManaShield))
        {
            if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pCounterspellTarget = SelectMageCounterspellTarget(this, pVictim))
        {
            if (DoCastSpell(pCounterspellTarget, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
            !BattleBotMageHasDamageSpellInRange(this, pVictim) &&
            me->GetDistance(pVictim) > 30.0f)
        {
            me->GetMotionMaster()->MoveChase(pVictim, 29.0f);
        }
        else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                (pVictim->GetVictim() == me) &&
                (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
        {
            bool rootedTarget = pVictim->HasUnitState(UNIT_STATE_ROOT) ||
                pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL);

            if (!rootedTarget &&
                m_spells.mage.pFrostNova &&
                CanTryToCastSpell(me, m_spells.mage.pFrostNova))
            {
                if (DoCastSpell(me, m_spells.mage.pFrostNova) == SPELL_CAST_OK)
                {
                    me->GetMotionMaster()->MoveDistance(pVictim, 29.0f);
                    return;
                }
            }

            if (m_spells.mage.pBlink &&
               (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ||
                pVictim->CanReachWithMeleeAutoAttack(me)) &&
                CanTryToCastSpell(me, m_spells.mage.pBlink))
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                    me->GetMotionMaster()->MoveIdle();

                if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pConeofCold &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;
            }

            if (!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
            {
                if (me->GetMotionMaster()->MoveDistance(pVictim, 29.0f))
                    return;
            }
        }

        if (GetAttackersInRangeCount(10.0f) > 1)
        {
            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pBlizzard &&
               (me->GetHealthPercent() > 40.0f) &&
               (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE) &&
                CanTryToCastSpell(pVictim, m_spells.mage.pBlizzard))
            {
                if (DoCastSpell(pVictim, m_spells.mage.pBlizzard) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.mage.pArcaneExplosion &&
               (me->GetHealthPercent() > 50.0f || pVictim->GetHealthPercent() < 20.0f) &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
        {
            if (m_spells.mage.pFireBlast &&
                CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
            {
                DoCastSpell(pVictim, m_spells.mage.pFireBlast);
            }
            return;
        }

        if (m_spells.mage.pRemoveLesserCurse &&
           (me->GetAttackers().size() < 3) &&
            CanTryToCastSpell(me, m_spells.mage.pRemoveLesserCurse) &&
            IsValidDispelTarget(me, m_spells.mage.pRemoveLesserCurse))
        {
            if (DoCastSpell(me, m_spells.mage.pRemoveLesserCurse) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pPolymorphTarget = SelectMagePolymorphTarget(this, pVictim))
        {
            if (DoCastSpell(pPolymorphTarget, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireBlast &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(BB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, BB_SPELL_SHOOT_WAND, false);
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Priest()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {
        if (m_spells.priest.pPrayerofFortitude)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofFortitude))
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofFortitude))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofFortitude) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pPrayerofSpirit)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofSpirit))
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofSpirit))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofSpirit) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pShadowProtection)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection))
            {
                if (CanTryToCastSpell(pTarget, m_spells.priest.pShadowProtection))
                {
                    if (DoCastSpell(pTarget, m_spells.priest.pShadowProtection) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }
    else if (bg && bg->GetStatus() == STATUS_IN_PROGRESS)
    {
        if (m_spells.priest.pPowerWordFortitude &&
            IsValidBuffTarget(me, m_spells.priest.pPowerWordFortitude) &&
            CanTryToCastSpell(me, m_spells.priest.pPowerWordFortitude))
        {
            if (DoCastSpell(me, m_spells.priest.pPowerWordFortitude) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.priest.pDivineSpirit &&
            IsValidBuffTarget(me, m_spells.priest.pDivineSpirit) &&
            CanTryToCastSpell(me, m_spells.priest.pDivineSpirit))
        {
            if (DoCastSpell(me, m_spells.priest.pDivineSpirit) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Priest();
}

void BattleBotAI::UpdateInCombatAI_Priest()
{
    if (m_role == ROLE_HEALER &&
        m_spells.priest.pShadowform &&
        me->HasAura(m_spells.priest.pShadowform->Id))
    {
        me->RemoveAurasDueToSpellByCancel(m_spells.priest.pShadowform->Id);
        return;
    }

    if (Unit* shieldTarget = SelectPriestPowerWordShieldTarget(this))
    {
        if (DoCastSpell(shieldTarget, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.priest.pInnerFocus &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    // Heal
    if (me->GetShapeshiftForm() == FORM_NONE &&
        FindAndHealInjuredAlly(m_role == ROLE_HEALER ? 60.0f : 40.0f, m_role == ROLE_HEALER ? 82.0f : 40.0f))
        return;

    if (m_role == ROLE_HEALER &&
        m_spells.priest.pFade &&
        GetAttackersInRangeCount(20.0f) > 1 &&
        CanTryToCastSpell(me, m_spells.priest.pFade))
    {
        if (DoCastSpell(me, m_spells.priest.pFade) == SPELL_CAST_OK)
            return;
    }

    // Dispels
    if (m_spells.priest.pDispelMagic)
    {
        if (m_role == ROLE_HEALER)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pDispelMagic))
            {
                if (CanTryToCastSpell(pFriend, m_spells.priest.pDispelMagic))
                {
                    if (DoCastSpell(pFriend, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pDispelMagic) &&
                 CanTryToCastSpell(me, m_spells.priest.pDispelMagic))
        {
            if (DoCastSpell(me, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                return;
        }
    }
    if (m_spells.priest.pAbolishDisease)
    {
        if (m_role == ROLE_HEALER)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pAbolishDisease))
            {
                if (CanTryToCastSpell(pFriend, m_spells.priest.pAbolishDisease))
                {
                    if (DoCastSpell(pFriend, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pAbolishDisease) &&
                 CanTryToCastSpell(me, m_spells.priest.pAbolishDisease))
        {
            if (DoCastSpell(me, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                return;
        }
    }

    // Attack
    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.priest.pShadowform &&
            m_role != ROLE_HEALER &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;
        }

        if (Unit* silenceTarget = SelectPriestSilenceTarget(this, pVictim))
        {
            if (DoCastSpell(silenceTarget, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pVampiricEmbrace &&
            m_role != ROLE_HEALER &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindBlast &&
            m_role != ROLE_HEALER &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pShadowWordPain &&
            m_role != ROLE_HEALER &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pDevouringPlague &&
            m_role != ROLE_HEALER &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;
        }

        if (BattleBotShouldUsePsychicScream(this))
        {
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.priest.pMindFlay &&
            m_role != ROLE_HEALER &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {
            if (m_spells.priest.pHolyNova &&
                m_role == ROLE_HEALER &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;
            }
        }

        if (me->HasSpell(BB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < (m_role == ROLE_HEALER ? 30.0f : 5.0f)) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, BB_SPELL_SHOOT_WAND, false);
    }

}

void BattleBotAI::UpdateOutOfCombatAI_Warlock()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {
        if (m_spells.warlock.pDetectInvisibility)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
            {
                if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
                {
                    if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {
        m_isBuffing = false;
    }

    if (m_spells.warlock.pLifeTap &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
       (me->GetHealthPercent() > 85.0f) &&
        CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
    {
        if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
            return;
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (Pet* pPet = me->GetPet())
        {
            Unit* pPetTarget = SelectWarlockPetProtectTarget(this);
            if (!pPetTarget)
                pPetTarget = pVictim;

            if (pPetTarget && pPet->GetVictim() != pPetTarget)
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pPetTarget);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();
}

void BattleBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (Pet* pPet = me->GetPet())
        {
            Unit* pPetTarget = SelectWarlockPetProtectTarget(this);
            if (!pPetTarget)
                pPetTarget = pVictim;

            if (pPetTarget && pPet->GetVictim() != pPetTarget)
            {
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pPetTarget);
            }
        }

        if (m_spells.warlock.pHowlofTerror &&
            GetAttackersInRangeCount(10.0f) > 1 &&
            CanTryToCastSpell(me, m_spells.warlock.pHowlofTerror))
        {
            if (DoCastSpell(me, m_spells.warlock.pHowlofTerror) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pDeathCoilTarget = SelectWarlockDeathCoilTarget(this, pVictim))
        {
            if (DoCastSpell(pDeathCoilTarget, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pFearTarget = SelectWarlockFearTarget(this, pVictim))
        {
            if (DoCastSpell(pFearTarget, m_spells.warlock.pFear) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowWard &&
           (pVictim->GetClass() == CLASS_WARLOCK) &&
            CanTryToCastSpell(me, m_spells.warlock.pShadowWard))
        {
            if (DoCastSpell(me, m_spells.warlock.pShadowWard) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pTonguesTarget = SelectWarlockCurseOfTonguesTarget(this, pVictim))
        {
            if (DoCastSpell(pTonguesTarget, m_spells.warlock.pCurseofTongues) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pExhaustionTarget = SelectWarlockCurseOfExhaustionTarget(this, pVictim))
        {
            if (DoCastSpell(pExhaustionTarget, m_spells.warlock.pCurseofExhaustion) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 35.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {
            me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 20.0f) &&
           (me->GetHealthPercent() > 75.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;
        }

        if (me->HasSpell(BB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, BB_SPELL_SHOOT_WAND, false);
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Warrior()
{
    // Need Battle Stance for Charge; tank will switch to Defensive Stance in combat
    if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);
        else if (m_spells.warrior.pBloodrage &&
            (me->GetPower(POWER_RAGE) < 10) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {
        if (m_spells.warrior.pCharge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;
        }
    }
}

// ── helpers used only inside UpdateInCombatAI_Warrior ──────────────────────
static inline bool WarriorCast(BattleBotAI* ai, Unit* pTarget, SpellEntry const* pSpell)
{
    if (!pSpell || !ai->CanTryToCastSpell(pTarget, pSpell))
        return false;
    return ai->DoCastSpell(pTarget, pSpell) == SPELL_CAST_OK;
}

void BattleBotAI::UpdateInCombatAI_Warrior()
{
    bool const isTank = (GetRole() == ROLE_TANK);

    if (Unit* pVictim = me->GetVictim())
    {
        // ── INTERRUPT (both roles) ──────────────────────────────────────────
        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {
            if (WarriorCast(this, pVictim, m_spells.warrior.pPummel))
                return;
            if (IsWearingShield(me) &&
                WarriorCast(this, pVictim, m_spells.warrior.pShieldBash))
                return;
        }

        if (isTank)
        {
            // ── TANK: survival cooldowns ────────────────────────────────────
            if (m_spells.warrior.pLastStand &&
                me->GetHealthPercent() < 20.0f &&
                WarriorCast(this, me, m_spells.warrior.pLastStand))
                return;

            if (IsWearingShield(me))
            {
                if (m_spells.warrior.pShieldWall &&
                    me->GetHealthPercent() < 40.0f &&
                    !me->GetAttackers().empty() &&
                    WarriorCast(this, me, m_spells.warrior.pShieldWall))
                    return;

                // ── TANK: threat generators ─────────────────────────────────
                if (WarriorCast(this, pVictim, m_spells.warrior.pShieldSlam))
                    return;
            }

            // ── TANK: control ───────────────────────────────────────────────
            if (WarriorCast(this, pVictim, m_spells.warrior.pConcussionBlow))
                return;

            // ── TANK: debuffs ───────────────────────────────────────────────
            // Apply Sunder Armor up to 5 stacks
            if (m_spells.warrior.pSunderArmor)
            {
                SpellAuraHolder const* sunderHolder = pVictim->GetSpellAuraHolder(m_spells.warrior.pSunderArmor->Id);
                bool const needsSunder = !sunderHolder || sunderHolder->GetStackAmount() < 5;
                if (needsSunder && WarriorCast(this, pVictim, m_spells.warrior.pSunderArmor))
                    return;
            }

            if (m_spells.warrior.pThunderClap &&
                IsPhysicalDamageClass(pVictim->GetClass()) &&
                WarriorCast(this, pVictim, m_spells.warrior.pThunderClap))
                return;

            if (m_spells.warrior.pDemoralizingShout &&
                IsPhysicalDamageClass(pVictim->GetClass()) &&
                !pVictim->HasAura(m_spells.warrior.pDemoralizingShout->Id) &&
                WarriorCast(this, pVictim, m_spells.warrior.pDemoralizingShout))
                return;

            // ── TANK: mitigation ────────────────────────────────────────────
            if (IsWearingShield(me) &&
                !me->GetAttackers().empty() &&
                IsPhysicalDamageClass(pVictim->GetClass()) &&
                WarriorCast(this, me, m_spells.warrior.pShieldBlock))
                return;

            if (m_spells.warrior.pDisarm &&
                IsMeleeWeaponClass(pVictim->GetClass()) &&
                WarriorCast(this, pVictim, m_spells.warrior.pDisarm))
                return;

            // ── TANK: slows ─────────────────────────────────────────────────
            if (pVictim->IsMoving() &&
               !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
               !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
            {
                if (WarriorCast(this, pVictim, m_spells.warrior.pHamstring))
                    return;
                if (m_spells.warrior.pPiercingHowl &&
                    me->GetCombatDistance(pVictim) <= 10.0f &&
                    WarriorCast(this, pVictim, m_spells.warrior.pPiercingHowl))
                    return;
            }

            // ── TANK: area fear when overwhelmed ────────────────────────────
            if (m_spells.warrior.pIntimidatingShout &&
                me->GetHealthPercent() < 50.0f &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                WarriorCast(this, pVictim, m_spells.warrior.pIntimidatingShout))
                return;

            // ── TANK: execute ───────────────────────────────────────────────
            if (m_spells.warrior.pExecute &&
                pVictim->GetHealthPercent() < 20.0f &&
                WarriorCast(this, pVictim, m_spells.warrior.pExecute))
                return;

            // ── TANK: rage dump ─────────────────────────────────────────────
            if (m_spells.warrior.pCleave &&
                GetAttackersInRangeCount(10.0f) > 1 &&
                me->GetPower(POWER_RAGE) > 25 &&
                WarriorCast(this, pVictim, m_spells.warrior.pCleave))
                return;

            if (m_spells.warrior.pHeroicStrike &&
                me->GetPower(POWER_RAGE) > 30 &&
                WarriorCast(this, pVictim, m_spells.warrior.pHeroicStrike))
                return;

            // ── TANK: stay in Defensive Stance ─────────────────────────────
            if (IsWearingShield(me) &&
                m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);

            // ── TANK: close distance (e.g. after fear) ──────────────────────
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                me->GetMotionMaster()->MoveChase(pVictim);
            }
        }
        else // DPS
        {
            // ── DPS: execute ────────────────────────────────────────────────
            if (m_spells.warrior.pExecute &&
                pVictim->GetHealthPercent() < 20.0f &&
                WarriorCast(this, pVictim, m_spells.warrior.pExecute))
                return;

            // ── DPS: proc ──────────────────────────────────────────────────
            if (WarriorCast(this, pVictim, m_spells.warrior.pOverpower))
                return;

            // ── DPS: survival ───────────────────────────────────────────────
            if (m_spells.warrior.pLastStand &&
                me->GetHealthPercent() < 20.0f &&
                WarriorCast(this, me, m_spells.warrior.pLastStand))
                return;

            // ── DPS: offensive cooldowns ────────────────────────────────────
            // Berserker Rage: fear immunity + rage gen — use broadly in Berserker Stance
            if (WarriorCast(this, me, m_spells.warrior.pBerserkerRage))
                return;

            // Recklessness (Battle Stance): vs casters who might fear/kite
            if (m_spells.warrior.pRecklessness &&
                !IsPhysicalDamageClass(pVictim->GetClass()) &&
                WarriorCast(this, me, m_spells.warrior.pRecklessness))
                return;

            // Death Wish (Berserker Stance): vs casters
            if (m_spells.warrior.pDeathWish &&
                !IsPhysicalDamageClass(pVictim->GetClass()) &&
                WarriorCast(this, me, m_spells.warrior.pDeathWish))
                return;

            // Retaliation (Battle Stance): vs melee multi-attack
            if (m_spells.warrior.pRetaliation &&
                IsMeleeDamageClass(pVictim->GetClass()) &&
                (GetAttackersInRangeCount(10.0f) > 1 || pVictim->GetClass() == CLASS_ROGUE) &&
                WarriorCast(this, me, m_spells.warrior.pRetaliation))
                return;

            // ── DPS: primary damage rotation ────────────────────────────────
            if (WarriorCast(this, pVictim, m_spells.warrior.pMortalStrike))
                return;

            if (WarriorCast(this, pVictim, m_spells.warrior.pBloodthirst))
                return;

            if (WarriorCast(this, pVictim, m_spells.warrior.pWhirlwind))
                return;

            // ── DPS: control ────────────────────────────────────────────────
            if (m_spells.warrior.pConcussionBlow &&
                (pVictim->IsNonMeleeSpellCasted() || me->GetHealthPercent() < 50.0f) &&
                WarriorCast(this, pVictim, m_spells.warrior.pConcussionBlow))
                return;

            if (pVictim->IsMoving() &&
               !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
               !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
            {
                if (WarriorCast(this, pVictim, m_spells.warrior.pHamstring))
                    return;
                if (m_spells.warrior.pPiercingHowl &&
                    me->GetCombatDistance(pVictim) <= 10.0f &&
                    WarriorCast(this, pVictim, m_spells.warrior.pPiercingHowl))
                    return;
            }

            // Rend: bleed DoT vs physical non-undead
            if (m_spells.warrior.pRend &&
                IsPhysicalDamageClass(pVictim->GetClass()) &&
                pVictim->GetRace() != RACE_UNDEAD &&
                WarriorCast(this, pVictim, m_spells.warrior.pRend))
                return;

            // Area fear when surrounded and low
            if (m_spells.warrior.pIntimidatingShout &&
                me->GetHealthPercent() < 50.0f &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                WarriorCast(this, pVictim, m_spells.warrior.pIntimidatingShout))
                return;

            // ── DPS: situational ────────────────────────────────────────────
            // Sunder Armor vs plate wearers
            if (m_spells.warrior.pSunderArmor &&
                (pVictim->GetClass() == CLASS_WARRIOR || pVictim->GetClass() == CLASS_PALADIN) &&
                me->GetPower(POWER_RAGE) > 20 &&
                WarriorCast(this, pVictim, m_spells.warrior.pSunderArmor))
                return;

            if (m_spells.warrior.pDisarm &&
                IsMeleeWeaponClass(pVictim->GetClass()) &&
                WarriorCast(this, pVictim, m_spells.warrior.pDisarm))
                return;

            // ── DPS: gap closer ─────────────────────────────────────────────
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                !me->CanReachWithMeleeAutoAttack(pVictim))
            {
                me->GetMotionMaster()->MoveChase(pVictim);
            }

            if (WarriorCast(this, pVictim, m_spells.warrior.pIntercept))
                return;

            // ── DPS: rage dump ──────────────────────────────────────────────
            if (m_spells.warrior.pCleave &&
                GetAttackersInRangeCount(10.0f) > 1 &&
                me->GetPower(POWER_RAGE) > 25 &&
                WarriorCast(this, pVictim, m_spells.warrior.pCleave))
                return;

            if (m_spells.warrior.pHeroicStrike &&
                me->GetPower(POWER_RAGE) > 30 &&
                WarriorCast(this, pVictim, m_spells.warrior.pHeroicStrike))
                return;

            // ── DPS: stance management ──────────────────────────────────────
            if (me->GetHealthPercent() < 20.0f)
            {
                if (m_spells.warrior.pDefensiveStance &&
                    CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
                    DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
            else
            {
                if (m_spells.warrior.pBerserkerStance &&
                    pVictim->GetClass() != CLASS_ROGUE &&
                    CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
                    DoCastSpell(me, m_spells.warrior.pBerserkerStance);
            }
        }
    }
    else // no victim
    {
        if (m_spells.warrior.pBattleShout &&
            WarriorCast(this, me, m_spells.warrior.pBattleShout))
            return;
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Rogue()
{
    // --- 在这里插入拦截逻辑 ---
    // 只有在【没有目标】且【骑马】时才拦截，防止骑马时尝试潜行导致下马
    if (me->IsMounted() && !me->GetVictim())
        return;
    // -----------------------

    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;
    }

    if (m_spells.rogue.pStealth &&
        !me->IsMounted() &&               // <--- 新增：如果已经骑在马上，绝对不要再尝试潜行
        CanTryToCastSpell(me, m_spells.rogue.pStealth) &&
       !me->HasAura(AURA_WARSONG_FLAG) &&
       !me->HasAura(AURA_SILVERWING_FLAG))
    {
        if (DoCastSpell(me, m_spells.rogue.pStealth) == SPELL_CAST_OK)
            return;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Rogue();
}

void BattleBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;
                }
            }
            else
            {
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;
                }
            }
        }
        else
        {
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish->Id) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                    {
                        if (me->GetMotionMaster()->MoveDistance(pVictim, 40.0f))
                            return;
                    }
                }
            }
        }

        bool const nearOpenObjective = BattleBotIsNearOpenObjectiveFlag(this, 18.0f);
        if (nearOpenObjective)
        {
            if (Unit* pBlindTarget = SelectRogueBlindTarget(this, pVictim, true))
            {
                if (DoCastSpell(pBlindTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                {
                    me->AttackStop();
                    if (pBlindTarget != pVictim)
                        AttackStart(pVictim);
                    return;
                }
            }
        }

        if (Unit* pKickTarget = SelectRogueInterruptTarget(this, m_spells.rogue.pKick))
        {
            if (DoCastSpell(pKickTarget, m_spells.rogue.pKick) == SPELL_CAST_OK)
                return;
        }

        if (Unit* pGougeTarget = SelectRogueInterruptTarget(this, m_spells.rogue.pGouge))
        {
            if (DoCastSpell(pGougeTarget, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                return;
        }

        if (me->GetComboPoints() > 4)
        {
            if ((pVictim->IsCaster() || CombatBotBaseAI::IsHealerClass(pVictim->GetClass())) &&
                m_spells.rogue.pKidneyShot &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKidneyShot))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pKidneyShot) == SPELL_CAST_OK)
                    return;
            }

            if (pVictim->GetHealthPercent() < 45.0f &&
                TryRogueEviscerate(this, pVictim, true))
                return;

            if (m_spells.rogue.pSliceAndDice &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pSliceAndDice))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pSliceAndDice) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pExposeArmor &&
                pVictim->GetHealthPercent() > 70.0f &&
               (pVictim->GetClass() == CLASS_WARRIOR || pVictim->GetClass() == CLASS_PALADIN) &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pExposeArmor))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pExposeArmor) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pRupture &&
                pVictim->GetHealthPercent() > 50.0f &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pRupture))
            {
                if (DoCastSpell(pVictim, m_spells.rogue.pRupture) == SPELL_CAST_OK)
                    return;
            }

            if (TryRogueEviscerate(this, pVictim, false))
                return;
        }

        if (Unit* pBlindTarget = SelectRogueBlindTarget(this, pVictim, false))
        {
            if (DoCastSpell(pBlindTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
            {
                me->AttackStop();
                AttackStart(pVictim);
                return;
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
           (me->GetPower(POWER_ENERGY) < 35) &&
           (pVictim->GetHealthPercent() > 40.0f) &&
            me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.rogue.pBladeFlurry &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateOutOfCombatAI_Druid()
{
    // 1. 如果正在读条（比如正在上马），绝对不要做任何动作打断自己
    if (me->IsNonMeleeSpellCasted(false))
        return;

    // 2. 核心优化：如果你骑着马，且【没有】攻击目标，才拦截后续的变身和加Buff逻辑
    // 这样如果一旦有了 Victim（被怪打了或者决定开怪），逻辑就能向下走，正常触发战斗
    if (me->IsMounted() && !me->GetVictim())
        return;

    if (me->HasAura(BB_SPELL_FOOD) || me->HasAura(BB_SPELL_DRINK))
        return;

    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetTypeID() == BATTLEGROUND_WS && BattleBotIsWSGHomeGuardCandidate(this) && !me->GetVictim())
        return;

    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {
        if (m_spells.druid.pGiftoftheWild)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pGiftoftheWild))
            {
                if (CanTryToCastSpell(pTarget, m_spells.druid.pGiftoftheWild))
                {
                    if (DoCastSpell(pTarget, m_spells.druid.pGiftoftheWild) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.druid.pThorns)
        {
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
            {
                if (CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
                {
                    if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                    {
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_isBuffing &&
           (!m_spells.druid.pGiftoftheWild || !me->HasGCD(m_spells.druid.pGiftoftheWild)) &&
           (!m_spells.druid.pThorns || !me->HasGCD(m_spells.druid.pThorns)))
            m_isBuffing = false;

        return;
    }
    else
    {
        if (m_spells.druid.pMarkoftheWild && CanTryToCastSpell(me, m_spells.druid.pMarkoftheWild))
        {
            if (DoCastSpell(me, m_spells.druid.pMarkoftheWild) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.druid.pThorns && CanTryToCastSpell(me, m_spells.druid.pThorns))
        {
            if (DoCastSpell(me, m_spells.druid.pThorns) == SPELL_CAST_OK)
            {
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
        {
            if (BattleBotEnterDruidCombatForm(this))
                return;

            if (m_spells.druid.pCatForm &&
                !me->IsMounted() &&               // <--- 新增：如果已经骑在马上，绝不变猫
                CanTryToCastSpell(me, m_spells.druid.pCatForm))
            {
                if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                    return;
            }

            if (m_spells.druid.pBearForm &&
                CanTryToCastSpell(me, m_spells.druid.pBearForm))
            {
                if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                    return;
            }
        }
        else
        {
            if ((me->GetPowerPercent(POWER_MANA) >  80.0f) &&
                FindAndHealInjuredAlly(80.0f))
                return;
        }
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {
        if (m_spells.druid.pProwl &&
            !me->IsMounted() &&               // <--- 新增：如果已经骑在马上，绝不潜行
            CanTryToCastSpell(me, m_spells.druid.pProwl) &&
            !me->HasAura(AURA_WARSONG_FLAG) &&
            !me->HasAura(AURA_SILVERWING_FLAG))
        {
            if (DoCastSpell(me, m_spells.druid.pProwl) == SPELL_CAST_OK)
                return;
        }
    }

    if (me->GetVictim())
    {
        if (m_role == ROLE_RANGE_DPS &&
            BattleBotEnterDruidCombatForm(this))
            return;

        if (m_role == ROLE_RANGE_DPS &&
            m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;
        }

        UpdateInCombatAI_Druid();
    }
    else
    {
        if (m_spells.druid.pMoonkinForm &&
            me->GetShapeshiftForm() == FORM_MOONKIN)
            me->RemoveAurasDueToSpellByCancel(m_spells.druid.pMoonkinForm->Id);

        if (m_spells.druid.pTravelForm &&
           !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.druid.pTravelForm))
        {
            if (DoCastSpell(me, m_spells.druid.pTravelForm) == SPELL_CAST_OK)
                return;
        }
    }
}

void BattleBotAI::UpdateInCombatAI_Druid()
{
    if (m_spells.druid.pTravelForm &&
        me->GetShapeshiftForm() == FORM_TRAVEL)
        me->RemoveAurasDueToSpellByCancel(m_spells.druid.pTravelForm->Id);

    if (m_role == ROLE_HEALER &&
        me->GetShapeshiftForm() != FORM_NONE)
    {
        me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        return;
    }

    ShapeshiftForm const currentForm = me->GetShapeshiftForm();
    if ((currentForm == FORM_NONE || currentForm == FORM_MOONKIN) &&
        m_spells.druid.pEntanglingRoots)
    {
        if (Unit* pRootTarget = SelectDruidEntanglingRootsTarget(this, m_spells.druid.pEntanglingRoots))
        {
            if (DoCastSpell(pRootTarget, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
            {
                me->AddCooldown(*m_spells.druid.pEntanglingRoots, nullptr, false, 15000);
                return;
            }
        }
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {
        if (m_role == ROLE_HEALER)
        {
            if (TryDruidNatureSwiftnessHeal(this))
                return;

            if (FindAndHealInjuredAlly(60.0f, 90.0f))
                return;
        }
        else if (me->GetHealthPercent() < 55.0f)
        {
            if (TryDruidNatureSwiftnessHeal(this))
                return;

            if (HealInjuredTarget(me))
                return;
        }

        // Dispels
       SpellEntry const* pDispelSpell = m_spells.druid.pAbolishPoison ?
                                         m_spells.druid.pAbolishPoison :
                                         m_spells.druid.pCurePoison;
       if (pDispelSpell)
       {
           if (m_role == ROLE_HEALER)
           {
               if (Unit* pFriend = SelectDispelTarget(pDispelSpell))
               {
                   if (CanTryToCastSpell(pFriend, pDispelSpell))
                   {
                       if (DoCastSpell(pFriend, pDispelSpell) == SPELL_CAST_OK)
                           return;
                   }
               }
           }
           else if (IsValidDispelTarget(me, pDispelSpell) &&
                    CanTryToCastSpell(me, pDispelSpell))
           {
               if (DoCastSpell(me, pDispelSpell) == SPELL_CAST_OK)
                   return;
           }
       }

        if (m_spells.druid.pRemoveCurse)
        {
            if (Unit* pFriend = SelectDispelTarget(m_spells.druid.pRemoveCurse))
            {
                if (CanTryToCastSpell(pFriend, m_spells.druid.pRemoveCurse))
                {
                    if (DoCastSpell(pFriend, m_spells.druid.pRemoveCurse) == SPELL_CAST_OK)
                        return;
                }
            }
        }

        if (Unit* pInnervateTarget = SelectDruidInnervateTarget(this))
        {
            if (DoCastSpell(pInnervateTarget, m_spells.druid.pInnervate) == SPELL_CAST_OK)
                return;
        }

        if (m_role == ROLE_HEALER &&
            m_spells.druid.pEntanglingRoots)
        {
            if (Unit* pRootTarget = SelectDruidEntanglingRootsTarget(this, m_spells.druid.pEntanglingRoots))
            {
                if (DoCastSpell(pRootTarget, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                {
                    me->AddCooldown(*m_spells.druid.pEntanglingRoots, nullptr, false, 15000);
                    return;
                }
            }
        }

        if (m_role == ROLE_HEALER &&
            FindAndPreHealTarget())
            return;

        if (m_role == ROLE_RANGE_DPS &&
            m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;
        }

        if (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
        {
            if (BattleBotEnterDruidCombatForm(this))
                return;

            if (Unit* pVictim = me->GetVictim())
            {
                if (m_spells.druid.pBearForm &&
                    pVictim->CanReachWithMeleeAutoAttack(me) &&
                    IsPhysicalDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(me, m_spells.druid.pBearForm))
                {
                    if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pCatForm &&
                    CanTryToCastSpell(me, m_spells.druid.pCatForm))
                {
                    if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                        return;
                }
            }
        }
    }
    else
    {
        if (me->HasUnitState(UNIT_STATE_ROOT) &&
            me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
    }

    if (Unit* pVictim = me->GetVictim())
    {
        ShapeshiftForm const form = me->GetShapeshiftForm();
        if (m_spells.druid.pBarkskin &&
           (form == FORM_NONE || form == FORM_MOONKIN) &&
           (me->GetHealthPercent() < 50.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pBarkskin))
        {
            if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
                return;
        }

        if (m_role == ROLE_HEALER)
        {
            // Healer druids: cast Moonfire/Wrath in caster form only.
            if (form == FORM_NONE)
            {
                if (m_spells.druid.pMoonfire &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pWrath &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                        return;
                }
            }
            return;
        }

        switch (form)
        {
            case FORM_CAT:
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {
                    me->GetMotionMaster()->MoveChase(pVictim);
                }

                if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
                {
                    if (m_spells.druid.pPounce &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                            return;
                    }
                    if (m_spells.druid.pRavage &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                            return;
                    }
                    if (m_spells.druid.pTigersFury &&
                        CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                    {
                        if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                            return;
                    }
                    return;
                }

                if (me->GetComboPoints() > 4)
                {
                    bool const hasRip = m_spells.druid.pRip && pVictim->HasAura(m_spells.druid.pRip->Id);
                    if (!hasRip &&
                        m_spells.druid.pRip &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                            return;
                    }

                    if (m_spells.druid.pFerociousBite &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                    {
                        if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (m_spells.druid.pFaerieFireFeral)
                {
                    if (Unit* pFaerieTarget = SelectDruidFaerieFireTarget(this, m_spells.druid.pFaerieFireFeral))
                    {
                        if (DoCastSpell(pFaerieTarget, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (!me->CanReachWithMeleeAutoAttack(pVictim))
                {
                    if (m_spells.druid.pDash &&
                        pVictim->IsMoving() &&
                        CanTryToCastSpell(me, m_spells.druid.pDash))
                    {
                        if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (m_spells.druid.pTigersFury &&
                    CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                {
                    if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pShred &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pShred))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pRake &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRake))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pClaw &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                        return;
                }

                break;
            }
            case FORM_BEAR:
            case FORM_DIREBEAR:
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {
                    me->GetMotionMaster()->MoveChase(pVictim);
                }

                if (m_spells.druid.pFeralCharge)
                {
                    if (Unit* pInterruptTarget = SelectDruidInterruptTarget(this, m_spells.druid.pFeralCharge))
                    {
                        if (DoCastSpell(pInterruptTarget, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (m_spells.druid.pBash)
                {
                    if (Unit* pInterruptTarget = SelectDruidInterruptTarget(this, m_spells.druid.pBash))
                    {
                        if (DoCastSpell(pInterruptTarget, m_spells.druid.pBash) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (m_spells.druid.pFeralCharge &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pBash &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pBash))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pFrenziedRegeneration &&
                   (me->GetHealthPercent() < 50.0f) &&
                    CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
                {
                    if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pFaerieFireFeral)
                {
                    if (Unit* pFaerieTarget = SelectDruidFaerieFireTarget(this, m_spells.druid.pFaerieFireFeral))
                    {
                        if (DoCastSpell(pFaerieTarget, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (m_spells.druid.pDemoralizingRoar &&
                    IsMeleeDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pSwipe &&
                   ((me->GetPower(POWER_RAGE) > 50) || (GetAttackersInRangeCount(10.0f) > 1)) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pMaul &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                        return;
                }
                break;
            }
            case FORM_NONE:
            case FORM_MOONKIN:
            {
                if (m_spells.druid.pEntanglingRoots)
                {
                    if (Unit* pRootTarget = SelectDruidEntanglingRootsTarget(this, m_spells.druid.pEntanglingRoots))
                    {
                        if (DoCastSpell(pRootTarget, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                        {
                            me->AddCooldown(*m_spells.druid.pEntanglingRoots, nullptr, false, 15000);
                            return;
                        }
                    }
                }

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                    me->GetDistance(pVictim) > 30.0f)
                {
                    me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
                }
                else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                        (pVictim->GetVictim() == me) &&
                        !me->HasUnitState(UNIT_STATE_ROOT) &&
                        (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
                {
                    me->SetCasterChaseDistance(25.0f);
                    if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                        return;
                }

                if (m_spells.druid.pNaturesGrasp &&
                    pVictim->CanReachWithMeleeAutoAttack(me) &&
                    !me->HasAura(m_spells.druid.pNaturesGrasp->Id) &&
                    CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
                {
                    if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pFaerieFire)
                {
                    if (Unit* pFaerieTarget = SelectDruidFaerieFireTarget(this, m_spells.druid.pFaerieFire))
                    {
                        if (DoCastSpell(pFaerieTarget, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                            return;
                    }
                }

                if (m_spells.druid.pInsectSwarm &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                        return;
                }

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
                    return;

                if (m_spells.druid.pMoonfire &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pStarfire &&
                   (pVictim->GetHealthPercent() > 30.0f) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                        return;
                }

                if (m_spells.druid.pWrath &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
                {
                    if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                        return;
                }

                break;
            }
        }
    }
}
