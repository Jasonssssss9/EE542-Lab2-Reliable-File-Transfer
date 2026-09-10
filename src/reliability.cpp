#include "reliability.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace frft {
namespace {

constexpr auto kRepeatedFastRetransmitDelay =
    std::chrono::milliseconds(kDefaultRtoMs / 2);

}  // namespace

SenderWindow::SenderWindow(std::uint32_t total_chunks, std::uint32_t window_chunks)
    : total_chunks_(total_chunks),
      window_chunks_(window_chunks),
      chunks_(total_chunks),
      last_base_advance_time_(std::chrono::steady_clock::now()) {
    if (window_chunks == 0) {
        throw std::invalid_argument("window must contain at least one chunk");
    }
}

bool SenderWindow::can_send_new() const {
    return next_sequence_ < total_chunks_ &&
           static_cast<std::uint64_t>(next_sequence_) <
               static_cast<std::uint64_t>(base_) + window_chunks_;
}

std::optional<SendDecision> SenderWindow::select_next_packet() {
    while (!retransmission_queue_.empty()) {
        const std::uint32_t sequence = retransmission_queue_.front();
        retransmission_queue_.pop_front();
        ChunkState& chunk = chunks_[sequence];
        if (chunk.state == PacketState::IN_FLIGHT && chunk.retransmit_pending) {
            chunk.retransmit_pending = false;
            const bool fast_retransmission = chunk.fast_retransmit_pending;
            chunk.fast_retransmit_pending = false;
            chunk.fast_retransmitted = true;
            chunk.retransmit_evidence = chunk.latest_later_acked;
            return SendDecision {sequence, true, fast_retransmission};
        }
    }

    if (can_send_new()) {
        return SendDecision {next_sequence_++, false, false};
    }
    return std::nullopt;
}

void SenderWindow::mark_sent(std::uint32_t sequence,
                             std::chrono::steady_clock::time_point send_time) {
    if (sequence >= next_sequence_ || chunks_[sequence].state == PacketState::ACKED) {
        throw std::logic_error("cannot mark an unselected or ACKed chunk as sent");
    }
    chunks_[sequence].state = PacketState::IN_FLIGHT;
    chunks_[sequence].last_sent_time = send_time;
}

void SenderWindow::mark_acked(std::uint32_t sequence) {
    if (sequence >= next_sequence_ || sequence >= total_chunks_) {
        return;
    }
    chunks_[sequence].state = PacketState::ACKED;
    chunks_[sequence].retransmit_pending = false;
    chunks_[sequence].fast_retransmit_pending = false;
}

void SenderWindow::queue_retransmission(std::uint32_t sequence, bool fast_retransmit) {
    ChunkState& chunk = chunks_[sequence];
    if (chunk.state != PacketState::IN_FLIGHT || chunk.retransmit_pending) {
        return;
    }
    chunk.retransmit_pending = true;
    chunk.fast_retransmit_pending = fast_retransmit;
    if (fast_retransmit) {
        chunk.fast_retransmitted = true;
    }
    retransmission_queue_.push_back(sequence);
}

void SenderWindow::advance_base(std::chrono::steady_clock::time_point now) {
    const std::uint32_t previous_base = base_;
    while (base_ < total_chunks_ && chunks_[base_].state == PacketState::ACKED) {
        ++base_;
    }
    if (base_ != previous_base) {
        const auto stall_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_base_advance_time_);
        total_base_stall_time_ += stall_time;
        longest_base_stall_time_ = std::max(longest_base_stall_time_, stall_time);
        last_base_advance_time_ = now;
        ++base_advancement_events_;
    }
}

void SenderWindow::process_ack(const AckPayload& ack,
                               std::chrono::steady_clock::time_point now) {
    const std::uint32_t prefix_end =
        std::min({ack.cumulative_ack, total_chunks_, next_sequence_});
    for (std::uint32_t sequence = base_; sequence < prefix_end; ++sequence) {
        mark_acked(sequence);
    }

    const std::size_t available_bits = std::min<std::size_t>(
        ack.bitmap_bits, ack.bitmap.size() * 8);
    const std::uint64_t sequence_limit =
        std::min<std::uint64_t>({ack.largest_received_plus_one, total_chunks_, next_sequence_});
    const std::size_t bits_to_scan = ack.bitmap_base < sequence_limit
                                         ? std::min<std::uint64_t>(
                                               available_bits,
                                               sequence_limit - ack.bitmap_base)
                                         : 0;
    for (std::size_t bit = 0; bit < bits_to_scan; ++bit) {
        if ((ack.bitmap[bit / 8] & (1U << (bit % 8))) == 0) {
            continue;
        }
        const std::uint64_t sequence = static_cast<std::uint64_t>(ack.bitmap_base) + bit;
        if (sequence < total_chunks_ && sequence < next_sequence_) {
            mark_acked(static_cast<std::uint32_t>(sequence));
        }
    }

    advance_base(now);

    std::uint32_t later_acked = 0;
    for (std::uint32_t sequence = next_sequence_; sequence > base_;) {
        --sequence;
        ChunkState& chunk = chunks_[sequence];
        if (chunk.state == PacketState::ACKED) {
            ++later_acked;
        } else if (chunk.state == PacketState::IN_FLIGHT) {
            const bool has_new_later_sack = later_acked > chunk.latest_later_acked;
            chunk.latest_later_acked = later_acked;
            const bool has_new_sack_evidence =
                later_acked >= chunk.retransmit_evidence &&
                later_acked - chunk.retransmit_evidence >= kFastRetransmitThreshold;
            const bool retry_delay_elapsed =
                !chunk.fast_retransmitted ||
                now - chunk.last_sent_time >= kRepeatedFastRetransmitDelay;
            if (has_new_later_sack && has_new_sack_evidence && retry_delay_elapsed) {
                // New SACK progress plus a conservative delay avoids retrying on duplicate ACKs.
                queue_retransmission(sequence, true);
            }
        }
    }
}

