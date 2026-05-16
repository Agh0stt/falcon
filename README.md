# Falcon

Falcon is a small compiled language that targets x86-32 Linux. It has a C-like feel but cleaner syntax — no semicolons, significant newlines, and Python-style `and`/`or`/`not`. The whole toolchain is a single C file (`falconc.c`) plus a pure-assembly runtime (`flr.s`). No libc, no LLVM, no external dependencies.

---

## Getting started

```bash
# Build the compiler
gcc -O2 -o falconc falconc.c

# Assemble the runtime
as --32 flr.s -o flr.o

# Write a program
echo 'import "std"
func main() -> void {
    print("hello")
}' > hello.fl

# Compile → assemble → link → run
./falconc hello.fl -o hello.s
as --32 hello.s -o hello.o
ld -m elf_i386 flr.o hello.o -o hello
./hello
```

Or use the Makefile:

```bash
make                    # builds falconc and flr.o
make run F=hello.fl     # compile, link, and run in one step
make clean
```

---

## Language reference

### Types

| Type | Description |
|------|-------------|
| `int` | 32-bit signed integer |
| `str` | Pointer to a null-terminated string |
| `bool` | Boolean (`true` / `false`, stored as 0/1) |
| `void` | No value, only valid as a function return type |
| `[T]` | Array of T (heap-allocated, 0-indexed) |
| `*T` | Pointer to T |
| `Name` | User-defined struct |

### Variable declaration

```falcon
name: type
name: type = expression
```

```falcon
x: int = 42
greeting: str = "hello"
flag: bool = true
```

### Functions

```falcon
func name(param: type, ...) -> returntype {
    # body
}
```

```falcon
func add(a: int, b: int) -> int {
    return a + b
}

func greet(s: str) -> void {
    print(s)
}
```

A `main` function is required. It must be `-> void` (or just `-> void` returning nothing). The runtime calls it from `_start` and passes its return value to `sys_exit`.

### Arithmetic

```falcon
a + b    a - b    a * b    a / b    a % b
```

All operations are 32-bit signed integer arithmetic.

### Comparison

```falcon
a == b    a != b    a < b    a > b    a <= b    a >= b
```

Return `1` (true) or `0` (false).

### Logical

```falcon
a and b    a or b    not a
```

Short-circuits are not guaranteed — both sides may be evaluated.

### Bitwise

```falcon
a & b    a | b    a ^ b    ~a    a << b    a >> b
```

Right shift is arithmetic (sign-extending).

### Compound assignment

```falcon
x += 1    x -= 1    x *= 2    x /= 2    x %= 3
x &= mask    x |= flags    x ^= bit
x <<= 1    x >>= 1
```

### If / elif / else

```falcon
if (condition) {
    # ...
} elif (other) {
    # ...
} else {
    # ...
}
```

### While

```falcon
while (condition) {
    # ...
}
```

### For

```falcon
for (init, condition, post) {
    # ...
}
```

```falcon
for (i: int = 0, i < 10, i += 1) {
    print(i)
}
```

### Break and continue

```falcon
while (true) {
    if (done) { break }
    if (skip) { continue }
}
```

### Arrays

```falcon
nums: [int] = [10, 20, 30]
print(nums[0])    # 10
nums[1] = 99
```

Arrays are heap-allocated. Elements start at index 0.

### Structs

```falcon
struct Point {
    x: int
    y: int
}

func main() -> void {
    p: Point = _flr_alloc(8)   # 2 fields × 4 bytes each
    p.x = 10
    p.y = 20
    print(p.x)
}
```

Struct variables hold a pointer. You must allocate memory before writing to any field (`nfields × 4` bytes).

### Integer literals

```falcon
42        # decimal
0xFF      # hexadecimal
0xDEAD
```

Binary literals (`0b...`) are not supported — use hex or decimal.

### String literals

```falcon
"hello\nworld"
"tab\there"
"quote: \""
```

Escape sequences: `\n \t \r \0 \\ \"`.

### Comments

```falcon
# line comment

/* block
   comment */
```

### Imports

```falcon
import "std"       # the standard library (virtual, no file needed)
import "mylib"     # loads mylib.fl from the search path
```

Use `-I<path>` to add directories to the search path. Imports are recursive and deduplicated automatically.

---

## Standard library (`import "std"`)

Importing `"std"` enables the `print()` built-in and links the Falcon runtime functions. No file is needed on disk — `std` is a virtual import.

### print

```falcon
print(42)          # prints an integer followed by a newline
print("hello")     # prints a string followed by a newline
```

`print` automatically picks `_flr_print_int` or `_flr_print_str` based on the type of its argument.

### Runtime functions

These are always available after `import "std"`. Call them directly by name.

