#include "DbWriteOutbox.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseMysql.h"
#include <hiredis/hiredis.h>
#include <chrono>
#include <cstdlib>

DbWriteOutbox sLogsOutbox;
DbWriteOutbox sWorldOutbox;
DbWriteOutbox sCharactersOutbox;

DbWriteOutbox::DbWriteOutbox() = default;

// Defined here (not defaulted in the header) because m_flusherMysqlConn is a
// std::unique_ptr<MySQLConnection> and MySQLConnection is only forward-declared in the
// header - its destructor must be visible where unique_ptr's deleter actually runs.
DbWriteOutbox::~DbWriteOutbox()
{
    Shutdown();
}

void DbWriteOutbox::Initialize(std::string const& redisSocketPath, std::string const& streamKey,
                                Database& targetDb, std::string const& dbConnectionInfo)
{
    m_targetDb        = &targetDb;
    m_dbConnectionInfo = dbConnectionInfo;
    m_streamKey        = streamKey;
    m_groupName        = streamKey + ":group";
    m_consumerName      = "flusher";

    m_redisSocketPath = redisSocketPath;
    m_enabled         = !redisSocketPath.empty();

    if (!m_enabled)
    {
        sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL,
                 "DbWriteOutbox[%s]: no local Redis configured, disabled (writes go straight to the DB, same as before).",
                 m_streamKey.c_str());
        return;
    }

    m_stop = false;
    m_flusherThread = std::thread(&DbWriteOutbox::FlusherThreadMain, this);
    sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL, "DbWriteOutbox[%s]: enabled, Flusher thread started.", m_streamKey.c_str());
}

