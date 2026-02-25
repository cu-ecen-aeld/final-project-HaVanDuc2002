/**
 * @file tls_client.hpp
 * @brief TLS client for frame streaming with reconnect support
 */

#ifndef TLS_CLIENT_HPP
#define TLS_CLIENT_HPP

#include <cstdint>
#include <string>
#include <atomic>
#include <memory>

#include <openssl/ssl.h>

namespace streamer {

// Forward declaration
class RingQueue;

// TLS client configuration
struct TlsConfig {
    std::string host;            // Remote hostname
    uint16_t port = 4433;        // Remote port
    std::string ca_path;         // Path to CA bundle (empty = default)

    // Reconnect settings
    uint32_t reconnect_base_ms = 1000;    // Base delay
    uint32_t reconnect_max_ms = 30000;    // Max delay
    float reconnect_multiplier = 2.0f;    // Backoff multiplier

    // Frame info for protocol
    uint16_t frame_width = 0;
    uint16_t frame_height = 0;
    uint32_t pixel_format = 0;
};

// Connection state
enum class TlsState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

/**
 * TLS client for streaming frames
 */
class TlsClient {
public:
    /**
     * Initialize TLS client
     * @param config Client configuration
     */
    explicit TlsClient(const TlsConfig& config);

    ~TlsClient();

    // Disable copy
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    /**
     * Initialize SSL context
     * @return true on success
     */
    bool initialize();

    /**
     * Network thread entry point
     * Sends frames from queue until running flag is false
     * @param queue Source queue for frames
     * @param running Control flag
     */
    void networkLoop(RingQueue& queue, const std::atomic<bool>& running);

    /**
     * Get current connection state
     */
    TlsState state() const { return state_.load(); }

    /**
     * Get frames sent count
     */
    uint64_t framesSent() const { return frames_sent_.load(); }

    /**
     * Get bytes sent count
     */
    uint64_t bytesSent() const { return bytes_sent_.load(); }

private:
    TlsConfig config_;

    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int sockfd_ = -1;

    std::atomic<TlsState> state_{TlsState::Disconnected};
    uint32_t reconnect_delay_ms_ = 0;

    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> bytes_sent_{0};

    // SSL context creation
    bool createSslContext();

    // TCP connection
    int createTcpConnection(const std::string& host, uint16_t port);

    // TLS connection
    bool tlsConnect();

    // TLS disconnect
    void tlsDisconnect();

    // Write all data over TLS
    bool sslWriteAll(const void* data, size_t len);

    // Sleep for milliseconds
    static void sleepMs(uint32_t ms);

    // Log OpenSSL errors
    static void logSslErrors(const char* context);
};

}  // namespace streamer

#endif  // TLS_CLIENT_HPP
