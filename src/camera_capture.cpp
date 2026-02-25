/**
 * @file camera_capture.cpp
 * @brief OpenCV-based camera capture implementation
 */

#include "camera_capture.hpp"
#include "ring_queue.hpp"
#include "protocol.hpp"
#include "log.hpp"

#include <chrono>
#include <thread>

namespace streamer {

CameraCapture::CameraCapture(const CameraConfig& config)
    : config_(config) {
    // Setup JPEG encoding parameters
    jpeg_params_ = {cv::IMWRITE_JPEG_QUALITY, config.jpeg_quality};
}

CameraCapture::~CameraCapture() {
    stop();
    if (cap_.isOpened()) {
        cap_.release();
    }
    LOG_DEBUG << "Camera capture resources cleaned up";
}

bool CameraCapture::open() {
    // Try to parse device as integer index first
    int device_index = -1;
    try {
        device_index = std::stoi(config_.device);
    } catch (...) {
        device_index = -1;
    }

    bool opened = false;
    if (device_index >= 0) {
        // Open by index
        LOG_INFO << "Opening camera by index: " << device_index;
        opened = cap_.open(device_index, cv::CAP_V4L2);
        if (!opened) {
            // Try without V4L2 backend
            opened = cap_.open(device_index);
        }
    } else {
        // Open by path
        LOG_INFO << "Opening camera by path: " << config_.device;
        opened = cap_.open(config_.device, cv::CAP_V4L2);
        if (!opened) {
            // Try without V4L2 backend
            opened = cap_.open(config_.device);
        }
    }

    if (!opened) {
        LOG_ERROR << "Failed to open camera: " << config_.device;
        return false;
    }

    LOG_INFO << "Camera opened: " << config_.device;

    // Set requested resolution
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
    cap_.set(cv::CAP_PROP_FPS, config_.fps);

    // Get actual values after negotiation
    actual_width_ = static_cast<uint16_t>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    actual_height_ = static_cast<uint16_t>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    actual_fps_ = static_cast<uint32_t>(cap_.get(cv::CAP_PROP_FPS));

    // Determine pixel format
    if (config_.use_jpeg) {
        pixel_format_ = static_cast<uint32_t>(PixelFormat::JPEG);
    } else {
        pixel_format_ = static_cast<uint32_t>(PixelFormat::BGR24);
    }

    LOG_INFO << "Camera configured: " << actual_width_ << "x" << actual_height_
             << " @ " << actual_fps_ << " fps"
             << ", format=" << pixelFormatToString(pixel_format_);

    if (actual_width_ != config_.width || actual_height_ != config_.height) {
        LOG_WARN << "Actual resolution (" << actual_width_ << "x" << actual_height_
                 << ") differs from requested (" << config_.width << "x" << config_.height << ")";
    }

    return true;
}

bool CameraCapture::start() {
    if (!cap_.isOpened()) {
        LOG_ERROR << "Cannot start capture: camera not opened";
        return false;
    }

    streaming_ = true;
    frame_seq_ = 0;
    LOG_INFO << "Capture streaming started";
    return true;
}

void CameraCapture::stop() {
    if (streaming_) {
        streaming_ = false;
        LOG_INFO << "Capture streaming stopped";
    }
}

bool CameraCapture::isOpened() const {
    return cap_.isOpened();
}

uint64_t CameraCapture::getTimestampNs() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

bool CameraCapture::encodeJpeg(const cv::Mat& frame, std::vector<uint8_t>& output) {
    try {
        return cv::imencode(".jpg", frame, output, jpeg_params_);
    } catch (const cv::Exception& e) {
        LOG_ERROR << "JPEG encoding failed: " << e.what();
        return false;
    }
}

void CameraCapture::captureLoop(RingQueue& queue, const std::atomic<bool>& running) {
    LOG_INFO << "Capture thread starting";

    cv::Mat frame;
    std::vector<uint8_t> encoded_data;

    while (running.load() && streaming_) {
        // Capture frame
        if (!cap_.read(frame)) {
            LOG_WARN << "Failed to read frame from camera";
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (frame.empty()) {
            LOG_WARN << "Empty frame received";
            continue;
        }

        // Get timestamp
        uint64_t timestamp_ns = getTimestampNs();

        // Prepare frame data
        const uint8_t* data_ptr = nullptr;
        uint32_t data_len = 0;

        if (config_.use_jpeg) {
            // Encode as JPEG
            if (!encodeJpeg(frame, encoded_data)) {
                LOG_WARN << "Failed to encode frame as JPEG";
                continue;
            }
            data_ptr = encoded_data.data();
            data_len = static_cast<uint32_t>(encoded_data.size());
        } else {
            // Send raw BGR data
            data_ptr = frame.data;
            data_len = static_cast<uint32_t>(frame.total() * frame.elemSize());
        }

        // Push frame to queue
        uint64_t seq = frame_seq_++;
        bool pushed = queue.push(
            seq,
            timestamp_ns,
            data_ptr,
            data_len,
            pixel_format_,
            static_cast<uint16_t>(frame.cols),
            static_cast<uint16_t>(frame.rows)
        );

        if (!pushed) {
            LOG_DEBUG << "Frame " << seq << " dropped";
        }
    }

    LOG_INFO << "Capture thread exiting (captured " << frame_seq_.load() << " frames)";
}

}  // namespace streamer
