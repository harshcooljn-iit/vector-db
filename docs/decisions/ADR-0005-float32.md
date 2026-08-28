# ADR-0005 — `float32` is the only element type in v1

**Status:** Accepted

## Decision

Vector components are IEEE-754 single-precision floats. No `double`, no
`float16`/`bfloat16`, no integer quantization.

## Context

The element type sets the memory footprint, the bandwidth cost of every scan,
and the achievable SIMD width. For N × D floats it is the dominant term in the
entire system's memory use.

## Options considered

| Type | Bytes/component | 1M × 768 | Notes |
|---|---|---|---|
| `double` | 8 | 6.1 GB | Precision no embedding model provides |
| `float32` | 4 | 3.1 GB | What every model actually emits |
| `float16` | 2 | 1.5 GB | Needs hardware support or conversion cost |
| `int8` (quantized) | 1 | 0.77 GB | Lossy; needs calibration and a rescoring pass |

## Chosen approach

`float32`.

## Why

- **It is what the data already is.** Embedding models emit `float32`. Storing
  `double` would double memory and bandwidth to preserve precision that was
  never in the input — the numbers are model outputs with maybe 3 significant
  digits of meaning, not measurements.
- **Distance ranking does not need more.** We are comparing distances to pick a
  top-10. `float32` gives ~7 decimal digits; ties that fall inside that margin
  are ties in any meaningful sense.
- **SIMD width.** A 128-bit NEON register holds 4 `float32` lanes; a 256-bit
  AVX2 register holds 8. Doubles halve both.
- **`float16` is a real option and is deferred, not dismissed.** ARM has native
  `float16` arithmetic; x86 mostly has conversion instructions rather than
  arithmetic. Supporting it well means a second set of kernels, a second file
  format variant and a per-database element-type field. That is a feature, not
  a detail, and v1 has enough features.

## Trade-offs

| Cost | Benefit |
|---|---|
| 2× the memory of `float16` | One kernel implementation per metric per ISA |
| 4× the memory of `int8` | No accuracy loss, no calibration step |
| No precision headroom for iterative algorithms | Matches the input exactly; no conversion on load |

We do not run iterative numerical algorithms on these vectors — the operations
are a dot product, a sum of squares, and a comparison — so the lack of
precision headroom costs nothing in practice.

## Consequences

- Accumulation precision becomes a real question even in `float32`, and the
  answer differs by context. `l2_norm_squared()` runs once per vector and
  accumulates in `double` because that is free there. The distance kernels run
  once per candidate and accumulate in `float` with multiple independent
  accumulators, which recovers most of the accuracy at no cost. Both choices
  are documented at their call sites.
- Tests compare floats with tolerances, never `==`, except where the value was
  copied rather than computed — a copy that differs is a bug, not drift.
- The file format records the element type explicitly, so adding `float16`
  later is a format version bump rather than a breaking change.

## Alternatives for future versions

- **`float16` storage with `float32` accumulation** — halves memory, and on
  ARM the conversion is a single instruction. The most valuable next step.
- **Scalar or product quantization** with a `float32` rescoring pass over the
  top candidates. This is how production systems fit billions of vectors in
  RAM. See `learnings/next-steps.md`.
