/* Copyright (C) 2006 - 2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
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

/* ScriptData
SDName: Felwood
SD%Complete: 95
SDComment: Quest support: related to 4101/4102 (To obtain Cenarion Beacon)
SDCategory: Felwood
EndScriptData */

/* ContentData
npcs_riverbreeze_and_silversky
EndContentData */

#include "scriptPCH.h"

enum
{
    SPELL_CURSED    = 13483,
    SPELL_TAINTED   = 3335,
    SPELL_QUEST_CURSED_JAR  = 15698,
    SPELL_QUEST_TAINTED_JAR = 15699
};

/*###############
# Cursed Oose
################*/

struct npc_cursed_oozeAI : public ScriptedAI
{
    explicit npc_cursed_oozeAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        Reset();
    }
    uint32 SpellTimer;
    void SpellHit(SpellCaster* caster, SpellEntry const* spell) override
    {
        if (spell && spell->Id == SPELL_QUEST_CURSED_JAR)
            m_creature->ForcedDespawn();
    }
    void UpdateAI(uint32 const diff) override
    {
        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
            return;
        if (SpellTimer < diff)
        {
            if (DoCastSpellIfCan(m_creature, SPELL_CURSED) == CAST_OK)
                SpellTimer = 60000;
        }
        else
            SpellTimer -= diff;
        DoMeleeAttackIfReady();
    }
    void Reset() override
    {
        SpellTimer = 3000;
    }
};

CreatureAI* GetAI_npc_cursed_ooze(Creature* pCreature)
{
    return new npc_cursed_oozeAI(pCreature);
}

/*###############
# Tainted Oose
################*/

struct npc_tainted_oozeAI : public ScriptedAI
{
    explicit npc_tainted_oozeAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        Reset();
    }
    uint32 SpellTimer;
    void SpellHit(SpellCaster* caster, SpellEntry const* spell) override
    {
        if (spell && spell->Id == SPELL_QUEST_TAINTED_JAR)
            m_creature->ForcedDespawn();
    }
    void UpdateAI(uint32 const diff) override
    {
        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
            return;
        if (SpellTimer < diff)
        {
            if (DoCastSpellIfCan(m_creature, SPELL_TAINTED) == CAST_OK)
                SpellTimer = 60000;
        }
        else
            SpellTimer -= diff;
        DoMeleeAttackIfReady();
    }
    void Reset() override
    {
        SpellTimer = 3000;
    }
};

CreatureAI* GetAI_npc_tainted_ooze(Creature* pCreature)
{
    return new npc_tainted_oozeAI(pCreature);
}

//================================SD2 IMPORT DEC 2014====================================================

/*####
# npc_captured_arkonarin
####*/

enum
{
    SAY_ESCORT_START                = -1001148,
    SAY_FIRST_STOP                  = -1001149,
    SAY_SECOND_STOP                 = -1001150,
    SAY_AGGRO                       = -1001151,
    SAY_FOUND_EQUIPMENT             = -1001152,
    SAY_ESCAPE_DEMONS               = -1001153,
    SAY_FRESH_AIR                   = -1001154,
    SAY_TREY_BETRAYER               = -1001155,
    SAY_TREY                        = -1001156,
    SAY_TREY_ATTACK                 = -1001157,
    SAY_ESCORT_COMPLETE             = -1001158,

    SPELL_STRENGTH_ARKONARIN        = 18163,
    SPELL_MORTAL_STRIKE             = 16856,
    SPELL_CLEAVE                    = 15496,

    QUEST_ID_RESCUE_JAEDENAR        = 5203,
    NPC_JAEDENAR_LEGIONNAIRE        = 9862,
    NPC_SPIRT_TREY                  = 11141,
    GO_ARKONARIN_CHEST              = 176225,
    GO_ARKONARIN_CAGE               = 176306,

    NPC_ARKO_NARIN                  = 11018
};

struct npc_captured_arkonarinAI : npc_escortAI
{
    explicit npc_captured_arkonarinAI(Creature* pCreature) : npc_escortAI(pCreature)
    {
        npc_captured_arkonarinAI::Reset();
    }

