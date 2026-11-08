// Bounded single-producer/single-consumer IQ queue. GPL-3.0-or-later.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rx888 {
using Sample = std::complex<int16_t>;
static_assert(sizeof(Sample) == 4, "The SpyServer IQ wire format requires 4 bytes per sample");
class SampleQueue {
public:
    using Clock = std::chrono::steady_clock;
    struct Block {
        std::vector<Sample> data;
        size_t count = 0;
        Clock::time_point queued{};
    };
    explicit SampleQueue(size_t maximum_blocks = 256, size_t maximum_samples = 16384)
        : blocks_(maximum_blocks), capacity_(maximum_blocks) {
        if (!maximum_blocks || !maximum_samples) throw std::invalid_argument("Empty IQ queue");
        for (auto& b : blocks_) b.data.resize(maximum_samples);
    }
    // Only call reset after the producer has joined and the consumer released its block.
    void reset(size_t capacity) {
        if (!capacity || capacity > blocks_.size()) throw std::invalid_argument("Invalid IQ queue capacity");
        capacity_ = capacity;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
    Block* writable() {
        const auto h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= capacity_) return nullptr;
        return &blocks_[h % capacity_];
    }
    void publish(size_t samples) {
        const auto h = head_.load(std::memory_order_relaxed);
        auto& b = blocks_[h % capacity_];
        b.count = samples;
        b.queued = Clock::now();
        head_.store(h + 1, std::memory_order_release);
    }
    const Block* front() const {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return nullptr;
        return &blocks_[t % capacity_];
    }
    void release() {
        tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    size_t depth() const {
        const auto h = head_.load(std::memory_order_acquire);
        const auto t = tail_.load(std::memory_order_acquire);
        return h >= t ? std::min<uint64_t>(h - t, capacity_) : 0;
    }
    size_t capacity() const { return capacity_; }
    // Consumer only; the producer cannot overwrite a block until tail is released.
    uint64_t discard_ready() {
        const auto t = tail_.load(std::memory_order_relaxed);
        const auto h = head_.load(std::memory_order_acquire);
        uint64_t samples = 0;
        for (auto i = t; i < h; ++i) samples += blocks_[i % capacity_].count;
        tail_.store(h, std::memory_order_release);
        return samples;
    }
private:
    std::vector<Block> blocks_;
    size_t capacity_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};
}
