# SIMD — doing four things per instruction

🔴 Advanced · pairs with `src/distance/kernel_neon.cpp`

## The idea

A normal instruction operates on one value:

```
   a0 + b0  →  c0
```

A **SIMD** instruction — Single Instruction, Multiple Data — operates on
several, in one go:

```
   a0 a1 a2 a3            (one 128-bit register, four floats)
 + b0 b1 b2 b3
   ───────────
   c0 c1 c2 c3            one instruction, same latency
```

That is the whole concept. The complexity is in the plumbing around it.

## The registers

Your CPU has a second set of registers, wider than the ordinary ones:

| ISA | Width | float32 lanes |
|---|---|---|
| ARM NEON | 128-bit | 4 |
| x86 SSE | 128-bit | 4 |
| x86 AVX/AVX2 | 256-bit | 8 |
| x86 AVX-512 | 512-bit | 16 |

A NEON `float32x4_t` is not a struct of four floats. It is **one register**, and
`vaddq_f32` is one instruction with the same latency as a scalar add.

## Why distance kernels are the ideal case

SIMD works when the same operation applies to many independent values. Look at
what we do:

```cpp
for (i = 0; i < 768; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
}
```

- **Same operation** every iteration — subtract, multiply, add.
- **No branches** in the loop body.
- **No dependencies between iterations** — element 5 does not need element 4.
- **Contiguous memory**, so one instruction loads four values.
- **768 iterations**, so the setup cost amortises to nothing.

Textbook. And it is not a coincidence: [ADR-0004](../../docs/decisions/ADR-0004-contiguous-storage.md)
chose a contiguous layout partly so this would be possible.

## The four ingredients

### 1. Load

```cpp
float32x4_t va = vld1q_f32(a + i);    // four floats from memory
```

Note this is the **unaligned** load. There is an aligned form that is faster on
older hardware and **faults** on a misaligned address. Since stride equals
dimension, rows are only 4-byte aligned for odd dimensions — so unaligned it is.
On every core we target, an unaligned load that does not straddle a cache line
costs the same anyway.

### 2. Compute

```cpp
float32x4_t d = vsubq_f32(va, vb);    // four subtractions
sum = vfmaq_f32(sum, d, d);           // four fused multiply-adds
```

**FMA** computes `sum + d*d` as one instruction with **one** rounding step
instead of two. It is therefore slightly *more* accurate than a separate
multiply and add — the intuition usually runs the other way, so it is worth
saying twice.

### 3. Accumulate, in more than one chain

```cpp
float32x4_t sum0, sum1, sum2, sum3;   // four registers, 16 lanes total
```

This is the same reason the scalar kernel uses four accumulators: an FMA has
several cycles of *latency* but can be *issued* every cycle, so a single
accumulator waits on its own previous result and leaves most of the pipeline
idle.

SIMD and multiple accumulators are **orthogonal wins**. Lanes make each
operation wider; accumulators keep the pipeline full. You want both, and the
scalar kernel already has the second — which is why the measured speedup below
is the genuine lane-width win rather than a comparison against a naive loop.

### 4. Reduce

```cpp
float total = vaddvq_f32(vaddq_f32(vaddq_f32(sum0, sum1), vaddq_f32(sum2, sum3)));
```

Eventually you need one number. Combine the four registers pairwise (a tree, not
a chain), then sum the four lanes within one register.

This step is **not free**, which is why you do it once at the end and never
inside the loop. A common beginner mistake is reducing every iteration, which
removes the entire benefit.

### And the tail

```cpp
for (; i < count; ++i) { ... }        // scalar remainder
```

`count` is rarely a multiple of 16. For the dimensions people actually use —
128, 384, 768, 1536, all multiples of 16 — this runs **zero** times. But it must
be correct for `D = 5`, and there is a test covering every remainder from 1 to
17. Off-by-one errors in the tail survive precisely because nobody benchmarks
`D = 5`.

## What it actually bought us

Measured on an Apple M1, Release, cache-resident operands:

| dim | scalar | NEON | speedup |
|---|---|---|---|
| 128 | 37.1 ns | 10.3 ns | **3.59×** |
| 384 | 100.4 ns | 24.3 ns | **4.13×** |
| 768 | 200.0 ns | 51.0 ns | **3.92×** |
| 1536 | 398.7 ns | 106.7 ns | **3.74×** |

