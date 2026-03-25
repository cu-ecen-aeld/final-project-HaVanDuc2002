/**
 * @file ring_queue.cpp
 * @brief Thread-safe bounded ring buffer — mmap-backed, zero-copy pop
 */

#include "ring_queue.hpp"
#include "log.hpp"

#include <cstring>
#include <cerrno>
#include <new>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>

namespace streamer {

// ---------------------------------------------------------------------------
// Slot layout in the mmap region:
//
//   slot_i starts at: mmap_base + i * slot_stride
//   [SlotHeader (28 bytes)] [frame data (max_frame_size bytes)]
//   stride is rounded up to the next 64-byte (cache-line) boundary
// ---------------------------------------------------------------------------

struct SlotHeader {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t pixel_format;
    uint16_t width;
    uint16_t height;
    uint32_t len;           // actual payload bytes stored
} __attribute__((packed));

static inline size_t calcStride(size_t max_frame_size) {
    size_t sz = sizeof(SlotHeader) + max_frame_size;
    return (sz + 63u) & ~static_cast<size_t>(63u);  // round up to 64 bytes
}

static inline SlotHeader* slotHdr(void* base, size_t stride, size_t idx) {
    return reinterpret_cast<SlotHeader*>(static_cast<uint8_t*>(base) + idx * stride);
}

static inline uint8_t* slotData(void* base, size_t stride, size_t idx) {
    return static_cast<uint8_t*>(base) + idx * stride + sizeof(SlotHeader);
}

// ---------------------------------------------------------------------------

RingQueue::RingQueue(size_t capacity, size_t max_frame_size)
    : capacity_(capacity)
    , max_frame_size_(max_frame_size) {

    if (capacity == 0 || max_frame_size == 0) {
        LOG_ERROR << "Invalid queue parameters: capacity=" << capacity
                  << ", max_frame_size=" << max_frame_size;
        return;  // mmap_base_ stays nullptr; caller checks isValid()
    }

    slot_stride_ = calcStride(max_frame_size);
    size_t total = slot_stride_ * capacity;

    mmap_base_ = mmap(nullptr, total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mmap_base_ == MAP_FAILED) {
        mmap_base_ = nullptr;
        LOG_ERROR << "mmap failed for ring queue";
        return;  // caller checks isValid()
    }

    slot_in_use_ = static_cast<std::atomic<bool>*>(
        malloc(capacity * sizeof(std::atomic<bool>)));
    if (!slot_in_use_) {
        LOG_ERROR << "malloc failed for slot_in_use_";
        munmap(mmap_base_, total);
        mmap_base_ = nullptr;
        return;
    }
    for (size_t i = 0; i < capacity; i++) {
        new (&slot_in_use_[i]) std::atomic<bool>(false);
    }

    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&not_empty_, nullptr);

    LOG_INFO << "Ring queue created: capacity=" << capacity
             << ", max_frame_size=" << max_frame_size
             << ", total_mmap=" << total << " bytes";
}

RingQueue::~RingQueue() {
    if (!mmap_base_) return;

    pthread_cond_destroy(&not_empty_);
    pthread_mutex_destroy(&mutex_);

    if (slot_in_use_) {
        for (size_t i = 0; i < capacity_; i++) {
            slot_in_use_[i].~atomic();
        }
        free(slot_in_use_);
        slot_in_use_ = nullptr;
    }

    munmap(mmap_base_, slot_stride_ * capacity_);
    mmap_base_ = nullptr;
    LOG_DEBUG << "Ring queue destroyed";
}

