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
#include <chrono>
#include <csignal>
#include <ctime>
#include <pthread.h>
#include <sys/socket.h>

// Blocks SIGPIPE on the calling thread for the duration of its lifetime.
// Needed because hiredis 0.14 uses write() without MSG_NOSIGNAL, and threads
// created before WebChatMgr::Initialize() (e.g. AsyncPacket) don't inherit
// the SIG_BLOCK set there.  SIGPIPE is harmless (SIG_IGN disposition) but
// GDB/ptrace intercepts it before the disposition check and triggers anticrash
// full-thread dumps.  A blocked signal is never delivered via ptrace at all.
struct SigpipeGuard
{
    sigset_t prev;
    SigpipeGuard()
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, &prev);
    }
    ~SigpipeGuard()
    {
        // If restoring prev would unblock SIGPIPE, drain any pending SIGPIPE first.
        // Without this, a SIGPIPE that became pending while blocked is delivered the
        // instant SIG_SETMASK makes it unblocked again — causing a crash.
        if (!sigismember(&prev, SIGPIPE))
        {
            sigset_t pending;
            sigpending(&pending);
            if (sigismember(&pending, SIGPIPE))
            {
                struct timespec ts = {};
                sigset_t set;
                sigemptyset(&set);
                sigaddset(&set, SIGPIPE);
                sigtimedwait(&set, nullptr, &ts);
            }
        }
        pthread_sigmask(SIG_SETMASK, &prev, nullptr);
    }
};

WebChatMgr& WebChatMgr::instance()
{
    static WebChatMgr s;
    return s;
}

void WebChatMgr::Initialize(char const* socketPath, uint32 realmId)
{
    // hiredis 0.14 uses write() without MSG_NOSIGNAL; a broken socket would normally
    // send SIGPIPE.  We want two layers of protection:
    //   1. signal(SIG_IGN) — process-wide disposition fallback
    //   2. pthread_sigmask(SIG_BLOCK) — thread-level block; crucially, new threads
    //      inherit the calling thread's signal mask, so AsyncPacket and any other
    //      threads spawned after Initialize() also have SIGPIPE blocked.
    //      Blocked signals are never delivered via GDB ptrace, preventing crash dumps.
    signal(SIGPIPE, SIG_IGN);
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }

    m_socketPath = socketPath;
    m_keyLive    = "web_chat:live:"    + std::to_string(realmId);
    m_keyHistory = "web_chat:history:" + std::to_string(realmId);
    m_keyJianJiaIn  = "web_chat:jianjia_in:"  + std::to_string(realmId);
    m_keyJianJiaOut = "web_chat:jianjia_out:" + std::to_string(realmId);
    m_keyWebOnline  = "web_chat:web_online:"  + std::to_string(realmId) + ":";
    m_realmId       = realmId;
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
    // Interrupt the subscribe thread's blocking redisGetReply by shutting down
    // its socket fd.  We must NOT call redisFree from here — that would race
    // with the subscribe thread still using the context.  ::shutdown(fd) is
    // safe from another thread and causes redisGetReply to return REDIS_ERR.
    //
    // Loop up to 6 s: the thread may be in its reconnect sleep (m_subFd=-1)
    // when we first check.  If m_subFd stays -1 the whole time, the thread
    // will wake from its sleep, see m_stop=true, and exit on its own.
    for (int w = 0; w < 60; ++w)
    {
        int fd = m_subFd.load(std::memory_order_acquire);
        if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (m_subThread.joinable()) m_subThread.join();
    {
        std::lock_guard<std::mutex> lock(m_pubMutex);
        if (m_pubCtx) { redisFree(m_pubCtx); m_pubCtx = nullptr; }
    }
}

void WebChatMgr::Update()
{
    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_pending.empty()) return;
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
    uint32 faction, uint32 classId, std::string const& recipient, std::string const& msg,
    uint32 contextId)
{
    if (!m_pubCtx) return;
    std::string json = BuildJson("game", channel, charName, faction, classId, recipient, msg, contextId);
    Publish(json);
    // For party/raid: update char→group mapping so PHP can filter by group membership
    if (contextId > 0 && (channel == "party" || channel == "raid" || channel == "raid_leader"))
    {
        SigpipeGuard guard;
        std::lock_guard<std::mutex> lock(m_pubMutex);
        if (m_pubCtx)
        {
            std::string key = "web_chat:char_group:" + std::to_string(m_realmId) + ":" + charName;
            redisReply* r = (redisReply*)redisCommand(m_pubCtx, "SETEX %s 3600 %u", key.c_str(), contextId);
            if (r) freeReplyObject(r);
            else ReconnectPub();
        }
    }
}

