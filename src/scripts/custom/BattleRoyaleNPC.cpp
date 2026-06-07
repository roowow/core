#include "scriptPCH.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "Chat/Chat.h"

static bool BRPlayerHasWon(Player* player)
{
    auto res = CharacterDatabase.PQuery(
        "SELECT `total_wins` FROM `battle_royale_season_score` WHERE `guid` = %u",
        player->GetGUIDLow());
    return res && res->Fetch()[0].GetUInt32() >= 1;
}

// NPC entries — 900100 = Alliance (Ironforge, displayId 15728), 900102 = Horde (Orgrimmar, displayId 15731)
static uint32 const NPC_BATTLE_ROYALE_QUEUE = 900100;
static uint32 const BR_GOSSIP_TEXT_QUEUE    = 65021;
static uint32 const ITEM_BR_TOKEN           = 900105;

enum BRGossipAction
{
    BR_ACTION_JOIN      = 1,
    BR_ACTION_LEAVE     = 2,
    BR_ACTION_STATUS    = 3,
    BR_ACTION_GET_TOKEN = 4,
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

    if (!player->HasItemCount(ITEM_BR_TOKEN, 1) && BRPlayerHasWon(player))
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "领取论剑帖（可随身远程报名）", GOSSIP_SENDER_MAIN, BR_ACTION_GET_TOKEN);

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
        case BR_ACTION_GET_TOKEN:
        {
            if (player->HasItemCount(ITEM_BR_TOKEN, 1))
            {
                ChatHandler(player).PSendSysMessage("[孤胆称雄] 你已持有论剑帖。");
                break;
            }
            if (!BRPlayerHasWon(player))
            {
                ChatHandler(player).PSendSysMessage("[孤胆称雄] 须在此局中夺得魁首，方可持此帖。");
                break;
            }
            ItemPosCountVec dest;
            InventoryResult res = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_BR_TOKEN, 1);
            if (res != EQUIP_ERR_OK)
            {
                player->SendEquipError(res, nullptr, nullptr);
                break;
            }
            Item* token = player->StoreNewItem(dest, ITEM_BR_TOKEN, true);
            player->SendNewItem(token, 1, true, false);
            break;
        }
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