    ObjectGuid m_treyGuid;

    bool m_bCanAttack;

    uint32 m_uiMortalStrikeTimer;
    uint32 m_uiCleaveTimer;

    void Reset() override
    {
        if (!HasEscortState(STATE_ESCORT_ESCORTING))
            m_bCanAttack = false;

        m_uiMortalStrikeTimer = urand(5000, 7000);
        m_uiCleaveTimer = urand(1000, 4000);
    }

    void JustRespawned() override
    {
        m_creature->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC);
        npc_escortAI::JustRespawned();
    }

    void Aggro(Unit* pWho) override
    {
        if (pWho->GetEntry() == NPC_SPIRT_TREY)
            DoScriptText(SAY_TREY_ATTACK, m_creature);
        else if (roll_chance_i(25))
            DoScriptText(SAY_AGGRO, m_creature, pWho);
    }

    void JustSummoned(Creature* pSummoned) override
    {
        if (pSummoned->GetEntry() == NPC_JAEDENAR_LEGIONNAIRE)
            pSummoned->AI()->AttackStart(m_creature);
        else if (pSummoned->GetEntry() == NPC_SPIRT_TREY)
        {
            DoScriptText(SAY_TREY_BETRAYER, pSummoned);
            m_treyGuid = pSummoned->GetObjectGuid();
        }
    }

    void WaypointReached(uint32 uiPointId) override
    {
        switch (uiPointId)
        {
            case 0:
                if (Player* pPlayer = GetPlayerForEscort())
                    DoScriptText(SAY_ESCORT_START, m_creature, pPlayer);
                break;
            case 14:
                DoScriptText(SAY_FIRST_STOP, m_creature);
                break;
            case 34:
                DoScriptText(SAY_SECOND_STOP, m_creature);
                SetRun();
                break;
            case 38:
                if (GameObject* pChest = GetClosestGameObjectWithEntry(m_creature, GO_ARKONARIN_CHEST, 5.0f))
                    pChest->Use(m_creature);
                m_creature->HandleEmote(EMOTE_ONESHOT_KNEEL);
                break;
            case 39:
                DoCastSpellIfCan(m_creature, SPELL_STRENGTH_ARKONARIN);
                break;
            case 40:
                if (Player* pPlayer = GetPlayerForEscort())
                    m_creature->SetFacingToObject(pPlayer);
                m_bCanAttack = true;
                DoScriptText(SAY_FOUND_EQUIPMENT, m_creature);
                m_creature->UpdateEntry(NPC_ARKO_NARIN);
                m_creature->SetFactionTemporary(FACTION_ESCORT_N_NEUTRAL_ACTIVE, TEMPFACTION_RESTORE_RESPAWN);
                m_creature->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC);
                break;
            case 41:
                DoScriptText(SAY_ESCAPE_DEMONS, m_creature);
                m_creature->SummonCreature(NPC_JAEDENAR_LEGIONNAIRE, 5082.068f, -490.084f, 296.856f, 5.15f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 2 * MINUTE*IN_MILLISECONDS);
                m_creature->SummonCreature(NPC_JAEDENAR_LEGIONNAIRE, 5084.135f, -489.187f, 296.832f, 5.15f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 2 * MINUTE*IN_MILLISECONDS);
                m_creature->SummonCreature(NPC_JAEDENAR_LEGIONNAIRE, 5085.676f, -488.518f, 296.824f, 5.15f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 2 * MINUTE*IN_MILLISECONDS);
                break;
            case 43:
                SetRun(false);
                break;
            case 104:
                DoScriptText(SAY_FRESH_AIR, m_creature);
                break;
            case 105:
                m_creature->SummonCreature(NPC_SPIRT_TREY, 4844.839f, -395.763f, 350.603f, 6.25f, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 2 * MINUTE*IN_MILLISECONDS);
                break;
            case 106:
                DoScriptText(SAY_TREY, m_creature);
                break;
            case 107:
                if (Creature* pTrey = m_creature->GetMap()->GetCreature(m_treyGuid))
                    AttackStart(pTrey);
                break;
            case 108:
                if (Player* pPlayer = GetPlayerForEscort())
                    m_creature->SetFacingToObject(pPlayer);
                DoScriptText(SAY_ESCORT_COMPLETE, m_creature);
                break;
            case 109:
                if (Player* pPlayer = GetPlayerForEscort())
                    pPlayer->GroupEventHappens(QUEST_ID_RESCUE_JAEDENAR, m_creature);
                SetRun();
                break;
        }
    }

    void UpdateEscortAI(uint32 const uiDiff) override
    {
        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
            return;

        if (m_bCanAttack)
        {
            if (m_uiMortalStrikeTimer < uiDiff)
            {
                if (DoCastSpellIfCan(m_creature->GetVictim(), SPELL_MORTAL_STRIKE) == CAST_OK)
                    m_uiMortalStrikeTimer = urand(7000, 10000);
            }
            else
                m_uiMortalStrikeTimer -= uiDiff;

            if (m_uiCleaveTimer < uiDiff)
            {
                if (DoCastSpellIfCan(m_creature->GetVictim(), SPELL_CLEAVE) == CAST_OK)
                    m_uiCleaveTimer = urand(3000, 6000);
            }
            else
                m_uiCleaveTimer -= uiDiff;
        }

        DoMeleeAttackIfReady();
    }
};

