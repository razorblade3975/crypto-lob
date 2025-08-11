# Atom Pattern Borrowing Plan for Crypto-LOB

This document outlines the specific components and design patterns from the atom reference codebase that will be adapted for the crypto-lob market data provider system. Each section maps atom patterns to our architecture components defined in `arch_guide.md`.

## 1. Lock-Free Queue Infrastructure

### Source: `atom/tce/tcelf/`

### Components to Adapt:
- **SPSCRingBuffer** - Single Producer Single Consumer ring buffer
  - File: `atom/tce/tcelf/SPSCRingBuffer.h`
  - Class: `trc::tcelf::SPSCRingBuffer`
  - Key methods: `write()`, `read()` with callback-based zero-copy interface
- **FixedQueue** - Multi-producer multi-consumer fixed-size queue
  - File: `atom/tce/tcelf/FixedQueue.h`
  - Class: `trc::tcelf::FixedQueue<T>`
  - Key methods: `enqueue()`, `dequeue()`, `tryEnqueue()`, `tryDequeue()`
- **Lock-free ring buffer** with atomic indices
  - File: `atom/tce/tcelf/FixedQueue.cpp`
  - Implementation details: Uses `std::atomic<size_t>` for head/tail indices

### Implementation in Crypto-LOB:

#### Exchange Connector → Parser Queue
```cpp
// Single producer (connector), single consumer (parser)
template<typename T>
class ConnectorToParserQueue {
    // Based on tcelf::SPSCRingBuffer
    // Zero-copy interface with callbacks
    // Page-aligned memory for optimal performance
    std::atomic<size_t> writeIndex;
    std::atomic<size_t> readIndex;
    alignas(64) T* buffer;  // Cache-line aligned
};
```

#### Parser → Order Book Queue  
```cpp
// Single producer (parser), single consumer (order book)
template<typename T>
class ParserToBookQueue {
    // Based on tcelf::SPSCRingBuffer
    // Lock-free with acquire-release semantics
    // Pre-allocated NormalizedMessage pool
};
```

#### Order Book → Publisher Queue
```cpp
// Single producer (order book), multiple consumers (publishers)
template<typename T>
class BookToPublisherQueue {
    // Based on tcelf::FixedQueue with MPSC optimization
    // Atomic state transitions (DESTROYED→COPY_IN→CONSTRUCTED→COPY_OUT)
};
```

### Performance Targets:
- Enqueue/dequeue latency: < 100ns
- Zero allocations in steady state
- Cache-line alignment to prevent false sharing

## 2. Channel/Processor Architecture

### Source: `atom/atom_base/base/`

### Components to Adapt:
- **ProcessorBase** - Abstract message processor interface
  - File: `atom/atom_base/base/ProcessorBase.h`
  - Class: `ProcessorBase`
  - Key methods: `process()`, `onTimeout()`, `onClose()`, `predecode()`
- **LiveChannel** - Template-based channel implementation
  - File: `atom/atom_base/base/LiveChannel.h`
  - Class: `LiveChannel<Link, Processor, ChannelBufferType>`
  - Key methods: `init()`, `onRead()`, `onTimeout()`, `onClose()`
- **ProcessorManager** - Central processor coordination
  - File: `atom/atom_base/base/ProcessorManager.h`
  - Class: `ProcessorManager`
  - Function registration pattern for zero-overhead dispatch
- **SFINAE helpers** for conditional compilation
  - File: `atom/atom_base/base/ProcessorSFINAEHelpers.h`
  - Macro: `CALL_PROCESSOR_FUNCTION_IF_EXISTS`

### Implementation in Crypto-LOB:

#### Base Processor Interface
```cpp
class ProcessorBase {
    virtual void process(const void* buffer, size_t len) = 0;
    virtual void onTimeout() = 0;
    virtual void onClose() = 0;
    virtual void onSnapshot(const SnapshotMessage& snapshot) = 0;
};
```

