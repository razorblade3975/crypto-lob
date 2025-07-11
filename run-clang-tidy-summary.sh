#!/bin/bash

echo "Running clang-tidy analysis on crypto-lob project..."
echo "================================================="

# Create a temporary file to store results
TEMP_FILE=$(mktemp)

# Run clang-tidy on all source files
docker exec -w /workspace crypto-lob-dev bash -c "
    find src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) -not -path './build/*' | \
    xargs clang-tidy-17 -p build --header-filter='.*/(src|tests)/.*' 2>&1
" > "$TEMP_FILE"

# Extract and count warnings by category
echo -e "\nWarning Summary by Category:"
echo "----------------------------"
grep -E "warning:.*\[" "$TEMP_FILE" | \
    sed -E 's/.*\[(.*)\]$/\1/' | \
    sort | uniq -c | sort -rn | head -20

# Show files with the most warnings
echo -e "\nFiles with Most Warnings:"
echo "------------------------"
grep -E "^/workspace/(src|tests)" "$TEMP_FILE" | \
    cut -d: -f1 | sort | uniq -c | sort -rn | head -10

# Count total warnings
TOTAL_WARNINGS=$(grep -c "warning:" "$TEMP_FILE")
echo -e "\nTotal warnings: $TOTAL_WARNINGS"

# Show sample warnings
echo -e "\nSample Warnings (first 10):"
echo "---------------------------"
grep -E "warning:.*\[" "$TEMP_FILE" | head -10

# Clean up
rm -f "$TEMP_FILE"

echo -e "\nTo fix specific issues, you can run:"
echo "docker exec -w /workspace crypto-lob-dev clang-tidy-17 -p build --fix <file>"