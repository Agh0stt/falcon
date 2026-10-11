#!/bin/bash
# Each tests/errors/*.fl must FAIL to compile with a message containing the
# text after "# ERROR: " on its first line, on both targets.
F=${F:-.}; D=${1:-tests/errors}
pass=0; fail=0
for f in $(ls $D/*.fl | sort); do
  want=$(head -1 $f | sed -n 's/^# ERROR: //p')
  for arch in x86-32-linux x86-64-linux; do
    out=$($F/falconc $f -arch $arch -o /tmp/err-test.s 2>&1); rc=$?
    if [ $rc -ne 0 ] && echo "$out" | grep -qF -- "$want"; then pass=$((pass+1))
    else fail=$((fail+1)); echo "FAIL $(basename $f) [$arch] rc=$rc want '$want' got: $(echo $out | cut -c1-120)"; fi
  done
done
echo "errors: pass=$pass fail=$fail"; [ $fail -eq 0 ]