3.6–4.1× from a 4-lane unit. About as good as it gets.

128 is slightly worse because fixed overhead — the indirect call, loop setup, the
horizontal reduction — is a larger fraction of a shorter loop.

## The disappointment, and the lesson in it

Now look at whole-query brute-force search, which calls that kernel millions of
times:

| dim | N | latency | effective GB/s |
|---|---|---|---|
| 128 | 100,000 | 1,289 µs | 39.7 |
| 768 | 100,000 | 6,413 µs | 47.9 |

**Throughput plateaus at ~40–48 GB/s regardless of dimension.** That is this
machine's memory bandwidth, not its arithmetic capability.

The kernel benchmark reuses two cache-resident vectors, so it measures
arithmetic. The scan streams gigabytes, so it measures **memory**. Making the
arithmetic 4× faster does not help a loop that is waiting for bytes.

> **SIMD speeds up computation. It does not speed up waiting.**

Which is exactly why HNSW matters more than SIMD: it does not compute faster, it
**touches less data**. Same reason quantization works — it makes each vector
smaller.

SIMD still earns its place. It helps whenever data *is* in cache: HNSW's
traversal touches a few thousand vectors, many of them repeatedly, and the small
`ef`-sized heaps stay hot throughout.

## The abstraction

```cpp
struct DistanceKernel {
    BinaryReduceFn l2_squared;
    BinaryReduceFn dot;
    UnaryReduceFn  norm_squared;
    std::string_view name;
};
```

Three function pointers. Adding NEON meant adding a table, not editing a single
caller.

**Every SIMD kernel is validated against the scalar one** by the same
parameterised test suite — every tail length, a double-precision reference,
random data. Scalar is the oracle in exactly the way brute force is the oracle
for HNSW: it is simple enough to be obviously right, so an optimised kernel that
disagrees is wrong.

That parameterisation was written when only the scalar kernel existed. Writing it
first is why adding NEON was a low-risk change.

## Why not let the compiler do it?

Clang **does** auto-vectorise a loop like this at `-O3`. Sometimes.

It refuses when it cannot prove the pointers do not alias, when the trip count is
unknown, when the reduction would need reassociation it is not allowed to
perform — and it will not tell you which unless you ask for
`-Rpass-analysis=loop-vectorize`.

More importantly, it cannot reassociate a floating-point sum without
`-ffast-math`, which we deliberately do not enable
([03-build-configurations](../20-build-and-dependencies/03-build-configurations.md)).
So the compiler's hands are tied on the exact transformation that matters.

Writing it by hand means it is there, it is visible, and it is tested.

Always **measure the auto-vectorised version first**. If the compiler already
gets you 3.5×, hand-written intrinsics are not worth the maintenance.

## Experiments

1. Compile the scalar kernel with `-Rpass=loop-vectorize` and see whether clang
   vectorised it, and what it produced.
2. Reduce inside the loop instead of at the end. Measure. You have just deleted
   the speedup.
3. Use one accumulator instead of four in the NEON kernel. The gap you measure is
   the pipeline effect, isolated from the lane effect.
4. Add `-ffast-math` and re-time the scalar kernel. Then run the scalar-vs-NEON
   equivalence test and note which tolerance starts failing.
5. Compare `vectordb-bench distance` (cache-resident) with
   `vectordb-bench brute-force` (streaming) and explain the difference in your
   own words. This is the most important experiment on the page.

## Where this lives in the code

- `include/vectordb/distance/kernel.hpp` — the table, and why it is not virtual
- `src/distance/kernel_scalar.cpp` — the oracle, with the accumulator reasoning
- `src/distance/kernel_neon.cpp` — the NEON kernel
- `src/distance/kernel_avx2.cpp` — AVX2, with an honesty note about testing
- `src/distance/cpu_features.cpp` — runtime detection
- `tests/unit/distance_kernel_test.cpp` — every kernel, every tail length
- `docs/benchmark-results.md` — the measured table

---

Next: [04-arm-neon.md](04-arm-neon.md)
