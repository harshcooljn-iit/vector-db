// SPDX-License-Identifier: MIT
//
// HNSW is approximate, so these tests assert *structural invariants* exactly
// and *result quality* against a threshold. Demanding an exact result set from
// an approximate algorithm produces a test that is either trivially weak or
// permanently flaky.
#include <algorithm>
#include <set>
#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/index/brute_force_index.hpp>
#include <vectordb/index/hnsw_index.hpp>
#include <vectordb/storage/vector_store.hpp>
#include <vectordb/util/dataset.hpp>

#include "support/recall.hpp"

namespace vectordb {
namespace {

using testing::mean_recall;
using testing::recall_at_k;

/// Builds a store and an HNSW index over generated data.
struct Fixture {
    Fixture(DatasetSpec spec, Metric metric, HnswConfig config)
        : data(generate_dataset(spec)), store(spec.dimension) {
        store.reserve(spec.count);
        for (LocalId id = 0; id < data.size(); ++id) {
            store.insert(static_cast<VectorId>(id), data[id]);
        }
        index = std::make_unique<HnswIndex>(store.accessor(), metric, config, scalar_kernel());
        index->reserve(spec.count);
        for (LocalId id = 0; id < data.size(); ++id) {
            index->add(id);
        }
        oracle = std::make_unique<BruteForceIndex>(store.accessor(), metric, scalar_kernel());
    }

