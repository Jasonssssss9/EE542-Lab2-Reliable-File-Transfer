#include "common.hpp"
#include "file_io.hpp"
#include "pacer.hpp"
#include "protocol.hpp"
#include "reliability.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct Options {
    std::string host;
    std::string file;
    std::uint16_t port = 0;
    std::uint32_t mtu = 1500;
    double rate_mbps = 95.0;
};

void print_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " --host <server-ip> --port <port> --file <path>"
                 " [--mtu <bytes>] [--rate <Mbps>]\n";
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
        if (argument == "--host") {
            options.host = next_value(i, argc, argv);
        } else if (argument == "--port") {
            const auto value = std::stoul(next_value(i, argc, argv));
            if (value == 0 || value > 65535) {
                throw std::invalid_argument("port must be between 1 and 65535");
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--file") {
            options.file = next_value(i, argc, argv);
        } else if (argument == "--mtu") {
            options.mtu = static_cast<std::uint32_t>(std::stoul(next_value(i, argc, argv)));
        } else if (argument == "--rate") {
            options.rate_mbps = std::stod(next_value(i, argc, argv));
        } else if (argument == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }

    if (options.host.empty() || options.file.empty() || options.port == 0) {
        throw std::invalid_argument("--host, --port, and --file are required");
    }
    if (!std::isfinite(options.rate_mbps) || options.rate_mbps <= 0.0) {
        throw std::invalid_argument("rate must be greater than zero");
    }
    frft::chunk_size_for_mtu(options.mtu);
    return options;
}

sockaddr_in resolve_server(const Options& options) {
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(options.port);
    const int status = getaddrinfo(options.host.c_str(), port.c_str(), &hints, &result);
    if (status != 0) {
        throw std::runtime_error(std::string("cannot resolve server: ") + gai_strerror(status));
    }

    sockaddr_in address = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);
    return address;
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
                 const sockaddr_in& server,
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
                      reinterpret_cast<const sockaddr*>(&server),
                      sizeof(server));
    } while (sent < 0 && errno == EINTR);

    if (sent < 0 || static_cast<std::size_t>(sent) != datagram.size()) {
        throw std::runtime_error(std::string("sendto failed: ") + std::strerror(errno));
    }
}

bool receive_expected(int socket_fd,
                      const sockaddr_in& server,
                      frft::PacketType expected_type,
                      std::uint32_t session_id,
                      int timeout_ms,
                      frft::Packet& result) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::vector<std::uint8_t> buffer(65535);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd descriptor {socket_fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));
        if (ready == 0) {
            return false;
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

        frft::Packet packet;
        std::string error;
        if (!same_endpoint(source, server) ||
            !frft::deserialize_packet(buffer.data(), received, packet, error) ||
            packet.header.session_id != session_id || packet.header.type != expected_type) {
            continue;
        }
        result = std::move(packet);
        return true;
    }
    return false;
}

void apply_ack_packet(const frft::Packet& packet, frft::SenderWindow& window) {
    frft::AckPayload ack;
    if (!frft::deserialize_ack(packet.payload, ack) || ack.bitmap_bits != 0) {
        return;
    }
    window.apply_cumulative_ack(ack.cumulative_ack);
}

void drain_acks(int socket_fd,
                const sockaddr_in& server,
                std::uint32_t session_id,
                frft::SenderWindow& window) {
    std::vector<std::uint8_t> buffer(65535);
    while (true) {
        sockaddr_in source {};
        socklen_t source_length = sizeof(source);
        const ssize_t received = recvfrom(socket_fd,
                                          buffer.data(),
                                          buffer.size(),
                                          MSG_DONTWAIT,
                                          reinterpret_cast<sockaddr*>(&source),
                                          &source_length);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            throw std::runtime_error(std::string("recvfrom failed: ") + std::strerror(errno));
        }

        frft::Packet packet;
        std::string error;
        if (same_endpoint(source, server) &&
            frft::deserialize_packet(buffer.data(), received, packet, error) &&
            packet.header.session_id == session_id && packet.header.type == frft::PacketType::ACK) {
            apply_ack_packet(packet, window);
        }
    }
}

void wait_for_ack(int socket_fd,
                  const sockaddr_in& server,
                  std::uint32_t session_id,
                  frft::SenderWindow& window) {
    frft::Packet packet;
    if (!receive_expected(socket_fd, server, frft::PacketType::ACK, session_id, 5000, packet)) {
        throw std::runtime_error(
            "no cumulative ACK received; Stage 1 requires a zero-loss network");
    }
    apply_ack_packet(packet, window);
}

std::uint32_t random_session_id() {
    std::random_device random;
    std::uint32_t session_id = 0;
    while (session_id == 0) {
        session_id = (static_cast<std::uint32_t>(random()) << 16) ^ random();
    }
    return session_id;
}

