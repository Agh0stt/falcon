# Makefile — Falcon toolchain
#
# Targets
#   make            build falconc + flr.o (default)
#   make falconc    compile the compiler
#   make flr.o      assemble the runtime
#   make run F=x.fl run a .fl program end-to-end (compile → assemble → link → exec)
#   make clean      remove build artefacts
#
# Variables you can override on the command line:
#   CC      C compiler for falconc          (default: gcc)
#   CFLAGS  extra flags for falconc build   (default: -O2 -Wall)
#   F       source file for `make run`      (default: main.fl)
#   I       extra import search path        (default: .)

CC     ?= gcc
CFLAGS ?= -O2 -Wall
F      ?= main.fl
I      ?= .

# Derive output names from F: main.fl → main.s / main.o / main
_BASE  := $(basename $(F))
_ASM   := $(_BASE).s
_OBJ   := $(_BASE).o
_BIN   := $(_BASE)

# ─────────────────────────────────────────────────────────────────────
.PHONY: all run clean

all: falconc flr.o

# ── compiler ─────────────────────────────────────────────────────────
falconc: falconc.c
	$(CC) $(CFLAGS) -o $@ $<

# ── runtime ──────────────────────────────────────────────────────────
flr.o: flr.s
	as --32 $< -o $@

# ── end-to-end: compile + assemble + link + run ──────────────────────
# Usage:  make run F=hello.fl
#         make run F=hello.fl I=/path/to/libs
run: falconc flr.o $(F)
	./falconc $(F) -I$(I) -o $(_ASM)
	as --32 $(_ASM) -o $(_OBJ)
	ld -m elf_i386 flr.o $(_OBJ) -o $(_BIN)
	./$(_BIN)

# ── freestanding / bare-metal variant ────────────────────────────────
# Usage:  make freestanding F=kernel.fl LDSCRIPT=link.ld
LDSCRIPT ?= link.ld
freestanding: falconc $(F)
	./falconc $(F) --freestanding -I$(I) -o $(_ASM)
	as --32 $(_ASM) -o $(_OBJ)
	ld -m elf_i386 -T $(LDSCRIPT) $(_OBJ) -o $(_BASE).elf

# ── clean ─────────────────────────────────────────────────────────────
clean:
	rm -f falconc flr.o *.s *.o *.elf
	@# keep std.fl in place
