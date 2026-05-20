// ============================================================================
// metrics.hpp — Multi-threaded metrics aggregation library
// ============================================================================
// Provides Value, TagSet, Datapoint, Shard, and Aggregator classes for
// collecting and flushing time-bucketed metrics to CSV or InfluxDB format.
//
// Design overview:
//   - Metrics are sharded by hash of tags to reduce lock contention.
//   - Each shard maintains a map of (metric_name → Value) for the current
//     time bucket.  When the bucket expires, the shard is swapped and
//     flushed asynchronously by a DiskWriter thread.
//   - The 'add' operation is cumulative (counters); 'set' overwrites (gauges).
// ============================================================================

#ifndef METRICS_HPP
#define METRICS_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metrics {

// ============================================================================
// Output Format Enumeration
// ============================================================================

enum class OutputFormat {
    CSV,
    INFLUXDB_LINE
};

// ============================================================================
// Value - typed value holder for int64, double, string
// ============================================================================

class Value {
public:
    enum Type { INT64, DOUBLE, STRING };

    Value() : type_(INT64) { num_.i = 0; }
    explicit Value(int64_t v) : type_(INT64) { num_.i = v; }
    explicit Value(double v) : type_(DOUBLE) { num_.d = v; }
    explicit Value(const std::string& v) : type_(STRING), s_(v) {}
    explicit Value(std::string&& v) : type_(STRING), s_(std::move(v)) {}

    Type type() const { return type_; }

    int64_t asInt64() const { return num_.i; }
    double asDouble() const { return num_.d; }
    const std::string& asString() const { return s_; }

    // Accumulate (Add semantics): only valid for numeric types
    void add(const Value& other) {
        if (type_ == INT64 && other.type_ == INT64) {
            num_.i += other.num_.i;
        } else if (type_ == INT64 && other.type_ == DOUBLE) {
            type_ = DOUBLE;
            double converted = static_cast<double>(num_.i);
            num_.d = converted + other.num_.d;
        } else if (type_ == DOUBLE && other.type_ == INT64) {
            double converted = static_cast<double>(other.num_.i);
            num_.d += converted;
        } else if (type_ == DOUBLE && other.type_ == DOUBLE) {
            num_.d += other.num_.d;
        } else if (type_ == STRING || other.type_ == STRING) {
            // String + anything = string concatenation (unusual, but defined)
            s_ = formatCSV() + other.formatCSV();
            type_ = STRING;
        }
    }

    // Overwrite (Set semantics)
    void set(const Value& other) {
        type_ = other.type_;
        num_ = other.num_;
        s_ = other.s_;
    }

    // Format for InfluxDB Line Protocol
    std::string formatInfluxDB() const {
        switch (type_) {
            case INT64:
                return std::to_string(num_.i) + "i";
            case DOUBLE: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.6f", num_.d);
                // Trim trailing zeros and possible trailing dot
                std::string s(buf);
                size_t dot = s.find('.');
                if (dot != std::string::npos) {
                    size_t end = s.size();
                    while (end > dot + 1 && s[end - 1] == '0') --end;
                    if (end == dot + 1) --end; // remove trailing dot
                    s.resize(end);
                }
                return s;
            }
            case STRING: {
                std::string result = "\"";
                for (char c : s_) {
                    if (c == '\\' || c == '"') result += '\\';
                    result += c;
                }
                result += '"';
                return result;
            }
        }
        return "";
    }

    // Format for CSV
    std::string formatCSV() const {
        switch (type_) {
            case INT64:
                return std::to_string(num_.i);
            case DOUBLE: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.6f", num_.d);
                std::string s(buf);
                size_t dot = s.find('.');
                if (dot != std::string::npos) {
                    size_t end = s.size();
                    while (end > dot + 1 && s[end - 1] == '0') --end;
                    if (end == dot + 1) --end;
                    s.resize(end);
                }
                return s;
            }
            case STRING: {
                // Quote if contains comma, quote, or newline
                bool needs_quote = false;
                for (char c : s_) {
                    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                        needs_quote = true;
                        break;
                    }
                }
                if (!needs_quote) return s_;
                std::string result = "\"";
                for (char c : s_) {
                    if (c == '"') result += '"';
                    result += c;
                }
                result += '"';
                return result;
            }
        }
        return "";
    }

