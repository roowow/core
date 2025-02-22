/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 *
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

#ifndef _SCRIPTMGR_H
#define _SCRIPTMGR_H

#include "Common.h"
#include "Policies/Singleton.h"
#include "DBCEnums.h"
#include "ScriptCommands.h"
#include "UnitDefines.h"
#include "SpellDefines.h"
#include "SharedDefines.h"
#include "nonstd/optional.hpp"
#include <atomic>

using nonstd::optional;

struct AreaTriggerEntry;
class Aura;
class Object;
class Unit;
class Player;
class Creature;
class CreatureAI;
class GameObject;
class WorldObject;
class GameObjectAI;
class InstanceData;
class Item;
class Map;
class Quest;
class SpellAuraHolder;
class SpellCastTargets;
class SpellEntry;
class Spell;

typedef std::multimap<uint32, ScriptInfo> ScriptMap;
typedef std::map<uint32, ScriptMap > ScriptMapMap;

extern ScriptMapMap sQuestEndScripts;
extern ScriptMapMap sQuestStartScripts;
extern ScriptMapMap sSpellScripts;
extern ScriptMapMap sCreatureSpellScripts;
extern ScriptMapMap sGameObjectScripts;
extern ScriptMapMap sEventScripts;
extern ScriptMapMap sGenericScripts;
extern ScriptMapMap sGossipScripts;
extern ScriptMapMap sCreatureMovementScripts;
extern ScriptMapMap sCreatureAIScripts;

#define MAX_SCRIPTS         5000                            //72 bytes each (approx 351kb)
#define VISIBLE_RANGE       (166.0f)                        //MAX visible range (size of grid)
#define DEFAULT_TEXT        "<ScriptDev2 Text Entry Missing!>"

#define TEXT_SOURCE_RANGE (-1000000)                        //the amount of entries each text source has available

#define TEXT_SOURCE_TEXT_START      TEXT_SOURCE_RANGE
#define TEXT_SOURCE_TEXT_END        TEXT_SOURCE_RANGE*2 + 1

#define TEXT_SOURCE_CUSTOM_START    TEXT_SOURCE_RANGE*2
#define TEXT_SOURCE_CUSTOM_END      TEXT_SOURCE_RANGE*3 + 1

//Generic scripting functions
void DoScriptText(int32 textId, WorldObject* pSource, Unit* pTarget = nullptr, int32 chatTypeOverride = -1);
void DoOrSimulateScriptTextForMap(int32 textId, uint32 creatureId, Map* pMap, Creature* pSource = nullptr, Unit* pTarget = nullptr);

// Returns a target based on the type specified.
WorldObject* GetTargetByType(WorldObject* pSource, WorldObject* pTarget, Map* pMap, uint8 targetType, uint32 param1 = 0u, uint32 param2 = 0u, SpellEntry const* pSpellEntry = nullptr);

//TODO: find better namings and definitions.
//N=Neutral, A=Alliance, H=Horde.
//NEUTRAL or FRIEND = Hostility to player surroundings (not a good definition)
//ACTIVE or PASSIVE = Hostility to environment surroundings.
enum eEscortFaction
{
    FACTION_ESCORT_A_NEUTRAL_PASSIVE    = 10,
    FACTION_ESCORT_H_NEUTRAL_PASSIVE    = 33,
    FACTION_ESCORT_N_NEUTRAL_PASSIVE    = 113,

    FACTION_ESCORT_A_NEUTRAL_ACTIVE     = 231,
    FACTION_ESCORT_H_NEUTRAL_ACTIVE     = 232,
    FACTION_ESCORT_N_NEUTRAL_ACTIVE     = 250,

    FACTION_ESCORT_N_FRIEND_PASSIVE     = 290,
    FACTION_ESCORT_N_FRIEND_ACTIVE      = 495,

    FACTION_ESCORT_A_PASSIVE            = 774,
    FACTION_ESCORT_H_PASSIVE            = 775,

    FACTION_ESCORT_N_ACTIVE             = 1986,
    FACTION_ESCORT_H_ACTIVE             = 2046
};

