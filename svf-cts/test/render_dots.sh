#!/bin/bash
# Render all DOT files in test/output/ to SVG
# Usage: bash test/render_dots.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"

if ! command -v dot &> /dev/null; then
    echo "Error: 'dot' (graphviz) not found. Install with: sudo apt install graphviz"
    exit 1
fi

COUNT=0
for dotfile in "${OUTPUT_DIR}"/*.dot; do
    [ -f "$dotfile" ] || continue
    base="${dotfile%.dot}"
    echo "Rendering: $(basename "$dotfile") -> $(basename "$base").svg"
    dot -Tsvg "$dotfile" -o "${base}.svg"
    COUNT=$((COUNT + 1))
done

if [ $COUNT -eq 0 ]; then
    echo "No .dot files found in $OUTPUT_DIR"
    echo "Run 'bash test/run_ab_test.sh' first to generate them."
    exit 1
fi

echo ""
echo "Done. Rendered $COUNT SVG files in $OUTPUT_DIR"