bool RingQueue::push(uint64_t seq, uint64_t timestamp_ns,
                     const uint8_t* data, uint32_t len,
                     uint32_t pixel_format, uint16_t width, uint16_t height) {
    if (!mmap_base_ || !data || len == 0) return false;

    if (len > max_frame_size_) {
        LOG_WARN << "Frame too large: " << len << " > " << max_frame_size_ << " (max), dropping";
        return false;
    }

    pthread_mutex_lock(&mutex_);

    if (shutdown_.load()) {
        pthread_mutex_unlock(&mutex_);
        return false;
    }

    // DropOldest: advance tail past slots not held by the consumer
    while (count_ >= capacity_) {
        if (slot_in_use_[tail_].load()) {
            // Oldest queued slot is held by consumer — protect it, drop new frame
            pthread_mutex_unlock(&mutex_);
            frames_dropped_++;
            return false;
        }
        tail_ = (tail_ + 1) % capacity_;
        count_--;
        frames_dropped_++;
        LOG_DEBUG << "Dropped oldest frame due to backpressure";
    }

    // Guard against head wrapping onto a slot still held by the consumer
    if (slot_in_use_[head_].load()) {
        pthread_mutex_unlock(&mutex_);
        frames_dropped_++;
        return false;
    }

    // Write metadata + frame data into the mmap slot at head_
    SlotHeader* hdr  = slotHdr(mmap_base_, slot_stride_, head_);
    hdr->seq          = seq;
    hdr->timestamp_ns = timestamp_ns;
    hdr->pixel_format = pixel_format;
    hdr->width        = width;
    hdr->height       = height;
    hdr->len          = len;
    std::memcpy(slotData(mmap_base_, slot_stride_, head_), data, len);

    head_ = (head_ + 1) % capacity_;
    count_++;
    frames_pushed_++;

    pthread_cond_signal(&not_empty_);
    pthread_mutex_unlock(&mutex_);
    return true;
}

FramePacketPtr RingQueue::pop(int timeout_ms) {
    if (!mmap_base_) return nullptr;

    pthread_mutex_lock(&mutex_);

    if (timeout_ms < 0) {
        // Infinite wait
        while (count_ == 0 && !shutdown_.load()) {
            pthread_cond_wait(&not_empty_, &mutex_);
        }
    } else if (timeout_ms == 0) {
        if (count_ == 0 || shutdown_.load()) {
            pthread_mutex_unlock(&mutex_);
            return nullptr;
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (count_ == 0 && !shutdown_.load()) {
            if (pthread_cond_timedwait(&not_empty_, &mutex_, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&mutex_);
                return nullptr;
            }
        }
    }

    if (count_ == 0) {
        pthread_mutex_unlock(&mutex_);
        return nullptr;
    }

    // Grab tail slot metadata while holding the mutex
    size_t idx = tail_;
    SlotHeader* hdr = slotHdr(mmap_base_, slot_stride_, idx);
    uint64_t seq          = hdr->seq;
    uint64_t timestamp_ns = hdr->timestamp_ns;
    uint32_t pixel_format = hdr->pixel_format;
    uint16_t width        = hdr->width;
    uint16_t height       = hdr->height;
    uint32_t len          = hdr->len;

    // Mark slot as held BEFORE releasing the mutex so push() can't overwrite it
    slot_in_use_[idx].store(true);
    tail_ = (tail_ + 1) % capacity_;
    count_--;
    frames_popped_++;

    pthread_mutex_unlock(&mutex_);

    // Build zero-copy FramePacket — data pointer into mmap slot, no copy
    FramePacket* pkt = static_cast<FramePacket*>(malloc(sizeof(FramePacket)));
    if (!pkt) {
        // malloc failure: release slot and return null
        slot_in_use_[idx].store(false);
        return nullptr;
    }
    pkt->seq          = seq;
    pkt->timestamp_ns = timestamp_ns;
    pkt->pixel_format = pixel_format;
    pkt->width        = width;
    pkt->height       = height;
    pkt->len          = len;
    pkt->data         = slotData(mmap_base_, slot_stride_, idx);
    pkt->release_fn_  = &RingQueue::releaseSlot;
    pkt->release_ctx_ = this;
    pkt->slot_idx_    = idx;

    return pkt;
}

// Called from ~FramePacket — clears the in_use flag so push() can reuse the slot
void RingQueue::releaseSlot(void* ctx, size_t idx) {
    auto* self = static_cast<RingQueue*>(ctx);
    self->slot_in_use_[idx].store(false);
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

void frame_packet_free(FramePacketPtr pkt) {
    if (!pkt) return;
    pkt->~FramePacket();  // triggers releaseSlot via release_fn_
    free(pkt);
}

}  // namespace streamer