struct ScriptPointMove
{
    uint32 uiCreatureEntry;
    uint32 uiPointId;
    float  fX;
    float  fY;
    float  fZ;
    uint32 uiWaitTime;
};

struct StringTextData
{
    uint32 SoundId;
    uint8  Type;
    uint32 Language;
    uint32 Emote;
};

struct CreatureEscortData
{
    uint32 uiCreatureEntry;
    uint32 uiQuestEntry;
    uint32 uiEscortFaction;
    uint32 uiLastWaypointEntry;
};

struct SpellScript
{
    virtual ~SpellScript() = default;

    // called on spell init
    virtual void OnInit(Spell* /*spell*/) const {}
    // called on success during Spell::Prepare
    virtual void OnSuccessfulStart(Spell* /*spell*/) const {}
    // called on success inside Spell::finish - for channels this only happens if whole channel went through
    virtual void OnSuccessfulFinish(Spell* /*spell*/) const {}
    // called at end of Spell::CheckCast - strict is true in Spell::Prepare
    virtual SpellCastResult OnCheckCast(Spell* /*spell*/, bool /*strict*/) const { return SPELL_CAST_OK; }
    // called before effect execution
    virtual void OnEffectExecute(Spell* /*spell*/, SpellEffectIndex /*effIdx*/) const {}
    // called in targeting to determine radius for spell
    virtual void OnSetTargetMap(Spell* /*spell*/, SpellEffectIndex /*effIdx*/, uint32& /*targetMode*/, float& /*radius*/, uint32& /*unMaxTargets*/) const {}
    // called on Unit Spell::CheckTarget
    virtual bool OnCheckTarget(const Spell* /*spell*/, GameObject* /*target*/, SpellEffectIndex /*eff*/) const { return true; }
    // called on GO Spell::AddGOTarget
    virtual bool OnCheckTarget(const Spell* /*spell*/, Unit* /*target*/, SpellEffectIndex /*eff*/) const { return true; }
    // called in Spell::cast on all successful checks and after taking reagents
    virtual void OnCast(Spell* /*spell*/) const {}
    // called in Spell::DoAllEffectOnTarget, for Unit case right before damage/heal is dealt and procs happen
    virtual void OnHit(Spell* /*spell*/, SpellMissInfo /*missInfo*/) const {}
    // called in Spell::DoAllEffectOnTarget for Unit targets only, after damage/heal is dealt and procs have happened
    virtual void OnAfterHit(Spell* /*spell*/) const {}
    // called after summoning a creature
    virtual void OnSummon(Spell* /*spell*/, Creature* /*summon*/) const {}
    // called after summoning a gameobject
    virtual void OnSummon(Spell* /*spell*/, GameObject* /*summon*/) const {}
};

struct AuraScript
{
    virtual ~AuraScript() = default;

