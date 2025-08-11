# High-Performance C++ Crypto Market Data Provider Architecture

## 1. High-Level Overview and Design Principles

This system is a **crypto order book builder** designed for **high performance and ultra-low latency** parsing and bookkeeping of exchange order book data. Its purpose is to collect real-time market data updates (order book updates, trades, etc.) from multiple cryptocurrency exchanges and promptly update internal order book to reflect the update. The core architectural philosophy centers on minimizing latency at every step, enforcing a **single-responsibility design** for clarity and maintainability, and leveraging **cache-aware** optimizations in data structures.

### Design Principles

1. **Performance Above All**: Every design decision should prioritize microsecond-level latency. Performance target is <50μs wire-to-book.
2. **Use Compile-Time Configuration**: To use Templates and traits to move decisions from runtime to compile-time.
3. **Use Lock-Free Architecture**: To Eliminate contention in critical trading paths.
4. **Memory Efficiency**: Use Pre-allocation and pooling to eliminate dynamic allocation.
5. **Cache Optimization**: Use Data structures designed for CPU cache efficiency. Avoid false sharing.
6. **Comprehensive Testing**: Unit tests, benchmarks, and performance assertions.
7. **Clear Naming**: Consistent conventions across the entire codebase.
8. **Modular Organization**: Clear separation of concerns with explicit dependencies.
9. **Fail fast**: Use std::terminate instead of try-catch exception handling.

### Technology Stack

- Configuration: tomlplusplus
- Logging: spdlog
- JSON Parsing: simdjson
- WebSocket Client: Boost.Beast

## 2. System Components and Interactions

The market data provider consists of several key components running concurrently and interacting in a pipeline. 

1. A dedicated **Control Thread** oversees the system, handling TOML configuration, and startup/shutdown of components.
2. **Exchange Connectors** (one per exchange, e.g. `BinanceSpotConnector`) maintain live WebSocket connections to exchanges and receive raw market data feeds via WebSocket.
3. Incoming JSON exchange messages flow into the **Parser** component (one per exchange, such as `BinanceSpotJsonParser`, running on its own thread), which translates raw updates into a unified **NormalizedMessage** format understood by the rest of the system.
4. These normalized updates are then passed to the **Order Book Engine** (which contains an `OrderBookManager` managing multiple `OrderBook` for all configured trading pairs). The Order Book Engine updates the in-memory `OrderBook` for the relevant instrument.

## 2.1 Thread model

1. **Control Thread** (1 core)
   - Loads configuration in TOML, sets thread affinities for each thread, starts/stops components.
   - Monitors health and handles management commands.
   - Not on hot path.

2. **Exchange Connectors - WebSocket Connection Threads** (N cores, 1 per exchange)
   - One per exchange (e.g. `BinanceSpotConnector`), runs on dedicated core.
   - Maintains persistent WebSocket connection to venue.
   - Handles authentication, heartbeats, and raw frame reception.
   - Pushes raw messages into a lock-free **SPSC** queue (one per exchange) to its Parser thread.

3. **Parser Threads** (N cores, 1 per exchange)
   - One per exchange (e.g. `BinanceSpotJsonParser`), runs on dedicated core.
   - Decodes venue-specific raw JSON from the SPSC queue into unified message type `NormalizedMessage`, using **simdjson**.
   - `NormalizedMessage` objects are pre-allocated in a memory pool to avoid allocation overhead.
   - No networking or order book logic; purely format translation.
   - Publishes normalized updates into a SPSC queue to the Order Book Engine.

4. **Order Book Threads** (1 core)
   - One thread, sharded by instrument/symbol group.
   - Owns `BookManager` and per-symbol `OrderBook` instances.
   - Applies deltas/snapshots to in-memory books with sequence checks.

## 3. Latency-Critical Hot Path Flow

1. **Exchange Connector Thread Receives Data:** A WebSocket packet (e.g. an order book delta from Binance) arrives from the network. The **ExchangeConnector** thread dedicated to that connection immediately reads the packet from the socket. This is a raw JSON message containing the exchange’s update. The connector performs lightweight preprocessing (e.g. framing, basic validation) then **enqueues the raw message into an SPSC ring buffer** shared with the **Parser** stage. If queue is full, we should log and drop the newest.

2. **Parser Thread Decodes Message:** The Parser thread (running in parallel on another core) continuously polls the parse queue for new messages. When the raw message is available, the Parser thread dequeues it and **parses the exchange-specific format into a NormalizedMessage object**. For example, it will decode the JSON string into a structured update (fields like symbol, side, price, quantity, etc., in a unified format). This stage is optimized to avoid memory allocations – e.g. using a pre-allocated buffer or streaming parser to extract values without creating temporary objects, which is crucial for predictable low latency. The result is a compact NormalizedMessage representing the update. Once ready, the Parser thread enqueues the NormalizedMessage into a SPSC queue for the Order Book Engine. Again, this uses a lock-free ring buffer structure so that the OrderBook thread can later retrieve it without contention. If the queue is full, we should log and drop the newest.

3. **Order Book Engine Updates State:** The OrderBook thread waits on the OrderBook queue and, upon receiving a new NormalizedMessage, it processes the update. This involves finding the correct OrderBook (by instrument and market) and applying the delta: e.g. adding, modifying, or removing an order or price level, or updating trade volume. The **Order Book Engine** is highly optimized – it uses data structures engineered for fast updates and iterations. The complexity of an update is kept low, and **no locks** are needed since each book is updated by a single thread.

