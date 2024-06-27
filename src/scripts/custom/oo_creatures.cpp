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

#include "scriptPCH.h"
#include "custom.h"
#include "ScriptedAI.h"
#include <ctime>

// Hardcore NPC

bool GossipHello_HardcoreNPC(Player *player, Creature *_Creature)   
{
    if (player->IsHardcore())
    {
        // player:GossipMenuAddItem(0, "我要退役，输入：|cFFFF0000确认|r。", 1, 3, true, nil)
        if (! player->IsHardcoreRetired())
            player->ADD_GOSSIP_ITEM_EXTENDED(0, "我要退役，输入：|cFFFF0000确认|r。", 2, 3, "", true);

        player->SEND_GOSSIP_MENU(22004, _Creature->GetGUID());
    }
    else
    {player->ADD_GOSSIP_ITEM_EXTENDED(0, "我要退役，输入：|cFFFF0000确认|r。", 2, 3, "", true);
        player->ADD_GOSSIP_ITEM(0, "《勇敢者小队征集令》",               GOSSIP_SENDER_MAIN, 1);
        player->SEND_GOSSIP_MENU(22003, _Creature->GetGUID());
    }

    // ALLIANCE
    if (player->GetTeam() == ALLIANCE)
    {
        if (! player->oowowInfo.cache_HardcoreGossipHello || time(nullptr) - player->oowowInfo.cache_HardcoreGossipHello > 300)
        {
            _Creature->PlayDirectSound(7338, player); // Moment-KingsTheme

            player->oowowInfo.cache_HardcoreGossipHello = time(nullptr);
        }
    }
    // HORDE
    else
    {
        _Creature->PlayDirectSound(6734, player); // Moment-Orgrimmar
    }

    return true;
}

void SendDefaultMenu_HardcoreNPC(Player *player, Creature *_Creature, uint32 action)
{
    switch (action)
    {
        case 1:
            player->ADD_GOSSIP_ITEM_EXTENDED(0, "加入勇敢者，签署生死状：|cFFFF0000舍生取义|r。", 2, 2, "", true);
            player->SEND_GOSSIP_MENU(22005, _Creature->GetGUID());
            break;
    }
}

void SendDefaultMenu_HardcoreNPC2(Player *player, Creature *_Creature, uint32 action, char const* code)
{
    switch (action)
    {
        case 2: 
            if (strcmp(code, "舍生取义") != 0)
            {
                ChatHandler(player).SendSysMessage("你的签名不正确，希望你是故意的。");

                player->CLOSE_GOSSIP_MENU();
                break;
            }

            // player->SetHardcore(true);
            _Creature->CastSpell(player, 15852, true);
            _Creature->MonsterSay("勇敢者，是人类的明灯，是行走的火炬，带来希望与光明。希望你恪守勇敢者准则，不要辱没了这三个字。", 0, 0);

            player->CLOSE_GOSSIP_MENU();
            break;
        case 3: 
            if (strcmp(code, "确认") != 0)
            {
                ChatHandler(player).SendSysMessage("你的输入不正确。");

                player->CLOSE_GOSSIP_MENU();
                break;
            }
            
            // player->SetHardcoreRetired();
            _Creature->CastSpell(player, 25823, true); // 艾露恩灯柱
            _Creature->MonsterSay("勇敢者，你是人类的明灯！", 0, 0);

            player->CLOSE_GOSSIP_MENU();
            break;
    }
    
}

bool GossipSelect_HardcoreNPC(Player *player, Creature *_Creature, uint32 sender, uint32 action)
{
    SendDefaultMenu_HardcoreNPC(player, _Creature, action);

    return true;
}

bool GossipSelect_HardcoreNPC2(Player *player, Creature *_Creature, uint32 sender, uint32 action, char const* code)
{
    SendDefaultMenu_HardcoreNPC2(player, _Creature, action, code);

    return true;
}

void AddSC_OO_creatures()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "custom_oo_npc_hardcore";
    newscript->pGossipHello = &GossipHello_HardcoreNPC;
    newscript->pGossipSelect = &GossipSelect_HardcoreNPC;
    newscript->pGossipSelectWithCode = &GossipSelect_HardcoreNPC2;
    newscript->RegisterSelf(false);
}
