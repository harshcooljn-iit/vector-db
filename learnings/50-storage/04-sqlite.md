# SQLite — using a database inside your database

🟡 Intermediate · pairs with `src/storage/sqlite.cpp` and `metadata_store.cpp`

## Wait, we're using a database to build a database?

Yes, and the split is the interesting part.

We hand-wrote the vector format because vectors are 3 GB of homogeneous floats
that must be memory-mappable — a job no general-purpose store does well. We use
SQLite for metadata because metadata is small, heterogeneous, and queried by
predicate — a job SQLite does far better than we would.

```
vectors.bin        our format     3 GB    mmap'd, scanned linearly
metadata.sqlite    SQLite         2 MB    indexed, queried, transactional
```

**Match the storage engine to the access pattern.** Using one mechanism for both
would mean either losing zero-copy loads or writing a B-tree.

This does not contradict "implement the algorithms ourselves"
([ADR-0001](../../docs/decisions/ADR-0001-implement-algorithms-ourselves.md)).
That rule is about the *search algorithms*. Storing key-value rows is not one,
and writing our own would be months of work to arrive at a worse SQLite.

## SQLite is not a server

No process, no port, no configuration. It is a C library that reads and writes
one file. `sqlite3_open("foo.db")` and you have a transactional, indexed,
crash-safe store.

Which is also VectorDB's model — one process, files on disk. Same philosophy,
different data.

## The C API, and what RAII is for

```c
sqlite3* db;
sqlite3_open_v2(path, &db, flags, NULL);
sqlite3_stmt* stmt;
sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
sqlite3_bind_int64(stmt, 1, 42);
while (sqlite3_step(stmt) == SQLITE_ROW) { ... }
sqlite3_finalize(stmt);
sqlite3_close(db);
```

Every one of those returns a code you must check, and every resource must be
released on **every** path — including the one where a middle step throws.

```cpp
class Statement {
    ~Statement() { sqlite3_finalize(statement_); }
};
```

