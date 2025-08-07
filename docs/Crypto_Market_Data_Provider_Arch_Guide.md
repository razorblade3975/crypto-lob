

# **A High-Performance Architectural Guide for a C++ Crypto Market Data Provider**

## **Section 1: Architectural Blueprint for a Low-Latency Market Data Provider**

This section establishes the high-level design for an ultra-low-latency market data provider. The architecture prioritizes minimal latency, high throughput, and predictability, drawing upon principles from high-frequency trading (HFT) systems. The design defines the system's core components, their interactions, the underlying process and threading model, and the guiding principles that ensure performance and robustness.

### **1.1 System Overview and Core Components**

The system is architected as a collection of specialized, cooperating components, where each is optimized for a single, well-defined responsibility. This adherence to the Single Responsibility Principle is fundamental in managing the complexity of HFT systems and allows for targeted performance tuning of each component in isolation.1

The system comprises four primary components:

1. **Exchange Connectors**: These components are responsible for the direct interface with each cryptocurrency exchange (e.g., Binance, OKX, KuCoin). The responsibility of a connector includes establishing and maintaining a persistent WebSocket connection, handling exchange-specific authentication protocols 3, managing the connection lifecycle (e.g., responding to ping/pong heartbeats to prevent disconnection 4), and managing subscriptions to specific product data streams.  
2. **Data Normalization & Parsing**: This component receives raw JSON message payloads from the various Exchange Connectors. Its sole function is to parse these text-based messages into a unified, internal C++ binary representation with maximum velocity. This stage is a well-known and critical latency bottleneck in any system processing exchange data feeds.  
3. **Order Book Engine**: As the central processing unit of the system, the Order Book Engine consumes the normalized data stream. It reconstructs and maintains a full-depth, in-memory limit order book (LOB) for every subscribed financial instrument. It is also responsible for generating business-level events, such as notifications of a change in the top-of-book or the occurrence of a market trade.  
4. **IPC Publisher**: This component acts as the system's outbound interface. It listens for processed events from the Order Book Engine, serializes the necessary data payload (e.g., the top five price levels of the order book), and publishes this data into a shared memory region for high-speed, lock-free consumption by other trading-related processes running on the same host.

### **1.2 Process and Threading Model**

To minimize operating system context-switching overhead and simplify the management of shared state, the application will be contained within a single Linux process. Within this process, a meticulously designed multi-threaded model is employed to achieve parallelism, isolate distinct tasks, and ensure predictable performance.

* **Network I/O Threads**: A dedicated thread will be assigned to manage the network I/O for each connected exchange. This thread will run an Asio io\_context event loop and will be exclusively responsible for non-blocking socket operations. This critical design choice isolates network latency and variability from the core business logic, preventing a network-related delay from stalling order book processing.  
* **Parser Threads**: A dedicated parser thread will be assigned to parse raw JSON message from each connected exchange. This thread will be exclusively responsible for consuming raw JSON messages from queue and parse them into normalized data.
* **LOB Worker Threads**: One or more dedicated worker threads will be responsible for processing the normalized data and updating the order books. A common and effective pattern is to shard instruments across these workers (e.g., all BTC-related products are processed by Worker Thread A, while all ETH-related products are handled by Worker Thread B). This sharding ensures that all updates for a single instrument are processed serially by the same thread, which elegantly avoids the need for locks or other complex synchronization mechanisms when modifying a specific order book.  
* **IPC Publisher Thread**: A single, dedicated thread will be responsible for taking the fully processed market data updates from the LOB Worker Threads and writing them into the shared memory inter-process communication (IPC) channel. This design establishes a single-producer model for the IPC mechanism, which is significantly simpler to implement correctly and offers superior performance compared to multi-producer scenarios.8  
* **Control/Management Thread**: A single, non-latency-critical thread will handle ancillary tasks such as application startup, graceful shutdown, reloading configuration from the TOML file, and system health monitoring.

### **1.3 Latency-Critical Path Analysis (The "Hot Path")**

The "hot path" represents the sequence of operations from data receipt to publication that must be executed with the absolute minimum and most predictable latency. Optimizing this path is the primary goal of the system architecture.

The hot path is defined as follows:

1. A network packet containing a WebSocket message arrives at the Network Interface Card (NIC).
2. The Linux kernel's network stack processes the packet and delivers the data to the user-space application.
3. The dedicated Network I/O thread (e.g. `BinanceSpotConnector`) reads the raw data from the socket buffer.
4. The raw data is passed to the appropriate Exchange Parser Thread via a high-performance, lock-free queue (`RawMsgQueue`).
5. The Exchange Parser Thread is exchange-specific (e.g. `BinanceSpotParser`), and it invokes the simdjson parser to deserialize the JSON message from `RawMsgQueue` into another lock-free queue (`ParsedMsgQueue`).
6. The LOB worker thread uses the parsed data from `ParsedMsgQueue` to update the corresponding in-memory LOB data structure.
7. If this update results in a change to the top-of-book or signifies a trade, an event is generated. This event is passed to the IPC Publisher Thread via another lock-free queue (`BookEventQueue`).
8. The IPC Publisher Thread serializes the top-of-book data and writes it into the shared memory ring buffer, making it available to subscribing processes.

Every operation on this path must be meticulously optimized. This includes the strict avoidance of dynamic memory allocation (e.g., new, malloc), virtual function calls which introduce indirection, and any system call that could potentially block.1

### **1.4 Key Design Principles**

The architecture is founded on several key principles derived from the field of low-latency systems programming.