#### WebSocket Channel Template
```cpp
template<typename Link, typename Processor>
class WebSocketChannel {
    Link* link;           // WebSocket connection
    Processor& processor; // Message processor
    
    void onRead() {
        // Read from WebSocket
        // Pass to processor
    }
    
    void onDisconnect() {
        // Trigger reconnection
        // Request snapshot
    }
};
```

#### Exchange-Specific Processors
```cpp
class BinanceProcessor : public ProcessorBase {
    // Binance-specific message handling
    // Sequence number tracking
    // Snapshot/delta reconciliation
};

class CoinbaseProcessor : public ProcessorBase {
    // Coinbase-specific implementation
};
```

## 3. Publisher Pattern System

### Source: `atom/atom_apps/market_publisher/`

### Components to Adapt:
- **PublisherBase** - Abstract publisher with statistics
  - File: `atom/atom_apps/market_publisher/PublisherBase.h`
  - Class: `PublisherBase`
  - File: `atom/atom_apps/market_publisher/PublisherBase.cpp`
  - Key members: Statistics tracking, virtual `publish()` method
- **PublisherFactory** - Factory pattern for publisher creation
  - File: `atom/atom_apps/market_publisher/PublisherFactory.h`
  - File: `atom/atom_apps/market_publisher/PublisherFactory.cpp`
  - Class: `PublisherFactory`
  - Key method: `createPublisher()` with type-based dispatch
- **MultiPublisherBase** - Fan-out to multiple destinations
  - File: `atom/atom_apps/market_publisher/MultiPublisherBase.h`
  - Template class: `MultiPublisherBase<T>`
  - File: `atom/atom_apps/market_publisher/MultiPublisherBase.hpp`
- **LowbandPublisher** - Bandwidth optimization patterns
  - Files: `atom/atom_apps/market_publisher/LowbandLevelPublisher.h`
  - Class: `LowbandLevelPublisher`
  - Files: `atom/atom_apps/market_publisher/LowbandOrderPublisher.h`
  - Class: `LowbandOrderPublisher`

### Implementation in Crypto-LOB:

#### Publisher Hierarchy
```cpp
class PublisherBase {
    std::atomic<uint64_t> messagesPublished;
    std::atomic<uint64_t> bytesPublished;
    virtual void publish(const MarketUpdate& update) = 0;
};

class SharedMemoryPublisher : public PublisherBase {
    // Write to shared memory ring buffer
    // Atomic sequence number updates
};

class UDPMulticastPublisher : public PublisherBase {
    // Optional UDP multicast support
    // Batching for efficiency
};

class CompressedPublisher : public PublisherBase {
    // LZ4/Snappy compression for WAN
    // Based on LowbandPublisher pattern
};
```

#### Publisher Factory
```cpp
class PublisherFactory {
    static std::unique_ptr<PublisherBase> create(const Config& config) {
        if (config.type == "shared_memory") {
            return std::make_unique<SharedMemoryPublisher>(config);
        }
        // ... other types
    }
};
```

## 4. Memory Pool Architecture

### Source: `atom/tce/tcecs/` and atom pool patterns

### Components to Adapt:
- **MemoryPool** - High-performance thread-safe memory pool
  - File: `atom/tce/tcecs/MemoryPool.h`
  - Class: `trc::tcecs::MemoryPool<T>`
  - Singleton pattern: `MemoryPool<T>::instance()`
  - Key methods: `allocate()`, `deallocate()`
- **Thread-local pools** with work-stealing
  - Implementation in `MemoryPool.cpp`
  - Uses spinlock (`trc::tcemt::SpinLock`) for synchronization
- **Object pools from atom_base**
  - File: `atom/atom_base/trade/OrderPool.h` (example pattern)
  - File: `atom/atom_base/base/MessagePool.h` (message pooling)
- **Batch allocation strategies**
  - Referenced in `atom/atom_base/util/MemoryUtil.h`

### Implementation in Crypto-LOB:

