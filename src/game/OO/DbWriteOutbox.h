#pragma once
#include "Common.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>
#include <memory>

struct redisContext;
class Database;
class MySQLConnection;

// Generic, reusable local-Redis-backed durable write queue in front of a target Database.
// See HPHA.md "彻底解耦：数据库 Redis 化" for the full design rationale - short version:
//
// Database::PExecute()/Execute() already queue writes onto an in-process, in-memory delay
// queue (SqlDelayThread) that's non-blocking - but if the write fails (e.g. MariaDB
// unreachable), it's just silently dropped, no retry, no persistence. That's fine for a
// transient network hiccup, but not for "MariaDB is down for hours" - anything queued during
// that window would be lost forever.
//
// DbWriteOutbox fixes that for a specific target Database: Enqueue() durably appends the SQL
// to a local Redis Stream (survives a mangosd crash/restart - the write is safe as soon as
// Enqueue() returns), and an independent background Flusher thread consumes the stream and
// applies each entry to the target Database, retrying indefinitely (with backoff) until it
// succeeds - so a multi-hour MariaDB outage just makes the stream grow, never lose data.
//
// If the local Redis is unavailable or unconfigured, Enqueue() falls straight back to
// targetDb.PExecute(sql) - today's exact behavior, zero regression, this is a pure upgrade
// layered on top of the existing write path, never a hard new dependency.
//
// v1 scope: every Enqueue()'d string must be a single, independent, already-fully-formatted
// SQL statement (no multi-statement atomic transactions). This is enough for every current
// consumer (Phase1's InstanceStatistics.cpp writes, rewritten as single `INSERT ... ON
// DUPLICATE KEY UPDATE` statements instead of DELETE+INSERT pairs) and deliberately avoids
// touching Database::BeginTransaction()/m_currentTransaction, which is a single non-thread-
// local member on the Database object - not safe for this class's independently-threaded
// Flusher to share with whatever else (main thread, other Enqueue() callers) might also be
// using transactions on the same Database object. See HPHA.md Phase1 notes.
class DbWriteOutbox
{
public:
    DbWriteOutbox();
    ~DbWriteOutbox();

    // redisSocketPath empty => disabled entirely, Enqueue() always goes straight to
    // targetDb.PExecute(sql) (no Flusher thread is even started).
    // dbConnectionInfo is the same "host;port;user;pass;db" string targetDb itself was
    // initialized with (see Master.cpp's StartDB(), reads "<Name>Database.Info" from
    // mangosd.conf) - needed because the Flusher must own a completely independent MySQL
    // connection, not share targetDb's.
    // streamKey namespaces this instance's Redis Stream/consumer group (e.g. "outbox:logs");
    // callers migrating a different Database later (Phase2) just pick a different key.
    void Initialize(std::string const& redisSocketPath, std::string const& streamKey,
                     Database& targetDb, std::string const& dbConnectionInfo);
    void Shutdown();

    // Durably queues sql for eventual execution against targetDb. See class comment for the
    // single-statement requirement and the disabled/unavailable fallback behavior.
    void Enqueue(std::string const& sql);

    // Snapshot for GM/diagnostic reporting (see .server dboutbox). streamLength/pendingCount
    // are live XLEN/XPENDING queries against Redis (borrows the Enqueue-side connection under
    // m_redisMutex) - both are -1 if the cache/connection is disabled or unreachable right now,
    // not "0", so a caller can tell "definitely empty" apart from "couldn't check".
    struct Status
    {
        bool    enabled = false;
        bool    enqueueRedisConnected = false;
        bool    flusherRedisConnected = false;
        bool    flusherMysqlConnected = false;
        int64_t streamLength = -1;
        int64_t pendingCount = -1;
        uint64_t totalEnqueued = 0;       // successfully XADD'd
        uint64_t totalFallbackDirect = 0; // Enqueue() couldn't reach Redis, wrote straight to targetDb instead
        uint64_t totalApplied = 0;        // Flusher successfully executed against targetDb
        uint64_t totalDropped = 0;        // Flusher gave up on a permanently-failing entry (see DbOutbox.log)
    };
    Status GetStatus();

private:
    void FlusherThreadMain();

