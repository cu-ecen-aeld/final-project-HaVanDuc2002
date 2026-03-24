/**
 * @file tls_client.cpp
 * @brief TLS client implementation with reconnect and exponential backoff
 */

#include "tls_client.hpp"
#include "ring_queue.hpp"
#include "protocol.hpp"
#include "log.hpp"

#include <cstring>
#include <unistd.h>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <errno.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace streamer {

// Queue pop timeout
constexpr int QUEUE_TIMEOUT_MS = 1000;

TlsClient::TlsClient(const TlsConfig& config)
    : config_(config) {
    reconnect_delay_ms_ = config.reconnect_base_ms;
}

TlsClient::~TlsClient() {
    tlsDisconnect();
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    LOG_DEBUG << "TLS client resources cleaned up";
}

void TlsClient::logSslErrors(const char* context) {
    unsigned long err;
    char buf[256];
    while ((err = ERR_get_error()) != 0) {
        ERR_error_string_n(err, buf, sizeof(buf));
        LOG_ERROR << context << ": " << buf;
    }
}

bool TlsClient::initialize() {
    if (config_.host.empty() || config_.port == 0) {
        LOG_ERROR << "Invalid TLS client configuration";
        return false;
    }

    // OpenSSL 1.1+ auto-initializes
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    if (!createSslContext()) {
        return false;
    }

    LOG_INFO << "TLS client initialized for " << config_.host << ":" << config_.port;
    return true;
}

bool TlsClient::createSslContext() {
    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) {
        logSslErrors("SSL_CTX_new");
        return false;
    }

    // Set minimum TLS version to 1.2
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);

    // Load CA certificates
    if (!config_.ca_path.empty()) {
        if (!SSL_CTX_load_verify_locations(ssl_ctx_, config_.ca_path.c_str(), nullptr)) {
            logSslErrors("SSL_CTX_load_verify_locations");
            LOG_ERROR << "Failed to load CA bundle: " << config_.ca_path;
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
            return false;
        }
        LOG_INFO << "Loaded CA bundle: " << config_.ca_path;
    } else {
        // Use default CA paths
        if (!SSL_CTX_set_default_verify_paths(ssl_ctx_)) {
            logSslErrors("SSL_CTX_set_default_verify_paths");
            LOG_WARN << "Failed to load default CA paths";
        }
    }

    // Enable certificate verification
    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_verify_depth(ssl_ctx_, 4);

    // Set secure cipher list
    if (!SSL_CTX_set_cipher_list(ssl_ctx_,
            "ECDHE+AESGCM:DHE+AESGCM:ECDHE+CHACHA20:DHE+CHACHA20")) {
        logSslErrors("SSL_CTX_set_cipher_list");
        LOG_WARN << "Failed to set preferred ciphers";
    }

    return true;
}

int TlsClient::createTcpConnection(const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *res, *rp;
    int sockfd = -1;
    std::string port_str = std::to_string(port);

    hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int ret = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (ret != 0) {
        LOG_ERROR << "getaddrinfo(" << host << ":" << port << "): " << gai_strerror(ret);
        return -1;
    }

    // Try each address
    for (rp = res; rp != nullptr; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;

        // Set TCP_NODELAY for lower latency
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  // Success
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd < 0) {
        LOG_ERROR << "Failed to connect to " << host << ":" << port;
    }

    return sockfd;
}