CreatureAI* GetAI_npc_captured_arkonarin(Creature* pCreature)
{
    return new npc_captured_arkonarinAI(pCreature);
}

bool QuestAccept_npc_captured_arkonarin(Player* pPlayer, Creature* pCreature, Quest const* pQuest)
{
    if (pQuest->GetQuestId() == QUEST_ID_RESCUE_JAEDENAR)
    {
        if (auto pEscortAI = dynamic_cast<npc_captured_arkonarinAI*>(pCreature->AI()))
        {
            pEscortAI->Start(false, pPlayer->GetGUID(), pQuest);

            pCreature->SetStandState(UNIT_STAND_STATE_STAND);
            pCreature->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC);

            if (GameObject* pCage = GetClosestGameObjectWithEntry(pCreature, GO_ARKONARIN_CAGE, 5.0f))
                pCage->Use(pCreature);

        }
    }
    return true;
}

/*####
# npc_arei
####*/

enum
{
    SAY_AREI_ESCORT_START           = -1001159,
    SAY_ATTACK_IRONTREE             = -1001160,
    SAY_ATTACK_TOXIC_HORROR         = -1001161,
    SAY_EXIT_WOODS                  = -1001162,
    SAY_CLEAR_PATH                  = -1001163,//NPC_AREI,   4000
    SAY_ASHENVALE                   = -1001164,
    SAY_TRANSFORM                   = -1001165,
    SAY_LIFT_CURSE                  = -1001166,
    SAY_AREI_ESCORT_COMPLETE        = -1001167,

    SPELL_WITHER_STRIKE             = 5337,//5000
    SPELL_AREI_TRANSFORM            = 14888,

    NPC_AREI                        = 9598,
    NPC_TOXIC_HORROR                = 7132,
    NPC_IRONTREE_WANDERER           = 7138,
    NPC_IRONTREE_STOMPER            = 7139,
    QUEST_ID_ANCIENT_SPIRIT         = 4261,
};

struct npc_areiAI : public npc_escortAI
{
    npc_areiAI(Creature* pCreature) : npc_escortAI(pCreature)
    {
        m_bAggroIrontree = false;
        m_bAggroHorror = false;
        Reset();
    }

    uint32 m_uiWitherStrikeTimer;

    bool m_bAggroIrontree;
    bool m_bAggroHorror;

    uint16 dialogueStep;
    uint32 m_dialogueTimer;


    GuidList m_lSummonsGuids;

