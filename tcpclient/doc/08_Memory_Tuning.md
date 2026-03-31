# Memory Usage Estimation and Tuning Guide

## Overview

This document explains how to estimate memory usage for the `msg_client` at various throughput levels and how to tune memory settings.

## Memory Components

The total memory usage consists of:

```
Total Memory = Memory Pool + Message Queues + Connection State + Program Overhead
```

### 1. Memory Pool (Primary Consumer)

The memory pool uses **size-class allocation** with 8 different buffer sizes:

| Size Class | Block Size | Pre-allocated | Max Limit | Max Memory |
|------------|-----------|---------------|-----------|------------|
| 0 | 64 B | 128 | 4,096 | 0.2 MB |
| 1 | 256 B | 128 | 4,096 | 1.0 MB |
| 2 | 1 KB | 128 | 4,096 | 4.0 MB |
| 3 | 4 KB | 256 | 8,192 | 32.0 MB |
| 4 | 16 KB | 256 | 4,096 | 64.0 MB |
| 5 | **64 KB** | **512** | **8,192** | **512.0 MB** |
| 6 | 128 KB | 256 | 4,096 | 512.0 MB |
| 7 | 256 KB | 128 | 2,048 | 512.0 MB |

**Note:** The 64KB class (class 5) is used for receive buffers by default.

### 2. Message Queues

```
Raw Queue Memory = raw_queue_size × sizeof(RawMessage)
Decoded Queue Memory = num_workers × decoded_queue_size × sizeof(SubMessage)

Example (default):
- Raw: 16,384 × 48 bytes ≈ 0.8 MB
- Decoded (4 workers): 4 × 16,384 × 48 bytes ≈ 3.2 MB
```

### 3. Connection State

Per-connection overhead is minimal (~1 KB per connection):
```
Connection State = num_connections × (socket + buffers + stats)
                 = 64 connections × ~1 KB ≈ 64 KB
```

### 4. Program Overhead

- Code and libraries: ~20-30 MB
- Thread stacks: num_threads × 8 MB (default stack size)
- Other runtime overhead: ~10 MB

---

## Memory Estimation Formula

### Step 1: Calculate Buffers in Flight

The key insight is that buffers accumulate in the pipeline:

```
Buffers In Flight = Message Rate × Pipeline Latency

Where:
- Message Rate = msgs/second
- Pipeline Latency = time from recv() to handler completion
                    = IO latency + Queue wait + Decode + Worker processing
                    ≈ 5-100ms depending on load and processing time
```

### Step 2: Calculate Receive Buffer Memory

```
Recv Buffer Memory = Buffers In Flight × Recv Buffer Size
                   = (Msg Rate × Pipeline Latency) × 64KB
```

### Step 3: Calculate Queue Memory

```
Raw Queue Memory = raw_queue_size × sizeof(void*) × 2  # Ring buffer slots
Decoded Queue Memory = workers × decoded_queue_size × sizeof(void*) × 2
```

### Step 4: Total Estimated Memory

```
Total RES ≈ Recv Buffer Memory + Queue Memory + Pool Overhead + 50MB (program)
```

---

## Calculation Examples

### Example 1: Low Throughput (1,000 msg/s)

```python
# Inputs
msg_rate = 1_000           # msgs/second
pipeline_latency = 0.010   # 10ms
recv_buf_size = 65536      # 64KB
raw_queue_size = 16384
decoded_queue_size = 16384
num_workers = 4

# Calculation
buffers_in_flight = msg_rate × pipeline_latency
                  = 1,000 × 0.010 = 10 buffers

recv_buffer_memory = 10 × 64KB = 640 KB

raw_queue_memory = 16384 × 48 bytes ≈ 0.8 MB
decoded_queue_memory = 4 × 16384 × 48 bytes ≈ 3.2 MB

program_overhead = 50 MB

Total Estimated RES ≈ 0.6 + 0.8 + 3.2 + 50 ≈ 55 MB
```

### Example 2: Medium Throughput (50,000 msg/s)

```python
# Inputs
msg_rate = 50_000          # msgs/second
pipeline_latency = 0.020   # 20ms (more queueing)

# Calculation
buffers_in_flight = 50,000 × 0.020 = 1,000 buffers
recv_buffer_memory = 1,000 × 64KB = 64 MB

queue_memory ≈ 4 MB
program_overhead = 50 MB

Total Estimated RES ≈ 64 + 4 + 50 ≈ 118 MB
```

### Example 3: High Throughput (140,000 msg/s - Your Test)

```python
# Inputs
msg_rate = 140_000         # msgs/second (70k × 2 connections)
pipeline_latency = 0.050   # 50ms (burst accumulation)

# Calculation
buffers_in_flight = 140,000 × 0.050 = 7,000 buffers
recv_buffer_memory = 7,000 × 64KB = 448 MB

# Plus other pools, queues, overhead
Total Estimated RES ≈ 448 + 50 + 20 ≈ 518 MB

# Your actual: 350 MB RES (some buffers not at peak simultaneously)
```

