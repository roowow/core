/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "DbHealthMonitor.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseMysql.h"
#include "Config/Config.h"
#include "Log.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace
{
    std::atomic<bool> g_healthy{true};
    std::atomic<bool> g_stop{false};
    std::thread g_probeThread;
    // Deliberately a separate connection from LoginDatabase's own query connection (see
    // DbHealthMonitor.h) - constructed/reset only from the probe thread, no locking needed.
    std::unique_ptr<MySQLConnection> g_probeConn;
    std::string g_dbConnectionInfo;

    // Logged every HEARTBEAT_LOG_INTERVAL_SEC regardless of state changes, same reasoning as
    // DbWriteOutbox::LogHeartbeatIfDue() in the game core (see HPHA.md) - a probe that's been
    // silently wedged looks identical, from the log alone, to one that's healthy and just has
    // nothing to report; a periodic line even when nothing changed makes that distinguishable.
    uint32 const HEARTBEAT_LOG_INTERVAL_SEC = 300;
    time_t g_lastHeartbeatLogTime = 0;

    void ProbeThreadMain()
    {
        uint32 const intervalSec = sConfig.GetIntDefault("DbHealthMonitor.ProbeIntervalSec", 3);

        while (!g_stop)
        {
            // mysql_ping() is a real network round trip, not just "is the handle non-null" -
            // same reasoning as MySQLConnection::Ping()'s own comment (see HPHA.md Phase1's
            // "会不会误判" entry): only an actual round trip reliably tells connected apart
            // from merely-not-yet-noticed-as-dead.
            bool ok = g_probeConn && g_probeConn->Ping();
            if (!ok)
            {
                // Ping() failing likely means the connection itself is unusable - rebuild it
                // from scratch rather than keep retrying a possibly-broken handle.
                g_probeConn = std::make_unique<MySQLConnection>(LoginDatabase);
                ok = g_probeConn->Initialize(g_dbConnectionInfo) && g_probeConn->Ping();
            }

            bool const wasHealthy = g_healthy.exchange(ok, std::memory_order_relaxed);
            if (wasHealthy && !ok)
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[DbHealthMonitor] LoginDatabase probe failed - new logins will be rejected with WOW_FAIL_DB_BUSY until it recovers.");
            else if (!wasHealthy && ok)
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[DbHealthMonitor] LoginDatabase probe recovered - accepting logins again.");

            time_t const now = time(nullptr);
            if (now - g_lastHeartbeatLogTime >= time_t(HEARTBEAT_LOG_INTERVAL_SEC))
            {
                g_lastHeartbeatLogTime = now;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[DbHealthMonitor] heartbeat - LoginDatabase healthy=%s", ok ? "yes" : "no");
            }

            for (uint32 waited = 0; waited < intervalSec && !g_stop; ++waited)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
} // namespace

void StartDbHealthMonitor(std::string const& dbConnectionInfo)
{
    g_dbConnectionInfo = dbConnectionInfo;
    g_probeConn = std::make_unique<MySQLConnection>(LoginDatabase);
    g_probeConn->Initialize(g_dbConnectionInfo); // ok if this fails - the probe loop will retry

    g_stop = false;
    g_probeThread = std::thread(ProbeThreadMain);
}

void StopDbHealthMonitor()
{
    g_stop = true;
    if (g_probeThread.joinable())
        g_probeThread.join();
    g_probeConn.reset();
}

bool IsLoginDatabaseHealthy()
{
    return g_healthy.load(std::memory_order_relaxed);
}
