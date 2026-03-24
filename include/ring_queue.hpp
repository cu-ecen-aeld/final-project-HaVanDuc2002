/**
 * @file ring_queue.hpp
 * @brief Thread-safe bounded ring buffer for frame packets
 *
 * Backpressure policy: DROP_OLDEST — when queue is full, the oldest
 * frame is dropped to make room for the new one. The capture thread
 * never blocks.
 *
 * Zero-copy design: all frame buffers are pre-allocated in a single
 * mmap(MAP_ANONYMOUS) region. push() copies once (camera buffer →
 * mmap slot). pop() returns a FramePacket whose data pointer points
 * directly into the mmap slot — no extra copy. An atomic per-slot
 * in_use flag prevents the producer from overwriting a slot the
 * consumer is still reading.
 */

#ifndef RING_QUEUE_HPP
#define RING_QUEUE_HPP

#include <cstdint>
#include <atomic>
#include <pthread.h>

namespace streamer {

// Maximum frame payload size (adjust based on expected resolution)
constexpr size_t MAX_FRAME_SIZE = 4 * 1024 * 1024;  // 4MB for up to 4K frames

/**
 * Zero-copy frame view.
 *
 * data points directly into the pre-allocated mmap slot. The slot is
 * protected by an atomic in_use flag until ~FramePacket() is called,
 * at which point it is released back to the ring for reuse.
 *
 * Do NOT hold a FramePacket while calling pop() for the next frame
 * (though with capacity > 1 this is safe in practice).
 */
struct FramePacket {
    uint64_t    seq;           // Sequence number
    uint64_t    timestamp_ns;  // Capture timestamp (ns)
    uint32_t    pixel_format;  // Format identifier
    uint16_t    width;         // Frame width
    uint16_t    height;        // Frame height
    uint32_t    len;           // Payload length in bytes
    const uint8_t* data;       // Zero-copy view into mmap slot

    FramePacket() = default;
    ~FramePacket() {
        if (release_fn_) release_fn_(release_ctx_, slot_idx_);
    }

    // Non-copyable: copying would create double-release of the mmap slot
    FramePacket(const FramePacket&) = delete;
    FramePacket& operator=(const FramePacket&) = delete;

private:
    friend class RingQueue;
    void  (*release_fn_)(void* ctx, size_t slot_idx) = nullptr;
    void*   release_ctx_  = nullptr;
    size_t  slot_idx_     = 0;
};

// Raw-pointer owner — caller must call frame_packet_free() when done
typedef FramePacket* FramePacketPtr;

// Release a frame packet obtained from RingQueue::pop()
void frame_packet_free(FramePacketPtr pkt);

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
     * Check if the queue was successfully initialized
     */
    bool isValid() const { return mmap_base_ != nullptr; }

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
     * @param timeout_ms Milliseconds to wait; <0 = wait forever, 0 = no wait
     * @return Frame packet or nullptr on timeout/shutdown — free with frame_packet_free()
     */
    FramePacketPtr pop(int timeout_ms = -1);

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
    // mmap-backed pre-allocated slot storage
    void*  mmap_base_   = nullptr;
    size_t slot_stride_ = 0;
    size_t capacity_;
    size_t max_frame_size_;
    size_t head_  = 0;
    size_t tail_  = 0;
    size_t count_ = 0;

    // Per-slot in-use flag: true while consumer holds a FramePacket for that slot
    std::atomic<bool>* slot_in_use_ = nullptr;

    mutable pthread_mutex_t mutex_;
    pthread_cond_t not_empty_;

    std::atomic<bool> shutdown_{false};

    // Statistics
    std::atomic<uint64_t> frames_pushed_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> frames_popped_{0};

    // Called from ~FramePacket to mark the slot available for reuse
    static void releaseSlot(void* ctx, size_t slot_idx);
};

}  // namespace streamer

#endif  // RING_QUEUE_HPP
