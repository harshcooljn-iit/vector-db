// SPDX-License-Identifier: MIT
#include <limits>
#include <string>
#include <vector>

#include <vectordb/core/error.hpp>
#include <vectordb/index/hnsw_file.hpp>
#include <vectordb/persistence/byte_io.hpp>
#include <vectordb/persistence/file_io.hpp>

namespace vectordb {
namespace {

constexpr std::size_t kMagicSize = 8;
constexpr std::size_t kHeaderCrcOffset = 124;
constexpr std::uint32_t kNoUpperLinks = std::numeric_limits<std::uint32_t>::max();

std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b, std::string_view what) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        throw CorruptionError(std::string("hnsw index: ") + std::string(what) +
                              " overflows: " + std::to_string(a) + " * " + std::to_string(b));
    }
    return a * b;
}

std::uint32_t read_u32_at(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

}  // namespace

std::uint64_t HnswFileHeader::levels_offset() const noexcept {
    return kHnswFileHeaderSize;
}

std::uint64_t HnswFileHeader::offsets_offset() const noexcept {
    return levels_offset() + node_count;
}

std::uint64_t HnswFileHeader::layer0_offset() const noexcept {
    return offsets_offset() + node_count * sizeof(std::uint32_t);
}

std::uint64_t HnswFileHeader::upper_offset() const noexcept {
    return layer0_offset() + layer0_link_count * sizeof(LocalId);
}