#### Object Pool Template
```cpp
template<typename T>
class ObjectPool {
private:
    // Thread-local fast path
    static thread_local std::vector<T*> localPool;
    
    // Global pool for refill
    std::atomic<Stack<T*>*> globalPool;
    
    // Pre-allocated memory
    std::vector<std::unique_ptr<T[]>> chunks;
    
public:
    T* allocate() {
        if (localPool.empty()) {
            refillFromGlobal();
        }
        T* obj = localPool.back();
        localPool.pop_back();
        return obj;
    }
    
    void deallocate(T* obj) {
        obj->reset();  // Clear object state
        localPool.push_back(obj);
        
        if (localPool.size() > MAX_LOCAL_SIZE) {
            returnToGlobal();
        }
    }
};
```

#### Specific Pool Instances
```cpp
// Pre-allocate pools at startup
ObjectPool<NormalizedMessage> messagePool(100000);
ObjectPool<OrderBookLevel> levelPool(10000);
ObjectPool<MarketUpdate> updatePool(50000);
```

## 5. Shared Memory IPC Pattern

### Source: Atom shared memory and IPC components

### Components to Adapt:
- **SMBCastWriter** - Shared memory broadcast writer
  - File: `atom/atom_base/util/SMBCastWriter.h`
  - Class: `SMBCastWriter`
  - Enhanced version: `atom/atom_base/base/EnhancedSMBCastReadLink.h`
- **IPCPublisher** - IPC publishing pattern
  - File: `atom/atom_apps/market_publisher/IPCPublisher.h` (pattern reference)
  - Uses shared memory segment management
- **Shared memory structures**
  - File: `atom/atom_base/base/EnhancedSMBHeader.h`
  - Struct: `EnhancedSMBHeader` with atomic sequence numbers
- **IPC Queue implementation**
  - File: `atom/tce/tceipc/FixedQueue.h`
  - Class: `trc::tceipc::FixedQueue` - Process-safe queue

### Implementation in Crypto-LOB:

#### Shared Memory Structure
```cpp
struct SharedMemoryHeader {
    std::atomic<uint64_t> writeSequence;
    std::atomic<uint64_t> bufferSize;
    char padding[64 - 16];  // Cache line alignment
};

struct SharedMemoryRing {
    SharedMemoryHeader header;
    alignas(64) char buffer[];  // Ring buffer data
};
```

#### IPC Publisher Implementation
```cpp
class IPCPublisher {
private:
    SharedMemoryRing* shmRing;
    size_t ringSize;
    uint64_t currentWritePos;
    
public:
    void publish(const MarketUpdate& update) {
        // Serialize update
        size_t msgSize = serialize(update, tempBuffer);
        
        // Calculate position in ring
        uint64_t writePos = currentWritePos % ringSize;
        
        // Copy to shared memory
        memcpy(&shmRing->buffer[writePos], tempBuffer, msgSize);
        
        // Update sequence atomically
        shmRing->header.writeSequence.store(
            currentWritePos + msgSize, 
            std::memory_order_release
        );
        
        currentWritePos += msgSize;
    }
};
```

## 6. Order Book Management Patterns

### Source: `atom/atom_base/book/`

### Components to Adapt:
- **MarketBookBase** - Order-by-order book implementation
  - File: `atom/atom_base/book/MarketBookBase.h`
  - File: `atom/atom_base/book/MarketBookBase.cpp`
  - Class: `MarketBookBase`
  - Queue position tracking, auto-correction features
- **IndexedLevelBookBase** - High-performance indexed book
  - File: `atom/atom_base/book/IndexedLevelBookBase.h`
  - File: `atom/atom_base/book/IndexedLevelBookBase.cpp`
  - Class: `IndexedLevelBookBase`
  - Direct array indexing for O(1) access
- **BookAccessor pattern** - Read-only access
  - File: `atom/atom_base/book/BookAccessorBase.h`
  - File: `atom/atom_base/book/MarketOrderBookAccessor.h`
  - Class: `MarketOrderBookAccessor`
