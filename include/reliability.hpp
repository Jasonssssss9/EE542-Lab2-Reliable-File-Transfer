#pragma once

#include <cstdint>
#include <vector>

namespace frft {

class SenderWindow {
public:
    SenderWindow(std::uint32_t total_chunks, std::uint32_t window_chunks);

    bool can_send() const;
    std::uint32_t take_next_sequence();
    bool apply_cumulative_ack(std::uint32_t cumulative_ack);
    bool all_acked() const;
    std::uint32_t base() const;
    std::uint32_t next_sequence() const;

private:
    std::uint32_t total_chunks_;
    std::uint32_t window_chunks_;
    std::uint32_t base_ = 0;
    std::uint32_t next_sequence_ = 0;
};

class ReceiverTracker {
public:
    explicit ReceiverTracker(std::uint32_t total_chunks);

    bool has_received(std::uint32_t sequence) const;
    bool mark_received(std::uint32_t sequence);
    bool complete() const;
    std::uint32_t received_count() const;
    std::uint32_t cumulative_ack() const;
    std::uint32_t largest_received_plus_one() const;

private:
    std::vector<std::uint8_t> received_;
    std::uint32_t received_count_ = 0;
    std::uint32_t cumulative_ack_ = 0;
    std::uint32_t largest_received_plus_one_ = 0;
};

}  // namespace frft
