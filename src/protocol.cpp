#include "protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace frft {
namespace {

void write_u16(std::uint8_t* out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value >> 8);
    out[offset + 1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t* out, std::size_t offset, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        out[offset + (3 - i)] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void write_u64(std::uint8_t* out, std::size_t offset, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[offset + (7 - i)] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

std::uint16_t read_u16(const std::uint8_t* data) {
    return (static_cast<std::uint16_t>(data[0]) << 8) |
           static_cast<std::uint16_t>(data[1]);
}

std::uint32_t read_u32(const std::uint8_t* data) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

std::uint64_t read_u64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

bool valid_packet_type(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(PacketType::START) &&
           value <= static_cast<std::uint8_t>(PacketType::ERROR);
}

}  // namespace

std::uint32_t chunk_size_for_mtu(std::uint32_t mtu) {
    const std::size_t overhead = kIpv4UdpOverhead + kHeaderSize;
    if (mtu <= overhead || mtu > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("MTU must be between 53 and 65535 bytes");
    }
    return mtu - static_cast<std::uint32_t>(overhead);
}

std::uint32_t chunk_count(std::uint64_t file_size, std::uint32_t chunk_size) {
    if (chunk_size == 0) {
        throw std::invalid_argument("chunk size must be non-zero");
    }
    const std::uint64_t count = file_size / chunk_size + (file_size % chunk_size != 0);
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("file needs more than 32-bit chunk IDs");
    }
    return static_cast<std::uint32_t>(count);
}

std::array<std::uint8_t, kHeaderSize> serialize_packet_header(
    const PacketHeader& header,
    std::size_t payload_size) {
    if (payload_size > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("packet payload is too large");
    }

    std::array<std::uint8_t, kHeaderSize> out {};
    write_u32(out.data(), 0, header.magic);
    out[4] = header.version;
    out[5] = static_cast<std::uint8_t>(header.type);
    write_u16(out.data(), 6, header.flags);
    write_u32(out.data(), 8, header.session_id);
    write_u32(out.data(), 12, header.number);
    write_u16(out.data(), 16, static_cast<std::uint16_t>(payload_size));
    write_u16(out.data(), 18, kHeaderSize);
    write_u32(out.data(), 20, header.crc32c);
    return out;
}

std::vector<std::uint8_t> serialize_packet(const PacketHeader& header,
                                           const std::uint8_t* payload,
                                           std::size_t payload_size) {
    if (payload_size != 0 && payload == nullptr) {
        throw std::invalid_argument("packet payload is null");
    }

    const auto serialized_header = serialize_packet_header(header, payload_size);
    std::vector<std::uint8_t> out(kHeaderSize + payload_size, 0);
    std::copy(serialized_header.begin(), serialized_header.end(), out.begin());
    if (payload_size != 0) {
        std::copy(payload, payload + payload_size, out.begin() + kHeaderSize);
    }
    return out;
}

bool deserialize_packet_view(const std::uint8_t* data,
                             std::size_t size,
                             PacketView& packet,
                             std::string& error) {
    if (data == nullptr || size < kHeaderSize) {
        error = "datagram is shorter than the FRFT header";
        return false;
    }

    PacketHeader header;
    header.magic = read_u32(data);
    header.version = data[4];
    if (!valid_packet_type(data[5])) {
        error = "unknown packet type";
        return false;
    }
    header.type = static_cast<PacketType>(data[5]);
    header.flags = read_u16(data + 6);
    header.session_id = read_u32(data + 8);
    header.number = read_u32(data + 12);
    header.payload_length = read_u16(data + 16);
    header.header_length = read_u16(data + 18);
    header.crc32c = read_u32(data + 20);

    if (header.magic != kMagic) {
        error = "invalid FRFT magic";
        return false;
    }
    if (header.version != kVersion) {
        error = "unsupported FRFT version";
        return false;
    }
    if (header.header_length != kHeaderSize) {
        error = "invalid FRFT header length";
        return false;
    }
    if (size != header.header_length + header.payload_length) {
        error = "datagram length does not match header";
        return false;
    }
    if ((header.flags & FLAG_CRC32C) != 0) {
        error = "CRC32C is not supported in Stage 1";
        return false;
    }

    packet.header = header;
    packet.payload = data + kHeaderSize;
    packet.payload_size = header.payload_length;
    error.clear();
    return true;
}

bool deserialize_packet(const std::uint8_t* data,
                        std::size_t size,
                        Packet& packet,
                        std::string& error) {
    PacketView view;
    if (!deserialize_packet_view(data, size, view, error)) {
        return false;
    }
    packet.header = view.header;
    packet.payload.assign(view.payload, view.payload + view.payload_size);
    return true;
}

std::vector<std::uint8_t> serialize_start(const StartPayload& payload) {
    std::vector<std::uint8_t> out(24, 0);
    write_u64(out.data(), 0, payload.file_size);
    write_u32(out.data(), 8, payload.chunk_size);
    write_u32(out.data(), 12, payload.total_chunks);
    write_u32(out.data(), 16, payload.window_chunks);
    write_u16(out.data(), 20, payload.ack_bitmap_bits);
    write_u16(out.data(), 22, payload.ack_interval_ms);
    return out;
}

bool deserialize_start(const std::uint8_t* data,
                       std::size_t size,
                       StartPayload& payload) {
    if (data == nullptr || size != 24) {
        return false;
    }
    payload.file_size = read_u64(data);
    payload.chunk_size = read_u32(data + 8);
    payload.total_chunks = read_u32(data + 12);
    payload.window_chunks = read_u32(data + 16);
    payload.ack_bitmap_bits = read_u16(data + 20);
    payload.ack_interval_ms = read_u16(data + 22);
    return true;
}

bool deserialize_start(const std::vector<std::uint8_t>& data, StartPayload& payload) {
    return deserialize_start(data.data(), data.size(), payload);
}

std::vector<std::uint8_t> serialize_start_ack(const StartAckPayload& payload) {
    std::vector<std::uint8_t> out(12, 0);
    write_u16(out.data(), 0, static_cast<std::uint16_t>(payload.status));
    write_u32(out.data(), 4, payload.accepted_window_chunks);
    write_u16(out.data(), 8, payload.accepted_bitmap_bits);
    write_u16(out.data(), 10, payload.accepted_ack_interval_ms);
    return out;
}

bool deserialize_start_ack(const std::uint8_t* data,
                           std::size_t size,
                           StartAckPayload& payload) {
    if (data == nullptr || size != 12) {
        return false;
    }
    payload.status = static_cast<StatusCode>(read_u16(data));
    payload.accepted_window_chunks = read_u32(data + 4);
    payload.accepted_bitmap_bits = read_u16(data + 8);
    payload.accepted_ack_interval_ms = read_u16(data + 10);
    return true;
}

bool deserialize_start_ack(const std::vector<std::uint8_t>& data, StartAckPayload& payload) {
    return deserialize_start_ack(data.data(), data.size(), payload);
}

std::vector<std::uint8_t> serialize_ack(const AckPayload& payload) {
    const std::size_t bitmap_bytes = (payload.bitmap_bits + 7) / 8;
    if (payload.bitmap.size() != bitmap_bytes) {
        throw std::invalid_argument("ACK bitmap length does not match bitmap_bits");
    }

    std::vector<std::uint8_t> out(16 + bitmap_bytes, 0);
    write_u32(out.data(), 0, payload.cumulative_ack);
    write_u32(out.data(), 4, payload.largest_received_plus_one);
    write_u32(out.data(), 8, payload.bitmap_base);
    write_u16(out.data(), 12, payload.bitmap_bits);
    std::copy(payload.bitmap.begin(), payload.bitmap.end(), out.begin() + 16);
    return out;
}

bool deserialize_ack(const std::uint8_t* data, std::size_t size, AckPayload& payload) {
    if (data == nullptr || size < 16) {
        return false;
    }
    const std::uint16_t bitmap_bits = read_u16(data + 12);
    const std::size_t bitmap_bytes = (bitmap_bits + 7) / 8;
    if (size != 16 + bitmap_bytes) {
        return false;
    }

    payload.cumulative_ack = read_u32(data);
    payload.largest_received_plus_one = read_u32(data + 4);
    payload.bitmap_base = read_u32(data + 8);
    payload.bitmap_bits = bitmap_bits;
    payload.bitmap.assign(data + 16, data + size);
    return true;
}

bool deserialize_ack(const std::vector<std::uint8_t>& data, AckPayload& payload) {
    return deserialize_ack(data.data(), data.size(), payload);
}

std::vector<std::uint8_t> serialize_complete_ack(const CompleteAckPayload& payload) {
    std::vector<std::uint8_t> out(24, 0);
    write_u16(out.data(), 0, static_cast<std::uint16_t>(payload.status));
    write_u32(out.data(), 4, payload.received_chunks);
    write_u64(out.data(), 8, payload.received_bytes);
    write_u64(out.data(), 16, payload.receiver_transfer_time_us);
    return out;
}

bool deserialize_complete_ack(const std::uint8_t* data,
                              std::size_t size,
                              CompleteAckPayload& payload) {
    if (data == nullptr || size != 24) {
        return false;
    }
    payload.status = static_cast<StatusCode>(read_u16(data));
    payload.received_chunks = read_u32(data + 4);
    payload.received_bytes = read_u64(data + 8);
    payload.receiver_transfer_time_us = read_u64(data + 16);
    return true;
}

bool deserialize_complete_ack(const std::vector<std::uint8_t>& data,
                              CompleteAckPayload& payload) {
    return deserialize_complete_ack(data.data(), data.size(), payload);
}

}  // namespace frft