| Function | Description |
|----------|-------------|
| `_flr_str_len(s)` | Length of string `s` in bytes |
| `_flr_str_eq(a, b)` | 1 if strings are equal, 0 otherwise |
| `_flr_int_to_str(n)` | Convert integer to string (static buffer) |
| `_flr_str_to_int(s)` | Parse a decimal string to integer |
| `_flr_abs(n)` | Absolute value |
| `_flr_min(a, b)` | Smaller of two integers |
| `_flr_max(a, b)` | Larger of two integers |
| `_flr_alloc(n)` | Allocate `n` bytes from the bump heap, returns pointer |
| `_flr_free(ptr)` | No-op (bump allocator has no free) |
| `_flr_exit(code)` | Exit with status code |
| `_flr_assert(cond, msg)` | Abort with message if `cond` is 0 |

---

## Hardware intrinsics

These are built into the compiler and emit a single instruction or a short inline sequence. Zero overhead, no function call.

| Intrinsic | Description |
|-----------|-------------|
| `__syscall(num, a1..a5)` | Raw Linux `int 0x80` syscall |
| `__inb(port)` | Read a byte from an I/O port |
| `__outb(port, val)` | Write a byte to an I/O port |
| `__cli()` | Disable interrupts |
| `__sti()` | Enable interrupts |
| `__hlt()` | Halt the CPU |
| `__rdtsc()` | Read timestamp counter (low 32 bits) |
| `__peek(addr)` | Read a 32-bit word from memory |
| `__poke(addr, val)` | Write a 32-bit word to memory |
| `__peekb(addr)` | Read a byte from memory |
| `__pokeb(addr, val)` | Write a byte to memory |
| `__memset(ptr, val, n)` | Fill `n` bytes with `val` |
| `__memcpy(dst, src, n)` | Copy `n` bytes |

---

## Bare-metal / freestanding mode

Pass `--freestanding` to compile without the runtime and without a `sys_exit` call. The `main` function falls through to a `cli` / `hlt` loop instead.

```bash
./falconc kernel.fl --freestanding -o kernel.s
as --32 kernel.s -o kernel.o
ld -m elf_i386 -T link.ld kernel.o -o kernel.elf
```

In freestanding mode `import "std"` and all `_flr_*` functions are unavailable. Use `__syscall` and the memory intrinsics directly.

---

## Linking

```bash
# hosted (the normal case)
as --32 out.s -o out.o
ld -m elf_i386 flr.o out.o -o prog

# freestanding (OS kernel, bootloader, etc.)
as --32 out.s -o out.o
ld -m elf_i386 -T link.ld out.o -o kernel.elf
```

`flr.o` provides `_start`, all `_flr_*` symbols, and the 1 MB bump heap. It must come before user object files on the linker command line.

---

## Examples

The `examples/` folder has one file per language feature, numbered in order.

| File | What it shows |
|------|---------------|
| `01_hello.fl` | Hello World |
| `02_int_vars.fl` | Integer variables and arithmetic |
| `03_str_vars.fl` | String variables |
| `04_bool_vars.fl` | Boolean literals |
| `05_if_else.fl` | if / elif / else |
| `06_while.fl` | while loop |
| `07_for.fl` | for loop |
| `08_break_continue.fl` | break and continue |
| `09_functions.fl` | Defining and calling functions |
| `10_recursion.fl` | Recursive functions (factorial) |
| `11_compound_assign.fl` | Compound assignment operators |
| `12_bitwise.fl` | Bitwise operators |
| `13_logic.fl` | Logical operators |
| `14_comparison.fl` | Comparison operators |
| `15_hex_literals.fl` | Hexadecimal literals |
| `16_arrays.fl` | Array literals and indexing |
| `17_structs.fl` | Struct definition and field access |
| `18_unary.fl` | Unary operators |
| `19_str_builtins.fl` | String runtime functions |
| `20_math_builtins.fl` | abs, min, max |
| `21_alloc.fl` | Heap allocation |
| `22_peek_poke.fl` | Raw memory access |
| `23_syscall.fl` | Raw Linux syscall |
| `24_rdtsc.fl` | CPU timestamp counter |
| `25_assert.fl` | Assertions |
| `26_comments.fl` | Comment syntax |

Run all of them at once:

```bash
for f in examples/*.fl; do
    echo "=== $f ==="
    ./falconc "$f" -o /tmp/t.s && as --32 /tmp/t.s -o /tmp/t.o && \
    ld -m elf_i386 flr.o /tmp/t.o -o /tmp/t && /tmp/t
done
```

---

## Known limitations

- **32-bit only.** Targets i386; no x86-64 backend.
- **No binary literals.** Use decimal or `0x` hex instead.
- **Structs need manual allocation.** Declare `p: MyStruct = _flr_alloc(nfields * 4)` before writing fields.
- **int_to_str uses a static buffer.** The returned pointer is overwritten on the next call.
- **No type checker.** The compiler trusts you. Wrong types produce wrong code silently.
- **No closures, no generics, no modules beyond import.**
- **1 MB heap, no free.** The bump allocator never reclaims memory.
