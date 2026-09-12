# Architecture decision records

Short, dated notes on the choices that are expensive to reverse.

## ADR-001 — One shared C++ core, two native UIs (not a cross-platform UI)

whisper.cpp and llama.cpp are the only part of this product where platform
parity genuinely matters — the inference math is identical on both devices and
duplicating it would guarantee divergence. The UI is the opposite: a recording
app lives or dies on its audio session handling, its background behaviour and
its scroll performance, all of which are platform-specific in ways a
cross-platform toolkit abstracts away exactly where you need the control.

So the seam is drawn at the inference boundary, not above it.

## ADR-002 — Swift-C++ Interop instead of an Objective-C++ wrapper

The traditional bridge is a `.mm` file exposing an `@objc` class. That costs a
third hand-written API surface to keep in sync, forces every value type through
`NSObject`, and adds an ARC round trip per call.

Swift 5.9+ imports C++ directly. The cost is a discipline constraint: the
public headers must stay inside the subset the importer can model (see the
header comment in `Types.hpp`). That constraint is cheap to hold and it is
checked by the compiler, which a hand-written wrapper never is.

## ADR-003 — Values in, values out; no callbacks across the bridge

`std::function` is not importable into Swift, and a C-function-pointer callback
leaves the question of which thread — and on Android, which `JNIEnv` — the
callback arrives on. `drainSegments()` returns a vector instead. The UI polls a
cheap, lock-guarded queue; the engine never reaches upward.

## ADR-004 — mmap, never read

See the Memory Management section of the root README. The short version: a
`read()` GGUF is dirty anonymous memory and counts fully against the process
footprint; an `mmap()` GGUF is clean, file-backed and evictable.

## ADR-005 — Schema defined in C++, not in Core Data or Room

Two ORMs generating two schemas from two model files is a divergence with a
long fuse. `core/database/include/scribatic/db/Schema.hpp` is the only DDL in the
repository, and both platforms execute it verbatim.
