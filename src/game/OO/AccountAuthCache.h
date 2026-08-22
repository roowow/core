#pragma once
#include "Common.h"
#include <string>
#include <ctime>
#include <mutex>

struct redisContext;

// Local, Unix-socket-only Redis cache in front of WorldSocket::_HandleAuthSession()'s account
// lookup query (see HPHA.md "拍定方案：mangosd 本地缓存 WorldSocket 鉴权查询"). That query runs
// against mangosd's own LoginDatabase connection (the same physical account/login database
// realmd talks to, not CharacterDatabase) - see _HandleAuthSession()'s query comment. Scope is
// deliberately narrow: only covers a player who already logged in successfully once (this class
// gets written to on that success) and is now reconnecting on the same sessionkey (network
// blip, zoning, etc.) while that LoginDatabase connection happens to be down. A brand-new realmd
// login during an outage still fails the same way it does today - realmd would hand mangosd a
// sessionkey this cache has never seen, so there's nothing to fall back to. Same
// "practically-free local read" design as InstanceDataCache (see that class's comment) - one
// local Redis, several consumers, never a hard dependency.
//
// Not the same shape as InstanceDataCache's opaque single-string blob: this is a small set of
// named, structured fields, so it uses a Redis Hash (HSET/HGETALL) instead of a plain string
// value. Deliberately its own small class rather than folded into InstanceDataCache or given a
// shared base - only two consumers of "local Redis cache" exist right now, not enough to be
// worth abstracting yet (DbWriteOutbox only became a shared class once there were 3 same-shaped
// needs - see HPHA.md).
class AccountAuthCache
{
public:
    static AccountAuthCache& instance();

    // socketPath empty => cache disabled, Get always misses, Set is a no-op.
    void Initialize(std::string const& socketPath, uint32 realmId);
    void Shutdown();

    // Mirrors exactly the fields _HandleAuthSession() actually reads out of its account query
    // result (the query itself also selects `v`/`s`, but nothing in that handler ever reads
    // them back out of the Field*, so they're not cached either - no point storing data that's
    // never consumed). Values here are the already-resolved/validated forms (e.g. `security`
    // is the post-null-coalesce AccountTypes, not the raw possibly-NULL `gmLevel` column), so a
    // cache hit can feed straight into the same post-lookup logic the live DB path uses,
    // without re-deriving anything.
    struct Entry
    {
        uint32       accountId = 0;
        AccountTypes security  = SEC_PLAYER;
        std::string  sessionKeyHex;
        std::string  lastIp;
        time_t       mutetime = 0;
        uint8        locale = 0;
        std::string  os;
        std::string  platform;
        uint32       accountFlags = 0;
        std::string  email;
        bool         verifiedEmail = false;
        bool         banned = false;
    };

    // Returns true and fills outEntry on a cache hit. Returns false on a miss OR if the cache
    // is disabled/unavailable; caller must fall back to today's existing failure path
    // (AUTH_UNKNOWN_ACCOUNT) in both cases, same as it already does when the live query itself
    // comes back empty.
    bool Get(std::string const& username, Entry& outEntry);

    // Fire-and-forget write-through, called right after a successful live DB lookup. No-op if
    // the cache is disabled/unavailable - failing to cache never fails the login that's already
    // succeeded.
    void Set(std::string const& username, Entry const& entry);

private:
    bool EnsureConnected();
    void Disconnect();

    std::mutex    m_mutex;
    std::string   m_socketPath;
    std::string   m_keyPrefix; // "account_auth:realm:<realmId>:", see multi-realm note in InstanceDataCache.h
    bool          m_enabled = false;
    redisContext* m_ctx = nullptr;
    time_t        m_lastFailTime = 0;

    static uint32 const RECONNECT_COOLDOWN_SEC = 30;
    // Matches the source query's own "DATEDIFF(NOW(), a.last_login) < 1" freshness window (see
    // HPHA.md) - a cache entry is exactly as stale-tolerant as a real DB row would be. Not
    // refreshed on a cache-hit Get() (only on a fresh Set() from a real DB read) - during an
    // extended outage a player who keeps reconnecting off the cache doesn't get their entry's
    // validity extended indefinitely past what the last real DB confirmation actually earned.
    static int const TTL_SEC = 24 * 3600;
};

#define sAccountAuthCache AccountAuthCache::instance()
