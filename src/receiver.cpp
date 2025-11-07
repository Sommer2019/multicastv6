/*
 src/receiver.cpp
 C++ IPv6 multicast receiver (roundsend).
 Receives packets with 8-byte header:
   4 bytes sequence (big-endian uint32)
   4 bytes flags    (big-endian uint32) - bit 0 = final packet
 Buffers out-of-order and writes in-order payload to file/stdout.
 Build with: g++ -std=c++17 -O2 -o receiver src/receiver.cpp
*/

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

static constexpr size_t PAYLOAD_SIZE = 1200;
static constexpr size_t HDR_LEN = 8;
static constexpr uint32_t FLAG_FINAL = 1;
static constexpr size_t MAX_PKT = HDR_LEN + PAYLOAD_SIZE;

int main(int argc, char** argv) {
    std::string iface;
    std::string addr = "ff3e::1";
    int port = 12345;
    std::string outfile = "out.mp4";
    int timeout = 10;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if ((a == "-i" || a == "--iface") && i + 1 < argc) iface = argv[++i];
        else if ((a == "-a" || a == "--addr") && i + 1 < argc) addr = argv[++i];
        else if ((a == "-p" || a == "--port") && i + 1 < argc) port = std::stoi(argv[++i]);
        else if ((a == "-o" || a == "--out") && i + 1 < argc) outfile = argv[++i];
        else if ((a == "-t" || a == "--timeout") && i + 1 < argc) timeout = std::stoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::cerr << "Usage: " << argv[0] << " -o out.mp4 [-a addr] [-p port] [-i iface] [-t timeout]\n";
            return 1;
        }
    }

    unsigned int ifindex = 0;
    if (!iface.empty()) {
        ifindex = if_nametoindex(iface.c_str());
        if (ifindex == 0) {
            std::cerr << "Warning: interface not found: " << iface << "\n";
        }
    }

    int sock = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 2;
    }

    // allow multiple listeners on same host/port
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    struct sockaddr_in6 local{};
    local.sin6_family = AF_INET6;
    local.sin6_addr = in6addr_any;
    local.sin6_port = htons(port);

    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        perror("bind");
        close(sock);
        return 3;
    }

    // join group
    struct ipv6_mreq mreq{};
    if (inet_pton(AF_INET6, addr.c_str(), &mreq.ipv6mr_multiaddr) != 1) {
        std::cerr << "Error: invalid IPv6 address: " << addr << "\n";
    } else {
        mreq.ipv6mr_interface = ifindex;
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
            perror("setsockopt(IPV6_JOIN_GROUP)");
            // continue - group join may fail if kernel restrictions exist
        }
    }

    std::ostream* outptr = &std::cout;
    std::ofstream fout;
    if (outfile != "-") {
        fout.open(outfile, std::ios::binary);
        if (!fout) {
            std::cerr << "Error: cannot create output file: " << outfile << "\n";
            close(sock);
            return 4;
        }
        outptr = &fout;
    } else {
        // ensure cout is binary-friendly on Windows (not needed on Linux)
    }

    std::map<uint32_t, std::vector<char>> buffer;
    uint32_t expected = 1;
    bool final_seen = false;
    uint32_t final_seq = 0;
    using clock = std::chrono::steady_clock;
    clock::time_point final_at;

    std::vector<char> rxbuf(MAX_PKT);

    // set recv timeout to 1s to allow periodic timeout checks
    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::cerr << "Listening on [" << addr << "]:" << port << " (iface=" << iface << ")\n";

    while (true) {
        ssize_t n = recv(sock, rxbuf.data(), rxbuf.size(), 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                // timeout or interrupt - check final timeout
                if (final_seen) {
                    auto now = clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - final_at).count() > timeout) {
                        std::cerr << "Timeout waiting for missing packets (expected=" << expected
                                  << " final=" << final_seq << " buffered=" << buffer.size() << ")\n";
                        break;
                    }
                }
                continue;
            } else {
                perror("recv");
                break;
            }
        }

        if (n < (ssize_t)HDR_LEN) continue;

        uint32_t seq = 0;
        uint32_t flags = 0;
        std::memcpy(&seq, rxbuf.data(), 4);
        std::memcpy(&flags, rxbuf.data() + 4, 4);
        seq = ntohl(seq);
        flags = ntohl(flags);

        std::vector<char> payload;
        if ((size_t)n > HDR_LEN) payload.assign(rxbuf.begin() + HDR_LEN, rxbuf.begin() + n);

        if (seq < expected) {
            // old/duplicate
            continue;
        } else if (seq == expected) {
            if (!payload.empty()) {
                outptr->write(payload.data(), payload.size());
            }
            expected++;
            // flush contiguous buffered packets
            while (true) {
                auto it = buffer.find(expected);
                if (it == buffer.end()) break;
                if (!it->second.empty()) outptr->write(it->second.data(), it->second.size());
                buffer.erase(it);
                expected++;
            }
        } else {
            // out of order -> buffer
            if (buffer.find(seq) == buffer.end()) buffer[seq] = std::move(payload);
        }

        if (flags & FLAG_FINAL) {
            final_seen = true;
            final_seq = seq;
            final_at = clock::now();
            std::cerr << "Final marker seen seq=" << final_seq << "\n";
        }

        if (final_seen && expected > final_seq) {
            std::cerr << "Received through final sequence; finishing.\n";
            break;
        }
    }

    // best-effort flush remaining in-order buffered packets
    while (true) {
        auto it = buffer.find(expected);
        if (it == buffer.end()) break;
        if (!it->second.empty()) outptr->write(it->second.data(), it->second.size());
        buffer.erase(it);
        expected++;
    }

    if (fout.is_open()) fout.close();
    close(sock);
    return 0;
}