- **DiffBook implementations**
  - File: `atom/atom_apps/market_publisher/DiffBookBase.h`
  - Template: `DiffBookBase<T>`
  - File: `atom/atom_apps/market_publisher/LevelDiffBook.h`
  - Class: `LevelDiffBook` for level-based updates
  - File: `atom/atom_apps/market_publisher/OrderDiffBook.h`
  - Class: `OrderDiffBook` for order-based updates

### Implementation in Crypto-LOB:

#### Optimized Order Book Structure
```cpp
class OrderBook {
private:
    // Separate arrays for bids and asks
    alignas(64) std::vector<PriceLevel> bids;
    alignas(64) std::vector<PriceLevel> asks;
    
    // Price to index mapping for O(1) lookup
    std::unordered_map<Price, size_t> bidIndex;
    std::unordered_map<Price, size_t> askIndex;
    
public:
    void applyUpdate(const NormalizedMessage& msg) {
        // Binary search for insertion point
        // In-place update for existing levels
        // Maintain sorted order
    }
    
    // Read-only accessor for thread safety
    BookAccessor getAccessor() const {
        return BookAccessor(bids, asks);
    }
};
```

#### Price Level Structure
```cpp
struct PriceLevel {
    Price price;
    Quantity quantity;
    uint32_t orderCount;
    uint64_t updateId;
    
    // Ensure cache-line size
    char padding[64 - sizeof(Price) - sizeof(Quantity) - 8];
};
```

## 7. Configuration & Factory System

### Source: `atom/atom_base/config/`

### Components to Adapt:
- **MarketFactory** - Factory pattern for market creation
  - File: `atom/atom_base/config/MarketFactory.h`
  - File: `atom/atom_base/config/MarketFactory.cpp`
  - Class: `MarketFactory` (singleton pattern)
  - Key method: `createMarket()` with type dispatch
- **MarketCreator** - Base creator pattern
  - File: `atom/atom_base/config/MarketCreatorBase.h`
  - Class: `MarketCreatorBase`
  - File: `atom/atom_base/config/MarketCreatorHelper.h`
  - Helper utilities for market creation
- **PipelineTraits** - Compile-time configuration
  - File: `atom/atom_base/config/PipelineTraits.h`
  - Template: `PipelineTraits<MarketType>`
  - File: `atom/atom_base/config/DefaultPipelineTraits.h`
- **Configuration loading**
  - File: `atom/atom_base/config/MarketDataBase.h`
  - File: `atom/atom_base/config/MarketDataBase.cpp`
  - JSON files: `MarketCommon.json`, `MarketMain.json`

### Implementation in Crypto-LOB:

#### Configuration Structure
```cpp
struct SystemConfig {
    struct ExchangeConfig {
        std::string name;
        std::string wsUrl;
        std::vector<std::string> symbols;
        size_t queueSize;
        int parserCpu;
    };
    
    std::vector<ExchangeConfig> exchanges;
    
    struct PublisherConfig {
        std::string type;
        std::string shmPath;
        size_t ringSize;
    };
    
    PublisherConfig publisher;
    
    // Load from JSON/TOML
    static SystemConfig load(const std::string& path);
};
```

#### Exchange Factory
```cpp
class ExchangeConnectorFactory {
    using Creator = std::function<
        std::unique_ptr<ExchangeConnector>(const Config&)
    >;
    
    std::map<std::string, Creator> creators = {
        {"binance", [](const Config& c) { 
            return std::make_unique<BinanceConnector>(c); 
        }},
        {"coinbase", [](const Config& c) { 
            return std::make_unique<CoinbaseConnector>(c); 
        }}
    };
    
    std::unique_ptr<ExchangeConnector> create(
        const std::string& exchange, 
        const Config& config
    ) {
        return creators.at(exchange)(config);
    }
};
```

## 8. Thread Management Model

### Source: Atom threading components

