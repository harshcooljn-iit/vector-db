// SPDX-License-Identifier: MIT
#include <vectordb/db/concurrent_database.hpp>

namespace vectordb {

ConcurrentDatabase::ConcurrentDatabase(Database database, std::size_t threads)
    : database_(std::make_unique<Database>(std::move(database))), pool_(threads) {}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

std::vector<QueryResult> ConcurrentDatabase::search(VectorView query,
                                                    const QueryOptions& options) const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    return database_->search(query, options);
}

std::vector<std::vector<QueryResult>> ConcurrentDatabase::batch_search(
    const VectorArray& queries, const QueryOptions& options) const {
    std::vector<std::vector<QueryResult>> results(queries.size());
    if (queries.empty()) {
        return results;
    }

    // Each worker takes its own shared lock rather than one being held across
    // the whole batch. Holding it outside would also work and would be slightly
    // cheaper, but it would let a long batch starve a writer indefinitely —
    // most shared_mutex implementations only allow a waiting writer to make
    // progress at a point where no reader holds the lock.
    //
    // Each worker writes only results[i], so the output needs no lock at all:
    // distinct elements of a vector are distinct objects, and concurrent writes
    // to distinct objects are not a data race.
    pool_.parallel_for(queries.size(), [&](std::size_t i) {
        const std::shared_lock<std::shared_mutex> lock(mutex_);
        results[i] = database_->search(queries[static_cast<LocalId>(i)], options);
    });

    return results;
}

std::optional<StoredVector> ConcurrentDatabase::get(VectorId id) const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    return database_->get(id);
}

bool ConcurrentDatabase::contains(VectorId id) const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    return database_->contains(id);
}

DatabaseStats ConcurrentDatabase::stats() const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    return database_->stats();
}

std::size_t ConcurrentDatabase::size() const {
    const std::shared_lock<std::shared_mutex> lock(mutex_);
    return database_->size();
}

const DatabaseConfig& ConcurrentDatabase::config() const noexcept {
    // Immutable for the lifetime of the database, so no lock is needed. Taking
    // one here would be harmless but would suggest the value can change.
    return database_->config();
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

void ConcurrentDatabase::insert(VectorId id, VectorView vector, const Metadata& metadata) {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    database_->insert(id, vector, metadata);
}

VectorId ConcurrentDatabase::insert(VectorView vector, const Metadata& metadata) {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    return database_->insert(vector, metadata);
}

bool ConcurrentDatabase::remove(VectorId id) {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    return database_->remove(id);
}

void ConcurrentDatabase::flush() {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    database_->flush();
}

void ConcurrentDatabase::rebuild_index() {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    database_->rebuild_index();
}

std::size_t ConcurrentDatabase::compact() {
    const std::unique_lock<std::shared_mutex> lock(mutex_);
    return database_->compact();
}

}  // namespace vectordb
