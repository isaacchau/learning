#include "output_writer.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <cstring>

namespace aggregation {

OutputWriter::OutputWriter(const Config& config, OutputQueue* queue)
    : config_(config)
    , queue_(queue)
    , formatter_(config.format) {
}

OutputWriter::~OutputWriter() {
    stop();
}

void OutputWriter::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }
    
    // Ensure output directory exists
    mkdir(config_.output_dir.c_str(), 0750);
    
    thread_ = std::thread(&OutputWriter::writerLoop, this);
}

void OutputWriter::stop() {
    if (!running_.exchange(false)) {
        return;  // Not running
    }
    
    // Signal queue to wake up waiting consumer
    if (queue_) {
        queue_->shutdown();
    }
    
    if (thread_.joinable()) {
        thread_.join();
    }
    
    closeCurrentFile();
}

std::string OutputWriter::currentFilename() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(
        *reinterpret_cast<std::mutex*>(const_cast<OutputWriter*>(this))));  // Ugly but needed for const
    return current_filename_;
}

void OutputWriter::writerLoop() {
    OutputRecord record;
    
    while (running_.load(std::memory_order_relaxed)) {
        // Try to get a record with 100ms timeout
        if (queue_->pop(record, 100)) {
            // Open file if needed (first record or rotation)
            if (!current_file_.is_open()) {
                openNewFile(record.timestamp_ns);
            }
            
            // Format and write
            std::string line = formatter_.format(record);
            current_file_ << line << "\n";
            
            // Update stats
            bytes_written_.fetch_add(line.size() + 1, std::memory_order_relaxed);
            
            // Check for rotation
            if (shouldRotate()) {
                closeCurrentFile();
                openNewFile(record.timestamp_ns);
            }
        }
    }
    
    // Drain remaining records
    while (queue_->pop(record, 0)) {
        if (!current_file_.is_open()) {
            openNewFile(record.timestamp_ns);
        }
        
        std::string line = formatter_.format(record);
        current_file_ << line << "\n";
        bytes_written_.fetch_add(line.size() + 1, std::memory_order_relaxed);
    }
}

void OutputWriter::openNewFile(uint64_t timestamp_ns) {
    closeCurrentFile();
    
    std::string filename = generateFilename(timestamp_ns);
    std::string filepath = config_.output_dir + "/" + filename;
    
    current_file_.open(filepath, std::ios::out | std::ios::app);
    if (!current_file_.is_open()) {
        std::cerr << "[OutputWriter] Failed to open file: " << filepath << std::endl;
        return;
    }
    
    current_filename_ = filepath;
    bytes_written_ = 0;
    csv_header_written_ = false;
    
    // Convert nanoseconds to time_point for tracking
    auto secs = std::chrono::nanoseconds(timestamp_ns);
    file_start_time_ = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(secs));
    
    // Write CSV header if needed (will be written on first record of each type)
    // CSV header is measurement-specific, so we write it per measurement type
}

void OutputWriter::closeCurrentFile() {
    if (current_file_.is_open()) {
        current_file_.flush();
        current_file_.close();
    }
    current_filename_.clear();
    bytes_written_ = 0;
}

std::string OutputWriter::generateFilename(uint64_t timestamp_ns) {
    // Convert nanoseconds to time_t for formatting
    auto secs = std::chrono::nanoseconds(timestamp_ns);
    auto time_t_val = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(secs)));
    
    std::tm* tm = std::localtime(&time_t_val);
    
    std::ostringstream oss;
    oss << config_.filename_prefix << "_"
        << std::put_time(tm, "%Y%m%d_%H%M%S");
    
    if (config_.format == OutputFormat::INFLUXDB_LINE) {
        oss << ".txt";
    } else {
        oss << ".csv";
    }
    
    return oss.str();
}

bool OutputWriter::shouldRotate() const {
    if (config_.max_file_size_mb == 0) {
        return false;  // No size limit
    }
    
    uint64_t max_bytes = config_.max_file_size_mb * 1024 * 1024;
    return bytes_written_.load(std::memory_order_relaxed) >= max_bytes;
}

} // namespace aggregation
