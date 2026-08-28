# ADR-0009 — Report failures with exceptions

**Status:** Accepted

## Decision

Engine code signals failure by throwing a type derived from `vectordb::Error`.
The CLI is the only layer that catches broadly, converting an exception into a
message and an exit code.

## Context

C++ offers several error strategies: exceptions, error codes, `std::optional`,
`std::expected`, and out-parameters. The choice affects every function
signature in the project, so it is worth making deliberately.

## Options considered

1. **Error codes / out-parameters.** Every call site must check. In practice,
   some do not, and an unchecked failure propagates as corrupt state.
2. **`std::expected<T, Error>` (C++23).** Type-safe, explicit, no unwinding.
   Availability under Apple Clang's libc++ is uneven, and it forces every
   intermediate function to either handle or explicitly forward — which is a
   feature for a two-layer system and considerable noise for an eight-layer one.
3. **Exceptions.** (chosen)

## Chosen approach

Exceptions, with a hierarchy that lets callers react to categories rather than
parse message strings.

## Why

- **Our failures are exceptional.** A dimension mismatch, a corrupt index, a
  missing file — none of these are part of a normal control flow that the
  caller is expected to loop over. They abort the operation.
- **The interesting errors originate deep and are handled shallow.** A bad
  offset is detected in the deserializer and handled in the CLI, six frames up.
  With error codes, all six frames need forwarding boilerplate; with
  exceptions, the intermediate frames simply do not mention it.
- **RAII already guarantees cleanup.** File handles, SQLite statements, locks
  and buffers are all owned by objects with destructors, so unwinding releases
  them correctly with no `goto cleanup` and no leak.
- **They cannot be ignored.** An unchecked error code is silent; an uncaught
  exception terminates loudly.

## Trade-offs

| Cost | Benefit |
|---|---|
| Throwing is slow (microseconds) | Zero cost on the path that does not throw |
| Control flow is not visible at the call site | No boilerplate in intermediate frames |
| Every function must be exception-safe | RAII makes that largely automatic |
| Cannot be used across a C ABI | Not a constraint for v1 |

The performance objection does not apply here: modern implementations use
table-based unwinding, so the *non-throwing* path — the one taken billions of
times — costs nothing. Throwing is expensive, but we throw once per failed
operation, not once per candidate.

## Consequences

- **Nothing in a hot loop throws.** The search path validates its inputs at the
  boundary and then runs without exception machinery.
- **No destructor throws.** A throwing destructor during unwinding calls
  `std::terminate`.
- **Nothing is swallowed.** `catch (...)` with an empty body, or a log-and-
  continue that discards a `CorruptionError`, is a defect.
- Messages must name the values involved, and tests assert on message content —
  the hierarchy is useless if the message is "Error.".

## Alternatives for future versions

A `noexcept` C API layer, if VectorDB is ever embedded across an ABI boundary
or bound to another language. It would sit above the C++ API, catching and
translating to codes at that one boundary — which is exactly where a
translation layer belongs.