### Components to Adapt:
- **CondorThread** - High-performance thread management
  - File: `atom/atom_apps/mm/CondorThread.h`
  - File: `atom/atom_apps/mm/CondorThread.cpp`
  - Class: `CondorThread`
  - Features: CPU affinity, real-time priority, thread naming
- **Thread utilities from TCE**
  - File: `atom/tce/tcemt/Thread.h`
  - Class: `trc::tcemt::Thread` - Base thread class
  - File: `atom/tce/tcemt/ThreadUtil.h`
  - Thread affinity and naming utilities
- **ProcessorAffinityUtil** - CPU affinity management
  - Referenced in atom_apps code
  - Sets thread-to-CPU bindings
- **NUMA utilities**
  - File: `atom/tde/tdecs/NumaAllocator.h`
  - NUMA-aware memory allocation

### Implementation in Crypto-LOB:

#### Base Thread Class
```cpp
class TradingThread {
protected:
    std::string name;
    int cpuCore;
    bool useRealtimePriority;
    
public:
    void start() {
        thread = std::thread([this] {
            // Set thread name
            pthread_setname_np(pthread_self(), name.c_str());
            
            // Set CPU affinity
            if (cpuCore >= 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(cpuCore, &cpuset);
                pthread_setaffinity_np(pthread_self(), 
                    sizeof(cpuset), &cpuset);
            }
            
            // Set real-time priority
            if (useRealtimePriority) {
                struct sched_param param;
                param.sched_priority = 99;
                pthread_setschedparam(pthread_self(), 
                    SCHED_FIFO, &param);
            }
            
            // Run thread logic
            this->run();
        });
    }
    
    virtual void run() = 0;
};
```

#### Thread Assignment Strategy
```cpp
class ThreadManager {
    void assignThreads() {
        // Isolated cores for critical threads
        exchangeConnector1->setCpu(4);  // Isolated core
        parser1->setCpu(5);              // Adjacent for cache
        orderBookEngine->setCpu(6);      // Separate core
        ipcPublisher->setCpu(7);         // Dedicated I/O
        
        // Control thread on shared core
        controlThread->setCpu(0);
    }
};
```

## 9. Performance Monitoring (TracePoints)

### Source: Atom performance monitoring

### Components to Adapt:
- **TracePoints** - Main tracing infrastructure
  - File: `atom/atom_base/base/TracePoints.h`
  - File: `atom/atom_base/base/TracePoints.cpp`
  - Class: `TracePoints` (singleton)
  - Key methods: `add()`, `start()`, `stop()`, `trace()`
- **StoneTracepoints** - Enhanced tracing
  - File: `atom/atom_base/base/StoneTracepoints.h`
  - File: `atom/atom_base/base/StoneTracepointsUtil.h`
  - Additional tracing utilities
- **Cycle-accurate timing**
  - Uses `__rdtsc()` for TSC reading
  - Macro: `ATOM_TRACE(event_id)`
- **Histogram generation**
  - File: `atom/atom_apps/lag_printer/LagHistogram.x.cpp`
  - Adaptive binning for latency analysis

### Implementation in Crypto-LOB:

#### Trace Point System
```cpp
class TracePoints {
private:
    struct Event {
        uint64_t timestamp;
        uint32_t eventId;
        uint32_t threadId;
    };
    
    // Lock-free ring buffer per thread
    thread_local std::array<Event, 65536> buffer;
    thread_local size_t writeIndex = 0;
    
public:
    enum EventType {
        WEBSOCKET_RECEIVE = 0,
        PARSE_START,
        PARSE_END,
        BOOK_UPDATE_START,
        BOOK_UPDATE_END,
        PUBLISH_START,
        PUBLISH_END
    };
    
    inline void trace(EventType event) {
#ifdef ENABLE_TRACING
        uint64_t tsc = __rdtsc();
        buffer[writeIndex % buffer.size()] = {tsc, event, getThreadId()};
        writeIndex++;
#endif
    }
};

// Macro for zero overhead when disabled
#ifdef ENABLE_TRACING
    #define TRACE(event) TracePoints::instance().trace(event)
#else
    #define TRACE(event) ((void)0)
#endif
```

