/**
 * @file ring_queue.cpp
 * @brief Thread-safe bounded ring buffer implementation
 */

#include "ring_queue.hpp"
#include "log.hpp"

#include <cstring>
#include <cerrno>
#include <time.h>

namespace streamer {

RingQueue::RingQueue(size_t capacity, size_t max_frame_size)
    : slots_(capacity)
    , capacity_(capacity)
    , max_frame_size_(max_frame_size) {

    if (capacity == 0 || max_frame_size == 0) {
        LOG_ERROR << "Invalid queue parameters: capacity=" << capacity
                  << ", max_frame_size=" << max_frame_size;
        throw std::invalid_argument("Invalid queue parameters");
    }

    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&not_empty_, nullptr);

    LOG_INFO << "Ring queue created: capacity=" << capacity
             << ", max_frame_size=" << max_frame_size;
}

RingQueue::~RingQueue() {
    // Clear all slots (unique_ptrs will auto-delete)
    pthread_mutex_lock(&mutex_);
    for (auto& slot : slots_) {
        slot.reset();
    }
    pthread_mutex_unlock(&mutex_);

    pthread_cond_destroy(&not_empty_);
    pthread_mutex_destroy(&mutex_);
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

    pthread_mutex_lock(&mutex_);

    if (shutdown_.load()) {
        pthread_mutex_unlock(&mutex_);
        return false;
    }

    // When full, drop the oldest frame to make room
    while (count_ >= capacity_) {
        if (slots_[tail_]) {
            std::memset(slots_[tail_]->data.data(), 0, slots_[tail_]->data.size());
            slots_[tail_].reset();
        }
        tail_ = (tail_ + 1) % capacity_;
        count_--;
        frames_dropped_++;
        LOG_DEBUG << "Dropped oldest frame due to backpressure";
    }

    // Create new frame packet
    auto pkt = std::make_unique<FramePacket>(seq, timestamp_ns, pixel_format,
                                              width, height, data, len);

    // Insert at head
    slots_[head_] = std::move(pkt);
    head_ = (head_ + 1) % capacity_;
    count_++;
    frames_pushed_++;

    pthread_cond_signal(&not_empty_);
    pthread_mutex_unlock(&mutex_);
    return true;
}

FramePacketPtr RingQueue::pop(std::chrono::milliseconds timeout) {
    pthread_mutex_lock(&mutex_);

    if (timeout.count() < 0) {
        // Infinite wait — loop to handle spurious wakeups
        while (count_ == 0 && !shutdown_.load()) {
            pthread_cond_wait(&not_empty_, &mutex_);
        }
    } else if (timeout.count() == 0) {
        // No wait
        if (count_ == 0 || shutdown_.load()) {
            pthread_mutex_unlock(&mutex_);
            return nullptr;
        }
    } else {
        // Timed wait — build absolute deadline from CLOCK_REALTIME
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long ms = timeout.count();
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += (ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        // Loop to handle spurious wakeups
        while (count_ == 0 && !shutdown_.load()) {
            int ret = pthread_cond_timedwait(&not_empty_, &mutex_, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&mutex_);
                return nullptr;
            }
        }
    }

    if (count_ == 0) {
        // Shutdown with empty queue
        pthread_mutex_unlock(&mutex_);
        return nullptr;
    }

    // Remove from tail
    FramePacketPtr pkt = std::move(slots_[tail_]);
    slots_[tail_].reset();
    tail_ = (tail_ + 1) % capacity_;
    count_--;
    frames_popped_++;

    pthread_mutex_unlock(&mutex_);
    return pkt;
}

void RingQueue::shutdown() {
    pthread_mutex_lock(&mutex_);
    shutdown_.store(true);
    pthread_mutex_unlock(&mutex_);

    pthread_cond_broadcast(&not_empty_);
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
