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
#include "MapManager.h"
#include "PlayerBotMgr.h"
#include "PlayerBotAI.h"
#include "PartyBotAI.h"
#include <ctime>

// Guild Bank NPC
// .npc summon 7292
// 14337 914337 修理机器人

bool GossipHello_GuildBank(Player *player, Creature *_Creature)   
{
    // 欢迎来到自助仓库服务
    if (player->GetGuildId())
    {
        for (auto i = sOOMgr.OOGuildBanks.begin(); i != sOOMgr.OOGuildBanks.end(); i++)
        {
            OOGuildBank OOGuildBank = i->second;

            if (player->GetGuildId() == OOGuildBank.guild_id && player->GetRank() <= OOGuildBank.guild_rank)
            {
                std::string msg = std::string("接入仓库《") +  OOGuildBank.name + std::string("》") ;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TAXI, msg.c_str(), GOSSIP_SENDER_SEC_BANK, OOGuildBank.vendor_id);
            }
        }
    }

    player->SEND_GOSSIP_MENU(22028, _Creature->GetGUID());
    return true;
}

void SendDefaultMenu_GuildBank(Player *player, Creature *_Creature, uint32 action)
{
    player->CLOSE_GOSSIP_MENU();

    std::list<Creature*> botList;
    bool botExisted = false;
    GetCreatureListWithEntryInGrid(botList, player, action, VISIBILITY_DISTANCE_NORMAL);
    for (const auto& it : botList)
    {
        std::string msg = "尊敬的";
        msg += player->GetName();
        msg += "贵宾，我在这里为您服务。";
        it->MonsterSay(msg.c_str(), 0, 0);

        botExisted = true;
    }

    // uint32 lastSummonned = 0;
    // std::map< uint32, std::map< uint32, uint32 > >::iterator it1 = sOOMgr.OOGuildBankCreatureBots.find(_Creature->GetGUIDLow());
    // if (it1 != sOOMgr.OOGuildBankCreatureBots.end())
    // {
    //     std::map< uint32, uint32 >::iterator it2 = it1->second.find(action);
    //     if (it2 != it1->second.end())
    //     {
    //         lastSummonned = sOOMgr.OOGuildBankCreatureBots[_Creature->GetGUIDLow()][action];
    //     }
    // }

    // if (lastSummonned + 10 * MINUTE < time(nullptr))
    if (!botExisted)
    {
        Creature* pCreature = player->SummonCreature(action, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN, 10*60*1000);
        std::string msg = "尊敬的";
        msg += player->GetName();
        msg += "贵宾，";
        msg += std::to_string(urand(1000, 9999));
        msg += "号智能机器人很荣幸为您服务！我的服务时间为10分钟。";
        if (pCreature)
            pCreature->MonsterSay(msg.c_str(), 0, 0);

        // sOOMgr.OOGuildBankCreatureBots[_Creature->GetGUIDLow()][action] = time(nullptr);
    }
    // else
    // {
    //     player->PSendSysMessage("机器人正在服务或繁忙。");
    // }
}

void SendDefaultMenu_GuildBank2(Player *player, Creature *_Creature, uint32 action, char const* code)
{
}

bool GossipSelect_GuildBank(Player *player, Creature *_Creature, uint32 sender, uint32 action)
{
    SendDefaultMenu_GuildBank(player, _Creature, action);

    return true;
}

bool GossipSelect_GuildBank2(Player *player, Creature *_Creature, uint32 sender, uint32 action, char const* code)
{
    SendDefaultMenu_GuildBank2(player, _Creature, action, code);

    return true;
}

void AddSC_OO_guild_bank()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "custom_oo_npc_guild_bank";
    newscript->pGossipHello = &GossipHello_GuildBank;
    newscript->pGossipSelect = &GossipSelect_GuildBank;
    newscript->pGossipSelectWithCode = &GossipSelect_GuildBank2;
    newscript->RegisterSelf(false);
}