    // Redis connection used by Enqueue() (XADD only) - separate from the Flusher's own
    // connection so a slow/blocked Flusher read can never stall a hot-path Enqueue() call.
    // Guarded by m_redisMutex (Enqueue() may be called from multiple threads).
    bool EnsureEnqueueRedisConnected(); // caller must hold m_redisMutex
    void DisconnectEnqueueRedis();      // caller must hold m_redisMutex

    // Flusher-thread-only state below (no locking - single owner, the Flusher thread).
    bool EnsureFlusherRedisConnected();
    void DisconnectFlusherRedis();
    bool EnsureFlusherMysqlConnected();
    void DisconnectFlusherMysql();
    void Ack(std::string const& entryId);
    // Executes one already-fetched (id, sql) pair against MariaDB, retrying with backoff
    // until it succeeds, or until it's identified as a permanent query-level error rather
    // than MariaDB being unreachable (see .cpp) - either way it's ack'd once resolved. Returns
    // false only if m_stop was set mid-retry (shutdown), in which case the entry is left
    // unacked for next startup.
    bool ExecuteAndAck(std::string const& entryId, std::string const& sql);
    // Drains anything left pending for our (fixed) consumer from a previous crash/restart,
    // executing each in order before moving on to genuinely new entries. Returns true if the
    // backlog was fully drained (caller can stop worrying about it until the next reconnect),
    // false if it bailed early (shutdown, or the Redis connection dropped mid-replay) - in the
    // latter case the caller must try again after the next successful reconnect, since whatever
    // was left in the pending list is still sitting there unprocessed.
    bool ReplayPending();

    Database*   m_targetDb = nullptr;
    std::string m_dbConnectionInfo;
    std::string m_streamKey;
    std::string m_groupName;
    std::string m_consumerName; // fixed - only one Flusher ever runs for a given DbWriteOutbox

    bool m_enabled = false;
    std::string m_redisSocketPath;

    std::mutex    m_redisMutex;
    redisContext* m_redisCtx = nullptr;       // Enqueue() side
    time_t        m_redisLastFailTime = 0;

    std::thread       m_flusherThread;
    std::atomic<bool> m_stop{false};

    redisContext*                    m_flusherRedisCtx = nullptr; // Flusher side, no lock needed
    time_t                           m_flusherRedisLastFailTime = 0;
    // Starts true (must replay before doing anything else); set back to true by
    // DisconnectFlusherRedis() (the single choke point for "we lost the connection", covering
    // every call site that can trigger it) so a reconnect always re-drains the pending backlog
    // before resuming normal consumption - not just once at thread startup. See FlusherThreadMain().
    bool                             m_needReplayPending = true;
    std::unique_ptr<MySQLConnection> m_flusherMysqlConn;          // Flusher's own independent connection
    time_t                           m_flusherMysqlLastFailTime = 0;

    // Set only by the Flusher thread, read only by GetStatus() (possibly from another thread,
    // e.g. a GM command) - atomic instead of piggybacking on a mutex specifically so GetStatus()
    // never has to block on whatever the Flusher happens to be doing (a slow retry, a blocking
    // XREADGROUP) just to report a snapshot.
    std::atomic<bool>     m_flusherRedisConnectedFlag{false};
    std::atomic<bool>     m_flusherMysqlConnectedFlag{false};
    std::atomic<uint64_t> m_totalEnqueued{0};
    std::atomic<uint64_t> m_totalFallbackDirect{0};
    std::atomic<uint64_t> m_totalApplied{0};
    std::atomic<uint64_t> m_totalDropped{0};

    static uint32 const RECONNECT_COOLDOWN_SEC = 30;
};

// Phase1 (see HPHA.md): the only current instance, fronting LogsDatabase for
// InstanceStatistics.cpp. Phase2 will add a separate instance (its own streamKey) in front of
// CharacterDatabase - DbWriteOutbox itself stays a plain reusable class, not a singleton,
// since more than one target Database will eventually need one.
extern DbWriteOutbox sLogsOutbox;
