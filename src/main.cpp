/**
 * @file main.cpp
 * @brief Camera frame streaming over TLS - Main entry point
 *
 * This program captures frames from a camera using OpenCV and streams them
 * to a remote server over TLS using exactly 2 threads:
 *   Thread 1 (capture): OpenCV frame capture
 *   Thread 2 (network): TLS transmission
 *
 * Backpressure: When the queue is full, oldest frames are dropped.
 * This ensures the capture thread never blocks.
 *
 * Protocol: Each frame is sent with a fixed header (see protocol.hpp)
 * followed by the raw frame payload.
 *
 * Usage:
 *   ./camera_streamer --device 0 --width 1280 --height 720 \
 *                     --fps 30 --host example.com --port 4433
 */

#include <iostream>
#include <string>
#include <atomic>
#include <csignal>
#include <cstring>
#include <getopt.h>
#include <pthread.h>
#include <unistd.h>

#include "log.hpp"
#include "ring_queue.hpp"
#include "camera_capture.hpp"
#include "tls_client.hpp"

using namespace streamer;

// Global shutdown flag
static std::atomic<bool> g_running{true};

struct CaptureThreadContext {
    CameraCapture* capture;
    RingQueue* queue;
};

struct NetworkThreadContext {
    TlsClient* tls;
    RingQueue* queue;
};

static void* captureThreadMain(void* arg) {
    pthread_setname_np(pthread_self(), "capture");
    auto* ctx = static_cast<CaptureThreadContext*>(arg);
    ctx->capture->captureLoop(*ctx->queue, g_running);
    return nullptr;
}

static void* networkThreadMain(void* arg) {
    pthread_setname_np(pthread_self(), "network");
    auto* ctx = static_cast<NetworkThreadContext*>(arg);
    ctx->tls->networkLoop(*ctx->queue, g_running);
    return nullptr;
}

// Default configuration
constexpr const char* DEFAULT_DEVICE = "0";
constexpr uint16_t DEFAULT_WIDTH = 1280;
constexpr uint16_t DEFAULT_HEIGHT = 720;
constexpr uint32_t DEFAULT_FPS = 30;
constexpr uint16_t DEFAULT_PORT = 4433;
constexpr size_t DEFAULT_QUEUE_SIZE = 8;
constexpr int DEFAULT_JPEG_QUALITY = 85;

// Program configuration
struct Config {
    // Camera settings
    std::string device = DEFAULT_DEVICE;
    uint16_t width = DEFAULT_WIDTH;
    uint16_t height = DEFAULT_HEIGHT;
    uint32_t fps = DEFAULT_FPS;
    bool use_jpeg = true;
    int jpeg_quality = DEFAULT_JPEG_QUALITY;

    // Network settings
    std::string host;
    uint16_t port = DEFAULT_PORT;
    std::string ca_path;

    // Queue settings
    size_t queue_size = DEFAULT_QUEUE_SIZE;

    // Debug
    bool verbose = false;
};

// Signal handler
static void signalHandler(int sig) {
    LOG_INFO << "Received signal " << sig << ", shutting down...";
    g_running = false;
}

// Print usage
static void printUsage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "Camera frame streaming over TLS (OpenCV-based)\n"
        "\n"
        "Camera options:\n"
        "  --device PATH/INDEX  Camera device or index (default: " << DEFAULT_DEVICE << ")\n"
        "  --width N            Frame width (default: " << DEFAULT_WIDTH << ")\n"
        "  --height N           Frame height (default: " << DEFAULT_HEIGHT << ")\n"
        "  --fps N              Frames per second (default: " << DEFAULT_FPS << ")\n"
        "  --no-jpeg            Send raw BGR instead of JPEG\n"
        "  --jpeg-quality N     JPEG quality 1-100 (default: " << DEFAULT_JPEG_QUALITY << ")\n"
        "\n"
        "Network options:\n"
        "  --host HOSTNAME      Remote server hostname (required)\n"
        "  --port N             Remote server port (default: " << DEFAULT_PORT << ")\n"
        "  --ca PATH            CA certificate bundle path (default: system)\n"
        "\n"
        "Other options:\n"
        "  --queue-size N       Frame queue size (default: " << DEFAULT_QUEUE_SIZE << ")\n"
        "  --verbose, -v        Enable verbose logging\n"
        "  --help, -h           Show this help\n"
        "\n"
        "Example:\n"
        "  " << prog << " --device 0 --width 1280 --height 720 \\\n"
        "     --fps 30 --host stream.example.com --port 4433\n"
        "\n";
}

