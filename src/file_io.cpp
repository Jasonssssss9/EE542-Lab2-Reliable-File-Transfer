#include "file_io.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace frft {
namespace {

std::runtime_error system_error(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}

}  // namespace

MappedInputFile::MappedInputFile(const std::string& path) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw system_error("cannot open input file");
    }

    struct stat info {};
    if (fstat(fd_, &info) != 0) {
        const auto error = system_error("cannot inspect input file");
        close(fd_);
        fd_ = -1;
        throw error;
    }
    if (info.st_size < 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("input file has an invalid size");
    }
    size_ = static_cast<std::uint64_t>(info.st_size);

    if (size_ != 0) {
        void* mapping = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapping == MAP_FAILED) {
            const auto error = system_error("cannot mmap input file");
            close(fd_);
            fd_ = -1;
            throw error;
        }
        data_ = static_cast<std::uint8_t*>(mapping);
    }
}

MappedInputFile::~MappedInputFile() {
    if (data_ != nullptr) {
        munmap(data_, size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

const std::uint8_t* MappedInputFile::data() const {
    return data_;
}

std::uint64_t MappedInputFile::size() const {
    return size_;
}

MappedOutputFile::MappedOutputFile(const std::string& path, std::uint64_t size) : size_(size) {
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        throw std::runtime_error("output file is too large for this system");
    }

    fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        throw system_error("cannot create output file");
    }
    if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        const auto error = system_error("cannot resize output file");
        close(fd_);
        fd_ = -1;
        throw error;
    }

    if (size_ != 0) {
        void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping == MAP_FAILED) {
            const auto error = system_error("cannot mmap output file");
            close(fd_);
            fd_ = -1;
            throw error;
        }
        data_ = static_cast<std::uint8_t*>(mapping);
    }
}

MappedOutputFile::~MappedOutputFile() {
    if (data_ != nullptr) {
        munmap(data_, size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

std::uint8_t* MappedOutputFile::data() {
    return data_;
}

std::uint64_t MappedOutputFile::size() const {
    return size_;
}

void MappedOutputFile::sync() {
    if (data_ != nullptr && msync(data_, size_, MS_SYNC) != 0) {
        throw system_error("cannot flush output file");
    }
}

}  // namespace frft
