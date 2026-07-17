/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
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

#include "Common.h"
#include "DBCStores.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "SpellMgr.h"
#include "Log.h"
#include "Opcodes.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "GameObject.h"
#include "Map.h"
#include "Chat.h"
#include "ScriptedGossip.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "Utilities/Random.h"

using namespace Spells;

void WorldSession::HandleUseItemOpcode(WorldPackets::Spell::UseItem const& packet)
{
    Player* pUser = _player;

    // ignore for remote control state
    if (!pUser->IsSelfMover())
        return;

    Item *pItem = pUser->GetItemByPos(packet.bagIndex, packet.slot);
    if (!pItem)
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, nullptr, nullptr);
        return;
    }

    ItemPrototype const* proto = pItem->GetProto();
    if (!proto)
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, nullptr);
        return;
    }

    if (packet.spellSlot >= MAX_ITEM_PROTO_SPELLS ||
        proto->Spells[packet.spellSlot].SpellId == 0 ||
        proto->Spells[packet.spellSlot].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, nullptr);
        return;
    }

    // some item classes can be used only in equipped state
    if (proto->InventoryType != INVTYPE_NON_EQUIP && !pItem->IsEquipped())
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, nullptr);
        return;
    }

    InventoryResult msg = pUser->CanUseItem(pItem);
    if (msg != EQUIP_ERR_OK)
    {
        pUser->SendEquipError(msg, pItem, nullptr);
        return;
    }

    // not allow use item from trade (cheat way only)
    if (pItem->IsInTrade())
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, nullptr);
        return;
    }

    if (pUser->IsInCombat())
    {
        for (const auto& itr : proto->Spells)
        {
            if (SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(itr.SpellId))
            {
                if (spellInfo->IsNonCombatSpell())
                {
                    pUser->SendEquipError(EQUIP_ERR_NOT_IN_COMBAT, pItem, nullptr);
                    return;
                }
            }
        }
    }

    // check also  BIND_WHEN_PICKED_UP and BIND_QUEST_ITEM for .additem or .additemset case by GM (not binded at adding to inventory)
    if (pItem->GetProto()->Bonding == BIND_WHEN_USE || pItem->GetProto()->Bonding == BIND_WHEN_PICKED_UP || pItem->GetProto()->Bonding == BIND_QUEST_ITEM)
    {
        if (!pItem->IsSoulBound())
        {
            pItem->SetState(ITEM_CHANGED, pUser);
            pItem->SetBinding(true);
        }
    }

    const_cast<SpellCastTargets&>(packet.targets).PrepareForSpellSystem(_player);
    SpellCastResult itemCastCheckResult = SPELL_CAST_OK;

    if (!pItem->IsTargetValidForItemUse(packet.targets.getUnitTarget()))
        itemCastCheckResult = SPELL_FAILED_BAD_TARGETS;
    else if (pUser->IsShapeShifted())
    {
        // World of Warcraft Client Patch 1.10.0 (2006-03-28)
        // - All shapeshift forms can now use equipped items.
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
        if (!(packet.bagIndex == INVENTORY_SLOT_BAG_0 && packet.slot < EQUIPMENT_SLOT_END))
#endif
        itemCastCheckResult = SPELL_FAILED_NO_ITEMS_WHILE_SHAPESHIFTED;
    }

    if (itemCastCheckResult != SPELL_CAST_OK)
    {
        // free gray item after use fail
        pUser->SendEquipError(EQUIP_ERR_NONE, pItem, nullptr);

        // send spell error
        uint32 spellid = proto->Spells[packet.spellSlot].SpellId;
        if (SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellid))
            Spell::SendCastResult(_player, spellInfo, itemCastCheckResult);
        return;
    }

    // OnUse OnItemUse OnItemGossip
    bool cancelCast = false;

    // Hardcore
    if (pUser->IsHardcore() && ! pUser->IsHardcoreRetired())
    {
        // 骑士不允许无敌炉石 6948 炉石
        if (pUser->HasAura(25771) && pItem->GetEntry() == 6948)
        {
            ChatHandler(pUser).SendSysMessage("勇敢者准则：勇敢者无法在自律期间使用炉石。");
            cancelCast = true;
        }
    }

    // Party 便携式量子发生器 98623
    if (pItem->GetEntry() == 98623)
    {
        PlayerMenu* pMenu = pUser->PlayerTalkClass;
        pMenu->ClearMenus();

        Quest const* pNewQuest = sObjectMgr.GetQuestTemplate(32053);
        if (pNewQuest)
        {
            if (pUser->CanTakeQuest(pNewQuest, false) || pUser->GetQuestStatus(32053) == QUEST_STATUS_COMPLETE)
            {
                pMenu->SendGossipMenu(22025, pItem->GetGUID());
            }
        }
        else
        {
            std::array<uint32, 100> PartyTexts {22015, 22016, 22017, 22018, 22019, 22020, 22021, 22022, 22023, 22024};
            if (! pUser->oowowInfo.cache_PartyText)
            {
                pUser->oowowInfo.cache_PartyText = PartyTexts[urand(0, 9)];
                pUser->oowowInfo.cache_PartyCoolDown = time(nullptr) + 5*60;
            }

            pMenu->SendGossipMenu(pUser->oowowInfo.cache_PartyText, pItem->GetGUID());
        }

        pUser->CastSpell(pUser, 26638, true); // Twin Teleport Visual
        cancelCast = true;
    }

    // Party 派对入场券 920413
    if (pItem->GetEntry() == 920413)
    {
        pUser->oowowInfo.displayID = 0;

        if (pUser->GetQuestStatus(32053) == QUEST_STATUS_COMPLETE)
        {
            PlayerMenu* pMenu = pUser->PlayerTalkClass;
            pMenu->ClearMenus();

            pMenu->GetGossipMenu().AddMenuItem(3, "随机变形", 1, 10);
            pMenu->GetGossipMenu().AddMenuItem(2, "指定变形", 2, 20, "", true);
            pMenu->SendGossipMenu(22026, pItem->GetGUID());

            cancelCast = true;
        }
        else
        {
            if (pUser->HasAura(8067))
                pUser->RemoveAurasDueToSpell(8067);
        }
    }

    // Party 大雪球 921038
    if (pItem->GetEntry() == 921038 && pUser->GetMap())
    {
        if (packet.targets.getUnitTarget() && packet.targets.getUnitTarget()->ToPlayer() && packet.targets.getUnitTarget()->ToPlayer() != pUser)
        {
            itemCastCheckResult = SPELL_FAILED_BAD_TARGETS;

            // free gray item after use fail
            pUser->SendEquipError(EQUIP_ERR_NONE, pItem, nullptr);

            // send spell error
            uint32 spellid = proto->Spells[packet.spellSlot].SpellId;
            if (SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellid))
                Spell::SendCastResult(pUser, spellInfo, itemCastCheckResult);
            return;
        }

        // 180654 雪堆
        if (pUser->GetMap()->IsRaid() || pUser->GetMap()->IsDungeon() || pUser->InBattleGround())
        {
            ChatHandler(pUser).SendSysMessage("能量不足，无法生成雪堆。");
            cancelCast = true;
        }
        else
        {
            float x = float(pUser->GetPositionX());
            float y = float(pUser->GetPositionY());
            float z = float(pUser->GetPositionZ());
            float o = float(pUser->GetOrientation());

            // 120s
            pUser->SummonGameObject(180654, x, y, z, o, 0.0f, 0.0f, 0.0f, 0.0f, 300);
            pUser->SummonGameObject(180654, x+1, y+1, z, o, 0.0f, 0.0f, 0.0f, 0.0f, 300);
            pUser->SummonGameObject(180654, x+1, y-1, z, o, 0.0f, 0.0f, 0.0f, 0.0f, 300);
            pUser->SummonGameObject(180654, x-1, y+1, z, o, 0.0f, 0.0f, 0.0f, 0.0f, 300);
            pUser->SummonGameObject(180654, x-1, y-1, z, o, 0.0f, 0.0f, 0.0f, 0.0f, 300);
            pUser->TextEmote("打雪仗咯！");
        }
    }

    // DualTalent 魂器 922001
    if (pItem->GetEntry() == 922001)
    {
        PlayerMenu* pMenu = pUser->PlayerTalkClass;
        pMenu->ClearMenus();

        for (auto i = pUser->oowowInfo.DualTalents.begin(); i != pUser->oowowInfo.DualTalents.end(); i++)
        {
            if (i->first == pUser->ActiveTalent())
            {
                std::string msg = std::string("|cFFFF0000灵魂 ") + std::to_string(i->first) + std::string(" - ") +  i->second + std::string("|r ");
                pMenu->GetGossipMenu().AddMenuItem(2, msg.c_str(), 1, 99); // active talent
            }
            else
            {
                std::string msg = std::string("灵魂 ") + std::to_string(i->first) + std::string(" - ") + i->second;
                pMenu->GetGossipMenu().AddMenuItem(3, msg.c_str(), 1, i->first); // inactive talent
            }
        }

        if (time(nullptr) < pUser->oowowInfo.DualTalent_CoolDown)
        {
            int32 time1 = pUser->oowowInfo.DualTalent_CoolDown - time(nullptr);
            std::string time = secsToTimeString(time1, true);
            std::string msg = std::string("灵魂虚弱：|cFF4b4bdf") + time + std::string("|r");
            pMenu->GetGossipMenu().AddMenuItem(0, msg.c_str(), 1, 99);
        }
        else
        {
            pMenu->GetGossipMenu().AddMenuItem(4, "分裂灵魂", 1, 20);
        }

        pMenu->SendGossipMenu(22011, pItem->GetGUID());

        cancelCast = true;
    }

    // Guild Bank
    if (pItem->GetEntry() == 918232)
    {
        if (pUser->IsTianxuan())
        {
            ChatHandler(pUser).PSendSysMessage("[天选者] 所获皆凭己力，不受馈赠。");
            cancelCast = true;
        }
        else
        {

        PlayerMenu* pMenu = pUser->PlayerTalkClass;
        pMenu->ClearMenus();

        bool hasBank = false;
        if (pUser->GetGuildId())
        {
            for (auto i = sOOMgr.OOGuildBanks.begin(); i != sOOMgr.OOGuildBanks.end(); i++)
            {
                OOGuildBank OOGuildBank = i->second;

                if (pUser->GetGuildId() == OOGuildBank.guild_id && pUser->GetRank() <= OOGuildBank.guild_rank)
                {
                    std::string msg = std::string("接入仓库《") +  OOGuildBank.name + std::string("》") ;
                    pMenu->GetGossipMenu().AddMenuItem(GOSSIP_ICON_TAXI, msg.c_str(), GOSSIP_SENDER_SEC_BANK, OOGuildBank.vendor_id);

                    hasBank = true;
                }
            }
        }

        if (!hasBank)
            pMenu->GetGossipMenu().AddMenuItem(GOSSIP_ICON_CHAT, "您还没有仓库。", GOSSIP_SENDER_SEC_BANK, 999);

        pMenu->SendGossipMenu(22030, pItem->GetGUID());

        cancelCast = true;
        } // else (not Tianxuan)
    }

    // BR 报名令牌 — 世界地图远程加入/离开候战席（副本/战场/BR/战斗中不可用）
    if (pItem->GetEntry() == 900105)
    {
        if (pUser->GetMap()->Instanceable())
            ChatHandler(pUser).PSendSysMessage("[孤胆称雄] 此地无法使用论剑令，请前往野外。");
        else if (pUser->IsInCombat())
            ChatHandler(pUser).PSendSysMessage("[孤胆称雄] 战斗中无法使用论剑令。");
        else
        {
            bool inQueue = sBattleRoyaleMgr.IsPlayerInQueue(pUser->GetObjectGuid());
            if (inQueue)
                sBattleRoyaleMgr.DequeuePlayer(pUser);
            else
            {
                std::string err;
                if (!sBattleRoyaleMgr.EnqueuePlayer(pUser, err))
                    ChatHandler(pUser).PSendSysMessage("[孤胆称雄] %s", err.c_str());
            }
        }
        cancelCast = true;
    }

    // BR 积分商店 — 餐桌：召唤一张装饰桌 + 面包/水两个可反复点击的道具，5分钟后一起消失
    if (pItem->GetEntry() == 900109)
    {
        float x, y, z;
        pUser->GetClosePoint(x, y, z, pUser->GetObjectBoundingRadius(), 2.0f, 0.0f);
        float const tableOrient = pUser->GetOrientation();
        pUser->SummonGameObject(180879, x, y, pUser->GetPositionZ(), tableOrient, 0, 0, 0, 0, 300);

        // 面包/水/花相对桌子中心的偏移，用GM实测.gobject add摆放出来的真实坐标反推出来的
        // （forward=沿桌子朝向前方为正，left=沿桌子朝向左侧为正，height=高于桌子原点的Z差），
        // 不是拍脑袋估的。用桌子自己的朝向(tableOrient)做旋转，这样不管玩家用道具时面朝哪个
        // 方向，摆放关系都保持一致。
        auto summonOnTable = [&](uint32 entry, float forward, float left, float height)
        {
            float const px = x + forward * cos(tableOrient) - left * sin(tableOrient);
            float const py = y + forward * sin(tableOrient) + left * cos(tableOrient);
            pUser->SummonGameObject(entry, px, py, pUser->GetPositionZ() + height, tableOrient, 0, 0, 0, 0, 300);
        };

        summonOnTable(900109, 0.167f, 0.121f, 1.86f);   // 面包
        summonOnTable(900111, -0.625f, -0.130f, 1.86f); // 水
        summonOnTable(178125, -0.169f, -0.187f, 2.02f); // 花（纯装饰，Lotharian Lotus）

        ChatHandler(pUser).PSendSysMessage("[孤胆称雄] 且坐，且饮，且歇脚。");
        cancelCast = true;
    }

    if (cancelCast)
    {
        ObjectGuid guid = pItem->GetGUID();
        // Send equip error that shows no message
        // This is a hack fix to stop spell casting visual bug when a spell is not cast on use
        WorldPacket data(SMSG_INVENTORY_CHANGE_FAILURE, 18);
        data << uint8(59); // EQUIP_ERR_NONE / EQUIP_ERR_CANT_BE_DISENCHANTED
        data << guid;
        data << ObjectGuid(uint64(0));
        data << uint8(0);
        pUser->GetSession()->SendPacket(&data);
        return;
    }

    pUser->CastItemUseSpell(pItem, const_cast<SpellCastTargets&>(packet.targets));
}