---

## Quick Reference Table

| Msg Rate | Est. Pipeline Latency | Buffers In Flight | Est. Memory |
|----------|----------------------|-------------------|-------------|
| 1,000/s | 10ms | 10 | ~55 MB |
| 10,000/s | 15ms | 150 | ~65 MB |
| 50,000/s | 20ms | 1,000 | ~120 MB |
| 100,000/s | 30ms | 3,000 | ~250 MB |
| 200,000/s | 40ms | 8,000 | ~550 MB |
| 500,000/s | 50ms | 25,000 | ~1.6 GB |

---

## Tuning Memory Usage

### Option 1: Reduce Receive Buffer Size

If you have small messages (< 4KB), reduce the recv buffer:

```bash
# Reduce from 64KB to 32KB (halves memory usage)
export APP_TCP_RECV_BUFFER_SIZE="32768"

# Or even 16KB for very small messages
export APP_TCP_RECV_BUFFER_SIZE="16384"
```

**Trade-off:** May require more syscalls if messages don't fit.

### Option 2: Reduce Queue Sizes

```bash
# Default
./msg_client --raw-queue 16384 --dec-queue 16384

# Reduced (less burst capacity, less memory)
./msg_client --raw-queue 8192 --dec-queue 8192

# Minimal (may drop more under burst)
./msg_client --raw-queue 4096 --dec-queue 4096
```

**Trade-off:** Higher chance of message drops during bursts.

### Option 3: Reduce Memory Pool Limits

Edit `shared_ptr_pool.h`:

```cpp
// Default (high throughput)
{65536, 512, 1024, 8192}   // 512MB max for 64KB class

// Reduced (memory constrained)
{65536, 256, 512, 4096}    // 256MB max for 64KB class

// Minimal (low throughput only)
{65536, 128, 256, 2048}    // 128MB max for 64KB class
```

**Trade-off:** Risk of "Failed to allocate" errors under high load.

### Option 4: Reduce Worker Threads

Fewer workers = smaller decoded queue total:

```bash
# Default (2 workers)
./msg_client --workers 2    # 2 × dec_queue memory

# Single worker (low latency, less memory)
./msg_client --workers 1    # Half the decoded queue memory
```

**Trade-off:** Less parallelism for processing.

---

## Monitoring Memory Usage

### Check Pool Statistics

Enable pool stats logging:

```bash
./msg_client ... --pool-stats-interval 10
```

Watch for:
- `in_use` approaching `allocated` (near limit)
- `free_count` staying low (pool working hard)

### Check System Memory

```bash
# While client is running
watch -n 1 'cat /proc/$(pgrep msg_client)/status | grep -E "VmRSS|VmSize"'

# Or use ps
ps -o pid,comm,rss,vsz,size -p $(pgrep msg_client)
```

### Log Analysis

Watch for these warning signs:

```
# Memory pool exhausted - need to increase limits
[ERROR] Failed to allocate receive buffer - memory pool exhausted

# Queues full - may need larger queues or more workers
[WARN] Message dropped: raw queue full
[WARN] Message dropped: worker queue full
```

---

## Recommended Configurations

### Low Memory Environment (8GB RAM)

```bash
export APP_TCP_RECV_BUFFER_SIZE="32768"
./msg_client \
    --raw-queue 8192 \
    --dec-queue 8192 \
    --workers 2 \
    ...
# Expected: ~100-150 MB at 50k msg/s
```

### Standard Production (64GB RAM)

```bash
# Use defaults
./msg_client \
    --raw-queue 32768 \
    --dec-queue 32768 \
    --workers 4 \
    ...
# Expected: ~200-400 MB at 100k msg/s
```

### High Throughput (64GB+ RAM)

```bash
export APP_TCP_RECV_BUFFER_SIZE="65536"
./msg_client \
    --raw-queue 65536 \
    --dec-queue 65536 \
    --workers 8 \
    ...
# Expected: ~500MB-1GB at 200k+ msg/s
```

---

## Summary

**Key Formulas:**
```
Buffers In Flight = Message Rate × Pipeline Latency
Recv Memory = Buffers In Flight × 64KB
Total Memory ≈ Recv Memory + Queue Memory + 50MB
```

**Rule of Thumb:**
- 1,000 msg/s ≈ 55 MB
- 10,000 msg/s ≈ 65 MB  
- 100,000 msg/s ≈ 250 MB
- 200,000 msg/s ≈ 550 MB

**Most Important:** The 64KB receive buffer class dominates memory usage. To reduce memory, either:
1. Reduce `APP_TCP_RECV_BUFFER_SIZE` (for small messages)
2. Reduce max allocations in pool config (risk of drops)
3. Accept higher memory for high throughput (recommended for production)
