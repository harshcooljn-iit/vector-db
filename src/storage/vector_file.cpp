// SPDX-License-Identifier: MIT
#include <string>
#include <vector>

#include <vectordb/core/error.hpp>
#include <vectordb/persistence/byte_io.hpp>
#include <vectordb/persistence/file_io.hpp>
#include <vectordb/storage/vector_file.hpp>

namespace vectordb {
namespace {

constexpr std::size_t kMagicSize = 8;
constexpr std::size_t kHeaderCrcOffset = 60;
constexpr std::uint8_t kFlagNormalized = 0x01;

/// Multiplies with an explicit overflow check.
///
/// `slot_count * dimension * 4` is computed from values read out of a file. A
/// corrupt header claiming 2^40 slots of dimension 60000 would wrap silently in
/// 64-bit arithmetic and produce a *small* plausible size, which is exactly how
/// a validated-looking header leads to an out-of-bounds read.
std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b, std::string_view what) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        throw CorruptionError(std::string("vector file: ") + std::string(what) +
                              " overflows: " + std::to_string(a) + " * " + std::to_string(b));
    }
    return a * b;
}

}  // namespace

std::uint64_t VectorFileHeader::payload_offset() const noexcept {
    return kVectorFileHeaderSize;
}

std::uint64_t VectorFileHeader::ids_offset() const noexcept {
    return payload_offset() + slot_count * dimension * sizeof(float);
}

std::uint64_t VectorFileHeader::norms_offset() const noexcept {
    return ids_offset() + slot_count * sizeof(VectorId);
}

std::uint64_t VectorFileHeader::live_offset() const noexcept {
    return norms_offset() + slot_count * sizeof(float);
}

