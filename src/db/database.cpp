// SPDX-License-Identifier: MIT
#include <algorithm>

#include <spdlog/spdlog.h>

#include <vectordb/core/error.hpp>
#include <vectordb/db/database.hpp>
#include <vectordb/index/brute_force_index.hpp>
#include <vectordb/index/hnsw_file.hpp>
#include <vectordb/index/hnsw_index.hpp>
#include <vectordb/storage/metadata_store.hpp>
#include <vectordb/storage/vector_file.hpp>
#include <vectordb/storage/vector_store.hpp>

namespace vectordb {
namespace {

constexpr std::string_view kVectorFileName = "vectors.bin";
constexpr std::string_view kIndexFileName = "index.hnsw";
constexpr std::string_view kMetadataFileName = "metadata.sqlite";

// Config keys, in the metadata database's schema_info table.
constexpr std::string_view kKeyDimension = "dimension";
constexpr std::string_view kKeyMetric = "metric";
constexpr std::string_view kKeyIndexType = "index_type";
constexpr std::string_view kKeyNormalize = "normalize";
constexpr std::string_view kKeyHnswM = "hnsw_m";
constexpr std::string_view kKeyHnswEfConstruction = "hnsw_ef_construction";
constexpr std::string_view kKeyHnswEfSearch = "hnsw_ef_search";
constexpr std::string_view kKeyHnswSeed = "hnsw_seed";
constexpr std::string_view kKeyNextId = "next_auto_id";

/// How many extra candidates to fetch when filtering an approximate search.
///
/// A post-filter can only keep what the index returned, so a filter that
/// matches 1 vector in 10 needs roughly 10x the candidates to fill k. The
/// default is a compromise that covers mildly selective filters; anything
/// stricter should use an exact scan, and `search()` says so.
constexpr std::size_t kDefaultFilterOversample = 8;

std::uint64_t parse_u64(const std::optional<std::string>& text, std::string_view key) {
    if (!text.has_value()) {
        throw CorruptionError("database configuration is missing '" + std::string(key) + "'");
    }
    try {
        return std::stoull(*text);
    } catch (const std::exception&) {
        throw CorruptionError("database configuration key '" + std::string(key) +
                              "' is not a number: '" + *text + "'");
    }
}

std::uint64_t file_size_or_zero(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::uint64_t>(size);
}

}  // namespace

void DatabaseConfig::validate() const {
    if (dimension == 0 || dimension > kMaxDimension) {
        throw InvalidArgumentError("dimension must be between 1 and " +
                                   std::to_string(kMaxDimension) + " (got " +
                                   std::to_string(dimension) + ")");
    }
    if (index_type == IndexType::kHnsw) {
        hnsw.validate();
    }
}

std::size_t CheckReport::error_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(issues.begin(), issues.end(), [](const CheckIssue& issue) {
            return issue.severity == "error";
        }));
}

std::size_t CheckReport::warning_count() const noexcept {
    return issues.size() - error_count();
}

// ---------------------------------------------------------------------------

class Database::Impl {
public:
    Impl(std::filesystem::path directory,
         DatabaseConfig config,
         VectorStore store,
         MetadataStore metadata)
        : directory_(std::move(directory)),
          config_(config),
          store_(std::move(store)),
          metadata_(std::move(metadata)) {}

    /// Builds an index from scratch by replaying every slot.
    void build_index() {
        if (config_.index_type == IndexType::kHnsw) {
            auto hnsw =
                std::make_unique<HnswIndex>(store_.accessor(), config_.metric, config_.hnsw);
            hnsw->reserve(store_.slot_count());
            for (LocalId id = 0; id < store_.slot_count(); ++id) {
                hnsw->add(id);
                // A tombstoned slot is still added, then immediately marked
                // dead. Skipping it would break the ascending-slot contract and
                // renumber every node.
                if (!store_.live(id)) {
                    hnsw->remove(id);
                }
            }
            index_ = std::move(hnsw);
        } else {
            index_ = std::make_unique<BruteForceIndex>(store_.accessor(), config_.metric);
        }
    }

