// SPDX-License-Identifier: MIT
//
// Persisting a graph, and every way a persisted graph can be wrong.
//
// The corruption tests matter more than the round-trip ones. A graph that
// round-trips is table stakes; a graph that is *validated* is what stops a
// damaged file becoming an out-of-bounds read in the search loop.
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/index/hnsw_file.hpp>
#include <vectordb/storage/vector_store.hpp>
#include <vectordb/util/dataset.hpp>

#include "support/recall.hpp"
#include "support/temp_dir.hpp"

namespace vectordb {
namespace {

using testing::TempDir;

struct Built {
    explicit Built(DatasetSpec spec,
                   HnswConfig config = HnswConfig{.m = 8, .ef_construction = 60},
                   Metric metric = Metric::kL2Squared)
        : data(generate_dataset(spec)), store(spec.dimension), dimension(spec.dimension) {
        store.reserve(spec.count);
        for (LocalId id = 0; id < data.size(); ++id) {
            store.insert(static_cast<VectorId>(id), data[id]);
        }
        index = std::make_unique<HnswIndex>(store.accessor(), metric, config, scalar_kernel());
        for (LocalId id = 0; id < data.size(); ++id) {
            index->add(id);
        }
    }

    VectorArray data;
    VectorStore store;
    Dimension dimension;
    std::unique_ptr<HnswIndex> index;
};

void corrupt_byte(const std::filesystem::path& path,
                  std::streamoff offset,
                  std::uint8_t value) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(&value), 1);
    ASSERT_TRUE(file.good());
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST(HnswFile, RestoresTheGraphEdgeForEdge) {
    const TempDir dir;
    const auto path = dir.file("index.hnsw");

    Built built({.dimension = 16, .count = 800, .seed = 5});
    write_hnsw_file(path, *built.index, built.dimension);

    const auto loaded = read_hnsw_file(
        path, built.store.accessor(), Metric::kL2Squared, built.dimension, scalar_kernel());

    ASSERT_EQ(loaded->node_count(), built.index->node_count());
    EXPECT_EQ(loaded->entry_point(), built.index->entry_point());
    EXPECT_EQ(loaded->max_level(), built.index->max_level());
    EXPECT_EQ(loaded->size(), built.index->size());
    EXPECT_EQ(loaded->config().m, built.index->config().m);
    EXPECT_EQ(loaded->config().ef_construction, built.index->config().ef_construction);

    for (LocalId node = 0; node < loaded->node_count(); ++node) {
        ASSERT_EQ(loaded->node_levels()[node], built.index->node_levels()[node])
            << "node " << node;
        for (std::size_t layer = 0; layer <= loaded->node_levels()[node]; ++layer) {
            const auto before = built.index->neighbours(node, layer);
            const auto after = loaded->neighbours(node, layer);
            ASSERT_EQ(before.size(), after.size()) << "node " << node << " layer " << layer;
            for (std::size_t i = 0; i < before.size(); ++i) {
                EXPECT_EQ(before[i], after[i]) << "node " << node << " layer " << layer;
            }
        }
    }
}

TEST(HnswFile, ALoadedIndexAnswersIdenticallyToTheOriginal) {
    const TempDir dir;
    const auto path = dir.file("index.hnsw");
    const DatasetSpec spec{.dimension = 32, .count = 1500, .seed = 77};

    Built built(spec);
    write_hnsw_file(path, *built.index, built.dimension);
    const auto loaded = read_hnsw_file(
        path, built.store.accessor(), Metric::kL2Squared, built.dimension, scalar_kernel());

    const VectorArray queries = generate_queries(spec, 25);
    for (LocalId q = 0; q < queries.size(); ++q) {
        const auto original = built.index->search(queries[q], SearchParams{.k = 10});
        const auto restored = loaded->search(queries[q], SearchParams{.k = 10});
        ASSERT_EQ(original.size(), restored.size()) << "query " << q;
        for (std::size_t i = 0; i < original.size(); ++i) {
            EXPECT_EQ(original[i].local_id, restored[i].local_id)
                << "query " << q << " position " << i;
        }
    }
}

TEST(HnswFile, RoundTripsAnEmptyGraph) {
    const TempDir dir;
    const auto path = dir.file("index.hnsw");

    const VectorStore store(8);
    const HnswIndex empty(store.accessor(), Metric::kL2Squared, HnswConfig{});
    write_hnsw_file(path, empty, 8);

    const auto loaded =
        read_hnsw_file(path, store.accessor(), Metric::kL2Squared, 8, scalar_kernel());
    EXPECT_EQ(loaded->node_count(), 0U);
    EXPECT_EQ(loaded->entry_point(), kInvalidLocalId);
    EXPECT_TRUE(loaded->search(std::vector<float>(8, 0.0F), SearchParams{.k = 3}).empty());
}

TEST(HnswFile, PreservesTombstoneCounts) {
    const TempDir dir;
    const auto path = dir.file("index.hnsw");

    Built built({.dimension = 8, .count = 200, .seed = 3});
    for (VectorId id = 0; id < 50; ++id) {
        built.store.remove(id);
        built.index->remove(static_cast<LocalId>(id));
    }
    built.index->set_accessor(built.store.accessor());
    write_hnsw_file(path, *built.index, built.dimension);

    const auto loaded = read_hnsw_file(
        path, built.store.accessor(), Metric::kL2Squared, built.dimension, scalar_kernel());
    EXPECT_EQ(loaded->node_count(), 200U) << "tombstoned nodes stay in the graph";
    EXPECT_EQ(loaded->size(), 150U);
}

TEST(HnswFile, ReadsTheHeaderWithoutLoadingTheGraph) {
    const TempDir dir;
    const auto path = dir.file("index.hnsw");
    Built built({.dimension = 24, .count = 400, .seed = 9},
                HnswConfig{.m = 12, .ef_construction = 90, .ef_search = 45, .seed = 7});
    write_hnsw_file(path, *built.index, built.dimension);

    const HnswFileHeader header = read_hnsw_file_header(path);
    EXPECT_EQ(header.format_version, kHnswFileVersion);
    EXPECT_EQ(header.dimension, 24U);
    EXPECT_EQ(header.node_count, 400U);
    EXPECT_EQ(header.config.m, 12U);
    EXPECT_EQ(header.config.ef_construction, 90U);
    EXPECT_EQ(header.config.seed, 7U);
    EXPECT_EQ(header.metric, Metric::kL2Squared);
    EXPECT_EQ(header.total_size(), std::filesystem::file_size(path));
}

TEST(HnswFile, VerifiesThePayloadChecksum) {
    const TempDir dir;
    const auto path = dir.file("index.hnsw");
    Built built({.dimension = 8, .count = 300, .seed = 4});
    write_hnsw_file(path, *built.index, built.dimension);

    EXPECT_TRUE(verify_hnsw_file_payload(path));

    // Flip a bit inside a neighbour list. The reference may still be in range,
    // so structural validation can miss it — the checksum cannot.
    corrupt_byte(path, static_cast<std::streamoff>(kHnswFileHeaderSize) + 700, 0x01);
    EXPECT_FALSE(verify_hnsw_file_payload(path));
}

// ---------------------------------------------------------------------------
// Corruption
// ---------------------------------------------------------------------------

class HnswFileCorruption : public ::testing::Test {
protected:
    void SetUp() override {
        built = std::make_unique<Built>(DatasetSpec{.dimension = 8, .count = 300, .seed = 6});
        path = dir.file("index.hnsw");
        write_hnsw_file(path, *built->index, built->dimension);
    }