int run_client(const Options& options) {
    frft::MappedInputFile input(options.file);
    const std::uint32_t chunk_size = frft::chunk_size_for_mtu(options.mtu);
    const std::uint32_t total_chunks = frft::chunk_count(input.size(), chunk_size);
    const std::uint32_t window_chunks = static_cast<std::uint32_t>(
        (frft::kDefaultWindowBytes + chunk_size - 1) / chunk_size);
    if (window_chunks > frft::kDefaultAckBitmapBits) {
        throw std::runtime_error("configured MTU makes the default window exceed ACK coverage");
    }

    const sockaddr_in server = resolve_server(options);
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    try {
        const int discover = IP_PMTUDISC_DO;
        if (setsockopt(socket_fd, IPPROTO_IP, IP_MTU_DISCOVER, &discover, sizeof(discover)) != 0) {
            throw std::runtime_error(std::string("cannot enable path-MTU checks: ") +
                                     std::strerror(errno));
        }

        const std::uint32_t session_id = random_session_id();
        const frft::StartPayload start {
            input.size(),
            chunk_size,
            total_chunks,
            window_chunks,
            frft::kDefaultAckBitmapBits,
            frft::kDefaultAckIntervalMs,
        };
        const auto start_payload = frft::serialize_start(start);
        const auto start_header = make_header(frft::PacketType::START, session_id, 0);

        frft::StartAckPayload accepted;
        bool started = false;
        for (int attempt = 0; attempt < frft::kControlAttempts && !started; ++attempt) {
            send_packet(socket_fd,
                        server,
                        start_header,
                        start_payload.data(),
                        start_payload.size());
            frft::Packet response;
            if (!receive_expected(socket_fd,
                                  server,
                                  frft::PacketType::START_ACK,
                                  session_id,
                                  frft::kControlTimeoutMs,
                                  response)) {
                continue;
            }
            if (!frft::deserialize_start_ack(response.payload, accepted)) {
                throw std::runtime_error("server returned an invalid START_ACK");
            }
            if (accepted.status != frft::StatusCode::OK) {
                throw std::runtime_error("server rejected the transfer");
            }
            started = true;
        }
        if (!started) {
            throw std::runtime_error("START handshake timed out after 10 attempts");
        }
        if (accepted.accepted_window_chunks == 0 ||
            accepted.accepted_window_chunks > accepted.accepted_bitmap_bits) {
            throw std::runtime_error("server accepted an invalid sliding window");
        }

        frft::SenderWindow window(total_chunks, accepted.accepted_window_chunks);
        const auto rate_bps = static_cast<std::uint64_t>(options.rate_mbps * 1'000'000.0);
        frft::Pacer pacer(rate_bps);
        std::uint64_t sent_data_packets = 0;
        bool sent_first_data = false;
        std::chrono::steady_clock::time_point first_data_time;

        while (!window.all_acked()) {
            if (!window.can_send()) {
                wait_for_ack(socket_fd, server, session_id, window);
                continue;
            }

            const std::uint32_t sequence = window.take_next_sequence();
            const std::uint64_t offset = static_cast<std::uint64_t>(sequence) * chunk_size;
            const std::size_t payload_size = static_cast<std::size_t>(
                std::min<std::uint64_t>(chunk_size, input.size() - offset));
            const auto header = make_header(frft::PacketType::DATA, session_id, sequence);

            pacer.wait_for_slot(frft::kIpv4UdpOverhead + frft::kHeaderSize + payload_size);
            if (!sent_first_data) {
                first_data_time = std::chrono::steady_clock::now();
                sent_first_data = true;
            }
            send_packet(socket_fd, server, header, input.data() + offset, payload_size);
            ++sent_data_packets;
            drain_acks(socket_fd, server, session_id, window);
        }

        const auto complete_header =
            make_header(frft::PacketType::COMPLETE, session_id, total_chunks);
        frft::CompleteAckPayload completed;
        bool finished = false;
        for (int attempt = 0; attempt < frft::kControlAttempts && !finished; ++attempt) {
            send_packet(socket_fd, server, complete_header, nullptr, 0);
            frft::Packet response;
            if (!receive_expected(socket_fd,
                                  server,
                                  frft::PacketType::COMPLETE_ACK,
                                  session_id,
                                  frft::kControlTimeoutMs,
                                  response)) {
                continue;
            }
            if (!frft::deserialize_complete_ack(response.payload, completed)) {
                throw std::runtime_error("server returned an invalid COMPLETE_ACK");
            }
            finished = completed.status == frft::StatusCode::OK;
        }
        if (!finished) {
            throw std::runtime_error("COMPLETE handshake timed out after 10 attempts");
        }
        if (completed.received_chunks != total_chunks || completed.received_bytes != input.size()) {
            throw std::runtime_error("server completion statistics do not match the input file");
        }

        const auto end_time = std::chrono::steady_clock::now();
        const double elapsed_seconds = sent_first_data
                                           ? std::chrono::duration<double>(end_time - first_data_time)
                                                 .count()
                                           : 0.0;
        const double throughput_mbps =
            elapsed_seconds > 0.0 ? input.size() * 8.0 / elapsed_seconds / 1'000'000.0 : 0.0;

        std::cout << "Transfer complete\n"
                  << "  session: " << session_id << '\n'
                  << "  file bytes: " << input.size() << '\n'
                  << "  DATA packets: " << sent_data_packets << '\n'
                  << "  elapsed seconds: " << elapsed_seconds << '\n'
                  << "  client throughput Mbps: " << throughput_mbps << '\n'
                  << "  receiver data time us: " << completed.receiver_transfer_time_us << '\n';

        close(socket_fd);
        return 0;
    } catch (...) {
        close(socket_fd);
        throw;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run_client(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "client: " << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
