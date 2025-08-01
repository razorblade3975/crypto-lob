# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a C++ cryptocurrency market data provider system designed for high-frequency trading (HFT) applications. The system focuses on ultra-low latency, high throughput, and predictable performance for processing real-time market data from multiple cryptocurrency exchanges.

**Important**: This project uses exchange WebSocket streams for market data only (e.g., Binance WebSocket Streams), not WebSocket APIs for trading. The focus is on consuming and normalizing L2 order book data, not executing trades.

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

### Lock-Free Order Book Implementation (src/orderbook/)

**Complete Order Book System** with production-ready components:

#### Order Book Side (src/orderbook/book_side.hpp)
- **Intrusive RB-Tree**: Boost.Intrusive set for ordered price traversal
- **Hash Map Index**: Boost unordered_flat_map for O(1) price lookups
- **Memory Pool Integration**: All price level nodes allocated from thread-local pools
- **Top-N Tracking**: Efficient tracking of top price levels for market data feeds
- **Cache-Aligned Nodes**: 128-byte PriceLevelNode with hot data in first 32 bytes
- **Subtree Size Tracking**: Prepared for O(log n) rank queries (future feature)

#### Full Order Book (src/orderbook/order_book.hpp)
- **Bid/Ask Integration**: Complete order book with both sides
- **Top-of-Book Caching**: O(1) access to best bid/ask
- **Update Tracking**: Returns bool indicating if top-of-book changed
- **Snapshot Support**: Efficient snapshot generation for downstream consumers

#### Exchange Message Types (src/exchange/message_types.hpp)
- **Unified Message Format**: Common representation for all 6 exchanges
- **Zero-Copy Design**: Fixed-size arrays, no heap allocations
- **Exchange-Specific Handling**: SequenceInfo handles different update ID schemes
- **Cache-Aligned Structures**: All messages aligned to cache lines
- **Safety Features**: Deleted copy constructors prevent accidental 16KB copies

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
MemoryPool<Order> pool(10000);  // Capacity required
auto* order = pool.construct(order_id, price, quantity);
pool.destroy(order);

// RAII wrapper (recommended)
auto order_ptr = make_pooled(pool, order_id, price, quantity);

// Raw allocation with manual construction
auto* order = pool.allocate();
new (order) Order(order_id, price, quantity);
// ... use order ...
order->~Order();
pool.deallocate(order);
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

#### Binance WebSocket Details

**1. WebSocket Streams (Market Data)**
- **Base URLs**: 
  - Primary: `wss://stream.binance.com:9443`
  - Alternative: `wss://stream.binance.com:443`
  - Market data only: `wss://data-stream.binance.vision`
- **Connection Types**:
  - Raw streams: Single stream per connection (e.g., `/ws/bnbusdt@aggTrade`)
  - Combined streams: Multiple streams in one connection (e.g., `/stream?streams=bnbusdt@aggTrade/btcusdt@depth`)
- **Heartbeat**: 
  - Connection timeout: 24 hours maximum
  - Keep-alive requirement: Send message every 3 minutes to maintain connection
  - Ping frame interval: 20 seconds (server sends ping, client must respond with pong)
  - Pong timeout: 1 minute (connection closed if no pong received)
- **Message Format**: JSON with event type field `"e"` (e.g., `"aggTrade"`, `"depthUpdate"`)
- **Subscription**: Direct URL-based (no separate subscribe message needed)
- **Rate Limits**: 5 requests per second for new connections
- **No Authentication**: Public market data only, no user-specific streams

**2. WebSocket API (Trading/Account Management)**
- **Base URLs**:
  - Production: `wss://ws-api.binance.com/ws-api/v3`
  - Testnet: `wss://ws-api.testnet.binance.vision/ws-api/v3`
- **Authentication**: 
  - Ed25519 keys only (not HMAC)
  - Session-based with `session.logon` method
  - Requires API key, timestamp, and signature
- **Request/Response Model**:
  - Each request needs unique ID (UUID recommended)
  - Response includes matching request ID
  - Supports trading operations and account queries