    void expect_corruption() {
        EXPECT_THROW(static_cast<void>(read_hnsw_file(path,
                                                      built->store.accessor(),
                                                      Metric::kL2Squared,
                                                      built->dimension,
                                                      scalar_kernel())),
                     CorruptionError);
    }

    TempDir dir;
    std::unique_ptr<Built> built;
    std::filesystem::path path;
};

TEST_F(HnswFileCorruption, RejectsAFileThatIsNotOurs) {
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "This is definitely not a VectorDB HNSW index file, but it is long "
                "enough that the reader reaches the magic check rather than the "
                "too-small check, which is the branch under test here.";
    }
    expect_corruption();
}

TEST_F(HnswFileCorruption, RejectsAnUnsupportedVersion) {
    corrupt_byte(path, 8, 42);
    EXPECT_THROW(static_cast<void>(read_hnsw_file(path,
                                                  built->store.accessor(),
                                                  Metric::kL2Squared,
                                                  built->dimension,
                                                  scalar_kernel())),
                 UnsupportedVersionError);
}

TEST_F(HnswFileCorruption, RejectsADamagedHeader) {
    corrupt_byte(path, 32, 0xFF);  // M
    expect_corruption();
}

TEST_F(HnswFileCorruption, RejectsTruncationAndTrailingData) {
    const auto full = std::filesystem::file_size(path);
    std::error_code error;
    std::filesystem::resize_file(path, full - 32, error);
    ASSERT_FALSE(error);
    expect_corruption();

    write_hnsw_file(path, *built->index, built->dimension);
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "unexpected extra bytes";
    }
    expect_corruption();
}