void WebChatMgr::Publish(std::string const& json)
{
    SigpipeGuard guard;
    std::lock_guard<std::mutex> lock(m_pubMutex);
    // Attempt up to 2 times: first with the existing connection, then once after reconnect.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (!m_pubCtx)
        {
            ReconnectPub();
            if (!m_pubCtx) return;
        }

        redisAppendCommand(m_pubCtx, "PUBLISH %s %b", m_keyLive.c_str(), json.data(), json.size());
        redisAppendCommand(m_pubCtx, "LPUSH %s %b", m_keyHistory.c_str(), json.data(), json.size());
        redisAppendCommand(m_pubCtx, "LTRIM %s 0 99", m_keyHistory.c_str());

        bool ok = true;
        for (int i = 0; i < 3; ++i)
        {
            redisReply* r = nullptr;
            if (redisGetReply(m_pubCtx, (void**)&r) != REDIS_OK)
            {
                ReconnectPub();
                ok = false;
                break;
            }
            freeReplyObject(r);
        }
        if (ok) return;
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
    while (!m_stop.load(std::memory_order_relaxed))
    {
        redisContext* ctx = redisConnectUnix(m_socketPath.c_str());
        if (!ctx || ctx->err)
        {
            if (ctx) { redisFree(ctx); }
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "WebChatMgr: subscriber failed to connect, retrying in 5s");
            for (int w = 0; w < 50 && !m_stop.load(std::memory_order_relaxed); ++w)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        m_subCtx = ctx;
        // Publish fd before potentially blocking so Shutdown() can ::shutdown() it.
        m_subFd.store(ctx->fd, std::memory_order_release);

        // Re-check m_stop: Shutdown() may have set m_stop=true and finished its
        // m_subFd poll loop while we were still connecting (m_subFd was -1 then).
        // Without this check we would block in redisGetReply with no one to wake us.
        if (m_stop.load(std::memory_order_acquire))
        {
            m_subFd.store(-1, std::memory_order_release);
            redisFree(ctx);
            m_subCtx = nullptr;
            break;
        }

        {
            redisReply* r = (redisReply*)redisCommand(ctx, "SUBSCRIBE %s %s",
                m_keyLive.c_str(), m_keyJianJiaOut.c_str());
            if (!r)
            {
                m_subFd.store(-1, std::memory_order_release);
                redisFree(ctx); m_subCtx = nullptr;
                continue;
            }
            freeReplyObject(r);
            // SUBSCRIBE to 2 channels produces 2 confirmation replies; consume the extra one
            redisReply* rx = nullptr;
            if (redisGetReply(ctx, (void**)&rx) == REDIS_OK && rx)
                freeReplyObject(rx);
        }

        while (!m_stop.load(std::memory_order_relaxed))
        {
            redisReply* reply = nullptr;
            if (redisGetReply(ctx, (void**)&reply) != REDIS_OK)
                break; // Shutdown() called ::shutdown(fd), or Redis dropped us

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
                    m_jianJiaPending.push(std::move(json));
            }
            freeReplyObject(reply);
        }

        // Clear m_subFd before redisFree so Shutdown() can't call ::shutdown()
        // on a fd that we're about to close (would be harmless but confusing).
        m_subFd.store(-1, std::memory_order_release);
        redisFree(ctx);
        m_subCtx = nullptr;
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
    bool isInGame = !senderGuid.IsEmpty() && (
        ObjectAccessor::FindMasterPlayer(charName.c_str()) != nullptr ||
        ObjectAccessor::FindPlayer(senderGuid) != nullptr);
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
            // GetPlayerGuild works offline — GuildMgr keeps an in-memory guid→guildId map
            if (!cache) return;
            if (Guild* guild = sGuildMgr.GetPlayerGuild(cache->uiGuid))
            {
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_GUILD, dispMsg.c_str(),
                    LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, charName.c_str());
                auto sendFn = [&data](Player* p) { p->GetSession()->SendPacket(&data); };
                guild->BroadcastWorker(sendFn);
            }
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

// ── web presence ─────────────────────────────────────────────────────────────