* **Mechanical Sympathy**: The design must be deeply aware of the underlying hardware on which it runs. This includes a practical understanding of CPU architecture, particularly the memory hierarchy (L1/L2/L3 caches), cache line sizes, and the performance implications of cache hits versus misses. Data structures and algorithms will be chosen and designed to work *with* the hardware, not against it, to maximize cache efficiency.9  
* **Zero-Copy Data Flow**: To eliminate the overhead of moving data in memory, the system will strive for a zero-copy approach. Data will be processed in-place or passed by reference or pointer whenever possible. The final IPC stage, where subscribing processes read directly from a memory region written to by the provider, is the ultimate expression of this principle, bypassing kernel-level data copying entirely.12  
* **Compile-Time Optimization**: Modern C++ features will be leveraged to shift computational work from runtime to compile-time. This includes the use of constexpr for compile-time constants and functions, templates for static polymorphism, and compiler hints like \[\[likely\]\] and \[\[unlikely\]\] to guide the CPU's branch predictor, all of which contribute to reducing runtime latency.2  
* **Data-Oriented Design**: Rather than a purely object-oriented approach that focuses on abstractions, this design prioritizes the data itself and the transformations applied to it. This often leads to the use of simpler data structures (like POD structs) arranged in cache-friendly layouts (e.g., Structure of Arrays) that enable more efficient, sequential processing of data.14

The architectural separation of I/O, processing, and publishing into dedicated threads that are pinned to specific CPU cores is not merely a strategy for achieving parallelism. It is a fundamental technique for combating *jitter*—the variance in latency. In HFT, consistent, predictable latency is often more valuable than the lowest possible average latency. By isolating these critical tasks, the system prevents a delay in one domain, such as a network I/O hiccup, from unpredictably stalling another, like order book processing. This mirrors principles found in other real-time systems, such as professional audio processing 15, and is a core tenet of kernel tuning for low-latency networking.16 This physical separation of concerns on dedicated hardware resources transforms the logical software architecture into a predictable pipeline on the CPU itself.

Furthermore, the system design must account for the full data lifecycle, which includes the "cold path" of initialization. To build a correct order book from a differential feed, the application must first fetch a complete snapshot of the book, typically via a REST API call.17 This request-response interaction is a fundamentally different I/O pattern from the primary asynchronous WebSocket stream. The architecture must therefore accommodate this initial, blocking-style setup phase for each product without compromising the performance of the real-time "hot path." A naive implementation that only handles the WebSocket stream would be incapable of ever constructing a valid order book, highlighting the need to design for both the initialization and steady-state phases of operation.

## **Section 2: High-Performance Network Ingress: WebSocket Connectivity**

The networking layer is the gateway for all market data into the system. Its design must be robust, highly asynchronous, and optimized for minimal latency. The choice of technology at this layer dictates the application's entire concurrency model and is a critical architectural decision.

### **2.1 Selecting a C++ WebSocket Library**

A comparative analysis of leading C++ WebSocket libraries is essential. The primary candidates are evaluated based on performance, dependencies, programming model, and maturity.

* **Boost.Beast**: A header-only library built directly on the Boost.Asio networking framework.18 It is designed for high-performance, asynchronous applications and provides developers with fine-grained control over critical resources like memory buffers and threads. Its integration with the widely-used Asio library makes it a strong contender for robust systems.  
* **WebSocket++**: Another mature, header-only library that can use Boost.Asio or standalone Asio as its network transport layer.19 Its performance is largely comparable to that of Boost.Beast, as both delegate the underlying I/O operations to Asio.20  
* **uWebSockets**: This library is frequently cited in benchmarks for its exceptionally low latency and memory footprint.21 However, it uses a bespoke, non-Asio API. The validity of its benchmark results for real-world scenarios has been questioned, and it prioritizes raw speed over a standardized, maintainable programming model.23

For a system where robustness, maintainability, and architectural soundness are paramount, an Asio-based library is the superior choice. The established patterns and strong community support of the Asio ecosystem provide a more solid foundation than a library optimized purely for benchmark performance.

### **2.2 Recommended Implementation: Boost.Beast with Asio**

**Boost.Beast is the recommended library for this project.** As an official part of the Boost C++ Libraries, it adheres to high standards of quality, documentation, and performance. Its design philosophy, which grants the developer full control over threading and memory management, is perfectly aligned with the requirements of an HFT application.18

Underpinning this choice is **Asio**, the de-facto standard for high-performance, cross-platform asynchronous I/O in C++. Asio provides the fundamental building blocks for networking and is used in some of the world's fastest financial market systems. The combination of Beast for WebSocket protocol handling and Asio for the underlying I/O provides a powerful, modern, and proven foundation for the network layer.18

The choice of networking library is not merely a performance decision; it is a foundational architectural commitment that defines the application's entire approach to concurrency and error handling. Adopting an Asio-based library like Beast enforces a disciplined, asynchronous-first design from the outset. This is a non-negotiable prerequisite for any low-latency system, as attempting to retrofit asynchronicity onto a synchronous design is a common and critical failure pattern. The architectural integrity, standardized patterns, and maintainability afforded by the Asio ecosystem far outweigh the benchmark-driven performance claims of non-standard alternatives.23

### **2.3 Asynchronous I/O Strategy**

The networking layer will be built around an Asio io\_context event loop, running on a dedicated, pinned CPU core. All network operations—connecting, handshaking, reading, and writing—will be performed asynchronously to ensure that the I/O thread never blocks.

