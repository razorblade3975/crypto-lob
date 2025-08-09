# High-Performance Crypto Market Data Provider - Implementation TODO

## **Phase 0: Project Setup & Core Utilities**

This initial phase focuses on establishing the development environment, foundational libraries, and shared utilities that all other components will depend on.

### **To-Do List:**

* **1. Build & Dependency Management:**
    * [ ] Set up a **CMake** build system for the project.
    * [ ] Integrate a dependency manager (like Conan or vcpkg) or establish a manual library integration process.
    * [ ] Select and integrate core third-party libraries:
        * **WebSocket Client:** A high-performance, low-level C++ library (e.g., `uWebSockets`, `Boost.Beast`).
        * **JSON Parser:** A fast, low-allocation parser (e.g., `simdjson`, `RapidJSON`).
        * **Logging:** A performant, asynchronous logging library (e.g., `spdlog`).

* **2. Core Utilities Development:**
    * [ ] **Lock-Free Queues:**
        * [ ] Implement or integrate a battle-tested, header-only lock-free queue library.
    * [ ] **Shared Memory Wrapper:**
        * [ ] Create a C++ wrapper class abstracting platform-specific shared memory APIs (`shm_open`, `mmap` on Linux; `CreateFileMapping` on Windows).
        * [ ] The wrapper should handle creation, opening, mapping, and cleanup of shared memory segments.
    * [ ] **CPU Affinity Helper:**
        * [ ] Implement a utility function to pin threads to specific CPU cores to reduce OS scheduler jitter. This will be used by the `ControlThread` when launching components.

---

## **Phase 1: Foundational Data Structures**

This phase defines the common language of the system: the `NormalizedMessage`. It's a critical piece that decouples the exchange-specific formats from the core processing logic.

### **To-Do List:**

* **1. `NormalizedMessage` Struct:**
    * [ ] Define the `NormalizedMessage` as a Plain-Old-Data (POD) `struct` for efficient copying.
    * [ ] Include all necessary fields as described in the guide:
        * `exchange_id` (enum or integer).
        * `symbol_id` (integer for fast lookups).
        * `update_type` (enum: `SNAPSHOT`, `DELTA`, `TRADE`).
        * `sequence_number` or `update_id` from the exchange.
        * For deltas/snapshots: Contiguous arrays (e.g., `std::array` or C-style arrays) for bid and ask levels (`price`, `quantity`).
        * For trades: `price`, `quantity`, `side`.
    * [ ] Ensure the structure is tightly packed to optimize cache line usage.

* **2. `Parser` Module (Interface):**
    * [ ] Define an abstract base class or conceptual interface for all parsers.
    * [ ] The primary function should be `parse(const RawMessage& raw, NormalizedMessage& out_msg)`, designed to be allocation-free by writing into a pre-allocated output object.

---

## **Phase 2: The Order Book Engine**

This is the core stateful component of the system. The implementation must be highly optimized for fast, in-memory updates, directly reflecting the cache-aware design principles.

### **To-Do List:**

* **1. `OrderBook` Class:**
    * [ ] **Data Structures:**
        * [ ] Implement the bid and ask sides using **sorted `std::vector`s** of `PriceLevel` structs, not pointer-based maps, to ensure data locality.
        * [ ] A `PriceLevel` struct should contain `price` and `aggregate_quantity`.
    * [ ] **Core Methods:**
        * [ ] `applyUpdate(const NormalizedMessage& msg)`: The main entry point to modify the book. This method will dispatch to more specific private methods based on the update type.
        * [ ] `applySnapshot(const NormalizedMessage& msg)`: Clears existing state and populates the book from a full snapshot.
        * [ ] Methods to add, update, or remove price levels efficiently. Updates should use binary search (`std::lower_bound`) to find the price level, resulting in O(log N + M) performance, where M is the number of levels to shift.
    * [ ] **State Management:**
        * [ ] Store the current `sequence_number` or `update_id` to detect gaps.

* **2. `OrderBookManager` Class:**
    * [ ] **Data Structures:**
        * [ ] Use a hash map (`std::unordered_map`) to map a `symbol_id` to a unique `OrderBook` instance: `std::unordered_map<int, OrderBook>`.
    * [ ] **Core Logic:**
        * [ ] Implement the main processing loop that runs on the Order Book Engine thread.
        * [ ] The loop should continuously dequeue `NormalizedMessage` objects from the input queue.
        * [ ] Route incoming messages to the correct `OrderBook` instance based on `symbol_id`.
        * [ ] Define a clear state for a symbol's OrderBook when it's out of sync (e.g., STALE, SYNCING, LIVE). This state should be publishable to consumers.
        * [ ] Perform sequence number checks. If a gap is detected, flag the book for re-synchronization.
    * [ ] **Output Generation:**
        * [ ] After an `OrderBook` is updated, generate an output event (e.g., top-of-book change, full depth update).
        * [ ] Enqueue this output event into the MPSC queue leading to the `IPCPublisher`.

---

## **Phase 3: Data Ingestion Components**

This phase involves connecting to the outside world. The implementation will follow the single-responsibility principle, separating connection logic from parsing logic.

### **To-Do List:**