// The whole point of validating references at load: a neighbour id past the
// node count would be an out-of-bounds read on the very first search.
TEST_F(HnswFileCorruption, RejectsANeighbourReferencePastTheNodeCount) {
    const HnswFileHeader header = read_hnsw_file_header(path);
    // Node 0, layer 0: [count][n0]... — overwrite n0 with a huge id. The header
    // CRC still passes, because only the payload changed.
    const auto offset = static_cast<std::streamoff>(header.layer0_offset()) + 4;
    corrupt_byte(path, offset + 3, 0x7F);
    expect_corruption();

    try {
        static_cast<void>(read_hnsw_file(path,
                                         built->store.accessor(),
                                         Metric::kL2Squared,
                                         built->dimension,
                                         scalar_kernel()));
    } catch (const CorruptionError& error) {
        EXPECT_NE(std::string(error.what()).find("outside"), std::string::npos) << error.what();
    }
}

TEST_F(HnswFileCorruption, RejectsANeighbourCountAboveTheBudget) {
    const HnswFileHeader header = read_hnsw_file_header(path);
    // Slot 0 of node 0's layer-0 block is the count. M=8 so the budget is 16.
    corrupt_byte(path, static_cast<std::streamoff>(header.layer0_offset()), 200);
    expect_corruption();
}

TEST_F(HnswFileCorruption, RejectsAMetricThatDoesNotMatchTheDatabase) {
    try {
        static_cast<void>(read_hnsw_file(
            path, built->store.accessor(), Metric::kCosine, built->dimension, scalar_kernel()));
        FAIL() << "expected CorruptionError";
    } catch (const CorruptionError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("cosine"), std::string::npos) << message;
        EXPECT_NE(message.find("rebuild"), std::string::npos)
            << "the message must say what to do about it";
    }
}

TEST_F(HnswFileCorruption, RejectsADimensionThatDoesNotMatchTheDatabase) {
    EXPECT_THROW(static_cast<void>(read_hnsw_file(
                     path, built->store.accessor(), Metric::kL2Squared, 999, scalar_kernel())),
                 CorruptionError);
}

TEST_F(HnswFileCorruption, RejectsAGraphWhoseNodeCountDisagreesWithTheStore) {
    VectorStore smaller(8);
    smaller.insert(0, std::vector<float>(8, 1.0F));

    try {
        static_cast<void>(read_hnsw_file(
            path, smaller.accessor(), Metric::kL2Squared, built->dimension, scalar_kernel()));
        FAIL() << "expected CorruptionError";
    } catch (const CorruptionError& error) {
        EXPECT_NE(std::string(error.what()).find("rebuild"), std::string::npos);
    }
}

TEST_F(HnswFileCorruption, RejectsAMissingFile) {
    EXPECT_THROW(static_cast<void>(read_hnsw_file(dir.file("absent.hnsw"),
                                                  built->store.accessor(),
                                                  Metric::kL2Squared,
                                                  built->dimension,
                                                  scalar_kernel())),
                 IoError);
}

}  // namespace
}  // namespace vectordb
