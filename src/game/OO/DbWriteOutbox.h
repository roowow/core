#pragma once
#include "Common.h"
#include <string>
#include <vector>
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
// v1 scope: every Enqueue(string)'d value must be a single, independent, already-fully-formatted
// SQL statement. This deliberately avoids touching Database::BeginTransaction()/
// m_currentTransaction, which is a single non-thread-local member on the Database object - not
// safe for this class's independently-threaded Flusher to share with whatever else (main thread,
// other Enqueue() callers) might also be using transactions on the same Database object. See
// HPHA.md Phase1 notes.
//
// Multi-statement groups (added Phase3 batch 4, see HPHA.md "多语句事务扩展"): Enqueue(vector)
// durably queues N statements as one Stream entry, applied by the Flusher inside a single
// BeginTransaction()/CommitTransaction() on its OWN independent connection (never targetDb's
// shared one, so the m_currentTransaction sharing problem above still doesn't apply) - all-or-
// nothing, any statement failing rolls back the whole group and the group is retried/dropped as
// one unit, same policy as a single statement. Every statement in a group must still be safe
// under the class's normal at-least-once replay semantics (see the 幂等性/去重 decision in
// HPHA.md) - grouping doesn't relax that, it just extends "safe to replay" from one statement to
// N applied together.
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

    // Durably queues sqls to be applied together as one atomic transaction. See class comment
    // for the multi-statement group semantics. sqls.size()==1 is equivalent to (and internally
    // just calls) Enqueue(std::string const&) - no transaction wrapper for that case, byte-for-
    // byte the same wire format as before. sqls.empty() is a no-op.
    void Enqueue(std::vector<std::string> const& sqls);

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
    // Periodically (see HEARTBEAT_LOG_INTERVAL_SEC) writes a one-line status summary to
    // DbOutbox.log even when nothing is going wrong, so "is persistence sync actually healthy"
    // can be answered by tailing a log instead of only via the pull-based `.server dboutbox` GM
    // command. Flusher-thread-only, called from FlusherThreadMain() once connected.
    void LogHeartbeatIfDue();

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
    // Executes one already-fetched (id, sqls) entry against MariaDB, retrying with backoff
    // until it succeeds, or until it's identified as a permanent query-level error rather
    // than MariaDB being unreachable (see .cpp) - either way it's ack'd once resolved. Returns
    // false only if m_stop was set mid-retry (shutdown), in which case the entry is left
    // unacked for next startup.
    // sqls.size()==1: plain Execute(), exactly the pre-grouping behavior. sqls.size()>1: wrapped
    // in BeginTransaction()/CommitTransaction() on the Flusher's own connection, all-or-nothing -
    // any statement failing rolls back and the whole group is treated as one retry/drop unit.
    bool ExecuteAndAck(std::string const& entryId, std::vector<std::string> const& sqls);
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
    time_t                           m_lastHeartbeatLogTime = 0; // see LogHeartbeatIfDue()

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
    static uint32 const HEARTBEAT_LOG_INTERVAL_SEC = 300; // 5 minutes
    // Max time Shutdown() will wait for the Stream to fully drain before giving up and stopping
    // the Flusher anyway (see Shutdown()'s comment). Generous enough to absorb a shutdown-time
    // burst (e.g. many players logging out at once) without hanging process shutdown forever if
    // MariaDB itself is genuinely unreachable.
    static uint32 const SHUTDOWN_DRAIN_TIMEOUT_SEC = 60;
};

// Phase1 (see HPHA.md): fronts LogsDatabase for InstanceStatistics.cpp.
extern DbWriteOutbox sLogsOutbox;

// Phase2 (see HPHA.md): fronts WorldDatabase for the handful of runtime (not GM-command-only)
// writes identified there - ObjectMgr::_SaveVariable() (`variables`), GameEventMgr::EnableEvent()
// (`game_event`), the guild-bank-vendor `npc_vendor` writes in Player.cpp/ItemHandler.cpp.
// DbWriteOutbox itself stays a plain reusable class, not a singleton.
extern DbWriteOutbox sWorldOutbox;

// Phase3 (see HPHA.md): fronts CharacterDatabase, first batch only - the append-only
// character_log_* tables, character_displayid, character_social. Explicitly NOT
// Player::SaveToDB() (one giant periodic/logout bulk-save transaction spanning ~11 tables,
// structurally like the old `variables` DELETE+INSERT pattern but far bigger - a project of its
// own, not a Phase3-v1 target) and NOT battle_royale_season_score (relative-delta `points =
// points + N` writes - unsafe to replay under Outbox's at-least-once delivery without a
// dedup/idempotency-key design first).
extern DbWriteOutbox sCharactersOutbox;
