# Data interchange formats

How vectors get in and out. These are **not** the database's internal formats —
see [vector-storage.md](vector-storage.md) for that.

Implementation: `src/util/vector_io.cpp`.

## Why a separate format at all

`vectors.bin` carries tombstones, cached norms and internal slot ordering. None
of that means anything to another program, and exposing it would tie an external
tool to our internals.

An interchange format should be the smallest thing that round-trips the data: a
header, a float matrix, and optionally ids.

## `.vecs` — binary matrix

The format for bulk data.

```
offset  size                    field
------  ----------------------  ----------------------------------
     0  8                       magic "VDBDATA\0"
     8  4                       version (u32) = 1
    12  4                       dimension (u32)
    16  8                       count (u64)
    24  4                       flags (u32): bit 0 = ids present
    28  4                       header_crc32 over [0, 28)
------  ----------------------  ----------------------------------
    32  count*dimension*4       float32, row-major
     P  count*8                 VectorId (u64), if flags bit 0
```

Validated like every other format: magic, version, header CRC, range checks,
overflow-checked size arithmetic, and an exact file-size comparison that
distinguishes truncation from trailing data.

```sh
vectordb generate --output data.vecs --dimension 128 --count 100000 --seed 42
vectordb import mydb --input data.vecs
vectordb export mydb --output out.vecs        # carries ids
```

`export` writes **live vectors only, in id order** — no tombstones, no trace of
internal slot ordering.

## CSV — for examples and eyeballing

```
# VectorDB CSV: id,dimension 4
1,0.1,0.2,0.3,0.4
2,0.5,0.6,0.7,0.8
```

Blank lines and `#` comments are skipped, so a file can carry a header.

```sh
vectordb generate --output small.csv --dimension 4 --count 10 --csv
vectordb import mydb --input small.csv --csv           # no ids
vectordb import mydb --input small.csv --csv --csv-ids # first field is the id
```

**Nine significant digits** on write, which is what a float needs to round-trip
exactly. Fewer would make an export/import cycle quietly lossy — a nasty
surprise to discover later.

**Roughly 12× larger than binary, plus parsing.** Use it for examples. Never for
bulk data.

## Command-line literals

```sh
vectordb insert mydb --vector "0.1,0.2,0.3,0.4"
vectordb insert mydb --vector "0.1 0.2 0.3 0.4"
vectordb insert mydb --vector "[0.1, 0.2, 0.3, 0.4]"
```

Commas, spaces and brackets are all accepted. A parse error names the component
that failed, not just "bad vector".

## Converting from elsewhere

**From `.fvecs`** (the ANN-benchmarks convention: an `int32` dimension prefix per
vector) — a short Python script suffices; the layouts differ only in the header.

**From NumPy**:

```python
import numpy as np, struct
a = np.load("embeddings.npy").astype(np.float32)   # (N, D)
with open("data.vecs", "wb") as f:
    header = bytearray(b"VDBDATA\0")
    header += struct.pack("<IIQI", 1, a.shape[1], a.shape[0], 0)
    header += b"\0" * 4                             # CRC placeholder
    # ... compute CRC-32 over header[:28] and patch bytes 28:32 ...
    f.write(header)
    f.write(a.tobytes())
```

Or, more simply, write CSV and import that — correctness first, and you can
switch to binary once it works.

## Choosing

| | `.vecs` | CSV |
|---|---|---|
| Size | 1× | ~12× |
| Parse cost | none | real |
| Human-readable | no | yes |
| Ids | optional | optional |
| Use for | bulk data | examples, debugging |
