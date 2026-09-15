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

## ADR-006 — The product is retrieval over your own history, not transcription

Dated 2026-09-15, after surveying what already ships.

On-device transcription is no longer a differentiator. Apple Voice Memos does it
on every iPhone 12 or later: on-device, live while recording, searchable across
every recording, summarised by Apple Intelligence. Several third-party apps
(Whisper Notes, Aiko, Whisper Transcription, Whisperer) ship the same
whisper-on-device pitch for a one-off single-digit price. Competing there means
out-executing Apple on a feature Apple gives away.

What nobody ships is the other half: asking a question in natural language and
getting an answer drawn from your entire recording history, computed entirely on
the device. Apple's search is keyword-only and cannot answer "what did we decide
about pricing?". Plaud's "Ask Plaud" does exactly this and sends everything to a
server to do it. That gap — semantic retrieval across a personal corpus with no
network path — is the product.

So transcription is demoted to the ingestion pipeline. It still matters, and
whisper.cpp stays, but it is now judged on whether it produces text worth
indexing rather than on how fast a word appears on screen.

Three consequences follow, and they reverse earlier choices:

1. **Transcribe after the recording stops, not during it.** The five-second
   decode window was whisper doing what whisper is worst at: it loses context at
   every boundary and emits non-speech markers for the gaps. Every other
   whisper-based app transcribes the finished file for exactly this reason, and
   the extra context matters more when the output is being embedded than when it
   is being watched. Live captioning needs a streaming-architecture model, which
   is why Apple can do it and whisper cannot.

2. **Import is a first-class entry point, not a convenience.** Retrieval is
   worth nothing over a corpus of one. Most people already have years of audio
   in Voice Memos and elsewhere, and importing it is how a new user gets a
   history worth querying on the first day.

3. **Persistence stops being optional.** A transcript that dies with the screen
   cannot be retrieved from later. `notes`, `segments`, `chunks`, `vss_chunks`
   and `chunks_fts` move from defined-and-unused to the centre of the product.

The unwired halves — llama.cpp and SQLite-VSS — are therefore no longer the
part to do last. They are the part that makes this a product rather than a
cheaper Voice Memos.