    void Reset() override
    {
        m_uiWitherStrikeTimer = urand(1000, 4000);
        if (!HasEscortState(STATE_ESCORT_ESCORTING))
        {
            dialogueStep = 6;
            m_dialogueTimer = 4000;
        }
    }

    void Aggro(Unit* pWho) override
    {
        if ((pWho->GetEntry() == NPC_IRONTREE_WANDERER || pWho->GetEntry() == NPC_IRONTREE_STOMPER) && !m_bAggroIrontree)
        {
            DoScriptText(SAY_ATTACK_IRONTREE, m_creature);
            m_bAggroIrontree = true;
        }
        else if (pWho->GetEntry() == NPC_TOXIC_HORROR && ! m_bAggroHorror)
        {
            if (Player* pPlayer = GetPlayerForEscort())
                DoScriptText(SAY_ATTACK_TOXIC_HORROR, m_creature, pPlayer);
            m_bAggroHorror = true;
        }
    }
    void Dialogue(uint32 const uiDiff)
    {
        if (dialogueStep > 5)
            return;
        if (m_dialogueTimer < uiDiff)
        {
            switch (dialogueStep)
            {
                case 1:
                    if (DoCastSpellIfCan(m_creature->GetVictim(), SPELL_WITHER_STRIKE) == CAST_OK)
                    {
                        if (Player* pPlayer = GetPlayerForEscort())
                            DoScriptText(SAY_ASHENVALE, m_creature, pPlayer);
                        m_dialogueTimer = 3000;
                    }
                    break;
                case 2:
                    DoScriptText(SAY_TRANSFORM, m_creature);
                    m_dialogueTimer = 7000;
                    break;
                case 3:
                    if (DoCastSpellIfCan(m_creature, SPELL_AREI_TRANSFORM) == CAST_OK)
                    {
                        if (Player* pPlayer = GetPlayerForEscort())
                            DoScriptText(SAY_LIFT_CURSE, m_creature, pPlayer);
                    }
                    m_dialogueTimer = 5000;
                    break;
                case 4:
                    if (Player* pPlayer = GetPlayerForEscort())
                    {
                        DoScriptText(SAY_AREI_ESCORT_COMPLETE, m_creature, pPlayer);
                        pPlayer->GroupEventHappens(QUEST_ID_ANCIENT_SPIRIT, m_creature);
                        SetEscortPaused(false);
                    }
                    break;
            }
            dialogueStep++;
        }
        else
            m_dialogueTimer  -= uiDiff;
    }
    void JustSummoned(Creature* pSummoned) override
    {
        switch (pSummoned->GetEntry())
        {
            case NPC_IRONTREE_STOMPER:
                DoScriptText(SAY_EXIT_WOODS, m_creature, pSummoned);
            // no break;
            case NPC_IRONTREE_WANDERER:
                pSummoned->AI()->AttackStart(m_creature);
                m_lSummonsGuids.push_back(pSummoned->GetObjectGuid());
                break;
        }
    }

    void SummonedCreatureJustDied(Creature* pSummoned) override
    {
        if (pSummoned->GetEntry() == NPC_IRONTREE_STOMPER || pSummoned->GetEntry() == NPC_IRONTREE_WANDERER)
        {
            m_lSummonsGuids.remove(pSummoned->GetObjectGuid());
            if (m_lSummonsGuids.empty())
                dialogueStep = 1;
        }
    }

    void WaypointReached(uint32 uiPointId) override
    {
        if (uiPointId == 36)
        {
            SetEscortPaused(true);

            m_creature->SummonCreature(NPC_IRONTREE_STOMPER, 6573.321f, -1195.213f, 442.489f, 0, TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT, 60000);
            m_creature->SummonCreature(NPC_IRONTREE_WANDERER, 6573.240f, -1213.475f, 443.643f, 0, TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT, 60000);
            m_creature->SummonCreature(NPC_IRONTREE_WANDERER, 6583.354f, -1209.811f, 444.769f, 0, TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT, 60000);
            DoScriptText(SAY_CLEAR_PATH, m_creature);
        }
    }

