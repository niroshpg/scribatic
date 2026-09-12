#include "scribatic/core/ModelResidency.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

namespace scribatic::core {

ModelResidency::~ModelResidency() { unmap(); }

ModelResidency::ModelResidency(ModelResidency&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      fd_(std::exchange(other.fd_, -1)) {}

ModelResidency& ModelResidency::operator=(ModelResidency&& other) noexcept {
    if (this != &other) {
        unmap();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        fd_   = std::exchange(other.fd_, -1);
    }
    return *this;
}

bool ModelResidency::map(const std::string& path) noexcept {
    unmap();

    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }

    struct stat info {};
    if (::fstat(fd_, &info) != 0 || info.st_size <= 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    size_ = static_cast<std::size_t>(info.st_size);
    void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd_);
        fd_   = -1;
        size_ = 0;
        return false;
    }

    data_ = mapped;
    advise();
    return true;
}

void ModelResidency::advise() const noexcept {
    if (data_ != nullptr) {
        // Transformer weights are walked non-sequentially; suppress readahead.
        (void)::madvise(const_cast<void*>(data_), size_, MADV_RANDOM);
    }
}

void ModelResidency::unmap() noexcept {
    if (data_ != nullptr) {
        (void)::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

} // namespace scribatic::core