    VectorArray data;
    VectorStore store;
    std::unique_ptr<HnswIndex> index;
    std::unique_ptr<BruteForceIndex> oracle;
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

TEST(HnswConfig, RejectsParametersThatWouldProduceABrokenGraph) {
    EXPECT_THROW(HnswConfig{.m = 1}.validate(), InvalidArgumentError);
    EXPECT_THROW(HnswConfig{.m = 1000}.validate(), InvalidArgumentError);
    EXPECT_THROW((HnswConfig{.m = 16, .ef_construction = 0}).validate(), InvalidArgumentError);
    EXPECT_THROW((HnswConfig{.m = 16, .ef_construction = 8}).validate(), InvalidArgumentError)
        << "efConstruction below M cannot fill a neighbour list";
    EXPECT_THROW((HnswConfig{.m = 16, .ef_construction = 200, .ef_search = 0}).validate(),
                 InvalidArgumentError);
    EXPECT_NO_THROW(HnswConfig{}.validate());
}

TEST(HnswConfig, LayerZeroGetsTwiceTheNeighbourBudget) {
    const HnswConfig config{.m = 16};
    EXPECT_EQ(config.max_m0(), 32U);
    EXPECT_EQ(config.max_neighbours(0), 32U);
    EXPECT_EQ(config.max_neighbours(1), 16U);
    EXPECT_EQ(config.max_neighbours(5), 16U);
}

TEST(HnswConfig, LevelMultiplierIsOneOverLogM) {
    EXPECT_NEAR(HnswConfig{.m = 16}.level_multiplier(), 1.0 / std::log(16.0), 1e-12);
}

// ---------------------------------------------------------------------------
// Degenerate cases
// ---------------------------------------------------------------------------

TEST(HnswIndex, AnEmptyIndexReturnsNothing) {
    const VectorStore store(8);
    const HnswIndex index(store.accessor(), Metric::kL2Squared, HnswConfig{});

    EXPECT_EQ(index.size(), 0U);
    EXPECT_EQ(index.node_count(), 0U);
    EXPECT_EQ(index.entry_point(), kInvalidLocalId);
    EXPECT_TRUE(index.search(std::vector<float>(8, 0.0F), SearchParams{.k = 5}).empty());
}

TEST(HnswIndex, ASingleVectorIsAlwaysFound) {
    VectorStore store(4);
    store.insert(1, std::vector<float>{1.0F, 2.0F, 3.0F, 4.0F});

    HnswIndex index(store.accessor(), Metric::kL2Squared, HnswConfig{});
    index.add(0);

    EXPECT_EQ(index.size(), 1U);
    EXPECT_EQ(index.entry_point(), 0U);

    const auto results =
        index.search(std::vector<float>{1.0F, 2.0F, 3.0F, 4.0F}, SearchParams{.k = 5});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].local_id, 0U);
    EXPECT_NEAR(results[0].key, 0.0F, 1e-6F);
}

TEST(HnswIndex, RejectsOutOfOrderAdds) {
    VectorStore store(2);
    store.insert(1, std::vector<float>{1.0F, 0.0F});
    store.insert(2, std::vector<float>{0.0F, 1.0F});

    HnswIndex index(store.accessor(), Metric::kL2Squared, HnswConfig{});
    // Slots must arrive in ascending order; an index may size its arrays on
    // that assumption rather than carrying a map.
    EXPECT_THROW(index.add(1), InvalidArgumentError);
    EXPECT_NO_THROW(index.add(0));
    EXPECT_THROW(index.add(0), InvalidArgumentError);
}

TEST(HnswIndex, RejectsBadSearchArguments) {
    VectorStore store(4);
    store.insert(1, std::vector<float>(4, 1.0F));
    HnswIndex index(store.accessor(), Metric::kL2Squared, HnswConfig{});
    index.add(0);

    EXPECT_THROW(
        static_cast<void>(index.search(std::vector<float>(4, 0.0F), SearchParams{.k = 0})),
        InvalidArgumentError);
    EXPECT_THROW(
        static_cast<void>(index.search(std::vector<float>(3, 0.0F), SearchParams{.k = 1})),
        DimensionMismatchError);
}

// ---------------------------------------------------------------------------
// Graph structure invariants — these must hold exactly
// ---------------------------------------------------------------------------

TEST(HnswGraph, EveryNodeAppearsOnLayerZero) {
    const Fixture fixture({.dimension = 16, .count = 500, .seed = 7},
                          Metric::kL2Squared,
                          HnswConfig{.m = 8, .ef_construction = 50});

    ASSERT_EQ(fixture.index->node_count(), 500U);
    for (LocalId node = 0; node < 500; ++node) {
        // Every node must have at least one layer-0 edge, or it is unreachable
        // and its vector can never be returned.
        EXPECT_FALSE(fixture.index->neighbours(node, 0).empty())
            << "node " << node << " is isolated on layer 0";
    }
}

TEST(HnswGraph, NeighbourListsNeverExceedTheirBudget) {
    const HnswConfig config{.m = 8, .ef_construction = 60};
    const Fixture fixture(
        {.dimension = 16, .count = 600, .seed = 11}, Metric::kL2Squared, config);

    for (LocalId node = 0; node < fixture.index->node_count(); ++node) {
        const std::size_t level = fixture.index->node_levels()[node];
        for (std::size_t layer = 0; layer <= level; ++layer) {
            const std::size_t count = fixture.index->neighbours(node, layer).size();
            EXPECT_LE(count, config.max_neighbours(layer))
                << "node " << node << " layer " << layer;
        }
    }
}

TEST(HnswGraph, EveryNeighbourReferenceIsValid) {
    const Fixture fixture({.dimension = 8, .count = 400, .seed = 3},
                          Metric::kL2Squared,
                          HnswConfig{.m = 8, .ef_construction = 50});
    const std::size_t nodes = fixture.index->node_count();

    for (LocalId node = 0; node < nodes; ++node) {
        const std::size_t level = fixture.index->node_levels()[node];
        for (std::size_t layer = 0; layer <= level; ++layer) {
            std::set<LocalId> seen;
            for (const LocalId neighbour : fixture.index->neighbours(node, layer)) {
                EXPECT_LT(neighbour, nodes) << "dangling reference from node " << node;
                EXPECT_NE(neighbour, node) << "node " << node << " links to itself";
                EXPECT_TRUE(seen.insert(neighbour).second)
                    << "node " << node << " lists neighbour " << neighbour << " twice";
                // A neighbour on layer L must itself reach layer L, or the edge
                // points into empty storage.
                EXPECT_GE(fixture.index->node_levels()[neighbour], layer)
                    << "node " << node << " links on layer " << layer
                    << " to a node that does not reach it";
            }
        }
    }
}

TEST(HnswGraph, TheEntryPointIsTheHighestNode) {
    const Fixture fixture({.dimension = 8, .count = 800, .seed = 5},
                          Metric::kL2Squared,
                          HnswConfig{.m = 8, .ef_construction = 50});

    const std::size_t max_level = fixture.index->max_level();
    EXPECT_EQ(fixture.index->node_levels()[fixture.index->entry_point()], max_level);
    for (const std::uint8_t level : fixture.index->node_levels()) {
        EXPECT_LE(level, max_level);
    }
}

// The layer distribution is what makes the descent logarithmic. If it drifts,
// search silently becomes linear.
TEST(HnswGraph, LayerPopulationsThinGeometricallyByRoughlyM) {
    constexpr std::size_t kCount = 20000;
    const HnswConfig config{.m = 16, .ef_construction = 20};
    const Fixture fixture(
        {.dimension = 4, .count = kCount, .seed = 42}, Metric::kL2Squared, config);

    std::vector<std::size_t> population(fixture.index->max_level() + 1, 0);
    for (const std::uint8_t level : fixture.index->node_levels()) {
        for (std::size_t layer = 0; layer <= level; ++layer) {
            ++population[layer];
        }
    }

    EXPECT_EQ(population[0], kCount) << "layer 0 holds every node";
    ASSERT_GE(population.size(), 3U) << "20k nodes should produce several layers";

    for (std::size_t layer = 1; layer + 1 < population.size(); ++layer) {
        const double ratio =
            static_cast<double>(population[layer]) / static_cast<double>(population[layer - 1]);
        // Expected 1/M = 0.0625. Loose bounds: this is a random process, and a
        // tight assertion here would be flaky for no benefit.
        EXPECT_GT(ratio, 0.01) << "layer " << layer << " thinned far too fast";
        EXPECT_LT(ratio, 0.25) << "layer " << layer << " did not thin enough";
    }
}

// ---------------------------------------------------------------------------
// Recall against brute force — the reason brute force exists
// ---------------------------------------------------------------------------

TEST(HnswRecall, AchievesHighRecallOnClusteredData) {
    const DatasetSpec spec{.dimension = 64, .count = 5000, .seed = 20260829};
    const Fixture fixture(spec,
                          Metric::kL2Squared,
                          HnswConfig{.m = 16, .ef_construction = 200, .ef_search = 100});
    const VectorArray queries = generate_queries(spec, 50);

    const double recall = mean_recall(*fixture.index, *fixture.oracle, queries, 10);
    // Measured at 1.000 on this configuration. The threshold is set just below
    // that rather than at a comfortable 0.95, because a test that passes at
    // 0.75 would not have caught the query-generation bug that produced exactly
    // 0.75 here — see the note in generate_queries().
    EXPECT_GT(recall, 0.99) << "recall@10 = " << recall;
}

// At 5k vectors in 64 dimensions HNSW is effectively exact, which makes recall
// a weak signal. This test uses a deliberately starved configuration so the
// efSearch dial has something to move, and pins that it moves in the right
// direction by a meaningful amount.
TEST(HnswRecall, LowEfSearchCostsRecallOnAStarvedGraph) {
    const DatasetSpec spec{.dimension = 128, .count = 8000, .seed = 555};
    const Fixture fixture(spec, Metric::kL2Squared, HnswConfig{.m = 4, .ef_construction = 10});
    const VectorArray queries = generate_queries(spec, 50);

    const double starved = mean_recall(*fixture.index, *fixture.oracle, queries, 10, 5);
    const double generous = mean_recall(*fixture.index, *fixture.oracle, queries, 10, 400);

    EXPECT_LT(starved, 0.95) << "efSearch=5 should visibly cost recall: " << starved;
    EXPECT_GT(generous, starved + 0.05)
        << "efSearch=400 gave " << generous << ", efSearch=5 gave " << starved;
}

// Raising efSearch must raise recall. This is the property the entire
// latency/quality trade rests on; if it stops holding, the dial is broken.
TEST(HnswRecall, RisesMonotonicallyWithEfSearch) {
    const DatasetSpec spec{.dimension = 32, .count = 4000, .seed = 99};
    const Fixture fixture(spec, Metric::kL2Squared, HnswConfig{.m = 8, .ef_construction = 100});
    const VectorArray queries = generate_queries(spec, 40);

    double previous = 0.0;
    for (const std::size_t ef : {10U, 20U, 50U, 100U, 200U}) {
        const double recall = mean_recall(*fixture.index, *fixture.oracle, queries, 10, ef);
        EXPECT_GE(recall, previous - 0.02) << "recall fell when efSearch rose to " << ef << " ("
                                           << recall << " after " << previous << ")";
        previous = std::max(previous, recall);
    }
    EXPECT_GT(previous, 0.93) << "high efSearch should approach exactness";
}

TEST(HnswRecall, WorksForEveryMetric) {
    const DatasetSpec spec{.dimension = 32, .count = 3000, .seed = 17};
    const VectorArray queries = generate_queries(spec, 30);

    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine}) {
        const Fixture fixture(
            spec, metric, HnswConfig{.m = 16, .ef_construction = 200, .ef_search = 100});
        const double recall = mean_recall(*fixture.index, *fixture.oracle, queries, 10);
        EXPECT_GT(recall, 0.98) << "metric " << metric_name(metric) << ", recall " << recall;
    }
}