    Creature* GetSpeakerByEntry(uint32 uiEntry)
    {
        if (uiEntry == NPC_AREI)
            return m_creature;

        return nullptr;
    }

    void UpdateAI(uint32 const uiDiff) override
    {
        Dialogue(uiDiff);
        npc_escortAI::UpdateAI(uiDiff);
        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
            return;

        if (m_uiWitherStrikeTimer < uiDiff)
        {
            if (DoCastSpellIfCan(m_creature->GetVictim(), SPELL_WITHER_STRIKE) == CAST_OK)
                m_uiWitherStrikeTimer = urand(3000, 6000);
        }
        else
            m_uiWitherStrikeTimer -= uiDiff;

        DoMeleeAttackIfReady();
    }
};

CreatureAI* GetAI_npc_arei(Creature* pCreature)
{
    return new npc_areiAI(pCreature);
}

bool QuestAccept_npc_arei(Player* pPlayer, Creature* pCreature, Quest const* pQuest)
{
    if (pQuest->GetQuestId() == QUEST_ID_ANCIENT_SPIRIT)
    {
        if (npc_areiAI* pEscortAI = dynamic_cast<npc_areiAI*>(pCreature->AI()))
        {
            pEscortAI->Start(false, pPlayer->GetGUID(), pQuest);
            pCreature->SetFactionTemporary(FACTION_ESCORT_A_NEUTRAL_PASSIVE, TEMPFACTION_RESTORE_RESPAWN);
            DoScriptText(SAY_AREI_ESCORT_START, pCreature, pPlayer);
        }
    }
    return true;
}

// Corrupted plants

class go_corrupted_plantAI: public GameObjectAI
{
public:
    go_corrupted_plantAI(GameObject* gobject): GameObjectAI(gobject)
    {
        int entries[] =
        {
                164885, 164881,
                173324, 173325,
                174608, 174609,
                174684, 174685,
                164886, 164882,
                171939, 171940,
                171942, 171943,
                174594, 174612,
                174595, 174613,
                174596, 174614,
                174598, 174615,
                174712, 174714,
                174713, 174715,
                164888, 164883,
                173284, 174622,
                174605, 174623,
                174606, 174624,
                174607, 174625,
                174686, 174687,
                164887, 164884,
                173327, 173326,
                174599, 174616,
                174600, 174617,
                174601, 174618,
                174602, 174619,
                174603, 174620,
                174604, 174621,
                174708, 174710,
                174709, 174711,
                0,0
        };
        for (int i = 0; entries[i]; i += 2)
            if (entries[i] == me->GetEntry() || entries[i+1] == me->GetEntry())
            {
                cleansedEntry = entries[i+1];
                break;
            }
    }

    const static int CLEANSED_PLANT_RESPAWN_TIMER   = 25 * 60; // 25 minutes

    uint32 cleansedEntry;
    ObjectGuid cleansedGuid;

    void UpdateAI(uint32 const diff) override
    {
        if (me->isSpawned() && cleansedGuid)
        {
            GameObject* cleansed = me->GetMap()->GetGameObject(cleansedGuid);
            if (cleansed)
                cleansed->AddObjectToRemoveList();
            cleansedGuid.Clear();
        }
    }
    void PlantQuestRewarded()
    {
        if (GameObject* cleansed = me->SummonGameObject(cleansedEntry, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 0.0, 0.0, 0.0, 0.0, 0.0, CLEANSED_PLANT_RESPAWN_TIMER))
        {
            cleansedGuid = cleansed->GetObjectGuid();
            cleansed->SetSpawnedByDefault(true);
        }

        if (GameObjectData const* data = me->GetGOData())
            me->SetRespawnDelay(data->ComputeRespawnDelay(data->GetRandomRespawnTime()));

        me->Despawn();
        me->DestroyForNearbyPlayers();
    }
};

GameObjectAI* GetAI_go_corrupted_plant(GameObject* gobj)
{
    return new go_corrupted_plantAI(gobj);
}

