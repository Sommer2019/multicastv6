/*
 src/sender.cpp
 C++ IPv6 multicast sender (roundsend).
 Sends a file in chunks with 8-byte header:
   4 bytes sequence (big-endian uint32)
   4 bytes flags    (big-endian uint32) - bit 0 = final packet
 Build with: g++ -std=c++17 -O2 -o sender src/sender.cpp
*/

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static constexpr size_t PAYLOAD_SIZE = 1200;
static constexpr size_t HDR_LEN = 8;
static constexpr uint32_t FLAG_FINAL = 1;

volatile sig_atomic_t g_interrupted = 0;

void sigint_handler(int) {
    g_interrupted = 1;
}

int main(int argc, char** argv) {
    std::string iface;
    std::string addr = "ff3e::1";
    int port = 12345;
    std::string filename;
    int pps = 0;

    // simple arg parsing
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if ((a == "-i" || a == "--iface") && i + 1 < argc) iface = argv[++i];
        else if ((a == "-a" || a == "--addr") && i + 1 < argc) addr = argv[++i];
        else if ((a == "-p" || a == "--port") && i + 1 < argc) port = std::stoi(argv[++i]);
        else if ((a == "-f" || a == "--file") && i + 1 < argc) filename = argv[++i];
        else if ((a == "-r" || a == "--pps") && i + 1 < argc) pps = std::stoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::cerr << "Usage: " << argv[0] << " -f file [-a addr] [-p port] [-i iface] [-r pps]\n";
            return 1;
        }
    }

    if (filename.empty()) {
        std::cerr << "Error: -f file is required\n";
        return 2;
    }

    std::ifstream infile(filename, std::ios::binary);
    if (!infile) {
        std::cerr << "Error: cannot open file: " << filename << "\n";
        return 3;
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    int sock = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 4;
    }

    // set multicast hop limit
    int hops = 64;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0) {
        perror("setsockopt(IPV6_MULTICAST_HOPS)");
    }

    // set outgoing interface for multicast, if provided
    unsigned int ifindex = 0;
    if (!iface.empty()) {
        ifindex = if_nametoindex(iface.c_str());
        if (ifindex == 0) {
            std::cerr << "Warning: interface not found: " << iface << "\n";
        } else {
            if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex)) < 0) {
                perror("setsockopt(IPV6_MULTICAST_IF)");
            }
        }
    }

    // destination sockaddr
    struct sockaddr_in6 dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    dst.sin6_port = htons(port);
    if (inet_pton(AF_INET6, addr.c_str(), &dst.sin6_addr) != 1) {
        std::cerr << "Error: invalid IPv6 address: " << addr << "\n";
        close(sock);
        return 5;
    }
    if (ifindex != 0) dst.sin6_scope_id = ifindex;

    std::vector<char> buf(HDR_LEN + PAYLOAD_SIZE);
    uint32_t seq = 1;
    double interval = 0.0;
    if (pps > 0) interval = 1.0 / double(pps);
    double last_send = 0.0;

    std::cerr << "Sending " << filename << " -> [" << addr << "]:" << port
              << " (iface=" << iface << ", pps=" << pps << ")\n";

    while (!infile.eof() && !g_interrupted) {
        infile.read(buf.data() + HDR_LEN, PAYLOAD_SIZE);
        std::streamsize n = infile.gcount();

        uint32_t hdr_seq = htonl(seq);
        uint32_t hdr_flags = htonl(0);
        if (infile.eof()) hdr_flags = htonl(FLAG_FINAL);

        std::memcpy(buf.data(), &hdr_seq, 4);
        std::memcpy(buf.data() + 4, &hdr_flags, 4);

        ssize_t tosend = HDR_LEN + n;
        // pacing
        if (interval > 0.0) {
            double now = (double)time(nullptr) + (double)0; // coarse
            double elapsed = now - last_send;
            if (elapsed < interval) {
                std::this_thread::sleep_for(std::chrono::duration<double>(interval - elapsed));
            }
            last_send = (double)time(nullptr);
        }

        ssize_t wrote = sendto(sock, buf.data(), tosend, 0, (struct sockaddr*)&dst, sizeof(dst));
        if (wrote < 0) {
            perror("sendto");
            break;
        }

        if (ntohl(hdr_flags) & FLAG_FINAL) {
            std::cerr << "Sent final packet seq=" << seq << "\n";
            break;
        }
        ++seq;
    }

    // If interrupted, try to send a final marker
    if (g_interrupted) {
        uint32_t hdr_seq = htonl(seq);
        uint32_t hdr_flags = htonl(FLAG_FINAL);
        std::memcpy(buf.data(), &hdr_seq, 4);
        std::memcpy(buf.data() + 4, &hdr_flags, 4);
        sendto(sock, buf.data(), HDR_LEN, 0, (struct sockaddr*)&dst, sizeof(dst));
        std::cerr << "Interrupted: sent final marker seq=" << seq << "\n";
    }

    // send final marker a few times to increase chance of reception
    for (int i = 0; i < 3; ++i) {
        uint32_t hdr_seq = htonl(seq);
        uint32_t hdr_flags = htonl(FLAG_FINAL);
        std::memcpy(buf.data(), &hdr_seq, 4);
        std::memcpy(buf.data() + 4, &hdr_flags, 4);
        sendto(sock, buf.data(), HDR_LEN, 0, (struct sockaddr*)&dst, sizeof(dst));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    close(sock);
    return 0;
