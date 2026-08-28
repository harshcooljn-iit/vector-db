# Binary file formats — why you cannot just write the struct

🔴 Advanced · pairs with `src/persistence/byte_io.cpp` and `src/storage/vector_file.cpp`

## The tempting version

```cpp
struct Header {
    uint32_t version;
    uint64_t count;
    uint32_t dimension;
};

Header header{1, 1000, 768};
write(fd, &header, sizeof(header));       // DON'T
```

Three lines, no serialization code, obviously fast. And wrong in four separate
ways, each of which produces a bug that shows up long after the change that
caused it.

### 1. Padding

```cpp
static_assert(sizeof(Header) == 16);   // not 12!
```

```
offset  0    4         8              16       24
        [version][PAD][    count    ][dimension][PAD]
                  ^^^                            ^^^
```

The compiler inserts 4 bytes so `count` lands on an 8-byte boundary, and 4 more
at the end so an array of `Header` stays aligned.

Those padding bytes are **never initialised**. They contain whatever was on the
stack. So:

- the file differs between runs even when the data is identical, which defeats
  any checksum or content-addressed storage;
- you are writing stack memory into a file you may hand to someone else.

### 2. Layout is an ABI property, not a language property

The standard does not specify where the padding goes. It is decided by your
compiler, your architecture, and anything that sets `#pragma pack` in a header
included above yours.

Change any of those and a file written by yesterday's build is misread by
today's — silently, with fields offset by four bytes. The symptom is
"`count` is 4294967296 instead of 1", and the cause is three commits and two
weeks away.

### 3. Endianness

`0x11223344` is `44 33 22 11` on a little-endian machine and `11 22 33 44` on a
big-endian one. Dumping raw memory bakes in your CPU's convention without
recording it anywhere.

### 4. Types are not portable

`long` is 4 bytes on Windows and 8 on Linux. `bool` is one byte in practice and
unspecified in principle. `size_t` differs between 32- and 64-bit builds.

## What we do instead

```cpp
ByteWriter writer(buffer);
writer.u32(version);
writer.u64(count);
writer.u32(dimension);
```

Sixteen bytes become twelve. Every byte is written on purpose, in a stated
order, at a stated width. The layout is now a property of *this code* — visible,
reviewable, documented in `docs/vector-storage.md`, and stable forever.

```cpp
void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}
```

Shifts and masks, not memory reinterpretation, so it does the same thing on
every machine regardless of native byte order.

For floats, `std::bit_cast<uint32_t>(value)` — not a `reinterpret_cast`, not a
union. `bit_cast` is the only form that is *defined* rather than merely working
in practice.

> **The bulk float payload is the deliberate exception.** Writing 768 million
> floats one shift at a time would be absurd, so it goes out as one contiguous
> `write()`. That is safe only because a `static_assert` at the top of
> `byte_io.hpp` fails the build on a big-endian host, and because IEEE-754
> `binary32` is guaranteed on every supported target. The exception is narrow,
> explicit, and enforced by the compiler — which is what makes it acceptable
> rather than a shortcut.

## Reading is where the security lives

Writing a bad file is inconvenient. *Reading* one can be a memory-safety bug.

The classic:

```cpp
uint64_t count = read_u64();
std::vector<Node> nodes(count);        // ☠️
```

`count` came from a file. If the file is truncated, foreign, or corrupt, it
might be `0xFFFFFFFFFFFFFFFF`. That line then attempts a 300-exabyte allocation:
best case `bad_alloc`, worse case the OOM killer, worst case a smaller-but-still
-wrong value succeeds and the following loop reads past the end of the buffer.

So **every read is bounds-checked, before it happens**:

```cpp
void ByteReader::require(std::size_t count, std::string_view what) const {
    if (remaining() < count)
        throw CorruptionError(context + ": truncated while reading " + what +
                              " at offset " + ... );
}
```

The rule, stated once and applied everywhere:

> **A length read from a file is checked against the bytes that actually remain
> before it is used to size anything.**

And the error names the artefact, the field, the offset, and the shortfall —
because "corrupt file" tells a user nothing they can act on.

## The header design

```
 0  magic "VDBVEC\0\0"        is this even our file?
 8  format_version            can this build understand it?
12  dimension                 \
16  slot_count                 |  the actual metadata
24  live_count                 |
32  flags                     /
40  payload_bytes             redundant, on purpose
48  payload_crc32             for `vectordb check`
60  header_crc32              over bytes [0, 60)
```

Five ideas worth stealing for any format you design:

