#include "protocol.hpp"
#include "reliability.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void set_ack_bit(frft::AckPayload& ack, std::uint32_t bit) {
    ack.bitmap[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
}

bool ack_bit(const frft::AckPayload& ack, std::uint32_t bit) {
    return (ack.bitmap[bit / 8] & (1U << (bit % 8))) != 0;
}

void send_initial_chunks(frft::SenderWindow& sender,
                         std::uint32_t count,
                         std::chrono::steady_clock::time_point time) {
    for (std::uint32_t sequence = 0; sequence < count; ++sequence) {
        const auto decision = sender.select_next_packet();
        check(decision.has_value(), "sender has an initial packet to select");
        if (!decision) {
            return;
        }
        check(!decision->retransmission && decision->sequence == sequence,
              "initial packets are selected in sequence order");
        sender.mark_sent(decision->sequence, time);
    }
}

void test_header_and_data() {
    const std::vector<std::uint8_t> payload {0x10, 0x20, 0x30};
    frft::PacketHeader header;
    header.type = frft::PacketType::DATA;
    header.flags = frft::FLAG_RETRANSMITTED;
    header.session_id = 0x12345678;
    header.number = 42;

    const auto bytes = frft::serialize_packet(header, payload.data(), payload.size());
    const auto serialized_header = frft::serialize_packet_header(header, payload.size());
    check(bytes.size() == frft::kHeaderSize + payload.size(), "serialized DATA length");
    check(std::equal(serialized_header.begin(), serialized_header.end(), bytes.begin()),
          "scatter/gather header matches the complete packet header");
    check(bytes[0] == 0x46 && bytes[1] == 0x52 && bytes[2] == 0x46 && bytes[3] == 0x54,
          "magic is encoded in network byte order");
    check(bytes[5] == static_cast<std::uint8_t>(frft::PacketType::DATA), "DATA type byte");
    check(bytes[12] == 0 && bytes[13] == 0 && bytes[14] == 0 && bytes[15] == 42,
          "chunk number is encoded in network byte order");

    frft::Packet decoded;
    std::string error;
    check(frft::deserialize_packet(bytes.data(), bytes.size(), decoded, error),
          "serialized DATA can be decoded");
    check(decoded.header.session_id == header.session_id, "session ID round trip");
    check(decoded.header.number == 42, "chunk number round trip");
    check(decoded.header.flags == frft::FLAG_RETRANSMITTED,
          "DATA flags survive header serialization");
    check(decoded.payload == payload, "DATA payload round trip");

    frft::PacketView view;
    check(frft::deserialize_packet_view(bytes.data(), bytes.size(), view, error),
          "serialized DATA can be decoded without copying its payload");
    check(view.header.session_id == header.session_id && view.header.number == 42,
          "packet view header round trip");
    check(view.payload == bytes.data() + frft::kHeaderSize &&
              view.payload_size == payload.size() &&
              std::equal(payload.begin(), payload.end(), view.payload),
          "packet view refers to the payload in the receive buffer");

    auto malformed = bytes;
    malformed[0] = 0;
    check(!frft::deserialize_packet(malformed.data(), malformed.size(), decoded, error),
          "bad magic is rejected");
    check(!frft::deserialize_packet_view(
              malformed.data(), malformed.size(), view, error),
          "packet view rejects bad magic");
    check(!frft::deserialize_packet(bytes.data(), bytes.size() - 1, decoded, error),
          "wrong datagram length is rejected");
    check(!frft::deserialize_packet_view(
              bytes.data(), bytes.size() - 1, view, error),
          "packet view rejects a wrong datagram length");
}

void test_control_payloads() {
    const frft::StartPayload start {0x0102030405060708ULL, 1448, 1234, 512, 8192, 10};
    const auto start_bytes = frft::serialize_start(start);
    frft::StartPayload decoded_start;
    check(frft::deserialize_start(start_bytes.data(), start_bytes.size(), decoded_start),
          "START payload decodes");
    check(decoded_start.file_size == start.file_size &&
              decoded_start.chunk_size == start.chunk_size &&
              decoded_start.total_chunks == start.total_chunks &&
              decoded_start.window_chunks == start.window_chunks,
          "START payload round trip");

    const frft::StartAckPayload start_ack {frft::StatusCode::OK, 512, 8192, 10};
    const auto start_ack_bytes = frft::serialize_start_ack(start_ack);
    frft::StartAckPayload decoded_start_ack;
    check(frft::deserialize_start_ack(
              start_ack_bytes.data(), start_ack_bytes.size(), decoded_start_ack),
          "START_ACK payload decodes");
    check(decoded_start_ack.status == frft::StatusCode::OK &&
              decoded_start_ack.accepted_window_chunks == 512,
          "START_ACK payload round trip");

    frft::AckPayload ack {100, 104, 100, 16, std::vector<std::uint8_t>(2, 0)};
    set_ack_bit(ack, 1);
    set_ack_bit(ack, 3);
    const auto ack_bytes = frft::serialize_ack(ack);
    frft::AckPayload decoded_ack;
    check(frft::deserialize_ack(ack_bytes.data(), ack_bytes.size(), decoded_ack),
          "ACK payload decodes");
    check(decoded_ack.cumulative_ack == 100 && decoded_ack.bitmap_base == 100 &&
              decoded_ack.bitmap_bits == 16,
          "ACK prefix survives round trip");
    check(decoded_ack.bitmap == ack.bitmap, "SACK bitmap survives round trip");

    const frft::CompleteAckPayload complete {
        frft::StatusCode::OK, 1234, 0x0102030405060708ULL, 987654};
    const auto complete_bytes = frft::serialize_complete_ack(complete);
    frft::CompleteAckPayload decoded_complete;
    check(frft::deserialize_complete_ack(
              complete_bytes.data(), complete_bytes.size(), decoded_complete),
          "COMPLETE_ACK payload decodes");
    check(decoded_complete.received_bytes == complete.received_bytes &&
              decoded_complete.receiver_transfer_time_us == complete.receiver_transfer_time_us,
          "COMPLETE_ACK payload round trip");
}

