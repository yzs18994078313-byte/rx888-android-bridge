#include "sample_queue.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <stdexcept>

static void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static void sequential() {
    rx888::SampleQueue queue(4, 16);
    queue.reset(2);
    check(!queue.front(), "new queue must be empty");
    for (int id = 1; id <= 2; ++id) { auto* b = queue.writable(); check(b, "queue filled too early"); b->data[0] = {int16_t(id), int16_t(-id)}; queue.publish(1); }
    check(!queue.writable(), "full queue allowed overwrite");
    const auto* held = queue.front();
    check(held && held->data[0].real() == 1 && held->count == 1, "FIFO order incorrect");
    for (int i = 0; i < 10000; ++i) check(!queue.writable() && held->data[0].real() == 1, "producer overwrote a held TCP block");
    queue.release();
    check(queue.front()->data[0].real() == 2, "second block lost");
    queue.release();
    for (int i = 0; i < 10000; ++i) {
        auto* b = queue.writable(); check(b, "ring did not wrap"); b->data[0] = {int16_t(i), 0}; queue.publish(1);
        check(queue.front()->data[0].real() == int16_t(i), "ring wrap corrupted IQ"); queue.release();
    }
    auto* b = queue.writable(); b->data[0] = {7, 0}; queue.publish(5);
    check(queue.discard_ready() == 5 && !queue.front(), "retune flush incorrect");
}
static void concurrent() {
    rx888::SampleQueue queue(16, 32);
    std::atomic<bool> done{false}, failed{false};
    uint64_t accepted = 0, dropped = 0, consumed = 0;
    constexpr uint64_t attempts = 200000;
    std::thread producer([&] {
        for (uint64_t id = 1; id <= attempts; ++id) {
            auto* block = queue.writable();
            if (!block) ++dropped;
            else {
                for (size_t j = 0; j < block->data.size(); ++j) block->data[j] = {int16_t(id + j), int16_t(~(id + j))};
                std::memcpy(block->data.data(), &id, sizeof id);
                queue.publish(block->data.size()); ++accepted;
            }
            if ((id & 127) == 0) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // deliberately apply downstream backpressure
        uint64_t previous = 0;
        while (!done.load(std::memory_order_acquire) || queue.front()) {
            const auto* block = queue.front();
            if (!block) { std::this_thread::yield(); continue; }
            uint64_t id = 0; std::memcpy(&id, block->data.data(), sizeof id);
            if (id <= previous || block->count != 32) failed.store(true);
            for (size_t j = 2; j < block->count; ++j)
                if (block->data[j] != rx888::Sample(int16_t(id + j), int16_t(~(id + j)))) failed.store(true);
            previous = id; ++consumed;
            if ((consumed & 511) == 0) std::this_thread::sleep_for(std::chrono::microseconds(20));
            queue.release();
        }
    });
    producer.join(); consumer.join();
    check(!failed.load(), "concurrent producer/consumer corrupted IQ or ordering");
    check(accepted + dropped == attempts && consumed == accepted, "drop accounting or block count incorrect");
    check(dropped > 0, "test did not exercise queue backpressure");
    std::printf("Concurrent: accepted=%llu dropped=%llu consumed=%llu\n", (unsigned long long)accepted, (unsigned long long)dropped, (unsigned long long)consumed);
}
int main() {
    try { sequential(); concurrent(); std::puts("PASS: held-block safety, full queue, FIFO, wrap, retune flush, concurrent IQ integrity, drop accounting"); }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return 0;
}