* **Callbacks vs. Coroutines**: While Beast and Asio support both traditional callback-based programming and modern C++20 coroutines, **coroutines are strongly recommended**. They enable developers to write asynchronous logic that has the linear, readable structure of synchronous code. This dramatically simplifies the implementation of complex sequences of operations (e.g., connect \-\> perform TLS handshake \-\> perform WebSocket handshake \-\> start reading), which helps to avoid "callback hell" and reduces the likelihood of subtle bugs related to object lifetimes and state management.  
* **Underlying I/O Multiplexing**: On Linux, Asio transparently uses the epoll system call facility. epoll is a highly efficient and scalable I/O event notification mechanism capable of managing thousands of concurrent connections with minimal overhead.27 While the newer  
  io\_uring interface can offer even lower latency by further reducing system call overhead 28, its API is more complex and less mature. The pragmatic and robust approach is to build upon Asio's  
  epoll-based backend. The architecture should be designed to decouple the application logic from the specific I/O mechanism, which would facilitate a future migration to an io\_uring-based Asio backend if desired.

### **2.4 Exchange Connector Abstraction**

To accommodate multiple exchanges with their unique API requirements, an abstract ExchangeConnector interface will be defined. This can be achieved using a C++ abstract base class or a template-based policy design. This abstraction provides a uniform interface to the rest of the application, isolating exchange-specific implementation details.

* **Core Interface**: connect(), subscribe(products), on\_message(callback), disconnect().  
* **Exchange-Specific Implementations**:  
  * **BinanceConnector**: Will implement logic to handle Binance's use of combined streams (e.g., /stream?streams=...) 17 and its specific ping/pong timing requirements.4  
  * **KuCoinConnector**: Will implement the necessary two-step authentication process. This involves first making a REST API call to obtain a temporary connection token, and then using that token in the WebSocket connection URL.3 This highlights the need for a simple, non-performance-critical HTTP client within the connector's initialization logic.  
  * **OKXConnector**: Will implement the specific JSON message format for subscriptions and correctly handle the distinction between its public and private data channels.31

This abstraction is crucial for maintainability and extensibility, allowing new exchanges to be added in the future by simply creating a new connector implementation without altering the core data processing engine.33

It is critical to recognize that exchange WebSocket APIs are not uniform. They contain subtle but vital differences in connection management and data semantics that must be handled correctly. For instance, Binance Spot WebSocket connections require a pong response within one minute of a ping sent every 20 seconds 4, whereas their Futures API uses a 3-minute ping interval with a 10-minute response timeout.5 KuCoin connection tokens expire after 24 hours and must be renewed.3 A generic, one-size-fits-all keepalive strategy will inevitably fail. Failure to adhere to these specific protocol details will result in silent disconnections and lost market data, which is a catastrophic failure for an HFT system. Therefore, each

ExchangeConnector must encapsulate its own stateful logic for managing heartbeats, proactive reconnections, and re-authentication to ensure long-term session stability.

| Library | Core Dependency | Programming Model | Performance Profile | Key Strengths | Key Weaknesses |
| :---- | :---- | :---- | :---- | :---- | :---- |
| **Boost.Beast** | Boost.Asio | Asio-native (Callbacks, Coroutines) | High, leverages Asio for I/O 18 | Excellent integration with Boost, flexible, gives full control over threads/buffers, strong community support. | Requires familiarity with Boost.Asio. |
| **WebSocket++** | Standalone or Boost.Asio | Asio-based or custom transport | High, comparable to Beast when using Asio 20 | Mature, flexible dependency management, cross-platform. | Documentation is less centralized than Beast's. |
| **uWebSockets** | Custom (libuv) | Bespoke, non-Asio event-driven | Very High (benchmarked), but questions on real-world use 23 | Extremely low memory footprint and benchmark latency.21 | Non-standard API, less flexible, not built on the robust Asio model. |

## **Section 3: Data Deserialization at Line Speed**

This section addresses the pivotal task of parsing incoming JSON messages from the exchanges. In a high-message-rate environment like crypto market data, this deserialization step is frequently the single largest contributor to CPU utilization and latency on the application's hot path.

### **3.1 The JSON Parsing Bottleneck**

Cryptocurrency exchanges almost universally employ JSON as the data format for their real-time WebSocket APIs.3 As a text-based format, parsing JSON requires significant computational work, including string manipulation, validation of syntax, and conversion of text representations into native C++ data types (e.g., strings to integers or floating-point numbers). This process is inherently more expensive than processing a compact binary protocol. For a market data feed handler processing tens of thousands of messages per second, the choice of JSON library is a first-order determinant of overall system performance.35

### **3.2 Comparative Analysis of High-Speed JSON Libraries**

A careful evaluation of available C++ JSON libraries is necessary to select the optimal tool for this performance-critical task.

* **RapidJSON**: For many years, RapidJSON was the industry standard for high-performance JSON processing in C++. It provides both SAX (event-based) and DOM (tree-based) parsing models and is well-regarded for its speed and efficient memory usage.35  
* **simdjson**: This modern library has redefined the performance expectations for JSON parsing. It leverages SIMD (Single Instruction, Multiple Data) CPU instructions to process JSON data at rates of multiple gigabytes per second. This makes it orders of magnitude faster than traditional parsers like RapidJSON, while still performing full validation of the JSON structure and UTF-8 encoding.39  
* **Glaze**: Another contemporary, high-performance library that claims to be the fastest available for direct-to-struct deserialization. It achieves its speed by avoiding the creation of an intermediate DOM representation, instead parsing data directly from the JSON text into the memory of C++ objects. This approach is an excellent match for the application's need to convert incoming messages into a fixed internal format.41  
* **nlohmann/json**: This library is widely praised for its exceptional, modern C++ API that is intuitive and easy to use. However, its focus is on developer ergonomics rather than raw performance. Its speed is not in the same category as simdjson or RapidJSON, making it unsuitable for the hot path of an HFT application.37 It remains a viable choice for non-performance-critical tasks, such as parsing the initial configuration file.