void DbWriteOutbox::Shutdown()
{
    // Bugfix (2026-09-02): wait for the Stream to actually drain before stopping the Flusher,
    // instead of giving it one more ~2s BLOCK cycle and cutting it off regardless of how much
    // backlog is left (see HPHA.md "Phase 3 续 部署实测" for the incident this caused). Anything
    // still in the Stream when the Flusher stops sits untouched until the *next* process's
    // Flusher happens to pick it up - replaying a stale write an unbounded time later, silently
    // overwriting anything changed on the DB directly in between. This only matters if the caller
    // has already stopped enqueueing new writes by the time Shutdown() runs (World::Shutdown()
    // was reordered the same day to guarantee that - see World.cpp) - otherwise this loop could
    // chase a moving target forever, which is exactly why it's still bounded by
    // SHUTDOWN_DRAIN_TIMEOUT_SEC below rather than looping unconditionally.
    if (m_enabled)
    {
        time_t const deadline = time(nullptr) + time_t(SHUTDOWN_DRAIN_TIMEOUT_SEC);
        for (;;)
        {
            int64_t remaining = -1;
            {
                std::lock_guard<std::mutex> lock(m_redisMutex);
                if (EnsureEnqueueRedisConnected())
                {
                    redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "XLEN %s", m_streamKey.c_str());
                    if (reply && reply->type == REDIS_REPLY_INTEGER)
                        remaining = reply->integer;
                    if (reply)
                        freeReplyObject(reply);
                }
            }

            if (remaining == 0)
                break; // fully drained, safe to stop the Flusher now
            if (remaining < 0)
                break; // can't even check (Redis unreachable) - nothing more this loop can do
            if (time(nullptr) >= deadline)
            {
                sLog.Out(LOG_DB_OUTBOX, LOG_LVL_ERROR,
                         "DbWriteOutbox[%s]: Shutdown() gave up waiting for the Stream to drain after %us with "
                         "%lld entries still queued - they will be replayed on next startup instead.",
                         m_streamKey.c_str(), SHUTDOWN_DRAIN_TIMEOUT_SEC, (long long)remaining);
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    m_stop = true;
    // Bounded but not instant: the Flusher thread only checks m_stop between blocking calls,
    // so this join() can take up to ~2s (the BLOCK timeout) in the common case, or up to the
    // current retry backoff (max 10s) in the rare case where it's mid-retry against an
    // unreachable MariaDB. Acceptable for v1 - only affects shutdown latency, not correctness.
    if (m_flusherThread.joinable())
        m_flusherThread.join();

    {
        std::lock_guard<std::mutex> lock(m_redisMutex);
        DisconnectEnqueueRedis();
    }
}

// ---- Enqueue() side (may be called from multiple threads) ----

bool DbWriteOutbox::EnsureEnqueueRedisConnected()
{
    if (!m_enabled)
        return false;

    if (m_redisCtx)
        return true;

    if (m_redisLastFailTime != 0 && time(nullptr) - m_redisLastFailTime < time_t(RECONNECT_COOLDOWN_SEC))
        return false;

    timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 50000; // 50ms - local Unix socket, should be near-instant

    m_redisCtx = redisConnectUnixWithTimeout(m_redisSocketPath.c_str(), tv);
    if (!m_redisCtx || m_redisCtx->err)
    {
        DisconnectEnqueueRedis();
        m_redisLastFailTime = time(nullptr);
        return false;
    }

    redisSetTimeout(m_redisCtx, tv);
    m_redisLastFailTime = 0;
    return true;
}

void DbWriteOutbox::DisconnectEnqueueRedis()
{
    if (m_redisCtx)
    {
        redisFree(m_redisCtx);
        m_redisCtx = nullptr;
    }
}

void DbWriteOutbox::Enqueue(std::string const& sql)
{
    // Defensive: m_targetDb is only ever set inside Initialize(), so a call here before
    // Initialize() has run (shouldn't happen given callers only exist post-startup, but this
    // is cheap insurance against a null-pointer crash instead of a graceful no-op if it ever
    // does) has nowhere safe to send the write.
    if (!m_targetDb)
        return;

    std::unique_lock<std::mutex> lock(m_redisMutex);

    if (!EnsureEnqueueRedisConnected())
    {
        lock.unlock();
        // Disabled/unavailable - fall straight back to today's exact behavior. This is the
        // only fallback path: no silent no-op, the write always happens somewhere.
        m_totalFallbackDirect.fetch_add(1, std::memory_order_relaxed);
        sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL,
                  "DbWriteOutbox[%s]: enqueue-side redis unreachable, falling back to direct MySQL write (single statement)",
                  m_streamKey.c_str());
        m_targetDb->Execute(sql.c_str());
        return;
    }

    redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "XADD %s * sql %b", m_streamKey.c_str(), sql.data(), sql.size());
    bool const ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply)
        freeReplyObject(reply);

    if (ok)
    {
        m_totalEnqueued.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        DisconnectEnqueueRedis();
        m_redisLastFailTime = time(nullptr);
        lock.unlock();
        // Couldn't durably queue it - still don't want to just drop the write, fall back to
        // the direct (non-durable, but no worse than pre-Outbox) path instead.
        m_totalFallbackDirect.fetch_add(1, std::memory_order_relaxed);
        sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL,
                  "DbWriteOutbox[%s]: XADD failed, falling back to direct MySQL write (single statement)",
                  m_streamKey.c_str());
        m_targetDb->Execute(sql.c_str());
    }
}

