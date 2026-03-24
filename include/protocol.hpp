/**
 * @file protocol.hpp
 * @brief Frame streaming protocol definitions
 *
 * Protocol: For each frame, send a fixed header followed by payload.
 * All multi-byte integers are in network byte order (big-endian).
 *
 * Header format:
 *   magic[4]        - "FRAM"
 *   version         - uint16_t (currently 1)
 *   header_len      - uint16_t (size of header including magic)
 *   seq             - uint64_t (frame sequence number)
 *   timestamp_ns    - uint64_t (capture timestamp in nanoseconds)
 *   payload_len     - uint32_t (size of frame data)
 *   pixel_format    - uint32_t (format identifier)
 *   width           - uint16_t
 *   height          - uint16_t
 *   [payload bytes follow]
 */

#ifndef PROTOCOL_HPP
#define PROTOCOL_HPP

#include <cstdint>
#include <cstring>
#include <arpa/inet.h>

namespace streamer {

constexpr char PROTO_MAGIC[4] = {'F', 'R', 'A', 'M'};
constexpr size_t PROTO_MAGIC_LEN = 4;
constexpr uint16_t PROTO_VERSION = 1;

// Pixel format identifiers (matching common OpenCV/V4L2 formats)
enum class PixelFormat : uint32_t {
    JPEG = 0x47504A4D,   // "MJPG"
    BGR24 = 0x33524742,  // "BGR3"
    RGB24 = 0x33424752,  // "RGB3"
    YUYV = 0x56595559,   // "YUYV"
    NV12 = 0x3231564E,   // "NV12"
    GRAY = 0x59415247,   // "GRAY"
};

// Frame header structure (packed for wire format)
#pragma pack(push, 1)
struct FrameHeader {
    char magic[PROTO_MAGIC_LEN];   // "FRAM"
    uint16_t version;              // Protocol version
    uint16_t header_len;           // Total header length
    uint64_t seq;                  // Frame sequence number
    uint64_t timestamp_ns;         // Capture timestamp (ns)
    uint32_t payload_len;          // Payload size in bytes
    uint32_t pixel_format;         // Pixel format identifier
    uint16_t width;                // Frame width
    uint16_t height;               // Frame height
};
#pragma pack(pop)

constexpr size_t FRAME_HEADER_SIZE = sizeof(FrameHeader);

// Helper for 64-bit network byte order conversion
inline uint64_t htonll(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<uint64_t>(htonl(val & 0xFFFFFFFF)) << 32) |
           htonl(val >> 32);
#else
    return val;
#endif
}

inline uint64_t ntohll(uint64_t val) {
    return htonll(val);  // Same operation
}

/**
 * Initialize a frame header with network byte order values
 */
inline void initFrameHeader(FrameHeader& hdr,
                            uint64_t seq,
                            uint64_t timestamp_ns,
                            uint32_t payload_len,
                            uint32_t pixel_format,
                            uint16_t width,
                            uint16_t height) {
    std::memcpy(hdr.magic, PROTO_MAGIC, PROTO_MAGIC_LEN);
    hdr.version = htons(PROTO_VERSION);
    hdr.header_len = htons(static_cast<uint16_t>(FRAME_HEADER_SIZE));
    hdr.seq = htonll(seq);
    hdr.timestamp_ns = htonll(timestamp_ns);
    hdr.payload_len = htonl(payload_len);
    hdr.pixel_format = htonl(pixel_format);
    hdr.width = htons(width);
    hdr.height = htons(height);
}

/**
 * Convert pixel format fourcc to a null-terminated string.
 * buf must be at least 5 bytes.
 */
inline void pixelFormatToString(uint32_t format, char buf[5]) {
    buf[0] = static_cast<char>((format >> 0)  & 0xFF);
    buf[1] = static_cast<char>((format >> 8)  & 0xFF);
    buf[2] = static_cast<char>((format >> 16) & 0xFF);
    buf[3] = static_cast<char>((format >> 24) & 0xFF);
    buf[4] = '\0';
}

}  // namespace streamer

#endif  // PROTOCOL_HPP