### **3.3 Recommended Implementation: simdjson with a Writer Fallback**

For parsing the high-volume stream of incoming market data, **simdjson is the unequivocal choice.** Its revolutionary parsing speed provides a significant and measurable competitive advantage, directly reducing the "time-to-book" for every market data update.39

A key consideration is that simdjson is a highly specialized tool designed exclusively for parsing; it does not provide functionality for serializing C++ data structures back into JSON strings.43 The application, however, must send JSON-formatted messages to the exchanges to manage subscriptions. This creates an asymmetric performance requirement. To address this, a hybrid strategy is optimal: use

simdjson for the performance-critical ingress path, and a secondary, "fast enough" library for the non-critical egress path. A simple library like RapidJSON or even fmtlib-based string formatting can be used to construct the relatively infrequent and simple subscription messages. This approach challenges the common developer instinct to select a single "do-it-all" library, instead choosing the best tool for each specific job.

### **3.4 Implementation Pattern: Parser Object Reuse**

A critical, non-obvious optimization for achieving simdjson's advertised performance is the reuse of the parser object. User reports and performance analysis have shown that instantiating a new simdjson::ondemand::parser object for each individual message introduces significant overhead from repeated memory allocation and deallocation, which can severely degrade performance and even become the dominant factor in the processing loop.36

The correct implementation pattern is to ensure that each LOB worker thread owns a single, long-lived simdjson parser object. For each incoming message, the thread should call the parser.iterate(message) method on its existing parser instance. This pattern allows the parser to reuse its internal memory buffers across many invocations, effectively eliminating dynamic memory allocation from the parsing hot path and unlocking the full speed of the SIMD-powered engine.36 This highlights a crucial principle in low-latency development: understanding and correctly utilizing a tool's memory model is often as important as the algorithm the tool implements.

| Library | Parsing Speed (GB/s) | Serialization Support | API Style / Ease of Use | Key Feature |
| :---- | :---- | :---- | :---- | :---- |
| **simdjson** | Exceptional (up to 7 GB/s) 39 | No (Parser-only) 43 | On-demand API, requires careful use | SIMD-accelerated parsing |
| **Glaze** | Exceptional (claims faster than simdjson for direct-to-struct) 41 | Yes | Direct-to-struct mapping, minimal boilerplate | Zero-overhead, no intermediate representation |
| **RapidJSON** | High (approx. 0.5 GB/s) 40 | Yes | SAX/DOM APIs, C-style | Mature, balanced performance for read/write |
| **nlohmann/json** | Moderate (approx. 0.1 GB/s) 40 | Yes | Excellent, modern C++ API 37 | Unmatched ease of use and expressiveness |

## **Section 4: The Heart of the System: The Limit Order Book Engine**

The Limit Order Book (LOB) engine is the system's analytical core. It is responsible for maintaining an accurate, real-time model of the market for each instrument. The correctness of its logic and the performance of its data structures are non-negotiable, as all downstream trading decisions will be based on the data it produces.

### **4.1 Data Structures for an HFT-Grade Order Book**

A naive implementation of an order book, such as std::map\<double, double\>, is wholly inadequate for HFT. It suffers from the imprecision of floating-point keys and lacks the necessary granularity to represent individual orders at each price level. A more common approach, std::map\<Price, std::list\<Order\>\>, provides O(log N) access to price levels and O(1) insertion/deletion of orders within a level.44 However, the node-based memory allocation of

std::map and the pointer-chasing nature of std::list lead to poor CPU cache locality, creating performance bottlenecks that are unacceptable in a low-latency environment.45

A state-of-the-art implementation requires a hybrid data structure approach, combining multiple structures where each is optimized for a specific type of query. The goal is to achieve O(1) or O(log N) complexity for all critical operations. The most effective design, described in multiple analyses of HFT systems, involves a balanced binary search tree of price levels, with each level containing a queue of orders, and a separate hash map for direct order access.47 This demonstrates a key design principle: one does not model the "order book" as a single entity, but rather models the

*operations* required and creates the necessary data structures and indexes to make each operation fast.

### **4.2 Recommended Data Structure and Implementation**

The recommended implementation synthesizes these principles into a cohesive and performant unit.

* **Book-Side Structure**: Two std::map containers will represent the bid and ask sides of the book. std::map is chosen for its underlying red-black tree implementation, which provides efficient O(log N) insertion/deletion of price levels and, crucially, sorted iteration. Prices will be represented by a fixed-point integer type to avoid floating-point issues.  
  * std::map\<Price, PriceLevel, std::greater\<Price\>\> bids;  
  * std::map\<Price, PriceLevel, std::less\<Price\>\> asks;  
* **Order Struct**: A Plain Old Data (POD) struct containing essential order information (order\_id, qty, timestamp) and pointers for an intrusive, doubly-linked list (next\_order, prev\_order). Intrusive lists avoid separate node allocations, improving cache locality.  
* **PriceLevel Struct**: A struct containing the aggregate data for a single price level, including the total volume and the head/tail pointers for the intrusive list of Orders at that level.  
* **Order Cancellation Map**: A separate std::unordered\_map\<uint64\_t, Order\*\> will provide a direct mapping from a unique order\_id to its corresponding Order object in the book. This is the key to achieving O(1) order cancellation, a frequent operation in modern markets.47  
* **Custom Memory Allocation**: All elements within these data structures—the nodes for the maps and the Order objects themselves—will be allocated from a custom memory pool. This critical step eliminates calls to malloc or new from the hot path, avoiding the unpredictable latency of system memory allocation.

### **4.3 Logic for Order Book Reconstruction**

