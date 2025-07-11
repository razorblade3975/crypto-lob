#!/bin/bash

# Build script for crypto-lob project
# Usage: ./scripts/build.sh [OPTIONS]

set -euo pipefail

# Default values
BUILD_TYPE="Release"
SANITIZER="OFF"
CLEAN=true # clean up before build by default
JOBS=$(nproc 2>/dev/null || echo "4")
VERBOSE=false
HELP=false
RUN_TESTS=false

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build the crypto-lob project with various configurations.

OPTIONS:
    -t, --type TYPE         Build type: Release, Debug, RelWithDebInfo (default: Release)
    -s, --sanitizer SANI    Sanitizer: OFF, ASAN, TSAN (default: OFF)
    -c, --clean             Clean build directory before building
    -j, --jobs N            Number of parallel jobs (default: $(nproc 2>/dev/null || echo "4"))
    -v, --verbose           Verbose output
    -r, --run-tests         Run unit tests after building
    -h, --help              Show this help message

EXAMPLES:
    $0                              # Release build, no sanitizers
    $0 -t Debug                     # Debug build
    $0 -t Debug -s ASAN            # Debug build with AddressSanitizer
    $0 -t Debug -s TSAN -c         # Debug build with ThreadSanitizer, clean first
    $0 -s ASAN -j 8 -v             # ASAN build with 8 jobs, verbose
    $0 -r                          # Release build and run tests
    $0 -t Debug -s ASAN -r         # Debug ASAN build and run tests

NOTES:
    - ASAN and TSAN cannot be used together
    - Sanitizers require Debug build type
    - Clean build is recommended when switching sanitizers
    - This script must be run from the project root directory
EOF
}

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -s|--sanitizer)
            SANITIZER="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -r|--run-tests)
            RUN_TESTS=true
            shift
            ;;
        -h|--help)
            HELP=true
            shift
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

if [[ "$HELP" == true ]]; then
    usage
    exit 0
fi

# Validate arguments
case "$BUILD_TYPE" in
    Release|Debug|RelWithDebInfo)
        ;;
    *)
        log_error "Invalid build type: $BUILD_TYPE"
        log_error "Valid types: Release, Debug, RelWithDebInfo"
        exit 1
        ;;
esac

case "$SANITIZER" in
    OFF|ASAN|TSAN)
        ;;
    *)
        log_error "Invalid sanitizer: $SANITIZER"
        log_error "Valid sanitizers: OFF, ASAN, TSAN"
        exit 1
        ;;
esac

# Force Debug build for sanitizers
if [[ "$SANITIZER" != "OFF" && "$BUILD_TYPE" != "Debug" ]]; then
    log_warning "Sanitizers require Debug build type. Switching to Debug."
    BUILD_TYPE="Debug"
fi

# Check if we're in the project root
if [[ ! -f "conanfile.txt" ]]; then
    log_error "Must be run from project root directory (where conanfile.txt exists)"
    exit 1
fi

# Check if we're in Docker container (recommended for Linux features)
if [[ ! -f "/.dockerenv" ]]; then
    log_warning "Not running in Docker container. Some Linux features may not be available."
    log_warning "Consider running: docker exec -it crypto-lob-dev bash"
fi

log_info "Build configuration:"
log_info "  Build Type: $BUILD_TYPE"
log_info "  Sanitizer:  $SANITIZER"
log_info "  Jobs:       $JOBS"
log_info "  Clean:      $CLEAN"
log_info "  Verbose:    $VERBOSE"
log_info "  Run Tests:  $RUN_TESTS"

# Clean if requested or switching sanitizers
if [[ "$CLEAN" == true ]]; then
    log_info "Cleaning build directory..."
    rm -rf build/*
fi

# Conan install
log_info "Installing dependencies with Conan..."
CONAN_CMD="conan install . --output-folder=build --build=missing \
    -s compiler=clang -s compiler.version=17 \
    -s compiler.libcxx=libc++ -s compiler.cppstd=20 \
    -s build_type=$BUILD_TYPE"

if [[ "$VERBOSE" == true ]]; then
    log_info "Running: $CONAN_CMD"
fi

eval "$CONAN_CMD"

# CMake configure
log_info "Configuring with CMake..."
cd build

CMAKE_CMD="cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake \
    -DCMAKE_CXX_COMPILER=clang++-17 \
    -DBUILD_TESTS=ON \
    -DENABLE_SANITIZERS=$SANITIZER \
    -G Ninja .."

if [[ "$VERBOSE" == true ]]; then
    log_info "Running: $CMAKE_CMD"
fi

eval "$CMAKE_CMD"

# Build
log_info "Building with Ninja ($JOBS jobs)..."
NINJA_CMD="ninja -j$JOBS"

if [[ "$VERBOSE" == true ]]; then
    NINJA_CMD="$NINJA_CMD -v"
    log_info "Running: $NINJA_CMD"
fi

eval "$NINJA_CMD"

log_success "Build completed successfully!"
log_info "Build artifacts are in: $(pwd)"

# Show available targets
log_info "Available executables:"
find . -maxdepth 1 -type f -executable -name "crypto-lob*" | sed 's/^/  /'

log_info "To run tests: ctest -V"
if [[ "$SANITIZER" != "OFF" ]]; then
    log_info "Sanitizer is enabled. Check for any reported issues in test output."
fi

# Run tests if requested
if [[ "$RUN_TESTS" == true ]]; then
    log_info "Running unit tests..."
    
    # Run the main unit test executable directly, excluding benchmarks
    # The project has crypto-lob-tests for unit tests and separate benchmark executables
    if [[ -f "./crypto-lob-tests" ]]; then
        TEST_CMD="./crypto-lob-tests"
        
        if [[ "$VERBOSE" == true ]]; then
            # Show all test output with timing
            TEST_CMD="$TEST_CMD --gtest_print_time=1"
        else
            # Show test progress with dots
            TEST_CMD="$TEST_CMD --gtest_brief=0"
        fi
        
        log_info "Running: $TEST_CMD"
        
        if eval "$TEST_CMD"; then
            log_success "All unit tests passed!"
        else
            log_error "Some unit tests failed. Check output above for details."
            log_info "To see detailed output, run with -v flag"
            exit 1
        fi
    else
        # Fallback to ctest if crypto-lob-tests doesn't exist
        log_warning "crypto-lob-tests executable not found, using ctest"
        CTEST_CMD="ctest -E benchmark"
        
        if [[ "$VERBOSE" == true ]]; then
            CTEST_CMD="$CTEST_CMD -V"
        else
            CTEST_CMD="$CTEST_CMD --output-on-failure"
        fi
        
        if eval "$CTEST_CMD"; then
            log_success "All unit tests passed!"
        else
            log_error "Some unit tests failed. Check output above for details."
            exit 1
        fi
    fi
fi