# Architecture Decision Records

Each record captures one decision that was expensive to make and would be
expensive to reverse: what we chose, what we rejected, and — most importantly —
**what it costs us**. A decision recorded without its cost is marketing.

Format: Decision · Context · Options considered · Chosen approach · Why ·
Trade-offs · Consequences · Alternatives for future versions.

| # | Decision | Status |
|---|---|---|
| [0001](ADR-0001-implement-algorithms-ourselves.md) | Implement the search algorithms ourselves rather than wrap FAISS/hnswlib | Accepted |
| [0002](ADR-0002-fixed-dimension.md) | One fixed vector dimension per database | Accepted |
| [0003](ADR-0003-id-model.md) | Two id types: stable `VectorId`, dense `LocalId` | Accepted |
| [0004](ADR-0004-contiguous-storage.md) | Store vectors in one contiguous block, stride = dimension | Accepted |
| [0005](ADR-0005-float32.md) | `float32` is the only element type in v1 | Accepted |
| [0006](ADR-0006-brute-force-first.md) | Build brute force first and keep it as the correctness oracle | Accepted |
| [0007](ADR-0007-nan-policy.md) | Reject non-finite components at insertion | Accepted |
| [0008](ADR-0008-validate-on-load.md) | Validate every persisted artefact before trusting it | Accepted |
| [0009](ADR-0009-exceptions.md) | Report failures with exceptions, not error codes | Accepted |
| [0010](ADR-0010-sqlite-for-metadata.md) | SQLite for metadata, our own format for vectors | Accepted |