void test_receiver_sack_and_gap_filling() {
    frft::ReceiverTracker receiver(8);
    check(receiver.mark_received(1), "receiver accepts chunk 1 out of order");
    check(receiver.mark_received(2), "receiver accepts chunk 2 out of order");
    check(receiver.mark_received(3), "receiver accepts chunk 3 out of order");
    check(receiver.cumulative_ack() == 0, "missing chunk 0 holds cumulative ACK at zero");

    const auto sack = receiver.make_ack_snapshot(8);
    check(sack.cumulative_ack == 0 && sack.bitmap_base == 0 && sack.bitmap_bits == 8,
          "receiver SACK uses cumulative ACK as bitmap base");
    check(!ack_bit(sack, 0) && ack_bit(sack, 1) && ack_bit(sack, 2) && ack_bit(sack, 3),
          "receiver SACK reports the gap and later received chunks");

    check(receiver.mark_received(0), "receiver accepts the missing prefix chunk");
    check(receiver.cumulative_ack() == 4,
          "filling the gap advances ACK across already received chunks");
    check(!receiver.mark_received(1) && receiver.received_count() == 4,
          "duplicate DATA does not change receive count");
}

void test_sender_ack_processing_and_window() {
    const auto sent_time = std::chrono::steady_clock::time_point {};
    frft::SenderWindow sender(5, 2);
    send_initial_chunks(sender, 2, sent_time);
    check(!sender.select_next_packet(), "sender stops at the fixed window edge");

    frft::AckPayload ack {1, 2, 1, 8, std::vector<std::uint8_t>(1, 0)};
    set_ack_bit(ack, 0);
    sender.process_ack(ack);
    check(sender.state(0) == frft::PacketState::ACKED,
          "cumulative ACK marks the prefix ACKED");
    check(sender.state(1) == frft::PacketState::ACKED,
          "SACK marks a non-contiguous chunk ACKED");
    check(sender.base() == 2, "ACK processing advances contiguous sender base");

    const auto next = sender.select_next_packet();
    check(next && next->sequence == 2 && !next->retransmission,
          "ACKed chunks open the fixed window");
}

void test_fast_retransmit() {
    const auto sent_time = std::chrono::steady_clock::time_point {};
    frft::SenderWindow sender(105, 105);
    send_initial_chunks(sender, 104, sent_time);

    frft::AckPayload ack {100, 104, 100, 8, std::vector<std::uint8_t>(1, 0)};
    set_ack_bit(ack, 1);
    set_ack_bit(ack, 2);
    set_ack_bit(ack, 3);
    sender.process_ack(ack);

    check(sender.state(100) == frft::PacketState::IN_FLIGHT,
          "missing chunk remains in flight before retransmission");
    check(sender.retransmit_pending(100),
          "three later ACKed chunks make the missing chunk retransmission-pending");
    const auto retransmission = sender.select_next_packet();
    check(retransmission && retransmission->retransmission &&
              retransmission->fast_retransmission && retransmission->sequence == 100,
          "pending retransmission is selected before new DATA");
}

