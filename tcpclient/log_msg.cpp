// ============================================================================
// log_msg.cpp — Thread-safe singleton logger implementation
// ============================================================================
// Implements the LogMsg singleton with stdout, file, and syslog backends.
// See log_msg.h for the public API and convenience macros.
// ============================================================================

#include "log_msg.h"

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <syslog.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <mutex>

#ifdef __linux__
#include <sys/syscall.h>
#endif

struct LogEntry {
    LogLevel level;
    uint8_t  channels;
    uint64_t thread_id;
    uint64_t timestamp_ms;
    char     msg[512];
};

struct LogMsg::Impl {
    std::atomic<bool> running{false};
    std::thread bg_thread;
    
    // Bounded MPSC Queue
    static constexpr size_t Q_SIZE = 8192;
    LogEntry ring[Q_SIZE];
    std::atomic<size_t> write_idx{0};
    std::atomic<size_t> read_idx{0};
    std::mutex lock;
    std::atomic<uint64_t> dropped{0};

    FILE* log_file = nullptr;
    bool is_interactive = true;

    // Verbosity thresholds
    LogLevel thresh_stdout = LOG_LEVEL_INFO;
    LogLevel thresh_file   = LOG_LEVEL_DEBUG;
    LogLevel thresh_syslog = LOG_LEVEL_NOTICE;

    static uint64_t getThreadId() {
        static thread_local uint64_t tid = 0;
        if (tid == 0) {
#ifdef __linux__
            tid = syscall(SYS_gettid);
#else
            tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
        }
        return tid;
    }

    void init(const char* argv0, const char* log_dir,
              int arg_stdout, int arg_file, int arg_syslog) {
        if (running.load()) return;
        
        // Environment helper for LogLevels
        auto getEnvLvl = [](const char* name, int def) -> int {
            const char* val = std::getenv(name);
            if (val) {
                try { return std::stoi(val); } catch(...) {}
            }
            return def;
        };

        // 3-Level Hierarchy for thresholds (Default -> ENV -> Args)
        thresh_stdout = static_cast<LogLevel>(arg_stdout != -1 ? arg_stdout : getEnvLvl("APP_LOG_STDOUT", LOG_LEVEL_INFO));
        thresh_file   = static_cast<LogLevel>(arg_file   != -1 ? arg_file   : getEnvLvl("APP_LOG_FILE", LOG_LEVEL_DEBUG));
        thresh_syslog = static_cast<LogLevel>(arg_syslog != -1 ? arg_syslog : getEnvLvl("APP_LOG_SYSLOG", LOG_LEVEL_NOTICE));

        // Determine directory
        std::string dir = log_dir ? log_dir : "";
        if (dir.empty()) {
            const char* env_dir = getenv("APP_LOG_DIR");
            dir = env_dir ? env_dir : "./log";
        }

        // Create log directory (fails safely if already exists)
        mkdir(dir.c_str(), 0750);

        // Get time for filename
        auto t = std::time(nullptr);
        auto tm = *std::localtime(&t);
        char timebuf[64];
        std::strftime(timebuf, sizeof(timebuf), "%Y%m%d_%H%M%S", &tm);

        // Get basename of argv0
        const char* base = strrchr(argv0, '/');
        base = base ? base + 1 : argv0;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s_%s_%d.log", dir.c_str(), base, timebuf, getpid());

        log_file = fopen(filepath, "a");
        if (!log_file) {
            fprintf(stderr, "Failed to open log file %s\n", filepath);
        }

        is_interactive = isatty(fileno(stdout));
        
        // Open syslog
        openlog(base, LOG_PID | LOG_NDELAY, LOG_USER);

        running.store(true, std::memory_order_release);
        bg_thread = std::thread([this]() { threadFunc(); });
    }

    void shutdown() {
        if (!running.load()) return;
        running.store(false, std::memory_order_release);
        if (bg_thread.joinable()) {
            bg_thread.join();
        }
        if (log_file) {
            fclose(log_file);
            log_file = nullptr;
        }
        closelog();
    }

    void log(uint8_t channels, LogLevel level, const char* format, va_list ap) {
        if (!running.load(std::memory_order_relaxed)) return;

        LogEntry entry;
        entry.level = level;
        entry.channels = channels;
        entry.thread_id = getThreadId();
        
        auto now = std::chrono::system_clock::now();
        entry.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch()).count();