private:
    Type type_;
    union Num {
        int64_t i;
        double d;
        Num() : i(0) {}
    } num_;
    std::string s_;
};

// ============================================================================
// TagSet - sorted canonical tag key-value pairs
// ============================================================================

class TagSet {
public:
    TagSet() = default;

    void add(std::string key, std::string value) {
        tags_.emplace_back(std::move(key), std::move(value));
        std::sort(tags_.begin(), tags_.end(),
                  [](const Pair& a, const Pair& b) { return a.first < b.first; });
        hash_valid_ = false;
    }

    bool operator==(const TagSet& other) const {
        if (tags_.size() != other.tags_.size()) return false;
        for (size_t i = 0; i < tags_.size(); ++i) {
            if (tags_[i].first != other.tags_[i].first ||
                tags_[i].second != other.tags_[i].second)
                return false;
        }
        return true;
    }

    bool operator!=(const TagSet& other) const { return !(*this == other); }

    size_t hash() const {
        if (hash_valid_) return cached_hash_;
        size_t h = 0;
        for (const auto& p : tags_) {
            h ^= std::hash<std::string>{}(p.first) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::string>{}(p.second) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        cached_hash_ = h;
        hash_valid_ = true;
        return h;
    }

    const std::vector<std::pair<std::string, std::string>>& data() const { return tags_; }

    bool empty() const { return tags_.empty(); }

    size_t size() const { return tags_.size(); }

private:
    using Pair = std::pair<std::string, std::string>;
    std::vector<Pair> tags_;
    mutable size_t cached_hash_ = 0;
    mutable bool hash_valid_ = false;
};

struct TagSetHash {
    size_t operator()(const TagSet& ts) const { return ts.hash(); }
};

struct TagSetEq {
    bool operator()(const TagSet& a, const TagSet& b) const { return a == b; }
};

// ============================================================================
// Datapoint - one (tag-set, bucket) with accumulated fields
// ============================================================================

struct Datapoint {
    uint64_t bucket_ts;
    TagSet tags;
    std::vector<std::pair<std::string, Value>> fields;

    void addField(const std::string& name, const Value& value) {
        for (auto& f : fields) {
            if (f.first == name) {
                f.second.add(value);
                return;
            }
        }
        fields.emplace_back(name, value);
    }

    void setField(const std::string& name, const Value& value) {
        for (auto& f : fields) {
            if (f.first == name) {
                f.second.set(value);
                return;
            }
        }
        fields.emplace_back(name, value);
    }
};

// ============================================================================
// Shard - one shard of an Aggregator
// ============================================================================

class Shard {
public:
    using BucketMap = std::unordered_map<TagSet, Datapoint, TagSetHash, TagSetEq>;

    void add(const TagSet& tags, const std::string& field, const Value& value, uint64_t bucket_ts) {
        auto it = buckets_.find(tags);
        if (it != buckets_.end()) {
            it->second.addField(field, value);
        } else {
            Datapoint dp;
            dp.bucket_ts = bucket_ts;
            dp.tags = tags;
            dp.addField(field, value);
            buckets_.emplace(tags, std::move(dp));
        }
    }

    void set(const TagSet& tags, const std::string& field, const Value& value, uint64_t bucket_ts) {
        auto it = buckets_.find(tags);
        if (it != buckets_.end()) {
            it->second.setField(field, value);
        } else {
            Datapoint dp;
            dp.bucket_ts = bucket_ts;
            dp.tags = tags;
            dp.setField(field, value);
            buckets_.emplace(tags, std::move(dp));
        }
    }