void SenderWindow::check_timeouts(std::chrono::steady_clock::time_point now,
                                  std::chrono::milliseconds rto) {
    for (std::uint32_t sequence = base_; sequence < next_sequence_; ++sequence) {
        const ChunkState& chunk = chunks_[sequence];
        if (chunk.state == PacketState::IN_FLIGHT && !chunk.retransmit_pending &&
            now - chunk.last_sent_time >= rto) {
            queue_retransmission(sequence, false);
        }
    }
}

bool SenderWindow::all_acked() const {
    return base_ == total_chunks_;
}

std::uint32_t SenderWindow::base() const {
    return base_;
}

std::uint32_t SenderWindow::next_sequence() const {
    return next_sequence_;
}

PacketState SenderWindow::state(std::uint32_t sequence) const {
    if (sequence >= total_chunks_) {
        throw std::out_of_range("chunk sequence is outside the transfer");
    }
    return chunks_[sequence].state;
}

bool SenderWindow::retransmit_pending(std::uint32_t sequence) const {
    if (sequence >= total_chunks_) {
        return false;
    }
    return chunks_[sequence].retransmit_pending;
}

std::uint64_t SenderWindow::base_advancement_events() const {
    return base_advancement_events_;
}

std::chrono::nanoseconds SenderWindow::total_base_stall_time() const {
    return total_base_stall_time_;
}

std::chrono::nanoseconds SenderWindow::longest_base_stall_time() const {
    return longest_base_stall_time_;
}

ReceiverTracker::ReceiverTracker(std::uint32_t total_chunks) : received_(total_chunks, 0) {}

bool ReceiverTracker::has_received(std::uint32_t sequence) const {
    return sequence < received_.size() && received_[sequence] != 0;
}

bool ReceiverTracker::mark_received(std::uint32_t sequence) {
    if (sequence >= received_.size() || received_[sequence] != 0) {
        return false;
    }

    received_[sequence] = 1;
    ++received_count_;
    largest_received_plus_one_ = std::max(largest_received_plus_one_, sequence + 1);
    while (cumulative_ack_ < received_.size() && received_[cumulative_ack_] != 0) {
        ++cumulative_ack_;
    }
    return true;
}

bool ReceiverTracker::complete() const {
    return received_count_ == received_.size();
}

std::uint32_t ReceiverTracker::received_count() const {
    return received_count_;
}

std::uint32_t ReceiverTracker::cumulative_ack() const {
    return cumulative_ack_;
}

std::uint32_t ReceiverTracker::largest_received_plus_one() const {
    return largest_received_plus_one_;
}

AckPayload ReceiverTracker::make_ack_snapshot(std::uint16_t bitmap_bits) const {
    AckPayload ack;
    ack.cumulative_ack = cumulative_ack_;
    ack.largest_received_plus_one = largest_received_plus_one_;
    ack.bitmap_base = cumulative_ack_;
    ack.bitmap_bits = bitmap_bits;
    ack.bitmap.assign((bitmap_bits + 7) / 8, 0);

    const std::uint64_t sequence_limit =
        std::min<std::uint64_t>(largest_received_plus_one_, received_.size());
    const std::size_t bits_to_scan = ack.bitmap_base < sequence_limit
                                         ? std::min<std::uint64_t>(
                                               bitmap_bits,
                                               sequence_limit - ack.bitmap_base)
                                         : 0;
    for (std::size_t bit = 0; bit < bits_to_scan; ++bit) {
        const std::uint64_t sequence = static_cast<std::uint64_t>(ack.bitmap_base) + bit;
        if (received_[sequence] != 0) {
            ack.bitmap[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
        }
    }
    return ack;
}

}  // namespace frft
