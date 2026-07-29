#include "scriptPCH.h"
#include "BattleRoyale/BattleRoyaleMgr.h"
#include "Chat/Chat.h"
#include "Database/DBCStores.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"

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
    BR_ACTION_SHOP      = 5,
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
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "领取论剑令", GOSSIP_SENDER_MAIN, BR_ACTION_GET_TOKEN);

    // 积分商店：这个NPC同时挂了 UNIT_NPC_FLAG_VENDOR，但因为这里手动接管了 gossip 菜单（没有走
    // 默认的 PrepareGossipMenu 流程），商人选项不会被引擎自动加上，必须自己加一条并手动弹商人窗口。
    if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR))
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_VENDOR, "看看令使的珍藏", GOSSIP_SENDER_MAIN, BR_ACTION_SHOP);

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
        case BR_ACTION_SHOP:
            player->SEND_VENDORLIST(creature->GetObjectGuid());
            break;
        default:
            break;
    }
    return true;
}

// 亲吻/敬礼/跳舞/鞠躬/鼓掌——EmotesText.dbc 里的 text_emote ID。实测得出：在
// ChatHandler.cpp::HandleTextEmoteOpcode 里临时加日志打印 packet.textEmote，游戏内依次输入
// /kiss /salute /dance /bow /clap 各一次，从日志读到的真实数值（测试记录见AloneMode.md同类
// 排查方式，此处对应 BattleRoyale.md 餐桌功能）。
static uint32 const BR_TABLE_THANKS_EMOTES[] = { 58, 78, 34, 17, 24 };

// 让player对target做一个"真实"的client文字表情（带动作+"XX对YY做了个YY"聊天提示），
// 复刻自 ChatHandler.cpp::HandleTextEmoteOpcode 的核心逻辑（那边的 MaNGOS::EmoteChatBuilder
// 是文件内私有类，没有导出头文件，这里就没有再依赖localization/CameraDistWorker那一整套，
// 简化成直接用玩家当前语言环境的名字广播，对这种小彩蛋功能足够）。
static void DoTargetedTextEmote(Player* player, Unit* target, uint32 textEmoteId)
{
    EmotesTextEntry const* em = sEmotesTextStore.LookupEntry(textEmoteId);
    if (!em)
        return;

    switch (em->textid)
    {
        case EMOTE_STATE_SLEEP:
        case EMOTE_STATE_SIT:
        case EMOTE_STATE_KNEEL:
        case EMOTE_ONESHOT_NONE:
            break;
        default:
            player->HandleEmote(em->textid);
            break;
    }

    char const* name = target ? target->GetName() : nullptr;
    uint32 const namlen = (name ? uint32(strlen(name)) : 0) + 1;

    WorldPacket data(SMSG_TEXT_EMOTE, 20 + namlen);
    data << player->GetObjectGuid();
    data << uint32(textEmoteId);
    data << uint32(0); // emoteNum：配合动作播放的额外数字表情，这里不需要
    data << uint32(namlen);
    if (namlen > 1)
        data.append(name, namlen);
    else
        data << uint8(0);

    player->SendMessageToSet(&data, true);
}

// BR 积分商店 - 餐桌的面包/水道具：点一下直接施法给点击者、立刻返回true。
// 不能用GOOBER默认的"进入使用中状态->等冷却->回到就绪"这条路——这两个道具是SummonGameObject
// 野生召唤出来的，没有数据库出生记录（m_spawnedByDefault=false），只要它真的走完一次冷却到期的
// 状态转换（GO_ACTIVATED -> GO_JUST_DEACTIVATED），GameObject.cpp里对应逻辑会顺带把它删除——
// 不管冷却设多长，"能被删除"和"能重复点击"绑在同一条状态机上，没法只要一半。
// 这里直接接管OnUse，绕开整个状态机：施法后返回true会让GameObject::Use()提前return，
// 后面那一整套状态变化根本不会发生，对象保持原样，可以无限次点击，直到5分钟自然到期消失。
struct go_br_refreshment : public GameObjectAI
{
    go_br_refreshment(GameObject* go) : GameObjectAI(go) {}

