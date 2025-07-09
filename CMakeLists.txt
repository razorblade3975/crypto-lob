cmake_minimum_required(VERSION 3.20)
project(crypto-lob VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(ENABLE_SANITIZERS "Enable sanitizers" OFF)

# Compiler-specific flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -O3 -march=native)
    if(ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize=undefined)
    endif()
endif()

# Find required packages
find_package(Boost 1.82.0 REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

# Include directories
include_directories(src)

# Core library
file(GLOB_RECURSE CORE_SOURCES 
    "src/core/*.cpp"
    "src/networking/*.cpp"
    "src/exchanges/*.cpp"
    "src/parsing/*.cpp"
    "src/orderbook/*.cpp"
    "src/ipc/*.cpp"
    "src/threading/*.cpp"
)

add_library(crypto-lob-core STATIC ${CORE_SOURCES})

target_link_libraries(crypto-lob-core 
    PUBLIC
        Boost::system
        Threads::Threads
)

target_include_directories(crypto-lob-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

# Main executable
add_executable(crypto-lob src/main.cpp)
target_link_libraries(crypto-lob crypto-lob-core)

# Tests
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    
    file(GLOB_RECURSE TEST_SOURCES "tests/unit/*.cpp")
    add_executable(crypto-lob-tests ${TEST_SOURCES})
    target_link_libraries(crypto-lob-tests 
        crypto-lob-core
        GTest::GTest
        GTest::Main
    )
    
    add_test(NAME crypto-lob-tests COMMAND crypto-lob-tests)
endif()

# Benchmarks
if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)
    
    file(GLOB_RECURSE BENCH_SOURCES "tests/benchmarks/*.cpp")
    add_executable(crypto-lob-benchmarks ${BENCH_SOURCES})
    target_link_libraries(crypto-lob-benchmarks 
        crypto-lob-core
        benchmark::benchmark
    )
endif()

# Install targets
install(TARGETS crypto-lob crypto-lob-core
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY src/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)