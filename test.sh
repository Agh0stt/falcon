#!/bin/bash
# Runs every suite. A program only passes if it compiles, assembles, links AND
# exits with status 0 (examples 30/31 report success only through their exit code).
set -o pipefail

FIXED=${FIXED:-./falconc}
FLR=./flr.o
EXAMPLES=examples
W=$(mktemp -d); trap 'rm -rf $W' EXIT

[ -f $FLR ] || as --32 flr.s -o $FLR || exit 1

pass=0; fail=0; fail_list=""

for fl in $(ls $EXAMPLES/*.fl | sort); do
    name=$(basename $fl)
    err=""
    if ! $FIXED $fl -o $W/t.s 2>$W/err.txt; then err="compile failed: $(grep -v '^falconc: wrote' $W/err.txt | head -1)"
    elif ! as --32 $W/t.s -o $W/t.o 2>$W/err.txt; then err="as failed: $(head -1 $W/err.txt)"
    elif ! ld -m elf_i386 $FLR $W/t.o -o $W/t 2>$W/err.txt; then err="ld failed: $(head -1 $W/err.txt)"
    else
        out=$(timeout 3 $W/t 2>&1 | head -10; exit ${PIPESTATUS[0]}); rc=$?
        [ $rc -ne 0 ] && err="exit status $rc"
    fi
    if [ -z "$err" ]; then
        echo "PASS $name: $(echo $out | tr '\n' ' ' | cut -c1-60)"; pass=$((pass+1))
    else
        echo "FAIL $name: $err"; fail=$((fail+1)); fail_list="$fail_list $name"
    fi
done

echo ""
echo "examples: $pass passed, $fail failed"

extra=0
echo "== freestanding kernel compiles"
if ./falconc examples/EXAMPLE-OS/kernel.fl --freestanding -o $W/k.s 2>/dev/null && as --32 $W/k.s -o $W/k.o; then echo "ok"; else echo "FAIL kernel.fl"; extra=1; fi
for suite in "tests/fp/run.sh tests/fp" "tests/fp/run.sh tests/lang" "tests/errors/run.sh"; do
    echo "== $suite"
    out=$(F=. bash $suite 2>&1); rc=$?
    echo "$out" | tail -5
    echo "$out" | grep -q "fail=0" || extra=1
    [ $rc -ne 0 ] && extra=1
done

if [ -n "$fail_list" ] || [ $extra -ne 0 ]; then
    echo "FAILED:${fail_list} $([ $extra -ne 0 ] && echo '(see suites above)')"
    exit 1
fi
echo "ALL PASSED"
exit 0