std::uint64_t VectorFileHeader::total_size() const noexcept {
    return live_offset() + slot_count * sizeof(std::uint8_t);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

void write_vector_file(const std::filesystem::path& path, const VectorStore& store) {
    const std::uint64_t slot_count = store.slot_count();
    const Dimension dimension = store.dimension();

    // Regions are built as separate arrays rather than interleaved per slot.
    // The payload can then be handed to write() as one contiguous span — one
    // syscall for 3 GB instead of a million — and, more importantly, the file
    // layout matches VectorStore's in-memory layout exactly, so a load can map
    // it and use the floats in place.
    std::vector<std::byte> ids_bytes;
    std::vector<std::byte> norms_bytes;
    std::vector<std::byte> live_bytes;
    ids_bytes.reserve(slot_count * sizeof(VectorId));
    norms_bytes.reserve(slot_count * sizeof(float));
    live_bytes.reserve(slot_count);

    ByteWriter ids_writer(ids_bytes);
    ByteWriter norms_writer(norms_bytes);
    ByteWriter live_writer(live_bytes);
    for (LocalId id = 0; id < static_cast<LocalId>(slot_count); ++id) {
        ids_writer.u64(store.vector_id(id));
        norms_writer.f32(store.norm(id));
        live_writer.u8(store.live(id) ? 1U : 0U);
    }

    const std::span<const float> payload{
        store.slot_count() == 0 ? nullptr : store.vector(0).data(),
        static_cast<std::size_t>(slot_count) * dimension};
    const std::uint32_t payload_crc = crc32_floats(payload);

    std::vector<std::byte> header;
    header.reserve(kVectorFileHeaderSize);
    ByteWriter writer(header);
    writer.fixed_string(kVectorFileMagic, kMagicSize);    //  0..8
    writer.u32(kVectorFileVersion);                       //  8..12
    writer.u32(dimension);                                // 12..16
    writer.u64(slot_count);                               // 16..24
    writer.u64(store.live_count());                       // 24..32
    writer.u8(store.normalizes() ? kFlagNormalized : 0);  // 32..33
    writer.pad(7);                                        // 33..40  reserved
    writer.u64(slot_count * static_cast<std::uint64_t>(dimension) *
               sizeof(float));                             // 40..48  payload bytes
    writer.u32(payload_crc);                               // 48..52
    writer.pad(8);                                         // 52..60  reserved
    writer.u32(crc32({header.data(), kHeaderCrcOffset}));  // 60..64 header crc

    const std::span<const std::byte> payload_bytes{
        reinterpret_cast<const std::byte*>(payload.data()), payload.size() * sizeof(float)};

    const std::span<const std::byte> chunks[] = {
        header,
        payload_bytes,
        ids_bytes,
        norms_bytes,
        live_bytes,
    };
    write_file_atomically(path, chunks);
}

// ---------------------------------------------------------------------------
// Header validation
// ---------------------------------------------------------------------------

VectorFileHeader parse_vector_file_header(std::span<const std::byte> bytes,
                                          std::uint64_t file_size) {
    if (bytes.size() < kVectorFileHeaderSize) {
        throw CorruptionError("vector file: only " + std::to_string(bytes.size()) +
                              " bytes, which is smaller than the " +
                              std::to_string(kVectorFileHeaderSize) + " byte header");
    }

    ByteReader reader(bytes.first(kVectorFileHeaderSize), "vector file header");

    const std::string magic = reader.fixed_string(kMagicSize);
    if (magic != "VDBVEC") {
        throw CorruptionError("vector file: bad magic '" + magic +
                              "' — this is not a VectorDB vector file (expected 'VDBVEC')");
    }

    VectorFileHeader header;
    header.format_version = reader.u32();
    if (header.format_version != kVectorFileVersion) {
        throw UnsupportedVersionError("vector file", header.format_version, kVectorFileVersion);
    }

    // The header CRC is checked before any other field is believed. Everything
    // after this point rests on it.
    const std::uint32_t stored_crc =
        std::bit_cast<std::uint32_t>(std::array<std::byte, 4>{bytes[kHeaderCrcOffset],
                                                              bytes[kHeaderCrcOffset + 1],
                                                              bytes[kHeaderCrcOffset + 2],
                                                              bytes[kHeaderCrcOffset + 3]});
    const std::uint32_t computed_crc = crc32(bytes.first(kHeaderCrcOffset));
    if (stored_crc != computed_crc) {
        throw CorruptionError("vector file: header checksum mismatch (stored " +
                              std::to_string(stored_crc) + ", computed " +
                              std::to_string(computed_crc) + ") — the header has been damaged");
    }

    header.dimension = reader.u32();
    if (header.dimension == 0 || header.dimension > kMaxDimension) {
        throw CorruptionError("vector file: dimension " + std::to_string(header.dimension) +
                              " is outside the supported range [1, " +
                              std::to_string(kMaxDimension) + "]");
    }

    header.slot_count = reader.u64();
    if (header.slot_count > kMaxVectorCount) {
        throw CorruptionError("vector file: slot count " + std::to_string(header.slot_count) +
                              " exceeds the maximum of " + std::to_string(kMaxVectorCount));
    }

    header.live_count = reader.u64();
    if (header.live_count > header.slot_count) {
        throw CorruptionError("vector file: live count " + std::to_string(header.live_count) +
                              " exceeds slot count " + std::to_string(header.slot_count));
    }

    header.normalized = (reader.u8() & kFlagNormalized) != 0;
    reader.skip(7);

    // Overflow-checked before the value is used for anything.
    const std::uint64_t expected_payload = checked_multiply(
        checked_multiply(header.slot_count, header.dimension, "slot_count * dimension"),
        sizeof(float),
        "payload size");

    const std::uint64_t declared_payload = reader.u64();
    if (declared_payload != expected_payload) {
        throw CorruptionError("vector file: header declares a " +
                              std::to_string(declared_payload) +
                              " byte payload but slot_count * dimension * 4 = " +
                              std::to_string(expected_payload));
    }

    header.payload_crc = reader.u32();
    header.header_crc = stored_crc;

    // The final structural check, and the one that catches truncation: the file
    // must be exactly as long as the header says it is.
    const std::uint64_t expected_size = header.total_size();
    if (file_size != expected_size) {
        throw CorruptionError("vector file: header describes " + std::to_string(expected_size) +
                              " bytes but the file is " + std::to_string(file_size) +
                              (file_size < expected_size ? " — the file is truncated"
                                                         : " — the file has trailing data"));
    }

    return header;
}

VectorFileHeader read_vector_file_header(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        throw IoError("cannot stat '" + path.string() + "': " + error.message());
    }
    const std::vector<std::byte> prefix = read_file_prefix(path, kVectorFileHeaderSize);
    return parse_vector_file_header(prefix, static_cast<std::uint64_t>(size));
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

VectorStore read_vector_file(const std::filesystem::path& path) {
    MappedFile mapping(path);
    const std::span<const std::byte> bytes = mapping.bytes();

    const VectorFileHeader header = parse_vector_file_header(bytes, bytes.size());

    VectorStore store(header.dimension, header.normalized);
    store.reserve(static_cast<std::size_t>(header.slot_count));

    // Sequential from here on: one straight pass through the whole file.
    mapping.advise_sequential();

    const auto* floats = reinterpret_cast<const float*>(bytes.data() + header.payload_offset());
    ByteReader ids(bytes.subspan(header.ids_offset(), header.slot_count * sizeof(VectorId)),
                   "vector file ids");
    ByteReader norms(bytes.subspan(header.norms_offset(), header.slot_count * sizeof(float)),
                     "vector file norms");
    ByteReader live(bytes.subspan(header.live_offset(), header.slot_count),
                    "vector file liveness");

    std::uint64_t live_seen = 0;
    for (std::uint64_t slot = 0; slot < header.slot_count; ++slot) {
        const VectorId id = ids.u64();
        const float norm = norms.f32();
        const bool is_live = live.u8() != 0;
        live_seen += is_live ? 1U : 0U;

        const VectorView vector{floats + slot * header.dimension, header.dimension};
        store.append_trusted(id, vector, norm, is_live);
    }

    // The redundant live_count in the header exists precisely so this
    // comparison is possible; it costs one integer compare and catches a class
    // of corruption the structural checks cannot see.
    if (live_seen != header.live_count) {
        throw CorruptionError(
            "vector file: header claims " + std::to_string(header.live_count) +
            " live vectors but the liveness bytes contain " + std::to_string(live_seen));
    }

    return store;
}

bool verify_vector_file_payload(const std::filesystem::path& path) {
    MappedFile mapping(path);
    const std::span<const std::byte> bytes = mapping.bytes();
    const VectorFileHeader header = parse_vector_file_header(bytes, bytes.size());

    mapping.advise_sequential();
    const std::span<const std::byte> payload = bytes.subspan(
        header.payload_offset(),
        static_cast<std::size_t>(header.slot_count) * header.dimension * sizeof(float));

    return crc32(payload) == header.payload_crc;
}

}  // namespace vectordb
