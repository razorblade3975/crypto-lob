# Memory Pool Benchmarks
cmake_minimum_required(VERSION 3.20)

# Find required packages
find_package(benchmark REQUIRED)
find_package(Boost REQUIRED)
find_package(PkgConfig)

# Try to find jemalloc (optional)
set(HAS_JEMALLOC FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(JEMALLOC jemalloc)
    if(JEMALLOC_FOUND)
        set(HAS_JEMALLOC TRUE)
    endif()
endif()

# Common benchmark settings
set(BENCHMARK_COMMON_LIBS
    benchmark::benchmark
    Boost::headers
    pthread
)

# Memory pool comparison benchmark
add_executable(memory_pool_comparison_benchmark
    memory_pool_comparison_benchmark.cpp
)

target_link_libraries(memory_pool_comparison_benchmark
    ${BENCHMARK_COMMON_LIBS}
    crypto_lob_core
)

target_compile_options(memory_pool_comparison_benchmark PRIVATE
    -O3
    -march=native
    -mtune=native
    -fno-omit-frame-pointer  # For profiling
)

# Batch allocation benchmark
add_executable(batch_allocation_benchmark
    batch_allocation_benchmark.cpp
)

target_link_libraries(batch_allocation_benchmark
    ${BENCHMARK_COMMON_LIBS}
    crypto_lob_core
)

target_compile_options(batch_allocation_benchmark PRIVATE
    -O3
    -march=native
    -mtune=native
    -fno-omit-frame-pointer  # For profiling
)

# Memory pool latency benchmark (requires x86_64 for rdtsc)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(memory_pool_latency_benchmark
        memory_pool_latency_benchmark.cpp
    )
    
    target_link_libraries(memory_pool_latency_benchmark
        ${BENCHMARK_COMMON_LIBS}
        crypto_lob_core
    )
    
    # Link jemalloc if found
    if(HAS_JEMALLOC)
        target_link_libraries(memory_pool_latency_benchmark ${JEMALLOC_LIBRARIES})
        target_include_directories(memory_pool_latency_benchmark PRIVATE ${JEMALLOC_INCLUDE_DIRS})
        target_compile_definitions(memory_pool_latency_benchmark PRIVATE HAS_JEMALLOC=1)
    endif()
    
    target_compile_options(memory_pool_latency_benchmark PRIVATE
        -O3
        -march=native
        -mtune=native
        -mno-omit-leaf-frame-pointer  # Better profiling
        -fno-exceptions  # Match HFT requirements
    )
    
    # Enable CPU-specific optimizations
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(memory_pool_latency_benchmark PRIVATE
            -fvectorize
            -fslp-vectorize
            -ffast-math
        )
    endif()
endif()

# Add benchmark targets to CTest
add_test(NAME benchmark_memory_pool_comparison 
         COMMAND memory_pool_comparison_benchmark --benchmark_format=json --benchmark_out=comparison_results.json)
         
if(TARGET memory_pool_latency_benchmark)
    add_test(NAME benchmark_memory_pool_latency 
             COMMAND memory_pool_latency_benchmark)
endif()

# Simple latency test (works on all architectures)
add_executable(simple_latency_test
    simple_latency_test.cpp
)

target_link_libraries(simple_latency_test
    ${BENCHMARK_COMMON_LIBS}
    crypto_lob_core
)

target_compile_options(simple_latency_test PRIVATE
    -O3
    -march=native
    -mtune=native
)

# Custom target to run all benchmarks
set(BENCHMARK_TARGETS memory_pool_comparison_benchmark simple_latency_test)
if(TARGET memory_pool_latency_benchmark)
    list(APPEND BENCHMARK_TARGETS memory_pool_latency_benchmark)
endif()

add_custom_target(run_memory_benchmarks
    COMMAND ${CMAKE_CTEST_COMMAND} -R "benchmark_memory_pool_*" -V
    DEPENDS ${BENCHMARK_TARGETS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running memory pool benchmarks..."
)