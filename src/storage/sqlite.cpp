// SPDX-License-Identifier: MIT
#include "storage/sqlite.hpp"

#include <string>
#include <utility>

#include <sqlite3.h>

#include <vectordb/core/error.hpp>

namespace vectordb::sql {
namespace {

[[noreturn]] void throw_sqlite_error(sqlite3* handle, std::string_view operation, int code) {
    std::string message(operation);
    message += " failed: ";
    message += sqlite3_errstr(code);
    if (handle != nullptr) {
        const char* detail = sqlite3_errmsg(handle);
        if (detail != nullptr) {
            message += " (";
            message += detail;
            message += ")";
        }
    }
    message += " [code ";
    message += std::to_string(code);
    message += "]";
    throw MetadataError(message);
}

}  // namespace

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

Connection::Connection(const std::filesystem::path& path) : path_(path) {
    const int code = sqlite3_open_v2(
        path.c_str(), &handle_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (code != SQLITE_OK) {
        // sqlite3_open_v2 allocates the handle even on failure, so it must be
        // closed before throwing or the handle leaks.
        sqlite3* failed = handle_;
        handle_ = nullptr;
        std::string message = "cannot open metadata database '" + path.string() + "': ";
        message += failed != nullptr ? sqlite3_errmsg(failed) : sqlite3_errstr(code);
        sqlite3_close(failed);
        throw MetadataError(message);
    }

    // A statement that cannot get its lock waits up to 5 seconds instead of
    // failing immediately with SQLITE_BUSY. Without this, two processes touching
    // the same database produce spurious errors under trivial contention.
    sqlite3_busy_timeout(handle_, 5000);

    execute(
        // WAL lets readers proceed while a writer is active, which matches this
        // system's read-mostly shape. It also makes commits cheaper, because
        // they append to a log rather than rewriting pages in place.
        "PRAGMA journal_mode = WAL;"
        // NORMAL syncs at checkpoints rather than at every commit. Combined with
        // WAL this is durable against process crash, and can lose the most
        // recent transactions on a power cut. That trade is stated in
        // docs/persistence.md rather than assumed.
        "PRAGMA synchronous = NORMAL;"
        // Off by default in SQLite, which surprises everyone. Our schema
        // declares them, so they must be enforced.
        "PRAGMA foreign_keys = ON;"
        // Store temporary indices and sorters in memory rather than files.
        "PRAGMA temp_store = MEMORY;");
}

Connection::~Connection() {
    if (handle_ != nullptr) {
        // sqlite3_close_v2 tolerates outstanding statements by deferring the
        // close, which makes destruction order between a Connection and its
        // Statements a non-issue.
        sqlite3_close_v2(handle_);
    }
}

Connection::Connection(Connection&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            sqlite3_close_v2(handle_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

void Connection::execute(std::string_view sql) {
    char* error = nullptr;
    const std::string statement(sql);
    const int code = sqlite3_exec(handle_, statement.c_str(), nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        std::string message = "SQL failed: ";
        message += error != nullptr ? error : sqlite3_errstr(code);
        sqlite3_free(error);
        throw MetadataError(message);
    }
}

std::string Connection::last_error() const {
    const char* message = handle_ != nullptr ? sqlite3_errmsg(handle_) : nullptr;
    return message != nullptr ? message : "unknown error";
}

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------

Statement::Statement(Connection& connection, std::string_view sql)
    : connection_(connection.handle()) {
    const int code = sqlite3_prepare_v2(
        connection_, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr);
    if (code != SQLITE_OK) {
        throw_sqlite_error(connection_, "preparing statement '" + std::string(sql) + "'", code);
    }
}

Statement::~Statement() {
    sqlite3_finalize(statement_);
}

Statement::Statement(Statement&& other) noexcept
    : statement_(std::exchange(other.statement_, nullptr)),
      connection_(std::exchange(other.connection_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(statement_);
        statement_ = std::exchange(other.statement_, nullptr);
        connection_ = std::exchange(other.connection_, nullptr);
    }
    return *this;
}

void Statement::bind_null(int index) {
    const int code = sqlite3_bind_null(statement_, index);
    if (code != SQLITE_OK) {
        throw_sqlite_error(connection_, "bind null", code);
    }
}

void Statement::bind_int64(int index, std::int64_t value) {
    const int code = sqlite3_bind_int64(statement_, index, value);
    if (code != SQLITE_OK) {
        throw_sqlite_error(connection_, "bind integer", code);
    }
}

void Statement::bind_double(int index, double value) {
    const int code = sqlite3_bind_double(statement_, index, value);
    if (code != SQLITE_OK) {
        throw_sqlite_error(connection_, "bind real", code);
    }
}

void Statement::bind_text(int index, std::string_view value) {
    // SQLITE_TRANSIENT makes SQLite copy the bytes, so the caller's buffer does
    // not have to outlive the step. SQLITE_STATIC would avoid the copy and is a
    // reliable source of use-after-free.
    const int code = sqlite3_bind_text(
        statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (code != SQLITE_OK) {
        throw_sqlite_error(connection_, "bind text", code);
    }
}

bool Statement::step() {
    const int code = sqlite3_step(statement_);
    if (code == SQLITE_ROW) {
        return true;
    }
    if (code == SQLITE_DONE) {
        return false;
    }
    throw_sqlite_error(connection_, "step", code);
}

void Statement::run() {
    if (step()) {
        throw MetadataError("statement unexpectedly returned rows");
    }
    reset();
}

void Statement::reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
}

std::int64_t Statement::column_int64(int index) const {
    return sqlite3_column_int64(statement_, index);
}

double Statement::column_double(int index) const {
    return sqlite3_column_double(statement_, index);
}

std::string Statement::column_text(int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    if (text == nullptr) {
        return {};
    }
    const int size = sqlite3_column_bytes(statement_, index);
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)};
}

bool Statement::column_is_null(int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL;
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Connection& connection) : connection_(&connection) {
    connection_->execute("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    if (!finished_) {
        // Rolling back is the default because an exception unwinding past this
        // point means the operation did not complete. A destructor must not
        // throw, so a failing rollback is swallowed — SQLite will roll back on
        // the next connection open regardless.
        try {
            connection_->execute("ROLLBACK");
        } catch (const MetadataError&) {  // NOLINT(bugprone-empty-catch)
        }
    }
}

void Transaction::commit() {
    connection_->execute("COMMIT");
    finished_ = true;
}

}  // namespace vectordb::sql
