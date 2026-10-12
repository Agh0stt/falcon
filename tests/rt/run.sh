#!/bin/bash
# usage: F=. tests/rt/run.sh
# Runtime tests that need an exact exit status / stderr / per-arch behaviour.
# Header lines (first lines of each .fl):
#   # ARCH: 32 | 64 | both     (default both)
#   # EXIT: <n>                (default 0)
#   # STDOUT: a|b|c            ('|' = newline)
#   # STDERR: text             (optional; compared exactly, 1 line)
#   # STDIN: a|b|c            ('|' = newline, \\r etc. allowed; a trailing newline is added unless the value ends in '~')
#   # LIBS: 1                (make io.fl / strings.fl importable while keeping the built-in std)
#   # REALSTD: 32 | 64        (compile against the real std.fl / std64.fl via -I; forces that ARCH and links flr.o / flrt64.o)
F=${F:-.}
D=$(dirname "$0")
W=/tmp/rt-work; mkdir -p $W
pass=0; fail=0
for f in $(ls $D/*.fl | sort); do
  b=$(basename $f .fl)
  archs=$(sed -n 's/^# ARCH: //p' $f | head -1); [ -z "$archs" ] && archs=both
  want_exit=$(sed -n 's/^# EXIT: //p' $f | head -1); [ -z "$want_exit" ] && want_exit=0
  want_out=$(sed -n 's/^# STDOUT: //p' $f | head -1 | tr '|' '\n')
  want_err=$(sed -n 's/^# STDERR: //p' $f | head -1)
  has_in=$(grep -c '^# STDIN: ' $f); in_raw=$(sed -n 's/^# STDIN: //p' $f | head -1)
  libs=$(sed -n 's/^# LIBS: //p' $f | head -1)
  realstd=$(sed -n 's/^# REALSTD: //p' $f | head -1); inc=""; rt64="" ; if [ -n "$realstd" ]; then inc="-I$F"; [ "$realstd" = 1 ] && realstd=32; archs=$realstd; [ "$realstd" = 64 ] && rt64="$F/flrt64.o"; elif [ -n "$libs" ]; then mkdir -p $W/libs; cp $F/io.fl $F/strings.fl $W/libs/; inc="-I$W/libs"; fi
  if [ "${in_raw: -1}" = "~" ]; then printf '%b' "${in_raw%\~}" | tr '|' '\n' > $W/$b.in; else printf '%b\n' "$in_raw" | tr '|' '\n' > $W/$b.in; fi
  [ "$has_in" = 0 ] && : > $W/$b.in
  for arch in 32 64; do
    [ "$archs" != both ] && [ "$archs" != $arch ] && continue
    if [ $arch = 32 ]; then
      $F/falconc $f $inc -o $W/$b.$arch.s 2>$W/$b.$arch.err && as --32 $W/$b.$arch.s -o $W/$b.$arch.o 2>>$W/$b.$arch.err && ld -m elf_i386 $F/flr.o $W/$b.$arch.o -o $W/$b.$arch 2>>$W/$b.$arch.err
    else
      $F/falconc $f $inc -arch x86-64-linux -o $W/$b.$arch.s 2>$W/$b.$arch.err && as $W/$b.$arch.s -o $W/$b.$arch.o 2>>$W/$b.$arch.err && ld $rt64 $W/$b.$arch.o -o $W/$b.$arch 2>>$W/$b.$arch.err
    fi
    if [ $? -ne 0 ]; then echo "FAIL $b [$arch] (build)"; fail=$((fail+1)); continue; fi
    got_out=$(timeout 10 $W/$b.$arch <$W/$b.in 2>$W/$b.$arch.stderr); rc=$?
    got_err=$(cat $W/$b.$arch.stderr)
    ok=1
    [ "$got_out" == "$want_out" ] || ok=0
    [ "$rc" == "$want_exit" ] || ok=0
    [ -z "$want_err" ] || [ "$got_err" == "$want_err" ] || ok=0
    if [ $ok = 1 ]; then pass=$((pass+1)); else
      fail=$((fail+1)); echo "FAIL $b [$arch] rc=$rc (want $want_exit)"
      echo "  out: $(echo $got_out | tr '\n' ' ' | cut -c1-150)  want: $(echo $want_out | tr '\n' ' ')"
      echo "  err: $got_err  want: $want_err"
    fi
  done
done
echo "rt: pass=$pass fail=$fail"
[ $fail -eq 0 ]
