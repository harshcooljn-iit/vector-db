# Value, reference, and view semantics

🟢 Foundational · the first thing that changes when you move from DSA to systems

## Three ways to pass data

```cpp
void f(std::vector<float> v);          // value — copies
void f(const std::vector<float>& v);   // reference — no copy, ties you to the type
void f(std::span<const float> v);      // view — no copy, no ownership, no type
```

In competitive programming you use the first two and never think about it. In a
system where one of those vectors is 3 KB and the function runs a million times
per query, the choice is the design.

## What VectorDB uses, and where

| Type | Semantics | Why |
|---|---|---|
| `VectorView` (`span<const float>`) | view | a kernel should not care where the floats live |
| `VectorAccessor` | view (3 pointers) | an index must not own or copy the store |
| `Candidate`, `SearchResult` | value | 8–12 bytes; copying is cheaper than indirection |
| `DistanceFunction` | value (2 words) | construct one per search, pass freely |
| `VectorStore`, `Database` | owning, move-only | there is exactly one, and it owns its data |

## Views are the important one

```cpp
using VectorView = std::span<const float>;
```

A pointer and a length. Two words. **Pass by value** — taking
`const std::span&` adds an indirection to something already pointer-sized.

The payoff is that a kernel cannot tell where its argument came from:

```cpp
VectorView a = store[42];             // the big contiguous block
VectorView b = query_buffer;          // a stack array
VectorView c = mapped_region;         // an mmap'd page
float d = distance(a, b);             // identical code
```

That decoupling is what let memory mapping arrive later without touching a
single kernel. **A view is a promise about shape, not about ownership.**

## The rule that comes with it

A view does not keep anything alive.

```cpp
VectorView v = array[5];
array.push_back(x);        // may reallocate
use(v[0]);                 // ☠️ dangling
```

Exactly `std::vector`'s iterator-invalidation rule. Never hold a view across an
operation that can reallocate.

Under `asan` this is caught instantly with a stack trace. Under `release` it is a
silent read of freed memory. That difference is the whole argument for running
the sanitizers.

## Move-only ownership

```cpp
Database(const Database&) = delete;
Database(Database&&) noexcept;
```

There is one database per directory, and it owns file handles, a SQLite
connection and possibly gigabytes of floats. Copying it is never what anyone
means, so the compiler says so.

The general rule: **if copying would be a bug, delete the copy constructor.** A
compile error beats a mysterious duplicate at 3 a.m.

## Where a copy is right

```cpp
struct Candidate { LocalId local_id; RankKey key; };   // 8 bytes
```

Passed and stored by value, millions of times. A reference would be the same size
and would add a dereference plus a lifetime question.

**Below roughly two or three words, copy.** Above it, or when identity matters,
do not.

## `std::string_view` is the same idea for text

```cpp
[[nodiscard]] std::string_view metric_name(Metric metric) noexcept;
```

Returns a view of a string literal — no allocation, no copy. And the same rule
applies: never return a `string_view` into a temporary.

## Experiments

1. Change `VectorView` to `const std::vector<float>&` throughout. Note the first
   thing that stops compiling — probably the mmap path or a stack buffer.
2. Pass `VectorView` by `const&` and measure the distance benchmark. Explain the
   direction of the result.
3. Delete `Database`'s deleted copy constructor and write `Database b = a;`.
   Predict what breaks at runtime before you run it.
4. Hold a `VectorView` across a `push_back` that reallocates. Run under `release`,
   then under `asan`.

## Where this lives in the code

- `include/vectordb/core/vector.hpp` — `VectorView`, `MutableVectorView`
- `include/vectordb/index/vector_accessor.hpp` — a view of the whole store
- `include/vectordb/db/database.hpp` — move-only ownership
- `include/vectordb/core/search_result.hpp` — small value types

---

Next: [02-raii-and-resource-ownership.md](02-raii-and-resource-ownership.md)