The process of building and maintaining an accurate order book from a differential data feed is notoriously complex and has zero tolerance for error. A single out-of-sequence message or a mishandled snapshot can lead to a permanently corrupt book state, causing any dependent trading algorithm to operate on phantom data. The industry-standard best practice, as detailed in the Binance API documentation, must be followed precisely.17

The algorithm is as follows:

1. Establish a connection to the WebSocket differential depth stream (e.g., \<symbol\>@depth).  
2. Begin buffering all incoming update messages in a queue. Do not process them yet.  
3. Fetch a full depth snapshot of the order book via a REST API call (e.g., /api/v3/depth?symbol=...\&limit=5000).  
4. Examine the lastUpdateId from the snapshot. Discard any buffered messages where the final update ID (u) is less than or equal to the snapshot's lastUpdateId.  
5. Find the first buffered message that bridges the snapshot. The condition U \<= lastUpdateId \+ 1 \<= u must be true for this message, where U is the first update ID and u is the final update ID in the event. If this condition cannot be met, the state is inconsistent, and the process must be restarted from step 3\.  
6. Initialize the local in-memory order book with the data from the REST snapshot.  
7. Apply the remaining buffered updates sequentially to the local book.  
8. Continue processing new messages from the WebSocket in real-time as they arrive. Each update modifies the local book by adding, changing, or deleting a price level. An update with a quantity of '0' signifies the removal of that price level from the book.

This process implies that the system must maintain a state machine for each product's order book (UNINITIALIZED \-\> SYNCING\_SNAPSHOT \-\> SYNCING\_BUFFER \-\> LIVE). Any detected inconsistency (e.g., a gap in sequence numbers) must transition the state back to UNINITIALIZED and trigger a full resynchronization to guarantee data integrity.

### **4.4 Cache-Friendly Design and False Sharing Avoidance**

*False sharing* is a subtle but severe performance degradation pattern in multi-threaded applications. It occurs when logically separate variables, accessed by different threads, happen to reside on the same CPU cache line (typically 64 bytes). When one thread modifies its variable, the cache coherency protocol (e.g., MESI) invalidates the entire cache line for all other cores, forcing them to perform expensive reloads from a higher cache level or main memory, even though their data was not logically changed.49

This will be mitigated through several techniques:

1. **Padding and Alignment**: Critical data structures that may be accessed by different threads, such as the OrderBook objects themselves or the sequence numbers in inter-thread queues, will be explicitly aligned to a cache line boundary using alignas(64). This ensures they do not share a cache line with other data.  
2. **Strategic Struct Layout**: Within data structures, fields will be organized based on their access patterns. Frequently modified data will be separated from read-only or infrequently modified data to prevent unnecessary cache line invalidations.51 For example, if one LOB worker thread handles BTC and another handles ETH, their respective  
   OrderBook objects must be allocated with sufficient padding to prevent their internal members from causing cache contention between the threads.

| Data Structure | Add Order (New Price) | Add Order (Existing Price) | Cancel Order (by ID) | Get Best Bid/Ask | Cache Locality |
| :---- | :---- | :---- | :---- | :---- | :---- |
| std::map\<Price, std::list\<Order\>\> | O(log N) | O(log N) | O(N) \- requires search | O(1) | Poor (node-based map, pointer-chasing list) |
| Sparse Array \+ Linked List | O(1) | O(1) | O(N) \- requires search | O(M) in worst case | Good (contiguous array) but list is poor |
| **Recommended Hybrid Structure** | **O(log N)** | **O(log N)** | **O(1)** (via unordered\_map) | **O(1)** | **Good** (intrusive list, pooled allocators) |

#### 

### **Advanced LOB Design: A Deep Dive into Performance Engineering**

#### The design of a high-frequency trading (HFT) LOB is an exercise in optimization at every level, from data structure selection to memory layout. The goal is to eliminate any source of non-determinism or overhead from the critical path, particularly dynamic memory allocation and poor CPU cache utilization.\[2\]

#### **1\. The Zero-Allocation Memory Model**

The single most important principle for achieving predictable low latency on the "hot path" is the complete elimination of dynamic memory allocation calls like `malloc` or `new`.\[3, 4\] These system calls are slow and, more importantly, have unpredictable execution times, introducing jitter that is unacceptable in HFT.\[1\]

To achieve this, we employ a custom memory management strategy centered around a Memory Pool or Slab Allocator.\[3, 5\]

* Concept: At application startup, a large, contiguous block of memory is pre-allocated. This block is then carved up and managed by our application directly. When a new `Order` or `PriceLevel` object is needed, memory is taken from this pool instead of from the operating system. When an object is deleted, its memory is returned to the pool for reuse, not freed back to the OS.\[6\]  
* Implementation: A simple and highly effective approach is a free list-based pool allocator. The pool maintains a linked list of available memory chunks.  
  * Allocation: Requesting a new `Order` object is an O(1) operation: simply pop a pointer to a free memory block from the head of the free list.\[6\]  
  * Deallocation: Releasing an `Order` is also O(1): push its memory block pointer back onto the head of the free list.\[3\]  
* Placement `new`: Once a raw memory block is acquired from the pool, the C++ object is constructed in that specific location using `placement new`. This invokes the object's constructor without performing another memory allocation.\[2\] The object's lifetime is then managed manually, with an explicit destructor call before the memory is returned to the pool.

This strategy ensures that all memory management on the critical path is reduced to simple, deterministic pointer manipulations, completely avoiding kernel-level system calls.

#### **2\. Cache-Friendly Data Structures: Intrusiveness and Contiguity**

