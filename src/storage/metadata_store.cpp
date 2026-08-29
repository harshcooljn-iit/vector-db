// SPDX-License-Identifier: MIT
#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include <vectordb/core/error.hpp>
#include <vectordb/storage/metadata_store.hpp>

#include "storage/sqlite.hpp"

namespace vectordb {
namespace {

/// Schema version, so a future layout change can be migrated rather than
/// guessed at. Stored in `schema_info` alongside the database configuration.
constexpr int kSchemaVersion = 1;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE IF NOT EXISTS schema_info (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;

-- Entity-attribute-value. One row per (vector, key) pair, so users may define
-- their own attributes without an ALTER TABLE at runtime. See the class comment
-- in metadata_store.hpp for the trade this makes.
CREATE TABLE IF NOT EXISTS metadata (
    vector_id  INTEGER NOT NULL,
    key        TEXT    NOT NULL,
    value_type INTEGER NOT NULL,
    int_value  INTEGER,
    real_value REAL,
    text_value TEXT,
    PRIMARY KEY (vector_id, key)
) WITHOUT ROWID;

-- WITHOUT ROWID makes the primary key the table's storage order, so every read
-- of one vector's metadata is a single contiguous range scan rather than an
-- index lookup followed by a row fetch per key.

-- Filtering is always "key = ? AND <typed value> <op> ?", so the useful index
-- is on (key, value) per storage class. Three narrow indexes beat one wide one
-- here because a query only ever touches the column matching the value's type.
CREATE INDEX IF NOT EXISTS idx_metadata_key_text ON metadata(key, text_value)
    WHERE text_value IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_metadata_key_int ON metadata(key, int_value)
    WHERE int_value IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_metadata_key_real ON metadata(key, real_value)
    WHERE real_value IS NOT NULL;
)sql";

}  // namespace

// ---------------------------------------------------------------------------

class MetadataStore::Impl {
public:
    explicit Impl(const std::filesystem::path& path) : connection(path) {
        connection.execute(kSchema);

        sql::Statement version(connection,
                               "INSERT OR IGNORE INTO schema_info(key, value) "
                               "VALUES('schema_version', ?)");
        version.bind_text(1, std::to_string(kSchemaVersion));
        version.run();

        // Every statement is prepared once, here, and reused. Preparing is where
        // SQLite parses and plans; doing it per row turns an insert loop into a
        // compile loop.
        insert_value = std::make_unique<sql::Statement>(
            connection,
            "INSERT OR REPLACE INTO metadata"
            "(vector_id, key, value_type, int_value, real_value, text_value) "
            "VALUES(?, ?, ?, ?, ?, ?)");
        select_by_vector = std::make_unique<sql::Statement>(
            connection,
            "SELECT key, value_type, int_value, real_value, text_value "
            "FROM metadata WHERE vector_id = ? ORDER BY key");
        delete_by_vector = std::make_unique<sql::Statement>(
            connection, "DELETE FROM metadata WHERE vector_id = ?");
        delete_key = std::make_unique<sql::Statement>(
            connection, "DELETE FROM metadata WHERE vector_id = ? AND key = ?");
        set_config_value = std::make_unique<sql::Statement>(
            connection, "INSERT OR REPLACE INTO schema_info(key, value) VALUES(?, ?)");
        get_config_value = std::make_unique<sql::Statement>(
            connection, "SELECT value FROM schema_info WHERE key = ?");
    }

    void bind_value(sql::Statement& statement, const MetadataValue& value) {
        statement.bind_int64(3, static_cast<std::int64_t>(type_of(value)));
        statement.bind_null(4);
        statement.bind_null(5);
        statement.bind_null(6);

        std::visit(
            [&](const auto& held) {
                using T = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<T, std::int64_t>) {
                    statement.bind_int64(4, held);
                } else if constexpr (std::is_same_v<T, double>) {
                    statement.bind_double(5, held);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    statement.bind_text(6, held);
                }
            },
            value);
    }

    sql::Connection connection;
    std::unique_ptr<sql::Statement> insert_value;
    std::unique_ptr<sql::Statement> select_by_vector;
    std::unique_ptr<sql::Statement> delete_by_vector;
    std::unique_ptr<sql::Statement> delete_key;
    std::unique_ptr<sql::Statement> set_config_value;
    std::unique_ptr<sql::Statement> get_config_value;
    std::unique_ptr<sql::Transaction> transaction;

    /// Guards every use of a prepared statement.
    ///
    /// Prepared statements are shared and stateful, so even a const read
    /// mutates them. See the class comment in metadata_store.hpp.
    std::mutex mutex;

    // The transaction helpers, without the lock. Callers that already hold
    // `mutex` use these; the public methods take the lock and delegate. A
    // recursive_mutex would avoid the split and would also hide which methods
    // expect to be called with the lock held.
    void begin_locked() {
        if (depth++ == 0) {
            transaction = std::make_unique<sql::Transaction>(connection);
        }
    }