## 10. Snapshot and Recovery Patterns

### Source: `atom/tmdp/` and market publisher patterns

### Components to Adapt:
- **TMDPProcessor** - Main TMDP processing engine
  - File: `atom/atom_base/tmdp/TMDPProcessor.h`
  - Class: `TMDPProcessor`
  - Sequence number management, gap detection
- **TMDPSnapshotMessage** - Snapshot handling
  - File: `atom/atom_base/tmdp/TMDPSnapshotMessage.h`
  - Snapshot message structures
- **MBRHandler** - Market Book Request handler
  - File: `atom/atom_base/book/MBRHandler.h`
  - File: `atom/atom_base/book/MBRHandler.cpp`
  - Class: `MBRHandler`
  - Snapshot request and recovery logic
- **TMDPArbitrator** - Feed arbitration
  - File: `atom/atom_base/tmdp/TMDPArbitrator.h`
  - Class: `TMDPArbitrator`
  - A/B feed selection, synthetic sequencing

### Implementation in Crypto-LOB:

#### Snapshot Manager
```cpp
class SnapshotManager {
private:
    std::atomic<uint64_t> lastProcessedSequence;
    std::queue<NormalizedMessage> bufferedDeltas;
    
public:
    void processSnapshot(const Snapshot& snapshot) {
        // Apply snapshot to order book
        orderBook->loadSnapshot(snapshot);
        
        // Set baseline sequence
        lastProcessedSequence = snapshot.lastUpdateId;
        
        // Process buffered deltas after snapshot
        while (!bufferedDeltas.empty()) {
            auto& delta = bufferedDeltas.front();
            if (delta.sequenceId > lastProcessedSequence) {
                orderBook->applyUpdate(delta);
                lastProcessedSequence = delta.sequenceId;
            }
            bufferedDeltas.pop();
        }
    }
    
    bool checkSequence(uint64_t sequence) {
        uint64_t expected = lastProcessedSequence + 1;
        if (sequence != expected) {
            // Gap detected - trigger snapshot refresh
            requestSnapshot();
            return false;
        }
        return true;
    }
};
```

## 11. WebSocket Connection Management

### Source: `atom/atom_apps/websockets/` patterns

### Components to Adapt:
- **WebsocketConnectionManager** - Connection lifecycle
  - File: Pattern described in `atom/atom_apps/websockets/CLAUDE.md`
  - Manages connection state, reconnection logic
- **WebsocketConnectionPool** - Thread pool management
  - Referenced in `atom/atom_apps/websockets/NOTES.md`
  - CPU affinity, io_context per thread
- **Exchange-specific handlers**
  - File pattern: `atom/atom_apps/websockets/arb_server/markets/`
  - Binance example: Market data and user data handlers
- **Connection utilities**
  - boost::beast WebSocket implementation
  - Ping/pong latency measurement
  - Exponential backoff on disconnect

### Implementation in Crypto-LOB:

#### WebSocket Manager
```cpp
class WebSocketManager {
private:
    boost::asio::io_context ioContext;
    boost::asio::steady_timer reconnectTimer;
    int reconnectDelay = 1000;  // Start with 1 second
    
public:
    void connect() {
        // Establish WebSocket connection
        // Set up ping timer
        // Subscribe to channels
    }
    
    void onDisconnect() {
        // Exponential backoff
        reconnectDelay = std::min(reconnectDelay * 2, 30000);
        
        reconnectTimer.expires_after(
            std::chrono::milliseconds(reconnectDelay)
        );
        
        reconnectTimer.async_wait([this](auto ec) {
            if (!ec) {
                connect();
            }
        });
    }
    
    void onPing() {
        // Send pong response
        // Reset heartbeat timer
    }
};
```

## 12. Build System Integration

### Source: Atom build system

