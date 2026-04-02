#!/bin/bash
# Generate ground truth DOT files from LLVM frontend
# Usage: ./generate_ground_truth.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
C_SOURCES_DIR="${SCRIPT_DIR}/c_sources"
GROUND_TRUTH_DIR="${SCRIPT_DIR}/ground_truth"

# Tool paths
CLANG="/data1/wjw/SVF-projects/treesitter/SVF/llvm-18.1.0.obj/bin/clang"
OPT="/data1/wjw/SVF-projects/treesitter/SVF/llvm-18.1.0.obj/bin/opt"
WPA="/data1/wjw/SVF-projects/treesitter/SVF/Release-build/bin/wpa"

# Verify tools exist
for tool in "$CLANG" "$OPT" "$WPA"; do
    if [ ! -x "$tool" ]; then
        echo "Error: Tool not found or not executable: $tool"
        exit 1
    fi
done

mkdir -p "$GROUND_TRUTH_DIR"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "=== Generating LLVM Frontend Ground Truth ==="

for src in "${C_SOURCES_DIR}"/*.c; do
    base=$(basename "$src" .c)
    echo "Processing: $base.c"

    # Step 1: Compile to LLVM IR
    "$CLANG" -Wno-everything -S -Wno-implicit-function-declaration -fno-discard-value-names -g -emit-llvm -o "${TMP_DIR}/${base}.ll" "$src" 2>/dev/null || {
        echo "  Warning: clang failed for $base.c, skipping"
        continue
    }

    # Step 2: Run mem2reg to get SSA form
    "$OPT" -S -p=mem2reg "${TMP_DIR}/${base}.ll" -o "${TMP_DIR}/${base}_ssa.ll" 2>/dev/null || {
        echo "  Warning: opt failed for $base.c, skipping"
        continue
    }

    # Step 3: Run wpa with -ander to dump PAG (output is svfir_initial.dot in cwd)
    pushd "$GROUND_TRUTH_DIR" > /dev/null
    "$WPA" -ander -dump-pag "${TMP_DIR}/${base}_ssa.ll" > /dev/null 2>&1 || {
        echo "  Warning: wpa failed for $base.c, skipping"
        popd > /dev/null
        continue
    }
    popd > /dev/null

    # Rename the output file (wpa produces svfir_initial.dot, not pag.dot)
    if [ -f "${GROUND_TRUTH_DIR}/svfir_initial.dot" ]; then
        mv "${GROUND_TRUTH_DIR}/svfir_initial.dot" "${GROUND_TRUTH_DIR}/llvm_${base}_pag.dot"
        echo "  Generated: llvm_${base}_pag.dot"
    else
        echo "  Warning: No svfir_initial.dot generated for $base.c"
    fi
done

echo ""
echo "=== Ground Truth Generation Complete ==="
echo "Output directory: ${GROUND_TRUTH_DIR}"
ls -la "${GROUND_TRUTH_DIR}"/*.dot 2>/dev/null || echo "No DOT files generated"