    // called on SpellAuraHolder creation - caster can be nullptr
    virtual void OnHolderInit(SpellAuraHolder* /*holder*/, WorldObject* /*caster*/) {}
    // called after end of aura object constructor
    virtual void OnAuraInit(Aura* /*aura*/) {}
    // called during any event that calculates aura modifier amount - caster can be nullptr
    virtual int32 OnAuraValueCalculate(Aura* /*aura*/, Unit* /*caster*/, Unit* /*target*/, SpellEntry const* /*spellProto*/, SpellEffectIndex /*effIdx*/, Item* /*castItem*/, int32 value) { return value; }
    // called during duration calculation - target can be nullptr for channel duration calculation
    virtual int32 OnDurationCalculate(WorldObject const* /*caster*/, Unit const* /*target*/, int32 duration) { return duration; }
    //called in Aura::ApplyModifier
    virtual void OnBeforeApply(Aura* /*aura*/, bool /*apply*/) {}
    // called in Aura::ApplyModifier
    virtual void OnAfterApply(Aura* /*aura*/, bool /*apply*/) {}
    // called during proc eligibility checking, pOwner is the unit on which the aura is applied
    virtual optional<SpellProcEventTriggerCheck> OnCheckProc(Unit const* /*pOwner*/, Unit* /*pVictim*/, SpellAuraHolder* /*holder*/, SpellEntry const* /*procSpell*/, uint32 /*procFlag*/, uint32 /*procExtra*/, WeaponAttackType /*attType*/, bool /*isVictim*/) { optional<SpellProcEventTriggerCheck> nothing; return nothing; }
    // called before proc handler, pOwner is the unit on which the aura is applied
    virtual optional<SpellAuraProcResult> OnProc(Unit* /*pOwner*/, Unit* /*pVictim*/, uint32 /*amount*/, int32 /*originalAmount*/, Aura* /*triggeredByAura*/, SpellEntry const* /*procSpell*/, uint32 /*procFlag*/, uint32 /*procEx*/, uint32 /*cooldown*/) { optional<SpellAuraProcResult> nothing; return nothing; }
    // called on absorb of this aura
    virtual void OnAbsorb(Aura* /*aura*/, int32& /*currentAbsorb*/, int32& /*remainingDamage*/, bool& /*dropCharge*/, DamageEffectType /*damageType*/) {}
    // called on mana shield absorb of this aura
    virtual void OnManaAbsorb(Aura* /*aura*/, int32& /*currentAbsorb*/, int32& /*remainingDamage*/) {}
    // called on periodic auras which need amount calculation (damage, heal, burn, drain)
    virtual void OnPeriodicCalculateAmount(Aura* /*aura*/, float& /*amount*/) {}
    // called on periodic spell trigger
    virtual void OnPeriodicTrigger(Aura* /*aura*/, Unit* caster, Unit* target, WorldObject* targetObject, SpellEntry const*& spellInfo) {}
    // called on periodic dummy
    virtual void OnPeriodicDummy(Aura* /*aura*/) {}
    // called on periodic tick end
    virtual void OnPeriodicTickEnd(Aura* /*aura*/) {}
    // called on AreaAura target checking
    virtual bool OnAreaAuraCheckTarget(Aura const* aura, Unit* target) { return true; }
};

struct Script
{
    Script() :
        Name(""), pGossipHello(nullptr), pGOGossipHello(nullptr), pQuestAcceptNPC(nullptr),
        pGossipSelect(nullptr), pGOGossipSelect(nullptr),
        pGossipSelectWithCode(nullptr), pGOGossipSelectWithCode(nullptr), pQuestComplete(nullptr),
        pNPCDialogStatus(nullptr), pGODialogStatus(nullptr), pQuestRewardedNPC(nullptr), pQuestRewardedGO(nullptr), pGOHello(nullptr), pAreaTrigger(nullptr),
        pProcessEventId(nullptr), pGOQuestAccept(nullptr),
        pEffectDummyCreature(nullptr), pEffectDummyGameObj(nullptr),
        pEffectAuraDummy(nullptr), pGOOpen(nullptr),
        GOGetAI(nullptr), GetAI(nullptr), GetInstanceData(nullptr), GetSpellScript(nullptr), GetAuraScript(nullptr)
    {}

    std::string Name;

    // Methods to be scripted
    bool (*pGossipHello             )(Player*, Creature*);
    bool (*pGOGossipHello           )(Player*, GameObject*);
    bool (*pQuestAcceptNPC          )(Player*, Creature*, Quest const*);
    bool (*pGossipSelect            )(Player*, Creature*, uint32, uint32);
    bool (*pGOGossipSelect          )(Player*, GameObject*, uint32, uint32);
    bool (*pGossipSelectWithCode    )(Player*, Creature*, uint32, uint32, char const*);
    bool (*pGOGossipSelectWithCode  )(Player*, GameObject*, uint32, uint32, char const*);
    bool (*pQuestComplete           )(Player*, Creature*, Quest const*);
    uint32 (*pNPCDialogStatus       )(Player*, Creature*);
    uint32 (*pGODialogStatus        )(Player*, GameObject*);
    bool (*pQuestRewardedNPC        )(Player*, Creature*, Quest const*);
    bool (*pQuestRewardedGO         )(Player*, GameObject*, Quest const*);
    bool (*pGOHello                 )(Player*, GameObject*);
    bool (*pAreaTrigger             )(Player*, AreaTriggerEntry const*);
    bool (*pProcessEventId          )(uint32, Object*, Object*, bool);
    bool (*pGOQuestAccept           )(Player*, GameObject*, Quest const*);
    bool (*pEffectDummyCreature     )(WorldObject*, uint32, SpellEffectIndex, Creature*);
    bool (*pEffectDummyGameObj      )(WorldObject*, uint32, SpellEffectIndex, GameObject*);
    bool (*pEffectAuraDummy         )(Aura const*, bool);
    bool (*pGOOpen                   )(Player* pUser, GameObject* gobj);

