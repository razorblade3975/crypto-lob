# Crypto LOB (Limit Order Book) Builder

A high-performance cryptocurrency order book builder designed for ultra-low latency parsing and bookkeeping of exchange market data. Built with C++20 and optimized for deterministic performance in high-frequency trading environments.

## 🎯 Project Focus

This system is a **crypto order book builder** that collects real-time market data updates from cryptocurrency exchanges and maintains accurate, low-latency internal order books. The architecture prioritizes microsecond-level latency with a target of <50μs wire-to-book processing time.

## 🚀 Features

- **Ultra-Low Latency**: <50μs wire-to-book processing target
- **Lock-Free Architecture**: SPSC ring buffers for thread communication
- **Thread-Per-Component Model**: Dedicated cores for connector, parser, and order book threads
- **Zero-Copy Design**: Pre-allocated memory pools, no allocations on hot path
- **High-Performance Parsing**: simdjson for <0.5μs JSON parsing
- **Cache-Optimized**: Data structures designed for CPU cache efficiency
- **Fail-Fast Philosophy**: std::terminate instead of exceptions for HFT requirements

## 🏗️ Architecture

### Core Components

Following the single-responsibility principle, each component has one clear purpose:

1. **Control Thread** - System orchestration and configuration management
2. **Exchange Connectors** - WebSocket connections and raw message reception (e.g., `BinanceSpotConnector`)
3. **Parser Threads** - JSON to normalized message translation (e.g., `BinanceSpotJsonParser`)
4. **Order Book Engine** - Order book state management (`OrderBookManager` and `OrderBook`)

### Thread Model

```
┌─────────────┐      SPSC Queue       ┌─────────────┐      SPSC Queue      ┌──────────────┐
│  Connector  │ ──────────────────►   │   Parser    │ ──────────────────►  │ Order Book   │
│  Thread     │    (RawMessage*)      │   Thread    │  (NormalizedMsg*)    │   Engine     │
│  (Core A)   │                       │  (Core B)   │                      │  (Core C)    │
└─────────────┘                       └─────────────┘                      └──────────────┘
```

### Performance Features

- **Lock-Free SPSC Queues**: Zero-contention message passing between threads
- **Memory Pools**: Pre-allocated `NormalizedMessage` objects
- **Fixed-Point Arithmetic**: 10^9 scaled integer prices for precision
- **Cache-Aligned Structures**: 64-byte aligned for optimal cache usage
- **CPU Core Pinning**: Dedicated cores for each thread to avoid context switches

## 🛠️ Technology Stack

- **Language**: C++20 (templates, concepts, constexpr)
- **Compiler**: Clang 17+ with libc++
- **Build System**: CMake with Conan 2.0
- **Configuration**: tomlplusplus
- **Logging**: spdlog (asynchronous)
- **JSON Parsing**: simdjson
- **WebSocket**: Boost.Beast
- **Platform**: Linux (optimized for server deployment)

## 📦 Installation

### Prerequisites

- Docker (recommended) OR:
  - Clang 17+ compiler
  - CMake 3.20+
  - Conan 2.0+
  - Linux OS

### Build Instructions

#### Docker Development (Recommended)

```bash
# Clone the repository
git clone https://github.com/razorblade3975/crypto-lob.git
cd crypto-lob

# Build and start development container
docker compose up -d crypto-lob-dev

# Build the project
docker exec crypto-lob-dev bash -c "cd /workspace && ./scripts/build.sh -t Debug"

# Run tests
docker exec crypto-lob-dev bash -c "cd /workspace/build && ./crypto-lob-tests"
```

#### Native Build (Linux)

```bash
# Install dependencies
conan install . --output-folder=build --build=missing \
      -s compiler=clang -s compiler.version=17 \
      -s compiler.libcxx=libc++ -s compiler.cppstd=20

# Configure and build
cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake ..
ninja

# Run tests
./crypto-lob-tests
```

## ⚙️ Configuration

