# Exercise 9 — Corrupt a file

🟡 Intermediate · [ADR-0008](../../docs/decisions/ADR-0008-validate-on-load.md), [binary formats](../50-storage/02-binary-file-formats.md)

## Goal

Find out what validation catches, what it misses, and whether the gaps are the
right ones.

## Setup

```sh
vectordb create victim.vdb --dimension 8 --metric l2
vectordb generate --output data.vecs --dimension 8 --count 1000
vectordb import victim.vdb --input data.vecs
cp -r victim.vdb pristine.vdb        # so you can restore
```

Damage bytes with:

```sh
printf '\xff' | dd of=victim.vdb/vectors.bin bs=1 seek=<OFFSET> count=1 conv=notrunc
```

After each, run `vectordb check victim.vdb`, then `--deep`, then a search.
Restore from `pristine.vdb` between experiments.

## Part A: the header

The layout is in [`docs/vector-storage.md`](../../docs/vector-storage.md).

| Offset | Field | Try |
|---|---|---|
| 0 | magic | change one byte |
| 8 | version | set to 99 |
| 12 | dimension | set to 200 |
| 16 | slot_count | set the high byte to 0xFF |
| 60 | header CRC | change one byte |

**For each: predict the error message, then read the real one.**

**Questions.** Which failures are caught by the CRC and which by a range check?
Why is the version checked *before* the CRC? (Hint: a file from a newer build
fails both — which message helps the user more?)

## Part B: the payload

Damage a byte at offset 100, inside the float array.

**Questions.** Does `check` catch it? Does `check --deep`? Does the database
still open and search? Is a returned distance wrong, and by how much?

This is the **documented gap**: payload checksums are not verified on open,
because a full pass over 3 GB costs seconds to catch corruption that shifts one
distance slightly. Now that you have seen the failure mode — do you agree with
the trade?

## Part C: truncation and trailing data

```sh
truncate -s -100 victim.vdb/vectors.bin     # or use dd on macOS
echo "extra" >> victim.vdb/vectors.bin
```

**Questions.** Are these distinguished in the error message? Why does that
matter to someone debugging a failed copy?

## Part D: the index

Damage `index.hnsw`, first in the header and then inside a neighbour list.

**Questions.** Does the database still open? What does the log say with
`--verbose`? Why is a corrupt index handled so differently from a corrupt vector
file?

Now change a neighbour id to a value **larger than the node count**. Reading
`read_hnsw_file` may help you find the offset.

**Questions.** What catches it? What would happen at query time if it did not?
That validation pass is `O(edges)` — is it worth it?

## Part E: metadata

```sh
printf 'garbage' | dd of=victim.vdb/metadata.sqlite bs=1 seek=50 count=7 conv=notrunc
```

**Questions.** What fails, and where does the error come from — us or SQLite? Is
the message useful? Should VectorDB wrap it?

## Part F: the one that is not caught

Find a single-byte change that VectorDB does **not** detect and that changes a
search result.

**Questions.** How much would it cost to catch it? Design the cheapest mechanism
that would. (`docs/vector-storage.md` names one under future work.) Would you
ship it on by default?

## Where to look

- `src/storage/vector_file.cpp` — `parse_vector_file_header`, nine validation steps
- `src/index/hnsw/hnsw_file.cpp` — reference validation
- `src/db/database.cpp` — `check`, and the open-time rebuild
- `tests/unit/storage_vector_file_test.cpp` — one test per corruption mode
