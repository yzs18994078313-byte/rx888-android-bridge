// RX888 MKII receive-only, loopback SpyServer 2.0.1700 bridge. GPL-3.0-or-later.
#include "radio.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using rx888::Radio;
constexpr uint32_t version = (2u << 24) | 1700u;
constexpr uint32_t min_freq = 10000u, max_freq = 1750000000u;
static_assert(std::atomic<bool>::is_always_lock_free, "Stop signals require lock-free atomic storage");
std::atomic<bool> stopping{false};
bool stop_requested() { return stopping.load(std::memory_order_relaxed); }
void stop_signal(int) { stopping.store(true, std::memory_order_relaxed); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void put32(std::vector<uint8_t>& v, uint32_t n) { for (unsigned k = 0; k < 4; ++k) v.push_back(uint8_t(n >> (8 * k))); }
void put32(uint8_t* p, uint32_t n) { for (unsigned k = 0; k < 4; ++k) p[k] = uint8_t(n >> (8 * k)); }
bool wait_io(int fd, short event, Clock::time_point deadline) {
    while (!stop_requested() && Clock::now() < deadline) {
        pollfd p{fd, event, 0};
        const int ready = poll(&p, 1, 50);
        if (ready > 0) return !(p.revents & (POLLHUP | POLLERR | POLLNVAL)) && (p.revents & event);
        if (ready < 0 && errno != EINTR) return false;
    }
    return false;
}
bool receive_exact(int fd, void* data, size_t n) {
    auto* p = static_cast<uint8_t*>(data);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (n && !stop_requested()) {
        const ssize_t k = recv(fd, p, n, MSG_DONTWAIT);
        if (k > 0) { p += k; n -= size_t(k); continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_io(fd, POLLIN, deadline)) continue;
        return false;
    }
    return n == 0;
}
struct Peer {
    int fd;
    uint32_t sequence = 0;
    uint64_t backpressure_events = 0, peak_send_us = 0;
    bool packet(uint32_t type, const void* body, size_t size, bool iq = false) {
        // Send the header and IQ together; no allocation or tiny header send in the hot path.
        std::array<uint8_t, 20> h{};
        put32(h.data(), version); put32(h.data() + 4, type);
        put32(h.data() + 8, iq ? 1 : 0); put32(h.data() + 12, sequence++); put32(h.data() + 16, uint32_t(size));
        iovec buffers[2]{{h.data(), h.size()}, {const_cast<void*>(body), size}};
        size_t index = 0;
        const auto begin = Clock::now(), deadline = begin + std::chrono::seconds(2);
        while (index < 2 && !stop_requested()) {
            if (!buffers[index].iov_len) { ++index; continue; }
            msghdr message{}; message.msg_iov = buffers + index; message.msg_iovlen = 2 - index;
            const ssize_t result = sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (result > 0) {
                size_t left = size_t(result);
                while (index < 2 && left >= buffers[index].iov_len) { left -= buffers[index].iov_len; ++index; }
                if (index < 2 && left) { buffers[index].iov_base = static_cast<uint8_t*>(buffers[index].iov_base) + left; buffers[index].iov_len -= left; }
                continue;
            }
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ++backpressure_events;
                if (wait_io(fd, POLLOUT, deadline)) continue;
            }
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
        peak_send_us = std::max(peak_send_us, uint64_t(elapsed));
        return index == 2;
    }
    bool words(uint32_t type, std::initializer_list<uint32_t> words) {
        std::vector<uint8_t> body; body.reserve(words.size() * 4);
        for (uint32_t v : words) put32(body, v);
        return packet(type, body.data(), body.size());
    }
};
bool sync(Peer& p, const Radio& r) {
    return p.words(1, {1, r.gain, r.frequency, r.frequency, 0, min_freq, max_freq, min_freq, max_freq});
}
void session(int fd, Radio& r) {
    int one = 1, send_buffer = 2 * 1024 * 1024;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof send_buffer);
    Peer peer{fd};
    bool hello = false;
    auto last_stats = Clock::now();
    uint64_t total = 0, peak_queue_us = 0, last_usb = r.usb_errors();
    while (!stop_requested()) {
        r.check_receiver();
        pollfd events{fd, POLLIN, 0};
        const int ready = poll(&events, 1, r.streaming ? (r.queue.front() ? 0 : 2) : 200);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (events.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
        if (events.revents & POLLIN) {
            uint8_t h[8];
            if (!receive_exact(fd, h, sizeof h)) break;
            const uint32_t cmd = le32(h), n = le32(h + 4);
            if (n > 1024) { std::puts("Rejected oversized command"); break; }
            std::vector<uint8_t> body(n);
            if (!receive_exact(fd, body.data(), body.size())) break;
            if (cmd == 0) {
                if (n < 4 || (le32(body.data()) >> 24) != 2) break;
                hello = true;
                if (!peer.words(0, {0x8882, r.serial, r.maximum_rate, r.maximum_rate, 5, 1, r.maximum_gain, min_freq, max_freq, 16, 0, 2}) || !sync(peer, r)) break;
            } else if (cmd == 2) {
                if (!hello || n != 8) break;
                const uint32_t id = le32(body.data()), value = le32(body.data() + 4);
                if (id == 0 && value != 1) { std::puts("Only IQ streaming mode is supported"); break; }
                if (id == 1) { if (value) r.start(); else r.halt(); }
                else if (id == 2) r.set_gain(value);
                else if (id == 100 && value != 2) { std::puts("Only 16-bit IQ is advertised and supported"); break; }
                else if (id == 101) r.set_frequency(std::clamp(value, min_freq, max_freq));
                else if (id == 102) { if (value > 4) break; if (value != r.decimation) { r.decimation = value; r.apply(); } }
                if (!sync(peer, r)) break;
            } else std::printf("Ignored command %u\n", cmd);
        }
        if (hello && r.streaming) {
            if (const auto* block = r.queue.front()) {
                const size_t count = block->count;
                const auto age = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - block->queued).count();
                peak_queue_us = std::max(peak_queue_us, uint64_t(age));
                if (!peer.packet(101, block->data.data(), count * sizeof(rx888::Sample), true)) break;
                r.queue.release(); total += count;
            }
            const auto now = Clock::now();
            if (now - last_stats >= std::chrono::seconds(5)) {
                const uint64_t usb = r.usb_errors();
                std::printf("STAT: rate=%.0f rx_samples=%llu sent_samples=%llu usb_errors=%llu (+%llu) queue_dropped_samples=%llu queue=%zu/%zu peak_blocks=%zu queue_peak_ms=%.1f tcp_waits=%llu send_peak_ms=%.1f retune_discarded=%llu raw_dropped_samples=%llu usb_bytes=%llu IQ=16-bit\n",
                    r.rate, (unsigned long long)r.received.load(), (unsigned long long)total,
                    (unsigned long long)usb, (unsigned long long)(usb - last_usb),
                    (unsigned long long)r.queue_dropped.load(), r.queue.depth(), r.queue.capacity(), r.peak_blocks.load(), peak_queue_us / 1000.0,
                    (unsigned long long)peer.backpressure_events, peer.peak_send_us / 1000.0, (unsigned long long)r.retune_discarded, (unsigned long long)r.raw_dropped.load(), (unsigned long long)r.usb_bytes());
                last_usb = usb; last_stats = now;
            }
        }
    }
    r.halt();
    std::printf("Client disconnected: sent_samples=%llu usb_errors=%llu queue_dropped_samples=%llu\n",
        (unsigned long long)total, (unsigned long long)r.usb_errors(), (unsigned long long)r.queue_dropped.load());
}
void watch_stdin(bool managed) {
    std::thread([managed] {
        std::string pending;
        char input[64];
        while (!stop_requested()) {
            const ssize_t n = read(STDIN_FILENO, input, sizeof input);
            if (n > 0) {
                pending.append(input, size_t(n));
                if (pending.find("STOP") != std::string::npos) { stopping.store(true); break; }
                if (pending.size() > 128) pending.erase(0, pending.size() - 16);
            } else if (n < 0 && errno == EINTR) continue;
            else { if (managed) stopping.store(true); break; }
        }
    }).detach();
}
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); setvbuf(stderr, nullptr, _IOLBF, 0);
    signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal); signal(SIGPIPE, SIG_IGN);
    int server = -1, port = 5557;
    std::string pidfile;
    try {
        Radio radio(stop_requested);
        bool managed = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--synthetic") radio.synthetic = true;
            else if (key == "--managed") managed = true;
            else if (i + 1 < argc) {
                const std::string value = argv[++i];
                if (key == "--port") port = std::stoi(value);
                else if (key == "--firmware") radio.firmware_path = value;
                else if (key == "--pidfile") pidfile = value;
                else {
                    const auto number = std::stoull(value);
                    if (number > UINT32_MAX) throw std::invalid_argument("Numeric option exceeds 32-bit range");
                    if (key == "--frequency") radio.frequency = uint32_t(number);
                    else if (key == "--gain") radio.gain = uint32_t(number);
                    else if (key == "--max-rate") radio.maximum_rate = uint32_t(number);
                    else throw std::invalid_argument("Unknown option: " + key);
                }
            } else throw std::invalid_argument("Missing option value");
        }
        if (port < 1024 || port > 65535 || radio.frequency < min_freq || radio.frequency > max_freq || radio.gain > 49 ||
            radio.maximum_rate != 4000000)
            throw std::invalid_argument("Invalid port, frequency, gain or max-rate");
        watch_stdin(managed);
        if (!pidfile.empty()) {
            FILE* f = std::fopen(pidfile.c_str(), "w"); if (!f) throw std::runtime_error("Cannot create PID file");
            std::fprintf(f, "%ld\n", (long)getpid()); std::fclose(f);
        }
        if (!stop_requested()) radio.initialize();
        if (!stop_requested()) {
            server = socket(AF_INET, SOCK_STREAM, 0);
            if (server < 0) throw std::runtime_error("socket failed");
            int one = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(uint16_t(port)); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || listen(server, 1) != 0)
                throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
            std::printf("READY SpyServer: 127.0.0.1:%d, forced 16-bit IQ, max %.1f MS/s; version 1.0.0\n", port, radio.maximum_rate / 1e6);
            while (!stop_requested()) {
                pollfd p{server, POLLIN, 0};
                if (poll(&p, 1, 250) <= 0) continue;
                const int client = accept(server, nullptr, nullptr);
                if (client < 0) continue;
                try { session(client, radio); }
                catch (const std::exception& e) { std::fprintf(stderr, "CLIENT ERROR: %s\n", e.what()); radio.halt(); }
                shutdown(client, SHUT_RDWR); close(client);
            }
            close(server); server = -1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        if (server >= 0) close(server);
        if (!pidfile.empty()) unlink(pidfile.c_str());
        return 1;
    }
    if (!pidfile.empty()) unlink(pidfile.c_str());
    std::puts("STOPPED: receive thread joined and USB stream released");
    return 0;
}