// Inner product is not a metric — a vector is not its own nearest neighbour —
// so HNSW's assumptions are weaker and recall is expected to be lower. Stating
// that with a test is more useful than pretending otherwise.
TEST(HnswRecall, IsWeakerForInnerProductWhichIsNotAMetric) {
    const DatasetSpec spec{.dimension = 32, .count = 3000, .seed = 17};
    const VectorArray queries = generate_queries(spec, 30);
    const Fixture fixture(spec,
                          Metric::kInnerProduct,
                          HnswConfig{.m = 16, .ef_construction = 200, .ef_search = 200});

    const double recall = mean_recall(*fixture.index, *fixture.oracle, queries, 10);
    EXPECT_GT(recall, 0.40) << "recall " << recall << " — low is expected here, but not zero";
}

TEST(HnswRecall, HigherMImprovesRecallAtTheSameEfSearch) {
    const DatasetSpec spec{.dimension = 48, .count = 4000, .seed = 4242};
    const VectorArray queries = generate_queries(spec, 40);

    const Fixture small(
        spec, Metric::kL2Squared, HnswConfig{.m = 4, .ef_construction = 100, .ef_search = 32});
    const Fixture large(
        spec, Metric::kL2Squared, HnswConfig{.m = 32, .ef_construction = 100, .ef_search = 32});

    const double small_recall = mean_recall(*small.index, *small.oracle, queries, 10);
    const double large_recall = mean_recall(*large.index, *large.oracle, queries, 10);

    EXPECT_GT(large_recall, small_recall)
        << "M=32 gave " << large_recall << ", M=4 gave " << small_recall;
    EXPECT_GT(large.index->index_bytes(), small.index->index_bytes())
        << "and the better recall must have been paid for in memory";
}

