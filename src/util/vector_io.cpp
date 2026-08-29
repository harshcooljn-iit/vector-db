// SPDX-License-Identifier: MIT
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include <vectordb/core/error.hpp>
#include <vectordb/persistence/byte_io.hpp>
#include <vectordb/persistence/file_io.hpp>
#include <vectordb/util/vector_io.hpp>

namespace vectordb {
namespace {

constexpr std::size_t kMagicSize = 8;
constexpr std::size_t kHeaderCrcOffset = 28;
constexpr std::uint32_t kFlagHasIds = 0x01;
constexpr std::uint64_t kMaxMatrixBytes = 64ULL * 1024 * 1024 * 1024;

std::uint32_t read_u32_at(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

}  // namespace

void write_vector_matrix(const std::filesystem::path& path,
                         const VectorArray& vectors,
                         const std::vector<VectorId>& ids) {
    if (!ids.empty() && ids.size() != vectors.size()) {
        throw InvalidArgumentError("id count " + std::to_string(ids.size()) +
                                   " does not match vector count " +
                                   std::to_string(vectors.size()));
    }

    std::vector<std::byte> id_bytes;
    if (!ids.empty()) {
        id_bytes.reserve(ids.size() * sizeof(VectorId));
        ByteWriter writer(id_bytes);
        for (const VectorId id : ids) {
            writer.u64(id);
        }
    }

    const std::span<const std::byte> payload{
        reinterpret_cast<const std::byte*>(vectors.data()),
        vectors.size() * vectors.dimension() * sizeof(float)};

    std::vector<std::byte> header;
    header.reserve(kVectorMatrixHeaderSize);
    ByteWriter writer(header);
    writer.fixed_string(kVectorMatrixMagic, kMagicSize);   //  0
    writer.u32(kVectorMatrixVersion);                      //  8
    writer.u32(vectors.dimension());                       // 12
    writer.u64(vectors.size());                            // 16
    writer.u32(ids.empty() ? 0U : kFlagHasIds);            // 24
    writer.u32(crc32({header.data(), kHeaderCrcOffset}));  // 28

    const std::span<const std::byte> chunks[] = {header, payload, id_bytes};
    write_file_atomically(path, chunks);
}

VectorBatch read_vector_matrix(const std::filesystem::path& path) {
    const std::vector<std::byte> bytes = read_file(path, kMaxMatrixBytes);
    if (bytes.size() < kVectorMatrixHeaderSize) {
        throw CorruptionError("'" + path.string() + "' is only " +
                              std::to_string(bytes.size()) +
                              " bytes, smaller than a vector matrix header");
    }

    ByteReader reader({bytes.data(), kVectorMatrixHeaderSize}, "vector matrix header");
    const std::string magic = reader.fixed_string(kMagicSize);
    if (magic != kVectorMatrixMagic) {
        throw CorruptionError("'" + path.string() + "': bad magic '" + magic + "' (expected '" +
                              std::string(kVectorMatrixMagic) + "'); is this a .vecs file?");
    }

    const std::uint32_t version = reader.u32();
    if (version != kVectorMatrixVersion) {
        throw UnsupportedVersionError("vector matrix", version, kVectorMatrixVersion);
    }

    const std::uint32_t stored_crc = read_u32_at(bytes, kHeaderCrcOffset);
    if (stored_crc != crc32({bytes.data(), kHeaderCrcOffset})) {
        throw CorruptionError("'" + path.string() + "': header checksum mismatch");
    }

    const auto dimension = static_cast<Dimension>(reader.u32());
    if (dimension == 0 || dimension > kMaxDimension) {
        throw CorruptionError("'" + path.string() + "': dimension " +
                              std::to_string(dimension) + " is outside [1, " +
                              std::to_string(kMaxDimension) + "]");
    }

    const std::uint64_t count = reader.u64();
    if (count > kMaxVectorCount) {
        throw CorruptionError("'" + path.string() + "': count " + std::to_string(count) +
                              " exceeds the maximum of " + std::to_string(kMaxVectorCount));
    }
    const bool has_ids = (reader.u32() & kFlagHasIds) != 0;

    // Overflow-checked before the size is used for anything.
    if (count != 0 &&
        dimension > (std::numeric_limits<std::uint64_t>::max() / count) / sizeof(float)) {
        throw CorruptionError("'" + path.string() + "': count * dimension overflows");
    }
    const std::uint64_t payload_bytes = count * dimension * sizeof(float);
    const std::uint64_t id_bytes = has_ids ? count * sizeof(VectorId) : 0;
    const std::uint64_t expected = kVectorMatrixHeaderSize + payload_bytes + id_bytes;

    if (bytes.size() != expected) {
        throw CorruptionError("'" + path.string() + "': header describes " +
                              std::to_string(expected) + " bytes but the file is " +
                              std::to_string(bytes.size()) +
                              (bytes.size() < expected ? " — truncated" : " — trailing data"));
    }

    VectorBatch batch{VectorArray(dimension), {}};
    batch.vectors.reserve(static_cast<std::size_t>(count));

    const auto* floats = reinterpret_cast<const float*>(bytes.data() + kVectorMatrixHeaderSize);
    for (std::uint64_t i = 0; i < count; ++i) {
        batch.vectors.push_back(VectorView{floats + i * dimension, dimension});
    }

    if (has_ids) {
        ByteReader ids({bytes.data() + kVectorMatrixHeaderSize + payload_bytes,
                        static_cast<std::size_t>(id_bytes)},
                       "vector matrix ids");
        batch.ids.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
            batch.ids.push_back(ids.u64());
        }
    }

