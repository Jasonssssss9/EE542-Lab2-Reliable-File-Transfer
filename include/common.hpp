#pragma once

#include <cstddef>
#include <cstdint>

namespace frft {

constexpr std::uint32_t kMagic = 0x46524654;
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kIpv4UdpOverhead = 28;
constexpr std::uint64_t kDefaultWindowBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint16_t kDefaultAckBitmapBits = 8192;
constexpr std::uint16_t kDefaultAckIntervalMs = 10;
constexpr int kControlTimeoutMs = 1000;
constexpr int kControlAttempts = 10;

}  // namespace frft
