#include "protocol.hpp"
#include "reliability.hpp"

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

void test_header_and_data() {
    const std::vector<std::uint8_t> payload {0x10, 0x20, 0x30};
    frft::PacketHeader header;
    header.type = frft::PacketType::DATA;
    header.session_id = 0x12345678;
    header.number = 42;

    const auto bytes = frft::serialize_packet(header, payload.data(), payload.size());
    check(bytes.size() == frft::kHeaderSize + payload.size(), "serialized DATA length");
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
    check(decoded.payload == payload, "DATA payload round trip");

    auto malformed = bytes;
    malformed[0] = 0;
    check(!frft::deserialize_packet(malformed.data(), malformed.size(), decoded, error),
          "bad magic is rejected");
    check(!frft::deserialize_packet(bytes.data(), bytes.size() - 1, decoded, error),
          "wrong datagram length is rejected");
}

void test_control_payloads() {
    const frft::StartPayload start {0x0102030405060708ULL, 1448, 1234, 512, 8192, 10};
    frft::StartPayload decoded_start;
    check(frft::deserialize_start(frft::serialize_start(start), decoded_start),
          "START payload decodes");
    check(decoded_start.file_size == start.file_size &&
              decoded_start.chunk_size == start.chunk_size &&
              decoded_start.total_chunks == start.total_chunks &&
              decoded_start.window_chunks == start.window_chunks,
          "START payload round trip");

    const frft::StartAckPayload start_ack {frft::StatusCode::OK, 512, 8192, 10};
    frft::StartAckPayload decoded_start_ack;
    check(frft::deserialize_start_ack(frft::serialize_start_ack(start_ack), decoded_start_ack),
          "START_ACK payload decodes");
    check(decoded_start_ack.status == frft::StatusCode::OK &&
              decoded_start_ack.accepted_window_chunks == 512,
          "START_ACK payload round trip");

    const frft::AckPayload ack {100, 104, 100, 0};
    frft::AckPayload decoded_ack;
    check(frft::deserialize_ack(frft::serialize_ack(ack), decoded_ack), "ACK payload decodes");
    check(decoded_ack.cumulative_ack == 100 && decoded_ack.bitmap_bits == 0,
          "Stage 1 cumulative ACK round trip");

    const frft::CompleteAckPayload complete {
        frft::StatusCode::OK, 1234, 0x0102030405060708ULL, 987654};
    frft::CompleteAckPayload decoded_complete;
    check(frft::deserialize_complete_ack(frft::serialize_complete_ack(complete), decoded_complete),
          "COMPLETE_ACK payload decodes");
    check(decoded_complete.received_bytes == complete.received_bytes &&
              decoded_complete.receiver_transfer_time_us == complete.receiver_transfer_time_us,
          "COMPLETE_ACK payload round trip");
}

void test_stage_one_window_state() {
    frft::SenderWindow sender(5, 2);
    check(sender.can_send() && sender.take_next_sequence() == 0, "sender selects chunk zero");
    check(sender.can_send() && sender.take_next_sequence() == 1, "sender fills its window");
    check(!sender.can_send(), "sender stops at the fixed window edge");
    check(sender.apply_cumulative_ack(1), "cumulative ACK advances window base");
    check(sender.can_send() && sender.take_next_sequence() == 2, "ACK opens one window slot");

    frft::ReceiverTracker receiver(4);
    check(receiver.mark_received(1), "receiver accepts out-of-order chunk");
    check(receiver.cumulative_ack() == 0, "gap holds cumulative ACK at zero");
    check(receiver.mark_received(0), "receiver accepts missing prefix chunk");
    check(receiver.cumulative_ack() == 2, "cumulative ACK crosses filled gap");
    check(!receiver.mark_received(1) && receiver.received_count() == 2,
          "duplicate chunk does not change receive count");
}

}  // namespace

int main() {
    test_header_and_data();
    test_control_payloads();
    test_stage_one_window_state();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All protocol tests passed\n";
    return 0;
}
