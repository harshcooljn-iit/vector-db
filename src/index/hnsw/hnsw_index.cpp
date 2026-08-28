// SPDX-License-Identifier: MIT
//
// The HNSW graph: construction, search, and neighbour selection.
//
// Read learnings/40-search-indexes/04-hnsw-algorithm.md alongside this file.
#include <algorithm>
#include <cmath>
#include <string>

#include <vectordb/core/error.hpp>
#include <vectordb/core/top_k.hpp>
#include <vectordb/index/hnsw_index.hpp>

namespace vectordb {
namespace {

constexpr std::uint32_t kNoUpperLinks = std::numeric_limits<std::uint32_t>::max();

/// Tracks which nodes a search has already visited.
///
/// The obvious implementation is a `std::vector<bool>` cleared per query. At one
/// million nodes that is a 125 KB memset before every search — often more work
/// than the search itself, which may touch only a few thousand nodes.
///
/// Instead each slot holds the *generation* that last visited it. Resetting is
/// incrementing a counter: O(1) instead of O(N). The full clear happens only on
/// the roughly four-billionth query, when the generation wraps.
///
/// This trick — version stamps instead of clearing — shows up wherever a large
/// scratch structure is reused across many small operations.
class VisitedSet {
public:
    void resize(std::size_t count) {
        if (stamps_.size() < count) {
            stamps_.resize(count, 0);
        }
    }

    void begin_search() noexcept {
        if (++generation_ == 0) {
            std::fill(stamps_.begin(), stamps_.end(), 0);
            generation_ = 1;
        }
    }

