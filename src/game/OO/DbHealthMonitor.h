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

// mangosd counterpart to realmd/DbHealthMonitor (see HPHA.md "Phase 4/6 熔断设计") - same
// pattern (independent probe thread, independent MySQL connection, single atomic flag), scoped
// to CharacterDatabase only. Not a general "is mangosd's DB up" flag - Phase 0 already made the
// vast majority of CharacterDatabase writes async/non-blocking, and most of them are now durable
// via sCharactersOutbox besides. This monitor exists specifically to gate the small number of
// features whose writes are NOT covered by either of those (see HPHA.md Tier2: mail-with-item/
// money, auction house, trade, petition signing - all entangled with Item::SaveToDB()'s prepared-
// statement path or the SaveInventoryAndGoldToDB()/SaveGoldToDB() anti-cheat fast-save, neither of
// which can be safely wrapped in sCharactersOutbox without a larger rewrite) - reject those
// specific actions with a clear message instead of risking silent data loss/desync on a MariaDB
// outage that coincides with a mangosd crash/restart. Currently only the mail module and the
// auction house entrance actually check this flag - trade and petition signing have the same
// entanglement but aren't wired in yet (see HPHA.md).

// Starts the background probe thread. Call once, after CharacterDatabase has been initialized
// (World::SetInitialWorldSettings(), alongside sCharactersOutbox.Initialize()) - dbConnectionInfo
// is the same connection string CharacterDatabase itself was initialized with.
void StartDbHealthMonitor(std::string const& dbConnectionInfo);

// Stops the probe thread and joins it. Call during World::Shutdown(), alongside sCharactersOutbox.Shutdown().
void StopDbHealthMonitor();

// Cheap: just reads an atomic. Safe to call from opcode handlers on every relevant action.
// Starts out true - CharacterDatabase already proved connectivity once before this is called, so
// an optimistic initial value is accurate, not just hopeful.
bool IsCharacterDatabaseHealthy();