    // Extract all datapoints with bucket_ts <= cutoff.
    // If clear_after_flush is true, erase extracted buckets from the map.
    std::vector<Datapoint> extract(uint64_t cutoff, bool clear_after_flush) {
        std::vector<Datapoint> result;
        auto it = buckets_.begin();
        while (it != buckets_.end()) {
            if (it->second.bucket_ts <= cutoff) {
                if (clear_after_flush) {
                    result.push_back(std::move(it->second));
                    it = buckets_.erase(it);
                } else {
                    // Copy to result, then reset fields but keep the bucket entry
                    result.push_back(it->second);
                    it->second.fields.clear();
                    ++it;
                }
            } else {
                ++it;
            }
        }
        return result;
    }

    std::mutex& mutex() { return mutex_; }

private:
    BucketMap buckets_;
    std::mutex mutex_;
};

// ============================================================================
// Formatter - static formatting utilities
// ============================================================================

class Formatter {
public:
    static std::string formatInfluxDB(const std::string& measurement,
                                       const std::vector<Datapoint>& datapoints) {
        std::ostringstream oss;
        for (const auto& dp : datapoints) {
            oss << escapeMeasurement(measurement);
            for (const auto& tag : dp.tags.data()) {
                oss << "," << tag.first << "=" << escapeTagValue(tag.second);
            }
            oss << " ";
            bool first = true;
            // Sort fields for deterministic output
            auto sorted_fields = dp.fields;
            std::sort(sorted_fields.begin(), sorted_fields.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& field : sorted_fields) {
                if (!first) oss << ",";
                oss << field.first << "=" << field.second.formatInfluxDB();
                first = false;
            }
            oss << " " << dp.bucket_ts << "\n";
        }
        return oss.str();
    }

    static std::string formatCSV(const std::string& measurement,
                                  const std::vector<Datapoint>& datapoints) {
        if (datapoints.empty()) return "";

        // Collect union of all tag keys and field keys across all datapoints
        std::vector<std::string> tag_keys;
        std::vector<std::string> field_keys;
        {
            std::unordered_map<std::string, bool> tag_seen;
            std::unordered_map<std::string, bool> field_seen;
            for (const auto& dp : datapoints) {
                for (const auto& t : dp.tags.data()) {
                    if (!tag_seen[t.first]) {
                        tag_seen[t.first] = true;
                        tag_keys.push_back(t.first);
                    }
                }
                for (const auto& f : dp.fields) {
                    if (!field_seen[f.first]) {
                        field_seen[f.first] = true;
                        field_keys.push_back(f.first);
                    }
                }
            }
        }
        std::sort(tag_keys.begin(), tag_keys.end());
        std::sort(field_keys.begin(), field_keys.end());

        std::ostringstream oss;
        // Header
        oss << "measurement";
        for (const auto& k : tag_keys) {
            oss << ",tag_" << k;
        }
        for (const auto& k : field_keys) {
            oss << ",field_" << k;
        }
        oss << ",time\n";

        // Rows
        for (const auto& dp : datapoints) {
            oss << measurement;
            for (const auto& k : tag_keys) {
                oss << ",";
                bool found = false;
                for (const auto& t : dp.tags.data()) {
                    if (t.first == k) {
                        oss << t.second;
                        found = true;
                        break;
                    }
                }
                if (!found) oss << "";
            }
            for (const auto& k : field_keys) {
                oss << ",";
                for (const auto& f : dp.fields) {
                    if (f.first == k) {
                        oss << f.second.formatCSV();
                        break;
                    }
                }
            }
            oss << "," << dp.bucket_ts << "\n";
        }
        return oss.str();
    }

