# ScribaticCore module

`module.modulemap` here is the *only* seam between Swift and C++.

`include/` is a build-time symlink to `../../../core/engine/include`, created by
`make setup-ios`. It is not checked in — the source of truth is the shared core,
and a copied header would be a divergence waiting to happen.

Swift sees:

| C++                                | Swift                       |
|------------------------------------|-----------------------------|
| `scribatic::core::EngineInterface`     | `EngineInterface` (ARC class) |
| `std::string`                       | `std.string` → `String`     |
| `std::vector<TranscriptSegment>`    | `RandomAccessCollection`    |
| `enum class EngineStatus : int32_t` | `EngineStatus` (`Int32` raw) |
