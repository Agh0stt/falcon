#!/bin/bash
# usage: suite.sh [dir] ; runs each .fl in dir on x86-32 and x86-64, compares to "# EXPECT:" line
F=${F:-.}
D=${1:-tests/fp}
W=/tmp/fp-work; mkdir -p $W
pass=0; fail=0
for f in $(ls $D/*.fl | sort); do
  b=$(basename $f .fl)
  exp=$(head -1 $f | sed -n 's/^# EXPECT: //p' | tr '|' '\n')
  for arch in 32 64; do
    if [ $arch = 32 ]; then
      out=$( ($F/falconc $f -o $W/$b.$arch.s 2>$W/$b.$arch.err && as --32 $W/$b.$arch.s -o $W/$b.$arch.o 2>>$W/$b.$arch.err && ld -m elf_i386 $F/flr.o $W/$b.$arch.o -o $W/$b.$arch 2>>$W/$b.$arch.err && timeout 5 $W/$b.$arch) 2>&1 )
    else
      out=$( ($F/falconc $f -arch x86-64-linux -o $W/$b.$arch.s 2>$W/$b.$arch.err && as $W/$b.$arch.s -o $W/$b.$arch.o 2>>$W/$b.$arch.err && ld $W/$b.$arch.o -o $W/$b.$arch 2>>$W/$b.$arch.err && timeout 5 $W/$b.$arch) 2>&1 )
    fi
    if [ "$out" == "$exp" ]; then pass=$((pass+1)); [ -n "$V" ] && echo "ok   $b [$arch]"
    else fail=$((fail+1)); echo "FAIL $b [$arch]"; if [ -n "$V" ]; then echo "  got:  $(echo $out | tr '\n' ' ' | cut -c1-200)"; echo "  want: $(echo $exp | tr '\n' ' ')"; fi; fi
  done
done
echo "pass=$pass fail=$fail"
