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
    std::uint64_t rate_bits_per_second_;
    std::chrono::steady_clock::time_point next_send_time_;
};

}  // namespace frft