    GameObjectAI* (*GOGetAI)(GameObject* pGo);
    CreatureAI* (*GetAI)(Creature*);
    InstanceData* (*GetInstanceData)(Map*);
    SpellScript* (*GetSpellScript)(SpellEntry const*);
    AuraScript* (*GetAuraScript)(SpellEntry const*);

    void RegisterSelf(bool reportUnused = true);
};

class ScriptMgr
{
    public:
        ScriptMgr();
        ~ScriptMgr();

        void LoadGameObjectScripts();
        void LoadQuestEndScripts();
        void LoadQuestStartScripts();
        void LoadEventScripts();
        void LoadSpellScripts();
        void LoadCreatureSpellScripts();
        void LoadGenericScripts();
        void LoadGossipScripts();
        void LoadCreatureMovementScripts();
        void LoadCreatureEventAIScripts();

        void CheckAllScriptTexts();
        bool CheckScriptTargets(uint32 targetType, uint32 targetParam1, uint32 targetParam2, char const* tableName, uint32 tableEntry);

        void LoadScriptNames();
        void LoadAreaTriggerScripts();
        void LoadEventIdScripts();

        uint32 GetAreaTriggerScriptId(uint32 triggerId) const;
        uint32 GetEventIdScriptId(uint32 eventId) const;

        char const* GetScriptName(uint32 id) const { return id < m_scriptNames.size() ? m_scriptNames[id].c_str() : ""; }
        uint32 GetScriptId(char const* name) const;
        uint32 GetScriptIdsCount() const { return m_scriptNames.size(); }
        
        void Initialize();
        void LoadDatabase();

        void LoadScriptTexts();
        void LoadScriptTextsCustom();
        void LoadScriptWaypoints();
        void LoadEscortData();

        StringTextData const* GetTextData(int32 uiTextId) const
        {
            TextDataMap::const_iterator itr = m_mTextDataMap.find(uiTextId);

            if (itr == m_mTextDataMap.end())
                return nullptr;

            return &itr->second;
        }
        
        CreatureEscortData const* GetEscortData(int32 creature_id) const
        {
            EscortDataMap::const_iterator itr = m_mEscortDataMap.find(creature_id);

            if (itr == m_mEscortDataMap.end())
                return nullptr;

            return &itr->second;
        }

        std::vector<ScriptPointMove> const& GetPointMoveList(uint32 uiCreatureEntry) const
        {
            static std::vector<ScriptPointMove> vEmpty;

            auto itr = m_mPointMoveMap.find(uiCreatureEntry);

            if (itr == m_mPointMoveMap.end())
                return vEmpty;

            return itr->second;
        }

        bool IsCreatureGuidReferencedInScripts(uint32 dbGuid) const
        {
            return m_referencedCreatureGuids.find(dbGuid) != m_referencedCreatureGuids.end();
        }

        bool IsGameObjectGuidReferencedInScripts(uint32 dbGuid) const
        {
            return m_referencedGameObjectGuids.find(dbGuid) != m_referencedGameObjectGuids.end();
        }

        uint32 IncreaseScheduledScriptsCount() { return (uint32)++m_scheduledScripts; }
        uint32 DecreaseScheduledScriptCount() { return (uint32)--m_scheduledScripts; }
        uint32 DecreaseScheduledScriptCount(size_t count) { return (uint32)(m_scheduledScripts -= count); }
        bool IsScriptScheduled() const { return m_scheduledScripts > 0; }

