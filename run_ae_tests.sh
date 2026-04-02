#!/bin/bash
cd /srv/scratch/PAG/Wjw/vibe/SVF-TS
pass=0
fail=0
total=0
declare -A error_categories

for f in TS-TestSuite/src/ae_assert_tests/*.c; do
    name=$(basename "$f")
    total=$((total+1))
    result=$(timeout 30 ./Release-build/bin/cts-ae "$f" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        errline=$(echo "$result" | grep -iE "error|assert|abort|fault|exception|cannot|unknown|unhandled|unsupported|TODO|Segmentation|terminate" | head -1)
        if [ -z "$errline" ]; then
            errline=$(echo "$result" | tail -3 | head -1)
        fi
        echo "FAIL: $name | $errline"
        # Categorize
        category=$(echo "$errline" | grep -oP '(Unsupported|unhandled|unknown|TODO|assert|Segmentation|No such|cannot)[^ ]*' | head -1)
        if [ -n "$category" ]; then
            error_categories["$category"]=$(( ${error_categories["$category"]:-0} + 1 ))
        fi
    fi
done

echo ""
echo "=========================================="
echo "TOTAL: $total  PASS: $pass  FAIL: $fail"
echo "Pass rate: $(( pass * 100 / total ))%"
echo "=========================================="
echo ""
echo "Error categories:"
for cat in "${!error_categories[@]}"; do
    echo "  $cat: ${error_categories[$cat]}"
done
