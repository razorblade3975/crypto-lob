# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a C++ cryptocurrency market data provider system designed for high-frequency trading (HFT) applications. The system focuses on ultra-low latency, high throughput, and predictable performance for processing real-time market data from multiple cryptocurrency exchanges.

## High-Level Architecture

The system is architected as a single-process, multi-threaded application with specialized components:

### Core Components

1. **Exchange Connectors** - Direct WebSocket interfaces to crypto exchanges (Binance, OKX, KuCoin)
   - Handle exchange-specific authentication and connection protocols
   - Manage WebSocket lifecycle and heartbeat requirements
   - Each exchange has unique requirements (e.g., KuCoin's two-step auth, Binance ping/pong timing)

2. **Data Normalization & Parsing** - High-speed JSON parsing using simdjson
   - Converts raw JSON messages to unified C++ binary representation
   - Critical latency bottleneck requiring SIMD optimization
   - Parser object reuse pattern essential for performance

3. **Order Book Engine** - Core processing unit maintaining full-depth limit order books
   - Reconstructs and maintains in-memory LOB for each instrument
   - Generates business events (top-of-book changes, trades)
   - Complex state machine for book synchronization (UNINITIALIZED → SYNCING_SNAPSHOT → SYNCING_BUFFER → LIVE)

4. **IPC Publisher** - Outbound interface using shared memory for lock-free communication
   - Serializes top-of-book data to shared memory ring buffer
   - Single-producer model for optimal performance

### Threading Model

- **Network I/O Threads** - Dedicated per-exchange threads running Asio event loops
- **LOB Worker Threads** - Process normalized data, typically sharded by instrument
- **IPC Publisher Thread** - Single thread handling shared memory writes
- **Control/Management Thread** - Non-latency-critical operations

### Performance Principles

- **Zero-Copy Data Flow** - Data processed in-place, passed by reference/pointer
- **Mechanical Sympathy** - Design aware of CPU cache hierarchy and memory layout
- **Compile-Time Optimization** - Leverage constexpr, templates, compiler hints
- **Data-Oriented Design** - Focus on data transformations over abstractions

## Implemented Core Infrastructure

### Memory Management System (src/core/memory_pool.hpp)

**Production-Ready Lock-Free Memory Pool** with enterprise-grade optimizations:

- **Thread-Local Caching**: Ultra-fast allocation/deallocation with zero atomics on hot path
- **128-bit ABA-Protected Tagged Pointers**: Prevents race conditions in lock-free operations
- **Huge Page Support**: 2MB/1GB pages for reduced TLB misses and predictable latency
- **NUMA-Aware Architecture**: Per-socket memory pools for multi-socket servers
- **Configurable Pre-faulting**: Eliminates page faults during market data bursts
- **Branch Prediction Hints**: [[likely]]/[[unlikely]] attributes for optimal CPU pipeline utilization
- **Software Prefetching**: __builtin_prefetch() to hide memory latency
- **Cache-Line Alignment**: Prevents false sharing between threads

**Key Features:**
- **O(1) allocation/deallocation** in common case (thread-local cache hit)
- **Lock-free global pool** with ABA protection for cache refills
- **Graceful shutdown** handling to prevent use-after-free
- **Comprehensive statistics** for monitoring and tuning
- **Configurable depletion policies** (throw/terminate/resize)

### Fixed-Point Price System (src/core/price.hpp)

**Adaptive Precision Price Representation** for cryptocurrency price ranges:

- **Dynamic Scaling**: 10^8 precision for high-value assets (Bitcoin), 10^18 for micro-cap tokens
- **128-bit Integer Storage**: Prevents overflow for extreme price ranges
- **Zero Floating-Point Arithmetic**: Eliminates precision errors in financial calculations
- **Optimized String Conversion**: Fast parsing from/to exchange JSON formats
- **Compile-Time Operations**: constexpr functions for zero-runtime-cost calculations

**Price Range Support:**
- **High-value**: $1,000,000+ (Bitcoin) with 8 decimal places
- **Micro-cap**: $0.000000001+ (meme coins) with 18 decimal places
- **Specialized constructors**: from_satoshi(), from_wei() for blockchain units

### Cache Alignment Infrastructure (src/core/cache.hpp)

**Centralized Cache Management** for optimal performance:

- **CACHELINE_SIZE constant**: Centralized 64-byte alignment to prevent magic numbers
- **Alignment utilities**: `align_to_cacheline()`, `is_cacheline_aligned()` with bit-math optimization
- **Template wrappers**: `aligned_64<T>` for alignment, `sized_64<T>` for full cache-line sizing
- **Prefetch support**: Software prefetch hints with temporal locality control
- **Branch prediction**: `likely`/`unlikely` macros with header conflict protection
- **Memory barriers**: Portable compiler fence using `std::atomic_signal_fence`

### Wait-Free Inter-Thread Communication (src/core/spsc_ring.hpp)

**Production-Ready SPSC Ring Buffer** using Disruptor pattern:

- **Single-producer/single-consumer**: Wait-free operation with no locks
- **Disruptor sequence numbers**: Per-slot sequences prevent ABA issues and wraparound bugs
- **Cache-line aligned slots**: 64-byte slots prevent false sharing
- **Power-of-two capacity**: Fast modulo via bit-mask operations
- **Aligned memory allocation**: Cache-line aligned storage with proper cleanup
- **Monitoring support**: Accurate `size()`, `fill_ratio()` for back-pressure detection

**Key Features:**
- **One atomic load per operation** on hot path (producer and consumer)
- **Sequence-based state management**: Slots cycle between free/written states
- **64-bit counter safety**: Supports ~5×10^19 operations before wraparound
- **Ultra-low latency**: Optimized for sub-microsecond operation times

### Technology Stack

- **Language**: C++20 (leveraging coroutines, constexpr, templates, concepts)
- **Build System**: CMake with Conan package management
- **WebSocket Library**: Boost.Beast with Asio (planned)
- **JSON Parsing**: simdjson for hot path, RapidJSON for outbound messages
- **NUMA Support**: libnuma for multi-socket server optimization
- **Configuration**: TOML++ for human-readable config files
- **Platform**: Linux (utilizing epoll, huge pages, NUMA APIs)

## Critical Implementation Notes

### Memory Pool Usage Patterns

**Correct Usage:**
```cpp
// High-level API (automatic construction/destruction)
MemoryPool<Order> pool;
auto order = pool.construct(order_id, price, quantity);
pool.destroy(order);

// RAII wrapper (recommended)
auto order_ptr = make_pooled(pool, order_id, price, quantity);
```

**Advanced Usage:**
```cpp
// Raw API (manual lifetime management)
void* raw_memory = pool.allocate_raw();
auto* order = new (raw_memory) Order(order_id, price, quantity);
order->~Order();
pool.deallocate_raw(raw_memory);
```

### Order Book Synchronization

Must follow precise algorithm for differential feed reconstruction:
1. Connect to WebSocket stream and buffer messages
2. Fetch REST snapshot
3. Discard buffered messages older than snapshot
4. Find bridging message where U <= lastUpdateId + 1 <= u
5. Initialize book with snapshot, apply buffered updates
6. Process real-time updates

### Exchange-Specific Requirements

- **Binance**: 20-second ping interval, 1-minute pong response requirement
- **KuCoin**: Two-step auth (REST token → WebSocket), 24-hour token expiry
- **OKX**: Distinct public/private channels, specific subscription format

## Development Commands

### Build System
```bash
# Setup build environment
mkdir build && cd build
conan install .. --build=missing
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build with optimizations
make -j$(nproc)

# Run tests
ctest -V

# Run benchmarks
./crypto-lob-benchmarks
```

### Configuration
- **Main config**: `config/system.toml`
- **Exchange settings**: `config/exchanges.toml`
- **Instrument definitions**: `config/instruments.toml`

### Performance Tuning

**System-level optimizations:**
- Enable huge pages: `echo 1024 > /proc/sys/vm/nr_hugepages`
- CPU isolation: `isolcpus=1-7` in kernel parameters
- IRQ affinity: Pin network interrupts to specific CPUs
- NUMA policy: `numactl --cpubind=0 --membind=0 ./crypto-lob`

**Application-level tuning:**
- Thread-local cache size: Adjust based on allocation patterns
- Batch sizes: Optimize for your specific workload
- Memory pool capacity: Size based on peak instrument count

## Architecture Decisions

### Lock-Free Design Philosophy
- **Zero-malloc hot path**: All critical allocations use custom pools
- **ABA protection**: 128-bit tagged pointers prevent race conditions
- **Thread-local optimization**: Common case requires no atomic operations
- **Graceful degradation**: Fallback paths for exceptional conditions

### NUMA Considerations
- **Per-socket pools**: Eliminate cross-socket memory access
- **Thread affinity**: Pin threads to specific NUMA nodes
- **Memory locality**: Allocate data structures on local NUMA nodes

### Error Handling Strategy
- **Fail-fast philosophy**: Terminate on unrecoverable errors
- **Graceful shutdown**: Proper cleanup sequence for worker threads
- **Resource safety**: RAII patterns throughout the codebase

## Testing Strategy

### Unit Tests (tests/unit/)
- Memory pool correctness under concurrent access
- Price arithmetic edge cases and precision validation
- Configuration parsing and validation

### Integration Tests (tests/integration/)
- Full pipeline simulation with mock exchange data
- Network failure and reconnection scenarios
- Performance regression detection

### Benchmarks (tests/benchmarks/)
- Memory allocation/deallocation latency
- JSON parsing throughput
- Order book update performance

## Future Implementation Priority

### Phase 1: Core Infrastructure ✅
- [x] Memory pool allocator with thread-local caching
- [x] Fixed-point price representation
- [x] Basic build system and project structure

### Phase 2: Data Structures (In Progress)
- [x] Cache alignment infrastructure with centralized constants
- [x] Wait-free SPSC ring buffer using Disruptor pattern
- [ ] Flat hash map for O(1) order ID lookups
- [ ] Dense adaptive arrays with tick bucketing for price levels
- [ ] Intrusive FIFO order lists with cache-line optimization
- [ ] Complete order book assembly

### Phase 3: Networking Layer
- [ ] WebSocket client with Boost.Beast
- [ ] Exchange-specific connectors
- [ ] Connection management and reconnection logic

### Phase 4: Order Book Engine
- [ ] High-performance order book implementation
- [ ] Differential feed synchronization
- [ ] Market event generation

### Phase 5: Integration
- [ ] IPC publisher with shared memory
- [ ] End-to-end performance testing
- [ ] Production monitoring and observability

## Memories

- Use the system design plan from @Crypto Market Data Provider_.md as the primary reference point for implementation
- Please use C++20 in your code. make sure it has clean formatting. For configuration files please use toml and toml format with toml++ module
- The codebase is targeting Linux hosts only - no Windows or macOS compatibility needed
- Focus on ultra-low latency and deterministic performance - every nanosecond matters
- Memory pool implementation has been thoroughly reviewed and hardened against production edge cases