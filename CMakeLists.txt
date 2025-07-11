cmake_minimum_required(VERSION 3.20)
project(crypto-lob VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find Conan-generated package config files
list(APPEND CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR})
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR})

# Build options
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
set(ENABLE_SANITIZERS "OFF" CACHE STRING "Enable sanitizers: OFF, ASAN, TSAN")
set_property(CACHE ENABLE_SANITIZERS PROPERTY STRINGS "OFF" "ASAN" "TSAN")

# Compiler-specific flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -O3 -march=native)
    
    # Clang-specific optimizations for HFT
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(
            -flto=thin                    # Fast link-time optimization
            -fstrict-aliasing            # Enable strict aliasing optimizations
            -ffast-math                  # Allow aggressive FP optimizations
            -fno-rtti                    # Disable RTTI (not needed for HFT)
            -fomit-frame-pointer         # Free up register
            -momit-leaf-frame-pointer    # Optimize leaf functions
        )
        
        # Use libc++ with Clang
        add_compile_options(-stdlib=libc++)
        add_link_options(-stdlib=libc++)
        
        # Enable thin LTO for linking
        add_link_options(-flto=thin)
    endif()
    
    # Sanitizer configuration
    if(ENABLE_SANITIZERS STREQUAL "ASAN")
        message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
        add_compile_options(-fsanitize=address -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize=undefined)
    elseif(ENABLE_SANITIZERS STREQUAL "TSAN")
        message(STATUS "Enabling ThreadSanitizer")
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
        # ThreadSanitizer is incompatible with AddressSanitizer/UBSan
    endif()
endif()

# Find required packages
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(simdjson REQUIRED)
find_package(tomlplusplus REQUIRED)

# Include directories
include_directories(src)

# Core library - commented out until source files exist
# file(GLOB_RECURSE CORE_SOURCES 
#     "src/core/*.cpp"
#     "src/networking/*.cpp"
#     "src/exchanges/*.cpp"
#     "src/parsing/*.cpp"
#     "src/orderbook/*.cpp"
#     "src/ipc/*.cpp"
#     "src/threading/*.cpp"
# )

# add_library(crypto-lob-core STATIC ${CORE_SOURCES})

# target_link_libraries(crypto-lob-core 
#     PUBLIC
#         Boost::boost
#         Boost::system
#         Threads::Threads
#         simdjson::simdjson
#         tomlplusplus::tomlplusplus
# )

# target_include_directories(crypto-lob-core
#     PUBLIC
#         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
#         $<INSTALL_INTERFACE:include>
# )

# Main executable
add_executable(crypto-lob src/main.cpp)
target_link_libraries(crypto-lob 
    Boost::boost
    Boost::system
    Threads::Threads
    simdjson::simdjson
    tomlplusplus::tomlplusplus
)

# Tests
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    
    file(GLOB_RECURSE TEST_SOURCES "tests/unit/*.cpp")
    add_executable(crypto-lob-tests ${TEST_SOURCES})
    target_link_libraries(crypto-lob-tests 
        Boost::boost
        Boost::system
        Threads::Threads
        simdjson::simdjson
        tomlplusplus::tomlplusplus
        GTest::gtest
        GTest::gtest_main
    )
    
    # Include directories for tests
    target_include_directories(crypto-lob-tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    
    add_test(NAME crypto-lob-tests COMMAND crypto-lob-tests)
    
    # Configure TSAN suppression file if ThreadSanitizer is enabled
    if(ENABLE_SANITIZERS STREQUAL "TSAN")
        set_tests_properties(crypto-lob-tests PROPERTIES
            ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tsan.suppressions"
        )
    endif()
endif()

# Benchmarks - TODO
# if(BUILD_BENCHMARKS)
#     find_package(benchmark REQUIRED)
    
#     file(GLOB_RECURSE BENCH_SOURCES "tests/benchmarks/*.cpp")
#     add_executable(crypto-lob-benchmarks ${BENCH_SOURCES})
#     target_link_libraries(crypto-lob-benchmarks 
#         crypto-lob-core
#         benchmark::benchmark
#     )
# endif()

# Install targets
install(TARGETS crypto-lob
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY src/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)