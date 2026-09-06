#pragma once
#include "Common.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>
#include <memory>
#include <deque>
#include <unordered_map>
#include <utility>

struct redisContext;
class Database;
class MySQLConnection;

// Ambient "which player guid is executing right now" context - read by DbWriteOutbox::Enqueue()
// so each queued write can be tagged with the guid it's for, without touching every one of the
// several dozen Enqueue() call sites across the codebase (see HPHA.md 十三 "方案C"). Set by
// WorldSession::Update() (around packet processing) and Player::Update() (around the periodic
// tick) - together these cover the overwhelming majority of writes, since almost everything that
// enqueues a characters-table write does so as a direct consequence of a player's own packet or
// their own tick. A handful of call sites act on an OFFLINE player (e.g. mail with COD delivered
// while the recipient isn't logged in) - those are simply untracked (guid stays 0), which is an
// acceptable gap since the player can't control the timing of those from outside their own
// session anyway (see HasPendingWrites()'s own comment for what guid 0 means downstream).
class DbOutboxGuidContext
{
public:
    static uint32 GetCurrent() { return s_current; }
    static void SetCurrent(uint32 guid) { s_current = guid; }
private:
    static thread_local uint32 s_current;
};

// RAII helper - sets the ambient guid context for the scope's lifetime, restoring whatever was
// there before on exit (so a nested scope - e.g. code reached from within a player's tick that
// briefly touches another player - never leaks the wrong guid onto its own writes). Prefer this
// over calling DbOutboxGuidContext::SetCurrent() directly.
class ScopedDbOutboxGuidContext
{
public:
    explicit ScopedDbOutboxGuidContext(uint32 guid) : m_previous(DbOutboxGuidContext::GetCurrent())
    {
        DbOutboxGuidContext::SetCurrent(guid);
    }
    ~ScopedDbOutboxGuidContext() { DbOutboxGuidContext::SetCurrent(m_previous); }
    ScopedDbOutboxGuidContext(ScopedDbOutboxGuidContext const&) = delete;
    ScopedDbOutboxGuidContext& operator=(ScopedDbOutboxGuidContext const&) = delete;
private:
    uint32 m_previous;
};

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
        uint64_t totalEnqueued = 0;         // successfully XADD'd
        // 2026-09-05: split in two once Enqueue()'s fallback stopped writing directly on the
        // caller's thread every time - see BufferOrDirectWrite()/ProcessFallbackQueue(). Almost
        // all fallbacks land in totalFallbackBuffered now; totalFallbackDirect should stay near
        // zero unless the buffer itself filled up or an outage genuinely outlasted the grace
        // period (both real, both worth noticing - see DbOutbox.log for the ERROR-level line
        // either one logs).
        uint64_t totalFallbackBuffered = 0; // Enqueue() couldn't reach Redis, buffered instead of writing immediately
        uint64_t totalFallbackDirect = 0;   // genuinely wrote straight to targetDb (buffer was full, or a buffered entry outlasted the grace period)
        uint64_t totalApplied = 0;          // Flusher successfully executed against targetDb
        uint64_t totalDropped = 0;          // Flusher gave up on a permanently-failing entry (see DbOutbox.log)
    };
    Status GetStatus();

    // One entry as delivered by an XREADGROUP call - public only so the file-local
    // ParseStreamEntries() in DbWriteOutbox.cpp (a free function, matching this class's existing
    // parser-function style) can build a vector of these; not meant to be used by outside callers.
    struct StreamEntry
    {
        std::string id;
        uint32 guid = 0; // 0 = untracked, see DbOutboxGuidContext's class comment
        std::vector<std::string> sqls;
    };

    // True if `guid` has at least one write (tagged via the ambient DbOutboxGuidContext at
    // Enqueue() time) that this Flusher hasn't finished resolving yet - "resolved" meaning
    // applied to targetDb OR permanently dropped as an unrecoverable query error (either way,
    // nothing more is coming for it, so it can't leave the DB in a half-updated state - see
    // MarkGuidResolved()'s call sites for the exhaustive list of "resolved" moments). Used at
    // login (see CharacterHandler.cpp) to delay loading a character's data until any of their own
    // still-in-flight writes have landed, closing the "buy something, relog before the async
    // write lands, still have the item" class of bug (see HPHA.md 十三). guid 0 always returns
    // false - untracked writes (see DbOutboxGuidContext) must never make every login look pending.
    bool HasPendingWrites(uint32 guid);

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
    // Increments/decrements the (enqueued, applied) counters HasPendingWrites() reads - a no-op
    // for guid 0 (untracked). MarkGuidEnqueued() is called once per Enqueue() call that accepted
    // responsibility for a guid-tagged write (regardless of which path it ends up taking);
    // MarkGuidResolved() is called at every point downstream where this class is done with that
    // write one way or another - see its call sites for the exhaustive list (applied, permanently
    // dropped, or forced through the buffer-full/grace-period-expired direct-write escape
    // hatches). Stale (guid, count, count) entries where both sides match are left in the map
    // rather than pruned - cheap enough (a character-count-sized map of small ints) not to bother.
    void MarkGuidEnqueued(uint32 guid);
    void MarkGuidResolved(uint32 guid);
    // Executes one already-fetched entry against MariaDB, retrying with backoff until it
    // succeeds, or until it's identified as a permanent query-level error rather than MariaDB
    // being unreachable (see .cpp) - either way it's ack'd and MarkGuidResolved()'d once resolved.
    // Returns false only if m_stop was set mid-retry (shutdown), in which case the entry is left
    // unacked (and unresolved) for next startup.
    // sqls.size()==1: plain Execute(), exactly the pre-grouping behavior. sqls.size()>1: wrapped
    // in BeginTransaction()/CommitTransaction() on the Flusher's own connection, all-or-nothing -
    // any statement failing rolls back and the whole group is treated as one retry/drop unit.
    bool ExecuteAndAck(StreamEntry const& entry);

    // New-server-migration incident (2026-09-06, see HPHA.md): the Flusher's per-entry MySQL
    // round trip (~14ms measured against a remote target after moving to a farther-away host)
    // caps single-entry throughput at roughly 70/s - nowhere near enough for a busy characters
    // table, so the Stream backlog only ever grew. Fix: read a batch of entries per XREADGROUP
    // call, and for however many of them are plain single-statement entries (the overwhelming
    // majority - character saves etc.), join their SQL with ';' and send them as ONE
    // MySQLConnection::ExecuteMultiBatch() call - one round trip pays for the whole run instead
    // of one per entry. Group entries (sqls.size()>1, e.g. DeleteInstanceFromDB's all-or-nothing
    // multi-statement units) are deliberately excluded from this combining and still go through
    // ExecuteAndAck() one at a time, unchanged - correctness of their atomicity guarantee matters
    // more than the (rare) throughput they'd otherwise contribute. Whatever a batch's
    // ExecuteMultiBatch() call didn't get to (MySQL stops a multi-statement batch at the first
    // error) falls back to the same proven ExecuteAndAck() retry/backoff/drop path, one at a
    // time, starting from the statement that failed - so a single bad entry mid-batch still only
    // costs that one entry (and whatever ran after it in the same batch, which just got slightly
    // delayed, not lost) exactly like it would outside of batching.
    void ExecuteAndAckBatch(std::vector<StreamEntry> const& entries);

    // Drains anything left pending for our (fixed) consumer from a previous crash/restart,
    // executing each in order before moving on to genuinely new entries. Returns true if the
    // backlog was fully drained (caller can stop worrying about it until the next reconnect),
    // false if it bailed early (shutdown, or the Redis connection dropped mid-replay) - in the
    // latter case the caller must try again after the next successful reconnect, since whatever
    // was left in the pending list is still sitting there unprocessed.
    bool ReplayPending();

    // Bugfix (2026-09-05): Enqueue()'s fallback used to write straight to targetDb, synchronously,
    // on the caller's (game/map) thread, every single time the enqueue-side Redis connection was
    // down - a real production incident showed a brief local-Redis blip alone trigger 1700+ of
    // these in a few seconds, each a real network round trip to the (remote) target database,
    // stalling the map thread(s) that hit them. Fix: buffer instead of writing immediately - see
    // BufferOrDirectWrite()/ProcessFallbackQueue().
    //
    // Caller must hold m_redisMutex and have already confirmed EnsureEnqueueRedisConnected()
    // succeeded (m_redisCtx valid). Returns whether the XADD itself succeeded. Factored out of
    // Enqueue() so ProcessFallbackQueue() (retrying a buffered entry once Redis looks back up)
    // can share the exact same wire-format logic instead of duplicating it. `guid` (0 = untracked)
    // is written into the entry as an extra field so a Flusher restart replaying the Stream from
    // scratch (ParseStreamEntries()) still recovers it for MarkGuidResolved() bookkeeping - the
    // in-memory m_guidPendingCounts map itself does NOT survive a restart, but that's fine: a
    // restart is exactly the "logins should wait anyway" case, and by the time this process comes
    // back up with a fresh (empty) map, ReplayPending() re-resolves every carried-over entry
    // before FlusherThreadMain() ever reports anything as caught up.
    bool TryXAdd(std::vector<std::string> const& sqls, uint32 guid);

    // Common tail for both Enqueue() overloads' fallback paths (Redis unreachable, or the XADD
    // itself failed): buffers sqls for ProcessFallbackQueue() to retry shortly, unless the buffer
    // itself is already full (FALLBACK_QUEUE_MAX) - an extreme, sustained-outage case - in which
    // case this does today's original immediate synchronous direct write as a last-resort escape
    // hatch, so a write is never silently dropped (and MarkGuidResolved()s it either way, since
    // this call is the last thing that will ever happen to it). `reason` is just for the log line.
    void BufferOrDirectWrite(std::vector<std::string> sqls, uint32 guid, char const* reason);

    // Flusher-thread-only (called once per FlusherThreadMain() loop iteration, so at least every
    // ~2s - see the XREADGROUP BLOCK timeout there). For each buffered entry: try to push it into
    // Redis now (cheap way to check "is it back yet" - if so, the entry rejoins the normal durable
    // path with no special-casing needed downstream). If Redis is still down and the entry has
    // been waiting less than FALLBACK_GRACE_SEC, leave it buffered and try again next pass - this
    // is what makes a brief blip "smooth" (the calling game thread never blocked, and if Redis
    // recovers within the grace window the entry never even touches MySQL directly). Only past
    // that grace window does this fall through to writing the entry directly via the Flusher's own
    // MySQL connection (never the game thread's) - a genuine outage still eventually gets applied,
    // just off the hot path. Unlike ExecuteAndAck(), a query-level failure here just re-buffers for
    // another attempt next pass rather than classifying transient-vs-permanent and dropping after N
    // tries - acceptable for v1 since entries reaching this path are the same SQL the normal Outbox
    // path already applies successfully day to day; a genuinely malformed statement would just sit
    // here occupying one of FALLBACK_QUEUE_MAX slots rather than being surfaced loudly.
    void ProcessFallbackQueue();

    struct PendingFallbackEntry
    {
        std::vector<std::string> sqls;
        uint32 guid; // 0 = untracked, see DbOutboxGuidContext - no default initializer here
                     // (unlike StreamEntry's) since this project targets C++14: a default member
                     // initializer would make this type a non-aggregate, breaking the brace-init
                     // construction at its one call site (BufferOrDirectWrite()) under C++14's
                     // (pre-C++17) aggregate rules. Always fully brace-initialized there with an
                     // explicit guid, so no default value is actually needed.
        time_t queuedTime;
    };
    std::mutex                        m_fallbackQueueMutex;
    std::deque<PendingFallbackEntry>  m_fallbackQueue;      // guarded by m_fallbackQueueMutex

    // guid -> (times MarkGuidEnqueued() called, times MarkGuidResolved() called). "Pending" means
    // the two differ - see HasPendingWrites(). Its own mutex since Enqueue() (any thread) writes
    // the enqueued side while the Flusher thread (and BufferOrDirectWrite(), also any thread)
    // write the resolved side - deliberately separate from every other lock in this class so a
    // login-path HasPendingWrites() call never has to wait on whatever the Flusher happens to be
    // doing (a blocking XREADGROUP, a slow retry).
    std::mutex                                              m_guidTrackingMutex;
    std::unordered_map<uint32, std::pair<uint64_t, uint64_t>> m_guidPendingCounts;

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
    std::atomic<uint64_t> m_totalFallbackBuffered{0};
    std::atomic<uint64_t> m_totalFallbackDirect{0};
    std::atomic<uint64_t> m_totalApplied{0};
    std::atomic<uint64_t> m_totalDropped{0};

    static uint32 const RECONNECT_COOLDOWN_SEC = 30;
    static uint32 const HEARTBEAT_LOG_INTERVAL_SEC = 300; // 5 minutes
    // Sized off a real incident (2026-09-05): a few seconds of local-Redis blip alone produced
    // ~1700 fallback entries. 10000 leaves comfortable headroom over that for a worse blip, at a
    // negligible memory cost (SQL strings are a few hundred bytes each - low single-digit MB even
    // completely full).
    static std::size_t const FALLBACK_QUEUE_MAX = 10000;
    // How long an entry waits in the fallback buffer for Redis to come back on its own before
    // ProcessFallbackQueue() gives up and writes it directly. Same incident's blip resolved on its
    // own within ~5s; 10s leaves margin so a slightly-longer blip still gets smoothed over instead
    // of paying the direct-MySQL-write cost.
    static uint32 const FALLBACK_GRACE_SEC = 10;
    // How many Stream entries the Flusher pulls per XREADGROUP call and, for the plain single-
    // statement ones among them, combines into one ExecuteMultiBatch() round trip - see
    // ExecuteAndAckBatch(). Sized from a real measurement (2026-09-06, see HPHA.md): ~14ms
    // measured per round trip against this deployment's target DB gives diminishing returns past
    // ~50 (14ms/50 + ~1ms server-side execution per statement is already close to the per-
    // statement floor that no amount of batching can remove) - larger batches buy little more
    // throughput while increasing how many entries a single mid-batch failure delays.
    static std::size_t const FLUSHER_BATCH_SIZE = 50;
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