### Components to Adapt:
- **CMake structure**
  - Root file: `atom/CMakeLists.txt`
  - Module pattern: `atom/atom_base/CMakeLists.txt`
  - Test integration: Uses `enable_testing()` and `add_test()`
- **Build variants** from atom_apps
  - File: `atom/atom_apps/CMakeLists.txt`
  - Variants: `mktpub-regtest`, `mktpub-lesslog`, `mktpub-lite`
  - Conditional compilation flags
- **Sanitizer support**
  - Referenced in `atom/tce/CLAUDE.md`
  - AddressSanitizer: `-fsanitize=address`
  - ThreadSanitizer: `-fsanitize=thread`
- **Test patterns**
  - Unit tests: `*.t.cpp` files
  - Executable tests: `*.x.cpp` files
  - Benchmark tests: `*.b.cpp` files

### Implementation in Crypto-LOB:

#### CMake Structure
```cmake
# Root CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(crypto-lob)

# Compiler flags for performance
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

# Sanitizer support
if(ENABLE_ASAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

# Components
add_subdirectory(src/connectors)
add_subdirectory(src/parser)
add_subdirectory(src/order_book)
add_subdirectory(src/publisher)

# Test infrastructure
enable_testing()
add_subdirectory(tests)
```

## Implementation Priority

### Phase 1: Core Infrastructure (Weeks 1-2)
1. Lock-free queues (SPSCRingBuffer, FixedQueue)
2. Memory pools for core objects
3. Basic thread management with CPU affinity

### Phase 2: Data Pipeline (Weeks 3-4)
1. Channel/Processor architecture
2. WebSocket connectors for Binance/Coinbase
3. Parser implementation with normalization

### Phase 3: Order Book Engine (Weeks 5-6)
1. Optimized order book structure
2. Snapshot/delta reconciliation
3. Sequence number tracking

### Phase 4: Publishing System (Weeks 7-8)
1. Shared memory IPC implementation
2. Publisher hierarchy
3. Performance monitoring integration

### Phase 5: Production Hardening (Weeks 9-10)
1. Configuration system with hot reload
2. Comprehensive error handling
3. Recovery mechanisms
4. Performance tuning and optimization

## Performance Targets

Based on atom's performance characteristics:

- **Network to Parser**: < 1 microsecond
- **Parsing**: < 5 microseconds
- **Order Book Update**: < 2 microseconds  
- **Publishing**: < 1 microsecond
- **End-to-end latency**: < 10 microseconds (p50), < 20 microseconds (p99)
- **Throughput**: > 1 million updates/second per symbol
- **Memory usage**: < 1GB for 100 symbols with 1000-level depth

## Testing Strategy

### Unit Tests
- Test each component in isolation
- Use Google Test framework
- Mock dependencies

### Integration Tests
- End-to-end data flow testing
- Use recorded market data for replay
- Verify sequence number handling

### Performance Tests
- Latency measurements with TracePoints
- Throughput testing under load
- Memory profiling

### Stress Tests
- Sustained high message rates
- Connection failures and recovery
- Memory leak detection with ASAN

## Risk Mitigation

### Technical Risks
1. **Lock-free complexity**: Start with simpler SPSC queues, add MPMC later
2. **Memory growth**: Implement pool size limits and monitoring
3. **Sequence gaps**: Comprehensive snapshot refresh logic
4. **CPU usage**: Implement adaptive spinning with backoff

### Performance Risks
1. **Parsing bottleneck**: Use SIMD JSON parsing if needed
2. **Order book updates**: Profile and optimize hot paths
3. **False sharing**: Careful cache-line alignment
4. **NUMA effects**: Pin memory to local NUMA nodes

## Conclusion

This borrowing plan provides a clear roadmap for adapting atom's battle-tested patterns to the crypto-lob system. The phased approach allows for incremental development while maintaining focus on the ultra-low latency requirements. Each borrowed component has been carefully selected for its relevance to cryptocurrency market data processing and mapped to specific implementation requirements in the crypto-lob architecture.