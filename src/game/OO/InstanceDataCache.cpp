#include "InstanceDataCache.h"
#include "Log.h"
#include <hiredis/hiredis.h>

// Defensive-only: every write path is meant to write through here explicitly (see
// Map::CreateInstanceData, InstanceData::SaveToDB, DungeonPersistentState::SaveToDB/
// DeleteRespawnTimesAndData, MapPersistentStateManager::DeleteInstanceFromDB). This TTL
// just bounds the damage if a save path is ever missed - a expired/missing key is just
// a cache miss, which always falls back to the DB, never serves wrong data.
static int const INSTANCE_CACHE_TTL_SEC = 6 * 3600;

InstanceDataCache& InstanceDataCache::instance()
{
    static InstanceDataCache s;
    return s;
}

void InstanceDataCache::Initialize(std::string const& socketPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_socketPath   = socketPath;
    m_enabled      = !socketPath.empty();
    m_lastFailTime = 0;

    if (!m_enabled)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "InstanceDataCache: no socket configured, disabled (instance loads will always hit the DB directly).");
        return;
    }

    if (EnsureConnected())
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "InstanceDataCache: connected to local redis-server at unix:%s.", m_socketPath.c_str());
    else
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                 "InstanceDataCache: redis-server not reachable at unix:%s, will retry lazily; falling back to direct DB access until then.",
                 m_socketPath.c_str());
}

void InstanceDataCache::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Disconnect();
}

bool InstanceDataCache::EnsureConnected()
{
    if (!m_enabled)
        return false;

    if (m_ctx)
        return true;

    // Don't hammer a dead/misconfigured socket with a connect syscall on every single
    // hot-path call - back off for a bit after a failure and just treat the cache as
    // unavailable (callers fall back to the DB) until the cooldown expires.
    if (m_lastFailTime != 0 && time(nullptr) - m_lastFailTime < time_t(RECONNECT_COOLDOWN_SEC))
        return false;

    // Local Unix socket - a working redis-server answers essentially instantly. Keep
    // both the connect and the per-command timeout short so a hung/unresponsive socket
    // can never stall the caller anywhere near as long as the DB round trip we're
    // trying to avoid in the first place.
    timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 50000; // 50ms

    m_ctx = redisConnectUnixWithTimeout(m_socketPath.c_str(), tv);
    if (!m_ctx || m_ctx->err)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
                 "InstanceDataCache: redis-server not reachable at unix:%s, falling back to direct DB access.",
                 m_socketPath.c_str());
        Disconnect();
        m_lastFailTime = time(nullptr);
        return false;
    }

    redisSetTimeout(m_ctx, tv);
    m_lastFailTime = 0;
    return true;
}

void InstanceDataCache::Disconnect()
{
    if (m_ctx)
    {
        redisFree(m_ctx);
        m_ctx = nullptr;
    }
}

bool InstanceDataCache::Get(std::string const& key, std::string& outData)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!EnsureConnected())
        return false;

    redisReply* reply = (redisReply*)redisCommand(m_ctx, "GET %s", key.c_str());
    if (!reply)
    {
        // Command failed on a connection that looked fine (e.g. socket died mid-session).
        // Drop it so the next call reconnects (subject to the cooldown) instead of
        // repeatedly handing commands to a broken context.
        Disconnect();
        m_lastFailTime = time(nullptr);
        return false;
    }

    bool const hit = (reply->type == REDIS_REPLY_STRING);
    if (hit)
        outData.assign(reply->str, reply->len);
    freeReplyObject(reply);
    return hit;
}

void InstanceDataCache::Set(std::string const& key, std::string const& data)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!EnsureConnected())
        return;

    redisReply* reply = (redisReply*)redisCommand(m_ctx, "SET %s %b EX %d",
        key.c_str(), data.data(), data.size(), INSTANCE_CACHE_TTL_SEC);
    if (!reply)
    {
        Disconnect();
        m_lastFailTime = time(nullptr);
        return;
    }
    freeReplyObject(reply);
}

void InstanceDataCache::Del(std::string const& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!EnsureConnected())
        return;

    redisReply* reply = (redisReply*)redisCommand(m_ctx, "DEL %s", key.c_str());
    if (!reply)
    {
        Disconnect();
        m_lastFailTime = time(nullptr);
        return;
    }
    freeReplyObject(reply);
}
