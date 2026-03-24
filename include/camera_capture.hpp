/**
 * @file camera_capture.hpp
 * @brief OpenCV-based camera capture interface
 */

#ifndef CAMERA_CAPTURE_HPP
#define CAMERA_CAPTURE_HPP

#include <cstdint>
#include <atomic>
#include <vector>
#include <opencv2/opencv.hpp>

namespace streamer {

// Forward declaration
class RingQueue;

// Capture configuration
struct CameraConfig {
    const char* device  = "0";  // Device path or index (e.g., "/dev/video0" or "0")
    uint16_t width    = 1280;   // Requested width
    uint16_t height   = 720;    // Requested height
    uint32_t fps      = 30;     // Requested frame rate
    bool use_jpeg     = true;   // Encode frames as JPEG for efficient transmission
    int jpeg_quality  = 85;     // JPEG quality (1-100)
};

/**
 * OpenCV-based camera capture
 */
class CameraCapture {
public:
    /**
     * Initialize camera capture
     * @param config Capture configuration
     */
    explicit CameraCapture(const CameraConfig& config);

    ~CameraCapture();

    // Disable copy
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    /**
     * Open and configure the camera
     * @return true on success
     */
    bool open();

    /**
     * Start capturing frames
     * @return true on success
     */
    bool start();

    /**
     * Stop capturing frames
     */
    void stop();

    /**
     * Capture thread entry point
     * Captures frames and pushes them to the queue until running flag is false
     * @param queue Target queue for frames
     * @param running Control flag
     */
    void captureLoop(RingQueue& queue, const std::atomic<bool>& running);

    /**
     * Check if camera is open
     */
    bool isOpened() const;

    /**
     * Get actual width after negotiation
     */
    uint16_t actualWidth() const { return actual_width_; }

    /**
     * Get actual height after negotiation
     */
    uint16_t actualHeight() const { return actual_height_; }

    /**
     * Get actual FPS after negotiation
     */
    uint32_t actualFps() const { return actual_fps_; }

    /**
     * Get pixel format being used
     */
    uint32_t pixelFormat() const { return pixel_format_; }

    /**
     * Get frame count captured
     */
    uint64_t frameCount() const { return frame_seq_.load(); }

private:
    CameraConfig config_;
    cv::VideoCapture cap_;

    uint16_t actual_width_ = 0;
    uint16_t actual_height_ = 0;
    uint32_t actual_fps_ = 0;
    uint32_t pixel_format_ = 0;

    bool streaming_ = false;
    std::atomic<uint64_t> frame_seq_{0};

    // JPEG encoding parameters
    std::vector<int> jpeg_params_;

    // Helper to get timestamp in nanoseconds
    static uint64_t getTimestampNs();

    // Encode frame as JPEG
    bool encodeJpeg(const cv::Mat& frame, std::vector<uint8_t>& output);
};

}  // namespace streamer

#endif  // CAMERA_CAPTURE_HPP
