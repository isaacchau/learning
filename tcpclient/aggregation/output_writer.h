#ifndef OUTPUT_WRITER_H
#define OUTPUT_WRITER_H

#include "output_queue.h"
#include "output_formatter.h"
#include "aggregation_config.h"

#include <thread>
#include <atomic>
#include <fstream>
#include <string>
#include <chrono>

namespace aggregation {

// ============================================================================
// Output Writer Thread
// Single thread that dequeues records and writes to files
// ============================================================================

class OutputWriter {
public:
    struct Config {
        std::string output_dir;
        std::string filename_prefix;
        OutputFormat format;
        size_t max_file_size_mb;
    };
    
    OutputWriter(const Config& config, OutputQueue* queue);
    ~OutputWriter();
    
    // Start the writer thread
    void start();
    
    // Graceful shutdown
    void stop();
    
    // Check if running
    bool isRunning() const { return running_.load(std::memory_order_relaxed); }
    
    // Get current filename
    std::string currentFilename() const;
    
    // Get bytes written to current file
    uint64_t bytesWritten() const { return bytes_written_.load(std::memory_order_relaxed); }

private:
    void writerLoop();
    
    // Open a new output file
    void openNewFile(uint64_t timestamp_ns);
    
    // Close current file
    void closeCurrentFile();
    
    // Generate filename based on timestamp
    std::string generateFilename(uint64_t timestamp_ns);
    
    // Check if we need to rotate (size limit)
    bool shouldRotate() const;
    
    // Write CSV header if needed
    void writeCsvHeaderIfNeeded();
    
    Config config_;
    OutputQueue* queue_;
    OutputFormatter formatter_;
    
    std::thread thread_;
    std::atomic<bool> running_{false};
    
    // Current file state
    std::ofstream current_file_;
    std::string current_filename_;
    std::atomic<uint64_t> bytes_written_{0};
    
    // Track which measurements we've written headers for (CSV only)
    // In current window/file
    bool csv_header_written_ = false;
    
    // For generating unique filenames
    std::chrono::system_clock::time_point file_start_time_;
};

} // namespace aggregation

#endif // OUTPUT_WRITER_H