    /// Returns true if `id` had not been seen this generation, and marks it.
    [[nodiscard]] bool visit(LocalId id) noexcept {
        if (stamps_[id] == generation_) {
            return false;
        }
        stamps_[id] = generation_;
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return stamps_.size(); }

private:
    std::vector<std::uint32_t> stamps_;
    std::uint32_t generation_ = 0;
};

/// Per-thread scratch space for a search.
///
/// Every buffer here would otherwise be allocated per query. The visited set is
/// the expensive one — it is O(node count) — so allocating it per query would
/// dominate a small search entirely.
///
/// `thread_local` rather than a member, because `search()` is `const` and must
/// be safe to call concurrently: shared mutable scratch would be a data race.
/// Per-thread scratch gives each searcher its own without any locking.
///
/// The cost of this choice, stated plainly: the buffers persist for the life of
/// the thread, and a thread that searches two differently-sized indexes keeps
/// the larger visited set. For a search path that is the right trade.
struct SearchScratch {
    VisitedSet visited;
    /// Frontier still to expand: a max-heap under CandidateBetter, so the root
    /// is the *worst* — we pop from the back after sorting. Kept as a vector
    /// with heap operations rather than a priority_queue so it can be reserved
    /// once and reused.
    std::vector<Candidate> candidates;
    /// The best `ef` found so far: a max-heap whose root is the worst kept.
    std::vector<Candidate> results;
    std::vector<Candidate> working;
    std::vector<LocalId> selected;
    std::vector<LocalId> entry_buffer;
};

SearchScratch& scratch_for_this_thread() {
    thread_local SearchScratch scratch;
    return scratch;
}

constexpr CandidateBetter kBetter{};

/// Orders candidates worst-first, so `push_heap` with it yields a min-heap by
/// distance — the frontier we always want to expand from the closest end.
struct CandidateWorse {
    [[nodiscard]] bool operator()(const Candidate& a, const Candidate& b) const noexcept {
        return kBetter(b, a);
    }
};

constexpr CandidateWorse kWorse{};

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HnswIndex::HnswIndex(VectorAccessor accessor,
                     Metric metric,
                     HnswConfig config,
                     const DistanceKernel& kernel)
    : accessor_(accessor),
      metric_(metric),
      config_(config),
      distance_(metric, kernel),
      rng_(config.seed) {
    config_.validate();
}

void HnswIndex::reserve(std::size_t count) {
    node_level_.reserve(count);
    layer0_links_.reserve(count * layer0_stride());
    upper_offsets_.reserve(count);
    // Roughly 1/(M-1) of nodes have upper layers, each averaging a little over
    // one upper layer. This is a hint, not a bound.
    upper_links_.reserve(count / config_.m * upper_stride() * 2);
}

// ---------------------------------------------------------------------------
// Link accessors
// ---------------------------------------------------------------------------

const LocalId* HnswIndex::links(LocalId node, std::size_t layer) const noexcept {
    if (layer == 0) {
        return layer0_links_.data() + static_cast<std::size_t>(node) * layer0_stride();
    }
    const std::uint32_t base = upper_offsets_[node];
    return upper_links_.data() + base + (layer - 1) * upper_stride();
}

LocalId* HnswIndex::mutable_links(LocalId node, std::size_t layer) noexcept {
    return const_cast<LocalId*>(links(node, layer));
}

std::span<const LocalId> HnswIndex::neighbours(LocalId node, std::size_t layer) const noexcept {
    if (static_cast<std::size_t>(node) >= node_level_.size()) {
        return {};
    }
    if (layer > node_level_[node]) {
        return {};
    }
    const LocalId* block = links(node, layer);
    return {block + 1, static_cast<std::size_t>(block[0])};
}

void HnswIndex::set_neighbours(LocalId node,
                               std::size_t layer,
                               std::span<const LocalId> ids) noexcept {
    LocalId* block = mutable_links(node, layer);
    block[0] = static_cast<LocalId>(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        block[i + 1] = ids[i];
    }
}

// ---------------------------------------------------------------------------
// Level assignment
// ---------------------------------------------------------------------------

std::size_t HnswIndex::random_level() noexcept {
    // level = floor(-ln(U) * mL), which is an exponential distribution.
    //
    // With mL = 1/ln(M), the probability of reaching layer L is M^-L: each
    // layer holds about 1/M of the one below it. That geometric thinning is
    // what makes the tower of layers logarithmically deep, and it is why the
    // descent costs O(log N) rather than O(N).
    float u = rng_.uniform();
    if (u <= 0.0F) {
        u = std::numeric_limits<float>::min();
    }
    const double level = -std::log(static_cast<double>(u)) * config_.level_multiplier();
    // Clamped so a freak draw cannot exceed what u8 can store, and so the
    // tower cannot become taller than the graph is wide.
    return static_cast<std::size_t>(std::min(level, 254.0));
}

std::uint64_t HnswIndex::rng_state() const noexcept {
    // The RNG is copied so its state can be inspected without advancing it.
    DeterministicRng copy = rng_;
    return copy.next();
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

LocalId HnswIndex::greedy_descend(VectorView query,
                                  float query_norm,
                                  LocalId entry,
                                  std::size_t layer) const noexcept {
    LocalId current = entry;
    float current_distance = distance_.with_norms(
        query, query_norm, accessor_.vector(current), accessor_.norm(current));

    bool improved = true;
    while (improved) {
        improved = false;
        // Iterative, never recursive: a graph walk can be arbitrarily long and
        // recursion here would be a stack overflow waiting for a large index.
        const std::span<const LocalId> candidates = neighbours(current, layer);
        for (const LocalId neighbour : candidates) {
            const float d = distance_.with_norms(
                query, query_norm, accessor_.vector(neighbour), accessor_.norm(neighbour));
            if (d < current_distance) {
                current_distance = d;
                current = neighbour;
                improved = true;
            }
        }
    }
    return current;
}

void HnswIndex::search_layer(VectorView query,
                             float query_norm,
                             std::span<const LocalId> entry_points,
                             std::size_t ef,
                             std::size_t layer,
                             std::vector<Candidate>& out) const {
    SearchScratch& scratch = scratch_for_this_thread();
    scratch.visited.resize(node_level_.size());
    scratch.visited.begin_search();

    std::vector<Candidate>& frontier = scratch.candidates;
    std::vector<Candidate>& best = scratch.results;
    frontier.clear();
    best.clear();

    for (const LocalId entry : entry_points) {
        if (!scratch.visited.visit(entry)) {
            continue;
        }
        const float d = distance_.with_norms(
            query, query_norm, accessor_.vector(entry), accessor_.norm(entry));
        frontier.push_back(Candidate{entry, d});
        std::push_heap(frontier.begin(), frontier.end(), kWorse);
        best.push_back(Candidate{entry, d});
        std::push_heap(best.begin(), best.end(), kBetter);
    }

    while (!frontier.empty()) {
        // Closest unexpanded node. `frontier` is a min-heap by distance.
        std::pop_heap(frontier.begin(), frontier.end(), kWorse);
        const Candidate current = frontier.back();
        frontier.pop_back();

        // The termination condition, and the reason this is not a full BFS:
        // once the nearest thing left to explore is worse than the worst result
        // we are already keeping, nothing reachable through it can improve the
        // answer. Greedy search relies on the graph's small-world property for
        // this to be true often enough.
        if (best.size() >= ef && kBetter(best.front(), current)) {
            break;
        }

        for (const LocalId neighbour : neighbours(current.local_id, layer)) {
            if (!scratch.visited.visit(neighbour)) {
                continue;
            }

            const float d = distance_.with_norms(
                query, query_norm, accessor_.vector(neighbour), accessor_.norm(neighbour));

            if (best.size() < ef || d < best.front().key) {
                frontier.push_back(Candidate{neighbour, d});
                std::push_heap(frontier.begin(), frontier.end(), kWorse);

                best.push_back(Candidate{neighbour, d});
                std::push_heap(best.begin(), best.end(), kBetter);
                if (best.size() > ef) {
                    std::pop_heap(best.begin(), best.end(), kBetter);
                    best.pop_back();
                }
            }
        }
    }

    out.assign(best.begin(), best.end());
    std::sort(out.begin(), out.end(), kBetter);
}

std::vector<Candidate> HnswIndex::search(VectorView query, const SearchParams& params) const {
    if (params.k == 0) {
        throw InvalidArgumentError("search requires k >= 1");
    }
    if (query.size() != static_cast<std::size_t>(accessor_.dimension())) {
        throw DimensionMismatchError(accessor_.dimension(),
                                     static_cast<Dimension>(query.size()));
    }
    if (entry_point_ == kInvalidLocalId) {
        return {};
    }

    // ef must be at least k, or the result list is smaller than what was asked
    // for and the answer is truncated for no reason the caller can see.
    const std::size_t ef =
        std::max(params.k, params.ef_search > 0 ? params.ef_search : config_.ef_search);

    const float query_norm = metric_ == Metric::kCosine ? l2_norm(query) : 0.0F;

    // Phase 1: descend the sparse upper layers with a width-1 greedy walk.
    // These layers exist only to get us into the right neighbourhood cheaply;
    // spending a wide search on them would be paying for precision we discard.
    LocalId entry = entry_point_;
    for (std::size_t layer = max_level_; layer > 0; --layer) {
        entry = greedy_descend(query, query_norm, entry, layer);
    }

    // Phase 2: a wide best-first search of layer 0, which holds every node.
    SearchScratch& scratch = scratch_for_this_thread();
    std::vector<Candidate>& found = scratch.working;
    scratch.entry_buffer.assign(1, entry);
    search_layer(query, query_norm, scratch.entry_buffer, ef, 0, found);

    // Tombstoned nodes still route traffic but are never returned, so filtering
    // happens here rather than during traversal. See ADR-0011.
    std::vector<Candidate> results;
    results.reserve(std::min(params.k, found.size()));
    for (const Candidate& candidate : found) {
        if (results.size() >= params.k) {
            break;
        }
        if (accessor_.live(candidate.local_id)) {
            results.push_back(candidate);
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// Neighbour selection
// ---------------------------------------------------------------------------

void HnswIndex::select_neighbours(std::vector<Candidate>& candidates,
                                  std::size_t limit,
                                  std::vector<LocalId>& out) const {
    out.clear();
    if (candidates.size() <= limit) {
        for (const Candidate& candidate : candidates) {
            out.push_back(candidate.local_id);
        }
        return;
    }

    std::sort(candidates.begin(), candidates.end(), kBetter);

    // The heuristic that makes HNSW work, and the part most often replaced by
    // "just take the M closest" — which produces a measurably worse graph.
    //
    // Accept a candidate e only if it is closer to the target than to anything
    // already accepted. If some already-chosen r is closer to e than the target
    // is, then e is reachable *through* r: the edge to e would be redundant,
    // pointing into a direction already covered.
    //
    // The effect is a diverse neighbourhood — edges fanning out in different
    // directions rather than clustering on one side. That diversity is what
    // keeps the graph navigable: greedy search needs an edge pointing roughly
    // towards wherever the query is, and taking the M closest tends to give M
    // edges pointing the same way.
    for (const Candidate& candidate : candidates) {
        if (out.size() >= limit) {
            break;
        }

        const VectorView candidate_vector = accessor_.vector(candidate.local_id);
        const float candidate_norm = accessor_.norm(candidate.local_id);

        bool dominated = false;
        for (const LocalId chosen : out) {
            const float to_chosen = distance_.with_norms(candidate_vector,
                                                         candidate_norm,
                                                         accessor_.vector(chosen),
                                                         accessor_.norm(chosen));
            if (to_chosen < candidate.key) {
                dominated = true;
                break;
            }
        }
        if (!dominated) {
            out.push_back(candidate.local_id);
        }
    }

    // If the heuristic was strict enough to leave the list short, fill from the
    // rejects in distance order. An under-filled neighbour list wastes budget
    // that is already paid for in memory, and a sparser graph is a slower one.
    if (out.size() < limit) {
        for (const Candidate& candidate : candidates) {
            if (out.size() >= limit) {
                break;
            }
            if (std::find(out.begin(), out.end(), candidate.local_id) == out.end()) {
                out.push_back(candidate.local_id);
            }
        }
    }
}

void HnswIndex::link(LocalId from, LocalId to, std::size_t layer) {
    LocalId* block = mutable_links(from, layer);
    const std::size_t limit = config_.max_neighbours(layer);
    const auto count = static_cast<std::size_t>(block[0]);

    if (count < limit) {
        block[count + 1] = to;
        block[0] = static_cast<LocalId>(count + 1);
        return;
    }

    // Full. Re-run the heuristic over the existing neighbours plus the new one
    // and keep the best `limit`. Simply dropping the new edge would let early
    // insertions monopolise a node's budget; simply evicting the furthest would
    // lose the diversity the heuristic is protecting.
    std::vector<Candidate> candidates;
    candidates.reserve(limit + 1);

    const VectorView from_vector = accessor_.vector(from);
    const float from_norm = accessor_.norm(from);
    for (std::size_t i = 0; i < count; ++i) {
        const LocalId existing = block[i + 1];
        candidates.push_back(Candidate{
            existing,
            distance_.with_norms(
                from_vector, from_norm, accessor_.vector(existing), accessor_.norm(existing))});
    }
    candidates.push_back(
        Candidate{to,
                  distance_.with_norms(
                      from_vector, from_norm, accessor_.vector(to), accessor_.norm(to))});

    std::vector<LocalId> kept;
    select_neighbours(candidates, limit, kept);
    set_neighbours(from, layer, kept);
}

// ---------------------------------------------------------------------------
// Insertion
// ---------------------------------------------------------------------------

void HnswIndex::add(LocalId local_id) {
    const std::size_t expected = node_level_.size();
    if (static_cast<std::size_t>(local_id) != expected) {
        throw InvalidArgumentError(
            "hnsw: nodes must be added in ascending slot order (expected " +
            std::to_string(expected) + ", got " + std::to_string(local_id) + ")");
    }
    if (static_cast<std::size_t>(local_id) >= accessor_.size()) {
        throw InvalidArgumentError("hnsw: slot " + std::to_string(local_id) +
                                   " is not present in the vector store");
    }

    const std::size_t level = random_level();

    node_level_.push_back(static_cast<std::uint8_t>(level));
    layer0_links_.resize(layer0_links_.size() + layer0_stride(), kInvalidLocalId);
    mutable_links(local_id, 0)[0] = 0;

    if (level > 0) {
        upper_offsets_.push_back(static_cast<std::uint32_t>(upper_links_.size()));
        upper_links_.resize(upper_links_.size() + level * upper_stride(), kInvalidLocalId);
        for (std::size_t layer = 1; layer <= level; ++layer) {
            mutable_links(local_id, layer)[0] = 0;
        }
    } else {
        upper_offsets_.push_back(kNoUpperLinks);
    }

    ++live_count_;

    if (entry_point_ == kInvalidLocalId) {
        entry_point_ = local_id;
        max_level_ = level;
        return;
    }

    const VectorView vector = accessor_.vector(local_id);
    const float norm = accessor_.norm(local_id);

    // Phase 1: greedy descent through layers this node does not occupy, just to
    // find a good entry point for the layers it does.
    LocalId entry = entry_point_;
    for (std::size_t layer = max_level_; layer > level; --layer) {
        entry = greedy_descend(vector, norm, entry, layer);
    }

    // Phase 2: on each layer the node occupies, find efConstruction candidates,
    // pick neighbours from them, and wire the edges both ways.
    std::vector<Candidate> found;
    std::vector<LocalId> chosen;
    std::vector<LocalId> entry_points{entry};

    const std::size_t start = std::min(level, max_level_);
    for (std::size_t layer = start + 1; layer-- > 0;) {
        search_layer(vector, norm, entry_points, config_.ef_construction, layer, found);
        select_neighbours(found, config_.max_neighbours(layer), chosen);

        set_neighbours(local_id, layer, chosen);
        for (const LocalId neighbour : chosen) {
            // The reverse edge is what makes the new node reachable. Without
            // it the node would be findable only by luck, and recall for
            // recently-inserted vectors would silently collapse.
            link(neighbour, local_id, layer);
        }

        // The candidates found on this layer become the entry points for the
        // next one down. Descending from many entry points rather than one
        // costs almost nothing — they are already visited — and makes the
        // lower-layer search far less likely to start in the wrong basin.
        entry_points.clear();
        entry_points.reserve(found.size());
        for (const Candidate& candidate : found) {
            entry_points.push_back(candidate.local_id);
        }
    }

    if (level > max_level_) {
        max_level_ = level;
        entry_point_ = local_id;
    }
}

void HnswIndex::remove(LocalId local_id) {
    if (static_cast<std::size_t>(local_id) >= node_level_.size()) {
        return;
    }
    // The node stays in the graph and keeps routing traffic; search filters it
    // out of the results. See the class comment and ADR-0011.
    if (live_count_ > 0) {
        --live_count_;
    }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::size_t HnswIndex::index_bytes() const noexcept {
    return node_level_.capacity() * sizeof(std::uint8_t) +
           layer0_links_.capacity() * sizeof(LocalId) +
           upper_links_.capacity() * sizeof(LocalId) +
           upper_offsets_.capacity() * sizeof(std::uint32_t);
}

std::string HnswIndex::describe() const {
    return "hnsw(" + config_.describe() + ", metric=" + std::string(metric_name(metric_)) +
           ", kernel=" + std::string(distance_.kernel().name) +
           ", layers=" + std::to_string(max_level_ + 1) + ")";
}

void HnswIndex::restore(std::vector<std::uint8_t> node_levels,
                        std::vector<LocalId> layer0_links,
                        std::vector<LocalId> upper_links,
                        std::vector<std::uint32_t> upper_offsets,
                        LocalId entry_point,
                        std::size_t max_level,
                        std::size_t live_count,
                        std::uint64_t rng_state) {
    node_level_ = std::move(node_levels);
    layer0_links_ = std::move(layer0_links);
    upper_links_ = std::move(upper_links);
    upper_offsets_ = std::move(upper_offsets);
    entry_point_ = entry_point;
    max_level_ = max_level;
    live_count_ = live_count;
    rng_ = DeterministicRng(rng_state);
}

}  // namespace vectordb
