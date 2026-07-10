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
            auto reply = std::make_unique<NullClientPacket>(CMSG_GROUP_ACCEPT);
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

            if (chatType != CHAT_MSG_PARTY && chatType != CHAT_MSG_RAID &&
                chatType != CHAT_MSG_BATTLEGROUND)
                break;

            ObjectGuid senderGuid;
            pkt >> senderGuid;

            // CHAT_MSG_PARTY writes senderGuid twice (see BuildChatPacket)
            if (chatType == CHAT_MSG_PARTY)
            {
                ObjectGuid dummy;
                pkt >> dummy;
            }

            uint32 msgLen;
            pkt >> msgLen;
            std::string message;
            pkt >> message;

            if (senderGuid == me->GetObjectGuid()) break; // ignore own messages
            if (message.empty() || language == LANG_ADDON) break;

            Player* sender = sObjectMgr.GetPlayer(senderGuid);
            if (!sender) break;

            char const* ctx = (chatType == CHAT_MSG_BATTLEGROUND) ? "bg" :
                              (chatType == CHAT_MSG_RAID)          ? "raid" : "party";
            uint32 groupId = sender->GetGroup() ? sender->GetGroup()->GetId() : 0;
            sWebChatMgr.ForwardGroupChatToJianJia(sender->GetName(), message, ctx, groupId);
            break;
        }
        default:
            break;
    }
}
