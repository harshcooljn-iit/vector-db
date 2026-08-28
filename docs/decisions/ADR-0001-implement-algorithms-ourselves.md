# ADR-0001 — Implement the search algorithms ourselves

**Status:** Accepted

## Decision

VectorDB implements distance kernels, top-k selection, brute-force search and
HNSW from scratch. FAISS, hnswlib, ScaNN, Annoy and every other ANN library are
excluded from the dependency set. External libraries are used only for logging
(spdlog), JSON (nlohmann), metadata storage (SQLite) and testing (GoogleTest).

## Context

Every one of those libraries is faster and more battle-tested than what we will
write. A production system with a deadline should use one.

This project has a different objective: it is simultaneously a working database
and a teaching artefact. The interesting engineering — memory layout, graph
construction, neighbour pruning, serialization, crash safety, concurrency — is
exactly what a library encapsulates and hides.

## Options considered

1. **Wrap hnswlib.** Working ANN search in an afternoon. Teaches an API.
2. **Wrap FAISS.** Also gets quantization and GPU support. Very large
   dependency; the abstraction gap between FAISS's index model and ours would
   itself become the design problem.
3. **Implement it ourselves.** Slow, error-prone, educational.

## Chosen approach

Option 3, with one mitigation that makes it safe: **brute force is implemented
first and never removed.** It is exact by construction, so every approximate
result can be measured against ground truth rather than against hope. See
[ADR-0006](ADR-0006-brute-force-first.md).

## Why

- The stated deliverable is understanding, and you cannot understand a graph
  index by calling `index.addPoint()`.
- Owning the code means owning the file format, the concurrency model and the
  failure modes. A wrapper would have forced us to adopt hnswlib's persistence
  format and its threading assumptions, which would have removed most of the
  systems content.
- It keeps the dependency surface tiny, which makes a clean checkout build in
  minutes rather than tens of minutes.

## Trade-offs

| We give up | We gain |
|---|---|
| Peak performance — a mature library will beat us | Every layer is inspectable and modifiable |
| Years of correctness hardening | A measured recall number for our own code |
| Quantization, GPU, IVF | A small, comprehensible codebase |
| Development speed | The actual point of the project |

We will be slower than hnswlib. We will say so, with numbers, rather than
implying otherwise.

## Consequences

- Correctness is not assumable, so it must be measured: recall against brute
  force is a first-class test, not an afterthought.
- SIMD kernels must be validated against scalar kernels, because we wrote both.
- Any performance claim in this repository requires a measurement in
  `docs/benchmark-results.md`.

## Alternatives for future versions

A `--index=external` backend delegating to hnswlib would make an excellent
benchmark *baseline* — "here is how far our implementation is from a mature
one" is a far more useful number than an absolute latency. It would live behind
the existing index interface and change nothing else.
