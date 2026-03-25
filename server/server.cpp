/**
 * @file server.cpp
 * @brief Simple TLS server to receive camera frames
 *
 * This server accepts TLS connections and receives frames using the
 * protocol defined in protocol.hpp. Frames are saved to disk or displayed
 * as statistics.
 *
 * Usage:
 *   ./frame_server --port 4433 --cert server.crt --key server.key
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <csignal>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <getopt.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

// Protocol definitions
constexpr char PROTO_MAGIC[4] = {'F', 'R', 'A', 'M'};
constexpr size_t PROTO_MAGIC_LEN = 4;
constexpr uint16_t PROTO_VERSION = 1;

#pragma pack(push, 1)
struct FrameHeader {
    char magic[PROTO_MAGIC_LEN];
    uint16_t version;
    uint16_t header_len;
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t payload_len;
    uint32_t pixel_format;
    uint16_t width;
    uint16_t height;
};
#pragma pack(pop)

constexpr size_t FRAME_HEADER_SIZE = sizeof(FrameHeader);

// Helper for 64-bit network byte order
inline uint64_t ntohll(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<uint64_t>(ntohl(val & 0xFFFFFFFF)) << 32) | ntohl(val >> 32);
#else
    return val;
#endif
}

// Configuration
struct Config {
    uint16_t port = 4433;
    std::string cert_file;
    std::string key_file;
    std::string output_dir;
    bool save_frames = false;
    bool verbose = false;
};

// Statistics
struct Stats {
    std::atomic<uint64_t> frames_received{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> frames_saved{0};
    std::chrono::steady_clock::time_point start_time;
};

// Global state
static std::atomic<bool> g_running{true};
static Stats g_stats;

// Signal handler (async-signal-safe: only writes to atomic bool)
static void signalHandler(int sig) {
    (void)sig;
    g_running = false;
}

// Convert fourcc to string
static std::string fourccToString(uint32_t fourcc) {
    char buf[5] = {0};
    buf[0] = static_cast<char>((fourcc >> 0) & 0xFF);
    buf[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    buf[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    buf[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    return std::string(buf);
}

// Log OpenSSL errors
static void logSslErrors(const char* ctx) {
    unsigned long err;
    char buf[256];
    while ((err = ERR_get_error()) != 0) {
        ERR_error_string_n(err, buf, sizeof(buf));
        std::cerr << "[ERROR] " << ctx << ": " << buf << "\n";
    }
}

// Create SSL context for server
static SSL_CTX* createSslContext(const std::string& cert_file, const std::string& key_file) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        logSslErrors("SSL_CTX_new");
        return nullptr;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // Load certificate
    if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        logSslErrors("SSL_CTX_use_certificate_file");
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // Load private key
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        logSslErrors("SSL_CTX_use_PrivateKey_file");
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // Verify key matches certificate
    if (!SSL_CTX_check_private_key(ctx)) {
        std::cerr << "[ERROR] Certificate and private key do not match\n";
        SSL_CTX_free(ctx);
        return nullptr;
    }

    return ctx;
}

// Read exactly n bytes from SSL
static bool sslReadAll(SSL* ssl, void* buf, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        int n = SSL_read(ssl, ptr, static_cast<int>(remaining));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                return false;  // Connection closed
            }
            if (err == SSL_ERROR_SYSCALL && errno == EINTR) {
                continue;
            }
            return false;
        }
        ptr += n;
        remaining -= n;
    }
    return true;
}

// Save frame to file
static bool saveFrame(const Config& cfg, const FrameHeader& hdr,
                      const uint8_t* data, uint32_t len) {
    std::string fourcc = fourccToString(ntohl(hdr.pixel_format));

    // Determine file extension based on format
    std::string ext = "raw";
    if (fourcc == "MJPG") {
        ext = "jpg";
    } else if (fourcc == "YUYV") {
        ext = "yuyv";
    } else if (fourcc == "BGR3") {
        ext = "bgr";
    }

    char filename[512];
    snprintf(filename, sizeof(filename), "%s/frame_%08lu.%s",
             cfg.output_dir.c_str(),
             static_cast<unsigned long>(ntohll(hdr.seq)),
             ext.c_str());

    // Open file with Linux system call
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "[ERROR] Failed to open " << filename << " for writing: "
                  << strerror(errno) << "\n";
        return false;
    }

    // Write data using Linux write() function
    ssize_t written = 0;
    ssize_t remaining = len;
    const uint8_t* ptr = data;
    
    while (remaining > 0) {
        ssize_t n = write(fd, ptr, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[ERROR] Failed to write frame data: " 
                      << strerror(errno) << "\n";
            close(fd);
            return false;
        }
        written += n;
        ptr += n;
        remaining -= n;
    }

    close(fd);
    return true;
}

// Handle one client connection
static void handleClient(SSL* ssl, const Config& cfg, const std::string& client_addr) {
    std::cout << "[INFO] Client connected: " << client_addr << "\n";

    FrameHeader hdr;
    std::vector<uint8_t> frame_buf;
    uint64_t client_frames = 0;

    while (g_running) {
        // Read frame header
        if (!sslReadAll(ssl, &hdr, FRAME_HEADER_SIZE)) {
            break;
        }

        // Validate magic
        if (std::memcmp(hdr.magic, PROTO_MAGIC, PROTO_MAGIC_LEN) != 0) {
            std::cerr << "[ERROR] Invalid magic\n";
            break;
        }

        // Validate version
        uint16_t version = ntohs(hdr.version);
        if (version != PROTO_VERSION) {
            std::cerr << "[ERROR] Unsupported protocol version: " << version << "\n";
            break;
        }

        // Get payload length
        uint32_t payload_len = ntohl(hdr.payload_len);
        if (payload_len > 16 * 1024 * 1024) {  // 16MB max
            std::cerr << "[ERROR] Payload too large: " << payload_len << "\n";
            break;
        }

        // Resize buffer if needed
        if (payload_len > frame_buf.size()) {
            frame_buf.resize(payload_len);
        }

        // Read payload
        if (!sslReadAll(ssl, frame_buf.data(), payload_len)) {
            std::cerr << "[ERROR] Failed to read payload\n";
            break;
        }

        // Update stats
        g_stats.frames_received++;
        g_stats.bytes_received += FRAME_HEADER_SIZE + payload_len;
        client_frames++;

        // Parse header fields
        uint64_t seq = ntohll(hdr.seq);
        uint64_t ts = ntohll(hdr.timestamp_ns);
        uint16_t width = ntohs(hdr.width);
        uint16_t height = ntohs(hdr.height);
        std::string fourcc = fourccToString(ntohl(hdr.pixel_format));

        if (cfg.verbose) {
            std::cout << "[DEBUG] Frame #" << seq << ": " << width << "x" << height
                      << " " << fourcc << ", " << payload_len << " bytes, ts=" << ts << " ns\n";
        }

        // Save frame if enabled
        if (cfg.save_frames) {
            if (saveFrame(cfg, hdr, frame_buf.data(), payload_len)) {
                g_stats.frames_saved++;
            }
        }

        // Print periodic stats
        if (client_frames % 100 == 0) {
            std::cout << "[INFO] Received " << client_frames << " frames from " << client_addr << "\n";
        }
    }

    // Zero out frame buffer for security
    if (!frame_buf.empty()) {
        std::memset(frame_buf.data(), 0, frame_buf.size());
    }

    std::cout << "[INFO] Client disconnected: " << client_addr
              << " (received " << client_frames << " frames)\n";
}

// Print usage
static void printUsage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "Simple TLS server to receive camera frames\n"
        "\n"
        "Options:\n"
        "  --port N          Listen port (default: 4433)\n"
        "  --cert FILE       Server certificate (PEM)\n"
        "  --key FILE        Server private key (PEM)\n"
        "  --output DIR      Save frames to directory\n"
        "  --verbose, -v     Verbose output\n"
        "  --help, -h        Show this help\n"
        "\n"
        "To generate self-signed certificate:\n"
        "  openssl req -x509 -newkey rsa:4096 -keyout server.key \\\n"
        "    -out server.crt -days 365 -nodes -subj '/CN=localhost'\n"
        "\n";
}

// Parse command line
static bool parseArgs(int argc, char* argv[], Config& cfg) {
    static struct option long_options[] = {
        {"port",    required_argument, nullptr, 'p'},
        {"cert",    required_argument, nullptr, 'c'},
        {"key",     required_argument, nullptr, 'k'},
        {"output",  required_argument, nullptr, 'o'},
        {"verbose", no_argument,       nullptr, 'v'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:c:k:o:vh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'p':
                cfg.port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'c':
                cfg.cert_file = optarg;
                break;
            case 'k':
                cfg.key_file = optarg;
                break;
            case 'o':
                cfg.output_dir = optarg;
                cfg.save_frames = true;
                break;
            case 'v':
                cfg.verbose = true;
                break;
            case 'h':
            default:
                printUsage(argv[0]);
                return false;
        }
    }

    if (cfg.cert_file.empty() || cfg.key_file.empty()) {
        std::cerr << "Error: --cert and --key are required\n\n";
        printUsage(argv[0]);
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) {
        return EXIT_FAILURE;
    }

    // Create output directory if saving frames
    if (cfg.save_frames) {
        if (mkdir(cfg.output_dir.c_str(), 0755) < 0 && errno != EEXIST) {
            perror("mkdir");
            return EXIT_FAILURE;
        }
        std::cout << "[INFO] Saving frames to: " << cfg.output_dir << "\n";
    }

    // Setup signal handlers using POSIX sigaction
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  // restart interrupted syscalls where possible
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Initialize OpenSSL
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    // Create SSL context
    SSL_CTX* ssl_ctx = createSslContext(cfg.cert_file, cfg.key_file);
    if (!ssl_ctx) {
        return EXIT_FAILURE;
    }

    // Create server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        SSL_CTX_free(ssl_ctx);
        return EXIT_FAILURE;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cfg.port);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        SSL_CTX_free(ssl_ctx);
        return EXIT_FAILURE;
    }

    // Listen
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        SSL_CTX_free(ssl_ctx);
        return EXIT_FAILURE;
    }

    std::cout << "[INFO] Frame server listening on port " << cfg.port << "\n";
    std::cout << "[INFO] Press Ctrl+C to stop\n";

    g_stats.start_time = std::chrono::steady_clock::now();

    // Accept loop — use select() with 1-second timeout so Ctrl+C is handled promptly
    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        struct timeval tv{1, 0};  // 1-second timeout

        int ready = select(server_fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;  // signal interrupted select, recheck g_running
            perror("select");
            break;
        }
        if (ready == 0) continue;  // timeout, loop back to check g_running

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));

        // Create SSL connection
        SSL* ssl = SSL_new(ssl_ctx);
        SSL_set_fd(ssl, client_fd);

        if (SSL_accept(ssl) <= 0) {
            logSslErrors("SSL_accept");
            SSL_free(ssl);
            close(client_fd);
            continue;
        }

        // Handle client (blocking - single threaded for simplicity)
        handleClient(ssl, cfg, addr_str);

        // Cleanup
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(client_fd);
    }

    // Print final stats
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_stats.start_time).count();

    std::cout << "\n[INFO] Server stopped\n";
    std::cout << "[INFO] Total frames received: " << g_stats.frames_received.load() << "\n";
    std::cout << "[INFO] Total bytes received: " << g_stats.bytes_received.load() << "\n";
    if (cfg.save_frames) {
        std::cout << "[INFO] Frames saved: " << g_stats.frames_saved.load() << "\n";
    }
    if (elapsed > 0) {
        std::cout << "[INFO] Average: "
                  << static_cast<double>(g_stats.frames_received.load()) / elapsed << " frames/sec, "
                  << static_cast<double>(g_stats.bytes_received.load()) / 1024.0 / elapsed << " KB/sec\n";
    }

    close(server_fd);
    SSL_CTX_free(ssl_ctx);

    return EXIT_SUCCESS;
}