#### Standard library containers like `std::list` or `std::map` are "non-intrusive," meaning they allocate separate node objects to hold the user's data. This results in scattered memory allocations and pointer-chasing during iteration, which is devastating for CPU cache performance.\[7\] The solution is to use intrusive data structures and prioritize contiguous memory layouts.

#### Intrusive Linked Lists: Instead of `std::list<Order>` at each price level, we embed the list's linkage pointers directly within the `Order` struct itself.\[7, 8\]   struct Order {     //... order data (ID, quantity, timestamp)

####     Order\* next\_order; // Intrusive pointer

####     Order\* prev\_order; // Intrusive pointer

#### };

* ####  This has a profound impact on cache locality. All `Order` objects are allocated from our contiguous memory pool. Linking them together intrusively means that when traversing the list of orders at a price level, the CPU is more likely to find the next `Order` object already in its cache, as they were allocated close to each other in memory.\[9\] This eliminates the pointer indirection inherent in `std::list`, which would have to fetch a separate `std::list::node` object before getting to the actual `Order` data.\[7\] 

* #### Contiguous Price Levels: While `std::map` provides O(log N) access, its node-based structure also suffers from poor cache locality. A more cache-friendly approach for storing the price levels themselves is a sorted `std::vector` or a sparse array.\[10\] 

  * #### `std::vector` Approach: Using two sorted `std::vector`s (one for bids, one for asks) to hold `PriceLevel` objects ensures that all price levels are stored in a single, contiguous block of memory.\[10\]

    * #### Lookup: Finding a price level becomes a binary search (O(log N)). However, in practice, most activity occurs near the best bid and offer (BBO). A linear search starting from the inside of the book is often faster because it benefits from better branch prediction and cache performance.\[10\]

    * #### Insertion/Deletion: This becomes the main drawback, as it may require shifting elements within the vector (an O(N) operation). However, `memcpy` operations on contiguous memory are highly optimized and can be faster than a series of scattered memory accesses required by tree rebalancing.\[10\]

#### **3\. The Final LOB Design Synthesis**

#### Combining these principles gives us a complete, high-performance LOB design:

1. #### `Order` Struct: A simple, POD-like struct containing core data (`order_id`, `quantity`, etc.) and the intrusive `next_order` and `prev_order` pointers.\[8\]

2. #### `PriceLevel` Struct: Contains aggregate data for a price level (e.g., `total_volume`) and head/tail pointers to the intrusive list of `Order`s at that level.

3. #### Book-Side Containers: Two `std::vector<PriceLevel>` containers, one for bids (sorted descending) and one for asks (sorted ascending). This ensures price levels are stored contiguously.\[10\]

4. #### O(1) Cancellation Map: A `std::unordered_map<uint64_t, Order*>` provides immediate access to any order by its ID for O(1) cancellation, a critical and frequent operation.\[11, 12\]

5. #### Unified Memory Management: A single, master Slab Allocator is created at startup. It manages pools of fixed-size memory blocks for `Order` objects. All `Order`s, regardless of which `PriceLevel` they belong to, are allocated from this single source.\[5\] The `std::vector`s for `PriceLevel`s and the `std::unordered_map` will use their default allocators, which is acceptable as they perform large block allocations infrequently, not on the per-message hot path.

#### This design ensures that every operation on the critical path—adding, modifying, or canceling an order—is a sequence of deterministic, cache-friendly memory accesses with zero calls to the system's general-purpose allocator. This is the foundation for building a LOB engine that is not just fast on average, but predictably fast under load.

#### 

#### **Works cited**

