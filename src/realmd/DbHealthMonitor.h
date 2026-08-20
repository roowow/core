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

#pragma once

#include "Common.h"
#include <string>

// Lightweight circuit breaker for realmd's LoginDatabase (see HPHA.md "熔断 + 错误区分设计").
// A background thread with its own independent MySQL connection - deliberately NOT sharing
// LoginDatabase's own query connection, so a stuck reconnect attempt on that connection can't
// also block this probe - periodically pings the database and exposes the result as a single
// atomic flag. AuthSocket.cpp checks this before touching the database, so a real outage fails
// every new login attempt fast (WOW_FAIL_DB_BUSY) instead of each one paying a full
// blocking-reconnect-timeout tax on realmd's single IO thread.

// Starts the background probe thread. Call once, after LoginDatabase.Initialize() has already
// succeeded (StartDB() in Main.cpp) - dbConnectionInfo is the same connection string
// LoginDatabase itself was initialized with.
void StartDbHealthMonitor(std::string const& dbConnectionInfo);

// Stops the probe thread and joins it. Call during shutdown.
void StopDbHealthMonitor();

// Cheap: just reads an atomic. Safe to call from the IO thread on every login attempt.
// Starts out true - StartDB() already proved connectivity once before this is called, so an
// optimistic initial value is accurate, not just hopeful.
bool IsLoginDatabaseHealthy();
