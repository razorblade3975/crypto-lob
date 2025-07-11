# Memory Pool Benchmarking Suite

This directory contains comprehensive benchmarks comparing our custom `MemoryPool` implementation against standard allocators and other high-performance alternatives.

## Benchmark Design

### 1. **Comparison Benchmark** (`memory_pool_comparison_benchmark.cpp`)

Uses Google Benchmark framework to compare:
- `std::allocator` (baseline)
- Our `MemoryPool` implementation
- `Boost.Pool` (boost::object_pool)

#### Test Scenarios:

1. **Sequential Allocation/Deallocation**
   - Single alloc/dealloc in tight loop
   - Measures: Hot path latency
   - Simulates: Simple object lifecycle

2. **Bulk Allocation**
   - Allocate N objects, then deallocate all
   - Measures: Batch operation performance
   - Simulates: Market data burst scenarios

3. **Random Pattern**
   - 50% allocated pool, random alloc/dealloc
   - Measures: Realistic mixed workload
   - Simulates: Order book update patterns

4. **Multithreaded Stress**
   - Concurrent allocations from N threads
   - Measures: Scalability and contention
   - Simulates: Multi-threaded LOB processing

5. **Cache Behavior**
   - Allocate many, traverse with stride
   - Measures: Memory locality benefits
   - Simulates: Order book traversal

#### Object Sizes Tested:
- **Small (32B)**: Typical message header
- **Medium (136B)**: Order structure
- **Large (640B)**: Complex market data

### 2. **Latency Analysis Benchmark** (`memory_pool_latency_benchmark.cpp`)

Focuses on HFT-critical latency measurements:

#### Features:
- **RDTSC-based timing**: CPU cycle accurate measurements
- **Percentile analysis**: 50th, 90th, 99th, 99.9th, 99.99th percentiles
- **Warm-up phases**: Eliminates first-allocation overhead
- **CPU affinity**: Pins to specific core for consistency

#### Additional Allocators:
- **jemalloc**: Facebook's scalable allocator
- Custom allocator wrappers for fair comparison

#### Special Tests:
1. **Thread-Local Cache Effectiveness**
   - Tests different cache sizes (0, 16, 64, 256)
   - Measures impact of cache configuration

2. **Cache Miss Analysis**
   - Compares memory access patterns
   - Quantifies locality benefits

## Building the Benchmarks

```bash
# From build directory
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake ..
ninja memory_pool_comparison_benchmark memory_pool_latency_benchmark
```

## Running the Benchmarks

### Quick Run:
```bash
# Basic comparison benchmark
./tests/benchmarks/memory_pool_comparison_benchmark

# Detailed latency analysis (requires Linux)
./tests/benchmarks/memory_pool_latency_benchmark
```

### Full Analysis:
```bash
# Run comparison benchmark with JSON output
./tests/benchmarks/memory_pool_comparison_benchmark \
    --benchmark_format=json \
    --benchmark_out=results.json \
    --benchmark_repetitions=5

# Analyze results
python3 scripts/analyze_memory_benchmarks.py results.json output/

# View generated reports
ls output/
# - sequential_performance.png
# - multithreaded_scaling.png
# - performance_heatmap.png
# - latency_distribution.png
# - summary_report.md
```

### HFT Production Setup:
```bash
# Disable CPU frequency scaling
sudo cpupower frequency-set -g performance

# Run with elevated priority
sudo nice -n -20 ./tests/benchmarks/memory_pool_latency_benchmark

# With huge pages enabled
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
./tests/benchmarks/memory_pool_latency_benchmark
```

## Expected Results

Based on the design, MemoryPool should show:

1. **Lower latency** (typically 5-20ns vs 50-100ns for std::allocator)
2. **Better percentiles** (more predictable, less tail latency)
3. **Superior scaling** (thread-local caches reduce contention)
4. **Cache benefits** (contiguous memory improves locality)

### Key Metrics to Watch:

- **99.9th percentile latency**: Critical for HFT
- **Multithreaded throughput**: Should scale linearly
- **Cache miss rate**: Lower is better
- **Memory fragmentation**: Not directly measured but implied

## Interpreting Results

### Good Signs:
- MemoryPool 99th percentile < 50ns
- Linear scaling up to core count
- Consistent performance across object sizes
- Low standard deviation

### Red Flags:
- High tail latency (99.99th > 1000ns)
- Performance degradation under load
- Thread contention (non-linear scaling)
- Cache thrashing patterns

## Customization

### Tuning Parameters:
```cpp
// Adjust in benchmarks for testing
CacheConfig config{
    .cache_size = 64,        // Thread-local cache size
    .batch_size = 32,        // Batch transfer size
    .use_huge_pages = true,  // Enable 2MB pages
    .prefault_pages = true   // Pre-fault memory
};
```

### Adding New Tests:
1. Follow naming convention: `BM_TestType_AllocatorName`
2. Use `REGISTER_BENCHMARKS` macro for all object sizes
3. Consider both latency and throughput metrics

## Troubleshooting

### Linux Permissions:
```bash
# For huge pages
sudo sysctl -w vm.nr_hugepages=1024

# For CPU affinity
sudo setcap cap_sys_nice+ep ./memory_pool_latency_benchmark
```

### macOS Limitations:
- RDTSC timing may be less accurate
- No huge page support
- Use Docker container for Linux features

### High Variance:
- Check for thermal throttling
- Disable turbo boost for consistency
- Use performance governor
- Isolate benchmark CPU core