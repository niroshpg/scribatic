// =============================================================================
//  ModelResidency.hpp — RAII wrapper around mmap()'d GGUF weight files.
//
//  A 1–4 GB GGUF must never be read() into the heap: on iOS that counts fully
//  against the jetsam footprint limit, and on Android it inflates PSS enough to
//  make the app the first candidate for the low-memory killer. Instead we map
//  the file read-only and let the kernel page in what the graph touches; those
//  pages are clean, file-backed and evictable, so they are largely excluded
//  from the process's dirty-memory accounting.
//
//  `advise()` issues madvise(MADV_RANDOM) because transformer weight access is
//  effectively random across layers — the default readahead heuristic hurts.
// =============================================================================
#pragma once

#include <cstddef>
#include <string>

namespace scribatic::core {

class ModelResidency {
public:
    ModelResidency() = default;
    ~ModelResidency();

    ModelResidency(const ModelResidency&)            = delete;
    ModelResidency& operator=(const ModelResidency&) = delete;
    ModelResidency(ModelResidency&& other) noexcept;
    ModelResidency& operator=(ModelResidency&& other) noexcept;

    /// mmap(PROT_READ, MAP_PRIVATE). Returns false and leaves the object empty
    /// on failure; errno is folded into the engine's EngineStatus by the caller.
    bool map(const std::string& path) noexcept;

    /// Explicit munmap. Called by hibernate() so backgrounded sessions shrink
    /// their footprint without destroying the engine object.
    void unmap() noexcept;

    /// madvise() hint for transformer-style random access.
    void advise() const noexcept;

    [[nodiscard]] const void* data()  const noexcept { return data_; }
    [[nodiscard]] std::size_t size()  const noexcept { return size_; }
    [[nodiscard]] bool        valid() const noexcept { return data_ != nullptr; }

private:
    void*       data_ = nullptr;
    std::size_t size_ = 0;
    int         fd_   = -1;
};

} // namespace scribatic::core
