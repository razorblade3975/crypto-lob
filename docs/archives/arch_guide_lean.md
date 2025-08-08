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
