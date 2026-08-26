#include "scriptPCH.h"
#include "molten_core.h"
#include "Utilities/EventMap.h"

enum Garr : uint32
{
    // Spells
    SPELL_ANTIMAGICPULSE     = 19492,
    SPELL_MAGMASHACKLES      = 19496,
    SPELL_ENRAGE             = 19516, // stacks up to 10 times
    SPELL_SEPARATION_ANXIETY = 23487,
    SPELL_ERUPTION_TRIGGER   = 20482,
    SPELL_ENRAGE_TRIGGER     = 19515,

    // Texts
    EMOTE_MASSIVE_ERUPTION  = 8254,

    // Events
    EVENT_ANTIMAGICPULSE        = 1,
    EVENT_MAGMASHACKLES         = 2,
    EVENT_MASSIVE_ERUPTION      = 3,
    EVENT_FIRESWORN_LEASH_CHECK = 4
};

struct boss_garrAI : ScriptedAI
{
    explicit boss_garrAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        m_pInstance = static_cast<ScriptedInstance*>(pCreature->GetInstanceData());
        Reset();
    }

    EventMap m_CombatEvents;
    ScriptedInstance* m_pInstance;
    std::vector<ObjectGuid> m_lFiresworn;

    void Reset() override
    {
        m_CombatEvents.Reset();
        m_lFiresworn.clear();

        if (m_creature->IsAlive() && m_pInstance && m_pInstance->GetData(TYPE_GARR) != DONE)
        {
            m_pInstance->SetData(TYPE_GARR, NOT_STARTED);
        }
    }

    void Aggro(Unit* /*pWho*/) override
    {
        if (!m_pInstance)
        {
            return;
        }

        if (m_pInstance->GetData(TYPE_GARR) == DONE)
        {
            m_creature->DeleteLater();
            return;
        }

        m_pInstance->SetData(TYPE_GARR, IN_PROGRESS);
        m_creature->SetInCombatWithZone();

        // Store add guids
        std::list<Creature*> adds;
        GetCreatureListWithEntryInGrid(adds, m_creature, NPC_FIRESWORN, 150.0f);
        m_lFiresworn.clear();
        for (const auto& itr : adds)
        {
            m_lFiresworn.push_back(itr->GetObjectGuid());

            if (Creature* pAdd = m_creature->GetMap()->GetCreature(itr->GetObjectGuid()))
            {
                DoCastSpellIfCan(pAdd, SPELL_SEPARATION_ANXIETY, CF_TRIGGERED | CF_AURA_NOT_PRESENT);
            }
        }

        ScheduleCombatEvents();
    }

    void JustDied(Unit* /*pKiller*/) override
    {
        if (m_pInstance)
        {
            m_pInstance->SetData(TYPE_GARR, DONE);
        }
    }

    void SpellHit(SpellCaster* /*pCaster*/, SpellEntry const* pSpell) override
    {
        if (pSpell->Id == SPELL_ENRAGE_TRIGGER)
        {
            DoCastSpellIfCan(m_creature, SPELL_ENRAGE, CF_TRIGGERED);
        }
    }

    void FireswornJustDied(ObjectGuid addGuid)
    {
        // Remove add from guid list and cast enrage on self
        auto it = std::find(m_lFiresworn.begin(), m_lFiresworn.end(), addGuid);
        if (it != m_lFiresworn.end())
        {
            m_lFiresworn.erase(it);
        }
    }

    void UpdateAI(uint32 const diff) override
    {
        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
        {
            return;
        }

        m_CombatEvents.Update(diff);
        UpdateEvents();

        DoMeleeAttackIfReady();
    }

    void ScheduleCombatEvents()
    {
        m_CombatEvents.RescheduleEvent(EVENT_ANTIMAGICPULSE,        Seconds(15));
        m_CombatEvents.RescheduleEvent(EVENT_MAGMASHACKLES,         Seconds(10));
        m_CombatEvents.RescheduleEvent(EVENT_MASSIVE_ERUPTION,      Minutes(6));
        m_CombatEvents.RescheduleEvent(EVENT_FIRESWORN_LEASH_CHECK, Seconds(5));
    }

    // Finds the closest valid attack target for pFor anywhere in the instance - used to force
    // a Firesworn that has never aggroed anyone onto somebody, since it has no threat list of
    // its own to pick from yet.
    Player* FindNearestValidAttackTarget(Unit* pFor)
    {
        Player* pNearest = nullptr;
        float nearestDist = 0.0f;
        Map::PlayerList const& players = m_creature->GetMap()->GetPlayers();
        for (const auto& itr : players)
        {
            Player* pPlayer = itr.getSource();
            if (!pPlayer || !pPlayer->IsAlive() || !pFor->IsValidAttackTarget(pPlayer))
            {
                continue;
            }

            float const dist = pFor->GetDistance(pPlayer);
            if (!pNearest || dist < nearestDist)
            {
                pNearest = pPlayer;
                nearestDist = dist;
            }
        }
        return pNearest;
    }

    void UpdateEvents()
    {
        while (uint32 const eventId = m_CombatEvents.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_FIRESWORN_LEASH_CHECK:
                {
                    // Players can use LOS/terrain/positioning to keep every Firesworn
                    // permanently out of its own aggro range while zerging Garr down - since
                    // the add then never enters combat, mob_fireswornAI::UpdateAI()'s own
                    // EVENT_THREAT_CHECK safety net (which only runs once
                    // Creature::SelectHostileTarget() succeeds, i.e. the add already has a
                    // threat list) never gets a chance to run either - it can maintain
                    // aggro, not bootstrap it from zero. This is Garr's own catch-all:
                    // periodically force any known Firesworn that currently isn't in combat
                    // onto the nearest player in the instance.
                    //
                    // Deliberately unconditional on history (checks live IsInCombat() every
                    // time, not a one-shot "has it ever fought" flag): an earlier version tried
                    // to spare an add that fought once and then legitimately evaded (e.g. its
                    // attacker died mid-wipe) by permanently exempting anything that had ever
                    // triggered Aggro() once - but Aggro() itself calls SetInCombatWithZone(),
                    // which fires from literally any trigger (even a hunter's pet grazing it),
                    // so a raid could deliberately manufacture one throwaway engagement to
                    // permanently launder an add out of this safety net, defeating the whole
                    // point. Re-checking live combat state every 5s instead means there's no
                    // one-shot flag to launder - the only cost is that a genuinely evaded add
                    // gets re-engaged even if Garr is still being fought solo by a lone
                    // survivor after most of the raid has wiped, which is a far narrower and
                    // more acceptable edge case than a repeatable exploit.
                    for (ObjectGuid const& addGuid : m_lFiresworn)
                    {
                        Creature* pAdd = m_creature->GetMap()->GetCreature(addGuid);
                        if (pAdd && pAdd->IsAlive() && !pAdd->IsInCombat())
                        {
                            if (Player* pNearest = FindNearestValidAttackTarget(pAdd))
                            {
                                pAdd->EnterCombatWithTarget(pNearest);
                            }
                        }
                    }
                    m_CombatEvents.Repeat(Seconds(5));
                    return;
                }
                case EVENT_ANTIMAGICPULSE:
                {
                    // Garr lets out anti-magic pulses, removing a beneficial effect from players in the raid.
                    if (DoCastSpellIfCan(m_creature, SPELL_ANTIMAGICPULSE) == CAST_OK)
                    {
                        m_CombatEvents.Repeat(Seconds(urand(15, 20)));
                        return;
                    }

                    // Cast Failed: Try again in 1s
                    m_CombatEvents.Repeat(Seconds(1));
                    return;
                }
                case EVENT_MAGMASHACKLES:
                {
                    // Garr reduces the movement speed of nearby enemies by 60% for 15 seconds.
                    if (DoCastSpellIfCan(m_creature, SPELL_MAGMASHACKLES) == CAST_OK)
                    {
                        m_CombatEvents.Repeat(Seconds(urand(10, 15)));
                        return;
                    }

                    // Cast Failed: Try again in 1s
                    m_CombatEvents.Repeat(Seconds(1));
                    return;
                }
                case EVENT_MASSIVE_ERUPTION:
                {
                    // After 6 minutes, Garr will let his adds explode and deal massive damage.
                    // Every further 20s in fight, another add will explode.
                    if (!m_lFiresworn.empty())
                    {
                        uint32 const randomIndex = urand(0, m_lFiresworn.size() - 1);
                        if (auto pRandomFiresworn = m_creature->GetMap()->GetCreature(m_lFiresworn[randomIndex]))
                        {
                            if (!pRandomFiresworn->HasAuraType(SPELL_AURA_MOD_STUN))
                            {
                                DoScriptText(EMOTE_MASSIVE_ERUPTION, m_creature);
                                m_creature->CastSpell(pRandomFiresworn, SPELL_ERUPTION_TRIGGER, true);
                            }
                        }
                    }
                    m_CombatEvents.Repeat(Seconds(20));
                    return;
                }
            }
        }
    }
};

