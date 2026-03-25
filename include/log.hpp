/**
 * @file log.hpp
 * @brief Structured logging with levels — writes to /var/tmp/camera_log via POSIX I/O
 *
 * Log levels: ERROR, WARN, INFO, DEBUG
 * Output format: [LEVEL] [timestamp] [file:line] message
 *
 * Uses a fixed 512-byte stack buffer + snprintf overloads — no sstream/heap alloc.
 */

#ifndef LOG_HPP
#define LOG_HPP

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>

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
inline int            g_log_fd   = -1;
inline pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

inline void openLogFile() {
    g_log_fd = open(LOG_FILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_log_fd < 0) {
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

// Fill buf (>= 32 bytes) with "YYYY-MM-DD HH:MM:SS.mmm"
inline void fillTimestamp(char* buf, size_t bufsize) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char date_buf[24];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    snprintf(buf, bufsize, "%s.%03d",
             date_buf, (int)(ts.tv_nsec / 1000000L));
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

// Fixed-buffer log message — writes to /var/tmp/camera_log on destruction.
// No heap allocation; operator<< overloads cover all primitive types used in
// the codebase (const char*, char, bool, int, long, unsigned variants, double).
class LogMessage {
public:
    static constexpr size_t BUF_SIZE = 512;

    LogMessage(LogLevel level, const char* file, int line)
        : should_log_(level <= g_log_level), pos_(0), level_(level) {
        if (should_log_) {
            char ts[32];
            fillTimestamp(ts, sizeof(ts));
            int n = snprintf(buf_, BUF_SIZE - 1, "[%s] [%s] [%s:%d] ",
                             levelToString(level), ts,
                             extractFilename(file), line);
            if (n > 0) pos_ = (size_t)n < BUF_SIZE - 1 ? (size_t)n : BUF_SIZE - 2;
        }
    }

    ~LogMessage() {
        if (should_log_) {
            if (pos_ < BUF_SIZE - 1) buf_[pos_++] = '\n';
            buf_[pos_] = '\0';
            ensureLogOpen();
            pthread_mutex_lock(&g_log_mutex);
            // Write to log file /var/tmp/camera_log
            if (write(g_log_fd, buf_, pos_) < 0) {}
            // Write to terminal (stderr) — skip if log fd IS stderr (open failed)
            if (g_log_fd != STDERR_FILENO) {
                if (write(STDERR_FILENO, buf_, pos_) < 0) {}
            }
            pthread_mutex_unlock(&g_log_mutex);
        }
    }

    // Non-copyable — log objects are always temporaries
    LogMessage(const LogMessage&)            = delete;
    LogMessage& operator=(const LogMessage&) = delete;

    LogMessage& operator<<(const char* s) {
        if (should_log_ && s) append_s(s);
        return *this;
    }
    LogMessage& operator<<(char c) {
        if (should_log_ && pos_ < BUF_SIZE - 2) buf_[pos_++] = c;
        return *this;
    }
    LogMessage& operator<<(bool v)               { return *this << (v ? "true" : "false"); }
    LogMessage& operator<<(int v)                { return appendFmt("%d",   v); }
    LogMessage& operator<<(unsigned int v)       { return appendFmt("%u",   v); }
    LogMessage& operator<<(long v)               { return appendFmt("%ld",  v); }
    LogMessage& operator<<(unsigned long v)      { return appendFmt("%lu",  v); }
    LogMessage& operator<<(long long v)          { return appendFmt("%lld", v); }
    LogMessage& operator<<(unsigned long long v) { return appendFmt("%llu", v); }
    LogMessage& operator<<(double v)             { return appendFmt("%.2f", v); }

protected:
    bool   should_log_;
    char   buf_[BUF_SIZE];
    size_t pos_;

private:
    LogLevel level_;  // declared after should_log_ to match constructor init order

    void append_s(const char* s) {
        if (pos_ >= BUF_SIZE - 1) return;
        int n = snprintf(buf_ + pos_, BUF_SIZE - 1 - pos_, "%s", s);
        if (n > 0) {
            pos_ += (size_t)n;
            if (pos_ >= BUF_SIZE - 1) pos_ = BUF_SIZE - 2;
        }
    }

    template<typename T>
    LogMessage& appendFmt(const char* fmt, T v) {
        if (should_log_ && pos_ < BUF_SIZE - 1) {
            int n = snprintf(buf_ + pos_, BUF_SIZE - 1 - pos_, fmt, v);
            if (n > 0) {
                pos_ += (size_t)n;
                if (pos_ >= BUF_SIZE - 1) pos_ = BUF_SIZE - 2;
            }
        }
        return *this;
    }
};

// Errno variant — appends ": <strerror> (errno=N)" before the final write
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