void DbWriteOutbox::Enqueue(std::vector<std::string> const& sqls)
{
    if (sqls.empty())
        return;

    if (sqls.size() == 1)
    {
        // Same wire format as always (a plain "sql" field) - no transaction wrapper needed for
        // a single statement, so just reuse the existing path byte-for-byte.
        Enqueue(sqls[0]);
        return;
    }

    if (!m_targetDb)
        return;

    std::unique_lock<std::mutex> lock(m_redisMutex);

    if (!EnsureEnqueueRedisConnected())
    {
        lock.unlock();
        // Same fallback principle as the single-statement path (never just drop the write), but
        // wrapped in a real transaction so "Redis briefly unavailable" doesn't turn an atomic
        // group into a partially-applied one - this is exactly what the call site's code would
        // have done pre-Outbox.
        m_totalFallbackDirect.fetch_add(1, std::memory_order_relaxed);
        sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL,
                  "DbWriteOutbox[%s]: enqueue-side redis unreachable, falling back to direct MySQL write (%u statements)",
                  m_streamKey.c_str(), (unsigned)sqls.size());
        m_targetDb->BeginTransaction();
        for (std::string const& sql : sqls)
            m_targetDb->Execute(sql.c_str());
        m_targetDb->CommitTransaction();
        return;
    }

    // Variable field count (count + sql0..sqlN-1), so the fixed-format redisCommand(fmt, ...)
    // helper used by the single-statement path doesn't fit - build the argv manually instead.
    // Binary-safe (length-prefixed), same as the %b used for the single-statement "sql" field.
    std::vector<std::string> args;
    args.reserve(3 + 2 + sqls.size() * 2);
    args.push_back("XADD");
    args.push_back(m_streamKey);
    args.push_back("*");
    args.push_back("count");
    args.push_back(std::to_string(sqls.size()));
    for (size_t i = 0; i < sqls.size(); ++i)
    {
        args.push_back("sql" + std::to_string(i));
        args.push_back(sqls[i]);
    }

    std::vector<char const*> argv;
    std::vector<size_t> argvLen;
    argv.reserve(args.size());
    argvLen.reserve(args.size());
    for (std::string const& a : args)
    {
        argv.push_back(a.data());
        argvLen.push_back(a.size());
    }

    redisReply* reply = (redisReply*)redisCommandArgv(m_redisCtx, int(argv.size()), argv.data(), argvLen.data());
    bool const ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply)
        freeReplyObject(reply);

    if (ok)
    {
        m_totalEnqueued.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        DisconnectEnqueueRedis();
        m_redisLastFailTime = time(nullptr);
        lock.unlock();
        m_totalFallbackDirect.fetch_add(1, std::memory_order_relaxed);
        sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL,
                  "DbWriteOutbox[%s]: XADD failed, falling back to direct MySQL write (%u statements)",
                  m_streamKey.c_str(), (unsigned)sqls.size());
        m_targetDb->BeginTransaction();
        for (std::string const& sql : sqls)
            m_targetDb->Execute(sql.c_str());
        m_targetDb->CommitTransaction();
    }
}

DbWriteOutbox::Status DbWriteOutbox::GetStatus()
{
    Status s;
    s.enabled                = m_enabled;
    s.flusherRedisConnected  = m_flusherRedisConnectedFlag.load(std::memory_order_relaxed);
    s.flusherMysqlConnected  = m_flusherMysqlConnectedFlag.load(std::memory_order_relaxed);
    s.totalEnqueued          = m_totalEnqueued.load(std::memory_order_relaxed);
    s.totalFallbackDirect    = m_totalFallbackDirect.load(std::memory_order_relaxed);
    s.totalApplied           = m_totalApplied.load(std::memory_order_relaxed);
    s.totalDropped           = m_totalDropped.load(std::memory_order_relaxed);

    // Live XLEN/XPENDING query, borrowing the Enqueue-side connection under its own mutex -
    // brief and infrequent (a GM typing a command), no need for a third dedicated connection
    // just for this.
    std::lock_guard<std::mutex> lock(m_redisMutex);
    s.enqueueRedisConnected = EnsureEnqueueRedisConnected();
    if (s.enqueueRedisConnected)
    {
        redisReply* lenReply = (redisReply*)redisCommand(m_redisCtx, "XLEN %s", m_streamKey.c_str());
        if (lenReply && lenReply->type == REDIS_REPLY_INTEGER)
            s.streamLength = lenReply->integer;
        if (lenReply)
            freeReplyObject(lenReply);

        // XPENDING <key> <group> (summary form) replies [count, minId, maxId, [[consumer,count],...]];
        // errors (e.g. the group doesn't exist yet because the Flusher hasn't connected even
        // once) are left as pendingCount == -1, same as "couldn't check".
        redisReply* pendReply = (redisReply*)redisCommand(m_redisCtx, "XPENDING %s %s", m_streamKey.c_str(), m_groupName.c_str());
        if (pendReply && pendReply->type == REDIS_REPLY_ARRAY && pendReply->elements >= 1 &&
            pendReply->element[0]->type == REDIS_REPLY_INTEGER)
            s.pendingCount = pendReply->element[0]->integer;
        if (pendReply)
            freeReplyObject(pendReply);
    }

    return s;
}

