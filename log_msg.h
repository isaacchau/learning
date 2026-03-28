#ifndef LOG_MSG_H
#define LOG_MSG_H

#include <cstdint>
#include <cstdarg>
#include <string>

// Syslog compatible verbosity levels
enum LogLevel {
    LOG_LEVEL_CRIT   = 2,
    LOG_LEVEL_ERR    = 3,
    LOG_LEVEL_WARN   = 4,
    LOG_LEVEL_NOTICE = 5,
    LOG_LEVEL_INFO   = 6,
    LOG_LEVEL_DEBUG  = 7
};

// Bitmask for targeting multiple output channels ad-hoc
enum LogChannel : uint8_t {
    CH_STDOUT = 1 << 0,
    CH_FILE   = 1 << 1,
    CH_SYSLOG = 1 << 2,
    CH_ALL    = CH_STDOUT | CH_FILE | CH_SYSLOG
};

class LogMsg {
public:
    // Returns the thread-safe singleton instance
    static LogMsg& getInstance();

    // Must be called at startup. 
    // argv0: the program name, used to generate log filename.
    // log_dir: optional override for the log directory (default: current_dir/log)
    // *_level: optional overrides for verbosity thresholds (default: reads APP_LOG_* envs)
    void init(const char* argv0, const char* log_dir = nullptr,
              int stdout_level = -1, int file_level = -1, int syslog_level = -1);
    
    // Shut down gracefully, flushing all logs.
    void shutdown();

    // Log to channels based on default severity thresholds
    void log(LogLevel level, const char* format, ...);

    // Ad-hoc logging to explicitly specified channels
    void log_to(uint8_t channels, LogLevel level, const char* format, ...);

private:
    LogMsg();
    ~LogMsg();
    LogMsg(const LogMsg&) = delete;
    LogMsg& operator=(const LogMsg&) = delete;

    struct Impl;
    Impl* pimpl_;
};

// Thread-safe fast convenience macros
#define LOG_CRIT(...)   LogMsg::getInstance().log(LOG_LEVEL_CRIT, __VA_ARGS__)
#define LOG_ERR(...)    LogMsg::getInstance().log(LOG_LEVEL_ERR, __VA_ARGS__)
#define LOG_WARN(...)   LogMsg::getInstance().log(LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_NOTICE(...) LogMsg::getInstance().log(LOG_LEVEL_NOTICE, __VA_ARGS__)
#define LOG_INFO(...)   LogMsg::getInstance().log(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUG(...)  LogMsg::getInstance().log(LOG_LEVEL_DEBUG, __VA_ARGS__)

#endif // LOG_MSG_H