    void refresh_accessor() {
        if (auto* hnsw = dynamic_cast<HnswIndex*>(index_.get())) {
            hnsw->set_accessor(store_.accessor());
        } else if (auto* brute = dynamic_cast<BruteForceIndex*>(index_.get())) {
            brute->set_accessor(store_.accessor());
        }
    }

    void persist_config() {
        metadata_.in_transaction([&] {
            metadata_.set_config(kKeyDimension, std::to_string(config_.dimension));
            metadata_.set_config(kKeyMetric, metric_name(config_.metric));
            metadata_.set_config(kKeyIndexType, index_type_name(config_.index_type));
            metadata_.set_config(kKeyNormalize, config_.normalize ? "1" : "0");
            metadata_.set_config(kKeyHnswM, std::to_string(config_.hnsw.m));
            metadata_.set_config(kKeyHnswEfConstruction,
                                 std::to_string(config_.hnsw.ef_construction));
            metadata_.set_config(kKeyHnswEfSearch, std::to_string(config_.hnsw.ef_search));
            metadata_.set_config(kKeyHnswSeed, std::to_string(config_.hnsw.seed));
            metadata_.set_config(kKeyNextId, std::to_string(next_auto_id_));
        });
    }

    std::filesystem::path directory_;
    DatabaseConfig config_;
    VectorStore store_;
    MetadataStore metadata_;
    std::unique_ptr<VectorIndex> index_;
    VectorId next_auto_id_ = 0;
    bool dirty_ = false;
};

// ---------------------------------------------------------------------------

std::filesystem::path Database::vector_file_path(const std::filesystem::path& directory) {
    return directory / kVectorFileName;
}

std::filesystem::path Database::index_file_path(const std::filesystem::path& directory) {
    return directory / kIndexFileName;
}

std::filesystem::path Database::metadata_file_path(const std::filesystem::path& directory) {
    return directory / kMetadataFileName;
}

