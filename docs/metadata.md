# Metadata and filtering

Implementation: `src/storage/metadata_store.cpp`, `src/storage/filter.cpp`.
Decision record: [ADR-0010](decisions/ADR-0010-sqlite-for-metadata.md).

## The value model

Four types, which are SQLite's storage classes minus BLOB:

| Type | C++ | Example |
|---|---|---|
| integer | `std::int64_t` | `year = 2026` |
| real | `double` | `score = 0.87` |
| text | `std::string` | `category = "finance"` |
| null | `std::monostate` | key present, no value |

BLOB is excluded deliberately. Metadata is for attributes you filter and
display; large binary payloads belong in the vector store or outside the
database.

A vector's metadata is a `std::map` — ordered, so JSON output, `vectordb get`
and every test expectation are in a stable key order.

## Schema

```sql
CREATE TABLE metadata (
    vector_id  INTEGER NOT NULL,
    key        TEXT    NOT NULL,
    value_type INTEGER NOT NULL,   -- 0 null, 1 integer, 2 real, 3 text
    int_value  INTEGER,
    real_value REAL,
    text_value TEXT,
    PRIMARY KEY (vector_id, key)
) WITHOUT ROWID;

CREATE INDEX idx_metadata_key_text ON metadata(key, text_value) WHERE text_value IS NOT NULL;
CREATE INDEX idx_metadata_key_int  ON metadata(key, int_value)  WHERE int_value  IS NOT NULL;
CREATE INDEX idx_metadata_key_real ON metadata(key, real_value) WHERE real_value IS NOT NULL;

CREATE TABLE schema_info (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
```

### Why entity-attribute-value

Users define their own attributes. A column-per-attribute table would need
`ALTER TABLE` at runtime and a migration for every new key someone invents.

EAV trades query efficiency for a schema that never changes. The cost is real —
reconstructing one vector's metadata reads several rows instead of one — and it
is bounded by two things: `WITHOUT ROWID` makes the primary key the storage
order, so those rows are physically adjacent and read as one range scan; and
metadata objects are small.

### Why `value_type` is stored explicitly

SQLite is dynamically typed. Without the type column, `"2026"` stored as text
would come back indistinguishable from `2026` stored as an integer, and
`year == "2026"` would silently match `year == 2026`. There is a test named for
exactly this.

### Why three narrow indexes

A filter is always `key = ? AND <typed column> <op> ?`. A query only ever
touches the column matching the value's type, so three partial indexes are used
where one wide index would mostly be dead weight.

## The filter language

```
filter    := condition ( "and" condition )*
condition := key op value
op        := "==" | "!=" | ">=" | "<=" | ">" | "<" | "="
value     := number | quoted-string | bare-word
```

```
category == "finance" and year >= 2020 and score < 0.5
```

### What it deliberately does not have

**No `or`.** No nesting, no parentheses, no negation of groups.

The useful 95% of vector-search filtering is equality and range predicates ANDed
together — a narrowing of the search space. Every construct beyond that adds a
parser case, an evaluator case, an index-selection consideration and a class of
bug, and this is a vector database rather than a SQL engine. Attempting `or`
produces an error that says so and points here.

### Values and quoting

A bare value that parses as an integer becomes an integer; as a float, a real;
otherwise text. Quoting forces text.

```
year == 2026      matches the integer 2026
year == "2026"    matches the string "2026"
```

That distinction is the entire reason quoting exists, and it is tested.

### Comparison rules

| Stored | Filter | Behaviour |
|---|---|---|
| integer | integer or real | numeric comparison |
| real | integer or real | numeric comparison |
| text | text | lexicographic |
| anything | different type class | **never matches** |
| *key absent* | anything | **never matches** |

Two rules that are worth stating explicitly because both are choices, not
inevitabilities:

**A missing key never matches, `!=` included.** SQL's three-valued logic would
say `missing != "x"` is unknown; some systems say it is true. We say false, so
that a filter's meaning does not depend on which keys happen to exist. With
sparse metadata, the alternative is deeply surprising: `category != "finance"`
would return every vector that has no category at all.

**Cross-type comparison never matches.** Comparing a number with a string has no
defensible answer. SQL invents one by sorting values into type classes; the
result is behaviour nobody predicts.

If you need "has no category", that is a different feature — a `key exists`
predicate — and it is not in v1.

## Filter evaluation

`MetadataStore::matching()` streams every metadata row in `(vector_id, key)`
order, reassembles one vector's metadata at a time, and evaluates the filter in
C++.

### Why not generate SQL?

Pushing the predicate into SQL would use the indexes, and for a large database
that is the right answer. It is not what we do, for three reasons:

1. **EAV makes it ugly.** Each condition needs its own self-join or `EXISTS`
   subquery. Three conditions, three joins.
2. **It means generating SQL from user input.** Parameters are bound, so this is
   not an injection risk, but it is considerably more machinery.
3. **It creates a second evaluator.** Search-time filtering needs a C++
   evaluator regardless (you cannot ask SQLite about a candidate mid-traversal).
   Two evaluators that must agree on every edge case — missing keys, cross-type
   comparison, numeric coercion — is a durable source of bugs. One evaluator,
   used in both places, cannot disagree with itself.

This is a deliberate simplicity-over-speed trade, recorded here rather than
discovered later. If metadata filtering becomes a bottleneck, pushing the
*first* condition into SQL as a pre-filter — and keeping the C++ evaluator as
the authority — is the natural next step.

## Transactions

`in_transaction(body)` wraps a lambda. The transaction rolls back unless
`commit()` is reached, so an exception unwinding past it discards the partial
write. Calls nest: `set()` opens one internally and composes with a caller's.

This is a performance lever as much as a correctness one. SQLite commits each
unbatched statement separately, which is a durability round trip per statement.
Wrapping a bulk load in one transaction is routinely a 100× difference.

## Consistency with the vector store

Deleting a vector touches two stores, and a crash between them leaves metadata
rows whose vector no longer exists.

Those rows are **inert** — nothing reads metadata for an absent vector — so this
is a tidiness problem rather than a correctness one. It is still handled:

- `vectordb check` reports orphaned metadata rows;
- `MetadataStore::remove_orphans` deletes them;
- the vector store is authoritative, so the reconciliation direction is never
  ambiguous.

## SQLite configuration

```sql
PRAGMA journal_mode = WAL;       -- readers proceed during a write
PRAGMA synchronous  = NORMAL;    -- sync at checkpoints, not every commit
PRAGMA foreign_keys = ON;        -- off by default, which surprises everyone
PRAGMA temp_store   = MEMORY;
```

`synchronous = NORMAL` with WAL is durable against **process crash** and can
lose the most recent transactions on a **power cut**. That is the standard
trade for a read-mostly workload, and it is stated here rather than assumed.

A 5-second busy timeout means a statement that cannot immediately get its lock
waits rather than failing with `SQLITE_BUSY` under trivial contention.
