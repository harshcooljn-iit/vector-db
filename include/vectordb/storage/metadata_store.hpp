// SPDX-License-Identifier: MIT
//
// Metadata persistence, backed by SQLite.
//
// Nothing above this class knows SQLite exists — the wrapper header lives under
// src/, so the dependency cannot leak upward even by accident.
//
// Learning note: learnings/50-storage/04-sqlite.md
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <vectordb/core/types.hpp>
#include <vectordb/storage/filter.hpp>
#include <vectordb/storage/metadata.hpp>

namespace vectordb {

/// Stores per-vector metadata and the database's own configuration.
///
/// ## Why SQLite rather than another file of our own?
///
/// We hand-rolled the vector format because it is 3 GB of homogeneous floats
/// that must be memory-mappable — a job no general-purpose store does well.
/// Metadata is the opposite: small, heterogeneous, queried by predicate, and
/// updated one row at a time. That is exactly what a B-tree with a query
/// planner is for, and writing our own would mean writing indexes, a WAL and a
/// query evaluator to get less than SQLite gives for free.
///
/// See docs/decisions/ADR-0010-sqlite-for-metadata.md.
///
/// ## Why key-value rows rather than a column per attribute?
///
/// Users define their own attributes, so a wide table would need `ALTER TABLE`
/// at runtime and a migration per new key. The entity-attribute-value shape
/// trades some query efficiency for a schema that never changes. The cost is
/// real — reconstructing one vector's metadata is a multi-row read — and it is
/// bounded by an index on `(vector_id)` and by metadata objects being small.
class MetadataStore {
public:
    /// Opens or creates the database at `path`, creating the schema if needed.
    /// @throws MetadataError
    explicit MetadataStore(const std::filesystem::path& path);

    ~MetadataStore();

    MetadataStore(const MetadataStore&) = delete;
    MetadataStore& operator=(const MetadataStore&) = delete;
    MetadataStore(MetadataStore&&) noexcept;
    MetadataStore& operator=(MetadataStore&&) noexcept;

    /// Replaces all metadata for `id`. An empty map removes every key.
    /// @throws MetadataError, InvalidArgumentError on an empty key
    void set(VectorId id, const Metadata& metadata);

    /// Sets or replaces one key, leaving the others alone.
    void set_key(VectorId id, std::string_view key, const MetadataValue& value);

    /// Returns everything stored for `id`, or an empty map if there is none.
    [[nodiscard]] Metadata get(VectorId id) const;

    /// Removes one key. Returns false if it was not present.
    bool remove_key(VectorId id, std::string_view key);

    /// Removes all metadata for `id`. Returns false if there was none.
    bool remove(VectorId id);

    /// Ids matching `filter`, ascending. An empty filter returns every id that
    /// has any metadata at all — which is *not* every vector in the database.
    [[nodiscard]] std::vector<VectorId> matching(const Filter& filter) const;

    /// The same, as a set, for membership testing during a search.
    [[nodiscard]] std::unordered_set<VectorId> matching_set(const Filter& filter) const;

    /// Number of distinct vectors that have metadata.
    [[nodiscard]] std::size_t vector_count() const;

    /// Number of key-value rows across all vectors.
    [[nodiscard]] std::size_t row_count() const;

    /// Every distinct key in use, sorted. Reported by `vectordb info` so a user
    /// can see what is filterable.
    [[nodiscard]] std::vector<std::string> keys() const;

    // --- Database configuration -------------------------------------------
    // Kept in the same file so a database is a directory of files that agree
    // with each other, with no separate config format to version.

    void set_config(std::string_view key, std::string_view value);
    [[nodiscard]] std::optional<std::string> get_config(std::string_view key) const;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> all_config() const;

    /// Applies `body` inside one transaction, rolling back if it throws.
    ///
    /// This is a large performance lever, not a correctness nicety: SQLite
    /// commits each unbatched statement separately, so a bulk load without a
    /// transaction pays a durability round trip per row.
    template<typename Body>
    void in_transaction(Body&& body);

    /// Ids that have metadata but are absent from `live_ids` — orphan rows left
    /// behind by a deletion that did not finish. Reported by `vectordb check`.
    [[nodiscard]] std::vector<VectorId> orphaned_ids(
        const std::unordered_set<VectorId>& live_ids) const;

    /// Deletes metadata for ids that are not in `live_ids`. Returns how many
    /// vectors were cleaned up.
    std::size_t remove_orphans(const std::unordered_set<VectorId>& live_ids);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    // Public entry points that lock, then delegate to the unlocked forms.
    void begin_transaction();
    void commit_transaction();
    void rollback_transaction() noexcept;
};

template<typename Body>
void MetadataStore::in_transaction(Body&& body) {
    begin_transaction();
    try {
        body();
    } catch (...) {
        rollback_transaction();
        throw;
    }
    commit_transaction();
}

}  // namespace vectordb
