#include "pacer.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace frft {

Pacer::Pacer(std::uint64_t rate_bits_per_second)
    : rate_bits_per_second_(rate_bits_per_second),
      next_send_time_(std::chrono::steady_clock::now()) {
    if (rate_bits_per_second == 0) {
        throw std::invalid_argument("pacing rate must be greater than zero");
    }
}

void Pacer::wait_for_slot(std::size_t wire_bytes) {
    const auto now = std::chrono::steady_clock::now();
    if (now > next_send_time_) {
        next_send_time_ = now;
    }
    std::this_thread::sleep_until(next_send_time_);

    const long double nanoseconds =
        static_cast<long double>(wire_bytes) * 8.0L * 1'000'000'000.0L /
        static_cast<long double>(rate_bits_per_second_);
    const auto interval = std::chrono::nanoseconds(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(nanoseconds + 0.5L)));
    next_send_time_ += interval;
}

void Pacer::reset() {
    next_send_time_ = std::chrono::steady_clock::now();
}

}  // namespace frft