* **1. `ExchangeConnector` Abstract Class:**
    * [ ] Define the base class with pure virtual functions:
        * `connect()`
        * `subscribe(const std::vector<std::string>& symbols)`
        * `run()` (the main event loop for the thread)
        * `disconnect()`
        * `fetchSnapshot(const std::string& symbol)` (for REST API calls)
    * [ ] The `run()` method's implementation in concrete classes will handle receiving WebSocket frames and pushing them to the SPSC queue for the `Parser`.
    * [ ] Implement a robust reconnection strategy with exponential backoff to avoid spamming an exchange's servers during an outage.

* **2. Concrete `BinanceConnector` Implementation:**
    * [ ] Inherit from `ExchangeConnector`.
    * [ ] Implement the WebSocket connection logic specific to Binance's endpoints.
    * [ ] Implement the subscription message format for Binance streams (e.g., `{"method": "SUBSCRIBE", ...}`).
    * [ ] Implement the `fetchSnapshot` method using an HTTP client to call the Binance `/api/v3/depth` REST endpoint.
    * [ ] The `run()` loop should handle WebSocket pings/pongs and reconnect logic.

* **3. Concrete `Parser` for Binance:**
    * [ ] Implement the `Parser` interface for Binance's JSON message formats.
    * [ ] Use the selected high-performance JSON library (`simdjson`) to extract fields with zero or minimal allocations.
    * [ ] Write translators for different message types (`depthUpdate`, `trade`) into the `NormalizedMessage` struct.

---

## **Phase 4: Data Publication (IPC)**

This component is responsible for fanning out the processed data to local consumers with minimal latency.

### **To-Do List:**

* **1. `IPCPublisher` Class:**
    * [ ] **Initialization:**
        * [ ] In the constructor or an `init()` method, use the shared memory wrapper to create/open a segment of a configured size.
        * [ ] Set up a **ring buffer** structure within the shared memory. This should include metadata like a write index, read indices for consumers (if needed), and sequence numbers to signal new data.
    * [ ] **Processing Loop:**
        * [ ] Implement the main loop that runs on the IPC Publisher thread.
        * [ ] The loop will dequeue processed updates from the `OrderBookManager`'s output queue.
        * [ ] **Serialization:** Serialize the update message into a compact binary format.
        * [ ] **Writing:** Copy the serialized data into the next available slot in the shared memory ring buffer.
        * [ ] **Notification:** Atomically update the write index or a sequence number to make the data visible to consumers. This atomic update serves as the publication commit.

---

## **Phase 5: Orchestration and Control**

This is the brain of the application, responsible for starting, stopping, and managing the lifecycle of all components.

### **To-Do List:**

* **1. `ControlThread` / Main Application Logic:**
    * [ ] **Configuration Loading:**
        * [ ] Implement logic to parse a configuration file (e.g., YAML or JSON) that specifies exchanges, symbols, thread affinities, and queue sizes.
    * [ ] **Component Initialization (Startup Sequence):**
        * [ ] 1. Initialize `IPCPublisher` to set up the shared memory.
        * [ ] 2. Initialize `OrderBookManager`.
        * [ ] 3. For each configured exchange:
            * Instantiate the corresponding `ExchangeConnector` and `Parser`.
            * Create the SPSC queue between them.
            * Launch the Connector and Parser on dedicated threads, setting CPU affinity using the utility.
        * [ ] 4. Launch the `OrderBookManager` and `IPCPublisher` threads, setting their CPU affinities.
    * [ ] **Snapshot and Sync Orchestration:**
        * [ ] After connecting, instruct each `ExchangeConnector` to fetch the initial snapshot for its symbols.
        * [ ] Route the snapshot data to the `OrderBookManager` to initialize the `OrderBook` instances.
        * [ ] Signal the `OrderBookManager` to begin processing the live delta stream only after the snapshot is applied.
    * [ ] **Health Monitoring & Recovery:**
        * [ ] Implement logic to monitor the health of connector threads.
        * [ ] If a connection drops, trigger a reconnect and a full re-synchronization flow (snapshot fetch -> book reset -> stream resume).
    * [ ] **Graceful Shutdown:**
        * [ ] Implement a shutdown sequence that signals all threads to stop, disconnects from exchanges, and cleans up shared memory.

---

## **Phase 6: Testing and Performance Tuning**

Continuous testing and profiling are crucial for a low-latency system.

### **To-Do List:**

* **1. Unit Testing:**
    * [ ] `Parser`: Test with various valid and malformed JSON messages.
    * [ ] `OrderBook`: Test edge cases for updates (e.g., adding to top, removing from middle, clearing a side).
    * [ ] `OrderBookManager`: Test sequence gap detection and routing logic.

* **2. Integration Testing:**
    * [ ] Test the full pipeline from a mock `ExchangeConnector` to the `IPCPublisher`.
    * [ ] Verify data integrity in the shared memory segment.
    * [ ] Test the snapshot synchronization and recovery flow.

* **3. Performance Benchmarking:**
    * [ ] Measure end-to-end latency: from a message's arrival at the connector to its publication in shared memory.
    * [ ] Profile CPU usage to identify hot spots in the parsing or order book update logic.
    * [ ] Measure queue depths under heavy load to ensure they are sized appropriately and to identify potential bottlenecks.
    * [ ] Use profiling tools (e.g., `perf`, Intel VTune) to analyze cache miss rates and optimize data structures accordingly.