bool WebChatMgr::IsWebOnline(std::string const& charName) const
{
    if (!m_pubCtx || charName.empty()) return false; // optimistic fast-path
    SigpipeGuard guard;
    std::lock_guard<std::mutex> lock(m_pubMutex);
    if (!m_pubCtx) return false; // re-check under lock
    std::string key = m_keyWebOnline + charName;
    redisReply* r = (redisReply*)redisCommand(m_pubCtx, "EXISTS %s", key.c_str());
    bool exists = r && r->type == REDIS_REPLY_INTEGER && r->integer > 0;
    if (r) freeReplyObject(r);
    else const_cast<WebChatMgr*>(this)->ReconnectPub();
    return exists;
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
    return !m_jianJiaName.empty() && name == m_jianJiaName;
}

void WebChatMgr::ForwardWhisperToJianJia(std::string const& senderName, std::string const& message,
    uint8 level, uint8 cls, uint8 race, std::string const& zone)
{
    if (!m_pubCtx || senderName.empty()) return;
    SigpipeGuard guard;
    std::lock_guard<std::mutex> lock(m_pubMutex);
    if (!m_pubCtx) return;
    std::string j = "{\"sender\":\"";
    j += EscapeJson(senderName);
    j += "\",\"bot_name\":\"";
    j += EscapeJson(m_jianJiaName);
    j += "\",\"level\":";
    j += std::to_string(level);
    j += ",\"class\":";
    j += std::to_string(cls);
    j += ",\"race\":";
    j += std::to_string(race);
    j += ",\"zone\":\"";
    j += EscapeJson(zone);
    j += "\",\"message\":\"";
    j += EscapeJson(message);
    j += "\"}";
    redisReply* r = (redisReply*)redisCommand(m_pubCtx, "PUBLISH %s %b",
        m_keyJianJiaIn.c_str(), j.data(), j.size());
    if (r) freeReplyObject(r);
    else ReconnectPub();
}

void WebChatMgr::ForwardGroupChatToJianJia(std::string const& senderName, std::string const& message,
    char const* chatContext, uint32 groupId)
{
    if (!m_pubCtx || senderName.empty() || !chatContext) return;
    SigpipeGuard guard;
    std::lock_guard<std::mutex> lock(m_pubMutex);
    if (!m_pubCtx) return;
    std::string j = "{\"event\":\"group_chat\",\"sender\":\"";
    j += EscapeJson(senderName);
    j += "\",\"bot_name\":\"";
    j += EscapeJson(m_jianJiaName);
    j += "\",\"context\":\"";
    j += EscapeJson(chatContext);
    j += "\",\"group_id\":";
    j += std::to_string(groupId);
    j += ",\"message\":\"";
    j += EscapeJson(message);
    j += "\"}";
    redisReply* r = (redisReply*)redisCommand(m_pubCtx, "PUBLISH %s %b",
        m_keyJianJiaIn.c_str(), j.data(), j.size());
    if (r) freeReplyObject(r);
    else ReconnectPub();
}

bool WebChatMgr::NotifyBgAfkViaJianJia(Player* player, BattleGround* /*bg*/, uint8 stage, uint8 afkLevel,
    char const* noticeType)
{
    if (!m_pubCtx || m_jianJiaName.empty() || !player) return false;
    SigpipeGuard guard;
    std::lock_guard<std::mutex> lock(m_pubMutex);
    if (!m_pubCtx) return false;
    std::string zoneName;
    if (AreaEntry const* zoneEntry = AreaEntry::GetById(player->GetZoneId()))
        zoneName = zoneEntry->Name;
    std::string j = "{\"event\":\"bg_afk\",\"sender\":\"";
    j += EscapeJson(player->GetName());
    j += "\",\"bot_name\":\"";
    j += EscapeJson(m_jianJiaName);
    j += "\",\"stage\":";
    j += std::to_string(stage);
    j += ",\"afk_level\":";
    j += std::to_string(afkLevel);
    j += ",\"notice\":\"";
    j += EscapeJson(noticeType ? noticeType : "warning");
    j += "\",\"level\":";
    j += std::to_string(player->GetLevel());
    j += ",\"class\":";
    j += std::to_string(player->GetClass());
    j += ",\"race\":";
    j += std::to_string(player->GetRace());
    j += ",\"zone\":\"";
    j += EscapeJson(zoneName);
    j += "\"}";
    redisReply* r = (redisReply*)redisCommand(m_pubCtx, "PUBLISH %s %b",
        m_keyJianJiaIn.c_str(), j.data(), j.size());
    if (r) { freeReplyObject(r); return true; }
    ReconnectPub();
    return false;
}