    return batch;
}

VectorMatrixInfo read_vector_matrix_info(const std::filesystem::path& path) {
    const std::vector<std::byte> prefix = read_file_prefix(path, kVectorMatrixHeaderSize);
    ByteReader reader({prefix.data(), prefix.size()}, "vector matrix header");

    const std::string magic = reader.fixed_string(kMagicSize);
    if (magic != kVectorMatrixMagic) {
        throw CorruptionError("'" + path.string() + "': bad magic '" + magic + "'");
    }
    const std::uint32_t version = reader.u32();
    if (version != kVectorMatrixVersion) {
        throw UnsupportedVersionError("vector matrix", version, kVectorMatrixVersion);
    }

    VectorMatrixInfo info;
    info.dimension = static_cast<Dimension>(reader.u32());
    info.count = reader.u64();
    info.has_ids = (reader.u32() & kFlagHasIds) != 0;
    return info;
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

void write_vector_csv(const std::filesystem::path& path,
                      const VectorArray& vectors,
                      const std::vector<VectorId>& ids) {
    if (!ids.empty() && ids.size() != vectors.size()) {
        throw InvalidArgumentError("id count does not match vector count");
    }

    std::ofstream file(path);
    if (!file) {
        throw IoError("cannot open '" + path.string() + "' for writing");
    }

    file << "# VectorDB CSV: " << (ids.empty() ? "" : "id,") << "dimension "
         << vectors.dimension() << "\n";

    for (LocalId i = 0; i < vectors.size(); ++i) {
        if (!ids.empty()) {
            file << ids[i] << ',';
        }
        const VectorView vector = vectors[i];
        for (Dimension d = 0; d < vector.size(); ++d) {
            if (d > 0) {
                file << ',';
            }
            // Enough digits to round-trip a float exactly; anything less makes
            // an export/import cycle lossy, which is a nasty surprise.
            file << std::setprecision(9) << vector[d];
        }
        file << '\n';
    }

    if (!file) {
        throw IoError("failed while writing '" + path.string() + "'");
    }
}

VectorBatch read_vector_csv(const std::filesystem::path& path, bool with_ids) {
    std::ifstream file(path);
    if (!file) {
        throw IoError("cannot open '" + path.string() + "' for reading");
    }

    std::vector<std::vector<float>> rows;
    std::vector<VectorId> ids;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;
        // Trim, then skip blanks and comments so a file can carry a header.
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') {
            continue;
        }

        std::vector<float> values;
        std::stringstream stream(line);
        std::string field;
        bool first_field = true;

        while (std::getline(stream, field, ',')) {
            const std::size_t begin = field.find_first_not_of(" \t\r");
            if (begin == std::string::npos) {
                continue;
            }
            const std::size_t end = field.find_last_not_of(" \t\r");
            const std::string token = field.substr(begin, end - begin + 1);

            if (with_ids && first_field) {
                try {
                    ids.push_back(std::stoull(token));
                } catch (const std::exception&) {
                    throw InvalidArgumentError("'" + path.string() + "' line " +
                                               std::to_string(line_number) + ": '" + token +
                                               "' is not a valid id");
                }
                first_field = false;
                continue;
            }
            first_field = false;

            try {
                values.push_back(std::stof(token));
            } catch (const std::exception&) {
                throw InvalidArgumentError("'" + path.string() + "' line " +
                                           std::to_string(line_number) + ": '" + token +
                                           "' is not a number");
            }
        }

        if (values.empty()) {
            continue;
        }
        if (!rows.empty() && values.size() != rows.front().size()) {
            throw InvalidArgumentError(
                "'" + path.string() + "' line " + std::to_string(line_number) + ": has " +
                std::to_string(values.size()) + " components but the first row had " +
                std::to_string(rows.front().size()));
        }
        rows.push_back(std::move(values));
    }

    if (rows.empty()) {
        throw InvalidArgumentError("'" + path.string() + "' contains no vectors");
    }

    VectorBatch batch{VectorArray(static_cast<Dimension>(rows.front().size())), std::move(ids)};
    batch.vectors.reserve(rows.size());
    for (const std::vector<float>& row : rows) {
        batch.vectors.push_back(row);
    }
    return batch;
}

std::vector<float> parse_vector_literal(std::string_view text) {
    std::vector<float> values;
    std::string token;

    const auto flush = [&] {
        if (token.empty()) {
            return;
        }
        try {
            std::size_t consumed = 0;
            const float value = std::stof(token, &consumed);
            if (consumed != token.size()) {
                throw std::invalid_argument("trailing characters");
            }
            values.push_back(value);
        } catch (const std::exception&) {
            throw InvalidArgumentError("component " + std::to_string(values.size()) +
                                       " of the vector is not a number: '" + token + "'");
        }
        token.clear();
    };

    for (const char character : text) {
        if (character == ',' || character == ' ' || character == '\t' || character == '[' ||
            character == ']') {
            flush();
        } else {
            token.push_back(character);
        }
    }
    flush();

    if (values.empty()) {
        throw InvalidArgumentError(
            "empty vector: expected comma- or space-separated "
            "numbers, for example \"0.1,0.2,0.3\"");
    }
    return values;
}

}  // namespace vectordb