bool QuestRewarded_go_corrupted_plant(Player* player, GameObject* gobj, Quest const* quest)
{
    if (gobj->AI())
        if (go_corrupted_plantAI* ai = dynamic_cast<go_corrupted_plantAI*>(gobj->AI()))
            ai->PlantQuestRewarded();
    return false;
}


/*####
# at_irontree_wood
####*/

enum
{
    NPC_VARTRUS_THE_ANCIENT = 14524,
    NPC_STOME_THE_ANCIENT   = 14525,
    NPC_HASTAT_THE_ANCIENT  = 14526,

    QUEST_THE_ANCIENT_LEAF  = 7632,

    AT_IRONTREE_WOOD        = 3587
};

bool AreaTrigger_at_irontree_wood(Player* pPlayer, AreaTriggerEntry const* pAt)
{
    if (pAt->id == AT_IRONTREE_WOOD)
    {
        if (pPlayer->GetClass() == CLASS_HUNTER && pPlayer->GetQuestStatus(QUEST_THE_ANCIENT_LEAF) == QUEST_STATUS_COMPLETE)
        {
            Creature* pAncient = GetClosestCreatureWithEntry(pPlayer, NPC_HASTAT_THE_ANCIENT, 100.0f);
            if (!pAncient)
            {
                pPlayer->SummonCreature(NPC_VARTRUS_THE_ANCIENT, 6194.55f, -1176.35f, 369.056f, 1.1098f, TEMPSUMMON_TIMED_DESPAWN, 10*MINUTE*IN_MILLISECONDS);
                pPlayer->SummonCreature(NPC_STOME_THE_ANCIENT,   6197.12f, -1135.42f, 366.31f, 5.28025f, TEMPSUMMON_TIMED_DESPAWN, 10*MINUTE*IN_MILLISECONDS);
                pPlayer->SummonCreature(NPC_HASTAT_THE_ANCIENT,  6245.91f, -1165.98f, 366.325f, 2.60598f, TEMPSUMMON_TIMED_DESPAWN, 10*MINUTE*IN_MILLISECONDS);
            }

            return true;
        }
    }

    return false;
}

// 6946 - Curse of the Bleakheart
struct CurseOfTheBleakheartScript : public AuraScript
{
    void OnBeforeApply(Aura* aura, bool apply) final
    {
        if (apply && aura->GetEffIndex() == EFFECT_INDEX_0)
            aura->SetPeriodicTimer(5 * IN_MILLISECONDS);
    }

    void OnPeriodicDummy(Aura* aura) final
    {
        if (roll_chance_i(5))
            aura->GetTarget()->CastSpell(aura->GetTarget(), 6945, true, nullptr, aura);
    }
};

AuraScript* GetScript_CurseOfTheBleakheart(SpellEntry const*)
{
    return new CurseOfTheBleakheartScript();
}

void AddSC_felwood()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "npc_cursed_ooze";
    newscript->GetAI = &GetAI_npc_cursed_ooze;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_tainted_ooze";
    newscript->GetAI = &GetAI_npc_tainted_ooze;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_captured_arkonarin";
    newscript->GetAI = &GetAI_npc_captured_arkonarin;
    newscript->pQuestAcceptNPC = &QuestAccept_npc_captured_arkonarin;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_arei";
    newscript->GetAI = &GetAI_npc_arei;
    newscript->pQuestAcceptNPC = &QuestAccept_npc_arei;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "go_corrupted_plant";
    newscript->GOGetAI = &GetAI_go_corrupted_plant;
    newscript->pQuestRewardedGO = &QuestRewarded_go_corrupted_plant;
    newscript->RegisterSelf();
    
    newscript = new Script;
    newscript->Name = "at_irontree_wood";
    newscript->pAreaTrigger = &AreaTrigger_at_irontree_wood;
    newscript->RegisterSelf(); 

    newscript = new Script;
    newscript->Name = "spell_curse_of_bleakheart";
    newscript->GetAuraScript = &GetScript_CurseOfTheBleakheart;
    newscript->RegisterSelf();
}