void WorldSession::HandleOpenItemOpcode(WorldPackets::Spell::OpenItem const& packet)
{
    Player* pUser = _player;

    // ignore for remote control state
    if (!pUser->IsSelfMover())
        return;

    Item *pItem = pUser->GetItemByPos(packet.bagIndex, packet.slot);
    if (!pItem)
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, nullptr, nullptr);
        return;
    }

    ItemPrototype const* proto = pItem->GetProto();
    if (!proto)
    {
        pUser->SendEquipError(EQUIP_ERR_ITEM_NOT_FOUND, pItem, nullptr);
        return;
    }

    if (pUser->IsTaxiFlying())
    {
        pUser->SendEquipError(EQUIP_ERR_CANT_DO_RIGHT_NOW, pItem, nullptr);
        return;
    }

    if (!pUser->IsAlive())
    {
        pUser->SendEquipError(EQUIP_ERR_YOU_ARE_DEAD, pItem, nullptr);
        return;
    }

    // locked item
    uint32 lockId = proto->LockID;
    if (lockId && !pItem->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_UNLOCKED))
    {
        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);

        if (!lockInfo)
        {
            pUser->SendEquipError(EQUIP_ERR_ITEM_LOCKED, pItem, nullptr);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "WORLD::OpenItem: item [guid = %u] has an unknown lockId: %u!", pItem->GetGUIDLow() , lockId);
            return;
        }

        // required picklocking
        if (lockInfo->Skill[1] || lockInfo->Skill[0])
        {
            pUser->SendEquipError(EQUIP_ERR_ITEM_LOCKED, pItem, nullptr);
            return;
        }
    }

    if (_player->IsNonMeleeSpellCasted())
        _player->InterruptNonMeleeSpells(false);

    if (pItem->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_WRAPPED))// wrapped?
    {
        std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery("SELECT `item_id`, `flags` FROM `character_gifts` WHERE `item_guid` = '%u'", pItem->GetGUIDLow());
        if (result)
        {
            Field* fields = result->Fetch();
            uint32 entry = fields[0].GetUInt32();
            uint32 flags = fields[1].GetUInt32();

            pItem->SetGuidValue(ITEM_FIELD_GIFTCREATOR, ObjectGuid());
            pItem->SetEntry(entry);
            pItem->SetUInt32Value(ITEM_FIELD_FLAGS, flags);
            pItem->SetState(ITEM_CHANGED, pUser);
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Wrapped item %u don't have record in character_gifts table and will deleted", pItem->GetGUIDLow());
            pUser->DestroyItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
            return;
        }

        static SqlStatementID delGifts ;

        SqlStatement stmt = CharacterDatabase.CreateStatement(delGifts, "DELETE FROM `character_gifts` WHERE `item_guid` = ?");
        stmt.PExecute(pItem->GetGUIDLow());
    }
    else
        pUser->SendLoot(pItem->GetObjectGuid(), LOOT_CORPSE);
}