void WebChatMgr::SpeakAsJianJia(std::string const& targetName, std::string const& message, ChatMsg groupType)
{
    if (targetName.empty() || message.empty()) return;

    PlayerCacheData const* cache = sObjectMgr.GetPlayerDataByName(m_jianJiaName.c_str());
    ObjectGuid senderGuid = cache ? ObjectGuid(HIGHGUID_PLAYER, cache->uiGuid) : ObjectGuid();

    if (groupType != CHAT_MSG_WHISPER && senderGuid)
    {
        if (Player* targetPlayer = ObjectAccessor::FindPlayerByName(targetName.c_str()))
        {
            if (Group* group = targetPlayer->GetGroup())
            {
                if (group->IsMember(senderGuid))
                {
                    WorldPacket chatData;
                    ChatHandler::BuildChatPacket(chatData, groupType, message.c_str(),
                        LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, m_jianJiaName.c_str());
                    group->BroadcastPacket(&chatData, false);
                    return;
                }
            }
        }
    }

    // Fallback: whisper
    MasterPlayer* target = ObjectAccessor::FindMasterPlayer(targetName.c_str());
    if (!target) return;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, message.c_str(),
        LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, m_jianJiaName.c_str());
    target->GetSession()->SendPacket(&data);
}

void WebChatMgr::SpeakInBgAsJianJia(std::string const& targetName, std::string const& message)
{
    if (targetName.empty() || message.empty()) return;

    Player* targetPlayer = ObjectAccessor::FindPlayerByName(targetName.c_str());
    if (!targetPlayer)
    {
        SpeakAsJianJia(targetName, message);
        return;
    }

    BattleGround* bg = targetPlayer->GetBattleGround();
    if (!bg)
    {
        SpeakAsJianJia(targetName, message); // fallback to whisper
        return;
    }

    PlayerCacheData const* cache = sObjectMgr.GetPlayerDataByName(m_jianJiaName.c_str());
    ObjectGuid senderGuid = cache ? ObjectGuid(HIGHGUID_PLAYER, cache->uiGuid) : ObjectGuid();
    WorldPacket chatData;
    ChatHandler::BuildChatPacket(chatData, CHAT_MSG_BATTLEGROUND, message.c_str(),
        LANG_UNIVERSAL, CHAT_TAG_NONE, senderGuid, m_jianJiaName.c_str());
    bg->SendPacketToAll(&chatData);
}

void WebChatMgr::UpdateJianJia()
{
    std::queue<std::string> local;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_jianJiaPending.empty()) return;
        local.swap(m_jianJiaPending);
    }
    while (!local.empty())
    {
        std::string const& json = local.front();
        std::string target  = JsonGetStr(json, "target");
        std::string message = JsonGetStr(json, "message");
        std::string channel = JsonGetStr(json, "channel");
        if (!target.empty())
        {
            if (channel == "fallback")
            {
                uint32 stage    = JsonGetU32(json, "stage");
                uint32 afkLevel = JsonGetU32(json, "afk_level");
                std::string notice = JsonGetStr(json, "notice");
                if (Player* p = ObjectAccessor::FindPlayerByName(target.c_str()))
                    BattleGroundAfkMgr::SendFallbackNotice(p, uint8(stage), uint8(afkLevel), notice.c_str());
            }
            else if (!message.empty())
            {
                if (channel == "bg")
                    SpeakInBgAsJianJia(target, message);
                else if (channel == "raid" || channel == "group")
                    SpeakAsJianJia(target, message, CHAT_MSG_RAID);
                else if (channel == "party")
                    SpeakAsJianJia(target, message, CHAT_MSG_PARTY);
                else
                    SpeakAsJianJia(target, message);
            }
        }
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
    std::string const& charName, uint32 faction, uint32 classId, std::string const& recipient,
    std::string const& msg, uint32 contextId)
{
    std::string j;
    j.reserve(320);
    j += "{\"source\":\"";           j += EscapeJson(source);
    j += "\",\"character_name\":\""; j += EscapeJson(charName);
    j += "\",\"faction\":";          j += std::to_string(faction);
    j += ",\"class\":";              j += std::to_string(classId);
    j += ",\"channel\":\"";          j += EscapeJson(channel);
    j += "\",\"recipient_name\":\""; j += EscapeJson(recipient);
    j += "\",\"message\":\"";        j += EscapeJson(msg);
    j += "\",\"ts\":";               j += std::to_string(uint32(time(nullptr)));
    if (contextId > 0) { j += ",\"context_id\":"; j += std::to_string(contextId); }
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
