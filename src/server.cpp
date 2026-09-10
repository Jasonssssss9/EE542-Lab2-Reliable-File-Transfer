#include "common.hpp"
#include "file_io.hpp"
#include "protocol.hpp"
#include "reliability.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kServerReceiveBufferBytes = 16 * 1024 * 1024;

struct Options {
    std::string output;
    std::uint16_t port = 0;
    std::uint32_t mtu = 1500;
};

void print_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " --port <port> --output <path> [--mtu <bytes>]\n";
}

std::string next_value(int& index, int argc, char* argv[]) {
    if (++index >= argc) {
        throw std::invalid_argument(std::string("missing value after ") + argv[index - 1]);
    }
    return argv[index];
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--port") {
            const auto value = std::stoul(next_value(i, argc, argv));
            if (value == 0 || value > 65535) {
                throw std::invalid_argument("port must be between 1 and 65535");
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--output") {
            options.output = next_value(i, argc, argv);
        } else if (argument == "--mtu") {
            options.mtu = static_cast<std::uint32_t>(std::stoul(next_value(i, argc, argv)));
        } else if (argument == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }

    if (options.output.empty() || options.port == 0) {
        throw std::invalid_argument("--port and --output are required");
    }
    frft::chunk_size_for_mtu(options.mtu);
    return options;
}

bool same_endpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_family == right.sin_family && left.sin_port == right.sin_port &&
           left.sin_addr.s_addr == right.sin_addr.s_addr;
}

frft::PacketHeader make_header(frft::PacketType type,
                               std::uint32_t session_id,
                               std::uint32_t number) {
    frft::PacketHeader header;
    header.type = type;
    header.session_id = session_id;
    header.number = number;
    return header;
}

void send_packet(int socket_fd,
                 const sockaddr_in& destination,
                 const frft::PacketHeader& header,
                 const std::uint8_t* payload,
                 std::size_t payload_size) {
    const auto datagram = frft::serialize_packet(header, payload, payload_size);
    ssize_t sent;
    do {
        sent = sendto(socket_fd,
                      datagram.data(),
                      datagram.size(),
                      0,
                      reinterpret_cast<const sockaddr*>(&destination),
                      sizeof(destination));
    } while (sent < 0 && errno == EINTR);

    if (sent < 0 || static_cast<std::size_t>(sent) != datagram.size()) {
        throw std::runtime_error(std::string("sendto failed: ") + std::strerror(errno));
    }
}

bool receive_packet(int socket_fd,
                    frft::PacketView& packet,
                    sockaddr_in& source,
                    std::vector<std::uint8_t>& buffer) {
    while (true) {
        socklen_t source_length = sizeof(source);
        const ssize_t received = recvfrom(socket_fd,
                                          buffer.data(),
                                          buffer.size(),
                                          0,
                                          reinterpret_cast<sockaddr*>(&source),
                                          &source_length);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("recvfrom failed: ") + std::strerror(errno));
        }

        std::string error;
        if (frft::deserialize_packet_view(buffer.data(), received, packet, error)) {
            return true;
        }
    }
}

void send_start_ack(int socket_fd,
                    const sockaddr_in& client,
                    std::uint32_t session_id,
                    const frft::StartAckPayload& response) {
    const auto payload = frft::serialize_start_ack(response);
    const auto header = make_header(frft::PacketType::START_ACK, session_id, 0);
    send_packet(socket_fd, client, header, payload.data(), payload.size());
}

void send_ack(int socket_fd,
              const sockaddr_in& client,
              std::uint32_t session_id,
              std::uint32_t ack_number,
              const frft::ReceiverTracker& tracker,
              std::uint16_t bitmap_bits) {
    const frft::AckPayload ack = tracker.make_ack_snapshot(bitmap_bits);
    const auto payload = frft::serialize_ack(ack);
    auto header = make_header(frft::PacketType::ACK, session_id, ack_number);
    header.flags = frft::FLAG_SACK;
    send_packet(socket_fd, client, header, payload.data(), payload.size());
}