// ---- Flusher thread (single-owner, no locking needed on the members below) ----

bool DbWriteOutbox::EnsureFlusherRedisConnected()
{
    if (m_flusherRedisCtx)
        return true;

    if (m_flusherRedisLastFailTime != 0 && time(nullptr) - m_flusherRedisLastFailTime < time_t(RECONNECT_COOLDOWN_SEC))
        return false;

    timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 50000;

    m_flusherRedisCtx = redisConnectUnixWithTimeout(m_redisSocketPath.c_str(), tv);
    if (!m_flusherRedisCtx || m_flusherRedisCtx->err)
    {
        DisconnectFlusherRedis();
        m_flusherRedisLastFailTime = time(nullptr);
        return false;
    }

    // The Flusher deliberately does NOT set a short command timeout here (unlike Enqueue()'s
    // connection) - XREADGROUP...BLOCK is meant to sit waiting for up to a few seconds, that's
    // the whole point, and this connection is never on any hot path.
    m_flusherRedisLastFailTime = 0;
    m_flusherRedisConnectedFlag.store(true, std::memory_order_relaxed);

    redisReply* reply = (redisReply*)redisCommand(m_flusherRedisCtx, "XGROUP CREATE %s %s 0 MKSTREAM",
        m_streamKey.c_str(), m_groupName.c_str());
    // BUSYGROUP (group already exists from a previous run) is expected and fine - anything
    // else is logged but non-fatal, EnsureFlusherRedisConnected() will just be retried.
    if (reply && reply->type == REDIS_REPLY_ERROR && strncmp(reply->str, "BUSYGROUP", 9) != 0)
        sLog.Out(LOG_DB_OUTBOX, LOG_LVL_ERROR, "DbWriteOutbox[%s]: XGROUP CREATE failed: %s", m_streamKey.c_str(), reply->str);
    if (reply)
        freeReplyObject(reply);

    return true;
}

void DbWriteOutbox::DisconnectFlusherRedis()
{
    m_flusherRedisConnectedFlag.store(false, std::memory_order_relaxed);
    // Whatever was left in the pending list as of this connection is now unknown again - the
    // next successful reconnect must re-drain it before trusting XREADGROUP's ">" (new-only)
    // form, otherwise entries left pending from whatever just went wrong here would never be
    // looked at again for the rest of this process's lifetime. See FlusherThreadMain().
    m_needReplayPending = true;
    if (m_flusherRedisCtx)
    {
        redisFree(m_flusherRedisCtx);
        m_flusherRedisCtx = nullptr;
    }
}

bool DbWriteOutbox::EnsureFlusherMysqlConnected()
{
    if (m_flusherMysqlConn)
        return true;

    if (m_flusherMysqlLastFailTime != 0 && time(nullptr) - m_flusherMysqlLastFailTime < time_t(RECONNECT_COOLDOWN_SEC))
        return false;

    // Deliberately a brand-new, independent connection - never targetDb's shared m_pAsyncConn.
    // See the class comment / HPHA.md for why (Database::m_currentTransaction thread-safety).
    auto conn = std::make_unique<MySQLConnection>(*m_targetDb);
    // See SetTolerateQueryErrors()'s declaration in DatabaseMysql.h: without this, a malformed
    // enqueued statement (bad SQL, stale schema, ...) would ASSERT(false) and take the whole
    // server down instead of letting ExecuteAndAck()'s own retry-then-drop logic handle it.
    conn->SetTolerateQueryErrors(true);
    if (!conn->Initialize(m_dbConnectionInfo))
    {
        // MySQLConnection::OpenConnection() already logs the actual mysql_error() detail to
        // Server.log at ERROR level, but that line has no idea which of the several
        // DbWriteOutbox instances (or the game's own shared connections) it belongs to. This
        // line exists purely to disambiguate "this outbox's dedicated connection just failed" -
        // grep DbOutbox.log for m_streamKey the next time a heartbeat unexpectedly reads down -
        // from "this outbox has simply never had anything to flush yet" (the common case: the
        // flag defaults to false and this function is only reached from ExecuteAndAck(), i.e.
        // only once the Stream actually has an entry to apply).
        sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL,
                 "DbWriteOutbox[%s]: Flusher MySQL connection attempt failed, retrying in %us.",
                 m_streamKey.c_str(), RECONNECT_COOLDOWN_SEC);
        m_flusherMysqlLastFailTime = time(nullptr);
        return false;
    }

    m_flusherMysqlConn = std::move(conn);
    m_flusherMysqlLastFailTime = 0;
    m_flusherMysqlConnectedFlag.store(true, std::memory_order_relaxed);
    return true;
}

