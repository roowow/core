#include "scriptPCH.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "Chat/Chat.h"

// NPC entries — 900100 = Alliance (Ironforge, displayId 15728), 900102 = Horde (Orgrimmar, displayId 15731)
static uint32 const NPC_BATTLE_ROYALE_QUEUE = 900100;
static uint32 const BR_GOSSIP_TEXT_QUEUE = 65021;

enum BRGossipAction
{
    BR_ACTION_JOIN    = 1,
    BR_ACTION_LEAVE   = 2,
    BR_ACTION_STATUS  = 3,
};

bool GossipHello_BattleRoyaleNPC(Player* player, Creature* creature)
{
    bool inQueue = sBattleRoyaleMgr.IsPlayerInQueue(player->GetObjectGuid());
    bool inGame  = sBattleRoyaleMgr.IsPlayerInGame(player->GetObjectGuid());

    if (!inQueue && !inGame)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, "接下论剑帖，报名入局", GOSSIP_SENDER_MAIN, BR_ACTION_JOIN);
    if (inQueue)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT,   "收起论剑帖，离开候战", GOSSIP_SENDER_MAIN, BR_ACTION_LEAVE);

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "查看候战席人数", GOSSIP_SENDER_MAIN, BR_ACTION_STATUS);

    player->SEND_GOSSIP_MENU(BR_GOSSIP_TEXT_QUEUE, creature->GetGUID());
    return true;
}

bool GossipSelect_BattleRoyaleNPC(Player* player, Creature* creature, uint32 /*sender*/, uint32 action)
{
    player->CLOSE_GOSSIP_MENU();

    switch (action)
    {
        case BR_ACTION_JOIN:
        {
            std::string err;
            if (!sBattleRoyaleMgr.EnqueuePlayer(player, err))
                ChatHandler(player).PSendSysMessage("[孤胆称雄] %s", err.c_str());
            break;
        }
        case BR_ACTION_LEAVE:
            sBattleRoyaleMgr.DequeuePlayer(player);
            break;
        case BR_ACTION_STATUS:
            ChatHandler(player).PSendSysMessage("[孤胆称雄] 当前队列：%u 人。", sBattleRoyaleMgr.GetQueueSize());
            break;
        default:
            break;
    }
    return true;
}

void AddSC_BattleRoyaleNPC()
{
    Script* newscript = new Script;
    newscript->Name            = "npc_battle_royale_queue";
    newscript->pGossipHello    = &GossipHello_BattleRoyaleNPC;
    newscript->pGossipSelect   = &GossipSelect_BattleRoyaleNPC;
    newscript->RegisterSelf();
}