**Magic first.** Four to eight bytes that say "this is mine". Cheap, and it
means a user who points the tool at the wrong file gets "this is not a VectorDB
vector file" instead of a stack trace from deep inside a parser. Eight bytes of
readable ASCII shows up in a hex dump, which is worth a lot at 2 a.m.

**Version second, and checked before the checksum.** A file from a newer build
fails *both* the version check and the CRC. Reporting the version first gives
the user "written by a newer VectorDB" instead of "checksum mismatch", and only
one of those tells them what to do.

**Fixed-size header.** Exactly 64 bytes, always. A variable-length header cannot
be read until it has been parsed, and cannot be parsed until it has been read.
It also puts the payload at a 64-byte boundary — cache-line and page aligned, so
the file can be mapped and the floats used in place.

**Redundant fields.** `payload_bytes` is derivable from
`slot_count × dimension × 4`, and `live_count` from summing the liveness bytes.
Storing them anyway means a mismatch is a free corruption signal.

**Checksum the header always, the payload never.** The header is 60 bytes —
verifying it costs nothing and everything else depends on it. The payload is 3
GB; a full pass on every open, to catch corruption that would change one
distance slightly, is a bad trade. `vectordb check` does it explicitly. That gap
is documented in ADR-0008 rather than quietly hoped about.

## The overflow check that is easy to forget

```cpp
std::uint64_t expected = slot_count * dimension * 4;   // ☠️
```

Suppose a corrupt header claims `slot_count = 2^40` and `dimension = 60000`. The
true product is about 2.4 × 10¹⁷ × 4, which **wraps** in 64-bit arithmetic and
produces a small, plausible number. The size check then passes, and you proceed
to read from a region that does not exist.

```cpp
if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
    throw CorruptionError(...);
```

Validation that itself overflows is not validation. This is a general hazard:
any arithmetic on file-supplied values must be checked, including the arithmetic
inside your checks.

## "Why not just use..."

**Protocol Buffers / FlatBuffers / Cap'n Proto?** Excellent choices for
structured messages, and a real dependency plus a code generation step. More to
the point: our payload is 3 GB of raw floats where the whole design goal is that
the file layout equals the memory layout so it can be mapped and used in place.
A schema framework's framing gets in the way of precisely that. (FlatBuffers
gets closest — zero-copy is its entire premise — and is worth studying.)

**JSON?** For 768 million floats, no. Roughly 12× the size, plus parsing.
Excellent for the *config* file, which is small, human-editable, and rarely
read — and that is exactly what we use it for.

**SQLite BLOBs?** SQLite stores our metadata, and it would happily store the
vectors too. But it would mean a 3 GB BLOB read through a database API rather
than a file we can mmap, and it would put the vectors behind SQLite's page cache
instead of the OS's. Different layers, different tools.

## Experiments

1. Print `sizeof(Header)` for the struct at the top of this note. Reorder the
   fields largest-to-smallest and print it again. You have just discovered why
   struct field order affects memory use.
2. Write a struct to a file twice, from two separate program runs, and `cmp` the
   files. If they differ, you are looking at uninitialised padding.
3. Hand-craft a `vectors.bin` header with a huge `slot_count` and feed it to
   `read_vector_file`. Confirm it raises `CorruptionError` rather than
   attempting the allocation. Then delete the overflow check in
   `checked_multiply` and try `slot_count = 2^40`, `dimension = 60000`.
4. Corrupt one byte inside the payload region. Note that loading still succeeds
   (structural validation cannot see it) and that
   `verify_vector_file_payload` catches it. Decide for yourself whether the
   default is the right one.
5. Add a field to the header without bumping `kVectorFileVersion`, write a file,
   then read it with a build that has the old layout. Watch the failure mode,
   then bump the version and watch the difference.

## Where this lives in the code

- `include/vectordb/persistence/byte_io.hpp` — `ByteWriter`, `ByteReader`,
  the `static_assert` on endianness, and CRC-32
- `src/persistence/byte_io.cpp` — the shift-and-mask implementations, the
  compile-time CRC table
- `include/vectordb/storage/vector_file.hpp` — the format constants
- `src/storage/vector_file.cpp` — the write path, and the nine-step validation
  ladder in `parse_vector_file_header`
- `tests/unit/persistence_byte_io_test.cpp` — round trips, bounds checking,
  the CRC-32 standard check value
- `tests/unit/storage_vector_file_test.cpp` — one named test per corruption mode
- `docs/vector-storage.md` — the format specification

---

Next: [03-memory-mapped-files.md](03-memory-mapped-files.md)
