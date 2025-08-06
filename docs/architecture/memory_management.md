# Memory Management Architecture

## Overview

The crypto-lob system uses three specialized memory management components that work together to provide a complete, HFT-optimized memory management system for the WebSocket infrastructure. These components are designed to achieve <500ns latency from network to order book with zero allocations on the hot path.

## Core Components

### 1. SPSCRing (Single Producer Single Consumer Ring Buffer)

**Purpose**: Lock-free message passing between threads

**Key Features**:
- Wait-free operations (no locks, no spinning)
- Cache-line aligned slots to prevent false sharing
- Uses Disruptor-style sequence numbers for correctness
- O(1) push/pop operations with ~20ns latency

**Location**: `/src/core/spsc_ring.hpp`

### 2. MemoryPool (Generic Object Pool)

**Purpose**: General-purpose memory pool for any type

**Key Features**:
- Lock-free allocation/deallocation using CAS operations
- Thread-local caching to reduce contention
- Huge page support for better TLB performance
- Handles complex objects with constructors/destructors
- ABA-safe using 128-bit tagged pointers

**Location**: `/src/core/memory_pool.hpp`

### 3. RawMessagePool (Specialized WebSocket Buffer Pool)

**Purpose**: Optimized specifically for WebSocket raw message buffers

**Key Features**:
- Fixed-size 64KB buffers (WebSocket frame max)
- Thread-local pools (zero contention)
- Uses SPSCRing internally for free list management
- No constructor/destructor overhead (POD type)
- Huge page support with pre-faulting
- NUMA-aware memory allocation

**Location**: `/src/networking/raw_message_pool.hpp`

## Data Flow Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   WebSocket Data Flow                        │
└─────────────────────────────────────────────────────────────┘

1. Network Thread (per exchange):
   WebSocket → RawMessagePool::acquire() → RawMessage (64KB buffer)
                        ↓
   SPSCRing<RawMessage*>::try_push() → Parser Thread

2. Parser Thread:
   SPSCRing<RawMessage*>::try_pop() → RawMessage
                        ↓
   simdjson parse → MemoryPool<ParsedMessage>::allocate()
                        ↓
   SPSCRing<ParsedMessage*>::try_push() → LOB Thread

3. LOB Thread:
   SPSCRing<ParsedMessage*>::try_pop() → ParsedMessage
                        ↓
   Update OrderBook → MemoryPool<ParsedMessage>::deallocate()
                        ↓
   RawMessagePool::release() → Returns buffer to pool
```

## Component Comparison

### RawMessagePool vs MemoryPool

| Aspect       | RawMessagePool               | MemoryPool                       |
|--------------|------------------------------|----------------------------------|
| Purpose      | WebSocket buffers only       | Generic objects                  |
| Size         | Fixed 64KB messages          | Variable sized T                 |
| Thread Model | Thread-local (no contention) | Global with TLS cache            |
| Free List    | SPSCRing (single thread)     | Lock-free CAS (multi-thread)    |
| Complexity   | Simple POD                   | Complex objects with ctors/dtors |
| Overhead     | ~100ns allocate              | ~200ns with TLS cache            |

## Design Rationale

### Why Three Components?

1. **Separation of Concerns**:
   - SPSCRing: Thread communication
   - MemoryPool: Complex object lifecycle
   - RawMessagePool: High-volume buffer management

2. **Performance Optimization**:
   - RawMessagePool is faster because it's simpler (no locks, no CAS)
   - Thread-local design eliminates ALL contention
   - SPSCRing provides wait-free guarantees

3. **HFT Requirements**:
   - Zero allocations on hot path
   - Deterministic latency (<1μs)
   - No system calls after initialization

## Usage Example

```cpp
// Each I/O thread has its own pool
void websocket_io_thread(int exchange_id) {
    // Thread-local pool - no contention
    auto& raw_pool = get_thread_pool();
    
    // Global parser message pool (shared)
    MemoryPool<ParsedMessage> parsed_pool(10000);
    
    // Ring buffer to parser thread
    SPSCRing<RawMessage*> to_parser(1024);
    
    while (running) {
        // 1. Get buffer from pool (~100ns)
        RawMessage* msg = raw_pool.acquire();
        msg->timestamp_ns = __rdtsc();
        
        // 2. Read WebSocket data directly into buffer
        websocket.read(msg->data, msg->size);
        
        // 3. Push to parser via ring buffer (~20ns)
        to_parser.try_push(msg);
    }
}
```

## Performance Characteristics

This architecture achieves:
- **Zero contention**: Thread-local pools
- **Zero copies**: Data stays in same buffer
- **Zero allocations**: All memory pre-allocated
- **<500ns latency**: From network to order book

## Memory Layout

### RawMessage Structure
- **Metadata**: 64 bytes (1 cache line)
  - timestamp_ns: RDTSC timestamp
  - sequence_num: Global sequence
  - exchange_id: Exchange identifier
  - size: Actual data size
  - pool_index: Return index
- **Data Buffer**: 64KB (aligned for SIMD)

### Thread-Local Pools
- Default capacity: 16,384 messages
- Total memory per thread: ~1GB
- Huge pages: 2MB pages for TLB efficiency
- NUMA binding: Memory on same node as thread

## Configuration

### Compile-time Options
```cpp
// In raw_message_pool.hpp
static constexpr size_t DEFAULT_CAPACITY = 16384;  // Messages per pool
static constexpr size_t MAX_FRAME_SIZE = 65536;    // 64KB WebSocket max

// In spsc_ring.hpp  
static constexpr size_t MAX_CAPACITY = size_t{1} << 32;  // Max ring size
```

### Runtime Configuration
```cpp
// Set thread pool capacity before first use
set_thread_pool_capacity(32768);  // 32K messages

// Configure NUMA (Linux only)
#ifdef HAVE_NUMA
// Memory automatically bound to thread's NUMA node
#endif
```

## Thread Safety Guarantees

1. **RawMessagePool**: Single-threaded only (thread-local)
2. **SPSCRing**: Single producer, single consumer only
3. **MemoryPool**: Thread-safe with CAS operations

## Error Handling

All components use fail-fast strategy for HFT:
- Allocation failures → `std::terminate()`
- Double-free detection → `std::terminate()`
- Pool corruption → `std::terminate()`

This ensures deterministic behavior and prevents silent failures that could corrupt market data.