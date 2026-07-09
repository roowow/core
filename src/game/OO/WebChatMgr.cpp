#include "WebChatMgr.h"
#include "Battlegrounds/BattleGround.h"
#include "Battlegrounds/BattleGroundAfkMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Chat/Chat.h"
#include "Chat/Channel.h"
#include "Chat/ChannelMgr.h"
#include "Guild/Guild.h"
#include "Guild/GuildMgr.h"
#include "Group/Group.h"
#include "Objects/Player.h"
#include "Server/WorldSession.h"
#include <hiredis/hiredis.h>
#include <ctime>

WebChatMgr& WebChatMgr::instance()
{
    static WebChatMgr s;
    return s;
}

void WebChatMgr::Initialize(char const* socketPath, uint32 realmId)
{
    m_socketPath = socketPath;
    m_keyLive    = "web_chat:live:"    + std::to_string(realmId);
    m_keyHistory = "web_chat:history:" + std::to_string(realmId);
    m_keyJianJiaIn  = "web_chat:jianjia_in:" + std::to_string(realmId);
    m_keyJianJiaOut = "web_chat:jianjia_out:"+ std::to_string(realmId);
    m_jianJiaName   = ""; // set by World.cpp after Initialize() via SetJianJiaName()

    m_pubCtx = redisConnectUnix(socketPath);
    if (!m_pubCtx || m_pubCtx->err)
    {
        // Redis not available — WebChat disabled, no error
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "WebChatMgr: Redis not available at %s, WebChat disabled.", socketPath);
        if (m_pubCtx) { redisFree(m_pubCtx); m_pubCtx = nullptr; }
        return;
    }
    m_stop = false;
    m_subThread = std::thread(&WebChatMgr::SubscribeThread, this);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "WebChatMgr: connected to Redis (realm %u), WebChat enabled.", realmId);
}

void WebChatMgr::Shutdown()
{
    m_stop = true;
    // Freeing the subscribe context unblocks redisGetReply in the thread
    if (m_subCtx) { redisFree(m_subCtx); m_subCtx = nullptr; }
    if (m_subThread.joinable()) m_subThread.join();
    if (m_pubCtx) { redisFree(m_pubCtx); m_pubCtx = nullptr; }
}

void WebChatMgr::Update()
{
    if (m_pending.empty()) return;

    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        local.swap(m_pending);
    }
    while (!local.empty())
    {
        DispatchWebMessage(local.front());
        local.pop();
    }
}

// ── game → Redis ──────────────────────────────────────────────────────────────

void WebChatMgr::WriteWebChat(std::string const& channel, std::string const& charName,
    uint32 faction, uint32 classId, std::string const& recipient, std::string const& msg)
{
    if (!m_pubCtx) return;
    std::string json = BuildJson("game", channel, charName, faction, classId, recipient, msg);
    Publish(json);
}

void WebChatMgr::Publish(std::string const& json)
{
    redisAppendCommand(m_pubCtx, "PUBLISH %s %b", m_keyLive.c_str(), json.data(), json.size());
    redisAppendCommand(m_pubCtx, "LPUSH %s %b", m_keyHistory.c_str(), json.data(), json.size());
    redisAppendCommand(m_pubCtx, "LTRIM %s 0 99", m_keyHistory.c_str());

    for (int i = 0; i < 3; ++i)
    {
        redisReply* r = nullptr;
        if (redisGetReply(m_pubCtx, (void**)&r) != REDIS_OK)
        {
            ReconnectPub();
            return;
        }
        freeReplyObject(r);
    }
}

void WebChatMgr::ReconnectPub()
{
    if (m_pubCtx) { redisFree(m_pubCtx); m_pubCtx = nullptr; }
    m_pubCtx = redisConnectUnix(m_socketPath.c_str());
    if (m_pubCtx && m_pubCtx->err) { redisFree(m_pubCtx); m_pubCtx = nullptr; }
}

// ── Redis → game (subscribe thread) ──────────────────────────────────────────