void DbWriteOutbox::DisconnectFlusherMysql()
{
    m_flusherMysqlConnectedFlag.store(false, std::memory_order_relaxed);
    m_flusherMysqlConn.reset();
}

// Parses the first (and, given COUNT 1, only) delivered entry out of an XREADGROUP reply.
// Returns false if the reply is empty/nil (nothing delivered) or malformed.
//
// Two wire formats, both produced by this class (see the two Enqueue() overloads):
//   - legacy/single-statement: one field named "sql".
//   - multi-statement group: a "count" field plus N fields named "sql0".."sql{count-1}".
// Both are supported here (not just the format the currently-running binary's Enqueue() would
// produce) so that entries written by an older build, still sitting in the Stream across a
// rolling restart, remain readable instead of silently stuck unparseable forever.
static bool ParseFirstStreamEntry(redisReply* reply, std::string& outId, std::vector<std::string>& outSqls)
{
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 1)
        return false;

    redisReply* streamPair = reply->element[0]; // [streamName, entries[]]
    if (!streamPair || streamPair->type != REDIS_REPLY_ARRAY || streamPair->elements < 2)
        return false;

    redisReply* entries = streamPair->element[1];
    if (!entries || entries->type != REDIS_REPLY_ARRAY || entries->elements < 1)
        return false;

    redisReply* entry = entries->element[0]; // [id, fields[]]
    if (!entry || entry->type != REDIS_REPLY_ARRAY || entry->elements < 2)
        return false;

    redisReply* idReply = entry->element[0];
    redisReply* fields   = entry->element[1];
    if (!idReply || idReply->type != REDIS_REPLY_STRING || !fields || fields->type != REDIS_REPLY_ARRAY)
        return false;

    outId.assign(idReply->str, idReply->len);

    bool haveCount = false;
    size_t count = 0;
    std::vector<std::string> indexed; // indexed[i] <- field "sql<i>", only meaningful if haveCount
    std::string legacySql;
    bool haveLegacySql = false;

    for (size_t i = 0; i + 1 < fields->elements; i += 2)
    {
        redisReply* fname = fields->element[i];
        redisReply* fval   = fields->element[i + 1];
        if (!fname || fname->type != REDIS_REPLY_STRING || !fval || fval->type != REDIS_REPLY_STRING)
            continue;

        std::string const name(fname->str, fname->len);
        if (name == "count")
        {
            haveCount = true;
            count = size_t(strtoul(fval->str, nullptr, 10));
            indexed.resize(count);
        }
        else if (name == "sql")
        {
            haveLegacySql = true;
            legacySql.assign(fval->str, fval->len);
        }
        else if (name.size() > 3 && name.compare(0, 3, "sql") == 0)
        {
            char* endPtr = nullptr;
            unsigned long idx = strtoul(name.c_str() + 3, &endPtr, 10);
            if (endPtr && *endPtr == '\0')
            {
                if (idx >= indexed.size())
                    indexed.resize(idx + 1);
                indexed[idx].assign(fval->str, fval->len);
            }
        }
    }

    if (haveCount)
    {
        if (count == 0 || indexed.size() != count)
            return false; // malformed - shouldn't happen, we control the producer
        outSqls = std::move(indexed);
        return true;
    }

    if (haveLegacySql)
    {
        outSqls.assign(1, std::move(legacySql));
        return true;
    }

    return false; // delivered, but neither format matched - shouldn't happen, we control the producer
}

