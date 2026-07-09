#include "WebChatMgr.h"
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

void WebChatMgr::Initialize(char const* socketPath)
{
    m_socketPath = socketPath;
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
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "WebChatMgr: connected to Redis, WebChat enabled.");
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
    uint32 faction, std::string const& recipient, std::string const& msg)
{
    if (!m_pubCtx) return;
    std::string json = BuildJson("game", channel, charName, faction, recipient, msg);
    Publish(json);
}

void WebChatMgr::Publish(std::string const& json)
{
    redisAppendCommand(m_pubCtx, "PUBLISH web_chat:live %b", json.data(), json.size());
    redisAppendCommand(m_pubCtx, "LPUSH web_chat:history %b", json.data(), json.size());
    redisAppendCommand(m_pubCtx, "LTRIM web_chat:history 0 99");

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
        sLog.outError("WebChatMgr: subscriber failed to connect to Redis");
        return;
    }

    redisReply* r = (redisReply*)redisCommand(m_subCtx, "SUBSCRIBE web_chat:live");
    if (!r) return;
    freeReplyObject(r);

    while (!m_stop.load(std::memory_order_relaxed))
    {
        redisReply* reply = nullptr;
        if (redisGetReply(m_subCtx, (void**)&reply) != REDIS_OK)
            break; // connection closed (Shutdown freed the context)

        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3
            && reply->element[2]->type == REDIS_REPLY_STRING)
        {
            std::string json(reply->element[2]->str, reply->element[2]->len);
            if (JsonGetStr(json, "source") == "web")
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_pending.push(std::move(json));
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

    if (channel == "world")
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, msg.c_str(),
            LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, charName.c_str(),
            ObjectGuid(), nullptr, "世界频道");

        const Team kTeams[] = {ALLIANCE, HORDE};
        for (Team team : kTeams)
            if (ChannelMgr* cMgr = channelMgr(team))
                if (Channel* chan = cMgr->GetChannel("世界频道", PlayerPointer(), false))
                    chan->SendToAll(&data);
    }
    else if (channel == "whisper" && !recipient.empty())
    {
        if (MasterPlayer* rcp = ObjectAccessor::FindMasterPlayer(recipient.c_str()))
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, msg.c_str(),
                LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, charName.c_str());
            rcp->GetSession()->SendPacket(&data);
        }
    }
    else if (channel == "guild" || channel == "party" || channel == "raid")
    {
        // group/guild objects are bound to an online player — skip if offline
        Player* sender = !senderGuid.IsEmpty() ? ObjectAccessor::FindPlayer(senderGuid) : nullptr;
        if (!sender) return;

        if (channel == "guild")
        {
            if (Guild* guild = sGuildMgr.GetGuildById(sender->GetGuildId()))
                guild->BroadcastToGuild(sender->GetSession(), msg.c_str(), LANG_UNIVERSAL);
        }
        else
        {
            Group* group = sender->GetGroup();
            if (!group) return;
            WorldPacket data;
            ChatMsg type = (channel == "raid") ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
            ChatHandler::BuildChatPacket(data, type, msg.c_str(),
                LANG_UNIVERSAL, CHAT_TAG_NONE, sender->GetObjectGuid(), sender->GetName());
            group->BroadcastPacket(&data, false);
        }
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
    std::string const& charName, uint32 faction, std::string const& recipient, std::string const& msg)
{
    std::string j;
    j.reserve(300);
    j += "{\"source\":\"";           j += EscapeJson(source);
    j += "\",\"character_name\":\""; j += EscapeJson(charName);
    j += "\",\"faction\":";          j += std::to_string(faction);
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
