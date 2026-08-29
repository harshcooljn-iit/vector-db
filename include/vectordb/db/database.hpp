// SPDX-License-Identifier: MIT
//
// The application-level API. Everything the CLI can do is reachable here, which
// is what keeps the CLI thin enough to be uninteresting.
//
// Learning note: learnings/80-database-engineering/01-consistency.md
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vectordb/core/search_result.hpp>
#include <vectordb/core/types.hpp>
#include <vectordb/core/vector.hpp>
#include <vectordb/index/hnsw_config.hpp>
#include <vectordb/index/vector_index.hpp>
#include <vectordb/storage/filter.hpp>
#include <vectordb/storage/metadata.hpp>

namespace vectordb {

/// Everything a database needs to know about itself, fixed at creation and
/// persisted. A database that had to be told its own dimension on every open
/// would be one typo away from silent corruption.
struct DatabaseConfig {
    Dimension dimension = 0;
    Metric metric = Metric::kCosine;
    IndexType index_type = IndexType::kHnsw;
    HnswConfig hnsw;

    /// Scale every vector to unit length on insert.
    ///
    /// Lossy — the original magnitude is gone — and it makes cosine cost
    /// exactly what a dot product costs, because every cached norm becomes 1.
    bool normalize = false;

    /// @throws InvalidArgumentError
    void validate() const;
};

/// Per-query options.
struct QueryOptions {
    std::size_t k = 10;

    /// HNSW candidate width. 0 uses the database's configured default.
    std::size_t ef_search = 0;

    /// Metadata predicate. Empty matches everything.
    Filter filter;

    /// Force an exact scan even when an ANN index exists.
    ///
    /// The right choice for a highly selective filter — see `search()`.
    bool exact = false;

    /// How much to over-fetch when filtering an approximate search.
    /// 0 uses the built-in default.
    std::size_t filter_oversample = 0;
};

/// A vector plus its metadata, as returned by `get`.
struct StoredVector {
    VectorId id = kInvalidVectorId;
    std::vector<float> vector;
    Metadata metadata;
};

/// A search result, with metadata attached when it was asked for.
struct QueryResult {
    VectorId id = kInvalidVectorId;
    float score = 0.0F;
    Metadata metadata;
};

/// Counters reported by `vectordb stats`.
struct DatabaseStats {
    Dimension dimension = 0;
    Metric metric = Metric::kCosine;
    IndexType index_type = IndexType::kBruteForce;
    std::string index_description;

    std::size_t live_vectors = 0;
    std::size_t slots = 0;
    std::size_t tombstones = 0;
    double tombstone_ratio = 0.0;

    std::size_t metadata_vectors = 0;
    std::size_t metadata_rows = 0;
    std::vector<std::string> metadata_keys;

    std::size_t vector_bytes = 0;
    std::size_t index_bytes = 0;
    std::uint64_t vector_file_bytes = 0;
    std::uint64_t index_file_bytes = 0;
    std::uint64_t metadata_file_bytes = 0;

    bool dirty = false;
};

/// One problem found by `check`.
struct CheckIssue {
    /// "error" for something that makes the database unusable as it stands;
    /// "warning" for something inert, such as an orphaned metadata row.
    std::string severity;
    std::string message;
};

struct CheckReport {
    std::vector<CheckIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }

    [[nodiscard]] std::size_t error_count() const noexcept;
    [[nodiscard]] std::size_t warning_count() const noexcept;
};

/// A VectorDB database: a directory holding a vector file, an index file and a
/// metadata database.
///
/// ```
/// mydb/
///   vectors.bin       authoritative float payload
///   index.hnsw        derived, disposable, rebuildable
///   metadata.sqlite   metadata rows and the database configuration
/// ```
///
/// ## Durability
///
/// Vectors and the index are written on `flush()` and on destruction — not on
/// every insert, which would make a bulk load unusable. Metadata is written
/// immediately, because SQLite is transactional and cheap enough per row.
///
/// A crash before `flush()` loses the un-flushed inserts. It never produces
/// inconsistent state: the vector file is replaced atomically, the index file
/// is written after it, and on open an index that disagrees with the vector
/// store is rebuilt rather than trusted. See docs/persistence.md.
///
/// ## Thread safety
///
/// Concurrent `search` calls are safe. Any mutation requires exclusive access.
/// The class does not lock for you — see docs/concurrency.md and
/// `ConcurrentDatabase` for the wrapper that does.
class Database {
public:
    /// Creates a new database directory.
    /// @throws InvalidArgumentError if the directory already exists, or on a bad config
    static Database create(const std::filesystem::path& directory,
                           const DatabaseConfig& config);