// Parse command line arguments
static bool parseArgs(int argc, char* argv[], Config& config) {
    static struct option long_options[] = {
        {"device",       required_argument, nullptr, 'd'},
        {"width",        required_argument, nullptr, 'W'},
        {"height",       required_argument, nullptr, 'H'},
        {"fps",          required_argument, nullptr, 'f'},
        {"no-jpeg",      no_argument,       nullptr, 'J'},
        {"jpeg-quality", required_argument, nullptr, 'Q'},
        {"host",         required_argument, nullptr, 'h'},
        {"port",         required_argument, nullptr, 'p'},
        {"ca",           required_argument, nullptr, 'c'},
        {"queue-size",   required_argument, nullptr, 'q'},
        {"verbose",      no_argument,       nullptr, 'v'},
        {"help",         no_argument,       nullptr, '?'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:W:H:f:JQ:h:p:c:q:v?",
                              long_options, nullptr)) != -1) {
        switch (opt) {
            case 'd':
                config.device = optarg;
                break;
            case 'W':
                config.width = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'H':
                config.height = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'f':
                config.fps = static_cast<uint32_t>(std::stoi(optarg));
                break;
            case 'J':
                config.use_jpeg = false;
                break;
            case 'Q':
                config.jpeg_quality = std::stoi(optarg);
                if (config.jpeg_quality < 1 || config.jpeg_quality > 100) {
                    std::cerr << "Error: JPEG quality must be 1-100\n";
                    return false;
                }
                break;
            case 'h':
                config.host = optarg;
                break;
            case 'p':
                config.port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'c':
                config.ca_path = optarg;
                break;
            case 'q':
                config.queue_size = static_cast<size_t>(std::stoi(optarg));
                break;
            case 'v':
                config.verbose = true;
                break;
            case '?':
            default:
                printUsage(argv[0]);
                return false;
        }
    }

    // Validate required options
    if (config.host.empty()) {
        std::cerr << "Error: --host is required\n\n";
        printUsage(argv[0]);
        return false;
    }

    if (config.width == 0 || config.height == 0) {
        std::cerr << "Error: invalid dimensions\n";
        return false;
    }

    if (config.fps == 0) {
        std::cerr << "Error: invalid fps\n";
        return false;
    }

    if (config.queue_size < 2) {
        std::cerr << "Error: queue size must be at least 2\n";
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    Config config;

    // Parse arguments
    if (!parseArgs(argc, argv, config)) {
        return EXIT_FAILURE;
    }

    // Set log level
    if (config.verbose) {
        g_log_level = LogLevel::Debug;
    }

    LOG_INFO << "Camera Streamer starting";
    LOG_INFO << "Camera: " << config.device << " " << config.width << "x"
             << config.height << " @ " << config.fps << " fps";
    LOG_INFO << "Server: " << config.host << ":" << config.port;

    // Setup signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe

    int ret = EXIT_FAILURE;

    try {
        // Create frame queue
        auto queue = std::make_unique<RingQueue>(
            config.queue_size, MAX_FRAME_SIZE, BackpressurePolicy::DropOldest);

        // Initialize camera capture
        CameraConfig cam_config;
        cam_config.device = config.device;
        cam_config.width = config.width;
        cam_config.height = config.height;
        cam_config.fps = config.fps;
        cam_config.use_jpeg = config.use_jpeg;
        cam_config.jpeg_quality = config.jpeg_quality;

        auto capture = std::make_unique<CameraCapture>(cam_config);
        if (!capture->open()) {
            LOG_ERROR << "Failed to initialize camera";
            return EXIT_FAILURE;
        }

        // Initialize TLS client
        TlsConfig tls_config;
        tls_config.host = config.host;
        tls_config.port = config.port;
        tls_config.ca_path = config.ca_path;
        tls_config.frame_width = capture->actualWidth();
        tls_config.frame_height = capture->actualHeight();
        tls_config.pixel_format = capture->pixelFormat();

        auto tls = std::make_unique<TlsClient>(tls_config);
        if (!tls->initialize()) {
            LOG_ERROR << "Failed to initialize TLS client";
            return EXIT_FAILURE;
        }

        // Start capture
        if (!capture->start()) {
            LOG_ERROR << "Failed to start capture";
            return EXIT_FAILURE;
        }

        // Create capture thread
        pthread_t capture_thread{};
        CaptureThreadContext capture_ctx{capture.get(), queue.get()};
        if (pthread_create(&capture_thread, nullptr, captureThreadMain, &capture_ctx) != 0) {
            LOG_ERROR << "Failed to create capture thread";
            return EXIT_FAILURE;
        }

        // Create network thread
        pthread_t network_thread{};
        NetworkThreadContext network_ctx{tls.get(), queue.get()};
        if (pthread_create(&network_thread, nullptr, networkThreadMain, &network_ctx) != 0) {
            LOG_ERROR << "Failed to create network thread";
            g_running = false;
            queue->shutdown();
            pthread_join(capture_thread, nullptr);
            return EXIT_FAILURE;
        }

        LOG_INFO << "Streaming started (press Ctrl+C to stop)";

        // Main loop - just wait and periodically log stats
        while (g_running) {
            sleep(5);

            if (g_running) {
                auto stats = queue->getStats();
                LOG_INFO << "Stats: captured=" << stats.frames_pushed
                         << ", dropped=" << stats.frames_dropped
                         << ", sent=" << stats.frames_popped;
            }
        }

        ret = EXIT_SUCCESS;

        // Signal shutdown
        LOG_INFO << "Shutting down...";
        g_running = false;
        queue->shutdown();

        // Wait for threads
        pthread_join(capture_thread, nullptr);
        LOG_DEBUG << "Capture thread joined";

        pthread_join(network_thread, nullptr);
        LOG_DEBUG << "Network thread joined";

    } catch (const std::exception& e) {
        LOG_ERROR << "Exception: " << e.what();
        ret = EXIT_FAILURE;
    }

    LOG_INFO << "Camera Streamer stopped";
    return ret;
}
