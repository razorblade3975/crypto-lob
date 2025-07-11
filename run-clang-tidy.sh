#!/bin/bash

# Run clang-tidy in the Docker container with proper filtering

echo "Running clang-tidy on crypto-lob source files..."

# Find all source and header files, excluding build directories
docker exec -w /workspace crypto-lob-dev bash -c "
    find src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) -not -path './build/*' | \
    xargs clang-tidy-17 -p build --header-filter='.*/(src|tests)/.*' 2>&1 | \
    grep -E '^(/workspace/|[0-9]+ warnings)' | \
    grep -v 'Suppressed' | \
    head -n 100
"

echo -e "\nTo see full output without filtering, run:"
echo "docker exec -w /workspace crypto-lob-dev bash -c \"find src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) | xargs clang-tidy-17 -p build --header-filter='.*/(src|tests)/.*'\""