- **Key Methods**: `exchangeInfo`, `session.logon`, `session.status`, `session.logout`
- **Note**: This is for trading operations, not market data streaming

#### KuCoin WebSocket Details
- **Two-step auth**: REST token → WebSocket connection
- **Token expiry**: 24 hours
- **Ping requirement**: 18 seconds

#### OKX WebSocket Details
- **Base URLs**:
  - Production: `wss://ws.okx.com:8443/ws/v5/public`
  - Demo Trading: `wss://wspap.okx.com:8443/ws/v5/public`
- **Rate Limits**:
  - 3 connections per second per IP
  - 480 total subscribe/unsubscribe requests per hour
- **Connection Management**:
  - Automatic disconnect after 30 seconds of inactivity
  - Client-initiated ping/pong required (send "ping" string, expect "pong")
  - Reconnection required if no pong received
- **Subscription Format**:
  ```json
  {
    "op": "subscribe",
    "args": [{
      "channel": "books",
      "instId": "BTC-USDT"
    }]
  }
  ```
- **Order Book Channels**:
  - `books`: 400-level snapshot + updates
  - `books5`: 5-level snapshot + updates  
  - `books-l2-tbt`: Tick-by-tick full depth
  - `bbo-tbt`: Best bid/offer updates
- **Message Format**: JSON with `arg` (channel info) and `data` array
- **No Authentication Required**: For public market data channels

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
- [x] Complete order book assembly (bid/ask sides integration)
- [ ] Dense adaptive arrays with tick bucketing (future optimization)

### Phase 3A: Complete Order Book Assembly ✅
- [x] Full Order Book implementation combining bid/ask sides (src/orderbook/order_book.hpp)
- [x] Exchange message types with standardized formats (src/exchange/message_types.hpp)
- [x] Comprehensive unit tests for order book functionality

### Phase 3B: JSON Parsing Layer (Immediate Priority - Week 1)
- [ ] Base parser interface using simdjson
- [ ] Exchange-specific parsers:
  - [ ] BinanceParser: Handle array format ["price", "quantity"]
  - [ ] KuCoinParser: Handle ["price", "size", "sequence"] format
  - [ ] OKXParser: Handle 4-element arrays with extra fields
  - [ ] BybitParser: Standard ["price", "size"] format
  - [ ] BitgetParser: Similar to OKX with seq/checksum
  - [ ] GateParser: Incremental updates with U/u ranges
- [ ] Parser object pool for zero-allocation parsing
- [ ] String-to-Price conversion optimization
- [ ] Comprehensive parser tests with real message samples

### Phase 3C: WebSocket Infrastructure (Week 2)
- [ ] Base WebSocket client using Boost.Beast
- [ ] Connection management layer:
  - [ ] Automatic reconnection with exponential backoff
  - [ ] Message buffering during reconnection
  - [ ] Connection state machine
- [ ] Exchange-specific connectors:
  - [ ] Binance: 
    - Direct URL streams (e.g., `/ws/btcusdt@depth20@100ms`)
    - No subscription messages needed (URL-based)
    - 20s server ping → client pong required
    - 3-minute activity requirement
  - [ ] KuCoin: Token-based auth, 18s ping requirement
  - [ ] OKX: 
    - Channel-based subscriptions with op/args format
    - 30-second timeout, client-initiated ping/pong
    - Multiple book channels (books, books5, books-l2-tbt)
    - Rate limit: 3 conn/sec, 480 sub/hour
  - [ ] Bybit: Separate spot/futures endpoints
  - [ ] Bitget: Unified endpoint with instType
  - [ ] Gate.io: REST snapshot requirement
- [ ] Heartbeat management per exchange requirements

### Phase 3D: Integration Layer (Week 3)
- [ ] Message flow pipeline: WebSocket → Parser → Message Type → OrderBook
- [ ] Multi-symbol Order Book Manager:
  - [ ] Symbol-to-OrderBook mapping
  - [ ] Thread-safe access patterns
  - [ ] Memory pool per symbol