void test_repeated_fast_retransmit_requires_new_sack_evidence() {
    const auto sent_time = std::chrono::steady_clock::now();
    frft::SenderWindow sender(115, 115);
    send_initial_chunks(sender, 115, sent_time);

    frft::AckPayload first_gap_ack {100, 104, 100, 16, std::vector<std::uint8_t>(2, 0)};
    set_ack_bit(first_gap_ack, 1);
    set_ack_bit(first_gap_ack, 2);
    set_ack_bit(first_gap_ack, 3);
    sender.process_ack(first_gap_ack, sent_time + std::chrono::milliseconds(200));

    const auto first_retransmission = sender.select_next_packet();
    check(first_retransmission && first_retransmission->retransmission &&
              first_retransmission->fast_retransmission &&
              first_retransmission->sequence == 100,
          "first SACK threshold schedules a fast retransmission");
    sender.mark_sent(100, sent_time + std::chrono::milliseconds(200));

    sender.process_ack(first_gap_ack, sent_time + std::chrono::milliseconds(250));
    check(!sender.retransmit_pending(100),
          "duplicate SACK evidence does not schedule another retransmission");

    frft::AckPayload more_progress {100, 107, 100, 16, std::vector<std::uint8_t>(2, 0)};
    for (std::uint32_t bit = 1; bit <= 6; ++bit) {
        set_ack_bit(more_progress, bit);
    }
    sender.process_ack(more_progress, sent_time + std::chrono::milliseconds(300));
    check(!sender.retransmit_pending(100),
          "new SACK evidence cannot retry before the conservative delay");

    sender.process_ack(more_progress, sent_time + std::chrono::milliseconds(451));
    check(!sender.retransmit_pending(100),
          "a duplicate SACK cannot trigger a retry after the delay expires");

    frft::AckPayload eligible_progress {100, 108, 100, 16, std::vector<std::uint8_t>(2, 0)};
    for (std::uint32_t bit = 1; bit <= 7; ++bit) {
        set_ack_bit(eligible_progress, bit);
    }
    sender.process_ack(eligible_progress, sent_time + std::chrono::milliseconds(452));
    sender.process_ack(eligible_progress, sent_time + std::chrono::milliseconds(452));
    check(sender.retransmit_pending(100),
          "three newly ACKed later chunks allow one repeated fast retransmission");
    const auto repeated_retransmission = sender.select_next_packet();
    check(repeated_retransmission && repeated_retransmission->retransmission &&
              repeated_retransmission->fast_retransmission &&
              repeated_retransmission->sequence == 100,
          "repeated fast retransmission keeps its scheduling reason");
    check(!sender.select_next_packet(),
          "duplicate ACK processing cannot queue the same retransmission twice");
    sender.mark_sent(100, sent_time + std::chrono::milliseconds(452));

    frft::AckPayload third_progress {100, 111, 100, 16, std::vector<std::uint8_t>(2, 0)};
    for (std::uint32_t bit = 1; bit <= 10; ++bit) {
        set_ack_bit(third_progress, bit);
    }
    sender.process_ack(third_progress, sent_time + std::chrono::milliseconds(752));
    check(sender.retransmit_pending(100),
          "another three newly ACKed chunks can schedule a later retry");

    frft::AckPayload gap_filled {115, 115, 115, 16, std::vector<std::uint8_t>(2, 0)};
    sender.process_ack(gap_filled, sent_time + std::chrono::milliseconds(753));
    check(sender.state(100) == frft::PacketState::ACKED,
          "cumulative ACK marks a pending retransmission ACKED");
    check(!sender.select_next_packet(), "an ACKed chunk is never retransmitted");
}

void test_duplicate_and_reordered_ack_safety() {
    const auto sent_time = std::chrono::steady_clock::time_point {};
    frft::SenderWindow sender(8, 8);
    send_initial_chunks(sender, 6, sent_time);

    frft::AckPayload newer {2, 6, 2, 8, std::vector<std::uint8_t>(1, 0)};
    set_ack_bit(newer, 3);
    sender.process_ack(newer);
    check(sender.state(5) == frft::PacketState::ACKED, "new SACK marks chunk 5 ACKED");

    frft::AckPayload older {1, 2, 1, 8, std::vector<std::uint8_t>(1, 0)};
    sender.process_ack(older);
    sender.process_ack(older);
    check(sender.state(5) == frft::PacketState::ACKED,
          "old and duplicate ACKs cannot undo ACKed state");
}

void test_rto_and_completion() {
    const auto sent_time = std::chrono::steady_clock::time_point {};
    frft::SenderWindow sender(2, 2);
    send_initial_chunks(sender, 2, sent_time);

    frft::AckPayload ack {0, 2, 0, 8, std::vector<std::uint8_t>(1, 0)};
    set_ack_bit(ack, 1);
    sender.process_ack(ack);
    check(!sender.all_acked(), "sender is incomplete while one chunk is missing");

    sender.check_timeouts(sent_time + std::chrono::milliseconds(501),
                          std::chrono::milliseconds(500));
    check(sender.retransmit_pending(0), "expired in-flight chunk becomes pending");
    check(!sender.retransmit_pending(1), "ACKed chunk does not time out");
    const auto retransmission = sender.select_next_packet();
    check(retransmission && retransmission->retransmission &&
              !retransmission->fast_retransmission && retransmission->sequence == 0,
          "RTO retransmission keeps its timeout scheduling reason");

    frft::AckPayload complete {2, 2, 2, 8, std::vector<std::uint8_t>(1, 0)};
    sender.process_ack(complete);
    check(sender.all_acked(), "sender completes only after every chunk is ACKed");
}

}  // namespace

int main() {
    test_header_and_data();
    test_control_payloads();
    test_receiver_sack_and_gap_filling();
    test_sender_ack_processing_and_window();
    test_fast_retransmit();
    test_repeated_fast_retransmit_requires_new_sack_evidence();
    test_duplicate_and_reordered_ack_safety();
    test_rto_and_completion();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All protocol tests passed\n";
    return 0;
}
