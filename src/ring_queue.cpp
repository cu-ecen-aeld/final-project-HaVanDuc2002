/**
 * @file ring_queue.cpp
 * @brief Thread-safe bounded ring buffer implementation
 */

#include "ring_queue.hpp"
#include "log.hpp"

#include <cstring>

namespace streamer {

RingQueue::RingQueue(size_t capacity, size_t max_frame_size, BackpressurePolicy policy)
    : slots_(capacity)
    , capacity_(capacity)
    , max_frame_size_(max_frame_size)
    , policy_(policy) {

    if (capacity == 0 || max_frame_size == 0) {
        LOG_ERROR << "Invalid queue parameters: capacity=" << capacity
                  << ", max_frame_size=" << max_frame_size;
        throw std::invalid_argument("Invalid queue parameters");
    }

    LOG_INFO << "Ring queue created: capacity=" << capacity
             << ", max_frame_size=" << max_frame_size
             << ", policy=" << static_cast<int>(policy);
}

RingQueue::~RingQueue() {
    // Clear all slots (unique_ptrs will auto-delete)
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        slot.reset();
    }
    LOG_DEBUG << "Ring queue destroyed";
}

bool RingQueue::push(uint64_t seq, uint64_t timestamp_ns,
                     const uint8_t* data, uint32_t len,
                     uint32_t pixel_format, uint16_t width, uint16_t height) {
    if (!data || len == 0) {
        return false;
    }

    if (len > max_frame_size_) {
        LOG_WARN << "Frame too large: " << len << " > " << max_frame_size_ << " (max), dropping";
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);

    if (shutdown_.load()) {
        return false;
    }

    // Handle full queue based on policy
    while (count_ >= capacity_) {
        switch (policy_) {
            case BackpressurePolicy::DropOldest:
                // Drop the oldest frame (at tail)
                if (slots_[tail_]) {
                    // Clear data for security
                    std::memset(slots_[tail_]->data.data(), 0, slots_[tail_]->data.size());
                    slots_[tail_].reset();
                }
                tail_ = (tail_ + 1) % capacity_;
                count_--;
                frames_dropped_++;
                LOG_DEBUG << "Dropped oldest frame due to backpressure";
                break;

            case BackpressurePolicy::DropNewest:
                // Drop this new frame
                frames_dropped_++;
                LOG_DEBUG << "Dropped newest frame due to backpressure";
                return false;

            case BackpressurePolicy::Block:
                // Wait until space is available
                not_full_.wait(lock);
                if (shutdown_.load()) {
                    return false;
                }
                break;
        }
    }

    // Create new frame packet
    auto pkt = std::make_unique<FramePacket>(seq, timestamp_ns, pixel_format,
                                              width, height, data, len);

    // Insert at head
    slots_[head_] = std::move(pkt);
    head_ = (head_ + 1) % capacity_;
    count_++;
    frames_pushed_++;

    not_empty_.notify_one();
    return true;
}

FramePacketPtr RingQueue::pop(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for data or shutdown
    auto wait_pred = [this] { return count_ > 0 || shutdown_.load(); };

    if (timeout.count() < 0) {
        // Infinite wait
        not_empty_.wait(lock, wait_pred);
    } else if (timeout.count() == 0) {
        // No wait
        if (!wait_pred()) {
            return nullptr;
        }
    } else {
        // Timed wait
        if (!not_empty_.wait_for(lock, timeout, wait_pred)) {
            return nullptr;  // Timeout
        }
    }

    if (count_ == 0) {
        // Shutdown with empty queue
        return nullptr;
    }

    // Remove from tail
    FramePacketPtr pkt = std::move(slots_[tail_]);
    slots_[tail_].reset();
    tail_ = (tail_ + 1) % capacity_;
    count_--;
    frames_popped_++;

    not_full_.notify_one();
    return pkt;
}

void RingQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_.store(true);
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    LOG_INFO << "Ring queue shutdown signaled";
}

RingQueue::Stats RingQueue::getStats() const {
    return Stats{
        frames_pushed_.load(),
        frames_dropped_.load(),
        frames_popped_.load()
    };
}

}  // namespace streamer
