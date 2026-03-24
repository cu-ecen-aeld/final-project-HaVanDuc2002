/**
 * @file ring_queue.hpp
 * @brief Thread-safe bounded ring buffer for frame packets
 *
 * Backpressure policy: DROP_OLDEST - when queue is full, the oldest
 * frame is dropped to make room for the new one. This ensures the
 * capture thread never blocks, which is critical for real-time capture.
 */

#ifndef RING_QUEUE_HPP
#define RING_QUEUE_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include <chrono>
#include <atomic>
#include <pthread.h>

namespace streamer {

// Maximum frame payload size (adjust based on expected resolution)
constexpr size_t MAX_FRAME_SIZE = 4 * 1024 * 1024;  // 4MB for up to 4K frames

// Frame packet structure
struct FramePacket {
    uint64_t seq;           // Sequence number
    uint64_t timestamp_ns;  // Capture timestamp in nanoseconds
    uint32_t pixel_format;  // Format identifier
    uint16_t width;         // Frame width
    uint16_t height;        // Frame height
    std::vector<uint8_t> data;  // Frame data

    FramePacket() = default;

    FramePacket(uint64_t seq_, uint64_t ts, uint32_t fmt,
                uint16_t w, uint16_t h, const uint8_t* ptr, size_t len)
        : seq(seq_), timestamp_ns(ts), pixel_format(fmt),
          width(w), height(h), data(ptr, ptr + len) {}
};

using FramePacketPtr = std::unique_ptr<FramePacket>;

/**
 * Thread-safe ring buffer queue for frame packets
 */
class RingQueue {
public:
    /**
     * Create a new ring queue
     * @param capacity Number of frame slots
     * @param max_frame_size Maximum size of a single frame
     */
    RingQueue(size_t capacity, size_t max_frame_size);

    ~RingQueue();

    // Disable copy
    RingQueue(const RingQueue&) = delete;
    RingQueue& operator=(const RingQueue&) = delete;

    /**
     * Push a frame into the queue
     * @param seq Frame sequence number
     * @param timestamp_ns Capture timestamp
     * @param data Frame data
     * @param len Frame data length
     * @param pixel_format Format identifier
     * @param width Frame width
     * @param height Frame height
     * @return true on success, false if frame was dropped or error
     */
    bool push(uint64_t seq, uint64_t timestamp_ns,
              const uint8_t* data, uint32_t len,
              uint32_t pixel_format, uint16_t width, uint16_t height);

    /**
     * Pop a frame from the queue (blocking)
     * @param timeout Timeout duration (negative = infinite wait)
     * @return Frame packet or nullptr on timeout/shutdown
     */
    FramePacketPtr pop(std::chrono::milliseconds timeout = std::chrono::milliseconds(-1));

    /**
     * Signal shutdown to unblock waiting consumers
     */
    void shutdown();

    /**
     * Get queue statistics
     */
    struct Stats {
        uint64_t frames_pushed;
        uint64_t frames_dropped;
        uint64_t frames_popped;
    };
    Stats getStats() const;

    /**
     * Check if queue is shut down
     */
    bool isShutdown() const { return shutdown_.load(); }

private:
    std::vector<FramePacketPtr> slots_;
    size_t capacity_;
    size_t max_frame_size_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;

    mutable pthread_mutex_t mutex_;
    pthread_cond_t not_empty_;

    std::atomic<bool> shutdown_{false};

    // Statistics
    std::atomic<uint64_t> frames_pushed_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> frames_popped_{0};
};

}  // namespace streamer

#endif  // RING_QUEUE_HPP