Database::Database(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Database::~Database() {
    if (impl_ == nullptr) {
        return;
    }
    // Best effort: a destructor must not throw, and losing the un-flushed
    // writes is strictly better than terminating. Callers who need to know the
    // flush succeeded call flush() explicitly — the same split as
    // FileDescriptor::close().
    try {
        flush();
    } catch (const std::exception& error) {
        spdlog::error("failed to flush database on close: {}", error.what());
    }
}

Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

Database Database::create(const std::filesystem::path& directory,
                          const DatabaseConfig& config) {
    config.validate();

    if (std::filesystem::exists(directory)) {
        throw InvalidArgumentError("'" + directory.string() +
                                   "' already exists; refusing to overwrite it");
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw IoError("cannot create '" + directory.string() + "': " + error.message());
    }

    VectorStore store(config.dimension, config.normalize);
    MetadataStore metadata(metadata_file_path(directory));

    auto impl =
        std::make_unique<Impl>(directory, config, std::move(store), std::move(metadata));
    impl->persist_config();
    impl->build_index();
    impl->dirty_ = true;  // so an empty database still gets its files written

    spdlog::info("created database '{}' (dimension={}, metric={}, index={})",
                 directory.string(),
                 config.dimension,
                 metric_name(config.metric),
                 index_type_name(config.index_type));

    Database database(std::move(impl));
    database.flush();
    return database;
}

Database Database::open(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw NotFoundError("no database at '" + directory.string() + "'");
    }
    const auto metadata_path = metadata_file_path(directory);
    if (!std::filesystem::exists(metadata_path)) {
        throw NotFoundError("'" + directory.string() + "' is not a VectorDB database (no " +
                            std::string(kMetadataFileName) + ")");
    }

    MetadataStore metadata(metadata_path);

    DatabaseConfig config;
    config.dimension =
        static_cast<Dimension>(parse_u64(metadata.get_config(kKeyDimension), kKeyDimension));

    const std::optional<std::string> metric_text = metadata.get_config(kKeyMetric);
    if (!metric_text.has_value() || !parse_metric(*metric_text, config.metric)) {
        throw CorruptionError("database configuration has an unknown metric '" +
                              metric_text.value_or("<missing>") + "'");
    }

    const std::optional<std::string> index_text = metadata.get_config(kKeyIndexType);
    if (!index_text.has_value() || !parse_index_type(*index_text, config.index_type)) {
        throw CorruptionError("database configuration has an unknown index type '" +
                              index_text.value_or("<missing>") + "'");
    }

    config.normalize = metadata.get_config(kKeyNormalize).value_or("0") == "1";
    config.hnsw.m = parse_u64(metadata.get_config(kKeyHnswM), kKeyHnswM);
    config.hnsw.ef_construction =
        parse_u64(metadata.get_config(kKeyHnswEfConstruction), kKeyHnswEfConstruction);
    config.hnsw.ef_search = parse_u64(metadata.get_config(kKeyHnswEfSearch), kKeyHnswEfSearch);
    config.hnsw.seed = parse_u64(metadata.get_config(kKeyHnswSeed), kKeyHnswSeed);
    config.validate();

    const auto vector_path = vector_file_path(directory);
    VectorStore store = std::filesystem::exists(vector_path)
                            ? read_vector_file(vector_path)
                            : VectorStore(config.dimension, config.normalize);

    if (store.dimension() != config.dimension) {
        throw CorruptionError("vector file has dimension " + std::to_string(store.dimension()) +
                              " but the database configuration says " +
                              std::to_string(config.dimension));
    }

    auto impl =
        std::make_unique<Impl>(directory, config, std::move(store), std::move(metadata));
    impl->next_auto_id_ =
        parse_u64(impl->metadata_.get_config(kKeyNextId).value_or("0"), kKeyNextId);

    // Load the index if we can; rebuild it if we cannot. The vector store is
    // authoritative, so a bad index is never a reason to fail an open — it is a
    // reason to spend some CPU. Falling back silently would be wrong, though,
    // so the reason is logged.
    const auto index_path = index_file_path(directory);
    bool loaded = false;
    if (config.index_type == IndexType::kHnsw && std::filesystem::exists(index_path)) {
        try {
            impl->index_ = read_hnsw_file(
                index_path, impl->store_.accessor(), config.metric, config.dimension);
            loaded = true;
        } catch (const CorruptionError& error) {
            spdlog::warn("index at '{}' failed validation ({}); rebuilding from vectors",
                         index_path.string(),
                         error.what());
        } catch (const IoError& error) {
            spdlog::warn("index at '{}' could not be read ({}); rebuilding from vectors",
                         index_path.string(),
                         error.what());
        }
    }

    if (!loaded) {
        impl->build_index();
        // The rebuilt index has not been written yet, so the database is dirty
        // even though the caller has changed nothing.
        impl->dirty_ = config.index_type == IndexType::kHnsw;
    }

    spdlog::debug("opened database '{}' ({} vectors, {} slots)",
                  directory.string(),
                  impl->store_.live_count(),
                  impl->store_.slot_count());

    return Database(std::move(impl));
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void Database::insert(VectorId id, VectorView vector, const Metadata& metadata) {
    const LocalId local_id = impl_->store_.insert(id, vector);
    impl_->refresh_accessor();
    impl_->index_->add(local_id);

    if (!metadata.empty()) {
        impl_->metadata_.set(id, metadata);
    }
    if (id >= impl_->next_auto_id_) {
        impl_->next_auto_id_ = id + 1;
    }
    impl_->dirty_ = true;
}

VectorId Database::insert(VectorView vector, const Metadata& metadata) {
    const VectorId id = impl_->next_auto_id_;
    insert(id, vector, metadata);
    return id;
}

void Database::upsert(VectorId id, VectorView vector, const Metadata& metadata) {
    if (impl_->store_.contains(id)) {
        // The slot keeps its LocalId, so the graph's edges stay valid — but they
        // now describe where the *old* vector was. The node is in the wrong
        // place in the graph until a rebuild. Documented rather than hidden;
        // callers replacing many vectors should rebuild afterwards.
        impl_->store_.upsert(id, vector);
        impl_->refresh_accessor();
        impl_->metadata_.set(id, metadata);
        impl_->dirty_ = true;
        return;
    }
    insert(id, vector, metadata);
}

bool Database::remove(VectorId id) {
    const std::optional<LocalId> local_id = impl_->store_.find(id);
    if (!local_id.has_value()) {
        return false;
    }

    impl_->store_.remove(id);
    impl_->refresh_accessor();
    impl_->index_->remove(*local_id);
    impl_->metadata_.remove(id);
    impl_->dirty_ = true;
    return true;
}

void Database::set_metadata(VectorId id, const Metadata& metadata) {
    if (!impl_->store_.contains(id)) {
        throw NotFoundError("no vector with id " + std::to_string(id));
    }
    impl_->metadata_.set(id, metadata);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

std::optional<StoredVector> Database::get(VectorId id) const {
    const std::optional<LocalId> local_id = impl_->store_.find(id);
    if (!local_id.has_value()) {
        return std::nullopt;
    }

    const VectorView stored = impl_->store_.vector(*local_id);
    StoredVector result;
    result.id = id;
    result.vector.assign(stored.begin(), stored.end());
    result.metadata = impl_->metadata_.get(id);
    return result;
}

bool Database::contains(VectorId id) const {
    return impl_->store_.contains(id);
}

std::vector<QueryResult> Database::search(VectorView query, const QueryOptions& options) const {
    if (options.k == 0) {
        throw InvalidArgumentError("search requires k >= 1");
    }
    validate_vector(query, impl_->config_.dimension);

    const bool has_filter = !options.filter.empty();
    const bool use_exact = options.exact || impl_->config_.index_type == IndexType::kBruteForce;

    // Over-fetch so the post-filter has something to discard. See the header for
    // why this is a compromise rather than a solution.
    std::size_t fetch = options.k;
    if (has_filter && !use_exact) {
        const std::size_t oversample = options.filter_oversample > 0 ? options.filter_oversample
                                                                     : kDefaultFilterOversample;
        fetch = options.k * oversample;
    }

    std::vector<Candidate> candidates;
    if (use_exact) {
        const BruteForceIndex exact(impl_->store_.accessor(), impl_->config_.metric);
        // An exact scan with a filter must consider everything, or the filter
        // could exclude the entire fetched window while matches exist further
        // out. This is what makes `exact` the right answer for a selective
        // filter, and why it costs O(N).
        exact
            .search(query,
                    SearchParams{.k = has_filter ? impl_->store_.slot_count() : options.k})
            .swap(candidates);
    } else {
        impl_->index_->search(query, SearchParams{.k = fetch, .ef_search = options.ef_search})
            .swap(candidates);
    }

    std::vector<QueryResult> results;
    results.reserve(std::min(options.k, candidates.size()));

    for (const Candidate& candidate : candidates) {
        if (results.size() >= options.k) {
            break;
        }
        const VectorId id = impl_->store_.vector_id(candidate.local_id);
        Metadata metadata = impl_->metadata_.get(id);
        if (has_filter && !options.filter.matches(metadata)) {
            continue;
        }
        results.push_back(QueryResult{id,
                                      score_from_rank_key(impl_->config_.metric, candidate.key),
                                      std::move(metadata)});
    }

    return results;
}

std::vector<std::vector<QueryResult>> Database::batch_search(
    const VectorArray& queries, const QueryOptions& options) const {
    std::vector<std::vector<QueryResult>> results;
    results.reserve(queries.size());
    for (LocalId q = 0; q < queries.size(); ++q) {
        results.push_back(search(queries[q], options));
    }
    return results;
}

std::vector<VectorId> Database::filter_ids(const Filter& filter) const {
    std::vector<VectorId> ids = impl_->metadata_.matching(filter);
    // Metadata rows can outlive their vector if a crash landed between the two
    // deletes, so the store has the final say on what exists.
    std::erase_if(ids, [&](VectorId id) { return !impl_->store_.contains(id); });
    return ids;
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

void Database::flush() {
    if (!impl_->dirty_) {
        return;
    }

    // Vectors first, index second. A crash between them leaves an index that
    // disagrees with the store, which `open` detects and rebuilds. The reverse
    // order would leave an index describing vectors that were never written —
    // the same inconsistency, but pointing at data that does not exist.
    write_vector_file(vector_file_path(impl_->directory_), impl_->store_);

    if (impl_->config_.index_type == IndexType::kHnsw) {
        const auto* hnsw = dynamic_cast<const HnswIndex*>(impl_->index_.get());
        if (hnsw != nullptr) {
            write_hnsw_file(
                index_file_path(impl_->directory_), *hnsw, impl_->config_.dimension);
        }
    }

    impl_->metadata_.set_config(kKeyNextId, std::to_string(impl_->next_auto_id_));
    impl_->dirty_ = false;
}

void Database::rebuild_index() {
    spdlog::info("rebuilding index for '{}' from {} slots",
                 impl_->directory_.string(),
                 impl_->store_.slot_count());
    impl_->build_index();
    impl_->dirty_ = true;
    flush();
}

std::size_t Database::compact() {
    const std::size_t reclaimed = impl_->store_.tombstone_count();
    if (reclaimed == 0) {
        return 0;
    }

    // Rebuild the store with only the live slots. VectorIds are preserved;
    // LocalIds are renumbered, which is exactly why the two are separate id
    // spaces (ADR-0003) and why the graph must be rebuilt rather than patched.
    VectorStore compacted(impl_->config_.dimension, impl_->config_.normalize);
    compacted.reserve(impl_->store_.live_count());

    for (LocalId slot = 0; slot < impl_->store_.slot_count(); ++slot) {
        if (!impl_->store_.live(slot)) {
            continue;
        }
        compacted.append_trusted(impl_->store_.vector_id(slot),
                                 impl_->store_.vector(slot),
                                 impl_->store_.norm(slot),
                                 /*live=*/true);
    }

    impl_->store_ = std::move(compacted);
    impl_->build_index();
    impl_->dirty_ = true;
    flush();

    spdlog::info("compacted '{}': reclaimed {} slots", impl_->directory_.string(), reclaimed);
    return reclaimed;
}

CheckReport Database::check(bool deep) const {
    CheckReport report;
    const auto error = [&](std::string message) {
        report.issues.push_back(CheckIssue{"error", std::move(message)});
    };
    const auto warn = [&](std::string message) {
        report.issues.push_back(CheckIssue{"warning", std::move(message)});
    };

    const auto vector_path = vector_file_path(impl_->directory_);
    const auto index_path = index_file_path(impl_->directory_);

    if (!std::filesystem::exists(vector_path)) {
        if (impl_->store_.slot_count() > 0) {
            error("vector file is missing but the database holds " +
                  std::to_string(impl_->store_.slot_count()) + " slots");
        }
    } else {
        try {
            const VectorFileHeader header = read_vector_file_header(vector_path);
            if (header.dimension != impl_->config_.dimension) {
                error("vector file dimension " + std::to_string(header.dimension) +
                      " disagrees with the configured " +
                      std::to_string(impl_->config_.dimension));
            }
            if (deep && !verify_vector_file_payload(vector_path)) {
                error(
                    "vector file payload checksum mismatch — the float data has "
                    "been damaged");
            }
        } catch (const Error& e) {
            error(std::string("vector file: ") + e.what());
        }
    }

    if (impl_->config_.index_type == IndexType::kHnsw) {
        if (!std::filesystem::exists(index_path)) {
            warn("index file is missing; it will be rebuilt on the next open");
        } else {
            try {
                const HnswFileHeader header = read_hnsw_file_header(index_path);
                if (header.node_count != impl_->store_.slot_count()) {
                    error("index holds " + std::to_string(header.node_count) +
                          " nodes but the store holds " +
                          std::to_string(impl_->store_.slot_count()) +
                          " slots; run rebuild-index");
                }
                if (header.metric != impl_->config_.metric) {
                    error("index was built for metric '" +
                          std::string(metric_name(header.metric)) +
                          "' but the database uses '" +
                          std::string(metric_name(impl_->config_.metric)) + "'");
                }
                if (deep && !verify_hnsw_file_payload(index_path)) {
                    error("index payload checksum mismatch; run rebuild-index");
                }
            } catch (const Error& e) {
                error(std::string("index file: ") + e.what());
            }
        }
    }

    // Cross-artefact: metadata rows referring to vectors that no longer exist.
    // Inert — nothing reads them — so a warning rather than an error.
    std::unordered_set<VectorId> live;
    live.reserve(impl_->store_.live_count());
    for (LocalId slot = 0; slot < impl_->store_.slot_count(); ++slot) {
        if (impl_->store_.live(slot)) {
            live.insert(impl_->store_.vector_id(slot));
        }
    }
    const std::vector<VectorId> orphans = impl_->metadata_.orphaned_ids(live);
    if (!orphans.empty()) {
        warn(std::to_string(orphans.size()) +
             " metadata rows reference vectors that no longer exist (first: " +
             std::to_string(orphans.front()) +
             "); they are inert and can be cleaned "
             "up with compact");
    }

    if (impl_->store_.tombstone_ratio() > 0.5 && impl_->store_.slot_count() > 100) {
        warn("more than half the slots are tombstones (" +
             std::to_string(impl_->store_.tombstone_count()) + " of " +
             std::to_string(impl_->store_.slot_count()) + "); run compact to reclaim them");
    }

    if (deep) {
        // Non-finite values cannot enter through insert(), so finding one means
        // the file was damaged after it was written.
        for (LocalId slot = 0; slot < impl_->store_.slot_count(); ++slot) {
            if (!is_finite(impl_->store_.vector(slot))) {
                error("slot " + std::to_string(slot) + " (id " +
                      std::to_string(impl_->store_.vector_id(slot)) +
                      ") contains a non-finite component");
                break;
            }
        }
    }

    return report;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

const DatabaseConfig& Database::config() const noexcept {
    return impl_->config_;
}

const std::filesystem::path& Database::directory() const noexcept {
    return impl_->directory_;
}

std::size_t Database::size() const noexcept {
    return impl_->store_.live_count();
}

DatabaseStats Database::stats() const {
    DatabaseStats stats;
    stats.dimension = impl_->config_.dimension;
    stats.metric = impl_->config_.metric;
    stats.index_type = impl_->config_.index_type;
    stats.index_description = impl_->index_->describe();

    stats.live_vectors = impl_->store_.live_count();
    stats.slots = impl_->store_.slot_count();
    stats.tombstones = impl_->store_.tombstone_count();
    stats.tombstone_ratio = impl_->store_.tombstone_ratio();

    stats.metadata_vectors = impl_->metadata_.vector_count();
    stats.metadata_rows = impl_->metadata_.row_count();
    stats.metadata_keys = impl_->metadata_.keys();

    stats.vector_bytes = impl_->store_.memory_bytes();
    stats.index_bytes = impl_->index_->index_bytes();
    stats.vector_file_bytes = file_size_or_zero(vector_file_path(impl_->directory_));
    stats.index_file_bytes = file_size_or_zero(index_file_path(impl_->directory_));
    stats.metadata_file_bytes = file_size_or_zero(metadata_file_path(impl_->directory_));

    stats.dirty = impl_->dirty_;
    return stats;
}

}  // namespace vectordb
