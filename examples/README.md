# Examples

Runnable end-to-end demonstrations. Every one generates its own data — nothing
downloads, and nothing is committed.

Build first:

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset release && cmake --build --preset release
```

Then run any example from the repository root:

```sh
./examples/01-quickstart.sh
```

Each script writes into `examples/out/` and cleans up after itself. They use
`set -e`, so any failure stops the script — which makes them a rough smoke test
as well as documentation.

| Example | Shows |
|---|---|
| [01-quickstart.sh](01-quickstart.sh) | create, insert, search, get, delete — the whole loop in 4 dimensions |
| [02-metadata-filtering.sh](02-metadata-filtering.sh) | typed metadata, filter expressions, and where post-filtering breaks down |
| [03-persistence.sh](03-persistence.sh) | data surviving separate processes, and index rebuild after corruption |
| [04-hnsw-vs-brute-force.sh](04-hnsw-vs-brute-force.sh) | the same data in both index types, timed |
| [05-efsearch-recall.sh](05-efsearch-recall.sh) | turning the recall/latency dial and watching results change |
| [06-bulk-import.sh](06-bulk-import.sh) | generating, importing and exporting 20,000 vectors |
| [07-deletion-and-compaction.sh](07-deletion-and-compaction.sh) | tombstones, `check` warnings, and reclaiming space |

For measured numbers rather than demonstrations, see
[`docs/benchmark-results.md`](../docs/benchmark-results.md) and run
`./build/release/bin/vectordb-bench`.