### TOML Configuration Example

```toml
# config.toml
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
   "btcusdt@depth",
   "ethusdt@depth",
]
```

## 📊 Performance Characteristics

| Component | Latency | Notes |
|-----------|---------|-------|
| JSON Parsing (simdjson) | <0.5μs | Per message |
| SPSC Queue Operation | ~20ns | Lock-free |
| Memory Pool Allocation | ~100ns | Thread-local cache |
| Order Book Update | <1μs | Cache-optimized |
| End-to-End (wire-to-book) | <50μs | Target latency |

## 🧪 Testing

```bash
# Run all unit tests
./scripts/build.sh -t Debug -r

# Run with AddressSanitizer
./scripts/build.sh -t Debug -s ASAN -r

# Run with ThreadSanitizer
./scripts/build.sh -t Debug -s TSAN -r

# Run specific test suite
./build/crypto-lob-tests --gtest_filter=BinanceSpotParserTest.*
```

## 📈 Development Status

### ✅ Completed Components

- **Core Infrastructure**
  - [x] Lock-free SPSC ring buffer implementation
  - [x] Thread-safe memory pool with pre-allocation
  - [x] Fixed-point Price class (10^9 scaling)
  - [x] RDTSC timestamp utilities for low-latency timing
  
- **Order Book Components**
  - [x] BookSide implementation with RB-tree and hash map
  - [x] OrderBook class with bid/ask integration
  - [x] Top-of-book tracking and caching
  
- **Exchange Integration**
  - [x] Parser base class with memory pool integration
  - [x] BinanceSpotJsonParser implementation
  - [x] NormalizedMessage and RawMessage structures
  - [x] Comprehensive Binance parser tests (17 test cases)

- **Build & Testing**
  - [x] CMake build system with Conan integration
  - [x] Docker development environment
  - [x] Comprehensive unit test suite (177 tests)
  - [x] CI/CD pipeline with GitHub Actions

### 🚧 In Progress

- **Exchange Connectors**
  - [ ] WebSocket client base class
  - [ ] BinanceSpotConnector implementation
  - [ ] Reconnection and heartbeat logic
  
- **Order Book Engine**
  - [ ] OrderBookManager implementation
  - [ ] Sequence number validation
  - [ ] Snapshot/delta synchronization

- **Control Thread**
  - [ ] TOML configuration loading
  - [ ] Component lifecycle management
  - [ ] Thread affinity setting

### 📋 Roadmap

**Phase 1 - Core Pipeline (Current)**
- Complete WebSocket connector infrastructure
- Implement OrderBookManager with routing logic
- Add control thread with basic lifecycle management

**Phase 2 - Production Features**
- Add monitoring and metrics collection
- Implement graceful shutdown and recovery
- Add support for multiple symbols per exchange

**Phase 3 - Exchange Expansion**
- Add support for additional exchanges (OKX, KuCoin)
- Implement exchange-specific optimizations
- Add futures/derivatives support

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes with comprehensive tests
4. Ensure all tests pass and code is formatted
5. Submit a pull request

### Development Guidelines

- Follow the architecture guide in `docs/arch_guide.md`
- Use `clang-format-17` for code formatting
- Write comprehensive unit tests for new features
- Avoid dynamic allocations on the hot path
- Document performance-critical sections

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- [simdjson](https://github.com/simdjson/simdjson) for ultra-fast JSON parsing
- [Boost.Beast](https://www.boost.org/doc/libs/master/libs/beast/) for WebSocket support
- [tomlplusplus](https://github.com/marzer/tomlplusplus) for configuration parsing
- [spdlog](https://github.com/gabime/spdlog) for high-performance logging

## 📞 Support

For questions and support:
- Create an [Issue](https://github.com/razorblade3975/crypto-lob/issues)
- Review the [Architecture Guide](./docs/arch_guide.md)
- Check the [Development Notes](./CLAUDE.md)

---

*Built for ultra-low latency cryptocurrency order book management*
