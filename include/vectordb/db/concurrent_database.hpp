// SPDX-License-Identifier: MIT
//
// The concurrency contract, made explicit and enforced.
//
// Learning note: learnings/60-concurrency/05-reader-writer-concurrency.md
// Specification: docs/concurrency.md
#pragma once

#include <memory>
#include <shared_mutex>
#include <vector>

#include <vectordb/concurrency/thread_pool.hpp>
#include <vectordb/db/database.hpp>

namespace vectordb {

/// Wraps a `Database` in a reader-writer lock and a thread pool.
///
/// ## The contract
///
/// | Operation | Concurrency |
/// |---|---|
/// | `search`, `get`, `stats` | any number at once |
/// | `insert`, `remove`, `flush`, `compact` | exclusive |
///
/// A `std::shared_mutex` gives exactly that: readers take a shared lock and do
/// not block each other; a writer takes an exclusive lock and waits for the
/// readers to drain.
///
/// ## Why not a plain mutex?
///
/// It would be correct, and it would serialise every search. This is a
/// read-mostly workload — that is the entire shape of a vector database — so
/// serialising reads throws away the only easy parallelism available.
///
/// ## Why not lock-free?
///
/// Because correctness first, and because the measurement does not call for it
/// yet. A lock-free index is a large undertaking (epoch reclamation, or hazard
/// pointers, to know when a replaced neighbour list can be freed) and the win
/// only appears when lock *contention* is the bottleneck. Under a read-mostly
/// load, a shared lock's readers do not contend with each other at all — they
/// contend on the mutex's own cache line, which is a much smaller cost. See
/// docs/decisions/ADR-0013-concurrency.md.
///
/// ## What this does not give you
///
/// Concurrent *writers*. Insertion mutates the HNSW graph, and making that
/// safe needs per-node locking with a careful ordering to avoid deadlock. Not
/// implemented, not claimed, and stated in the docs rather than left to be
/// discovered under load.
class ConcurrentDatabase {
public:
    /// @param database  taken by value; this wrapper owns it from now on
    /// @param threads   worker count for batch search; 0 means hardware threads
    explicit ConcurrentDatabase(Database database, std::size_t threads = 0);

    // --- Reads: shared lock ------------------------------------------------

    [[nodiscard]] std::vector<QueryResult> search(VectorView query,
                                                  const QueryOptions& options) const;

    /// Runs the queries across the pool.
    ///
    /// Each worker takes its own shared lock — they do not block each other —
    /// and writes into its own slot of the result vector, so no synchronisation
    /// is needed on the output.
    [[nodiscard]] std::vector<std::vector<QueryResult>> batch_search(
        const VectorArray& queries, const QueryOptions& options) const;

    [[nodiscard]] std::optional<StoredVector> get(VectorId id) const;
    [[nodiscard]] bool contains(VectorId id) const;
    [[nodiscard]] DatabaseStats stats() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] const DatabaseConfig& config() const noexcept;

    // --- Writes: exclusive lock -------------------------------------------

    void insert(VectorId id, VectorView vector, const Metadata& metadata = {});
    VectorId insert(VectorView vector, const Metadata& metadata = {});
    bool remove(VectorId id);
    void flush();
    void rebuild_index();
    std::size_t compact();

    /// Runs `body` under the exclusive lock, for a sequence of writes that must
    /// not interleave with readers.
    template<typename Body>
    void with_exclusive_access(Body&& body) {
        const std::unique_lock<std::shared_mutex> lock(mutex_);
        body(*database_);
    }

    /// Runs `body` under the shared lock.
    template<typename Body>
    void with_shared_access(Body&& body) const {
        const std::shared_lock<std::shared_mutex> lock(mutex_);
        body(*database_);
    }

    [[nodiscard]] std::size_t thread_count() const noexcept { return pool_.size(); }

private:
    // `mutable` so a const read method can lock it. The lock is not part of the
    // object's observable value, which is exactly what mutable is for.
    mutable std::shared_mutex mutex_;
    std::unique_ptr<Database> database_;
    mutable ThreadPool pool_;
};

}  // namespace vectordb