std::uint64_t HnswFileHeader::total_size() const noexcept {
    return upper_offset() + upper_link_count * sizeof(LocalId);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void write_hnsw_file(const std::filesystem::path& path,
                     const HnswIndex& index,
                     Dimension dimension) {
    const std::span<const std::uint8_t> levels = index.node_levels();
    const std::span<const std::uint32_t> offsets = index.upper_offsets();
    const std::span<const LocalId> layer0 = index.layer0_links();
    const std::span<const LocalId> upper = index.upper_links();
    const HnswConfig& config = index.config();

    // Each array is written as its raw little-endian bytes. That is safe here
    // for the same narrow reason it is safe for the vector payload: the format
    // is little-endian-only (enforced by a static_assert in byte_io.hpp) and
    // these are fixed-width types. Serializing 30 million LocalIds one shift at
    // a time would be absurd.
    const std::span<const std::byte> levels_bytes{
        reinterpret_cast<const std::byte*>(levels.data()), levels.size()};
    const std::span<const std::byte> offsets_bytes{
        reinterpret_cast<const std::byte*>(offsets.data()),
        offsets.size() * sizeof(std::uint32_t)};
    const std::span<const std::byte> layer0_bytes{
        reinterpret_cast<const std::byte*>(layer0.data()), layer0.size() * sizeof(LocalId)};
    const std::span<const std::byte> upper_bytes{
        reinterpret_cast<const std::byte*>(upper.data()), upper.size() * sizeof(LocalId)};

    std::uint32_t payload_crc = crc32(levels_bytes);
    payload_crc = crc32(offsets_bytes, payload_crc);
    payload_crc = crc32(layer0_bytes, payload_crc);
    payload_crc = crc32(upper_bytes, payload_crc);

    std::vector<std::byte> header;
    header.reserve(kHnswFileHeaderSize);
    ByteWriter writer(header);
    writer.fixed_string(kHnswFileMagic, kMagicSize);                 //   0
    writer.u32(kHnswFileVersion);                                    //   8
    writer.u32(dimension);                                           //  12
    writer.u64(index.node_count());                                  //  16
    writer.u64(index.size());                                        //  24
    writer.u32(static_cast<std::uint32_t>(config.m));                //  32
    writer.u32(static_cast<std::uint32_t>(config.ef_construction));  //  36
    writer.u32(static_cast<std::uint32_t>(config.ef_search));        //  40
    writer.u32(index.entry_point());                                 //  44
    writer.u64(config.seed);                                         //  48
    writer.u64(index.rng_state());                                   //  56
    writer.u32(static_cast<std::uint32_t>(index.max_level()));       //  64
    writer.u8(static_cast<std::uint8_t>(index.metric()));            //  68
    writer.pad(3);                                                   //  69
    writer.u64(layer0.size());                                       //  72
    writer.u64(upper.size());                                        //  80
    writer.u32(payload_crc);                                         //  88
    writer.pad(32);                                                  //  92  reserved
    writer.u32(crc32({header.data(), kHeaderCrcOffset}));            // 124

    const std::span<const std::byte> chunks[] = {
        header,
        levels_bytes,
        offsets_bytes,
        layer0_bytes,
        upper_bytes,
    };
    write_file_atomically(path, chunks);
}

// ---------------------------------------------------------------------------
// Header validation
// ---------------------------------------------------------------------------

HnswFileHeader parse_hnsw_file_header(std::span<const std::byte> bytes,
                                      std::uint64_t file_size) {
    if (bytes.size() < kHnswFileHeaderSize) {
        throw CorruptionError("hnsw index: only " + std::to_string(bytes.size()) +
                              " bytes, smaller than the " +
                              std::to_string(kHnswFileHeaderSize) + " byte header");
    }

    ByteReader reader(bytes.first(kHnswFileHeaderSize), "hnsw index header");

    const std::string magic = reader.fixed_string(kMagicSize);
    if (magic != kHnswFileMagic) {
        throw CorruptionError("hnsw index: bad magic '" + magic +
                              "' — this is not a VectorDB index file (expected '" +
                              std::string(kHnswFileMagic) + "')");
    }

    HnswFileHeader header;
    header.format_version = reader.u32();
    if (header.format_version != kHnswFileVersion) {
        throw UnsupportedVersionError("hnsw index", header.format_version, kHnswFileVersion);
    }

    const std::uint32_t stored_crc = read_u32_at(bytes, kHeaderCrcOffset);
    const std::uint32_t computed_crc = crc32(bytes.first(kHeaderCrcOffset));
    if (stored_crc != computed_crc) {
        throw CorruptionError("hnsw index: header checksum mismatch (stored " +
                              std::to_string(stored_crc) + ", computed " +
                              std::to_string(computed_crc) + ")");
    }

    header.dimension = reader.u32();
    if (header.dimension == 0 || header.dimension > kMaxDimension) {
        throw CorruptionError("hnsw index: dimension " + std::to_string(header.dimension) +
                              " is outside [1, " + std::to_string(kMaxDimension) + "]");
    }

    header.node_count = reader.u64();
    if (header.node_count > kMaxVectorCount) {
        throw CorruptionError("hnsw index: node count " + std::to_string(header.node_count) +
                              " exceeds the maximum of " + std::to_string(kMaxVectorCount));
    }

    header.live_count = reader.u64();
    if (header.live_count > header.node_count) {
        throw CorruptionError("hnsw index: live count " + std::to_string(header.live_count) +
                              " exceeds node count " + std::to_string(header.node_count));
    }

    header.config.m = reader.u32();
    header.config.ef_construction = reader.u32();
    header.config.ef_search = reader.u32();
    header.entry_point = reader.u32();
    header.config.seed = reader.u64();
    header.rng_state = reader.u64();
    header.max_level = reader.u32();

    const std::uint8_t metric_value = reader.u8();
    if (metric_value > static_cast<std::uint8_t>(Metric::kInnerProduct)) {
        throw CorruptionError("hnsw index: unknown metric code " +
                              std::to_string(metric_value));
    }
    header.metric = static_cast<Metric>(metric_value);
    reader.skip(3);

    // Config validation reuses the same rules the constructor applies, so a
    // corrupt M can never produce a graph the code would refuse to build.
    try {
        header.config.validate();
    } catch (const InvalidArgumentError& error) {
        throw CorruptionError(std::string("hnsw index: stored configuration is invalid: ") +
                              error.what());
    }

    header.layer0_link_count = reader.u64();
    header.upper_link_count = reader.u64();
    header.payload_crc = reader.u32();
    header.header_crc = stored_crc;

    // Structural cross-checks. Layer 0 has a fixed stride, so its size is fully
    // determined by the node count — a mismatch means the header is internally
    // inconsistent and nothing below it can be trusted.
    const std::uint64_t expected_layer0 =
        checked_multiply(header.node_count, header.config.max_m0() + 1, "layer 0 link count");
    if (header.layer0_link_count != expected_layer0) {
        throw CorruptionError(
            "hnsw index: layer 0 holds " + std::to_string(header.layer0_link_count) +
            " entries but node_count * (2M + 1) = " + std::to_string(expected_layer0));
    }

    if (header.max_level > 254) {
        throw CorruptionError("hnsw index: max level " + std::to_string(header.max_level) +
                              " exceeds the 254 that a u8 level can encode");
    }

    if (header.node_count == 0) {
        if (header.entry_point != kInvalidLocalId) {
            throw CorruptionError("hnsw index: empty graph has an entry point");
        }
    } else if (static_cast<std::uint64_t>(header.entry_point) >= header.node_count) {
        throw CorruptionError("hnsw index: entry point " + std::to_string(header.entry_point) +
                              " is outside the node range [0, " +
                              std::to_string(header.node_count) + ")");
    }

    const std::uint64_t expected_size = header.total_size();
    if (file_size != expected_size) {
        throw CorruptionError(
            "hnsw index: header describes " + std::to_string(expected_size) +
            " bytes but the file is " + std::to_string(file_size) +
            (file_size < expected_size ? " — truncated" : " — trailing data"));
    }

    return header;
}

HnswFileHeader read_hnsw_file_header(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        throw IoError("cannot stat '" + path.string() + "': " + error.message());
    }
    const std::vector<std::byte> prefix = read_file_prefix(path, kHnswFileHeaderSize);
    return parse_hnsw_file_header(prefix, static_cast<std::uint64_t>(size));
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

std::unique_ptr<HnswIndex> read_hnsw_file(const std::filesystem::path& path,
                                          VectorAccessor accessor,
                                          Metric expected_metric,
                                          Dimension expected_dimension,
                                          const DistanceKernel& kernel) {
    MappedFile mapping(path);
    const std::span<const std::byte> bytes = mapping.bytes();
    const HnswFileHeader header = parse_hnsw_file_header(bytes, bytes.size());

    // An index built for a different metric or dimension is not stale, it is
    // meaningless: its edges encode proximity under rules that no longer apply.
    if (header.metric != expected_metric) {
        throw CorruptionError(
            "hnsw index was built for metric '" + std::string(metric_name(header.metric)) +
            "' but this database uses '" + std::string(metric_name(expected_metric)) +
            "'; rebuild the index");
    }
    if (header.dimension != expected_dimension) {
        throw CorruptionError("hnsw index was built for dimension " +
                              std::to_string(header.dimension) + " but this database uses " +
                              std::to_string(expected_dimension) + "; rebuild the index");
    }
    if (header.node_count != accessor.size()) {
        throw CorruptionError("hnsw index holds " + std::to_string(header.node_count) +
                              " nodes but the vector store holds " +
                              std::to_string(accessor.size()) + " slots; rebuild the index");
    }

    const auto node_count = static_cast<std::size_t>(header.node_count);

    std::vector<std::uint8_t> levels(node_count);
    std::vector<std::uint32_t> offsets(node_count);
    std::vector<LocalId> layer0(static_cast<std::size_t>(header.layer0_link_count));
    std::vector<LocalId> upper(static_cast<std::size_t>(header.upper_link_count));

    std::memcpy(levels.data(), bytes.data() + header.levels_offset(), levels.size());
    std::memcpy(offsets.data(),
                bytes.data() + header.offsets_offset(),
                offsets.size() * sizeof(std::uint32_t));
    std::memcpy(
        layer0.data(), bytes.data() + header.layer0_offset(), layer0.size() * sizeof(LocalId));
    std::memcpy(
        upper.data(), bytes.data() + header.upper_offset(), upper.size() * sizeof(LocalId));

    // ---- Reference validation -------------------------------------------
    //
    // O(edges), and it pays for itself twice over. It means the search loop
    // never bounds-checks a neighbour id — a corrupt file cannot become an
    // out-of-bounds read at query time — and it means a damaged graph is
    // reported as damaged instead of returning plausible wrong answers forever.
    //
    // Never trust an offset or an index that came from a file.

    const std::size_t layer0_stride = header.config.max_m0() + 1;
    const std::size_t upper_stride = header.config.m + 1;

    const auto validate_block = [&](const std::vector<LocalId>& array,
                                    std::size_t base,
                                    std::size_t budget,
                                    LocalId node,
                                    std::size_t layer) {
        const LocalId count = array[base];
        if (count > budget) {
            throw CorruptionError("hnsw index: node " + std::to_string(node) + " layer " +
                                  std::to_string(layer) + " claims " + std::to_string(count) +
                                  " neighbours, above the " + std::to_string(budget) +
                                  " budget");
        }
        for (std::size_t i = 0; i < count; ++i) {
            const LocalId neighbour = array[base + 1 + i];
            if (static_cast<std::uint64_t>(neighbour) >= header.node_count) {
                throw CorruptionError("hnsw index: node " + std::to_string(node) + " layer " +
                                      std::to_string(layer) + " references node " +
                                      std::to_string(neighbour) + ", outside [0, " +
                                      std::to_string(header.node_count) + ")");
            }
            if (neighbour == node) {
                throw CorruptionError("hnsw index: node " + std::to_string(node) +
                                      " links to itself on layer " + std::to_string(layer));
            }
            if (levels[neighbour] < layer) {
                throw CorruptionError(
                    "hnsw index: node " + std::to_string(node) + " links on layer " +
                    std::to_string(layer) + " to node " + std::to_string(neighbour) +
                    ", which only reaches layer " + std::to_string(levels[neighbour]));
            }
        }
    };

    for (std::size_t node = 0; node < node_count; ++node) {
        const std::size_t level = levels[node];
        if (level > header.max_level) {
            throw CorruptionError("hnsw index: node " + std::to_string(node) +
                                  " claims level " + std::to_string(level) +
                                  " above the graph maximum of " +
                                  std::to_string(header.max_level));
        }

        validate_block(layer0,
                       node * layer0_stride,
                       header.config.max_m0(),
                       static_cast<LocalId>(node),
                       0);

        if (level == 0) {
            if (offsets[node] != kNoUpperLinks) {
                throw CorruptionError("hnsw index: node " + std::to_string(node) +
                                      " is level 0 but carries an upper-layer offset");
            }
            continue;
        }

        // Checked before it is used as an index, not after.
        const std::uint64_t needed =
            static_cast<std::uint64_t>(offsets[node]) + level * upper_stride;
        if (offsets[node] == kNoUpperLinks || needed > header.upper_link_count) {
            throw CorruptionError(
                "hnsw index: node " + std::to_string(node) + " has upper offset " +
                std::to_string(offsets[node]) + " which would run past the " +
                std::to_string(header.upper_link_count) + " entry upper-link arena");
        }

        for (std::size_t layer = 1; layer <= level; ++layer) {
            validate_block(upper,
                           offsets[node] + (layer - 1) * upper_stride,
                           header.config.m,
                           static_cast<LocalId>(node),
                           layer);
        }
    }

    if (node_count > 0 && levels[header.entry_point] != header.max_level) {
        throw CorruptionError("hnsw index: entry point " + std::to_string(header.entry_point) +
                              " is at level " + std::to_string(levels[header.entry_point]) +
                              " but the graph maximum is " + std::to_string(header.max_level));
    }

    auto index = std::make_unique<HnswIndex>(accessor, header.metric, header.config, kernel);
    index->restore(std::move(levels),
                   std::move(layer0),
                   std::move(upper),
                   std::move(offsets),
                   header.entry_point,
                   header.max_level,
                   static_cast<std::size_t>(header.live_count),
                   header.rng_state);
    return index;
}

bool verify_hnsw_file_payload(const std::filesystem::path& path) {
    MappedFile mapping(path);
    const std::span<const std::byte> bytes = mapping.bytes();
    const HnswFileHeader header = parse_hnsw_file_header(bytes, bytes.size());

    const std::uint64_t payload_size = header.total_size() - kHnswFileHeaderSize;
    return crc32(bytes.subspan(kHnswFileHeaderSize, static_cast<std::size_t>(payload_size))) ==
           header.payload_crc;
}

}  // namespace vectordb
