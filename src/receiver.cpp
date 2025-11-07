/* src/receiver.cpp
   Receiver that supports multiple stream_ids.
   - Subscribe to specific streams with -s "42,43"
   - Or use -s all to accept any stream; files are created per stream.
   - Output pattern: -o "out_{id}.mp4" (use {id} placeholder for per-stream files)
   - If subscribing to a single stream and -o "-" is given, data goes to stdout.
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
#include <set>
#include <sstream>
#include <string>
#include <vector>

static constexpr size_t PAYLOAD_SIZE = 1200;
static constexpr size_t HDR_LEN = 12;
static constexpr uint32_t FLAG_FINAL = 1;
static constexpr size_t MAX_PKT = HDR_LEN + PAYLOAD_SIZE;

struct StreamState {
    uint32_t expected = 1;
    std::map<uint32_t, std::vector<char>> buffer;
    bool final_seen = false;
    uint32_t final_seq = 0;
    std::chrono::steady_clock::time_point final_at;
    std::ofstream fout;
    bool has_file = false;
};

static std::set<uint32_t> parse_list(const std::string &s) {
    std::set<uint32_t> out;
    if (s.empty()) return out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        try {
            uint32_t v = static_cast<uint32_t>(std::stoul(item));
            out.insert(v);
        } catch (...) {}
    }
    return out;
}

int main(int argc, char** argv) {
    std::string iface;
    std::string addr = "ff3e::1";
    int port = 12345;
    std::string out_pattern = "stream_{id}.mp4";
    std::string subscribe = "all"; // "all" or comma list
    int timeout = 10;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if ((a == "-i" || a == "--iface") && i + 1 < argc) iface = argv[++i];
        else if ((a == "-a" || a == "--addr") && i + 1 < argc) addr = argv[++i];
        else if ((a == "-p" || a == "--port") && i + 1 < argc) port = std::stoi(argv[++i]);
        else if ((a == "-o" || a == "--out") && i + 1 < argc) out_pattern = argv[++i];
        else if ((a == "-s" || a == "--subscribe") && i + 1 < argc) subscribe = argv[++i];
        else if ((a == "-t" || a == "--timeout") && i + 1 < argc) timeout = std::stoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::cerr << "Usage: " << argv[0] << " -s all|id1,id2 [-o pattern] [-a addr] [-p port] [-i iface] [-t timeout]\n";
            return 1;
        }
    }

    bool subscribe_all = (subscribe == "all");
    std::set<uint32_t> subs;
    if (!subscribe_all) subs = parse_list(subscribe);

    unsigned int ifindex = 0;
    if (!iface.empty()) {
        ifindex = if_nametoindex(iface.c_str());
        if (ifindex == 0) std::cerr << "Warning: interface not found: " << iface << "\n";
    }

    int sock = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 2; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in6 local{};
    local.sin6_family = AF_INET6;
    local.sin6_addr = in6addr_any;
    local.sin6_port = htons(port);
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) { perror("bind"); close(sock); return 3; }

    struct ipv6_mreq mreq{};
    if (inet_pton(AF_INET6, addr.c_str(), &mreq.ipv6mr_multiaddr) != 1) {
        std::cerr << "Error: invalid IPv6 address: " << addr << "\n";
        close(sock);
        return 4;
    }
    mreq.ipv6mr_interface = ifindex;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt(IPV6_JOIN_GROUP)");
        close(sock);
        return 5;
    }

    // recv timeout
    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::cerr << "Listening on [" << addr << "]:" << port << " (iface=" << iface << "), subscribe=" << subscribe << "\n";

    std::map<uint32_t, StreamState> streams;
    std::vector<char> rxbuf(MAX_PKT);

    bool single_to_stdout = false;
    if (!subscribe_all && subs.size() == 1 && out_pattern == "-") single_to_stdout = true;

    while (true) {
        ssize_t n = recv(sock, rxbuf.data(), rxbuf.size(), 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
                // timeout / check final timeouts
                for (auto &p : streams) {
                    StreamState &st = p.second;
                    if (st.final_seen) {
                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::seconds>(now - st.final_at).count() > timeout) {
                            std::cerr << "Timeout waiting for missing packets for stream " << p.first << "\n";
                            // allow finishing
                            st.final_seen = false; // break condition below uses expected > final_seq
                        }
                    }
                }
                // check global finish condition only per-stream (we don't auto-exit unless all subscribed streams finished)
                bool all_done = true;
                if (subscribe_all) all_done = false; // don't auto-exit
                else {
                    for (uint32_t sid : subs) {
                        auto it = streams.find(sid);
                        if (it == streams.end()) { all_done = false; break; }
                        StreamState &st = it->second;
                        if (!(st.final_seen && st.expected > st.final_seq)) { all_done = false; break; }
                    }
                }
                if (all_done) break;
                continue;
            } else {
                perror("recv");
                break;
            }
        }

        if ((size_t)n < HDR_LEN) continue;

        uint32_t sid_be = 0, seq_be = 0, flags_be = 0;
        std::memcpy(&sid_be, rxbuf.data(), 4);
        std::memcpy(&seq_be, rxbuf.data()+4, 4);
        std::memcpy(&flags_be, rxbuf.data()+8, 4);
        uint32_t sid = ntohl(sid_be), seq = ntohl(seq_be), flags = ntohl(flags_be);

        if (!subscribe_all) {
            if (subs.find(sid) == subs.end()) continue; // not subscribed
        }

        // ensure stream state exists
        StreamState &st = streams[sid];

        // open file if not yet
        if (!st.has_file) {
            if (!single_to_stdout) {
                // create filename from pattern
                std::string fname = out_pattern;
                size_t pos = fname.find("{id}");
                if (pos != std::string::npos) {
                    fname.replace(pos, 4, std::to_string(sid));
                }
                st.fout.open(fname, std::ios::binary);
                if (!st.fout) {
                    std::cerr << "Error: cannot open output file: " << fname << " for stream " << sid << "\n";
                } else {
                    st.has_file = true;
                    std::cerr << "Opened output file " << fname << " for stream " << sid << "\n";
                }
            } else {
                std::cerr << "Streaming stream " << sid << " to stdout\n";
                st.has_file = false; // writing to stdout handled below
            }
        }

        std::vector<char> payload;
        if ((size_t)n > HDR_LEN) payload.assign(rxbuf.begin()+HDR_LEN, rxbuf.begin()+n);

        if (seq < st.expected) {
            continue; // duplicate/old
        } else if (seq == st.expected) {
            if (!payload.empty()) {
                if (single_to_stdout && streams.size() == 1 && st.has_file==false) {
                    std::cout.write(payload.data(), payload.size());
                    std::cout.flush();
                } else if (st.has_file) {
                    st.fout.write(payload.data(), payload.size());
                }
            }
            st.expected++;
            // flush buffered
            while (true) {
                auto it = st.buffer.find(st.expected);
                if (it == st.buffer.end()) break;
                if (!it->second.empty()) {
                    if (single_to_stdout && streams.size() == 1 && st.has_file==false) {
                        std::cout.write(it->second.data(), it->second.size());
                        std::cout.flush();
                    } else if (st.has_file) {
                        st.fout.write(it->second.data(), it->second.size());
                    }
                }
                st.buffer.erase(it);
                st.expected++;
            }
        } else {
            // out of order
            if (st.buffer.find(seq) == st.buffer.end()) st.buffer[seq] = std::move(payload);
        }

        if (flags & FLAG_FINAL) {
            st.final_seen = true;
            st.final_seq = seq;
            st.final_at = std::chrono::steady_clock::now();
            std::cerr << "Final marker seen for stream " << sid << " seq=" << seq << "\n";
        }

        // If this stream finished, optionally close file
        if (st.final_seen && st.expected > st.final_seq) {
            std::cerr << "Stream " << sid << " finished (expected=" << st.expected << " final=" << st.final_seq << ")\n";
            if (st.has_file && st.fout.is_open()) st.fout.close();
            // if subscribed to a finite set of streams and all finished, exit; handled in timeout check above
            if (!subscribe_all) {
                bool all_done = true;
                for (uint32_t sid2 : subs) {
                    auto it = streams.find(sid2);
                    if (it == streams.end()) { all_done = false; break; }
                    StreamState &st2 = it->second;
                    if (!(st2.final_seen && st2.expected > st2.final_seq)) { all_done = false; break; }
                }
                if (all_done) break;
            }
        }
    }

    // flush remaining buffered in-order
    for (auto &p : streams) {
        StreamState &st = p.second;
        while (true) {
            auto it = st.buffer.find(st.expected);
            if (it == st.buffer.end()) break;
            if (st.has_file) st.fout.write(it->second.data(), it->second.size());
            st.buffer.erase(it);
            st.expected++;
        }
        if (st.has_file && st.fout.is_open()) st.fout.close();
    }

    close(sock);
    return 0;
}
