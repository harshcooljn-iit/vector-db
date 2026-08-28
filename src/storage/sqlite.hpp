// SPDX-License-Identifier: MIT
//
// Thin RAII wrappers over the SQLite C API.
//
// This header is private to the storage layer: it is under src/, not
// include/vectordb/, so nothing above the metadata store can see SQLite at all.
// That is what keeps "HNSW must not know about SQLite" a compile-time fact
// rather than a request. See learnings/50-storage/04-sqlite.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace vectordb::sql {

/// An open database handle.
class Connection {
public:
    /// Opens or creates the database at `path` and applies our PRAGMAs.
    /// @throws MetadataError
    explicit Connection(const std::filesystem::path& path);

    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    [[nodiscard]] sqlite3* handle() const noexcept { return handle_; }

    /// Runs one or more statements that return no rows (DDL, PRAGMA).
    /// @throws MetadataError
    void execute(std::string_view sql);

    /// The last error message SQLite produced, for building our own.
    [[nodiscard]] std::string last_error() const;

private:
    sqlite3* handle_ = nullptr;
    std::filesystem::path path_;
};

/// A prepared statement.
///
/// Prepared once and reused, because preparing is where SQLite parses and plans
/// the query. Re-preparing per row turns an insert loop into a compile loop.
///
/// It is also the only safe way to pass values: parameters are bound, never
/// concatenated, so a metadata value containing a quote is data rather than
/// syntax.
class Statement {
public:
    /// @throws MetadataError if the SQL does not compile
    Statement(Connection& connection, std::string_view sql);

    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    /// Parameter indices are 1-based, as SQLite defines them.
    void bind_null(int index);
    void bind_int64(int index, std::int64_t value);
    void bind_double(int index, double value);
    /// Copies `value`, so the caller's buffer need not outlive the step.
    void bind_text(int index, std::string_view value);

    /// Advances to the next row.
    /// @return true if a row is available, false when the statement is done
    /// @throws MetadataError on any other result code
    [[nodiscard]] bool step();

    /// Runs a statement expected to produce no rows.
    /// @throws MetadataError
    void run();

    /// Resets for reuse, keeping the compiled plan. Bindings are cleared.
    void reset();

    // Column accessors, 0-based as SQLite defines them.
    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] double column_double(int index) const;
    [[nodiscard]] std::string column_text(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;

private:
    sqlite3_stmt* statement_ = nullptr;
    sqlite3* connection_ = nullptr;
};

/// A transaction that rolls back unless it is committed.
///
/// The default is rollback, on purpose. If an exception unwinds past this
/// object, the half-applied write disappears; you have to *ask* for the change
/// to persist. The opposite default — commit on destruction — turns every
/// forgotten error path into silent partial data.
///
/// Transactions also matter for speed here: SQLite commits each unbatched
/// statement separately, which means an fsync per statement. Wrapping 10,000
/// metadata inserts in one transaction is routinely a 100x difference.
class Transaction {
public:
    /// @throws MetadataError
    explicit Transaction(Connection& connection);

    /// Rolls back if `commit()` was never called.
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    /// @throws MetadataError
    void commit();

private:
    Connection* connection_;
    bool finished_ = false;
};

}  // namespace vectordb::sql