CreatureAI* GetAI_boss_garr(Creature* pCreature)
{
    return new boss_garrAI(pCreature);
}

enum GarrAdds : uint32
{
    // Spells
    SPELL_THRASH            = 3391,
    SPELL_IMMOLATE          = 15732,
    SPELL_ADD_ERUPTION      = 19497,
    SPELL_MASSIVE_ERUPTION  = 20483,

    // Events
    EVENT_IMMOLATE     = 1,
    EVENT_THREAT_CHECK = 2
};

struct mob_fireswornAI : ScriptedAI
{
    explicit mob_fireswornAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        m_pInstance = static_cast<ScriptedInstance*>(pCreature->GetInstanceData());
        mob_fireswornAI::Reset();
    }

    EventMap m_CombatEvents;
    ScriptedInstance* m_pInstance;

    bool m_bForceExplosion;

    void Reset() override
    {
        m_bForceExplosion = false;
        m_CombatEvents.Reset();
    }

    void Aggro(Unit* /*pWho*/) override
    {
        DoCastSpellIfCan(m_creature, SPELL_THRASH, CF_TRIGGERED | CF_AURA_NOT_PRESENT);
        m_creature->SetInCombatWithZone();
        ScheduleCombatEvents();
    }

    // A Firesworn must not be allowed to evade home while Garr is being actively fought
    // (IN_PROGRESS) or already dead (DONE) - only while Garr hasn't been engaged yet, or has
    // fully reset himself after a wipe (NOT_STARTED), is it acceptable for a stuck add to reset
    // too. Otherwise players could use terrain/LOS to keep an add permanently out of reach mid-
    // fight (not just after Garr dies) and let the engine's own 24s "target unreachable" timer
    // (Creature::Update(), independent of Garr's state) quietly remove it from the fight instead
    // of having to kill it. This intercepts every evade trigger unconditionally (stuck timer,
    // leash distance, empty threat list from SelectHostileTarget), not just the stuck-timer case,
    // since the intent is "never let it leave combat while Garr is engaged or dead", not one
    // specific cause. Checking TYPE_GARR (already used elsewhere in this file for the same
    // purpose) instead of querying Garr's Creature object directly also means a genuine wipe
    // resolves itself correctly: once Garr has no reachable/alive targets left he evades too,
    // boss_garrAI::Reset() sets TYPE_GARR back to NOT_STARTED, and stuck adds are then allowed to
    // reset right along with him. Harmless if this fires during map/grid teardown (Map::UnloadAll
    // etc.) - the creature object is being destroyed either way, this only skips clearing
    // gameplay state nobody will observe.
    void EnterEvadeMode() override
    {
        if (m_pInstance && m_pInstance->GetData(TYPE_GARR) != NOT_STARTED)
        {
            return;
        }

        ScriptedAI::EnterEvadeMode();
    }

    void JustDied(Unit*) override
    {
        if (!m_pInstance)
        {
            return;
        }

        // Garr gains 9% attack speed for every Firesworn slain.
        if (Creature* pGarr = m_pInstance->GetSingleCreatureFromStorage(NPC_GARR))
        {
            if (pGarr->IsAlive())
            {
                DoCastSpellIfCan(pGarr, SPELL_ENRAGE_TRIGGER, CF_TRIGGERED);
                if (auto pGarrAI = dynamic_cast<boss_garrAI*>(pGarr->AI()))
                {
                    pGarrAI->FireswornJustDied(m_creature->GetObjectGuid());
                }
            }
        }

        if (!m_bForceExplosion)
        {
            // On death, Firesworns will explode, dealing massive Fire damage and knocking players back.
            m_creature->CastSpell(m_creature, SPELL_ADD_ERUPTION, true);
        }
    }

    void SpellHit(SpellCaster* /*pCaster*/, SpellEntry const* pSpell) override
    {
        if (pSpell->Id == SPELL_ERUPTION_TRIGGER)
        {
            m_bForceExplosion = true;
            m_creature->CastSpell(m_creature, SPELL_MASSIVE_ERUPTION, true);
        }
    }

    void UpdateAI(uint32 const diff) override
    {
        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
        {
            return;
        }

        m_CombatEvents.Update(diff);
        UpdateEvents();

        DoMeleeAttackIfReady();
    }

    void ScheduleCombatEvents()
    {
        m_CombatEvents.RescheduleEvent(EVENT_IMMOLATE, Seconds(10));
        m_CombatEvents.RescheduleEvent(EVENT_THREAT_CHECK, Seconds(3));
    }

    void UpdateEvents()
    {
        while (const uint32 eventId = m_CombatEvents.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_IMMOLATE:
                {
                    if (DoCastSpellIfCan(m_creature->GetVictim(), SPELL_IMMOLATE) == CAST_OK)
                    {
                        m_CombatEvents.Repeat(Seconds(20));
                        return;
                    }

                    // Cast Failed: Try again in 1s
                    m_CombatEvents.Repeat(Seconds(1));
                    return;
                }
                case EVENT_THREAT_CHECK:
                {
                    // 玩家会卡视线/地形把 Firesworn 拉住，同时贴脸打 Garr 的近战完全不在
                    // 这只小怪的仇恨表里，导致小怪永远不会转去打近战。这里每3秒给附近
                    // （20码内，对齐 Separation Anxiety 要求 Firesworn 离 Garr 不超过20码的范围）
                    // 还没上仇恨表的玩家一笔保底仇恨，让小怪迟早会注意到他们。
                    std::list<Player*> nearby;
                    GetPlayersWithinRange(nearby, 20.0f);
                    for (Player* pPlayer : nearby)
                    {
                        if (!pPlayer->IsAlive() || !m_creature->IsValidAttackTarget(pPlayer))
                        {
                            continue;
                        }

                        if (m_creature->GetThreatManager().getThreat(pPlayer) <= 0.0f)
                        {
                            m_creature->AddThreat(pPlayer, 50.0f);
                        }
                    }

                    m_CombatEvents.Repeat(Seconds(3));
                    return;
                }
            }
        }
    }
};

CreatureAI* GetAI_mob_firesworn(Creature* pCreature)
{
    return new mob_fireswornAI(pCreature);
}

void AddSC_boss_garr()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "boss_garr";
    newscript->GetAI = &GetAI_boss_garr;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "mob_firesworn";
    newscript->GetAI = &GetAI_mob_firesworn;
    newscript->RegisterSelf();
}