    uint32 m_ambientSoundTimer = 0;

    // 灵魂之井(900116)：每隔10秒播放一次MoonWellLightLoop环境音(8193)，营造"月井光辉、
    // 灵魂低语"的氛围（原来用WispLoop(3349)已验证能正常3D定位播放，8193同样是Loop结尾的
    // 环境音效资源，跟3349同一类，主题上更贴"井"而换过来）。
    // 面包(900109，即摆桌子时召唤的那份"桌子代表"实体)：每隔10秒播放一次FluteRun(7734)
    // 短笛声，用户实测确认能正常听到。最初用的8440(Darkmoon Faire整曲配乐)在PlayDistanceSound
    // 这种3D定位播放方式下客户端不出声——排查确认是"整曲配乐"类资源不支持定位播放，只有
    // Loop结尾/短促的环境音效(SFX)类资源才行，见BattleRoyale.md/TODO.md排查记录。
    // 水(900111)不出声，避免两个音源同时播放互相打架。
    // PlayDistanceSound自带sourceGuid，客户端按3D定位播放，离得越近听得越清楚，不用自己
    // 算距离；SendObjectMessageToSet天然只发给"看得见这个对象"的附近玩家，不会传到全图。
    void UpdateAI(uint32 const uiDiff) override
    {
        uint32 soundId = 0;
        uint32 interval = 0;
        if (me->GetEntry() == 900116)
        {
            soundId = 8193;  // MoonWellLightLoop
            interval = 10000;
        }
        else if (me->GetEntry() == 900109)
        {
            soundId = 7734;  // FluteRun
            interval = 10000;
        }
        else
            return;

        if (m_ambientSoundTimer <= uiDiff)
        {
            me->PlayDistanceSound(soundId);
            m_ambientSoundTimer = interval;
        }
        else
            m_ambientSoundTimer -= uiDiff;
    }

    bool OnUse(Unit* user) override
    {
        if (Player* player = user->ToPlayer())
        {
            uint32 spellId = me->GetGOInfo()->goober.spellId;
            // 面包(900109)按服务端当前阶段分：22895 Conjured Cinnamon Roll是1.11版本才加入的道具，
            // 1.11之前的客户端不认识这个item，所以1.11+用61007(产22895魔法肉桂面包)，
            // 1.11之前继续用配置好的61003(产8076魔法甜面包)。
            if (me->GetEntry() == 900109 && sWorld.GetWowPatch() >= WOW_PATCH_111)
                spellId = 61007;
            player->CastSpell(player, spellId, true);

            // 对摆桌子的玩家随机做一个感谢表情（亲吻/敬礼/跳舞/鞠躬/鼓掌）。摆桌子时
            // SpellHandler.cpp 里 SummonGameObject 后手动 SetOwnerGuid 打了标记，这里反查回去；
            // 自己吃自己摆的桌子就不用对自己做表情了。
            if (ObjectGuid ownerGuid = me->GetOwnerGuid())
                if (ownerGuid != player->GetObjectGuid())
                    if (Player* summoner = ObjectAccessor::FindPlayer(ownerGuid))
                        DoTargetedTextEmote(player, summoner, BR_TABLE_THANKS_EMOTES[urand(0, 4)]);
        }
        return true;
    }
};

GameObjectAI* GetAI_go_br_refreshment(GameObject* go)
{
    return new go_br_refreshment(go);
}

void AddSC_BattleRoyaleNPC()
{
    Script* newscript = new Script;
    newscript->Name            = "npc_battle_royale_queue";
    newscript->pGossipHello    = &GossipHello_BattleRoyaleNPC;
    newscript->pGossipSelect   = &GossipSelect_BattleRoyaleNPC;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name    = "go_br_refreshment";
    newscript->GOGetAI = &GetAI_go_br_refreshment;
    newscript->RegisterSelf();
}