1. C++ Design Patterns for Low Latency Applications Including High Frequency Trading | @ieg, accessed July 9, 2025, [https://programmador.com/series/notes/cpp-design-patterns-for-low-latency-apps/](https://programmador.com/series/notes/cpp-design-patterns-for-low-latency-apps/)  
2. C++ Design Patterns for Low-latency Applications Including High ..., accessed July 9, 2025, [https://arxiv.org/pdf/2309.04259](https://arxiv.org/pdf/2309.04259)  
3. Introduction \- KUCOIN API, accessed July 9, 2025, [https://www.kucoin.com/docs-new/websocket-api/base-info/introduction](https://www.kucoin.com/docs-new/websocket-api/base-info/introduction)  
4. General API Information | Binance Open Platform, accessed July 9, 2025, [https://developers.binance.com/docs/binance-spot-api-docs/websocket-api/general-api-information](https://developers.binance.com/docs/binance-spot-api-docs/websocket-api/general-api-information)  
5. Websocket Market Streams \- Binance Developer center, accessed July 9, 2025, [https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams](https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams)  
6. automatesolutions/WebSocket\_HFT: A Deribit Trading System using C++ utilizing WebSocket that performs the following actions: Place an order Cancel an order Modify an order Retrieve the order book View current positions All operations are conducted via a command-line interface, with a focus on latency measurement and optimization. \- GitHub, accessed July 9, 2025, [https://github.com/automatesolutions/WebSocket\_HFT](https://github.com/automatesolutions/WebSocket_HFT)  
7. Low Latency C++ for HFT \- Part 2 \- Building Blocks | Stacy Gaudreau, accessed July 9, 2025, [https://stacygaudreau.com/blog/cpp/low-latency-cpp-for-hft-part2/](https://stacygaudreau.com/blog/cpp/low-latency-cpp-for-hft-part2/)  
8. Lock-Free Single-Producer \- Single Consumer Circular Queue \- CodeProject, accessed July 9, 2025, [https://www.codeproject.com/Articles/43510/Lock-Free-Single-Producer-Single-Consumer-Circular](https://www.codeproject.com/Articles/43510/Lock-Free-Single-Producer-Single-Consumer-Circular)  
9. C++ Low-Latency Magic for HFT: Speed, Cache, and Code Shenanigans | by Mubin Shaikh, accessed July 9, 2025, [https://shaikhmubin.medium.com/c-low-latency-magic-for-hft-speed-cache-and-code-shenanigans-3baed6f0e1e7](https://shaikhmubin.medium.com/c-low-latency-magic-for-hft-speed-cache-and-code-shenanigans-3baed6f0e1e7)  
10. Mastering C++ Low Latency: A Guide to High-Frequency Trading Systems \- Quantlabs.net, accessed July 9, 2025, [https://www.quantlabsnet.com/post/mastering-c-low-latency-a-guide-to-high-frequency-trading-systems](https://www.quantlabsnet.com/post/mastering-c-low-latency-a-guide-to-high-frequency-trading-systems)  
11. How to Ace the Hardest C++ Interview Questions in HFT \- Quantlabs.net, accessed July 9, 2025, [https://www.quantlabsnet.com/post/how-to-ace-the-hardest-c-interview-questions-in-hft](https://www.quantlabsnet.com/post/how-to-ace-the-hardest-c-interview-questions-in-hft)  
12. Introducing Shmipc: A High Performance Inter-process Communication Library, accessed July 9, 2025, [https://www.cloudwego.io/blog/2023/04/04/introducing-shmipc-a-high-performance-inter-process-communication-library/](https://www.cloudwego.io/blog/2023/04/04/introducing-shmipc-a-high-performance-inter-process-communication-library/)  
13. Optimizing TCP for High-Performance Applications: An HFT Developer's Guide, accessed July 9, 2025, [https://dev.to/sid\_hattangadi/optimizing-tcp-for-high-performance-applications-an-hft-developers-guide-1212](https://dev.to/sid_hattangadi/optimizing-tcp-for-high-performance-applications-an-hft-developers-guide-1212)  
14. Benchmarks of Cache-Friendly Data Structures in C++ | Hacker News, accessed July 9, 2025, [https://news.ycombinator.com/item?id=19032293](https://news.ycombinator.com/item?id=19032293)  
15. C++ patterns for low-latency applications including high-frequency trading \- Hacker News, accessed July 9, 2025, [https://news.ycombinator.com/item?id=40908273](https://news.ycombinator.com/item?id=40908273)  
16. The Anatomy of Networking in High-Frequency Trading \- NetDev conference, accessed July 9, 2025, [https://netdevconf.info/0x16/papers/43/pj-netdev-0x16.pdf](https://netdevconf.info/0x16/papers/43/pj-netdev-0x16.pdf)  
17. WebSocket Streams | Binance Open Platform, accessed July 9, 2025, [https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams)  
18. boostorg/beast: HTTP and WebSocket built on Boost.Asio ... \- GitHub, accessed July 9, 2025, [https://github.com/boostorg/beast](https://github.com/boostorg/beast)  
19. zaphoyd/websocketpp: C++ websocket client/server library \- GitHub, accessed July 9, 2025, [https://github.com/zaphoyd/websocketpp](https://github.com/zaphoyd/websocketpp)  
20. Boost Beast Websockets vs. websocketpp : r/cpp \- Reddit, accessed July 9, 2025, [https://www.reddit.com/r/cpp/comments/d6tnvj/boost\_beast\_websockets\_vs\_websocketpp/](https://www.reddit.com/r/cpp/comments/d6tnvj/boost_beast_websockets_vs_websocketpp/)  
21. boost::beast vs uwebsockets library performance c++ \- Stack Overflow, accessed July 9, 2025, [https://stackoverflow.com/questions/77801134/boostbeast-vs-uwebsockets-library-performance-c](https://stackoverflow.com/questions/77801134/boostbeast-vs-uwebsockets-library-performance-c)  
22. uNetworking/uWebSockets: Simple, secure & standards ... \- GitHub, accessed July 9, 2025, [https://github.com/uNetworking/uWebSockets](https://github.com/uNetworking/uWebSockets)  
23. Comparison to Pion · Issue \#602 · boostorg/beast \- GitHub, accessed July 9, 2025, [https://github.com/boostorg/beast/issues/602](https://github.com/boostorg/beast/issues/602)  
24. Asio C++ Library, accessed July 9, 2025, [https://think-async.com/](https://think-async.com/)  
25. rbeeli/websocketclient-cpp: A transport-agnostic, high-performance, header-only C++23 WebSocket client library with minimal dependencies. \- GitHub, accessed July 9, 2025, [https://github.com/rbeeli/websocketclient-cpp](https://github.com/rbeeli/websocketclient-cpp)  
26. Trouble choosing a networking library : r/cpp \- Reddit, accessed July 9, 2025, [https://www.reddit.com/r/cpp/comments/1823vjy/trouble\_choosing\_a\_networking\_library/](https://www.reddit.com/r/cpp/comments/1823vjy/trouble_choosing_a_networking_library/)  
27. epoll: The API that powers the modern internet : r/programming \- Reddit, accessed July 9, 2025, [https://www.reddit.com/r/programming/comments/1947m6n/epoll\_the\_api\_that\_powers\_the\_modern\_internet/](https://www.reddit.com/r/programming/comments/1947m6n/epoll_the_api_that_powers_the_modern_internet/)  
28. io\_uring vs. epoll – Which Is Better in Network Programming ..., accessed July 9, 2025, [https://www.alibabacloud.com/blog/io-uring-vs--epoll-which-is-better-in-network-programming\_599544](https://www.alibabacloud.com/blog/io-uring-vs--epoll-which-is-better-in-network-programming_599544)  
29. io\_uring based networking in prod experience : r/linux \- Reddit, accessed July 9, 2025, [https://www.reddit.com/r/linux/comments/qm09rf/io\_uring\_based\_networking\_in\_prod\_experience/](https://www.reddit.com/r/linux/comments/qm09rf/io_uring_based_networking_in_prod_experience/)  
30. Introduction \- KUCOIN API, accessed July 9, 2025, [https://www.kucoin.com/docs-new/introduction](https://www.kucoin.com/docs-new/introduction)  
31. connect/providers/websockets/okx/README.md at main \- GitHub, accessed July 9, 2025, [https://github.com/skip-mev/connect/blob/main/providers/websockets/okx/README.md](https://github.com/skip-mev/connect/blob/main/providers/websockets/okx/README.md)  
32. Complete Guide to Using the OKX API for Trading and Integration \- WunderTrading, accessed July 9, 2025, [https://wundertrading.com/journal/en/learn/article/okx-api](https://wundertrading.com/journal/en/learn/article/okx-api)  
33. C++ HFT on Crypto Exchanges with μs Latency\! \- Roq Trading Solutions, accessed July 9, 2025, [https://roq-trading.com/docs/blogs/2019-11-20/c++-hft-on-crypto-exchanges-with-%CE%BCs-latency/](https://roq-trading.com/docs/blogs/2019-11-20/c++-hft-on-crypto-exchanges-with-%CE%BCs-latency/)  
34. binance-spot-api-docs/web-socket-api.md at master \- GitHub, accessed July 9, 2025, [https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-api.md](https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-api.md)  
35. miloyip/nativejson-benchmark: C/C++ JSON parser/generator benchmark \- GitHub, accessed July 9, 2025, [https://github.com/miloyip/nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark)  
36. simdjson profiling vs rapidjosn · Issue \#745 \- GitHub, accessed July 9, 2025, [https://github.com/simdjson/simdjson/issues/745](https://github.com/simdjson/simdjson/issues/745)  
37. What is the best C++ JSON library? \- Quora, accessed July 9, 2025, [https://www.quora.com/What-is-the-best-C-JSON-library](https://www.quora.com/What-is-the-best-C-JSON-library)  
38. RapidJSON: Main Page, accessed July 9, 2025, [https://rapidjson.org/](https://rapidjson.org/)  
39. simdjson/simdjson: Parsing gigabytes of JSON per second ... \- GitHub, accessed July 9, 2025, [https://github.com/simdjson/simdjson](https://github.com/simdjson/simdjson)  
40. The simdjson library, accessed July 9, 2025, [https://simdjson.org/](https://simdjson.org/)  
41. New, fastest JSON library for C++20 : r/cpp \- Reddit, accessed July 9, 2025, [https://www.reddit.com/r/cpp/comments/y37pd7/new\_fastest\_json\_library\_for\_c20/](https://www.reddit.com/r/cpp/comments/y37pd7/new_fastest_json_library_for_c20/)  
42. Paper: Parsing Gigabytes of JSON per Second \- Branch Free, accessed July 9, 2025, [https://branchfree.org/2019/02/25/paper-parsing-gigabytes-of-json-per-second/](https://branchfree.org/2019/02/25/paper-parsing-gigabytes-of-json-per-second/)  
43. Recent Json library benchmarks? : r/cpp \- Reddit, accessed July 9, 2025, [https://www.reddit.com/r/cpp/comments/dhy3mx/recent\_json\_library\_benchmarks/](https://www.reddit.com/r/cpp/comments/dhy3mx/recent_json_library_benchmarks/)  
44. Implementing a Limit Order Book for HFT? : r/quant \- Reddit, accessed July 9, 2025, [https://www.reddit.com/r/quant/comments/l3jag4/implementing\_a\_limit\_order\_book\_for\_hft/](https://www.reddit.com/r/quant/comments/l3jag4/implementing_a_limit_order_book_for_hft/)  
45. What is an efficient data structure to model order book?, accessed July 9, 2025, [https://quant.stackexchange.com/questions/3783/what-is-an-efficient-data-structure-to-model-order-book](https://quant.stackexchange.com/questions/3783/what-is-an-efficient-data-structure-to-model-order-book)  
46. Limit Order Book Implementation for Low Latency Trading (in C++) \- alexabosi, accessed July 9, 2025, [https://alexabosi.wordpress.com/2014/08/28/limit-order-book-implementation-for-low-latency-trading-in-c/](https://alexabosi.wordpress.com/2014/08/28/limit-order-book-implementation-for-low-latency-trading-in-c/)  
47. Low latency Limit Order Book and Matching Engine created ... \- GitHub, accessed July 9, 2025, [https://github.com/brprojects/Limit-Order-Book](https://github.com/brprojects/Limit-Order-Book)  
48. How to Build a Fast Limit Order Book \- GitHub Gist, accessed July 9, 2025, [https://gist.github.com/halfelf/db1ae032dc34278968f8bf31ee999a25](https://gist.github.com/halfelf/db1ae032dc34278968f8bf31ee999a25)  
49. False sharing problem. False sharing is a critical issue in… | by Sireanu Roland | Medium, accessed July 9, 2025, [https://medium.com/@sireanu.roland/false-sharing-problem-56d9f4507a5d](https://medium.com/@sireanu.roland/false-sharing-problem-56d9f4507a5d)  
50. False sharing \- Wikipedia, accessed July 9, 2025, [https://en.wikipedia.org/wiki/False\_sharing](https://en.wikipedia.org/wiki/False_sharing)  
51. False Sharing \- The Linux Kernel documentation, accessed July 9, 2025, [https://docs.kernel.org/kernel-hacking/false-sharing.html](https://docs.kernel.org/kernel-hacking/false-sharing.html)