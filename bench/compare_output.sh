#!/bin/bash

# Compare BGP simulator output with expected results
# Usage: ./compare_output.sh <expected_file> <actual_file>

if [ $# -ne 2 ]; then
    echo "Usage: $0 <expected_file> <actual_file>"
    echo "Example: $0 ~/Desktop/bench/prefix/ribs.csv ~/work/exr/outputs/output_prefix.csv"
    exit 1
fi

EXPECTED="$1"
ACTUAL="$2"

if [ ! -f "$EXPECTED" ]; then
    echo "Error: Expected file not found: $EXPECTED"
    exit 1
fi

if [ ! -f "$ACTUAL" ]; then
    echo "Error: Actual file not found: $ACTUAL"
    exit 1
fi

echo "Comparing files (sorted, ignoring whitespace):"
echo "  Expected: $EXPECTED"
echo "  Actual:   $ACTUAL"
echo ""

# Compare sorted versions, ignoring whitespace
diff -b <(sort "$EXPECTED") <(sort "$ACTUAL")

if [ $? -eq 0 ]; then
    echo "✓ Files match perfectly!"
    exit 0
else
    echo ""
    echo "✗ Files differ"
    exit 1
fi