    void commit_locked() {
        if (depth == 0) {
            return;
        }
        if (--depth == 0) {
            transaction->commit();
            transaction.reset();
        }
    }

    void rollback_locked() noexcept {
        depth = 0;
        transaction.reset();  // ~Transaction rolls back
    }

    /// Nesting depth. set() opens a transaction internally and may be called
    /// from inside a caller's in_transaction block, so the two must compose.
    /// Only the outermost begin/commit pair touches SQLite; SQLite has
    /// SAVEPOINT for genuine nesting, but a single outer transaction is enough
    /// here and far harder to misuse.
    int depth = 0;
};

// ---------------------------------------------------------------------------

MetadataStore::MetadataStore(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {}

MetadataStore::~MetadataStore() = default;
MetadataStore::MetadataStore(MetadataStore&&) noexcept = default;
MetadataStore& MetadataStore::operator=(MetadataStore&&) noexcept = default;

void MetadataStore::set(VectorId id, const Metadata& metadata) {
    for (const auto& [key, value] : metadata) {
        if (key.empty()) {
            throw InvalidArgumentError("metadata keys must not be empty");
        }
    }

    // Replace semantics: clear then insert, in one transaction so a failure
    // part-way through does not leave the old and new keys mixed together.
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->begin_locked();
    try {
        impl_->delete_by_vector->bind_int64(1, static_cast<std::int64_t>(id));
        impl_->delete_by_vector->run();

        for (const auto& [key, value] : metadata) {
            sql::Statement& statement = *impl_->insert_value;
            statement.bind_int64(1, static_cast<std::int64_t>(id));
            statement.bind_text(2, key);
            impl_->bind_value(statement, value);
            statement.run();
        }
    } catch (...) {
        impl_->rollback_locked();
        throw;
    }
    impl_->commit_locked();
}

void MetadataStore::set_key(VectorId id, std::string_view key, const MetadataValue& value) {
    if (key.empty()) {
        throw InvalidArgumentError("metadata keys must not be empty");
    }
    sql::Statement& statement = *impl_->insert_value;
    statement.bind_int64(1, static_cast<std::int64_t>(id));
    statement.bind_text(2, key);
    impl_->bind_value(statement, value);
    statement.run();
}

Metadata MetadataStore::get(VectorId id) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    Metadata metadata;
    sql::Statement& statement = *impl_->select_by_vector;
    statement.bind_int64(1, static_cast<std::int64_t>(id));

    while (statement.step()) {
        const std::string key = statement.column_text(0);
        const auto type = static_cast<MetadataType>(statement.column_int64(1));
        switch (type) {
            case MetadataType::kInteger:
                metadata[key] = statement.column_int64(2);
                break;
            case MetadataType::kReal:
                metadata[key] = statement.column_double(3);
                break;
            case MetadataType::kText:
                metadata[key] = statement.column_text(4);
                break;
            case MetadataType::kNull:
                metadata[key] = std::monostate{};
                break;
        }
    }
    statement.reset();
    return metadata;
}

bool MetadataStore::remove_key(VectorId id, std::string_view key) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    sql::Statement count(impl_->connection,
                         "SELECT COUNT(*) FROM metadata WHERE vector_id = ? AND key = ?");
    count.bind_int64(1, static_cast<std::int64_t>(id));
    count.bind_text(2, key);
    const bool present = count.step() && count.column_int64(0) > 0;
    count.reset();

    if (!present) {
        return false;
    }

    impl_->delete_key->bind_int64(1, static_cast<std::int64_t>(id));
    impl_->delete_key->bind_text(2, key);
    impl_->delete_key->run();
    return true;
}

bool MetadataStore::remove(VectorId id) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    sql::Statement count(impl_->connection,
                         "SELECT COUNT(*) FROM metadata WHERE vector_id = ?");
    count.bind_int64(1, static_cast<std::int64_t>(id));
    const bool present = count.step() && count.column_int64(0) > 0;
    count.reset();

    if (!present) {
        return false;
    }

    impl_->delete_by_vector->bind_int64(1, static_cast<std::int64_t>(id));
    impl_->delete_by_vector->run();
    return true;
}

