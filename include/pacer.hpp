#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

// A single sender-owned instance must pace both new packets and retransmissions.
class Pacer {
public:
    explicit Pacer(std::uint64_t rate_bits_per_second);

    // Wait until this packet fits within the configured total sending rate.
    void wait_for_slot(std::size_t packet_bytes);

    void reset();

private:
    std::uint64_t rate_bits_per_second_;
    std::chrono::steady_clock::time_point next_send_time_;
};