void WorldSession::HandleGameObjectUseOpcode(WorldPackets::Misc::GameObjectUse const& packet)
{
    // ignore for remote control state
    if (!_player->IsSelfMover())
        return;

    GameObject* obj = GetPlayer()->GetMap()->GetGameObject(packet.guid);
    if (!obj || obj->IsDeleted())
        return;

    // Additional check preventing exploits (ie loot despawned chests)
    if (!obj->isSpawned())
        return;

    // Never expect this opcode for some type GO's
    if (obj->GetGoType() == GAMEOBJECT_TYPE_GENERIC)
        return;

    // Never expect this opcode for non intractable GO's
    if (obj->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NO_INTERACT))
        return;

    if (!obj->IsAtInteractDistance(_player))
        return;

    if (obj->PlayerCanUse(_player))
    {
        _player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_LOOTING_CANCELS);
        obj->Use(_player);
    }
}

void WorldSession::HandleCastSpellOpcode(WorldPackets::Spell::CastSpell const& packet)
{
    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(packet.spellId);

    if (!spellInfo)
        return;

    // not have spell in spellbook or spell passive and not casted by client
    if (!_player->HasActiveSpell(packet.spellId) || spellInfo->IsPassiveSpell())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "World: Player %u casts spell %u which he shouldn't have", _player->GetGUIDLow(), packet.spellId);
        //cheater? kick? ban?
        return;
    }

    // client provided targets
    const_cast<SpellCastTargets&>(packet.targets).PrepareForSpellSystem(_player);
    SpellEntry const* originalSpellInfo = spellInfo;

    // auto-selection buff level base at target level (in spellInfo)
    if (Unit* target = packet.targets.getUnitTarget())
    {
        // Cannot cast negative spells on yourself. Handle it here since casting negative
        // spells on yourself is frequently used within the core itself for certain mechanics.
        if (target == _player && IsExplicitlySelectedUnitTarget(spellInfo->EffectImplicitTargetA[0]) && !spellInfo->IsPositiveSpell(_player, target))
        {
            auto castPacket = std::make_unique<WorldPackets::Spell::CastResult>();
            castPacket->spellId = spellInfo->Id;
            castPacket->result = static_cast<uint8>(SPELL_RESULT_STATUS_FAIL);
            castPacket->failureReason = static_cast<uint8>(SPELL_FAILED_BAD_TARGETS);
            SendPacket(std::move(castPacket));
            return;
        }

        // if rank not found then function return nullptr but in explicit cast case original spell can be casted and later failed with appropriate error message
        if (SpellEntry const* actualSpellInfo = sSpellMgr.SelectAuraRankForLevel(spellInfo, target->GetLevel()))
            spellInfo = actualSpellInfo;
    }

    // Casting spells interrupts looting
    if (_player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_LOOTING))
    {
        if (ObjectGuid lootGuid = GetPlayer()->GetLootGuid())
            DoLootRelease(lootGuid);
    }

    Spell* spell = new Spell(_player, spellInfo, false, ObjectGuid(), nullptr, packet.targets.getUnitTarget());

    // Spell has been down-ranked, remember what client wanted to cast.
    if (spellInfo != originalSpellInfo)
        spell->m_originalSpellInfo = originalSpellInfo;

    // Nostalrius : Ivina
    spell->SetClientStarted(true);
    spell->prepare(std::move(const_cast<SpellCastTargets&>(packet.targets)));
}

