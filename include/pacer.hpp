#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace frft {

class Pacer {
public:
    explicit Pacer(std::uint64_t rate_bits_per_second);

    void wait_for_slot(std::size_t wire_bytes);
    void reset();

private:
    using Clock = std::chrono::steady_clock;

    void refill(Clock::time_point now, long double capacity_bytes);

    long double bytes_per_nanosecond_;
    long double tokens_bytes_;
    Clock::time_point last_refill_time_;
};

}  // namespace frft
