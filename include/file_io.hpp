#pragma once

#include <cstdint>
#include <string>

namespace frft {

class MappedInputFile {
public:
    explicit MappedInputFile(const std::string& path);
    ~MappedInputFile();

    MappedInputFile(const MappedInputFile&) = delete;
    MappedInputFile& operator=(const MappedInputFile&) = delete;

    const std::uint8_t* data() const;
    std::uint64_t size() const;

private:
    int fd_ = -1;
    std::uint8_t* data_ = nullptr;
    std::uint64_t size_ = 0;
};

class MappedOutputFile {
public:
    MappedOutputFile(const std::string& path, std::uint64_t size);
    ~MappedOutputFile();

    MappedOutputFile(const MappedOutputFile&) = delete;
    MappedOutputFile& operator=(const MappedOutputFile&) = delete;

    std::uint8_t* data();
    std::uint64_t size() const;
    void sync();

private:
    int fd_ = -1;
    std::uint8_t* data_ = nullptr;
    std::uint64_t size_ = 0;
};

}  // namespace frft
