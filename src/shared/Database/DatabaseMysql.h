/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
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

#ifndef _DATABASEMYSQL_H
#define _DATABASEMYSQL_H

//#include "Common.h"
#include "Database.h"
#include "Policies/Singleton.h"

#ifdef WIN32
#include <winsock2.h>
#endif
#include <mysql.h>

// my_bool declaration is removed in 8.0
#if MYSQL_VERSION_ID >= 80000
typedef char my_bool;
#ifdef _MSC_VER
#pragma message("You are using an incompatible mysql version!")
#else
#warning "You are using an incompatible mysql version!"
#endif
#endif

//MySQL prepared statement class
class MySqlPreparedStatement : public SqlPreparedStatement
{
public:
    MySqlPreparedStatement(std::string const& fmt, SqlConnection& conn, MYSQL* mysql);
    ~MySqlPreparedStatement() override;

    //prepare statement
    bool prepare() override;

    //bind input parameters
    void bind(SqlStmtParameters const& holder) override;

    //execute DML statement
    bool execute() override;

protected:
    //bind parameters
    void addParam(int nIndex, SqlStmtFieldData const& data);

    static enum_field_types ToMySQLType(SqlStmtFieldData const& data, my_bool& bUnsigned);

private:
    void RemoveBinds();

    MYSQL* m_pMySQLConn;
    MYSQL_STMT* m_stmt;
    MYSQL_BIND* m_pInputArgs;
    MYSQL_BIND* m_pResult;
    MYSQL_RES* m_pResultMetadata;
};

class MySQLConnection : public SqlConnection
{
    public:
        MySQLConnection(Database& db) : SqlConnection(db), mMysql(nullptr) {}
        ~MySQLConnection() override;

        bool OpenConnection(bool reconnect) override;
        bool Reconnect();
        bool HandleMySQLError(uint32 errNo);

        std::unique_ptr<QueryResult> Query(std::string const& sql) override;
        std::unique_ptr<QueryNamedResult> QueryNamed(std::string const& sql) override;
        bool Execute(std::string const& sql) override;

        unsigned long escape_string(char* to, char const* from, unsigned long length) override;

        bool BeginTransaction() override;
        bool CommitTransaction() override;
        bool RollbackTransaction() override;

        // Actively verifies the connection is genuinely usable (a real round trip via
        // mysql_ping(), not just "is the handle non-null") - deliberately NOT the same as
        // checking whether HandleMySQLError() happened to close mMysql, because that only
        // covers the 3 specific CR_SERVER_* codes it special-cases; any other connection-
        // related error it doesn't recognize falls into its `default:` branch and leaves
        // mMysql set without the connection actually working, which a "is the handle still
        // non-null" check would silently misreport as fine (see HPHA.md's "会不会误判" entry).
        // Used by DbWriteOutbox's Flusher to tell "MariaDB unreachable, keep retrying forever"
        // apart from "this specific statement is permanently invalid, retrying it forever would
        // just stall the whole queue" without needing to plumb the raw MySQL errno out through
        // Execute()'s bool.
        bool Ping() { return mMysql && mysql_ping(mMysql) == 0; }

        // Opt-in only, off by default. HandleMySQLError() normally ASSERT(false)s on a query-level
        // error it doesn't know how to recover from (bad SQL, missing table/column, unrecognized
        // errno) - correct for regular gameplay code, where a malformed query is always a core bug
        // that must fail loudly rather than silently corrupt/lose data. DbWriteOutbox's Flusher
        // connection is the one legitimate exception: it already has its own retry-then-log-and-
        // drop handling for exactly this class of error (see ExecuteAndAck() in DbWriteOutbox.cpp,
        // "a bad SQL statement... will never succeed no matter how many times it's retried") and
        // needs Execute() to actually return false so that logic can run, instead of the process
        // dying on the first occurrence - one malformed enqueued write should not be able to take
        // the whole server down, repeatedly, on every restart, until someone manually drains the
        // poisoned entry out of Redis. Set only on that dedicated connection.
        void SetTolerateQueryErrors(bool tolerate) { m_tolerateQueryErrors = tolerate; }

        // Opt-in only, off by default - must be called before Initialize()/OpenConnection() (it
        // only takes effect via the CLIENT_MULTI_STATEMENTS flag passed to mysql_real_connect(),
        // which can't be changed on an already-open connection). Enables ExecuteMultiBatch()
        // below on this connection. Set only on DbWriteOutbox's dedicated Flusher connection (see
        // its Enqueue()/ExecuteAndAckBatch() in DbWriteOutbox.cpp) - never on a connection that
        // might ever run a query built from unescaped/uncontrolled input, since a multi-statement-
        // enabled connection executes anything after a literal ';' as a second statement instead
        // of erroring on it.
        void SetMultiStatementsEnabled(bool enabled) { m_multiStatementsEnabled = enabled; }

        // Sends `count` already-complete, independent statements joined with ';' as a single
        // mysql_query() (one network round trip for all of them) - requires
        // SetMultiStatementsEnabled(true) to have been set before this connection was opened.
        // Autocommit is on (see OpenConnection()), so each statement still commits individually
        // as the server works through them - this is NOT an atomic all-or-nothing unit like
        // BeginTransaction()/CommitTransaction(), just a way to amortize one network round trip
        // over many statements. MySQL stops executing a multi-statement batch at the first
        // statement that errors - so the return value is how many of the `count` statements (in
        // order, starting from the first) actually completed; the caller must retry the
        // statement at that index and everything after it (they never ran at all), see
        // DbWriteOutbox::ExecuteAndAckBatch().
        size_t ExecuteMultiBatch(std::string const& combinedSql, size_t count);

    protected:
        SqlPreparedStatement* CreateStatement(std::string const& fmt) override;
        void OnPreparedStatementFailure() override;

    private:
        bool _TransactionCmd(std::string const& sql);
        bool _Query(std::string const& sql, MYSQL_RES** pResult, MYSQL_FIELD** pFields, uint64* pRowCount, uint32* pFieldCount);

        MYSQL* mMysql;
        bool m_tolerateQueryErrors = false;
        bool m_multiStatementsEnabled = false;
};

class DatabaseMysql : public Database
{
    friend class MaNGOS::OperatorNew<DatabaseMysql>;

    public:
        DatabaseMysql();
        ~DatabaseMysql() override;

        //! Initializes Mysql and connects to a server.
        /*! infoString should be formated like hostname;username;password;database. */

        // must be call before first query in thread
        void ThreadStart() override;
        // must be call before finish thread run
        void ThreadEnd() override;

    protected:
        SqlConnection* CreateConnection() override;

    private:
        static size_t db_count;
};

#endif