        vsnprintf(entry.msg, sizeof(entry.msg), format, ap);

        {
            std::lock_guard<std::mutex> guard(lock);
            size_t widx = write_idx.load(std::memory_order_relaxed);
            if (widx - read_idx.load(std::memory_order_relaxed) < Q_SIZE) {
                ring[widx % Q_SIZE] = entry;
                write_idx.store(widx + 1, std::memory_order_release);
            } else {
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const char* getLevelStr(LogLevel l) {
        switch(l) {
            case LOG_LEVEL_CRIT:   return "CRIT";
            case LOG_LEVEL_ERR:    return "ERROR";
            case LOG_LEVEL_WARN:   return "WARN";
            case LOG_LEVEL_NOTICE: return "NOTICE";
            case LOG_LEVEL_INFO:   return "INFO";
            case LOG_LEVEL_DEBUG:  return "DEBUG";
            default:               return "UNK";
        }
    }

    void processEntry(const LogEntry& e) {
        auto time_point = std::chrono::system_clock::time_point(std::chrono::milliseconds(e.timestamp_ms));
        auto r_t = std::chrono::system_clock::to_time_t(time_point);
        auto ms = e.timestamp_ms % 1000;
        
        char tstr[64];
        std::strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", std::localtime(&r_t));
        
        char full_msg[1024];
        snprintf(full_msg, sizeof(full_msg), "[%s.%03d] [%-6s] [TID:%llu] %s", 
                 tstr, (int)ms, getLevelStr(e.level), (unsigned long long)e.thread_id, e.msg);
                 
        if (e.channels & CH_STDOUT) {
            if (is_interactive) {
                fprintf(stdout, "%s\n", full_msg);
            }
        }
        if (e.channels & CH_FILE && log_file) {
            fprintf(log_file, "%s\n", full_msg);
        }
        if (e.channels & CH_SYSLOG) {
            syslog(e.level, "[TID:%llu] %s", (unsigned long long)e.thread_id, e.msg);
        }
    }

    void threadFunc() {
        while (running.load(std::memory_order_acquire) || 
               read_idx.load(std::memory_order_acquire) < write_idx.load(std::memory_order_acquire)) {
            
            size_t ridx = read_idx.load(std::memory_order_acquire);
            size_t widx = write_idx.load(std::memory_order_acquire); // Snapshot
            
            if (ridx < widx) {
                while (ridx < widx) {
                    processEntry(ring[ridx % Q_SIZE]);
                    ridx++;
                }
                read_idx.store(ridx, std::memory_order_release);
                if (log_file) fflush(log_file);

                uint64_t d = dropped.exchange(0, std::memory_order_relaxed);
                if (d > 0) {
                    fprintf(stderr, "[Logger] WARNING: %llu log messages dropped due to full queue\n",
                            (unsigned long long)d);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
};

// ==========================================================
// LogMsg Interface
// ==========================================================

LogMsg& LogMsg::getInstance() {
    static LogMsg instance;
    return instance;
}

LogMsg::LogMsg() : pimpl_(new Impl()) {}
LogMsg::~LogMsg() { delete pimpl_; }

void LogMsg::init(const char* argv0, const char* dir,
                  int stdout_lvl, int file_lvl, int syslog_lvl) {
    pimpl_->init(argv0, dir, stdout_lvl, file_lvl, syslog_lvl);
}
void LogMsg::shutdown() { pimpl_->shutdown(); }

void LogMsg::log(LogLevel level, const char* format, ...) {
    uint8_t channels = 0;
    // Dynamic Threshold Logic
    if (level <= pimpl_->thresh_stdout) channels |= CH_STDOUT;
    if (level <= pimpl_->thresh_file)   channels |= CH_FILE;
    if (level <= pimpl_->thresh_syslog) channels |= CH_SYSLOG;

    va_list args;
    va_start(args, format);
    pimpl_->log(channels, level, format, args);
    va_end(args);
}

void LogMsg::log_to(uint8_t channels, LogLevel level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    pimpl_->log(channels, level, format, args);
    va_end(args);
}