// Best-effort ack, not retried: whoever calls this has either successfully applied the SQL,
// or given up on it as unrecoverable (see ExecuteAndAck) - either way the entry is "done" from
// this Flusher's perspective. If XACK itself fails (Redis hiccup right at this instant) the
// entry stays "pending" and gets replayed - and re-executed - on the next ReplayPending(). This
// is the unavoidable dual-write race in any "write to A, then mark done in B" design (no atomic
// way to do both); it's why every write through this class needs to tolerate at-least-once
// delivery (see the 幂等性/去重 decision in HPHA.md) rather than assuming exactly-once.
//
// Also XDELs the entry, not just XACKs it: XACK only clears it from the consumer group's
// pending list, it does NOT remove the entry's data from the stream itself (Streams are an
// append-only log - past entries just sit there forever otherwise). Without this the stream
// would grow without bound even though every entry in it has long since been applied - a slow
// memory leak on the local Redis instance that would eventually matter on a server that stays
// up for weeks. XDEL on an already-gone id is a harmless no-op, so ordering/failure of either
// call relative to the other doesn't matter.
void DbWriteOutbox::Ack(std::string const& entryId)
{
    if (!EnsureFlusherRedisConnected())
        return;

    redisReply* ackReply = (redisReply*)redisCommand(m_flusherRedisCtx, "XACK %s %s %s",
        m_streamKey.c_str(), m_groupName.c_str(), entryId.c_str());
    if (!ackReply)
    {
        DisconnectFlusherRedis();
        m_flusherRedisLastFailTime = time(nullptr);
        return; // connection's gone, don't bother trying XDEL on it too
    }
    freeReplyObject(ackReply);

    redisReply* delReply = (redisReply*)redisCommand(m_flusherRedisCtx, "XDEL %s %s", m_streamKey.c_str(), entryId.c_str());
    if (!delReply)
    {
        DisconnectFlusherRedis();
        m_flusherRedisLastFailTime = time(nullptr);
    }
    else
        freeReplyObject(delReply);
}

