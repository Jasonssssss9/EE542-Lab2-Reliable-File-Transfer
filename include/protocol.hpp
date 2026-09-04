#pragma once

#include "common.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace frft {

enum class PacketType : std::uint8_t {
    START = 1,
    START_ACK = 2,
    DATA = 3,
    ACK = 4,
    COMPLETE = 5,
    COMPLETE_ACK = 6,
    ERROR = 7,
};

enum PacketFlags : std::uint16_t {
    FLAG_CRC32C = 0x0001,
    FLAG_RETRANSMITTED = 0x0002,
    FLAG_SACK = 0x0004,
    FLAG_MD5 = 0x0008,
};

enum class StatusCode : std::uint16_t {
    OK = 0,
    BUSY = 1,
    INVALID_REQUEST = 2,
    IO_ERROR = 3,
    INCOMPLETE = 4,
};

struct PacketHeader {
    std::uint32_t magic = kMagic;
    std::uint8_t version = kVersion;
    PacketType type = PacketType::ERROR;
    std::uint16_t flags = 0;
    std::uint32_t session_id = 0;
    std::uint32_t number = 0;
    std::uint16_t payload_length = 0;
    std::uint16_t header_length = kHeaderSize;
    std::uint32_t crc32c = 0;
};

struct Packet {
    PacketHeader header;
    std::vector<std::uint8_t> payload;
};

struct StartPayload {
    std::uint64_t file_size = 0;
    std::uint32_t chunk_size = 0;
    std::uint32_t total_chunks = 0;
    std::uint32_t window_chunks = 0;
    std::uint16_t ack_bitmap_bits = 0;
    std::uint16_t ack_interval_ms = 0;
};

struct StartAckPayload {
    StatusCode status = StatusCode::OK;
    std::uint32_t accepted_window_chunks = 0;
    std::uint16_t accepted_bitmap_bits = 0;
    std::uint16_t accepted_ack_interval_ms = 0;
};

struct AckPayload {
    std::uint32_t cumulative_ack = 0;
    std::uint32_t largest_received_plus_one = 0;
    std::uint32_t bitmap_base = 0;
    std::uint16_t bitmap_bits = 0;
};

struct CompleteAckPayload {
    StatusCode status = StatusCode::OK;
    std::uint32_t received_chunks = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t receiver_transfer_time_us = 0;
};

std::uint32_t chunk_size_for_mtu(std::uint32_t mtu);
std::uint32_t chunk_count(std::uint64_t file_size, std::uint32_t chunk_size);

std::vector<std::uint8_t> serialize_packet(const PacketHeader& header,
                                           const std::uint8_t* payload,
                                           std::size_t payload_size);
bool deserialize_packet(const std::uint8_t* data,
                        std::size_t size,
                        Packet& packet,
                        std::string& error);

std::vector<std::uint8_t> serialize_start(const StartPayload& payload);
bool deserialize_start(const std::vector<std::uint8_t>& data, StartPayload& payload);

std::vector<std::uint8_t> serialize_start_ack(const StartAckPayload& payload);
bool deserialize_start_ack(const std::vector<std::uint8_t>& data, StartAckPayload& payload);

std::vector<std::uint8_t> serialize_ack(const AckPayload& payload);
bool deserialize_ack(const std::vector<std::uint8_t>& data, AckPayload& payload);

std::vector<std::uint8_t> serialize_complete_ack(const CompleteAckPayload& payload);
bool deserialize_complete_ack(const std::vector<std::uint8_t>& data,
                              CompleteAckPayload& payload);

}  // namespace frft