TEST(HnswRecall, ReturnsExactResultsOnATinyIndex) {
    // With fewer vectors than efSearch, the search visits everything, so the
    // answer must be exactly the brute-force answer.
    const DatasetSpec spec{.dimension = 8, .count = 30, .seed = 1};
    const Fixture fixture(
        spec, Metric::kL2Squared, HnswConfig{.m = 8, .ef_construction = 50, .ef_search = 100});
    const VectorArray queries = generate_queries(spec, 10);

    EXPECT_DOUBLE_EQ(mean_recall(*fixture.index, *fixture.oracle, queries, 5), 1.0);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST(HnswIndex, TheSameSeedBuildsTheSameGraph) {
    const DatasetSpec spec{.dimension = 16, .count = 500, .seed = 8};
    const HnswConfig config{.m = 8, .ef_construction = 50, .seed = 1234};

    const Fixture a(spec, Metric::kL2Squared, config);
    const Fixture b(spec, Metric::kL2Squared, config);

    ASSERT_EQ(a.index->node_count(), b.index->node_count());
    EXPECT_EQ(a.index->entry_point(), b.index->entry_point());
    EXPECT_EQ(a.index->max_level(), b.index->max_level());

    for (LocalId node = 0; node < a.index->node_count(); ++node) {
        ASSERT_EQ(a.index->node_levels()[node], b.index->node_levels()[node])
            << "node " << node;
        for (std::size_t layer = 0; layer <= a.index->node_levels()[node]; ++layer) {
            const auto left = a.index->neighbours(node, layer);
            const auto right = b.index->neighbours(node, layer);
            ASSERT_EQ(left.size(), right.size()) << "node " << node << " layer " << layer;
            for (std::size_t i = 0; i < left.size(); ++i) {
                EXPECT_EQ(left[i], right[i]) << "node " << node << " layer " << layer;
            }
        }
    }
}

TEST(HnswIndex, DifferentSeedsBuildDifferentGraphs) {
    const DatasetSpec spec{.dimension = 16, .count = 500, .seed = 8};
    const Fixture a(
        spec, Metric::kL2Squared, HnswConfig{.m = 8, .ef_construction = 50, .seed = 1});
    const Fixture b(
        spec, Metric::kL2Squared, HnswConfig{.m = 8, .ef_construction = 50, .seed = 2});

    bool differs = false;
    for (LocalId node = 0; node < a.index->node_count() && !differs; ++node) {
        if (a.index->node_levels()[node] != b.index->node_levels()[node]) {
            differs = true;
        }
    }
    EXPECT_TRUE(differs) << "level assignment must depend on the seed";
}

TEST(HnswIndex, SearchIsRepeatable) {
    const DatasetSpec spec{.dimension = 16, .count = 800, .seed = 6};
    const Fixture fixture(spec, Metric::kL2Squared, HnswConfig{.m = 8});
    const VectorView query = generate_queries(spec, 1)[0];

    const auto first = fixture.index->search(query, SearchParams{.k = 10});
    for (int trial = 0; trial < 5; ++trial) {
        const auto again = fixture.index->search(query, SearchParams{.k = 10});
        ASSERT_EQ(again.size(), first.size());
        for (std::size_t i = 0; i < first.size(); ++i) {
            EXPECT_EQ(again[i].local_id, first[i].local_id) << "position " << i;
        }
    }
}

// ---------------------------------------------------------------------------
// Deletion
// ---------------------------------------------------------------------------

TEST(HnswIndex, NeverReturnsATombstonedVector) {
    const DatasetSpec spec{.dimension = 16, .count = 1000, .seed = 21};
    Fixture fixture(spec,
                    Metric::kL2Squared,
                    HnswConfig{.m = 16, .ef_construction = 100, .ef_search = 100});

    // Delete the exact nearest neighbours of a query, then confirm none comes
    // back — the case a naive implementation gets wrong.
    const VectorView query = generate_queries(spec, 1)[0];
    const auto before = fixture.index->search(query, SearchParams{.k = 5});
    ASSERT_EQ(before.size(), 5U);

    std::unordered_set<LocalId> deleted;
    for (const Candidate& candidate : before) {
        ASSERT_TRUE(fixture.store.remove(fixture.store.vector_id(candidate.local_id)));
        fixture.index->remove(candidate.local_id);
        deleted.insert(candidate.local_id);
    }
    fixture.index->set_accessor(fixture.store.accessor());

    const auto after = fixture.index->search(query, SearchParams{.k = 5});
    EXPECT_EQ(after.size(), 5U) << "the search must still fill k from live vectors";
    for (const Candidate& candidate : after) {
        EXPECT_EQ(deleted.count(candidate.local_id), 0U) << "returned a tombstoned vector";
    }
    EXPECT_EQ(fixture.index->size(), 995U);
}

// A tombstoned node keeps routing traffic. This test deletes a large fraction
// and checks that recall over what remains is still good — which is the reason
// the node is kept in the graph rather than cut out of it.
TEST(HnswIndex, RecallSurvivesHeavyDeletionBecauseTombstonesStillRoute) {
    const DatasetSpec spec{.dimension = 32, .count = 3000, .seed = 31};
    Fixture fixture(spec,
                    Metric::kL2Squared,
                    HnswConfig{.m = 16, .ef_construction = 200, .ef_search = 200});

    for (VectorId id = 0; id < 3000; id += 2) {
        ASSERT_TRUE(fixture.store.remove(id));
        fixture.index->remove(static_cast<LocalId>(id));
    }
    fixture.index->set_accessor(fixture.store.accessor());
    const BruteForceIndex oracle(fixture.store.accessor(), Metric::kL2Squared, scalar_kernel());

    const VectorArray queries = generate_queries(spec, 30);
    const double recall = mean_recall(*fixture.index, oracle, queries, 10);

    EXPECT_EQ(fixture.index->size(), 1500U);
    EXPECT_GT(recall, 0.85) << "recall after deleting half the vectors: " << recall;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

TEST(HnswIndex, ReportsItsConfigurationAndFootprint) {
    const Fixture fixture({.dimension = 16, .count = 200, .seed = 2},
                          Metric::kCosine,
                          HnswConfig{.m = 12, .ef_construction = 80, .ef_search = 40});

    const std::string description = fixture.index->describe();
    EXPECT_NE(description.find("hnsw"), std::string::npos);
    EXPECT_NE(description.find("M=12"), std::string::npos);
    EXPECT_NE(description.find("efConstruction=80"), std::string::npos);
    EXPECT_NE(description.find("cosine"), std::string::npos);

    EXPECT_EQ(fixture.index->type(), IndexType::kHnsw);
    EXPECT_EQ(fixture.index->metric(), Metric::kCosine);
    // Unlike brute force, HNSW genuinely costs memory beyond the vectors.
    EXPECT_GT(fixture.index->index_bytes(), 200U * 12U * sizeof(LocalId));
}

}  // namespace
}  // namespace vectordb
