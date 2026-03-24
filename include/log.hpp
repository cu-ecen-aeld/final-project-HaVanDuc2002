/**
 * @file log.hpp
 * @brief Structured logging with levels — writes to /var/tmp/camera_log via POSIX I/O
 *
 * Log levels: ERROR, WARN, INFO, DEBUG
 * Output format: [LEVEL] [timestamp] [file:line] message
 */

#ifndef LOG_HPP
#define LOG_HPP

#include <sstream>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>

namespace streamer {

static constexpr const char* LOG_FILE_PATH = "/var/tmp/camera_log";

enum class LogLevel {
    Error = 0,
    Warn  = 1,
    Info  = 2,
    Debug = 3
};

// Global log level — can be set at runtime
inline LogLevel g_log_level = LogLevel::Info;

// POSIX mutex for thread-safe log writes
inline pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Log file descriptor — opened once via pthread_once
inline int         g_log_fd   = -1;
inline pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

inline void openLogFile() {
    g_log_fd = open(LOG_FILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_log_fd < 0) {
        // Fall back to stderr if the log file cannot be opened
        g_log_fd = STDERR_FILENO;
    }
}

inline void ensureLogOpen() {
    pthread_once(&g_log_once, openLogFile);
}

// Extract filename from full __FILE__ path
inline const char* extractFilename(const char* path) {
    const char* file = std::strrchr(path, '/');
    return file ? file + 1 : path;
}

// Build timestamp string using POSIX clock_gettime + localtime_r
inline std::string getTimestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char date_buf[24];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    char result[32];
    snprintf(result, sizeof(result), "%s.%03ld", date_buf, ts.tv_nsec / 1000000L);
    return std::string(result);
}

inline const char* levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
        default:              return "?????";
    }
}

// Stream-style log message — writes to /var/tmp/camera_log on destruction
class LogMessage {
public:
    LogMessage(LogLevel level, const char* file, int line)
        : level_(level), should_log_(level <= g_log_level) {
        if (should_log_) {
            stream_ << "[" << levelToString(level) << "] "
                    << "[" << getTimestamp()        << "] "
                    << "[" << extractFilename(file) << ":" << line << "] ";
        }
    }

    ~LogMessage() {
        if (should_log_) {
            stream_ << '\n';
            std::string msg = stream_.str();
            ensureLogOpen();
            pthread_mutex_lock(&g_log_mutex);
            write(g_log_fd, msg.c_str(), msg.size());
            pthread_mutex_unlock(&g_log_mutex);
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
    LogLevel           level_;
    bool               should_log_;
    std::ostringstream stream_;
};

// Errno variant — appends strerror(errno) to the message
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
#define LOG_WARN  streamer::LogMessage(streamer::LogLevel::Warn,  __FILE__, __LINE__)
#define LOG_INFO  streamer::LogMessage(streamer::LogLevel::Info,  __FILE__, __LINE__)
#define LOG_DEBUG streamer::LogMessage(streamer::LogLevel::Debug, __FILE__, __LINE__)

// Log with errno
#define LOG_ERRNO(err_num) streamer::LogErrnoMessage(streamer::LogLevel::Error, __FILE__, __LINE__, err_num)

#endif  // LOG_HPP
