# Bytes, and what a float actually is

🟢 Foundational · prerequisite for [50-storage/02](../50-storage/02-binary-file-formats.md)

## A file is a sequence of bytes

That is all. No types, no structure — any meaning is a convention between the
writer and the reader, and **your format documentation is that convention**.

## `std::byte`, not `char`

```cpp
std::vector<std::byte> buffer;      // ✅ "raw bytes"
std::vector<char> buffer;           // is it text?
std::vector<uint8_t> buffer;        // is it numbers?
```

`std::byte` supports only bitwise operations. It cannot be accidentally used as
a number or printed as text — the type says "these are raw bytes and nothing
else", and the compiler enforces it.

## What a `float` is

IEEE-754 `binary32`: 32 bits, split into sign, exponent and significand.

```
 s | eeeeeeee | mmmmmmmmmmmmmmmmmmmmmmm
 1 |    8     |           23
```

`1.0f` is `0x3F800000`. Not "a number the computer stores somehow" — a specific
32-bit pattern you can print, compare and write to a file.

```cpp
std::uint32_t bits = std::bit_cast<std::uint32_t>(1.0F);   // 0x3F800000
```

Consequences worth carrying:

- **`0.1f` is not 0.1.** It is the nearest representable value, ~0.100000001.
  This is why float comparisons use tolerances.
- **Two zeros exist**, `+0.0` and `-0.0`. They compare equal and have different
  bit patterns — which is why the serialization test asserts bit equality rather
  than `==`.
- **NaN compares false against everything**, including itself. The single fact
  behind [ADR-0007](../../docs/decisions/ADR-0007-nan-policy.md).

## `std::bit_cast`, not `reinterpret_cast`

```cpp
std::uint32_t bits = std::bit_cast<std::uint32_t>(value);    // ✅ defined
std::uint32_t bits = *(std::uint32_t*)&value;                // ❌ UB (aliasing)
union { float f; uint32_t i; } u;                            // ❌ UB in C++
```

Only `bit_cast` (C++20) is actually defined. The other two work in practice on
most compilers, which is worse than failing — they are the kind of thing that
breaks when you change optimisation level.

## Endianness

`0x11223344` in memory:

```
little-endian (x86, ARM):  44 33 22 11
big-endian:                11 22 33 44
```

Writing the raw bytes bakes in your CPU's convention without recording it.

VectorDB writes explicitly:

```cpp
void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}
```

Shifts and masks do the same thing on every machine regardless of native order.

There is one deliberate exception — the bulk float payload goes out as one
contiguous `write()`, because 768 million shifts would be absurd. It is safe
because a `static_assert` fails the build on a big-endian host. **The exception
is narrow, explicit, and enforced by the compiler.**

## Alignment

Some types must sit at an address that is a multiple of their size.

```cpp
const auto* floats = reinterpret_cast<const float*>(bytes.data() + offset);
```

Legal only if `bytes.data() + offset` is 4-byte aligned. This is why the vector
file's header is exactly 64 bytes — so the payload starts page- and
cache-line-aligned, and this cast is defined.

Misaligned access is undefined behaviour; on x86 it merely runs slower, and on
some architectures it faults.

## Padding

```cpp
struct Header { uint32_t a; uint64_t b; };   // sizeof == 16, not 12
```

The compiler inserts 4 bytes so `b` is 8-byte aligned. Those bytes are **never
initialised** — they hold whatever was on the stack.

So dumping a struct to a file produces output that differs between runs with
identical data, and leaks stack memory into a file you might share.

That is the first of four reasons VectorDB never dumps a struct. The others are
in [50-storage/02](../50-storage/02-binary-file-formats.md).

## Checksums

CRC-32 detects truncation, byte swaps and single-bit flips. It is a **checksum**,
not a cryptographic hash: it catches accidents, not tampering.

That is the right tool here — the failure we guard against is a half-written file
or a flipped bit, not an adversary. A `static_assert`-worthy distinction: using
SHA-256 here would be slower and would not make the file more trustworthy in any
way that matters.

## Hex dumps

```sh
xxd vectors.bin | head -5
xxd -s 64 -l 32 vectors.bin      # the first eight floats
```

```
00000000: 5644 4256 4543 0000 0100 0000 8000 0000  VDBVEC..........
```

Eight bytes of readable ASCII magic is worth a great deal at 2 a.m., which is why
the magic is text rather than a random 32-bit constant.

## Experiments

1. Print the bit pattern of `0.1f`, `-0.0f`, `1.0f/3.0f` and a NaN. Confirm
   `nan == nan` is false.
2. `sizeof` the struct above, then reorder the fields largest-first and print it
   again.
3. Write a struct to a file twice from two program runs and `cmp` them.
4. `xxd` a `vectors.bin` and identify every header field from
   [`docs/vector-storage.md`](../../docs/vector-storage.md).
5. Flip one bit in the payload with `dd` and confirm `check --deep` catches it and
   `check` does not.

## Where this lives in the code

- `include/vectordb/persistence/byte_io.hpp` — the endianness `static_assert`
- `src/persistence/byte_io.cpp` — shift/mask serialization, the CRC table
- `src/storage/vector_file.cpp` — the aligned payload cast
- `tests/unit/persistence_byte_io_test.cpp` — bit-exact float round trips

---

Next: [06-system-calls.md](06-system-calls.md)