void WebChatMgr::SubscribeThread()
{
    m_subCtx = redisConnectUnix(m_socketPath.c_str());
    if (!m_subCtx || m_subCtx->err)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "WebChatMgr: subscriber failed to connect to Redis");
        return;
    }

    {
        redisReply* r = (redisReply*)redisCommand(m_subCtx, "SUBSCRIBE %s %s",
            m_keyLive.c_str(), m_keyJianJiaOut.c_str());
        if (!r) return;
        freeReplyObject(r);
        // SUBSCRIBE to 2 channels produces 2 confirmation replies; consume the extra one
        {
            redisReply* rx = nullptr;
            if (redisGetReply(m_subCtx, (void**)&rx) == REDIS_OK && rx)
                freeReplyObject(rx);
        }
    }

    while (!m_stop.load(std::memory_order_relaxed))
    {
        redisReply* reply = nullptr;
        if (redisGetReply(m_subCtx, (void**)&reply) != REDIS_OK)
            break; // connection closed (Shutdown freed the context)

        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3
            && reply->element[1]->type == REDIS_REPLY_STRING
            && reply->element[2]->type == REDIS_REPLY_STRING)
        {
            std::string chan(reply->element[1]->str, reply->element[1]->len);
            std::string json(reply->element[2]->str, reply->element[2]->len);

            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (chan == m_keyLive && JsonGetStr(json, "source") == "web")
                m_pending.push(std::move(json));
            else if (chan == m_keyJianJiaOut)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "JianJia DEBUG: jianjia_out received: %s", json.c_str());
                m_jianJiaPending.push(std::move(json));
            }
        }
        freeReplyObject(reply);
    }
}

// ── main thread dispatch ──────────────────────────────────────────────────────

void WebChatMgr::DispatchWebMessage(std::string const& json)
{
    std::string channel   = JsonGetStr(json, "channel");
    std::string charName  = JsonGetStr(json, "character_name");
    std::string recipient = JsonGetStr(json, "recipient_name");
    std::string msg       = JsonGetStr(json, "message");

    if (charName.empty() || msg.empty())
        return;

    PlayerCacheData const* cache = sObjectMgr.GetPlayerDataByName(charName);
    ObjectGuid senderGuid = cache ? ObjectGuid(HIGHGUID_PLAYER, cache->uiGuid) : ObjectGuid();

    // For web-only senders: append " [W]" to the message as a suffix indicator
    bool isInGame = !charName.empty() && (ObjectAccessor::FindMasterPlayer(charName.c_str()) != nullptr);
    std::string dispMsg = isInGame ? msg : "\xe2\x93\x94 " + msg; // ⓔ

    if (channel == "world")
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, dispMsg.c_str(),
            LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, charName.c_str(),
            ObjectGuid(), nullptr, "世界频道");

        Channel* chanA = nullptr;
        Channel* chanB = nullptr;
        if (ChannelMgr* cMgr = channelMgr(ALLIANCE))
            chanA = cMgr->GetChannel("世界频道", PlayerPointer(), false);
        if (ChannelMgr* cMgr = channelMgr(HORDE))
            chanB = cMgr->GetChannel("世界频道", PlayerPointer(), false);
        if (chanA) chanA->SendToAll(&data);
        if (chanB && chanB != chanA) chanB->SendToAll(&data);
    }
    else if (channel == "whisper" && !recipient.empty())
    {
        if (MasterPlayer* rcp = ObjectAccessor::FindMasterPlayer(recipient.c_str()))
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, dispMsg.c_str(),
                LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, charName.c_str());
            rcp->GetSession()->SendPacket(&data);
        }
    }
    else if (channel == "guild" || channel == "party" || channel == "raid")
    {
        Player* sender = !senderGuid.IsEmpty() ? ObjectAccessor::FindPlayer(senderGuid) : nullptr;

        if (channel == "guild")
        {
            if (!sender) return;
            if (Guild* guild = sGuildMgr.GetGuildById(sender->GetGuildId()))
                guild->BroadcastToGuild(sender->GetSession(), dispMsg.c_str(), LANG_UNIVERSAL);
        }
        else
        {
            // Find the group: prefer the sender's own Group* if online,
            // otherwise scan online players to find one who shares the group.
            Group* group = sender ? sender->GetGroup() : nullptr;

            if (!group && !senderGuid.IsEmpty())
            {
                for (auto const& kv : sWorld.GetAllSessions())
                {
                    if (WorldSession* sess = kv.second)
                    {
                        if (Player* p = sess->GetPlayer())
                        {
                            if (p->IsInWorld())
                            {
                                if (Group* g = p->GetGroup())
                                {
                                    if (g->IsMember(senderGuid))
                                    {
                                        group = g;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!group) return;
            WorldPacket data;
            ChatMsg type = (channel == "raid") ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
            ChatHandler::BuildChatPacket(data, type, dispMsg.c_str(),
                LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, charName.c_str());
            group->BroadcastPacket(&data, false);
        }
    }
}

// ── broadcast (server-wide system messages) ───────────────────────────────────

void WebChatMgr::WriteBroadcast(std::string const& msg)
{
    if (!m_pubCtx || msg.empty()) return;
    std::string clean = StripColorCodes(msg);
    std::string j;
    j.reserve(256);
    j += "{\"source\":\"system\",\"channel\":\"broadcast\",\"character_name\":\"\",\"faction\":0,\"class\":0,\"recipient_name\":\"\",\"message\":\"";
    j += EscapeJson(clean);
    j += "\",\"ts\":";
    j += std::to_string(uint32(time(nullptr)));
    j += '}';
    Publish(j);
}

// ── 蒹葭 AI companion ─────────────────────────────────────────────────────────

bool WebChatMgr::IsJianJiaName(std::string const& name) const
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "JianJia IsJianJiaName: name='%s'(%zu) vs stored='%s'(%zu)",
        name.c_str(), name.size(), m_jianJiaName.c_str(), m_jianJiaName.size());
    return !m_jianJiaName.empty() && name == m_jianJiaName;
}

void WebChatMgr::ForwardWhisperToJianJia(std::string const& senderName, std::string const& message)
{
    if (!m_pubCtx || senderName.empty()) return;
    std::string j = "{\"sender\":\"";
    j += EscapeJson(senderName);
    j += "\",\"bot_name\":\"";
    j += EscapeJson(m_jianJiaName);
    j += "\",\"message\":\"";
    j += EscapeJson(message);
    j += "\"}";
    redisReply* r = (redisReply*)redisCommand(m_pubCtx, "PUBLISH %s %b",
        m_keyJianJiaIn.c_str(), j.data(), j.size());
    if (r) freeReplyObject(r);
    else ReconnectPub();
}

void WebChatMgr::WhisperAsJianJia(std::string const& targetName, std::string const& message)
{
    if (targetName.empty() || message.empty()) return;

    MasterPlayer* target = ObjectAccessor::FindMasterPlayer(targetName.c_str());
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "JianJia DEBUG: WhisperAsJianJia target='%s' found=%s",
        targetName.c_str(), target ? "yes" : "no");
    if (!target) return;

    // Get 蒹葭's guid so the client can click-to-reply
    PlayerCacheData const* cache = sObjectMgr.GetPlayerDataByName(m_jianJiaName.c_str());
    ObjectGuid senderGuid = cache ? ObjectGuid(HIGHGUID_PLAYER, cache->uiGuid) : ObjectGuid();

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, message.c_str(),
        LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, m_jianJiaName.c_str());
    target->GetSession()->SendPacket(&data);
}

void WebChatMgr::UpdateJianJia()
{
    if (m_jianJiaPending.empty()) return;

    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        local.swap(m_jianJiaPending);
    }
    while (!local.empty())
    {
        std::string const& json = local.front();
        std::string target  = JsonGetStr(json, "target");
        std::string message = JsonGetStr(json, "message");
        if (!target.empty() && !message.empty())
            WhisperAsJianJia(target, message);
        local.pop();
    }
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

std::string WebChatMgr::EscapeJson(std::string const& s)
{
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
    {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c < 0x20)  r += ' ';
        else                r += char(c);
    }
    return r;
}