void WorldSession::HandleCancelCastOpcode(WorldPackets::Spell::CancelCast const& packet)
{
    // ignore for remote control state (for player case)
    Unit* mover = _player->GetMover();
    if (mover != _player && mover->GetTypeId() == TYPEID_PLAYER)
        return;

    if (_player->IsNonMeleeSpellCasted(false))
        _player->InterruptNonMeleeSpells(false, packet.spellId);

    if (_player->IsNextSwingSpellCasted())
        _player->InterruptSpell(CURRENT_MELEE_SPELL);
}

void WorldSession::HandleCancelAuraOpcode(WorldPackets::Spell::CancelAura const& packet)
{
    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(packet.spellId);
    if (!spellInfo)
        return;

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
    if (spellInfo->HasAttribute(SPELL_ATTR_NO_AURA_CANCEL))
        return;
#endif

    if (spellInfo->HasAttribute(SPELL_ATTR_DO_NOT_DISPLAY))
        return;

    if (spellInfo->HasAttribute(SPELL_ATTR_EX_NO_AURA_ICON) && !spellInfo->activeIconID)
        return;

    if (spellInfo->IsPassiveSpell())
        return;

    if (!IsPositiveSpell(packet.spellId))
    {
        // ignore for remote control state
        if (!_player->IsSelfMover())
        {
            // except own aura spells
            bool allow = false;
            for (uint32 k : spellInfo->EffectApplyAuraName)
            {
                if (k == SPELL_AURA_MOD_POSSESS ||
                    k == SPELL_AURA_MOD_POSSESS_PET)
                {
                    allow = true;
                    break;
                }
            }

            // this also include case when aura not found
            if (!allow)
                return;
        }
        else
            return;
    }

    // World of Warcraft Client Patch 1.7.0 (2005-09-13)
    // - Druids should now be able to shapeshift back into caster form while Feared.
#if SUPPORTED_CLIENT_BUILD <= CLIENT_BUILD_1_6_1
    if (_player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING | UNIT_FLAG_POSSESSED))