bool TlsClient::tlsConnect() {
    // Create TCP connection
    sockfd_ = createTcpConnection(config_.host, config_.port);
    if (sockfd_ < 0) {
        return false;
    }

    LOG_DEBUG << "TCP connected to " << config_.host << ":" << config_.port;

    // Create SSL object
    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_) {
        logSslErrors("SSL_new");
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Set SNI hostname
    if (!SSL_set_tlsext_host_name(ssl_, config_.host.c_str())) {
        logSslErrors("SSL_set_tlsext_host_name");
    }

    // Set hostname verification
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (!X509_VERIFY_PARAM_set1_host(param, config_.host.c_str(), 0)) {
        logSslErrors("X509_VERIFY_PARAM_set1_host");
        SSL_free(ssl_);
        ssl_ = nullptr;
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Attach SSL to socket
    if (!SSL_set_fd(ssl_, sockfd_)) {
        logSslErrors("SSL_set_fd");
        SSL_free(ssl_);
        ssl_ = nullptr;
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Perform TLS handshake
    int ret = SSL_connect(ssl_);
    if (ret != 1) {
        int err = SSL_get_error(ssl_, ret);
        LOG_ERROR << "SSL_connect failed: " << err;
        logSslErrors("SSL_connect");

        // Check verification result
        long verify_result = SSL_get_verify_result(ssl_);
        if (verify_result != X509_V_OK) {
            LOG_ERROR << "Certificate verification failed: "
                      << X509_verify_cert_error_string(verify_result);
        }

        SSL_free(ssl_);
        ssl_ = nullptr;
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Log connection info
    const char* version = SSL_get_version(ssl_);
    const char* cipher = SSL_get_cipher_name(ssl_);
    LOG_INFO << "TLS connected: " << version << " " << cipher;

    // Log peer certificate info
    X509* cert = SSL_get_peer_certificate(ssl_);
    if (cert) {
        char subject[256];
        X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
        LOG_DEBUG << "Server certificate: " << subject;
        X509_free(cert);
    }

    state_.store(TlsState::Connected);
    reconnect_delay_ms_ = config_.reconnect_base_ms;

    return true;
}

void TlsClient::tlsDisconnect() {
    if (ssl_) {
        // Attempt clean shutdown, don't wait for peer
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }

    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }

    state_.store(TlsState::Disconnected);
}

bool TlsClient::sslWriteAll(const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = len;

    while (remaining > 0) {
        int written = SSL_write(ssl_, ptr, static_cast<int>(remaining));

        if (written <= 0) {
            int err = SSL_get_error(ssl_, written);

            switch (err) {
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ:
                    // Retry (would block)
                    continue;

                case SSL_ERROR_SYSCALL:
                    if (errno == EINTR) continue;
                    LOG_ERRNO(errno) << "SSL_write syscall error";
                    return false;

                case SSL_ERROR_ZERO_RETURN:
                    LOG_ERROR << "SSL connection closed by peer";
                    return false;

                default:
                    logSslErrors("SSL_write");
                    return false;
            }
        }

        ptr += written;
        remaining -= written;
    }

    return true;
}

void TlsClient::sleepMs(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

void TlsClient::networkLoop(RingQueue& queue, const std::atomic<bool>& running) {
    LOG_INFO << "Network thread starting";

    while (running.load()) {
        // Ensure connected
        if (state_.load() != TlsState::Connected) {
            state_.store(TlsState::Connecting);
            LOG_INFO << "Connecting to " << config_.host << ":" << config_.port << "...";

            if (!tlsConnect()) {
                LOG_WARN << "Connection failed, retrying in " << reconnect_delay_ms_ << " ms";

                // Exponential backoff
                sleepMs(reconnect_delay_ms_);
                reconnect_delay_ms_ = static_cast<uint32_t>(
                    reconnect_delay_ms_ * config_.reconnect_multiplier);
                if (reconnect_delay_ms_ > config_.reconnect_max_ms) {
                    reconnect_delay_ms_ = config_.reconnect_max_ms;
                }
                continue;
            }

            LOG_INFO << "Connected to " << config_.host << ":" << config_.port;
        }

        // Get frame from queue
        FramePacketPtr pkt = queue.pop(QUEUE_TIMEOUT_MS);
        if (!pkt) {
            // Timeout or shutdown
            continue;
        }

        // Build frame header
        FrameHeader hdr;
        initFrameHeader(hdr,
                        pkt->seq,
                        pkt->timestamp_ns,
                        pkt->len,
                        pkt->pixel_format,
                        pkt->width,
                        pkt->height);

        // Send header
        if (!sslWriteAll(&hdr, sizeof(hdr))) {
            LOG_ERROR << "Failed to send frame header";
            frame_packet_free(pkt);
            tlsDisconnect();
            continue;
        }

        // Send payload — zero copy: pkt->data points into mmap slot
        if (!sslWriteAll(pkt->data, pkt->len)) {
            LOG_ERROR << "Failed to send frame payload";
            frame_packet_free(pkt);
            tlsDisconnect();
            continue;
        }

        frames_sent_++;
        bytes_sent_ += sizeof(hdr) + pkt->len;

        LOG_DEBUG << "Sent frame seq=" << pkt->seq << " len=" << pkt->len;
        frame_packet_free(pkt);
        pkt = nullptr;
    }

    // Disconnect on shutdown
    tlsDisconnect();

    LOG_INFO << "Network thread exiting (sent " << frames_sent_.load()
             << " frames, " << bytes_sent_.load() << " bytes)";
}

}  // namespace streamer