In summary, the hot path is a carefully constructed pipeline where **each thread does a small amount of work then hands off** to the next via a fast, non-blocking queue. This maximizes parallelism and throughput while keeping per-message latency very low and predictable. Performance measurements in similar systems show that such pipelines can process hundreds of thousands of messages per second with sub-millisecond end-to-end latency, thanks to avoiding unnecessary waits and leveraging CPU cache efficiency for data handling.

## 4. Key Modules and Classes

The system’s codebase is organized into modules and classes corresponding to the components above. Below is a breakdown of the **key classes/modules** and their roles and interactions:

**`ExchangeConnector` (abstract class):** Defines a generic interface for an exchange WebSocket connection.

It should have interface that manages the WebSocket connection to a crypto exchange and handles subscription to relevant market data streams. Responsibilities include connecting/reconnecting (with exponential backoff), listening for incoming messages, and pushing raw data into the SPSC queue consumed by `Parser`.

Each connector runs in its own thread context. The design expects one `ExchangeConnector` per exchange (or even per exchange instance/market), encapsulating all exchange-specific logic here. It collaborates with the Parser (by forwarding raw messages to be parsed) and the `ControlThread` (which can instruct it to start/stop).

**`BinanceSpotConnector` (concrete class):** These are subclasses of `ExchangeConnector` implementing exchange-specific details (e.g. URL endpoints, authentication if needed, message format specifics). For instance, `BinanceSpotConnector` knows the Binance Spot Market WebSocket endpoints and message schemas. When a message arrives, it produces a message for the Parser.

**`Parser` (abstract class):** Defines a generic interface for translating raw JSON exchange messages from the `ExchangeConnector` into the **NormalizedMessage** structure.

**`BinanceSpotJsonParser` (concrete class)** The concrete Parser that understands Binance Spot message format (JSON fields) and performs JSON parsing. 

For example, it will parse Binance JSON updates by extracting fields like `price` and `quantity` without creating intermediate strings using simdjson. 

The output is a populated `NormalizedMessage`. The Parser ensures that regardless of exchange differences (e.g. some exchanges send updates as deltas, others as snapshots), the resulting `NormalizedMessage` has a consistent schema. After parsing, it hands off the `NormalizedMessage` to the OrderBook engine via the queue. In summary, this module isolates all data format heterogeneity and produces clean, consumable update objects.

**`NormalizedMessage` (struct):** A lightweight POD data structure that holds a normalized market data update. 

It typically contains fields such as: `exchange_id`, `instrument_id`, update type (e.g. snapshot, delta update, trade), update_id (to detect gaps), and data details like price levels or trades. This is the **common format** that the `OrderBook` engine expects, decoupling the upstream format from internal processing. 

By making this a simple POD struct, the system can copy and pass these messages efficiently.

This normalization step is crucial for supporting multiple exchanges – it gives the rest of the system a uniform view of market events.

**`OrderBook` (class):** Represents the full order book for a single trading pair (instrument). It maintains the current state of all active buy orders (bids) and sell orders (asks) at various price levels. The OrderBook class provides methods to insert, modify, or delete levels as instructed. This class is used by OrderBookManager to maintain per-symbol books.

**`OrderBookManager` (class):** Coordinates all the OrderBook instances and the incoming data stream. 

It runs in the OrderBook thread.

The `OrderBookManager` holds a mapping from symbol (`instrument_id`) -> `OrderBook` object.

When it receives a `NormalizedMessage` from the SPSC queue, it routes it to the correct OrderBook. It then invokes the OrderBook’s update methods to apply the change. If the update is an incremental delta, the manager ensures sequence continuity (using sequence numbers in the NormalizedMessage if provided – it will drop if out-of-order updates are detected as per exchange requirements). For a snapshot message, it will load the OrderBook from scratch.

**`ControlThread` (management/orchestration class):** Oversees the entire system’s operation. This is essentially the **main thread** that initializes all components, and then manages runtime events. At startup, ControlThread loads TOML configuration that lists which exchanges and symbols to subscribe to, as well as parameters like thread pinning, etc. 

It then creates an `ExchangeConnector` for each exchange, assigns threads, and establishes the `Parser` and `OrderBook` threads. 

During operation, the ControlThread monitors each component (via heartbeats or simply catching thread exceptions). If a `ExchangeConnector` disconnects, ControlThread can restart it.

It can also handle configuration updates or management commands – for instance, an admin could ask for logging the current `OrderBook` top levels for a given instrument.

Essentially, ControlThread is the “brain” coordinating the other modules, without directly processing market data itself. It ensures the system remains **robust and in sync** with the exchanges and responds to external inputs.

## TOML configuration file example

config.toml:

```toml
# High-Performance Crypto Market Data Provider Configuration
# This file configures exchanges, symbols, threading, and performance parameters

[system]
log_level = "debug"  # debug, info, warn, error
log_file = "/workspace/output/all.log"

[threading]
# CPU core assignments (set to -1 for no pinning)
control_thread_core = 0
orderbook_thread_core = 1

# Exchanges configuration
[exchanges.binance_spot]
connector_core = 2
parser_core = 3
websocket_url = "wss://stream.binance.com:9443/ws"
reconnect_delay_ms = 1000
max_reconnect_attempts = 10
ping_interval_sec = 20
subscriptions = [
   "avaxusdt@depth",
   "avaxusdt@depth20@1000ms",
]

# Development/Testing overrides
[debug]
unused = true
```