#else
    // confirmed you cant remove buffs while mind controlled on wotlk ptr
    if (_player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_POSSESSED))
#endif
        return;

    // channeled spell case (it currently casted then)
    if (spellInfo->IsChanneledSpell())
    {
        if (Spell* curSpell = _player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (curSpell->m_spellInfo->Id == packet.spellId)
                _player->InterruptSpell(CURRENT_CHANNELED_SPELL);
        return;
    }

    SpellAuraHolder* holder = _player->GetSpellAuraHolder(packet.spellId);

    // not own area auras can't be cancelled (note: maybe need to check for aura on holder and not general on spell)
    if (holder && holder->GetCasterGuid() != _player->GetObjectGuid() && holder->GetSpellProto()->HasAreaAuraEffect())
        return;

    // non channeled case
    _player->RemoveAurasDueToSpellByCancel(packet.spellId);
}

void WorldSession::HandlePetCancelAuraOpcode(WorldPackets::Pet::PetCancelAura const& packet)
{
    // ignore for remote control state
    if (!_player->IsSelfMover())
        return;

    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(packet.spellId);
    if (!spellInfo)
        return;

    Creature* pet = GetPlayer()->GetMap()->GetAnyTypeCreature(packet.guid);

    if (!pet)
        return;

    if (packet.guid != GetPlayer()->GetPetGuid() && packet.guid != GetPlayer()->GetCharmGuid())
        return;

    if (!pet->IsAlive())
    {
        pet->SendPetActionFeedback(FEEDBACK_PET_DEAD);
        return;
    }

    pet->RemoveAurasDueToSpell(packet.spellId);
}