    /// Opens an existing database.
    ///
    /// If the index file is missing, corrupt, or inconsistent with the vector
    /// store, it is **rebuilt** rather than trusted, and the fact is logged.
    /// The vector store is authoritative; an index is derived state.
    ///
    /// @throws NotFoundError, CorruptionError, IoError
    static Database open(const std::filesystem::path& directory);

    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    // --- Writing ----------------------------------------------------------

    /// Inserts with a caller-supplied id.
    /// @throws DuplicateIdError, DimensionMismatchError, InvalidVectorError
    void insert(VectorId id, VectorView vector, const Metadata& metadata = {});

    /// Inserts with an automatically assigned id (one past the highest ever
    /// used, so ids are never reused after deletion).
    VectorId insert(VectorView vector, const Metadata& metadata = {});

    /// Inserts, or replaces the vector and metadata of an existing id.
    void upsert(VectorId id, VectorView vector, const Metadata& metadata = {});

    /// Tombstones `id` and drops its metadata. Returns false if absent.
    bool remove(VectorId id);

    /// Replaces the metadata of an existing vector.
    /// @throws NotFoundError
    void set_metadata(VectorId id, const Metadata& metadata);

    // --- Reading ----------------------------------------------------------

    [[nodiscard]] std::optional<StoredVector> get(VectorId id) const;
    [[nodiscard]] bool contains(VectorId id) const;

    /// Nearest neighbours of `query`.
    ///
    /// ## Filtering semantics
    ///
    /// When `options.filter` is non-empty and the index is approximate, the
    /// search **over-fetches and then filters**: it asks the index for
    /// `k * oversample` candidates and keeps the first `k` that match.
    ///
    /// This is simple and it is honest about its limitation: if the filter is
    /// highly selective, the over-fetched window may contain fewer than `k`
    /// matches, and the result is short even though matching vectors exist
    /// further out. The recall loss grows with selectivity, not with k.
    ///
    /// For a selective filter, set `options.exact` and take the exact scan —
    /// it is `O(N)` but it cannot miss. The trade is documented in
    /// docs/metadata.md and there is no way to have both cheaply, which is why
    /// the choice is exposed rather than guessed.
    ///
    /// @throws InvalidArgumentError, DimensionMismatchError
    [[nodiscard]] std::vector<QueryResult> search(VectorView query,
                                                  const QueryOptions& options) const;

    /// Runs several queries. Sequential today; `ConcurrentDatabase` parallelises.
    [[nodiscard]] std::vector<std::vector<QueryResult>> batch_search(
        const VectorArray& queries, const QueryOptions& options) const;

    // --- Maintenance ------------------------------------------------------

    /// Writes the vector file and the index file. Idempotent, and a no-op when
    /// nothing has changed.
    void flush();

    /// Discards the index and rebuilds it from the authoritative vector store.
    ///
    /// The recovery path for a corrupt index, and the way to reclaim the graph
    /// space held by tombstoned nodes.
    void rebuild_index();

    /// Rewrites the store without tombstoned slots, renumbering `LocalId`s, and
    /// rebuilds the index. `VectorId`s are unaffected — which is the whole
    /// reason they are a separate id space.
    ///
    /// @return how many slots were reclaimed
    std::size_t compact();

    /// Structural and cross-artefact checks.
    /// @param deep  also verify payload checksums, which is a full pass over
    ///              every byte of both files
    [[nodiscard]] CheckReport check(bool deep = false) const;

    // --- Inspection -------------------------------------------------------

    [[nodiscard]] const DatabaseConfig& config() const noexcept;
    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] DatabaseStats stats() const;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Ids matching a metadata filter, without any vector search.
    [[nodiscard]] std::vector<VectorId> filter_ids(const Filter& filter) const;

    // --- Paths ------------------------------------------------------------

    [[nodiscard]] static std::filesystem::path vector_file_path(
        const std::filesystem::path& directory);
    [[nodiscard]] static std::filesystem::path index_file_path(
        const std::filesystem::path& directory);
    [[nodiscard]] static std::filesystem::path metadata_file_path(
        const std::filesystem::path& directory);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    explicit Database(std::unique_ptr<Impl> impl);
};

}  // namespace vectordb