    static std::string generateFilename(const std::string& output_dir,
                                         const std::string& prefix,
                                         const std::string& measurement,
                                         const std::string& ext,
                                         uint64_t flush_ts_ns,
                                         uint64_t sequence) {
        auto secs = std::chrono::nanoseconds(flush_ts_ns);
        auto tp = std::chrono::system_clock::time_point(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(secs));
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        std::tm tm;
        if (!localtime_r(&time_t_val, &tm)) {
            std::cerr << "[Metrics] Warning: localtime_r failed, using epoch for filename" << std::endl;
            tm = std::tm{};
        }

        std::ostringstream oss;
        oss << output_dir << "/"
            << prefix << "_"
            << measurement << "_"
            << std::put_time(&tm, "%y%m%d_%H%M%S") << "_"
            << sequence << "."
            << ext;
        return oss.str();
    }

private:
    static std::string escapeMeasurement(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (c == ',' || c == ' ') r += '\\';
            r += c;
        }
        return r;
    }

    static std::string escapeTagValue(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (c == ',' || c == '=' || c == ' ') r += '\\';
            r += c;
        }
        return r;
    }
};

// ============================================================================
// DiskWriterTask - item for the MPSC queue
// ============================================================================

struct DiskWriterTask {
    std::string payload;
    std::string filename;
};

// ============================================================================
// DiskWriter - single thread performing all file I/O
// ============================================================================

class DiskWriter {
public:
    explicit DiskWriter(const std::string& output_directory, size_t queue_capacity = 65536)
        : output_directory_(output_directory), queue_capacity_(queue_capacity) {}

    ~DiskWriter() { stop(); }

    void start() {
        if (running_.exchange(true)) return;

        // Ensure output directory exists
        #ifdef __linux__
        mkdir(output_directory_.c_str(), 0750);
        #else
        std::system(("mkdir -p " + output_directory_).c_str());
        #endif

        thread_ = std::thread(&DiskWriter::writerLoop, this);
    }

    void stop() {
        if (!running_.exchange(false)) return;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            shutdown_ = true;
        }
        queue_cv_.notify_all();

        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Non-blocking enqueue. Returns false if queue full (drops task).
    bool enqueue(DiskWriterTask task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.size() >= queue_capacity_) {
                dropped_writes_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            queue_.push(std::move(task));
        }
        queue_cv_.notify_one();
        return true;
    }

    uint64_t droppedWrites() const {
        return dropped_writes_.load(std::memory_order_relaxed);
    }

private:
    void writerLoop() {
        while (running_.load(std::memory_order_relaxed) || !queue_.empty()) {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                [this] { return !queue_.empty() || shutdown_; });

            if (queue_.empty()) continue;

            auto task = std::move(queue_.front());
            queue_.pop();
            lock.unlock();

            writeTask(task);
        }
    }

    void writeTask(const DiskWriterTask& task) {
        std::ofstream file(task.filename, std::ios::out | std::ios::app);
        if (!file.is_open()) {
            std::cerr << "[DiskWriter] Failed to open: " << task.filename << std::endl;
            return;
        }
        file << task.payload;
        file.flush();
    }

    std::string output_directory_;
    size_t queue_capacity_;

    std::queue<DiskWriterTask> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    bool shutdown_ = false;

    std::atomic<uint64_t> dropped_writes_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ============================================================================
// Aggregator - single measurement, internally sharded
// ============================================================================

class Aggregator {
public:
    Aggregator(const std::string& measurement,
               const std::string& file_prefix,
               uint64_t bucket_period_ns,
               size_t num_shards,
               bool clear_on_flush,
               OutputFormat format,
               DiskWriter* writer,
               const std::string& output_dir)
        : measurement_(measurement)
        , file_prefix_(file_prefix)
        , bucket_period_ns_(bucket_period_ns)
        , clear_on_flush_(clear_on_flush)
        , format_(format)
        , writer_(writer)
        , output_dir_(output_dir)
        , flush_sequence_(0) {
        if (num_shards == 0) num_shards = 1;
        for (size_t i = 0; i < num_shards; ++i) {
            shards_.emplace_back(new Shard());
        }
    }

