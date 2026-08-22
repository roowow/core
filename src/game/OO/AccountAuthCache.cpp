#include "AccountAuthCache.h"
#include "Log.h"
#include <hiredis/hiredis.h>
#include <cstdlib>
#include <vector>

AccountAuthCache& AccountAuthCache::instance()
{
    static AccountAuthCache s;
    return s;
}

void AccountAuthCache::Initialize(std::string const& socketPath, uint32 realmId)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_socketPath   = socketPath;
    m_keyPrefix    = "account_auth:realm:" + std::to_string(realmId) + ":";
    m_enabled      = !socketPath.empty();
    m_lastFailTime = 0;

    if (!m_enabled)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "AccountAuthCache: no socket configured, disabled (WorldSocket auth always hits CharacterDatabase directly).");
        return;
    }

    if (EnsureConnected())
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "AccountAuthCache: connected to local redis-server at unix:%s.", m_socketPath.c_str());
    else
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "AccountAuthCache: redis-server not reachable at unix:%s, will retry lazily; falling back to direct DB access until then.",
                 m_socketPath.c_str());
}

void AccountAuthCache::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Disconnect();
}

bool AccountAuthCache::EnsureConnected()
{
    if (!m_enabled)
        return false;

    if (m_ctx)
        return true;

    if (m_lastFailTime != 0 && time(nullptr) - m_lastFailTime < time_t(RECONNECT_COOLDOWN_SEC))
        return false;

    timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 50000; // 50ms - local Unix socket, should be near-instant

    m_ctx = redisConnectUnixWithTimeout(m_socketPath.c_str(), tv);
    if (!m_ctx || m_ctx->err)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
                 "AccountAuthCache: redis-server not reachable at unix:%s, falling back to direct DB access.",
                 m_socketPath.c_str());
        Disconnect();
        m_lastFailTime = time(nullptr);
        return false;
    }

    redisSetTimeout(m_ctx, tv);
    m_lastFailTime = 0;
    return true;
}

void AccountAuthCache::Disconnect()
{
    if (m_ctx)
    {
        redisFree(m_ctx);
        m_ctx = nullptr;
    }
}

bool AccountAuthCache::Get(std::string const& username, Entry& outEntry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!EnsureConnected())
        return false;

    std::string const key = m_keyPrefix + username;
    redisReply* reply = (redisReply*)redisCommand(m_ctx, "HGETALL %s", key.c_str());
    if (!reply)
    {
        Disconnect();
        m_lastFailTime = time(nullptr);
        return false;
    }

    // HGETALL on a missing key replies with an empty array, not an error - that's a clean miss,
    // not a connection problem.
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2)
    {
        freeReplyObject(reply);
        return false;
    }

    Entry parsed;
    bool haveAccountId = false;
    for (size_t i = 0; i + 1 < reply->elements; i += 2)
    {
        redisReply* fname = reply->element[i];
        redisReply* fval   = reply->element[i + 1];
        if (!fname || fname->type != REDIS_REPLY_STRING || !fval || fval->type != REDIS_REPLY_STRING)
            continue;

        std::string const name(fname->str, fname->len);
        std::string const val(fval->str, fval->len);

        if (name == "accountId")
        {
            parsed.accountId = uint32(strtoul(val.c_str(), nullptr, 10));
            haveAccountId = true;
        }
        else if (name == "security")
            parsed.security = AccountTypes(strtoul(val.c_str(), nullptr, 10));
        else if (name == "sessionKeyHex")
            parsed.sessionKeyHex = val;
        else if (name == "lastIp")
            parsed.lastIp = val;
        else if (name == "mutetime")
            parsed.mutetime = time_t(strtoull(val.c_str(), nullptr, 10));
        else if (name == "locale")
            parsed.locale = uint8(strtoul(val.c_str(), nullptr, 10));
        else if (name == "os")
            parsed.os = val;
        else if (name == "platform")
            parsed.platform = val;
        else if (name == "accountFlags")
            parsed.accountFlags = uint32(strtoul(val.c_str(), nullptr, 10));
        else if (name == "email")
            parsed.email = val;
        else if (name == "verifiedEmail")
            parsed.verifiedEmail = (val == "1");
        else if (name == "banned")
            parsed.banned = (val == "1");
    }
    freeReplyObject(reply);

    if (!haveAccountId) // malformed/incomplete entry - shouldn't happen, we control the producer
        return false;

    outEntry = parsed;
    return true;
}

void AccountAuthCache::Set(std::string const& username, Entry const& entry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!EnsureConnected())
        return;

    std::string const key = m_keyPrefix + username;

    std::vector<std::string> args;
    args.reserve(2 + 12 * 2);
    args.push_back("HSET");
    args.push_back(key);
    args.push_back("accountId");     args.push_back(std::to_string(entry.accountId));
    args.push_back("security");      args.push_back(std::to_string(uint32(entry.security)));
    args.push_back("sessionKeyHex"); args.push_back(entry.sessionKeyHex);
    args.push_back("lastIp");        args.push_back(entry.lastIp);
    args.push_back("mutetime");      args.push_back(std::to_string(uint64(entry.mutetime)));
    args.push_back("locale");        args.push_back(std::to_string(uint32(entry.locale)));
    args.push_back("os");            args.push_back(entry.os);
    args.push_back("platform");      args.push_back(entry.platform);
    args.push_back("accountFlags");  args.push_back(std::to_string(entry.accountFlags));
    args.push_back("email");         args.push_back(entry.email);
    args.push_back("verifiedEmail"); args.push_back(entry.verifiedEmail ? "1" : "0");
    args.push_back("banned");        args.push_back(entry.banned ? "1" : "0");

    std::vector<char const*> argv;
    std::vector<size_t> argvLen;
    argv.reserve(args.size());
    argvLen.reserve(args.size());
    for (std::string const& a : args)
    {
        argv.push_back(a.data());
        argvLen.push_back(a.size());
    }

    redisReply* reply = (redisReply*)redisCommandArgv(m_ctx, int(argv.size()), argv.data(), argvLen.data());
    if (!reply)
    {
        Disconnect();
        m_lastFailTime = time(nullptr);
        return;
    }
    freeReplyObject(reply);

    // Separate EXPIRE, not SET...EX (that's not a thing for hashes). Not atomic with the HSET
    // above (a mid-command disconnect could leave the key without a TTL) - harmless if it
    // happens: worst case a stale entry outlives its intended 24h a little, and it's still only
    // ever consulted as a query-failure fallback, never trusted over a live DB read.
    redisReply* expireReply = (redisReply*)redisCommand(m_ctx, "EXPIRE %s %d", key.c_str(), TTL_SEC);
    if (!expireReply)
    {
        Disconnect();
        m_lastFailTime = time(nullptr);
        return;
    }
    freeReplyObject(expireReply);
}