bool DbWriteOutbox::ExecuteAndAck(std::string const& entryId, std::vector<std::string> const& sqls)
{
    // Separate, much smaller budget for "connection is fine but this specific statement keeps
    // failing" (see below) - deliberately not unbounded like the connectivity-retry path.
    static uint32 const MAX_QUERY_ERROR_ATTEMPTS = 5;
    uint32 queryErrorAttempts = 0;
    uint32 backoffMs = 200;
    bool const isGroup = sqls.size() > 1;

    while (!m_stop)
    {
        bool const wasConnected = EnsureFlusherMysqlConnected();
        bool succeeded = false;
        if (wasConnected)
        {
            if (!isGroup)
            {
                succeeded = m_flusherMysqlConn->Execute(sqls[0]);
            }
            else
            {
                // All-or-nothing on the Flusher's own connection (never targetDb's shared one -
                // see class comment). BeginTransaction()/CommitTransaction()/RollbackTransaction()
                // are the ones fixed to self-heal a dead connection instead of crashing/wedging
                // (see HPHA.md "部署实测：共享 Database/MySQLConnection 层的两处崩溃缺陷"), so
                // this reuses that same safety net rather than anything new.
                succeeded = m_flusherMysqlConn->BeginTransaction();
                if (succeeded)
                {
                    for (std::string const& sql : sqls)
                    {
                        if (!m_flusherMysqlConn->Execute(sql))
                        {
                            succeeded = false;
                            break;
                        }
                    }
                    if (succeeded)
                        succeeded = m_flusherMysqlConn->CommitTransaction();
                    else
                        m_flusherMysqlConn->RollbackTransaction();
                }
            }
        }

        if (succeeded)
        {
            Ack(entryId);
            m_totalApplied.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // wasConnected, and an ACTIVE mysql_ping() round trip (not just "is the handle
        // non-null" - see Ping()'s comment for why that would be an unreliable proxy) confirms
        // the connection is genuinely still usable - so Execute() failing here means this was
        // a query-level error, not MariaDB being unreachable. Two flavors of this exist and we
        // can't tell them apart just from the bool: transient ones (ER_LOCK_DEADLOCK - common
        // and expected to succeed on a quick retry) and permanent ones (a bad SQL statement or
        // a constraint violation like the duplicate-key case in HPHA.md's "DBErrors_*.log 巡查"
        // - will never succeed no matter how many times it's retried). Retrying a permanent one
        // forever would stall every entry queued behind it, so this gets a short, bounded retry
        // budget (covers the transient case) instead of the connectivity path's unbounded one -
        // after that, log it loudly (real code/data bug, DBErrors log above has the mysql
        // errno/message from inside Execute()) and drop it so the queue can move on.
        if (wasConnected && m_flusherMysqlConn->Ping())
        {
            if (++queryErrorAttempts < MAX_QUERY_ERROR_ATTEMPTS)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // LOG_DB_OUTBOX (its own dedicated DbOutbox.log, see Log.h/Log.cpp) rather than the
            // general server log or even DBErrors.log - this class's own story (what got
            // enqueued/dropped/retried) is meant to be readable on its own, without wading
            // through everything else those files also capture. The exact mysql errno/message
            // for each failed attempt is still only in DBErrors.log (MySQLConnection::Execute()
            // writes it there, not exposed to this class to duplicate) - cross-reference by
            // timestamp/SQL text for that level of detail if this summary line isn't enough.
            // For a group, join every statement into the one log line - groups are expected to
            // stay small (a handful of statements, see HPHA.md "多语句事务扩展"), so this stays
            // readable; the point is being able to see the whole all-or-nothing unit that got
            // dropped, not just whichever statement happened to fail.
            std::string sqlJoined = sqls[0];
            for (size_t i = 1; i < sqls.size(); ++i)
                sqlJoined += "; " + sqls[i];
            sLog.Out(LOG_DB_OUTBOX, LOG_LVL_ERROR,
                     "DbWriteOutbox[%s]: entry %s (%u statement%s) failed %u times in a row (query "
                     "error, not a connectivity issue - see DBErrors.log for the mysql errno/message "
                     "of each attempt) and is being DROPPED so the queue isn't stuck behind it "
                     "forever. SQL: %s",
                     m_streamKey.c_str(), entryId.c_str(), (uint32)sqls.size(), isGroup ? "s" : "",
                     queryErrorAttempts, sqlJoined.c_str());
            Ack(entryId);
            m_totalDropped.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Otherwise this genuinely looks like MariaDB being unreachable (couldn't connect at
        // all, or the connection died mid-query) - drop it so the next attempt reconnects
        // instead of reusing a broken one, back off, and retry the SAME entry indefinitely
        // (never skip it, never advance) - MariaDB being down for hours is exactly the case
        // this class exists to survive. Doesn't count against queryErrorAttempts.
        DisconnectFlusherMysql();
        std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
        backoffMs = std::min(backoffMs * 2, 10000u);
    }
    return false; // shutting down mid-retry - entry stays unacked, replayed on next startup
}

bool DbWriteOutbox::ReplayPending()
{
    for (;;)
    {
        if (m_stop)
            return false;
        if (!EnsureFlusherRedisConnected())
            return false; // DisconnectFlusherRedis() (called on the way here) already re-armed m_needReplayPending

        redisReply* reply = (redisReply*)redisCommand(m_flusherRedisCtx, "XREADGROUP GROUP %s %s COUNT 1 STREAMS %s 0",
            m_groupName.c_str(), m_consumerName.c_str(), m_streamKey.c_str());
        if (!reply)
        {
            DisconnectFlusherRedis();
            m_flusherRedisLastFailTime = time(nullptr);
            return false;
        }

        std::string id;
        std::vector<std::string> sqls;
        bool const got = ParseFirstStreamEntry(reply, id, sqls);
        freeReplyObject(reply);

        if (!got)
            return true; // backlog fully drained

        if (!ExecuteAndAck(id, sqls))
            return false; // shutting down
    }
}

// Logs a one-line summary (connection state on both sides + lifetime counters + current stream
// backlog) roughly every HEARTBEAT_LOG_INTERVAL_SEC, whether or not anything is wrong. Point is
// to make "is this instance actually syncing" answerable by tailing DbOutbox.log - previously
// this class only ever logged on state transitions (connect/disconnect/error), so a perfectly
// healthy instance produced nothing after its startup line, indistinguishable at a glance from
// one that's been silently wedged for hours. A growing streamLen or a nonzero/climbing
// totalDropped across consecutive heartbeats is the signal to go look at `.server dboutbox` or
// DBErrors.log for detail; this line alone won't explain *why*, just *that* something's off.
void DbWriteOutbox::LogHeartbeatIfDue()
{
    time_t const now = time(nullptr);
    if (now - m_lastHeartbeatLogTime < time_t(HEARTBEAT_LOG_INTERVAL_SEC))
        return;
    m_lastHeartbeatLogTime = now;

    // Reuses the Flusher's own already-open connection (we're only ever called right after
    // EnsureFlusherRedisConnected() succeeded) rather than borrowing the Enqueue-side one under
    // m_redisMutex like GetStatus() does - no cross-thread contention, and this runs far too
    // infrequently for the extra round trip to matter.
    int64_t streamLen = -1;
    redisReply* reply = (redisReply*)redisCommand(m_flusherRedisCtx, "XLEN %s", m_streamKey.c_str());
    if (reply && reply->type == REDIS_REPLY_INTEGER)
        streamLen = reply->integer;
    if (reply)
        freeReplyObject(reply);

    sLog.Out(LOG_DB_OUTBOX, LOG_LVL_MINIMAL,
             "DbWriteOutbox[%s]: heartbeat - flusherRedis=%s flusherMysql=%s streamLen=%lld "
             "enqueued=%llu fallbackDirect=%llu applied=%llu dropped=%llu",
             m_streamKey.c_str(),
             m_flusherRedisConnectedFlag.load(std::memory_order_relaxed) ? "up" : "down",
             m_flusherMysqlConnectedFlag.load(std::memory_order_relaxed) ? "up" : "down",
             (long long)streamLen,
             (unsigned long long)m_totalEnqueued.load(std::memory_order_relaxed),
             (unsigned long long)m_totalFallbackDirect.load(std::memory_order_relaxed),
             (unsigned long long)m_totalApplied.load(std::memory_order_relaxed),
             (unsigned long long)m_totalDropped.load(std::memory_order_relaxed));
}

void DbWriteOutbox::FlusherThreadMain()
{
    while (!m_stop)
    {
        if (!EnsureFlusherRedisConnected())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Must fully drain whatever's left pending from a previous crash/disconnect before
        // trusting XREADGROUP's ">" (new-only) form below - otherwise those entries would never
        // be looked at again for the rest of this process's lifetime. Re-armed by
        // DisconnectFlusherRedis() on every connection loss, not just once at thread startup.
        if (m_needReplayPending)
        {
            if (!ReplayPending())
                continue; // bailed early (shutdown or dropped connection) - retry from the top
            m_needReplayPending = false;
        }

        LogHeartbeatIfDue();

        redisReply* reply = (redisReply*)redisCommand(m_flusherRedisCtx, "XREADGROUP GROUP %s %s COUNT 1 BLOCK 2000 STREAMS %s >",
            m_groupName.c_str(), m_consumerName.c_str(), m_streamKey.c_str());
        if (!reply)
        {
            DisconnectFlusherRedis();
            m_flusherRedisLastFailTime = time(nullptr);
            continue;
        }

        std::string id;
        std::vector<std::string> sqls;
        bool const got = ParseFirstStreamEntry(reply, id, sqls);
        freeReplyObject(reply);

        if (!got)
            continue; // BLOCK timed out, nothing new - loop back and check m_stop

        ExecuteAndAck(id, sqls);
    }
}