void WorldSession::HandleCancelGrowthAuraOpcode(NullClientPacket const& /*packet*/)
{
    // nothing do
}

void WorldSession::HandleCancelAutoRepeatSpellOpcode(NullClientPacket const& /*packet*/)
{
    // may be better send SMSG_CANCEL_AUTO_REPEAT?
    // cancel and prepare for deleting
    _player->GetMover()->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
}

void WorldSession::HandleCancelChanneling(WorldPackets::Spell::CancelChanneling const& /*packet*/)
{
    // ignore for remote control state (for player case)
    Unit* mover = _player->GetMover();
    if (mover != _player && mover->GetTypeId() == TYPEID_PLAYER)
        return;

    if (Spell* spell = _player->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        if (spell->IsTriggered())
            return;
        _player->InterruptSpell(CURRENT_CHANNELED_SPELL);
    }
}

void WorldSession::HandleSelfResOpcode(NullClientPacket const& /*packet*/)
{
// World of Warcraft Client Patch 1.6.0 (2005-07-12)
// - Self-resurrection spells show their name on the button in the release spirit dialog.
#if SUPPORTED_CLIENT_BUILD >= CLIENT_BUILD_1_6_1
    if (_player->GetUInt32Value(PLAYER_SELF_RES_SPELL))
    {
        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(_player->GetUInt32Value(PLAYER_SELF_RES_SPELL));
        if (spellInfo)
            _player->CastSpell(_player, spellInfo, false);

        _player->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);
    }
#else
    if (_player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_CAN_SELF_RESURRECT))
    {
        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(_player->GetResurrectionSpellId());
        if (spellInfo)
            _player->CastSpell(_player, spellInfo, false);

        _player->SetResurrectionSpellId(0);
        _player->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_CAN_SELF_RESURRECT);
    }
#endif
}
