# ADR-0007 — Reject non-finite components at insertion

**Status:** Accepted

## Decision

A vector containing NaN, `+Inf` or `-Inf` is rejected at the database boundary
with an `InvalidVectorError` naming the offending component index. Non-finite
values never enter storage or an index.

## Context

NaN has a property no other float value has: **every comparison against it is
false**, including `NaN == NaN`. `Inf` is better behaved but arithmetic on it
produces NaN readily (`Inf - Inf`, `Inf × 0`), and squared-distance
computations do exactly that kind of arithmetic.

## What goes wrong if you allow it

This is not a hypothetical tidiness argument. Concretely:

**Top-k selection breaks.** A bounded max-heap keeps the k smallest by
comparing each candidate against the current worst. With `distance = NaN`,
`nan < worst` is false and `nan > worst` is also false. Depending on which
comparison the heap uses, the NaN candidate is either always rejected or
silently swallows a slot it should not hold. `std::priority_queue`'s
`Compare` must impose a strict weak ordering; NaN violates that outright, and
violating it is undefined behaviour, not merely a wrong answer.

**Sorting is undefined behaviour.** `std::sort` with a comparator that is not a
strict weak ordering can read out of bounds. A single NaN distance can crash
the process — or worse, not crash it.

**HNSW's greedy descent stops working.** The search loop is "move to the
neighbour closer than where I am". With NaN, "closer" is false everywhere, so
the traversal either terminates instantly at the entry point (returning
garbage) or, in a variant that checks `>=`, never terminates.

**Graph construction corrupts silently.** During insertion, neighbour selection
ranks candidates by distance. NaN distances scramble that ranking, producing a
graph with poor connectivity — which shows up months later as an unexplained
recall drop that no test reproduces.

The common thread: **nothing throws, nothing logs, results keep coming.** The
database returns plausible wrong answers indefinitely.

## Options considered

1. **Allow, and treat NaN as "infinitely far".** Requires a NaN check in the
   comparison inside the hottest loop in the system.
2. **Allow, and let the caller deal with it.** Undefined behaviour in `std::sort`
   makes this unacceptable regardless of intent.
3. **Reject at insertion.** (chosen)
4. **Sanitize at insertion** — replace non-finite components with 0.

## Chosen approach

Option 3. `validate_vector()` is called at every entry point that accepts
caller-supplied data.

## Why

- **It is the only cheap place to check.** Insertion happens once per vector;
  distance comparison happens billions of times. Moving the check to the
  boundary removes it from the inner loop entirely — and the inner loop can
  then assume a total order, which is what every algorithm above it needs.
- **A NaN in an embedding is always a bug upstream** — a division by a zero
  norm, an overflow, a corrupt file, an untrained model head. Silently
  accepting it hides a bug the caller needs to know about.
- Option 4 was rejected because zeroing a component fabricates data. The
  resulting vector is finite, plausible and wrong, which is the failure mode
  this whole ADR exists to prevent.

## Trade-offs

| Cost | Benefit |
|---|---|
| One pass over each vector at insert (~D comparisons) | The inner loop can assume a total order |
| A caller with a legitimately non-finite vector is stuck | No undefined behaviour in sort or heap |
| Bulk ingestion pays it per vector | Upstream bugs surface immediately, at the source |

The insert-time cost is `O(D)` against an insert that already copies `O(D)`
floats and, for HNSW, performs hundreds of `O(D)` distance computations. It is
noise.

## Consequences

- `validate_vector()` is called at the database boundary, **not** inside
  `VectorArray::push_back`. Internal copies, rebuilds and file loads move data
  that was already validated; re-checking would put an `O(N·D)` pass on every
  rebuild for no benefit.
- Data loaded from a vector file is *not* re-validated per component by default,
  because it was validated on the way in. `vectordb check` does perform the full
  scan, which is the right place for it: an explicit integrity command.
- The error message names the component index and whether it was NaN, `+Inf` or
  `-Inf`, so a user can find the bad row in their input.

## Alternatives for future versions

A `--allow-non-finite` escape hatch for a caller who genuinely wants it, gated
behind a per-database flag that also switches the comparison policy to a
NaN-aware one. Not planned; nobody has asked, and the cost is a branch in the
hot loop for everyone.
