# ADR-0010 — SQLite for metadata, our own format for vectors

**Status:** Accepted

## Decision

Vectors live in a hand-written binary file. Metadata and database configuration
live in SQLite. The two are different files with different lifecycles.

## Context

Both are "data the database must persist", so a single mechanism is tempting.
But they have almost nothing in common:

| | Vectors | Metadata |
|---|---|---|
| Size | GB | MB |
| Shape | one homogeneous float array | heterogeneous key-value |
| Access | bulk sequential scan | point lookup, predicate query |
| Update | append, tombstone | arbitrary per-row edit |
| Needs | mmap, alignment, zero copy | indexes, transactions, queries |

## Options considered

### 1. Everything in SQLite, vectors as BLOBs

Works, and it is what several systems do. But a 3 GB BLOB column is read through
a database API rather than being a file we can `mmap`, so the zero-copy load
disappears. It also puts the vectors behind SQLite's page cache instead of the
OS's, adding a layer of copying and eviction policy that helps nothing here.
And it costs us alignment control, which the direct `const float*` cast depends
on.

### 2. Everything in our own format

Then we write B-tree indexes, a query evaluator, transactions and a WAL — to
end up with a worse SQLite. Metadata is exactly the workload general-purpose
embedded databases are excellent at.

### 3. Split by workload (chosen)

## Why

Each side gets the storage engine its access pattern actually wants:

- **Vectors** need the file layout to *be* the memory layout, so a load maps and
  uses the floats in place. That is the property
  [ADR-0004](ADR-0004-contiguous-storage.md) refused to pad the stride for, and
  it is unavailable through any general-purpose store.
- **Metadata** needs `WHERE category = 'finance' AND year >= 2020` to use an
  index, updates to be transactional, and the schema to survive a crash. Writing
  that ourselves would be months of work to reach parity.

SQLite is also the right *kind* of dependency: a single C file, no server, no
configuration, in the public domain, and among the most thoroughly tested pieces
of software in existence. It does not violate
[ADR-0001](ADR-0001-implement-algorithms-ourselves.md) — that rule is about
implementing the *search algorithms* ourselves, and metadata storage is not one.

## Trade-offs

| Cost | Benefit |
|---|---|
| Two files must stay consistent | Each gets the engine its workload wants |
| A cross-cutting query needs both | mmap-able vectors, indexed metadata |
| Two durability models to reason about | SQLite's transactions for free |
| A dependency | Not writing a B-tree |

The consistency cost is the real one. Deleting a vector means removing it from
two stores, and a crash between them leaves orphaned metadata rows. This is
handled rather than ignored: the vector store is authoritative, metadata rows
referencing an absent vector are detected by `vectordb check`, and
`MetadataStore::remove_orphans` cleans them up. Orphans are inert — nothing
reads a metadata row whose vector does not exist — so this is a tidiness
problem, not a correctness one.

## Consequences

- The SQLite wrapper header lives under `src/storage/`, not
  `include/vectordb/`, and SQLite is linked `PRIVATE`. Nothing above the
  metadata store can see it even by accident, which turns "HNSW must not know
  about SQLite" into a compile error rather than a code-review comment.
- Database configuration (dimension, metric, index type, HNSW parameters) lives
  in the SQLite file too, in `schema_info`. One less format to version, and it
  is transactional for free.
- SQLite runs in WAL mode with `synchronous = NORMAL`: durable against process
  crash, and able to lose the most recent transactions on a power cut. Stated
  in `docs/persistence.md` rather than assumed.

## Alternatives for future versions

- **Metadata in the vector file's header region** for very small databases,
  avoiding the second file entirely.
- **A columnar metadata store** if filtering ever becomes the bottleneck; the
  EAV layout is the first thing that would have to go.
