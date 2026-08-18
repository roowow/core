#pragma once
#include "Common.h"
#include <string>
#include <ctime>
#include <mutex>

struct redisContext;

// Local, Unix-socket-only Redis cache in front of the `instance`/`world` tables'
// `data` column (serialized instance-script state: boss kill flags, etc.).
//
// This is deliberately NOT the same Redis as WebChatMgr's (that one is configured
// from mangosd.conf and may legitimately live on a separate machine, reached over
// TCP, for the game<->web chat bridge - see WebChat.md). The whole point of this
// cache is to recreate the "practically free" read latency instance loading had
// back when CharacterDatabase itself was on the LAN; a Redis instance a network
// hop away buys nothing. So this class only ever connects via a local Unix socket
// path and refuses to be configured any other way.
//
// If the socket isn't configured, or the redis-server behind it isn't reachable,
// every call here is a safe no-op / cache-miss - callers always have (and use) the
// existing direct-DB-read/write path as a fallback. This is a pure accelerator,
// never a hard dependency.
//
// Thread-safety: MapManager runs map updates AND new-instance creation
// (CreateNewInstancesForPlayers) across several worker thread pools (m_threads,
// m_continentThreads, m_instanceCreationThreads - see MapManager::Update()), so
// Map::CreateInstanceData()/InstanceData::SaveToDB() for different maps can call
// into this singleton concurrently from different threads. All access to m_ctx and
// the reconnect-cooldown state is serialized through m_mutex accordingly.
class InstanceDataCache
{
public:
    static InstanceDataCache& instance();

    // socketPath empty => cache disabled, Get always misses, Set/Del are no-ops.
    void Initialize(std::string const& socketPath);
    void Shutdown();

    // Returns true and fills outData on a cache hit (outData may legitimately be
    // empty - that's a cached "no saved state yet", distinct from a miss). Returns
    // false on a miss OR if the cache is disabled/unavailable; callers must fall
    // back to the DB in both cases.
    bool Get(std::string const& key, std::string& outData);

    // Fire-and-forget write-through. No-op if the cache is disabled/unavailable.
    void Set(std::string const& key, std::string const& data);
    void Del(std::string const& key);

private:
    // (Re)connects if not already connected, subject to a cooldown after a recent
    // failure so a dead/misconfigured socket doesn't retry a syscall on every single
    // hot-path call. Returns false (and leaves m_ctx null) if unavailable right now.
    // Caller must hold m_mutex.
    bool EnsureConnected();
    void Disconnect();

    std::mutex    m_mutex;      // guards everything below, see thread-safety note above
    std::string   m_socketPath;
    bool          m_enabled = false;
    redisContext* m_ctx = nullptr;
    time_t        m_lastFailTime = 0;

    static uint32 const RECONNECT_COOLDOWN_SEC = 30;
};

#define sInstanceDataCache InstanceDataCache::instance()

// Shared key format so Map.cpp (read side) and InstanceData.cpp/MapPersistentStateMgr.cpp
// (write side) can never drift apart on how a given instance/world row is keyed.
inline std::string MakeInstanceCacheKey(bool instanceable, uint32 id)
{
    return (instanceable ? "instdata:" : "worlddata:") + std::to_string(id);
}
