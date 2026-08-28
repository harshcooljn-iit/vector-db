# ADR-0002 — One fixed vector dimension per database

**Status:** Accepted

## Decision

A database declares its dimension at creation time and never changes it. Any
vector of a different length is rejected with an error naming both dimensions.
Vectors are never truncated or zero-padded to fit.

```sh
vectordb create papers --dimension 768
```

## Context

Embeddings come from a model, and a model has one output dimension. Mixing
dimensions in one collection is almost always a bug — someone swapped models
halfway through an ingestion run — and it is meaningless mathematically:
distance between a 768-vector and a 384-vector is not defined.

## Options considered

1. **Variable dimension per vector.** Every vector carries its own length.
2. **Fixed dimension, silently adapt.** Truncate longer inputs, zero-pad
   shorter ones.
3. **Fixed dimension, reject mismatches.** (chosen)

## Chosen approach

Option 3. `VectorArray` takes the dimension in its constructor; every entry
point validates against it.

## Why

- **It makes the layout possible at all.** Fixed dimension is what lets vector
  `i` live at `data() + i * dimension` — a multiplication instead of an offset
  table. That single fact enables contiguous storage, direct memory mapping and
  branch-free SIMD kernels. See [ADR-0004](ADR-0004-contiguous-storage.md).
- **Silent adaptation destroys data.** Truncating a 768-vector to 384 produces
  a *plausible* vector: finite numbers, reasonable norm, real distances. It
  will return results. They will be wrong, and nothing will ever say so.
  Padding is worse: the padded vector's cosine similarity to everything shifts
  in a way that looks like a modelling problem rather than a bug.
- The failure it prevents — swapping embedding models mid-ingestion — is common
  and otherwise invisible for months.

## Trade-offs

| Cost | Benefit |
|---|---|
| Cannot hold multiple models in one database | Layout is a multiplication, not a lookup |
| Changing models requires a new database and a re-ingest | Model mismatch fails loudly and immediately |
| No Matryoshka-style truncated embeddings | No silent data corruption |

The multi-model case is real, and the answer is "use two databases". That is
the same answer SQLite gives to "I want two schemas in one table".

## Consequences

- `DimensionMismatchError` carries `expected()` and `actual()`, so a caller can
  react programmatically and a user can read what happened.
- Dimension is persisted in the database configuration and re-checked on load;
  a vector file whose header disagrees with the config is corruption, not a
  conversion opportunity.
- `kMaxDimension = 65536` bounds it, so a corrupt header cannot trigger a
  multi-gigabyte allocation. See [ADR-0008](ADR-0008-validate-on-load.md).

## Alternatives for future versions

Named *collections* within one database file, each with its own dimension —
which is how production systems solve this. It changes the storage manager and
nothing below it.
