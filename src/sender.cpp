/* src/sender.cpp
   C++ IPv6 multicast sender (roundsend) with stream_id.
   Header per packet (12 bytes):
     4 bytes stream_id (BE)
     4 bytes sequence  (BE)
     4 bytes flags     (BE) - bit0 = final
*/
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static constexpr size_t PAYLOAD_SIZE = 1200;
static constexpr size_t HDR_LEN = 12;
static constexpr uint32_t FLAG_FINAL = 1;

volatile sig_atomic_t g_interrupted = 0;
void sigint_handler(int) { g_interrupted = 1; }

int main(int argc, char** argv) {
    std::string iface;
    std::string addr = "ff3e::1";
    int port = 12345;
    std::string filename;
    int pps = 0;
    uint32_t stream_id = 1;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if ((a == "-i" || a == "--iface") && i + 1 < argc) iface = argv[++i];
        else if ((a == "-a" || a == "--addr") && i + 1 < argc) addr = argv[++i];
        else if ((a == "-p" || a == "--port") && i + 1 < argc) port = std::stoi(argv[++i]);
        else if ((a == "-f" || a == "--file") && i + 1 < argc) filename = argv[++i];
        else if ((a == "-r" || a == "--pps") && i + 1 < argc) pps = std::stoi(argv[++i]);
        else if ((a == "-S" || a == "--stream-id") && i + 1 < argc) stream_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "-h" || a == "--help") {
            std::cerr << "Usage: " << argv[0] << " -f file [-S stream_id] [-a addr] [-p port] [-i iface] [-r pps]\n";
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

    int hops = 64;
    setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));

    unsigned int ifindex = 0;
    if (!iface.empty()) {
        ifindex = if_nametoindex(iface.c_str());
        if (ifindex != 0) setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
    }

    struct sockaddr_in6 dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    dst.sin6_port = htons(port);
    if (inet_pton(AF_INET6, addr.c_str(), &dst.sin6_addr) != 1) {
        std::cerr << "Error: invalid IPv6 address: " << addr << "\n";
        close(sock);
        return 5;
    }
    dst.sin6_scope_id = ifindex;

    std::vector<char> buf(HDR_LEN + PAYLOAD_SIZE);
    uint32_t seq = 1;
    double interval = 0.0;
    if (pps > 0) interval = 1.0 / double(pps);
    auto last = std::chrono::steady_clock::now();

    std::cerr << "Sending " << filename << " as stream_id=" << stream_id << " -> [" << addr << "]:" << port
              << " (iface=" << iface << ", pps=" << pps << ")\n";

    while (!g_interrupted) {
        infile.read(buf.data() + HDR_LEN, PAYLOAD_SIZE);
        std::streamsize n = infile.gcount();

        // If no bytes read and EOF, we're done
        if (n <= 0) {
            // file fully sent already (this handles exact-multiple sizes)
            break;
        }

        // Determine if this is the final chunk:
        // final if we read less than PAYLOAD_SIZE OR if EOF is set after read
        bool is_final = (static_cast<size_t>(n) < PAYLOAD_SIZE) || infile.eof();

        uint32_t sid_be = htonl(stream_id);
        uint32_t seq_be = htonl(seq);
        uint32_t flags_be = htonl(is_final ? FLAG_FINAL : 0);

        std::memcpy(buf.data(), &sid_be, 4);
        std::memcpy(buf.data()+4, &seq_be, 4);
        std::memcpy(buf.data()+8, &flags_be, 4);

        size_t tosend = HDR_LEN + static_cast<size_t>(n);

        if (interval > 0.0) {
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = now - last;
            if (elapsed.count() < interval) {
                std::this_thread::sleep_for(std::chrono::duration<double>(interval - elapsed.count()));
            }
            last = std::chrono::steady_clock::now();
        }

        ssize_t wrote = sendto(sock, buf.data(), tosend, 0, (struct sockaddr*)&dst, sizeof(dst));
        if (wrote < 0) {
            perror("sendto");
            break;
        }

        if (is_final) {
            std::cerr << "Sent final packet seq=" << seq << "\n";
            ++seq; // increment for the final marker consistency
            break;
        }
        ++seq;
    }

    // If interrupted before we've sent final, try to send a final marker
    if (g_interrupted) {
        uint32_t sid_be = htonl(stream_id), seq_be = htonl(seq), flags_be = htonl(FLAG_FINAL);
        std::memcpy(buf.data(), &sid_be, 4);
        std::memcpy(buf.data()+4, &seq_be, 4);
        std::memcpy(buf.data()+8, &flags_be, 4);
        sendto(sock, buf.data(), HDR_LEN, 0, (struct sockaddr*)&dst, sizeof(dst));
        std::cerr << "Interrupted: sent final marker seq=" << seq << "\n";
    }

    // send final marker a few times to increase chance of reception
    for (int i = 0; i < 3; ++i) {
        uint32_t sid_be = htonl(stream_id), seq_be = htonl(seq), flags_be = htonl(FLAG_FINAL);
        std::memcpy(buf.data(), &sid_be, 4);
        std::memcpy(buf.data()+4, &seq_be, 4);
        std::memcpy(buf.data()+8, &flags_be, 4);
        sendto(sock, buf.data(), HDR_LEN, 0, (struct sockaddr*)&dst, sizeof(dst));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    close(sock);
    return 0;
}