Wrapping it costs about 150 lines and buys: no leaked statements on an exception
path, no unchecked return code (each wrapper throws `MetadataError` with
SQLite's own message attached), and a compile error instead of a segfault if
someone copies a handle.

One subtlety worth stealing: the connection uses `sqlite3_close_v2`, not
`sqlite3_close`. `close_v2` tolerates outstanding statements by deferring the
actual close, which makes destruction order between a connection and its
statements a non-issue rather than a latent crash.

## Prepared statements: two reasons, and the second is the important one

```cpp
insert_value = std::make_unique<sql::Statement>(connection,
    "INSERT OR REPLACE INTO metadata(...) VALUES(?, ?, ?, ?, ?, ?)");
```

**Speed.** `prepare` is where SQLite parses the SQL and plans the query.
Preparing once and re-binding turns an insert loop into an insert loop;
preparing per row turns it into a compile loop.

**Safety.** Parameters are *bound*, never concatenated. So this:

```cpp
store.set(1, {{"note", "'; DROP TABLE metadata; --"}});
```

stores a 26-character string. There is a test named for it. With string
concatenation it would be a syntax error at best.

The general rule, worth carrying to every language you use: **data never becomes
syntax.** The moment user input is concatenated into a query, you have handed
the user your parser.

Note also `SQLITE_TRANSIENT` in `bind_text`. It tells SQLite to copy the bytes,
so the caller's buffer need not outlive the step. `SQLITE_STATIC` skips the copy
and is a reliable source of use-after-free.

## Transactions

```cpp
store.in_transaction([&] {
    for (id : ids) store.set(id, metadata[id]);
});
```

**Correctness**: all of it lands or none of it does. If the lambda throws, the
`Transaction` destructor rolls back.

Note which way the default points:

```cpp
~Transaction() { if (!finished_) rollback(); }
```

Rollback is what happens if you do nothing. You have to *ask* for the change to
persist. The opposite default — commit on destruction — turns every forgotten
error path into silent partial data.

**Speed**: this is where the surprise is. SQLite commits every unbatched
statement separately, and a commit means a durability round trip. Inserting
10,000 rows without a transaction is 10,000 fsyncs. Inside one, it is one. A
100× difference is routine, and it is the single most common reason people
conclude "SQLite is slow".

## Designing the schema: EAV

Users invent their own metadata keys. So this does not work:

```sql
CREATE TABLE metadata (vector_id INTEGER, category TEXT, year INTEGER, ...);
```

You would need `ALTER TABLE` at runtime, and a migration for every key anyone
thinks of.

**Entity-Attribute-Value** instead — one row per (vector, key):

```sql
CREATE TABLE metadata (
    vector_id, key, value_type, int_value, real_value, text_value,
    PRIMARY KEY (vector_id, key)
) WITHOUT ROWID;
```

| Gain | Cost |
|---|---|
| Any key, no schema change | one vector's metadata is several rows |
| Sparse data costs nothing | filters need self-joins to push into SQL |
| Trivially extensible | a type column instead of real typing |

EAV has a bad reputation, largely from people using it where a fixed schema was
known in advance. Here the schema genuinely is not known, which is the case it
was designed for.

### `WITHOUT ROWID` is doing real work

Normally SQLite keeps a hidden 64-bit `rowid` as the true key, and your declared
primary key becomes a *separate index* pointing at it. Reading by primary key is
then two lookups: index, then row.

`WITHOUT ROWID` makes the primary key the storage order itself. Since ours is
`(vector_id, key)`, all of one vector's metadata is physically adjacent — so
`get(id)` is one contiguous range scan, not one index probe per key. That
directly repays EAV's main cost.

### The type column, and why SQLite forces it

SQLite is **dynamically typed**: a column declared `TEXT` will happily hold an
integer, and the type is a property of the value, not the column.

Without an explicit `value_type`, `"2026"` stored as text and `2026` stored as
an integer would be indistinguishable on read, and `year == "2026"` would
silently match `year == 2026`. There is a test named
`PreservesTypeRatherThanLettingSqliteCoerceIt` for exactly this.

## Designing the filter language: what to leave out

```
category == "finance" and year >= 2020 and score < 0.5
```

Six operators, `and` only. **No `or`, no nesting, no parentheses.**

That is not laziness, it is scope. The useful 95% of vector-search filtering is
narrowing predicates ANDed together. Every construct past that adds a parser
case, an evaluator case, an index consideration and a class of bug — and this is
a vector database, not a SQL engine. Typing `or` produces an error that says so
and points at the documentation.

**The general skill: deciding what your language will not do, and saying it out
loud.** A small language you fully implement beats a large one you implement
80% of.

### Two rules that are choices, not inevitabilities

**A missing key never matches — `!=` included.**

```
category != "finance"    // does NOT match a vector with no category
```

SQL's three-valued logic would call that unknown; some systems return true. We
return false, so a filter's meaning does not depend on which keys happen to
exist. With sparse metadata the alternative is startling: that filter would
return every vector that has no category at all.

**Cross-type comparison never matches.** `year > 5` against `year = "hello"` is
false, not an error and not some invented ordering. SQL sorts values into type
classes to get an answer; the result is behaviour nobody predicts.

Both are written down in `docs/metadata.md`, because an undocumented semantic
choice is indistinguishable from a bug.

## Where filters are evaluated, and the trade behind it

`matching()` streams every metadata row, reassembles one vector at a time, and
evaluates the filter **in C++** — not as generated SQL.

That is slower and it uses no index. Three reasons anyway:

1. EAV makes the SQL ugly: one self-join or `EXISTS` per condition.
2. It means generating SQL from user input (safe, since parameters are bound —
   but a lot more machinery).
3. **It would create a second evaluator.** Search-time filtering needs a C++
   evaluator regardless — you cannot ask SQLite about a candidate mid-traversal.
   Two evaluators that must agree on missing keys, cross-type comparison and
   numeric coercion is a durable bug source. One evaluator, used in both places,
   cannot disagree with itself.

This is a real simplicity-over-speed trade, and it is written down rather than
discovered. If filtering ever becomes the bottleneck, pushing the *first*
condition into SQL as a pre-filter — with the C++ evaluator still the authority
— is the natural next step.

## PRAGMAs worth knowing

```sql
PRAGMA journal_mode = WAL;      -- readers proceed during a write
PRAGMA synchronous  = NORMAL;   -- sync at checkpoints, not every commit
PRAGMA foreign_keys = ON;       -- off by default!
```

**WAL** (write-ahead logging): changes append to a separate log rather than
rewriting pages in place, so readers see a consistent snapshot while a writer is
active. Exactly right for a read-mostly system.

**`synchronous = NORMAL`** with WAL: durable against **process crash**, and can
lose the most recent transactions on a **power cut**. `FULL` is safe against
both and much slower. Choose deliberately, and write the choice down —
`docs/persistence.md` states ours.

**`foreign_keys = ON`**: SQLite ignores foreign key constraints by default, for
backwards compatibility reasons from 2009. Declaring them without enabling them
is documentation, not enforcement.

## Experiments

1. Insert 10,000 metadata rows with and without a transaction. Time both. Then
   set `synchronous = OFF` and time again — you have measured what durability
   costs.
2. Store `"2026"` and `2026` for the same key on two vectors, then filter with
   `year == 2026`. Now delete the `value_type` column handling and try again.
3. Run `EXPLAIN QUERY PLAN SELECT ... WHERE key = 'category' AND text_value = 'finance'`
   in the `sqlite3` shell against a generated database. Then drop the index and
   run it again. Compare "SEARCH USING INDEX" with "SCAN".
4. Recreate the `metadata` table *with* a rowid and compare `get(id)` timings at
   100,000 vectors. You are measuring what `WITHOUT ROWID` bought.
5. Try to add `or` to the filter parser. Notice where it stops being a small
   change: the grammar needs precedence, the evaluator needs a tree instead of a
   list, and `to_string()` needs parentheses.

## Where this lives in the code

- `src/storage/sqlite.hpp` — the RAII wrappers. **Under `src/`, not `include/`**,
  so nothing above the metadata store can see SQLite at all; the library links
  it `PRIVATE`. That is how "HNSW must not know about SQLite" becomes a compile
  error rather than a review comment.
- `src/storage/sqlite.cpp` — connection PRAGMAs, prepared statements,
  the rollback-by-default transaction
- `src/storage/metadata_store.cpp` — the schema, the prepared statement set,
  `matching()`
- `include/vectordb/storage/filter.hpp` — the grammar, and what it refuses to do
- `src/storage/filter.cpp` — the parser and the C++ evaluator
- `tests/unit/storage_metadata_store_test.cpp` — type preservation, the
  injection attempt, transaction rollback, orphan cleanup
- `docs/metadata.md`, `docs/decisions/ADR-0010-sqlite-for-metadata.md`

---

Next: [05-index-persistence.md](05-index-persistence.md)
