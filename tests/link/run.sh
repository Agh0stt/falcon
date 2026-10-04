#!/bin/bash
# Two-object link test: tests/link/lib.fl + tests/link/main.fl built separately and linked, on both targets.
# Covers extern vars/funcs, static globals/functions/locals with clashing names, a string global initialised in a
# library without main, and two objects that both import "std" (weak _start).
F=${F:-.}; D=$(dirname "$0"); W=/tmp/link-work; mkdir -p $W
want="11 16 5000000001 5.000000 lib 1118 2222 6.000000 1 2 101 3"
pass=0; fail=0
for arch in 32 64; do
  ok=1
  for m in lib main; do
    if [ $arch = 32 ]; then $F/falconc $D/$m.fl -o $W/$m.$arch.s 2>/dev/null && as --32 $W/$m.$arch.s -o $W/$m.$arch.o || ok=0
    else $F/falconc $D/$m.fl -arch x86-64-linux -o $W/$m.$arch.s 2>/dev/null && as $W/$m.$arch.s -o $W/$m.$arch.o || ok=0; fi
  done
  if [ $ok = 1 ]; then
    if [ $arch = 32 ]; then ld -m elf_i386 $F/flr.o $W/lib.$arch.o $W/main.$arch.o -o $W/p.$arch 2>/dev/null || ok=0
    else ld $W/lib.$arch.o $W/main.$arch.o -o $W/p.$arch 2>/dev/null || ok=0; fi
  fi
  got=""; [ $ok = 1 ] && got=$(timeout 5 $W/p.$arch | tr '\n' ' ' | sed 's/ $//')
  if [ "$got" == "$want" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL link [$arch] got: $got"; fi
done
echo "link: pass=$pass fail=$fail"; [ $fail -eq 0 ]
