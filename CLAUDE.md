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

3. **Order Book Engine** - Core processing unit maintaining price-level order books (L2 data)
   - Processes aggregated price levels from exchange feeds (no individual order tracking)
   - Maintains in-memory LOB with price → aggregate quantity mapping
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

**Critical Design Fix (July 2025):**
- Fixed fundamental thread-local cache bug where each method created independent caches
- Implemented intrusive linked list for proper cache management
- Exactly one cache per thread per pool instance with automatic cleanup
- Thread-local vector ensures proper destructor calls at thread exit
- Added `flush_thread_cache()` for testing and graceful shutdown

**Shutdown Requirements:**
- Pool MUST outlive all threads that use it
- Join all worker threads before destroying the pool
- Failing to follow this order causes use-after-free errors

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

### Lock-Free Order Book Side (src/orderbook/book_side.hpp, src/orderbook/price_level.hpp)

**High-Performance Order Book Side Implementation** with dual data structures:

- **Intrusive RB-Tree**: Boost.Intrusive set for ordered price traversal
- **Hash Map Index**: Boost unordered_flat_map for O(1) price lookups
- **Memory Pool Integration**: All price level nodes allocated from thread-local pools
- **Top-N Tracking**: Efficient tracking of top price levels for market data feeds
- **Cache-Aligned Nodes**: 128-byte PriceLevelNode with hot data in first 32 bytes
- **Subtree Size Tracking**: Prepared for O(log n) rank queries (future feature)

**Design Highlights:**
- **Dual indexing**: Tree for ordering, hash for fast lookup
- **Zero allocation on updates**: Only allocate for new price levels
- **Atomic-free operations**: No atomics needed for single-threaded book side
- **Iterator stability**: Pointers remain valid across non-destructive updates
- **Comprehensive testing**: Includes fuzz testing with millions of operations

### Technology Stack

- **Language**: C++20 (leveraging coroutines, constexpr, templates, concepts)
- **Compiler**: Clang 17+ (see Compiler Choice section below)
- **Build System**: CMake 3.20+ with Conan 2.0+ package management
  - Migrated to Conan 2 for better Clang 17 support and modern CMake integration
- **WebSocket Library**: Boost.Beast with Asio (planned)
- **JSON Parsing**: simdjson for hot path, RapidJSON for outbound messages
- **NUMA Support**: libnuma for multi-socket server optimization
- **Configuration**: TOML++ for human-readable config files
- **Platform**: Linux (utilizing epoll, huge pages, NUMA APIs)

## Critical Implementation Notes

### libc++ Linking Considerations

When linking against system libraries (e.g., OpenSSL, libcurl) that were built with libstdc++:
- You may need to explicitly link `-lc++abi` to resolve ABI symbols
- Consider static linking where possible to avoid runtime conflicts
- Use `ldd` to verify all dependencies use consistent C++ runtime

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

### Docker Development Environment

The project includes Docker support to provide a consistent Linux development environment, especially useful for macOS developers who need access to Linux-specific features like huge pages and NUMA APIs.

**Key Benefits:**
- **Linux-specific features on macOS**: Access to huge pages, NUMA APIs, and `libnuma-dev`
- **Consistent build environment**: Ubuntu 22.04 base ensures identical toolchains across all developers
- **Pre-configured dependencies**: Conan, CMake, and all required libraries pre-installed
- **Performance tools**: `perf`, `valgrind`, `strace` included for profiling

**Docker Workflow:**
```bash
# Build and start the development container
docker compose up -d crypto-lob-dev

# Enter the container for development
docker exec -it crypto-lob-dev bash

# Inside container: build with Linux optimizations
cd /workspace
conan install . --output-folder=build --build=missing \
      -s compiler=clang -s compiler.version=17 \
      -s compiler.libcxx=libc++ -s compiler.cppstd=20
cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake ..
ninja -j$(nproc)  # Using Ninja for faster builds

# Run tests inside container
ctest -V

# Run benchmarks with Linux performance features
./crypto-lob-benchmarks
```

