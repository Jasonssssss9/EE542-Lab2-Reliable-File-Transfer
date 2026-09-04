#include "reliability.hpp"

#include <algorithm>
#include <stdexcept>

namespace frft {

SenderWindow::SenderWindow(std::uint32_t total_chunks, std::uint32_t window_chunks)
    : total_chunks_(total_chunks), window_chunks_(window_chunks) {
    if (window_chunks == 0) {
        throw std::invalid_argument("window must contain at least one chunk");
    }
}

bool SenderWindow::can_send() const {
    return next_sequence_ < total_chunks_ &&
           static_cast<std::uint64_t>(next_sequence_) <
               static_cast<std::uint64_t>(base_) + window_chunks_;
}

std::uint32_t SenderWindow::take_next_sequence() {
    if (!can_send()) {
        throw std::logic_error("sender window has no packet ready");
    }
    return next_sequence_++;
}

bool SenderWindow::apply_cumulative_ack(std::uint32_t cumulative_ack) {
    if (cumulative_ack > total_chunks_ || cumulative_ack > next_sequence_) {
        return false;
    }
    if (cumulative_ack > base_) {
        base_ = cumulative_ack;
        return true;
    }
    return false;
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

}  // namespace frft
