#include "JianJiaAI.h"
#include "OO/WebChatMgr.h"
#include "PlayerBots/PlayerBotMgr.h"
#include "Server/WorldSession.h"
#include "Server/Packet.h"
#include "Opcodes.h"
#include "ObjectMgr.h"
#include "Group/Group.h"
#include "Log.h"

bool JianJiaAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    // Load the existing character from DB (same as base class)
    sess->LoginPlayer(entry->playerGUID);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "JianJiaAI: 蒹葭 (guid %u) logged in.", entry->playerGUID);
    return true;
}

void JianJiaAI::OnPacketReceived(WorldPacket const* packet)
{
    if (!me) return;
    switch (packet->GetOpcode())
    {
        case SMSG_GROUP_INVITE:
        {
            // 蒹葭不需要物理入队（ChatHandler 层已拦截所有小队/团队消息并转发给 AI）。
            // 直接拒绝所有邀请，避免占用唯一的组队槽位。
            auto reply = std::make_unique<NullClientPacket>(CMSG_GROUP_DECLINE);
            me->GetSession()->QueuePacket(std::move(reply));
            break;
        }
        case SMSG_MESSAGECHAT:
        {
            if (!sWebChatMgr.IsJianJiaActive()) break;

            WorldPacket pkt(*packet);
            uint8  chatType;
            uint32 language;
            pkt >> chatType >> language;

            // 小队/团队消息已在 ChatHandler 层转发，这里只处理战场频道。
            if (chatType != CHAT_MSG_BATTLEGROUND)
                break;

            ObjectGuid senderGuid;
            pkt >> senderGuid;

            uint32 msgLen;
            pkt >> msgLen;
            std::string message;
            pkt >> message;

            if (senderGuid == me->GetObjectGuid()) break;
            if (message.empty() || language == LANG_ADDON) break;

            Player* sender = sObjectMgr.GetPlayer(senderGuid);
            if (!sender) break;

            uint32 groupId = sender->GetGroup() ? sender->GetGroup()->GetId() : 0;
            sWebChatMgr.ForwardGroupChatToJianJia(sender->GetName(), message, "bg", groupId);
            break;
        }
        default:
            break;
    }
}