        CreatureAI* GetCreatureAI(Creature* pCreature);
        GameObjectAI* GetGameObjectAI(GameObject* pGob);
        InstanceData* CreateInstanceData(Map* pMap);
        SpellScript* GetSpellScript(SpellEntry const* pSpell);
        AuraScript* GetAuraScript(SpellEntry const* pSpell);

        bool OnGossipHello(Player* pPlayer, Creature* pCreature);
        bool OnGossipHello(Player* pPlayer, GameObject* pGameObject);
        bool OnGossipSelect(Player* pPlayer, Creature* pCreature, uint32 sender, uint32 action, char const* code);
        bool OnGossipSelect(Player* pPlayer, GameObject* pGameObject, uint32 sender, uint32 action, char const* code);
        bool OnQuestAccept(Player* pPlayer, Creature* pCreature, Quest const* pQuest);
        bool OnQuestAccept(Player* pPlayer, GameObject* pGameObject, Quest const* pQuest);
        bool OnQuestRewarded(Player* pPlayer, Creature* pCreature, Quest const* pQuest);
        bool OnQuestRewarded(Player* pPlayer, GameObject* pGameObject, Quest const* pQuest);
        uint32 GetDialogStatus(Player* pPlayer, Creature* pCreature);
        uint32 GetDialogStatus(Player* pPlayer, GameObject* pGameObject);
        bool OnGameObjectUse(Player* pPlayer, GameObject* pGameObject);
        bool OnGameObjectOpen(Player* pPlayer, GameObject* pGameObject);
        bool OnAreaTrigger(Player* pPlayer, AreaTriggerEntry const* atEntry);
        bool OnProcessEvent(uint32 eventId, Object* pSource, Object* pTarget, bool isStart);
        bool OnEffectDummy(WorldObject* pCaster, uint32 spellId, SpellEffectIndex effIndex, Creature* pTarget);
        bool OnEffectDummy(WorldObject* pCaster, uint32 spellId, SpellEffectIndex effIndex, GameObject* pTarget);
        bool OnAuraDummy(Aura const* pAura, bool apply);

    private:
        void CollectPossibleEventIds(std::set<uint32>& eventIds);
        void CollectPossibleGenericIds(std::set<uint32>& eventIds);
        void LoadScripts(ScriptMapMap& scripts, char const* tablename);
        void CheckScriptTexts(ScriptMapMap const& scripts);

        typedef std::vector<std::string> ScriptNameMap;
        typedef std::unordered_map<uint32, uint32> AreaTriggerScriptMap;
        typedef std::unordered_map<uint32, uint32> EventIdScriptMap;
        
        //Maps and lists
        typedef std::unordered_map<int32, StringTextData> TextDataMap;
        typedef std::unordered_map<uint32, std::vector<ScriptPointMove> > PointMoveMap;
        typedef std::unordered_map<int32, CreatureEscortData> EscortDataMap;

        AreaTriggerScriptMap    m_AreaTriggerScripts;
        EventIdScriptMap        m_EventIdScripts;

        ScriptNameMap           m_scriptNames;
        
        TextDataMap     m_mTextDataMap;                     //additional data for text strings
        PointMoveMap    m_mPointMoveMap;                    //coordinates for waypoints
        EscortDataMap   m_mEscortDataMap;                   // Data for escort quests scripted via DB
        std::set<uint32> m_referencedCreatureGuids;
        std::set<uint32> m_referencedGameObjectGuids;

        //atomic op counter for active scripts amount
        std::atomic<int> m_scheduledScripts;
};

#define sScriptMgr MaNGOS::Singleton<ScriptMgr>::Instance()

uint32 GetAreaTriggerScriptId(uint32 triggerId);
uint32 GetEventIdScriptId(uint32 eventId);
uint32 GetScriptId(char const* name);
char const* GetScriptName(uint32 id);
uint32 GetScriptIdsCount();

#endif
