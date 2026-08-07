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
#include "ObjectDefines.h"
#include "Map.h"
#include "Chat.h"
#include "ScriptedGossip.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "Utilities/Random.h"
#include "Geometry.h"

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
        if (pUser->IsTianxuan() && !pUser->GetQuestRewardStatus(920501))
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
    // 召唤特效统一在下面共享的cancelCast尾巴里触发（5001，见那里的说明），这里不用重复处理。
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

    // BR 积分商店 — 餐桌：召唤一张装饰桌 + 面包/水两个可反复点击的道具，10分钟后一起消失
    // 召唤特效统一在下面共享的cancelCast尾巴里触发（5001，见那里的说明），这里不用重复处理。
    // 冷却本身走61005这个纯标记法术手动查/写 Player::IsSpellReady/AddCooldown（因为壳子本身没冷却）。
    if (pItem->GetEntry() == 900109)
    {
        SpellEntry const* cdMarker = sSpellMgr.GetSpellEntry(61005);
        if (cdMarker && !pUser->IsSpellReady(cdMarker))
        {
            ChatHandler(pUser).PSendSysMessage("杯盘狼藉，尚待片刻，方能再摆一桌。");
        }
        else
        {
            // GetClosePoint的z参数已经是按(x,y)这个偏移点算出来的地面高度（内部会调用
            // UpdateGroundPositionZ），不是玩家自己脚下的高度——斜坡上这两个差得多，之前一直
            // 用pUser->GetPositionZ()（玩家脚下高度）而不是这里算出来的z，导致桌子在斜坡上
            // 要么陷进地里、要么悬空。改成直接用这个z当基准。
            float x, y, z;
            pUser->GetClosePoint(x, y, z, pUser->GetObjectBoundingRadius(), 2.0f, 0.0f);
            float const tableOrient = pUser->GetOrientation();
            pUser->SummonGameObject(180879, x, y, z, tableOrient, 0, 0, 0, 0, 600, false);

            // 面包/水/花/水果/烛台相对桌子中心的偏移，用GM实测.gobject add摆放出来的真实坐标+朝向
            // 反推出来的（forward=沿桌子朝向前方为正，left=沿桌子朝向左侧为正，height=高于桌子原点的
            // Z差），不是拍脑袋估的。用桌子自己的朝向(tableOrient)做旋转，这样不管玩家用道具时面朝
            // 哪个方向，摆放关系都保持一致。水果(180370 Harvest Fruit)、烛台(177415 Light of Elune)
            // 都直接用原始entry，不需要clone。
            auto summonOnTable = [&](uint32 entry, float forward, float left, float height)
            {
                float const px = x + forward * cos(tableOrient) - left * sin(tableOrient);
                float const py = y + forward * sin(tableOrient) + left * cos(tableOrient);
                GameObject* go = pUser->SummonGameObject(entry, px, py, z + height, tableOrient, 0, 0, 0, 0, 600, false);
                // 只给面包/水（900109/900111，我们自己clone的GOOBER，OnUse在go_br_refreshment里
                // 完全接管）打主人标记，方便那边反查"这桌子是谁摆的"去触发感谢表情。SetOwnerGuid
                // 只是写一下OBJECT_FIELD_CREATED_BY这个原始GUID字段，跟attach=true会做的
                // Unit::AddGameObject（把GO注册进玩家的拥有物列表）是两回事——后者才是欢乐制造器
                // 那个陷阱owner-cast距离判定失效bug的根源，只影响GAMEOBJECT_TYPE_TRAP，这里不涉及。
                // 花/水果/烛台是真实原版entry、走引擎默认OnUse，不打标记，避免节外生枝影响到
                // 它们各自原有的行为。
                if (go && (entry == 900109 || entry == 900111))
                    go->SetOwnerGuid(pUser->GetObjectGuid());
            };

            summonOnTable(900109, 0.167f, 0.121f, 1.86f);   // 面包
            summonOnTable(900111, -0.625f, -0.130f, 1.86f); // 水
            summonOnTable(178125, -0.169f, -0.187f, 2.02f); // 花（纯装饰，Lotharian Lotus）
            summonOnTable(180370, 0.117f, -0.662f, 1.86f);  // 水果（Harvest Fruit，直接用原始entry，不clone）
            summonOnTable(177415, -0.085f, -0.209f, 2.043f); // 月光烛台（Light of Elune，直接用原始entry，不clone）

            if (cdMarker)
                pUser->AddCooldown(cdMarker);

            pUser->TextEmote("摆一瓯清水，一方薄饼，君子之交，淡如水。");
        }
        cancelCast = true;
    }

    // BR 积分商店 — 灵魂之井：花20个灵魂碎片(6265)召唤一个灵魂之井(GO 900116，克隆自164869
    // Spectral Chalice的外观)，10分钟后消失，30分钟冷却；任何职业都能召唤，只要背包里有20个
    // 灵魂碎片，不够则召唤失败（不消耗冷却）。其他玩家点击井可反复获得9421 Major Healthstone
    // （go_br_refreshment这段AI接管，跟餐桌面包/水完全同一套机制，见BattleRoyaleNPC.cpp）。
    // 冷却走61009这个纯标记法术手动查/写 Player::IsSpellReady/AddCooldown（因为壳子本身没冷却）。
    if (pItem->GetEntry() == 900115)
    {
        SpellEntry const* cdMarker = sSpellMgr.GetSpellEntry(61009);
        if (cdMarker && !pUser->IsSpellReady(cdMarker))
        {
            ChatHandler(pUser).PSendSysMessage("灵魂之井方才枯竭，尚需片刻方能再次汲取。");
        }
        else if (pUser->GetItemCount(6265, false) < 20)
        {
            ChatHandler(pUser).PSendSysMessage("灵魂碎片不足20个，无法汲取灵魂之力。");
        }
        else
        {
            // 先召唤、成功了再扣灵魂碎片+记冷却——SummonGameObject理论上可能因为地图/模板问题
            // 返回nullptr（虽然实测极少见），顺序反过来会出现"扣了碎片却什么都没召唤出来"。
            float x, y, z;
            pUser->GetClosePoint(x, y, z, pUser->GetObjectBoundingRadius(), 2.0f, 0.0f);
            if (GameObject* well = pUser->SummonGameObject(900116, x, y, z, pUser->GetOrientation(), 0, 0, 0, 0, 600, false))
            {
                // 纯装饰用的180514（Glyphed Crystal Prism），respawnTime跟井本体一样是600秒，
                // 两者同时消失；直接用原始entry，不需要clone。
                pUser->SummonGameObject(180514, x, y, z, pUser->GetOrientation(), 0, 0, 0, 0, 600, false);

                pUser->DestroyItemCount(6265, 20, true);

                if (cdMarker)
                    pUser->AddCooldown(cdMarker);

                pUser->TextEmote("井水映月，灵魂低语，危难之际，一石可生。");
            }
            else
                ChatHandler(pUser).PSendSysMessage("灵魂之井召唤失败，请稍后再试。");
        }
        cancelCast = true;
    }

    // BR 积分商店 — 欢乐制造器：召唤冬幕节"PX-238 Winter Wondervolt"整个布景（机关本体+陷阱+
    // 礼物/圣诞袜/圣诞树/告示牌/欢呼喇叭），10分钟后一起消失。直接复用真实entry，不需要clone/
    // 自定义AI——TRAP类型的自动触发靠引擎原生逻辑（GameObject.cpp:461起），走近的玩家会被180797
    // 自带的26275法术随机变身，可以在存续期间反复触发多次，不是GOOBER那种一次性状态机。
    // 各对象相对180797（陷阱本体，当参照锚点）的偏移，最初是从GM `.gobject near`截图OCR出来的
    // 坐标反推的，后来直接查了mangosdev的`gameobject`表核对了一遍——180798的Y坐标OCR漏了个负号，
    // 已经用DB里的真实精确值改过来，其它几个数值跟OCR版本基本一致。178434/178435圣诞袜和178667
    // 圣诞树在原场景里是挂在墙上/立在高台上的，height差有5+码，召唤到平地上会跟着悬空，这是真实
    // 场景数据决定的，不是算错了。
    // 冷却走61006这个纯标记法术手动查/写 Player::IsSpellReady/AddCooldown。
    if (pItem->GetEntry() == 900113)
    {
        SpellEntry const* cdMarker = sSpellMgr.GetSpellEntry(61006);
        if (cdMarker && !pUser->IsSpellReady(cdMarker))
        {
            ChatHandler(pUser).PSendSysMessage("瑞雪未歇，机关小憩，且待片刻，再闹一场。");
        }
        else
        {
            float x, y, z;
            pUser->GetClosePoint(x, y, z, pUser->GetObjectBoundingRadius(), 2.0f, 0.0f);
            float const orient = pUser->GetOrientation();
            // attach=false很关键：SummonGameObject默认attach=true会把这个GameObject挂到玩家名下当
            // "拥有者"（Unit::AddGameObject），而GameObject.cpp里TRAP类型的自动触发逻辑一旦有owner，
            // 就会用`owner->CastSpell(...)`而不是`CastSpell(...)`（陷阱自己当施法者）——这样一来
            // 26275法术里`m_caster->IsWithinDist(unitTarget, 1.0f)`那个"必须在陷阱1码内"的判定，
            // 实际比的是"玩家(拥有者)当前位置"跟"目标"的距离，不是"陷阱固定位置"跟目标的距离！
            // 单人测试时玩家自己就是被找到的目标，两者是同一个人，距离恒为0，判定形同虚设，只要在
            // 陷阱的搜索半径(8码)内就会触发——这才是之前反馈"离得很远还会变身"的真正原因，不是
            // buff自己周期性刷新。显式传false，陷阱没有owner，走`CastSpell`分支自己当施法者，
            // 恢复成"必须站在陷阱1码内"这个原版设计的判定。
            pUser->SummonGameObject(180797, x, y, z, orient, 0, 0, 0, 0, 600, false); // PX-238冬幕仙境机-陷阱（实际触发变身，锚点）

            // relOrient：每个对象在真实场景里自己的朝向减去锚点(180797)的朝向，召唤时叠加到玩家
            // 当前朝向上（Geometry::NormalizeOrientation归一化），这样每个对象不但位置跟着整体
            // 旋转，自己的朝向也保持跟原场景一致的相对关系，不会全部傻乎乎地朝着同一个方向。
            // 所有数值（含forward/left/height）都是直接从`gameobject`表里这一组真实坐标/朝向反查
            // 出来的（不是当初截图OCR的那份，那份180798的Y坐标缺了负号，已用DB数据核对纠正）。
            auto summonInScene = [&](uint32 entry, float forward, float left, float height, float relOrient)
            {
                float const px = x + forward * cos(orient) - left * sin(orient);
                float const py = y + forward * sin(orient) + left * cos(orient);
                float const objOrient = Geometry::NormalizeOrientation(orient + relOrient);
                pUser->SummonGameObject(entry, px, py, z + height, objOrient, 0, 0, 0, 0, 600, false);
            };

            summonInScene(180796, -0.3f, 1.275f, -0.06f, 3.22885f);    // PX-238冬幕仙境机（装饰模型）
            summonInScene(180798, -1.95f, 3.294f, -0.094f, 3.59537f);  // 大礼物
            summonInScene(180799, -2.57f, 2.575f, 0.026f, 3.68264f);  // 大礼物
            summonInScene(178746, 3.43f, 1.28f, -0.16f, 2.094391f);   // 烟林牧场（告示牌）
            summonInScene(178434, -1.14f, 3.469f, 5.229f, 2.67035f);  // 圣诞袜1（原场景挂墙上，会悬空）
            summonInScene(178435, 1.12f, 3.084f, 5.359f, 2.111844f);  // 圣诞袜2（同上）
            summonInScene(178667, -5.44f, 1.413f, 0.399f, 1.13446f);  // 圣诞树（中）
            summonInScene(180749, -9.03f, 1.376f, 0.228f, 0.698125f); // 欢呼扬声器

            // Pat's Snowcloud Guy（雪雾）：真实场景里是个生物，不是gameobject，坐标/朝向几乎跟180796
            // 完全重合（同一个点，只是Z高0.12码），SummonCreature的respawn参数是毫秒，不是秒。
            {
                float const forward = -0.3f, left = 1.275f, height = 0.06f, relOrient = 3.22885f;
                float const px = x + forward * cos(orient) - left * sin(orient);
                float const py = y + forward * sin(orient) + left * cos(orient);
                float const objOrient = Geometry::NormalizeOrientation(orient + relOrient);
                pUser->SummonCreature(15730, px, py, z + height, objOrient, TEMPSUMMON_TIMED_DESPAWN, 600 * 1000);
            }

            if (cdMarker)
                pUser->AddCooldown(cdMarker);

            pUser->TextEmote("瑞雪纷飞，机关轻响，欢声笑语，冬幕同庆！");
        }
        cancelCast = true;
    }

    // BR 令牌/餐桌/欢乐制造器/灵魂之井(900105/900109/900113/900115)：排查记录——SendEquipError + Spell::SendCastResult(道具真实
    // 法术ID) 这套比通用cancelCast hack更对症，但单独用仍然不够：实测发现"冷却拒绝"分支（只发
    // 聊天提示就直接走到这里）必卡，"召唤/报名成功"分支（走到这里之前额外真实完整施法过一次
    // 5001）反而不卡——说明真正解开客户端本地预判状态的，是"有没有一次真正完整走完的施法广播"，
    // 不只是发一个失败结果包。所以这里统一补一次真实的5001（Lotwil's Summoning）广播，不管这次
    // 点击是成功召唤还是冷却拒绝，都会有这一次完整广播。
    // 教训：曾经在这里用过8067(Party Time!)，只检查了它spellVisual1=0（没有粒子特效），没检查
    // 它effect1=SPELL_EFFECT_APPLY_AURA、effectApplyAuraName1=SPELL_AURA_DUMMY这个aura实际会
    // 做什么——SpellAuras.cpp:1611有个按spellId特判的case 8067，会真的调用SetDisplayId整人变身
    // （这仓库里"派对时间"这个真实功能，不是什么无害的空白法术）。5001虽然spellVisual1=74不是0，
    // 但它的效果经代码验证是真正的空操作（SPELL_EFFECT_SUMMON, effectMiscValue1=0, EffectSummon
    // 里petEntry为0会直接return，不会召唤任何东西），只是会多播一次它自带的特效，两个分支都会
    // 出现——比"完全没有反馈"更能接受，也比"意外触发别的功能"安全得多。
    if (cancelCast && (pItem->GetEntry() == 900105 || pItem->GetEntry() == 900109 || pItem->GetEntry() == 900113 || pItem->GetEntry() == 900115))
    {
        pUser->CastSpell(pUser, 5001, true);
        pUser->SendEquipError(EQUIP_ERR_NONE, pItem, nullptr);
        uint32 spellid = proto->Spells[packet.spellSlot].SpellId;
        if (SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellid))
            Spell::SendCastResult(_player, spellInfo, SPELL_FAILED_DONT_REPORT);
        return;
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