void time_wait(int socket_fd,
               const sockaddr_in& client,
               std::uint32_t session_id,
               std::uint32_t total_chunks,
               const std::vector<std::uint8_t>& complete_ack_payload) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::vector<std::uint8_t> buffer(65535);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd descriptor {socket_fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));
        if (ready == 0) {
            return;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
        }

        sockaddr_in source {};
        socklen_t source_length = sizeof(source);
        const ssize_t received = recvfrom(socket_fd,
                                          buffer.data(),
                                          buffer.size(),
                                          0,
                                          reinterpret_cast<sockaddr*>(&source),
                                          &source_length);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("recvfrom failed: ") + std::strerror(errno));
        }

        frft::PacketView packet;
        std::string error;
        if (same_endpoint(source, client) &&
            frft::deserialize_packet_view(buffer.data(), received, packet, error) &&
            packet.header.session_id == session_id &&
            packet.header.type == frft::PacketType::COMPLETE &&
            packet.header.number == total_chunks && packet.payload_size == 0) {
            const auto header =
                make_header(frft::PacketType::COMPLETE_ACK, session_id, total_chunks);
            send_packet(socket_fd,
                        client,
                        header,
                        complete_ack_payload.data(),
                        complete_ack_payload.size());
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
    }
}

int run_server(const Options& options) {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    try {
        const int reuse = 1;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (setsockopt(socket_fd,
                       SOL_SOCKET,
                       SO_RCVBUF,
                       &kServerReceiveBufferBytes,
                       sizeof(kServerReceiveBufferBytes)) != 0) {
            throw std::runtime_error(std::string("cannot increase UDP receive buffer: ") +
                                     std::strerror(errno));
        }

        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(options.port);
        if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
        }

        std::cout << "Waiting for one client on UDP port " << options.port << "\n";

        std::vector<std::uint8_t> receive_buffer(65535);
        frft::PacketView start_packet;
        sockaddr_in client {};
        frft::StartPayload start;
        while (true) {
            receive_packet(socket_fd, start_packet, client, receive_buffer);
            if (start_packet.header.type != frft::PacketType::START ||
                start_packet.header.session_id == 0 || start_packet.header.number != 0 ||
                !frft::deserialize_start(
                    start_packet.payload, start_packet.payload_size, start)) {
                continue;
            }

            bool valid = start.chunk_size == frft::chunk_size_for_mtu(options.mtu) &&
                         start.total_chunks == frft::chunk_count(start.file_size, start.chunk_size) &&
                         start.window_chunks > 0 &&
                         start.ack_bitmap_bits == frft::kDefaultAckBitmapBits;
            if (!valid) {
                const frft::StartAckPayload rejected {frft::StatusCode::INVALID_REQUEST, 0, 0, 0};
                send_start_ack(socket_fd, client, start_packet.header.session_id, rejected);
                continue;
            }
            break;
        }

        const std::uint32_t session_id = start_packet.header.session_id;
        std::unique_ptr<frft::MappedOutputFile> output;
        try {
            output = std::make_unique<frft::MappedOutputFile>(options.output, start.file_size);
        } catch (...) {
            const frft::StartAckPayload failed {frft::StatusCode::IO_ERROR, 0, 0, 0};
            send_start_ack(socket_fd, client, session_id, failed);
            throw;
        }

        const frft::StartAckPayload accepted {
            frft::StatusCode::OK,
            std::min<std::uint32_t>(start.window_chunks, frft::kDefaultAckBitmapBits),
            frft::kDefaultAckBitmapBits,
            start.ack_interval_ms,
        };
        send_start_ack(socket_fd, client, session_id, accepted);

        frft::ReceiverTracker tracker(start.total_chunks);

        /*ack interval*/
        const auto ack_interval = std::chrono::milliseconds(
        std::max<std::uint16_t>(
            1,
            accepted.accepted_ack_interval_ms));

        bool ack_pending = false;
        std::uint32_t packets_since_ack = 0;

        auto next_ack_time = std::chrono::steady_clock::now() + ack_interval;
        constexpr std::uint32_t kAckPacketThreshold = 32;
        /*ack interval*/

        std::uint32_t ack_number = 0;
        std::uint64_t duplicate_packets = 0;
        bool received_first_data = false;
        std::chrono::steady_clock::time_point first_data_time;
        std::chrono::steady_clock::time_point last_data_time;

        while (true) {
            frft::PacketView packet;
            sockaddr_in source {};
            receive_packet(socket_fd, packet, source, receive_buffer);

            if (packet.header.type == frft::PacketType::START) {
                if (same_endpoint(source, client) && packet.header.session_id == session_id) {
                    send_start_ack(socket_fd, client, session_id, accepted);
                } else {
                    const frft::StartAckPayload busy {frft::StatusCode::BUSY, 0, 0, 0};
                    send_start_ack(socket_fd, source, packet.header.session_id, busy);
                }
                continue;
            }
            if (!same_endpoint(source, client) || packet.header.session_id != session_id) {
                continue;
            }

            if (packet.header.type == frft::PacketType::DATA) {
                const std::uint32_t sequence = packet.header.number;
                if (sequence >= start.total_chunks) {
                    continue;
                }
                const std::uint64_t offset = static_cast<std::uint64_t>(sequence) * start.chunk_size;
                const std::size_t expected_size = static_cast<std::size_t>(
                    std::min<std::uint64_t>(start.chunk_size, start.file_size - offset));
                if (packet.payload_size != expected_size) {
                    continue;
                }

                const bool duplicate = tracker.has_received(sequence);
                if (duplicate) {
                    ++duplicate_packets;
                } else {
                    if (expected_size != 0) {
                        std::memcpy(output->data() + offset, packet.payload, expected_size);
                    }
                    tracker.mark_received(sequence);
                    const auto now = std::chrono::steady_clock::now();
                    if (!received_first_data) {
                        first_data_time = now;
                        received_first_data = true;
                    }
                    last_data_time = now;
                }
                // send_ack(socket_fd,
                //          client,
                //          session_id,
                //          ack_number++,
                //          tracker,
                //          accepted.accepted_bitmap_bits);
                // continue;

                ++packets_since_ack;

                const auto now = std::chrono::steady_clock::now();

                if (!ack_pending) {
                    ack_pending = true;
                    next_ack_time = now + ack_interval;
                }

                const bool retransmitted =
                    (packet.header.flags &
                    frft::FLAG_RETRANSMITTED) != 0;

                const bool should_send_ack =
                    duplicate ||
                    retransmitted ||
                    tracker.complete() ||
                    packets_since_ack >= kAckPacketThreshold ||
                    now >= next_ack_time;

                if (should_send_ack) {
                    send_ack(socket_fd,
                            client,
                            session_id,
                            ack_number++,
                            tracker,
                            accepted.accepted_bitmap_bits);

                    ack_pending = false;
                    packets_since_ack = 0;
                }

                continue;
            }

            if (packet.header.type != frft::PacketType::COMPLETE ||
                packet.header.number != start.total_chunks || packet.payload_size != 0) {
                continue;
            }
            if (!tracker.complete() || tracker.cumulative_ack() != start.total_chunks) {
                send_ack(socket_fd,
                         client,
                         session_id,
                         ack_number++,
                         tracker,
                         accepted.accepted_bitmap_bits);
                continue;
            }

            output->sync();
            const std::uint64_t receiver_time_us = received_first_data
                                                       ? std::chrono::duration_cast<
                                                             std::chrono::microseconds>(
                                                             last_data_time - first_data_time)
                                                             .count()
                                                       : 0;
            const frft::CompleteAckPayload completed {
                frft::StatusCode::OK,
                tracker.received_count(),
                start.file_size,
                receiver_time_us,
            };
            const auto complete_payload = frft::serialize_complete_ack(completed);
            const auto complete_header =
                make_header(frft::PacketType::COMPLETE_ACK, session_id, start.total_chunks);
            send_packet(socket_fd,
                        client,
                        complete_header,
                        complete_payload.data(),
                        complete_payload.size());

            const double seconds = receiver_time_us / 1'000'000.0;
            const double throughput_mbps =
                seconds > 0.0 ? start.file_size * 8.0 / seconds / 1'000'000.0 : 0.0;
            std::cout << "Transfer complete\n"
                      << "  session: " << session_id << '\n'
                      << "  file bytes: " << start.file_size << '\n'
                      << "  unique chunks: " << tracker.received_count() << '\n'
                      << "  duplicate DATA packets: " << duplicate_packets << '\n'
                      << "  receiver data time us: " << receiver_time_us << '\n'
                      << "  receiver throughput Mbps: " << throughput_mbps << '\n';

            time_wait(socket_fd,
                      client,
                      session_id,
                      start.total_chunks,
                      complete_payload);
            close(socket_fd);
            return 0;
        }
    } catch (...) {
        close(socket_fd);
        throw;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run_server(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "server: " << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