std::string WebChatMgr::BuildJson(std::string const& source, std::string const& channel,
    std::string const& charName, uint32 faction, uint32 classId, std::string const& recipient, std::string const& msg)
{
    std::string j;
    j.reserve(300);
    j += "{\"source\":\"";           j += EscapeJson(source);
    j += "\",\"character_name\":\""; j += EscapeJson(charName);
    j += "\",\"faction\":";          j += std::to_string(faction);
    j += ",\"class\":";              j += std::to_string(classId);
    j += ",\"channel\":\"";          j += EscapeJson(channel);
    j += "\",\"recipient_name\":\""; j += EscapeJson(recipient);
    j += "\",\"message\":\"";        j += EscapeJson(msg);
    j += "\",\"ts\":";               j += std::to_string(uint32(time(nullptr)));
    j += '}';
    return j;
}

std::string WebChatMgr::JsonGetStr(std::string const& json, char const* key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();

    std::string r;
    for (size_t i = pos; i < json.size(); ++i)
    {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size())
        {
            char next = json[++i];
            if (next == '"') r += '"';
            else if (next == '\\') r += '\\';
            else { r += '\\'; r += next; }
        }
        else if (c == '"') break;
        else r += c;
    }
    return r;
}

std::string WebChatMgr::StripColorCodes(std::string const& s)
{
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (s[i] == '|' && i + 1 < s.size())
        {
            char n = s[i + 1];
            if ((n == 'c' || n == 'C') && i + 9 < s.size()) { i += 10; continue; } // |cAARRGGBB
            if (n == 'r' || n == 'R')                        { i += 2;  continue; } // |r
        }
        r += s[i++];
    }
    return r;
}

uint32 WebChatMgr::JsonGetU32(std::string const& json, char const* key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    if (pos >= json.size()) return 0;
    try { return uint32(std::stoul(json.substr(pos))); }
    catch (...) { return 0; }
}