**Container Features:**
- **Huge pages support**: `/dev/hugepages` mounted for 2MB page access
- **Privileged mode**: Enables performance features and system tuning
- **Remote debugging**: Port 2345 exposed for GDB remote debugging
- **Build caching**: Anonymous volume preserves build artifacts between sessions
- **Source mounting**: Local source code mounted at `/workspace` for live editing

**Production-like testing:**
```bash
# Run production container (no dev tools)
docker-compose up crypto-lob-prod
```

### Native Build System (Linux)
```bash
# Setup build environment (from project root)
conan install . --output-folder=build --build=missing \
      -s compiler=clang -s compiler.version=17 \
      -s compiler.libcxx=libc++ -s compiler.cppstd=20

cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake ..

# Build with optimizations (Ninja is faster than Make)
ninja -j$(nproc)

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

### Compiler Choice: Clang 17+

**Why Clang for HFT Applications:**

1. **Superior C++20 Support**
   - Full support for `std::hardware_destructive_interference_size` (critical for cache-aware programming)
   - Complete coroutines implementation for async I/O
   - Better concepts support and error messages
   - Earlier adoption of latest C++ features

2. **Performance Advantages**
   - **Aggressive inlining**: Better inter-procedural optimization for template-heavy code
   - **Superior LTO**: Link-Time Optimization produces tighter, faster binaries
   - **Vectorization**: Excellent auto-vectorization for SIMD operations
   - **Branch prediction**: Better static branch prediction analysis

3. **Development Experience**
   - **Faster compilation**: 20-30% faster than GCC for template-heavy code
   - **Better diagnostics**: Clear, actionable error messages for template metaprogramming
   - **Superior sanitizers**: ThreadSanitizer and AddressSanitizer integration
   - **Consistent behavior**: Less compiler-specific quirks than GCC

4. **HFT-Specific Benefits**
   - **Predictable codegen**: More consistent assembly output across optimization levels
   - **Better profile-guided optimization**: Critical for hot path tuning
   - **Strong static analysis**: Catches more potential race conditions at compile time
   - **Excellent `__builtin` support**: For prefetching, branch hints, etc.

**Compiler Configuration:**
```bash
# Required flags for optimal HFT performance
-std=c++20              # Full C++20 features
-stdlib=libc++          # Clang's optimized standard library
-O3                     # Maximum optimization
-march=native           # CPU-specific optimizations
-flto=thin              # Fast link-time optimization
-fno-exceptions         # Disable exceptions on hot paths (optional)
```

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
- Book side operations: insertion, updates, deletions
- Tree/hash coherence validation with fuzz testing
- Top-N tracking and kth-level queries
- Iterator stability across modifications

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

### Phase 2: Data Structures ✅
- [x] Cache alignment infrastructure with centralized constants
- [x] Wait-free SPSC ring buffer using Disruptor pattern
- [x] Thread-safe memory pool with proper thread-local cache management
- [x] Lock-free order book side implementation with intrusive RB-tree
- [x] Boost unordered_flat_map for O(1) price lookups
- [x] Price level nodes with cache-aligned layout
- [ ] Complete order book assembly (bid/ask sides integration)
- [ ] Dense adaptive arrays with tick bucketing (future optimization)

### Phase 3A: Complete Order Book Assembly (Immediate Priority)
- [ ] Full Order Book implementation combining bid/ask sides
- [ ] Exchange message types with standardized formats
- [ ] Comprehensive unit tests for order book functionality

### Phase 3B: Exchange Data Parsing (Week 1-2)
- [ ] JSON message parser using simdjson for high performance
- [ ] Exchange-specific parsers for format differences
- [ ] Parser tests with real exchange message samples

### Phase 3C: WebSocket Infrastructure (Week 2-3)
- [ ] WebSocket client implementation using Boost.Beast
- [ ] Connection manager with automatic reconnection and heartbeat handling
- [ ] Exchange-specific connectors for WebSocket protocols

### Phase 3D: Order Book Engine Integration (Week 3-4)
- [ ] Order Book Manager to coordinate multiple symbols
- [ ] Synchronization Engine for snapshot + delta reconciliation
- [ ] Feed Handler as main processing pipeline

### Phase 4: Configuration & Deployment (Week 4)
- [ ] TOML-based configuration system for exchanges and system settings
- [ ] Logging and monitoring for performance and health tracking
- [ ] Integration testing with end-to-end scenarios

### Phase 5: Production Features
- [ ] IPC publisher with shared memory for downstream consumers
- [ ] Performance optimization and regression testing
- [ ] Production monitoring and observability tools

## Implementation Timeline & Success Criteria

### Estimated Timeline
- **Week 1**: Complete OrderBook class + basic parsing infrastructure
- **Week 2**: WebSocket infrastructure + exchange connectors implementation
- **Week 3**: Synchronization engine + feed handler integration
- **Week 4**: Configuration system, monitoring, and integration testing

### Success Criteria
1. **Functional Requirements**:
   - Successfully maintain real-time order books for all 6 target exchanges
   - Handle connection failures and sequence gaps gracefully
   - Support 50+ symbols per exchange simultaneously

2. **Performance Requirements**:
   - <100μs end-to-end latency for market data updates
   - Deterministic memory allocation patterns
   - Zero-copy data processing where possible

3. **Reliability Requirements**:
   - Automatic reconnection with exponential backoff
   - Sequence gap detection and recovery
   - Graceful degradation under load

### Key Implementation Details

#### Order Book Assembly Components:
- Full Order Book combining bid/ask sides with sequence validation
- Exchange message types with standardized snapshot/delta formats
- State machine for synchronization (UNINITIALIZED → SYNCING → LIVE)
- Cross-spread validation and thread-safe operations

#### Exchange Data Parsing:
- High-performance JSON parsing using simdjson with object reuse
- Exchange-specific parsers for format differences (Binance, KuCoin, OKX, etc.)
- Graceful handling of malformed messages

#### WebSocket Infrastructure:
- Boost.Beast-based WebSocket client with connection pooling
- Exchange-specific heartbeat requirements (Binance 20s ping, KuCoin 18s)
- Automatic reconnection and error handling

#### Integration Pipeline:
- Order Book Manager for multi-symbol coordination
- Synchronization Engine for complex snapshot + delta reconciliation  
- Feed Handler as main processing pipeline with worker thread pools

## Platform Requirements

### Compiler and Environment
- **Compiler**: Clang 17+ (required)
- **Standard Library**: libc++ (Clang's optimized STL)
- **C++ Standard**: C++20 (required)
- **Platform**: Linux x86_64 only
- **Features Required**:
  - 128-bit integer support (`__int128`)
  - Hardware interference size constants
  - Huge page support (2MB/1GB)
  - NUMA APIs (optional but recommended)

### Build Environment
- **Development**: Docker container with Ubuntu 22.04 + Clang 17
- **Production**: Bare metal Linux with real-time kernel patches recommended
- **Key Dependencies**:
  - Conan 2.0+ package manager (migrated from 1.x for better C++20/Clang 17 support)
  - Boost 1.82+ (with Asio/Beast)
  - simdjson 3.9+
  - TOML++ 3.4+
  - libnuma (for NUMA-aware allocations)

## Memories

- Use the system design plan from @Crypto Market Data Provider_.md as the primary reference point for implementation
- Please use C++20 in your code. make sure it has clean formatting. For configuration files please use toml and toml format with toml++ module
- The codebase is targeting Linux hosts only - no Windows or macOS compatibility needed
- Focus on ultra-low latency and deterministic performance - every nanosecond matters
- Memory pool implementation has been thoroughly reviewed and hardened against production edge cases
- All code must be compiled with Clang 17+ for optimal HFT performance
- You need to use new style docker command, such as "docker compose" instead of "docker-compose"
- Always think hard. Before proposing code changes, think hard to make sure you don't break current code functionalities, such as API protocols, dev environment assumptions, features, etc. Think hard to not introducing new bugs when proposing a fix to the existing bugs. Always take a hard look of the larger window context of the code you are changing.
- Remember all dev work need to be done in a linux devcontainer, not on this MacBook host
- Run any code in devcontainer.