- [ ] Synchronization Engine:
  - [ ] Snapshot + delta reconciliation algorithm
  - [ ] Buffering during snapshot fetch
  - [ ] Sequence validation and gap detection
  - [ ] State machine: UNINITIALIZED → SYNCING_SNAPSHOT → SYNCING_BUFFER → LIVE
- [ ] Feed Handler:
  - [ ] Worker thread pool for parallel processing
  - [ ] Back-pressure handling
  - [ ] Statistics and monitoring

### Phase 4: Configuration & Deployment (Week 4)
- [ ] TOML-based configuration system for exchanges and system settings
- [ ] Logging and monitoring for performance and health tracking
- [ ] Integration testing with end-to-end scenarios

### Phase 5: Production Features
- [ ] IPC publisher with shared memory for downstream consumers
- [ ] Performance optimization and regression testing
- [ ] Production monitoring and observability tools

## Implementation Timeline & Success Criteria

### Updated Timeline (From Current State)
- **Week 1**: JSON parsing infrastructure + comprehensive tests
- **Week 2**: WebSocket base implementation + 2-3 exchange connectors
- **Week 3**: Remaining exchange connectors + synchronization engine
- **Week 4**: Integration layer, configuration, end-to-end testing

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

## Critical Implementation Notes - Recent Updates

### JSON Parsing Pipeline (July 2025)
The system must handle JSON → Binary message conversion efficiently:
1. **Raw JSON arrives via WebSocket** (e.g., Binance: `{"b":[["50000","1.5"]]}`)
2. **simdjson parses into DOM** - Zero-copy where possible
3. **Exchange-specific parser extracts fields** - Each exchange has unique format
4. **String prices convert to Price type** - Using `Price::from_string()`
5. **Populate MarketDataMessage** - Fixed arrays, no allocations
6. **Zero-copy handoff to OrderBook** - Message passed by reference

### Memory Pool Benchmark Alignment (July 2025)
- Test structures using memory pools must be at least 64 bytes and aligned to 64 bytes
- Use `struct alignas(64)` for proper cache-line alignment
- FreeNode requires 64-byte alignment for ABA protection

### CI Environment Considerations
- Timestamp tests may see coarse clock resolution in CI environments
- Tests should handle zero-latency measurements gracefully
- Increased workloads may be needed to ensure measurable timings

## Current Implementation Status (July 2025)

### ✅ Completed Components
- **Memory Management**: Production-ready pools with thread-local caching
- **Core Data Structures**: SPSC ring buffer, cache alignment utilities
- **Order Book**: Complete implementation with bid/ask sides
- **Message Types**: Unified format for all 6 exchanges
- **Unit Tests**: Comprehensive coverage for all implemented components

### 🚧 Next Priority: JSON Parsing Layer
The system has all data structures ready but needs the parsing layer to connect to live exchanges. Each exchange sends JSON with different formats that must be parsed into our unified message types.

### 📋 Remaining Work
1. JSON parsing with simdjson
2. WebSocket connectivity with Boost.Beast
3. Integration layer and synchronization engine
4. Configuration and monitoring

## Memories

- Use the system design plan from @docs/Crypto Market Data Provider_.md as the primary reference point for implementation
- Please use C++20 in your code. make sure it has clean formatting. For configuration files please use toml and toml format with toml++ module
- The codebase is targeting Linux hosts only - no Windows or macOS compatibility needed
- Focus on ultra-low latency and deterministic performance - every nanosecond matters
- Memory pool implementation has been thoroughly reviewed and hardened against production edge cases
- All code must be compiled with Clang 17+ for optimal HFT performance
- You need to use new style docker command, such as "docker compose" instead of "docker-compose"
- Always think hard. Always Ultrathink.
- Before proposing code changes, think hard to make sure you don't break current code functionalities, such as API protocols, dev environment assumptions, features, etc. 
- Think hard to not introducing new bugs when proposing a fix to the existing bugs.
- Run any code in the dev-container.
- Always think hard to present multiple options as you can. Don't rush to fix. Ultrathink and compare pros and cons of each option.
- Always use @scripts/build.sh to build. Remember always build in docker.