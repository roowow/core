/* Copyright (C) 2009 - 2010 ScriptDevZero <http://github.com/scriptdevzero/scriptdevzero>
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
#include "Opcodes.h"
#include "scriptPCH.h"
#include "custom.h"
#include "ScriptedAI.h"
#include <ctime>

// Guild Bank NPC
// .npc summon 7292
// 14337 修理机器人

bool GossipHello_GuildBank_Bot(Player *player, Creature *_Creature)   
{
    std::map< uint32, OOGuildBank >::iterator OObank = sOOMgr.OOGuildBankVendors.find(_Creature->GetEntry());
    OOGuildBank OOGuildBank;
    if (OObank != sOOMgr.OOGuildBanks.end())
    {
        OOGuildBank = OObank->second;
        // guild check
        if (player->GetGuildId() == OOGuildBank.guild_id)
        {
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_VENDOR, "我想要看看仓库里的货物", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TRADE);

            if (!player->GetMap()->IsRaid())
            {
                // 道德风险，其他人恶意关闭 ？
                if (player->GetRank() <= OOGuildBank.guild_rank)
                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, "谢谢你的服务，再见！", GOSSIP_SENDER_SEC_BATTLEINFO, GOSSIP_ACTION_BATTLE);
            }
        }
    }

    player->SEND_GOSSIP_MENU(22029, _Creature->GetGUID());
    return true;
}

void SendDefaultMenu_GuildBank_Bot(Player *player, Creature *_Creature, uint32 action)
{
    switch (action)
    {
        case GOSSIP_ACTION_TRADE:
            player->SEND_VENDORLIST(_Creature->GetObjectGuid());
            break;
        case GOSSIP_ACTION_BATTLE:
            std::string msg = "尊敬的";
            msg += player->GetName();
            msg += "贵宾，下次再会！祝您生活愉快！";
            _Creature->MonsterSay(msg.c_str(), 0, 0);
            _Creature->DespawnOrUnsummon();
            break;
    }
}

void SendDefaultMenu_GuildBank_Bot2(Player *player, Creature *_Creature, uint32 action, char const* code)
{
    // auto it = sOOMgr.OOItems.find(code.c_str());
    // auto it1 = sOOMgr.OOItems.find(code); 
    // uint32 count1 = 0;

    // VendorItemData const* vItems = _Creature->GetVendorItems();
    // VendorItemData const* tItems = _Creature->GetVendorTemplateItems();
    // uint8 customitems = vItems ? vItems->GetItemCount() : 0;
    // uint8 numitems = customitems + (tItems ? tItems->GetItemCount() : 0);
    // uint8 count = 0;
    // VendorItem const* crItem;

    // WorldPacket data(SMSG_LIST_INVENTORY, (8 + 1 + numitems * 7 * 4));
    // data << ObjectGuid(_Creature->GetGUIDLow());

    // size_t count_pos = data.wpos();
    // data << uint8(count);

    CreatureData const* data = nullptr;
    data = sObjectMgr.GetCreatureData(127);
    if (data)
        printf("11111 \n");
    
    if (data->GetObjectGuid(127))
        printf("2222 \n");
    
    // Creature* unit = player->GetNPCIfCanInteractWith(data->GetObjectGuid(127), UNIT_NPC_FLAG_AUCTIONEER);
    // Creature* pCreature = player->GetMap()->GetCreature(data->GetObjectGuid(1751));
    
    switch (action)
    {
        case 2: 
            // std::wstring wnamepart;
            // // const char *cstr = code;
            // Utf8toWStr(code, wnamepart);

            // wstrToLower(wnamepart);

            // for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
            // {
            //     ItemPrototype const* pProto = &itr.second;

            //     int loc_idx = player->GetSession()->GetSessionDbLocaleIndex();
            //     if (loc_idx >= 0)
            //     {
            //         ItemLocale const* il = sObjectMgr.GetItemLocale(pProto->ItemId);
            //         if (il)
            //         {
            //             if ((int32)il->Name.size() > loc_idx && !il->Name[loc_idx].empty())
            //             {
            //                 std::string name = il->Name[loc_idx];
                            
            //                 if (Utf8FitTo(name, wnamepart))
            //                 {
            //                     printf("item  < %s %u\n", code, pProto->ItemId);
            //                     // ShowItemListHelper(pProto->ItemId, loc_idx, pl);
            //                     // ++counter;
            //                     // continue;
            //                 }
            //             }
            //         }
            //     }

            //     // std::string name = pProto->Name1;
            //     // if (name.empty())
            //     //     continue;

            //     // if (Utf8FitTo(name, wnamepart))
            //     // {
            //     //     ShowItemListHelper(pProto->ItemId, -1, pl);
            //     //     ++counter;
            //     // }
            // }

            // Search for a key
            
            // if (it != sOOMgr.OOItems.end()) { 
            //     _Creature->MonsterSay("item = %u\n", it->second.c_str());
            // }
            // if (it1 != sOOMgr.OOItems.end()) {
            //     _Creature->MonsterSay("item = %u\n", it1->second);
            //     printf("item = %u %s\n", it1->second, it1->first.c_str());

            //     sObjectMgr.AddVendorItem(_Creature->GetEntry(), it1->second, 10, 999999, 0);
            // }

            // sObjectMgr.LoadVendorTemplates();
            // sObjectMgr.LoadVendors();

            // for (int i = 0; i < numitems; ++i)
            // {
            //     crItem = i < customitems ? vItems->GetItem(i) : tItems->GetItem(i - customitems);

            //     if (crItem)
            //     {
            //         if (ItemPrototype const* pProto = sObjectMgr.GetItemPrototype(crItem->item))
            //         {
            //             ++count;

            //             data << uint32(count);
            //             data << uint32(crItem->item);
            //             data << uint32(pProto->DisplayInfoID);
            //             data << uint32(crItem->maxcount <= 0 ? 0xFFFFFFFF : _Creature->GetVendorItemCurrentCount(crItem));
            //             data << uint32(pProto->BuyPrice);
            //             data << uint32(pProto->MaxDurability);
            //             data << uint32(pProto->BuyCount);

            //             if (count >= MAX_VENDOR_ITEMS)
            //                 break;
            //         }
            //     }
            // }

            // if (count != 0)
            // {
            //     data.put<uint8>(count_pos, count);
            //     player->GetSession()->SendPacket(&data);
            // }
            // player->GetMap()->GetCreature();
            // player->GetSession()->SendListInventory(_Creature->GetObjectGuid());
            // player->GetSession()->SendAuctionHello(unit);
            // player->SetAuctionAccessMode(player->GetTeam() != ALLIANCE ? -1 : 0);
            // player->GetSession()->SendAuctionHello2(player);
            
            // player->GetSession()->SendListInventory(data->GetObjectGuid(1751));
            // SendListInventory(guid);

            // for (const auto& pair : sOOMgr.OOItems) {
            //     count1++;
            //     if (count1 < 5)
            //         printf("item x %u %s\n", pair.second, pair.first.c_str());
            // }

            // Search for the first key associated with targetValue
            // auto it = std::find_if(m_ItemLocaleMap.begin(), m_ItemLocaleMap.end(),
            //                     ["毛料"](const std::pair<const std::string, int>& pair) {
            //                         return pair.second == targetValue;
            //                     });

            // if (it != myMap.end()) {
            //     std::cout << "Found key for value " << targetValue << ": " << it->first << std::endl;
            // } else {
            //     std::cout << "Value " << targetValue << " not found." << std::endl;
            // }

            if (strcmp(code, "舍生取义") != 0)
            {
                _Creature->MonsterSay("你的签名不正确，希望你是故意的。");

                player->CLOSE_GOSSIP_MENU();
                break;
            }

            if (player->GetLevel() > 5)
            {
                _Creature->MonsterSay("我只招募刚刚返回归地球不超过5级的人类。");
                break;
            }

            _Creature->CastSpell(player, 15851, true);
            _Creature->MonsterSay("勇敢者，是人类的明灯，是行走的火炬，带来希望与光明。希望你恪守勇敢者准则，不要辱没了这三个字。", 0, 0);

            player->SetHardcore(true);

            player->CLOSE_GOSSIP_MENU();
            break;
        case 3: 
            if (strcmp(code, "确认") != 0)
            {
                _Creature->MonsterSay("你的输入不正确。");

                player->CLOSE_GOSSIP_MENU();
                break;
            }

            _Creature->CastSpell(player, 25823, true); // 艾露恩灯柱
            _Creature->MonsterSay("勇敢者，你完成了不可能完成的任务，你是人类的明灯！", 0, 0);

            player->AddAura(461, 0, player); // 正义火焰
            player->RemoveAurasDueToSpell(7363);
            player->SetHardcoreRetired();

            player->CLOSE_GOSSIP_MENU();
            break;
    }
    
}

bool GossipSelect_GuildBank_Bot(Player *player, Creature *_Creature, uint32 sender, uint32 action)
{
    SendDefaultMenu_GuildBank_Bot(player, _Creature, action);

    return true;
}

bool GossipSelect_GuildBank_Bot2(Player *player, Creature *_Creature, uint32 sender, uint32 action, char const* code)
{
    SendDefaultMenu_GuildBank_Bot2(player, _Creature, action, code);

    return true;
}

void AddSC_OO_guild_bank_bot()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "custom_oo_npc_guild_bank_bot";
    newscript->pGossipHello = &GossipHello_GuildBank_Bot;
    newscript->pGossipSelect = &GossipSelect_GuildBank_Bot;
    newscript->pGossipSelectWithCode = &GossipSelect_GuildBank_Bot2;
    newscript->RegisterSelf(false);
}
