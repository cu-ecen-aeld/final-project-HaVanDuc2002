/**
 * @file log.hpp
 * @brief Structured logging with levels for C++
 *
 * Log levels: ERROR, WARN, INFO, DEBUG
 * Output format: [LEVEL] [timestamp] [file:line] message
 */

#ifndef LOG_HPP
#define LOG_HPP

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <mutex>

namespace streamer {

enum class LogLevel {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3
};

// Global log level - can be set at runtime
inline LogLevel g_log_level = LogLevel::Info;
inline std::mutex g_log_mutex;

// Extract filename from path
inline const char* extractFilename(const char* path) {
    const char* file = std::strrchr(path, '/');
    return file ? file + 1 : path;
}

// Get current timestamp string
inline std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_info;
    localtime_r(&time_t_now, &tm_info);

    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// Log level to string
inline const char* levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
        default: return "?????";
    }
}

// Log message class for stream-style logging
class LogMessage {
public:
    LogMessage(LogLevel level, const char* file, int line)
        : level_(level), should_log_(level <= g_log_level) {
        if (should_log_) {
            stream_ << "[" << levelToString(level) << "] "
                    << "[" << getTimestamp() << "] "
                    << "[" << extractFilename(file) << ":" << line << "] ";
        }
    }

    ~LogMessage() {
        if (should_log_) {
            stream_ << '\n';
            std::lock_guard<std::mutex> lock(g_log_mutex);
            std::cerr << stream_.str();
        }
    }

    template<typename T>
    LogMessage& operator<<(const T& value) {
        if (should_log_) {
            stream_ << value;
        }
        return *this;
    }

private:
    LogLevel level_;
    bool should_log_;
    std::ostringstream stream_;
};

// Errno log message
class LogErrnoMessage : public LogMessage {
public:
    LogErrnoMessage(LogLevel level, const char* file, int line, int err_num)
        : LogMessage(level, file, line), err_num_(err_num) {}

    ~LogErrnoMessage() {
        if (err_num_ != 0) {
            *this << ": " << std::strerror(err_num_) << " (errno=" << err_num_ << ")";
        }
    }

private:
    int err_num_;
};

}  // namespace streamer

// Logging macros
#define LOG_ERROR streamer::LogMessage(streamer::LogLevel::Error, __FILE__, __LINE__)
#define LOG_WARN  streamer::LogMessage(streamer::LogLevel::Warn, __FILE__, __LINE__)
#define LOG_INFO  streamer::LogMessage(streamer::LogLevel::Info, __FILE__, __LINE__)
#define LOG_DEBUG streamer::LogMessage(streamer::LogLevel::Debug, __FILE__, __LINE__)

// Log with errno
#define LOG_ERRNO(err_num) streamer::LogErrnoMessage(streamer::LogLevel::Error, __FILE__, __LINE__, err_num)

#endif  // LOG_HPP
