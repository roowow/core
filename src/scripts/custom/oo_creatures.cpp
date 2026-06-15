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
        if (! player->IsHardcoreRetired() && player->GetLevel() == 60)
            player->ADD_GOSSIP_ITEM_EXTENDED(0, "我要退役，输入：|cFFFF0000确认|r。", 2, 3, "", true);

        player->PrepareQuestMenu(_Creature->GetGUID());
        player->SEND_GOSSIP_MENU(22004, _Creature->GetGUID());
    }
    else
    {
        player->ADD_GOSSIP_ITEM(0, "《勇敢者小队征集令》", GOSSIP_SENDER_MAIN, 1);

        player->PrepareQuestMenu(_Creature->GetGUID());
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
        if (! player->oowowInfo.cache_HardcoreGossipHello || time(nullptr) - player->oowowInfo.cache_HardcoreGossipHello > 300)
        {
            _Creature->PlayDirectSound(6734, player); // Moment-Orgrimmar

            player->oowowInfo.cache_HardcoreGossipHello = time(nullptr);
        }
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
                _Creature->MonsterSay("你的签名不正确，希望你是故意的。");

                player->CLOSE_GOSSIP_MENU();
                break;
            }

            if (player->GetLevel() > 5)
            {
                _Creature->MonsterSay("我只招募刚刚返回归地球不超过5级的人类。");
                break;
            }

            // Require at least one level-60 character on the same account.
            {
                auto levelCheck = CharacterDatabase.PQuery(
                    "SELECT 1 FROM `characters` WHERE `account` = %u AND `level` = 60 LIMIT 1",
                    player->GetSession()->GetAccountId());
                bool hasMaxLevelChar = (levelCheck != nullptr);

                if (!hasMaxLevelChar)
                {
                    _Creature->MonsterSay("勇敢者之路，非轻率之举。先将一位英雄带至巅峰，方可踏上这条不归路。");
                    player->CLOSE_GOSSIP_MENU();
                    break;
                }
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

// ── 天选者 NPC ────────────────────────────────────────────────

void SendDefaultMenu_TianxuanNPC(Player* player, Creature* creature, uint32 action)
{
    switch (action)
    {
        case 1:
            // 描述页：npc_text 22041 存放天选者规则介绍文案，直接弹出口令输入框
            player->ADD_GOSSIP_ITEM_EXTENDED(0, "踏上天选者之路，输入：|cFFFF0000天命所归|r。", 2, 3, "", true);
            player->SEND_GOSSIP_MENU(22041, creature->GetGUID());
            break;
    }
}

void SendDefaultMenu_TianxuanNPC2(Player* player, Creature* creature, uint32 action, char const* code)
{
    switch (action)
    {
        case 3:
            if (strcmp(code, "天命所归") != 0)
            {
                creature->MonsterSay("天命之人，自有天定。你的答案不对。", 0, 0);
                player->CLOSE_GOSSIP_MENU();
                break;
            }

            if (player->GetLevel() > 5)
            {
                creature->MonsterSay("天选者之路，须于踏出新手村之前立誓，方得天命眷顾。", 0, 0);
                player->CLOSE_GOSSIP_MENU();
                break;
            }

            if (player->IsTianxuan())
            {
                creature->MonsterSay("天命已降，印记已刻，无需重誓。", 0, 0);
                player->CLOSE_GOSSIP_MENU();
                break;
            }

            player->SendSpellGo(player, 24240); // 闪电视觉
            creature->MonsterSay("天命所归，此路唯你独行。愿你不负天选，功成名就。", 0, 0);

            player->SetTianxuan(true);

            ChatHandler(player->GetSession()).PSendSysMessage("【天选】承天命，踏孤途。天选者之路，由此而始。");

            player->CLOSE_GOSSIP_MENU();
            break;
    }
}

bool GossipHello_TianxuanNPC(Player* player, Creature* creature)
{
    if (player->IsTianxuan())
    {
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "天命已降，印记在身。愿不负天选。", GOSSIP_SENDER_MAIN, 0);
        player->PrepareQuestMenu(creature->GetGUID());
        player->SEND_GOSSIP_MENU(22042, creature->GetGUID());
        return true;
    }

    if (player->GetLevel() > 5)
    {
        creature->MonsterSay("天选者之路，须于踏出新手村之前立誓。", 0, 0);
        player->PrepareQuestMenu(creature->GetGUID());
        player->SEND_GOSSIP_MENU(22040, creature->GetGUID());
        return true;
    }

    player->ADD_GOSSIP_ITEM(0, "《天命独行令》", GOSSIP_SENDER_MAIN, 1);
    player->PrepareQuestMenu(creature->GetGUID());
    player->SEND_GOSSIP_MENU(22040, creature->GetGUID());
    return true;
}

bool GossipSelect_TianxuanNPC(Player* player, Creature* creature, uint32 sender, uint32 action)
{
    SendDefaultMenu_TianxuanNPC(player, creature, action);
    return true;
}

bool GossipSelect_TianxuanNPC2(Player* player, Creature* creature, uint32 sender, uint32 action, char const* code)
{
    SendDefaultMenu_TianxuanNPC2(player, creature, action, code);
    return true;
}

// ─────────────────────────────────────────────────────────────

void AddSC_OO_creatures()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "custom_oo_npc_hardcore";
    newscript->pGossipHello = &GossipHello_HardcoreNPC;
    newscript->pGossipSelect = &GossipSelect_HardcoreNPC;
    newscript->pGossipSelectWithCode = &GossipSelect_HardcoreNPC2;
    newscript->RegisterSelf(false);

    newscript = new Script;
    newscript->Name = "custom_oo_npc_tianxuan";
    newscript->pGossipHello = &GossipHello_TianxuanNPC;
    newscript->pGossipSelect = &GossipSelect_TianxuanNPC;
    newscript->pGossipSelectWithCode = &GossipSelect_TianxuanNPC2;
    newscript->RegisterSelf(false);
}
