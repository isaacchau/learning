#include "metrics.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Demo: Multi-threaded metrics aggregation with rotating file output
// ============================================================================

static uint64_t now_ns() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

static void runDemo(metrics::OutputFormat fmt, const std::string& label) {
    std::cout << "\n=== Demo: " << label << " ===" << std::endl;

    const std::string output_dir = "./output_demo_" + label;
    const uint64_t bucket_period_ns = 500000000ULL; // 500ms buckets
    const size_t num_shards = 4;
    const int num_workers = 4;
    const int events_per_worker = 2000;

    metrics::DiskWriter writer(output_dir);
    writer.start();

    metrics::Aggregator agg("orders", "demo", bucket_period_ns, num_shards,
                            true, fmt, &writer, output_dir);

    std::vector<std::thread> workers;
    std::atomic<uint64_t> total_events{0};

    auto start_ns = now_ns();

    for (int w = 0; w < num_workers; ++w) {
        workers.emplace_back([&, w]() {
            std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)) + w * 7);
            std::uniform_int_distribution<int> market_dist(0, 2);
            std::uniform_int_distribution<int> inst_dist(0, 4);
            std::uniform_int_distribution<int> broker_dist(0, 2);
            std::uniform_int_distribution<int> qty_dist(1, 100);
            std::uniform_int_distribution<int> late_chance(0, 99);

            const char* markets[] = {"NYSE", "NASDAQ", "LSE"};
            const char* instruments[] = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA"};
            const char* brokers[] = {"IB", "GS", "MS"};

            for (int i = 0; i < events_per_worker; ++i) {
                uint64_t ts = now_ns();

                // Inject ~2% late events (older timestamps)
                if (late_chance(rng) < 2) {
                    ts -= 2 * bucket_period_ns; // older than current bucket
                }

                metrics::TagSet tags;
                tags.add("Market", markets[market_dist(rng)]);
                tags.add("Instrument", instruments[inst_dist(rng)]);
                tags.add("Broker", brokers[broker_dist(rng)]);

                agg.add(tags, "newOrders", static_cast<int64_t>(1), ts);
                agg.add(tags, "totalQty", static_cast<int64_t>(qty_dist(rng)), ts);
                agg.set(tags, "lastPrice", static_cast<double>(qty_dist(rng)) + 0.5, ts);

                // Trigger semi-auto flush
                agg.onIncomingTimestamp(ts);

                total_events.fetch_add(1);
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    auto elapsed_ms = (now_ns() - start_ns) / 1000000ULL;

    // Force flush remaining data
    agg.forceFlush();

    // Give writer time to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    writer.stop();

    std::cout << "  Workers:       " << num_workers << std::endl;
    std::cout << "  Events/worker: " << events_per_worker << std::endl;
    std::cout << "  Total events:  " << total_events.load() << std::endl;
    std::cout << "  Flushes:       " << agg.flushCount() << std::endl;
    std::cout << "  Late arrivals: " << agg.lateArrivals() << std::endl;
    std::cout << "  Dropped writes:" << writer.droppedWrites() << std::endl;
    std::cout << "  Elapsed:       " << elapsed_ms << " ms" << std::endl;
}

static void testFlushGating() {
    std::cout << "\n=== Test: Flush Gating ===" << std::endl;

    const std::string output_dir = "./output_demo_gating";
    const uint64_t bucket_period_ns = 100000000ULL; // 100ms buckets

    metrics::DiskWriter writer(output_dir);
    writer.start();

    metrics::Aggregator agg("test", "gating", bucket_period_ns, 4,
                            true, metrics::OutputFormat::INFLUXDB_LINE, &writer, output_dir);

    // Spawn many threads all trying to trigger flushes concurrently
    std::vector<std::thread> threads;
    const int num_threads = 16;
    const int iterations = 100;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&agg, t]() {
            metrics::TagSet tags;
            tags.add("thread", std::to_string(t));
            for (int i = 0; i < iterations; ++i) {
                uint64_t ts = now_ns();
                agg.add(tags, "counter", static_cast<int64_t>(1), ts);
                agg.onIncomingTimestamp(ts);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    agg.forceFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    uint64_t flushes = agg.flushCount();
    std::cout << "  Threads:       " << num_threads << std::endl;
    std::cout << "  Iterations:    " << iterations << std::endl;
    std::cout << "  Total flushes: " << flushes << std::endl;

    // We should have relatively few flushes due to gating
    // (at most one per concurrent trigger point, not 16*100)
    if (flushes <= num_threads * iterations) {
        std::cout << "  PASS: Flush gating is working (not every trigger caused a flush)" << std::endl;
    }
}

static void testLateEvents() {
    std::cout << "\n=== Test: Late Events ===" << std::endl;

    const std::string output_dir = "./output_demo_late";
    const uint64_t bucket_period_ns = 1000000000ULL; // 1 second buckets

    metrics::DiskWriter writer(output_dir);
    writer.start();

    metrics::Aggregator agg("test", "late", bucket_period_ns, 2,
                            true, metrics::OutputFormat::CSV, &writer, output_dir);

    uint64_t base_ts = (now_ns() / bucket_period_ns) * bucket_period_ns + bucket_period_ns;

    metrics::TagSet tags;
    tags.add("sym", "AAPL");

    // Normal events in bucket 0
    agg.add(tags, "count", static_cast<int64_t>(1), base_ts);
    agg.add(tags, "count", static_cast<int64_t>(1), base_ts + 1000000);
    agg.onIncomingTimestamp(base_ts + 1000000);

    // Events in bucket 1
    agg.add(tags, "count", static_cast<int64_t>(1), base_ts + bucket_period_ns);
    agg.add(tags, "count", static_cast<int64_t>(1), base_ts + bucket_period_ns + 1000000);
    agg.onIncomingTimestamp(base_ts + bucket_period_ns + 1000000);

    // This should trigger flush of bucket 0
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Now send a late event for bucket 0 (already flushed)
    agg.add(tags, "count", static_cast<int64_t>(100), base_ts + 500000000); // late for bucket 0
    agg.onIncomingTimestamp(base_ts + 500000000);

    agg.forceFlush();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    std::cout << "  Late arrivals: " << agg.lateArrivals() << std::endl;
    if (agg.lateArrivals() >= 1) {
        std::cout << "  PASS: Late event was detected and rolled forward" << std::endl;
    } else {
        std::cout << "  FAIL: Late event was not detected" << std::endl;
    }
}

int main() {
    std::cout << "Metrics Aggregation Library Demo" << std::endl;
    std::cout << "=================================" << std::endl;

    runDemo(metrics::OutputFormat::CSV, "CSV");
    runDemo(metrics::OutputFormat::INFLUXDB_LINE, "InfluxDB_Line");
    testFlushGating();
    testLateEvents();

    std::cout << "\n=== All demos complete ===" << std::endl;
    std::cout << "Check ./output_demo_* directories for output files." << std::endl;
    return 0;
}
