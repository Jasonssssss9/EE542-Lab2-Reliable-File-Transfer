#include "pacer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace frft {
namespace {

constexpr std::size_t kBurstCapacityBytes = 9015;
constexpr long double kRefillTargetFraction = 2.0L / 3.0L;
constexpr long double kNanosecondsPerSecond = 1'000'000'000.0L;
constexpr long double kBitsPerByte = 8.0L;
constexpr long double kMaximumWaitNanoseconds = kNanosecondsPerSecond;
constexpr auto kJumboSpinWindow = std::chrono::microseconds(100);

}  // namespace

Pacer::Pacer(std::uint64_t rate_bits_per_second)
    : bytes_per_nanosecond_(0.0L),
      tokens_bytes_(kBurstCapacityBytes),
      last_refill_time_(Clock::now()) {
    if (rate_bits_per_second == 0) {
        throw std::invalid_argument("pacing rate must be greater than zero");
    }
    bytes_per_nanosecond_ = static_cast<long double>(rate_bits_per_second) /
                            (kBitsPerByte * kNanosecondsPerSecond);
}

void Pacer::refill(Clock::time_point now, long double capacity_bytes) {
    if (now <= last_refill_time_) {
        return;
    }

    const long double elapsed_nanoseconds =
        std::chrono::duration<long double, std::nano>(now - last_refill_time_).count();
    tokens_bytes_ = std::min(
        capacity_bytes, tokens_bytes_ + elapsed_nanoseconds * bytes_per_nanosecond_);
    last_refill_time_ = now;
}

void Pacer::wait_for_slot(std::size_t wire_bytes) {
    const long double packet_bytes = static_cast<long double>(wire_bytes);
    const long double capacity_bytes =
        std::max<long double>(kBurstCapacityBytes, packet_bytes);

    auto now = Clock::now();
    refill(now, capacity_bytes);

    while (tokens_bytes_ < packet_bytes) {
        // Refill a useful batch while leaving room to retain ordinary VM oversleep credit.
        const long double target_bytes =
            std::max(packet_bytes, capacity_bytes * kRefillTargetFraction);
        const long double missing_bytes = target_bytes - tokens_bytes_;
        const long double wait_nanoseconds = missing_bytes / bytes_per_nanosecond_;
        const long double bounded_wait_nanoseconds =
            std::min(wait_nanoseconds, kMaximumWaitNanoseconds);
        const auto wait_duration = std::chrono::nanoseconds(
            std::max<std::int64_t>(
                1, static_cast<std::int64_t>(std::ceil(bounded_wait_nanoseconds))));

        const auto deadline = now + wait_duration;
        const bool single_packet_bucket = packet_bytes * 2.0L > capacity_bytes;
        if (!single_packet_bucket) {
            std::this_thread::sleep_until(deadline);
        } else {
            if (wait_duration > kJumboSpinWindow) {
                std::this_thread::sleep_until(deadline - kJumboSpinWindow);
            }
            while (Clock::now() < deadline) {
            }
        }
        now = Clock::now();
        refill(now, capacity_bytes);
    }

    tokens_bytes_ -= packet_bytes;
}

void Pacer::reset() {
    tokens_bytes_ = kBurstCapacityBytes;
    last_refill_time_ = Clock::now();
}

}  // namespace frft
