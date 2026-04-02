#!/bin/bash
# A/B Test: Generate PAG + ICFG DOTs from BOTH frontends and compare them
# Usage: ./run_ab_test.sh [path-to-cts-svf]
#
# Output naming (all in test/output/):
#   llvm_<name>_pag.dot    - LLVM frontend PAG
#   llvm_<name>_icfg.dot   - LLVM frontend ICFG
#   ts_<name>_pag.dot      - TreeSitter frontend PAG
#   ts_<name>_icfg.dot     - TreeSitter frontend ICFG

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
C_SOURCES_DIR="${SCRIPT_DIR}/c_sources"
OUTPUT_DIR="${SCRIPT_DIR}/output"

# Tool paths
CTS_SVF="${1:-${PROJECT_DIR}/build/bin/cts-svf}"
CLANG="/data1/wjw/SVF-projects/treesitter/SVF/llvm-18.1.0.obj/bin/clang"
OPT="/data1/wjw/SVF-projects/treesitter/SVF/llvm-18.1.0.obj/bin/opt"
WPA="/data1/wjw/SVF-projects/treesitter/SVF/Release-build/bin/wpa"

# Verify tools exist
for tool in "$CTS_SVF" "$CLANG" "$OPT" "$WPA"; do
    if [ ! -x "$tool" ]; then
        echo "Error: Tool not found or not executable: $tool"
        exit 1
    fi
done

mkdir -p "$OUTPUT_DIR"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "=== A/B Test: TreeSitter vs LLVM Frontend (PAG + ICFG) ==="
echo "  cts-svf: $CTS_SVF"
echo "  clang:   $CLANG"
echo "  wpa:     $WPA"
echo "  output:  $OUTPUT_DIR"
echo ""

PASS=0
FAIL=0
TOTAL=0

for src in "${C_SOURCES_DIR}"/*.c; do
    base=$(basename "$src" .c)
    TOTAL=$((TOTAL + 1))
    echo "--- [$TOTAL] $base.c ---"

    # ============================================
    # Step 1: Generate LLVM frontend DOTs (PAG + ICFG)
    # ============================================
    echo "  [LLVM] clang -g -> opt -> wpa ..."

    # clang -> LLVM IR (with -g for debug info in ICFG node labels)
    "$CLANG" -Wno-everything -S -Wno-implicit-function-declaration \
        -fno-discard-value-names -g -emit-llvm \
        -o "${TMP_DIR}/${base}.ll" "$src" 2>/dev/null || {
        echo "  [LLVM] FAILED: clang could not compile $base.c"
        FAIL=$((FAIL + 1))
        continue
    }

    # opt -mem2reg -> SSA form
    "$OPT" -S -p=mem2reg "${TMP_DIR}/${base}.ll" -o "${TMP_DIR}/${base}_ssa.ll" 2>/dev/null || {
        echo "  [LLVM] FAILED: opt mem2reg failed for $base.c"
        FAIL=$((FAIL + 1))
        continue
    }

    # wpa -ander -dump-pag -dump-icfg -> svfir_initial.dot + icfg_initial.dot (in cwd)
    pushd "$OUTPUT_DIR" > /dev/null
    "$WPA" -ander -dump-pag -dump-icfg "${TMP_DIR}/${base}_ssa.ll" > /dev/null 2>&1 || {
        echo "  [LLVM] FAILED: wpa failed for $base.c"
        popd > /dev/null
        FAIL=$((FAIL + 1))
        continue
    }
    popd > /dev/null

    # Rename LLVM outputs
    LLVM_PAG="${OUTPUT_DIR}/llvm_${base}_pag.dot"
    LLVM_ICFG="${OUTPUT_DIR}/llvm_${base}_icfg.dot"

    if [ -f "${OUTPUT_DIR}/svfir_initial.dot" ]; then
        mv "${OUTPUT_DIR}/svfir_initial.dot" "$LLVM_PAG"
        echo "  [LLVM] PAG  -> llvm_${base}_pag.dot"
    else
        echo "  [LLVM] WARNING: no svfir_initial.dot"
    fi

    if [ -f "${OUTPUT_DIR}/icfg_initial.dot" ]; then
        mv "${OUTPUT_DIR}/icfg_initial.dot" "$LLVM_ICFG"
        echo "  [LLVM] ICFG -> llvm_${base}_icfg.dot"
    else
        echo "  [LLVM] WARNING: no icfg_initial.dot"
    fi

    # ============================================
    # Step 2: Generate TreeSitter frontend DOTs (PAG + ICFG)
    # ============================================
    echo "  [TS]   cts-svf --dump-all ..."
    "$CTS_SVF" --dump-all -o "$OUTPUT_DIR" "$src" > /dev/null 2>&1 || {
        echo "  [TS]   FAILED: cts-svf crashed on $base.c"
        FAIL=$((FAIL + 1))
        continue
    }

    # Rename TS outputs
    TS_PAG="${OUTPUT_DIR}/ts_${base}_pag.dot"
    TS_ICFG="${OUTPUT_DIR}/ts_${base}_icfg.dot"

    if [ -f "${OUTPUT_DIR}/${base}_pag.dot" ]; then
        mv "${OUTPUT_DIR}/${base}_pag.dot" "$TS_PAG"
        echo "  [TS]   PAG  -> ts_${base}_pag.dot"
    else
        echo "  [TS]   WARNING: no ${base}_pag.dot"
    fi

    if [ -f "${OUTPUT_DIR}/${base}_icfg.dot" ]; then
        mv "${OUTPUT_DIR}/${base}_icfg.dot" "$TS_ICFG"
        echo "  [TS]   ICFG -> ts_${base}_icfg.dot"
    else
        echo "  [TS]   WARNING: no ${base}_icfg.dot"
    fi

    # ============================================
    # Step 3: Compare PAG
    # ============================================
    if [ -f "$LLVM_PAG" ] && [ -f "$TS_PAG" ]; then
        echo "  [Compare PAG]"
        python3 "${SCRIPT_DIR}/compare_dot.py" "$LLVM_PAG" "$TS_PAG" 2>&1 | sed 's/^/    /'

        if [ ${PIPESTATUS[0]} -eq 0 ]; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
        fi
    fi

    echo ""
done

echo "========================================="
echo "  Results: $PASS PASS / $FAIL FAIL / $TOTAL TOTAL"
echo "========================================="
echo ""
echo "  Output directory: $OUTPUT_DIR"
echo "  Files:"
ls -1 "${OUTPUT_DIR}"/*.dot 2>/dev/null | sed 's/^/    /'
echo ""
echo "  To render all to SVG: bash test/render_dots.sh"
