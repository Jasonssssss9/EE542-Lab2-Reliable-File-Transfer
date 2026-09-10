#pragma once

#include "protocol.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace frft {

enum class PacketState {
    UNSENT,
    IN_FLIGHT,
    ACKED,
};

struct SendDecision {
    std::uint32_t sequence;
    bool retransmission;
    bool fast_retransmission;
};

class SenderWindow {
public:
    SenderWindow(std::uint32_t total_chunks, std::uint32_t window_chunks);

    std::optional<SendDecision> select_next_packet();
    void mark_sent(std::uint32_t sequence,
                   std::chrono::steady_clock::time_point send_time);
    void process_ack(
        const AckPayload& ack,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void check_timeouts(std::chrono::steady_clock::time_point now,
                        std::chrono::milliseconds rto);

    bool all_acked() const;
    std::uint32_t base() const;
    std::uint32_t next_sequence() const;
    PacketState state(std::uint32_t sequence) const;
    bool retransmit_pending(std::uint32_t sequence) const;
    std::uint64_t base_advancement_events() const;
    std::chrono::nanoseconds total_base_stall_time() const;
    std::chrono::nanoseconds longest_base_stall_time() const;

private:
    struct ChunkState {
        PacketState state = PacketState::UNSENT;
        std::chrono::steady_clock::time_point last_sent_time {};
        bool retransmit_pending = false;
        bool fast_retransmit_pending = false;
        bool fast_retransmitted = false;
        std::uint32_t latest_later_acked = 0;
        std::uint32_t retransmit_evidence = 0;
    };

    bool can_send_new() const;
    void mark_acked(std::uint32_t sequence);
    void queue_retransmission(std::uint32_t sequence, bool fast_retransmit);
    void advance_base(std::chrono::steady_clock::time_point now);

    std::uint32_t total_chunks_;
    std::uint32_t window_chunks_;
    std::uint32_t base_ = 0;
    std::uint32_t next_sequence_ = 0;
    std::vector<ChunkState> chunks_;
    std::deque<std::uint32_t> retransmission_queue_;
    std::chrono::steady_clock::time_point last_base_advance_time_;
    std::uint64_t base_advancement_events_ = 0;
    std::chrono::nanoseconds total_base_stall_time_ {0};
    std::chrono::nanoseconds longest_base_stall_time_ {0};
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
    AckPayload make_ack_snapshot(std::uint16_t bitmap_bits) const;

private:
    std::vector<std::uint8_t> received_;
    std::uint32_t received_count_ = 0;
    std::uint32_t cumulative_ack_ = 0;
    std::uint32_t largest_received_plus_one_ = 0;
};

}  // namespace frft