    // Thread-safe Add for int64
    void add(const TagSet& tags, const std::string& field, int64_t by, uint64_t ts_ns) {
        doAddSet(tags, field, Value(by), ts_ns, true);
    }

    // Thread-safe Add for double
    void add(const TagSet& tags, const std::string& field, double by, uint64_t ts_ns) {
        doAddSet(tags, field, Value(by), ts_ns, true);
    }

    // Thread-safe Set for int64
    void set(const TagSet& tags, const std::string& field, int64_t value, uint64_t ts_ns) {
        doAddSet(tags, field, Value(value), ts_ns, false);
    }

    // Thread-safe Set for double
    void set(const TagSet& tags, const std::string& field, double value, uint64_t ts_ns) {
        doAddSet(tags, field, Value(value), ts_ns, false);
    }

    // Thread-safe Set for string
    void set(const TagSet& tags, const std::string& field, const std::string& value, uint64_t ts_ns) {
        doAddSet(tags, field, Value(value), ts_ns, false);
    }

    // Semi-auto flush triggered by incoming timestamp
    void onIncomingTimestamp(uint64_t ts_ns) {
        if (bucket_period_ns_ == 0) return;

        // Update max_event_ts_seen
        uint64_t old_max = max_event_ts_seen_.load(std::memory_order_relaxed);
        while (ts_ns > old_max &&
               !max_event_ts_seen_.compare_exchange_weak(old_max, ts_ns,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
            // retry
        }

        uint64_t bucket = bucketFor(ts_ns);

        // Avoid uint64_t underflow
        if (bucket < bucket_period_ns_) return;

        uint64_t cutoff = bucket - bucket_period_ns_;

        if (cutoff <= last_flushed_cutoff_.load(std::memory_order_relaxed)) {
            return;
        }

        // Coalesce: store max cutoff in pending
        uint64_t prev = pending_cutoff_.load(std::memory_order_relaxed);
        while (cutoff > prev &&
               !pending_cutoff_.compare_exchange_weak(prev, cutoff,
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
            // retry
        }

        // Try to acquire flush gate
        bool expected = false;
        if (!flush_in_progress_.compare_exchange_strong(expected, true,
                                                         std::memory_order_acq_rel)) {
            return; // Another thread is flushing; our cutoff is coalesced
        }

        // We own the flush. Read the coalesced cutoff.
        uint64_t flush_cutoff = pending_cutoff_.exchange(0, std::memory_order_acq_rel);

        if (flush_cutoff > last_flushed_cutoff_.load(std::memory_order_relaxed)) {
            executeFlush(flush_cutoff);
        }

        // Release gate
        flush_in_progress_.store(false, std::memory_order_release);

        // Check if new requests arrived during our flush
        uint64_t new_pending = pending_cutoff_.load(std::memory_order_acquire);
        uint64_t last = last_flushed_cutoff_.load(std::memory_order_relaxed);
        if (new_pending > 0 && new_pending > last) {
            onIncomingTimestamp(max_event_ts_seen_.load(std::memory_order_relaxed));
        }
    }

    // Force flush (for shutdown) - flushes everything
    void forceFlush() {
        // Flush all data including current bucket (used for shutdown)
        uint64_t ts = max_event_ts_seen_.load(std::memory_order_relaxed);
        if (ts == 0) return;
        uint64_t bucket = bucketFor(ts);
        // Use bucket + bucket_period_ns_ as cutoff to include current bucket
        if (bucket <= UINT64_MAX - bucket_period_ns_) {
            executeFlush(bucket + bucket_period_ns_);
        } else {
            executeFlush(UINT64_MAX);
        }
    }

    uint64_t lateArrivals() const {
        return late_arrivals_.load(std::memory_order_relaxed);
    }

    uint64_t flushCount() const {
        return flush_count_.load(std::memory_order_relaxed);
    }

    const std::string& measurement() const { return measurement_; }

private:
    void doAddSet(const TagSet& tags, const std::string& field,
                  const Value& value, uint64_t ts_ns, bool is_add) {
        if (bucket_period_ns_ == 0) return;

        uint64_t bucket = bucketFor(ts_ns);
        uint64_t last_cutoff = last_flushed_cutoff_.load(std::memory_order_relaxed);

        // Late event check
        if (bucket <= last_cutoff && last_cutoff > 0) {
            late_arrivals_.fetch_add(1, std::memory_order_relaxed);
            // Roll to current bucket: max(bucket(max_event_ts_seen), last_cutoff + P)
            uint64_t max_ts = max_event_ts_seen_.load(std::memory_order_relaxed);
            uint64_t current_bucket = bucketFor(max_ts);
            uint64_t target = current_bucket;
            if (target <= last_cutoff) {
                target = last_cutoff + bucket_period_ns_;
            }
            bucket = target;
        }

        size_t idx = shardIndex(tags);
        std::lock_guard<std::mutex> lock(shards_[idx]->mutex());
        if (is_add) {
            shards_[idx]->add(tags, field, value, bucket);
        } else {
            shards_[idx]->set(tags, field, value, bucket);
        }
    }

    uint64_t bucketFor(uint64_t ts_ns) const {
        return (ts_ns / bucket_period_ns_) * bucket_period_ns_;
    }

    size_t shardIndex(const TagSet& tags) const {
        return tags.hash() % shards_.size();
    }

    void executeFlush(uint64_t cutoff) {
        std::vector<Datapoint> all_datapoints;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard->mutex());
            auto extracted = shard->extract(cutoff, clear_on_flush_);
            all_datapoints.insert(all_datapoints.end(),
                                  std::make_move_iterator(extracted.begin()),
                                  std::make_move_iterator(extracted.end()));
        }

        if (all_datapoints.empty()) {
            last_flushed_cutoff_.store(cutoff, std::memory_order_release);
            return;
        }

        // Update last_flushed_cutoff
        last_flushed_cutoff_.store(cutoff, std::memory_order_release);
        flush_count_.fetch_add(1, std::memory_order_relaxed);

        // Get flush time for filename
        auto now = std::chrono::system_clock::now();
        auto flush_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();

        // Format off-lock
        std::string payload;
        std::string ext;
        if (format_ == OutputFormat::CSV) {
            payload = Formatter::formatCSV(measurement_, all_datapoints);
            ext = "csv";
        } else {
            payload = Formatter::formatInfluxDB(measurement_, all_datapoints);
            ext = "lp";
        }

        // Generate filename
        uint64_t seq = flush_sequence_.fetch_add(1, std::memory_order_relaxed);
        std::string filename = Formatter::generateFilename(
            output_dir_, file_prefix_, measurement_, ext, flush_ts_ns, seq);

        // Enqueue to disk writer
        DiskWriterTask task;
        task.payload = std::move(payload);
        task.filename = std::move(filename);
        writer_->enqueue(std::move(task));
    }

    std::string measurement_;
    std::string file_prefix_;
    uint64_t bucket_period_ns_;
    bool clear_on_flush_;
    OutputFormat format_;
    DiskWriter* writer_;
    std::string output_dir_;

    std::vector<std::unique_ptr<Shard>> shards_;

    // Flush gating
    std::atomic<uint64_t> pending_cutoff_{0};
    std::atomic<bool> flush_in_progress_{false};

    // Timestamp tracking
    std::atomic<uint64_t> max_event_ts_seen_{0};
    std::atomic<uint64_t> last_flushed_cutoff_{0};

    // Counters
    std::atomic<uint64_t> late_arrivals_{0};
    std::atomic<uint64_t> flush_count_{0};
    std::atomic<uint64_t> flush_sequence_;
};

} // namespace metrics

#endif // METRICS_HPP