std::vector<VectorId> MetadataStore::matching(const Filter& filter) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    // Evaluation happens in C++ over the reconstructed metadata, not as
    // generated SQL.
    //
    // Pushing the predicate into SQL would use the indexes and is the right
    // answer for a large database — but with the EAV layout it means one
    // self-join per condition, plus generating SQL from user input, plus
    // keeping the C++ and SQL evaluators agreeing on every edge case (missing
    // keys, cross-type comparison, NaN). Two evaluators that must agree is a
    // bug source; one evaluator that is also used for post-filtering during
    // search is not. This is the simplest thing that is correct, and the
    // trade-off is stated in docs/metadata.md rather than hidden.
    std::vector<VectorId> matches;

    sql::Statement statement(
        impl_->connection,
        "SELECT vector_id, key, value_type, int_value, real_value, text_value "
        "FROM metadata ORDER BY vector_id, key");

    Metadata current;
    VectorId current_id = 0;
    bool have_current = false;

    const auto flush = [&] {
        if (have_current && filter.matches(current)) {
            matches.push_back(current_id);
        }
    };

    while (statement.step()) {
        const auto id = static_cast<VectorId>(statement.column_int64(0));
        if (!have_current || id != current_id) {
            flush();
            current.clear();
            current_id = id;
            have_current = true;
        }

        const std::string key = statement.column_text(1);
        switch (static_cast<MetadataType>(statement.column_int64(2))) {
            case MetadataType::kInteger:
                current[key] = statement.column_int64(3);
                break;
            case MetadataType::kReal:
                current[key] = statement.column_double(4);
                break;
            case MetadataType::kText:
                current[key] = statement.column_text(5);
                break;
            case MetadataType::kNull:
                current[key] = std::monostate{};
                break;
        }
    }
    flush();

    return matches;
}

std::unordered_set<VectorId> MetadataStore::matching_set(const Filter& filter) const {
    const std::vector<VectorId> ids = matching(filter);
    return {ids.begin(), ids.end()};
}

std::size_t MetadataStore::vector_count() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    sql::Statement statement(impl_->connection,
                             "SELECT COUNT(DISTINCT vector_id) FROM metadata");
    const std::size_t count =
        statement.step() ? static_cast<std::size_t>(statement.column_int64(0)) : 0;
    return count;
}

std::size_t MetadataStore::row_count() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    sql::Statement statement(impl_->connection, "SELECT COUNT(*) FROM metadata");
    return statement.step() ? static_cast<std::size_t>(statement.column_int64(0)) : 0;
}

std::vector<std::string> MetadataStore::keys() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string> result;
    sql::Statement statement(impl_->connection,
                             "SELECT DISTINCT key FROM metadata ORDER BY key");
    while (statement.step()) {
        result.push_back(statement.column_text(0));
    }
    return result;
}

void MetadataStore::set_config(std::string_view key, std::string_view value) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->set_config_value->bind_text(1, key);
    impl_->set_config_value->bind_text(2, value);
    impl_->set_config_value->run();
}

std::optional<std::string> MetadataStore::get_config(std::string_view key) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    sql::Statement& statement = *impl_->get_config_value;
    statement.bind_text(1, key);
    std::optional<std::string> value;
    if (statement.step()) {
        value = statement.column_text(0);
    }
    statement.reset();
    return value;
}

std::vector<std::pair<std::string, std::string>> MetadataStore::all_config() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::pair<std::string, std::string>> entries;
    sql::Statement statement(impl_->connection,
                             "SELECT key, value FROM schema_info ORDER BY key");
    while (statement.step()) {
        entries.emplace_back(statement.column_text(0), statement.column_text(1));
    }
    return entries;
}

std::vector<VectorId> MetadataStore::orphaned_ids(
    const std::unordered_set<VectorId>& live_ids) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<VectorId> orphans;
    sql::Statement statement(impl_->connection,
                             "SELECT DISTINCT vector_id FROM metadata ORDER BY vector_id");
    while (statement.step()) {
        const auto id = static_cast<VectorId>(statement.column_int64(0));
        if (live_ids.find(id) == live_ids.end()) {
            orphans.push_back(id);
        }
    }
    return orphans;
}

std::size_t MetadataStore::remove_orphans(const std::unordered_set<VectorId>& live_ids) {
    // orphaned_ids takes the lock itself, so it must be called before we do.
    const std::vector<VectorId> orphans = orphaned_ids(live_ids);
    if (orphans.empty()) {
        return 0;
    }

    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->begin_locked();
    try {
        for (const VectorId id : orphans) {
            impl_->delete_by_vector->bind_int64(1, static_cast<std::int64_t>(id));
            impl_->delete_by_vector->run();
        }
    } catch (...) {
        impl_->rollback_locked();
        throw;
    }
    impl_->commit_locked();
    return orphans.size();
}

void MetadataStore::begin_transaction() {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->begin_locked();
}

void MetadataStore::commit_transaction() {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->commit_locked();
}

void MetadataStore::rollback_transaction() noexcept {
    // An inner failure aborts the whole outer transaction, not just its own
    // frame. That is the conservative reading and the right one: a caller whose
    // nested write failed has no basis for believing the rest succeeded.
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->rollback_locked();
}

}  // namespace vectordb
