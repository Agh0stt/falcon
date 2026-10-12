# Falcon

Falcon is a small compiled language targeting x86 Linux. It has a C-like feel but cleaner syntax — no semicolons, significant newlines, and Python-style `and`/`or`/`not`. The whole toolchain is a single C file (`falconc.c`) plus a pure-assembly runtime (`flr.s`). No libc, no LLVM, no external dependencies.

---

## What's new

### v6.1
- **Top-level `const`** — `const A: int = 10`, `const B = A * 2 + 1`, `static const`. Immutable; initializer must be a compile-time constant (literals, operators, other consts).
- Global initializers are type-checked (`g: int = 1.5` is now an error instead of silently truncating).

### v6
- **`static`** — file-local linkage for top-level functions and globals. A `static func` or `static x: type` is emitted with `.local` instead of `.globl`, so it isn't visible to other object files linked into the same program.
- **`static` locals** — `static n: int = 0` inside a function body keeps its value across calls, backed by a hidden file-local global instead of the stack. The initializer must be a compile-time literal (same rule as top-level globals); anything else starts at 0.
- **`extern`** — forward-declares a function or global that's defined in another object file. `extern func foo(...) -> T` takes no body; `extern x: type` takes no initializer. Both just register a signature/symbol for the type checker and linker to resolve at link time.

### v5
- **`typedef`** — C-style type aliases with full support for three forms:
  - `typedef int myint` — primitive or any type alias
  - `typedef Point Vec2` — struct alias (field access works transparently)
  - `typedef struct { ... } Name` — inline anonymous struct definition
  - `typedef struct Tag { ... } Alias` — tagged inline struct (C-style)

### v4
- **`-arch x86-64-linux`** — new 64-bit target (System V AMD64 ABI): `syscall` instead of `int 0x80`, args in `rdi/rsi/rdx/rcx/r8/r9`, native 64-bit `long`, SSE2 for float/double
- **`-arch x86-32-linux`** — explicit flag for the original 32-bit target (still the default)

### v3
- **`float`** — 32-bit IEEE-754 via x87 FPU (`flds`/`fstps`/`fadd` etc.)
- **`double`** — 64-bit IEEE-754 via x87 FPU (`fldl`/`fstpl`/`faddp` etc.)
- **`let`** — mutable variable declaration (sugar: `let x: int = 5`)
- **`const`** — immutable compile-time constant, folded at compile time, no stack slot (`const PI: float = 3.14`, `const MAX: int = 100`)

### v2
- **`long`** — 64-bit signed integer (stored as two 32-bit stack slots; arithmetic via 64-bit helpers)
- **Type checker** — every expression has a computed type; mismatches are errors at compile time, not silent wrong code at runtime
- **`str_concat(a, b)`** — heap-allocate and concatenate two strings
- **`str_format(fmt, ...)`** — `%`-style formatting (`%d %s %%`) supporting up to 8 args
- **Dynamic heap** — `_flr_alloc` uses `brk`/`sbrk` instead of a fixed BSS slab; grows on demand with no hard cap

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

To target 64-bit:

```bash
./falconc hello.fl -arch x86-64-linux -o hello.s
as hello.s -o hello.o
ld hello.o -o hello
./hello
```

---

## Language reference

### Types

| Type | Description |
|------|-------------|
| `int` | 32-bit signed integer |
| `long` | 64-bit signed integer (stored as two 32-bit slots) |
| `float` | 32-bit IEEE-754 floating point (x87) |
| `double` | 64-bit IEEE-754 floating point (x87) |
| `str` | Pointer to a null-terminated string |
| `bool` | Boolean (`true` / `false`, stored as 0/1) |
| `void` | No value, only valid as a function return type |
| `[T]` | Array of T (heap-allocated, 0-indexed) |
| `*T` | Pointer to T |
| `Name` | User-defined struct |
| `Alias` | Typedef alias (resolves to the underlying type) |

### Variable declaration

```falcon
name: type
name: type = expression
let name: type = expression    # same as above; emphasises mutability
const name: type = expression  # compile-time constant; cannot be reassigned
```

```falcon
x: int = 42
greeting: str = "hello"
flag: bool = true
big: long = 1234567890L
pi: float = 3.14
ratio: double = 1.6180339887
let count: int = 0
const MAX: int = 100
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

A `main` function is required. It must return `-> void`. The runtime calls it from `_start` and passes its return value to `sys_exit`.

### Arithmetic

```falcon
a + b    a - b    a * b    a / b    a % b
```

Integer operations are 32-bit signed. `long` operands use 64-bit helpers. `float`/`double` operands use x87 FPU instructions.

### Comparison

```falcon
a == b    a != b    a < b    a > b    a <= b    a >= b
```

Return `1` (true) or `0` (false). Works for `int`, `long`, `float`, and `double`.

### Logical

```falcon
a and b    a or b    not a
```

`and` / `or` short-circuit: the right operand is only evaluated when needed. The result is always 0 or 1.

### Bitwise

```falcon
a & b    a | b    a ^ b    ~a    a << b    a >> b
```

Right shift is arithmetic (sign-extending). Bitwise operators require numeric operands.

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

### Linkage: `static` and `extern`

By default, every top-level function and global variable is exported (`.globl`) so other object files linked into the same program can see it.

```falcon
# file-local — not visible outside this object file
static hit_count: int = 0

static func track(label: str) -> void {
    hit_count += 1
    print(label)
}
```

`extern` declares a function or global that's *defined elsewhere* and lets you use it without redefining it. An `extern func` has no body; an `extern` variable has no initializer:

```falcon
# lib.fl
sq_cache: int = 0
func square(n: int) -> int {
    sq_cache = n * n
    return sq_cache
}

# main.fl
extern sq_cache: int
extern func square(n: int) -> int

func main() -> void {
    print(square(6))   # 36
    print(sq_cache)    # 36
}
```

```bash
./falconc lib.fl -o lib.s   && as --32 lib.s -o lib.o
./falconc main.fl -o main.s && as --32 main.s -o main.o
ld -m elf_i386 flr.o lib.o main.o -o prog
```

Two objects that both `import "std"` can be linked together (`_start` is weak). An `extern` declaration must match the
definition's type exactly, and `main` cannot be `static`. A `str` global initialised with a literal works in any object; an
array-literal global needs a `main` in the same object to initialise it (otherwise it starts at 0 and the compiler warns).

`static` and `extern` are mutually exclusive and only apply at the top level (functions and global variables) — they aren't valid on locals, `let`, or `const`.

### Static locals

A `static` variable declared inside a function keeps its value between calls, instead of getting a fresh stack slot every time:

```falcon
func counter() -> int {
    static n: int = 0
    n += 1
    return n
}

func main() -> void {
    print(counter())   # 1
    print(counter())   # 2
    print(counter())   # 3
}
```

It's implemented as a hidden, uniquely-named file-local global (`.local` linkage) rather than a stack slot, so two functions can each declare `static n: int` with no collision. As with top-level globals, the initializer must be a compile-time literal — anything else emits a warning and the variable starts at 0.

### Typedef

Type aliases let you give a new name to any existing type.

```falcon
# Primitive alias
typedef int   Number
typedef str   Text

# Struct alias — Vec2 is fully usable with .x .y fields
struct Point { x: int  y: int }
typedef Point Vec2

# Inline struct (no separate struct declaration needed)
typedef struct {
    r: int
    g: int
    b: int
} Color

# Tagged inline struct (C-style)
typedef struct Rect {
    x: int
    y: int
    w: int
    h: int
} Rectangle
```

Aliases resolve recursively, work in variable declarations, function parameters, and return types, and are fully visible to the type checker.

### Integer literals

```falcon
42        # decimal
0xFF      # hexadecimal
0xDEAD
1234L     # long (L suffix optional for long variables)
```

Binary literals (`0b...`) are not supported — use hex or decimal.

### Float / double literals

```falcon
3.14f     # float  (f suffix)
3.14      # double (no suffix, or d suffix)
2.718e0   # scientific notation
```

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
import "std"       # loads std.fl if found; otherwise uses Falcon's virtual std
import "mylib"     # loads mylib.fl from the search path
```

Use `-I<path>` to add directories to the search path. Imports are recursive and deduplicated automatically.

Import resolution is based on the source file, not the compiler binary. Falcon first searches next to the file doing the import, then each `-I` path. If a real `std.fl` is found, it is compiled like an ordinary source file and does not automatically enable the built-in runtime. The virtual built-in `std` is used only when no real `std.fl` can be found.

---

## Type checker

Falcon performs a full type check pass between parsing and code generation. Every expression node is annotated with its computed type; mismatches produce a compile-time error with file and line number.

**What the type checker catches:**

- Undefined variables
- Wrong number of arguments to a function
- Argument type mismatches for user-defined functions
- Return type mismatches
- Arithmetic or bitwise operators applied to non-numeric types
- Assignments to `const` variables
- Invalid compound assignments (e.g. `+=` on a non-numeric left-hand side)
- Indexing a non-array type
- Using a non-numeric array index
- Accessing a field that doesn't exist on a struct
- Mixed-type array literals
- `break` or `continue` outside a loop
- `str_concat` called with non-`str` arguments
- `str_format` called with a non-`str` format argument or more than 8 format args

**Type widening rules:**

| From | To | Allowed |
|------|----|---------|
| `int` | `long` | yes |
| `long` | `int` | yes |
| `int`/`long` | `float`/`double` | yes |
| `float` | `double` | yes |
| `bool` | `int` | yes |
| `int`/`*T` | `str` | yes (bare-metal pointer use) |
| `int` | struct variable | yes (`_flr_alloc` return) |

Unknown external functions produce a warning and are assumed to return `void`.

---

## Standard library (`import "std"`)

If no real `std.fl` is found, importing `"std"` enables the `print()` built-in and links the Falcon runtime functions. This fallback `std` is virtual, so no file is needed on disk.

If a project or library provides its own `std.fl`, Falcon imports that file only. In that case names like `print` must come from the file itself or another linked object; the compiler will not treat them as built-in runtime calls automatically.

### print

```falcon
print(42)          # prints an integer followed by a newline
print("hello")     # prints a string followed by a newline
print(3.14f)       # prints a float followed by a newline
print(3.14)        # prints a double followed by a newline
print(1234567890L) # prints a long followed by a newline
```

`print` automatically dispatches to the correct runtime function based on the type of its argument.

### Runtime functions

These are available when the virtual built-in `std` is active. Call them directly by name.

| Function | Description |
|----------|-------------|
| `_flr_str_len(s)` | Length of string `s` in bytes |
| `_flr_str_eq(a, b)` | 1 if strings are equal, 0 otherwise |
| `_flr_int_to_str(n)` | Convert integer to string (static buffer) |
| `_flr_str_to_int(s)` | Parse a decimal string to integer |
| `_flr_abs(n)` | Absolute value |
| `_flr_min(a, b)` | Smaller of two integers |
| `_flr_max(a, b)` | Larger of two integers |
| `_flr_alloc(n)` | Allocate `n` bytes from the dynamic heap, returns pointer |
| `_flr_free(ptr)` | No-op (bump allocator has no free) |
| `_flr_read_line()` | Read one line from stdin into a heap string; strips `\n` / `\r\n`; any length; `""` at EOF |
| `_flr_stdin_eof()` | 1 if the last `_flr_read_line()` hit end of input with no data, else 0 |
| `_flr_exit(code)` | Exit with status code |
| `_flr_assert(cond, msg)` | Abort with message if `cond` is 0 |
| `str_concat(a, b)` | Heap-allocate and return concatenation of two strings |
| `str_format(fmt, ...)` | Printf-style formatting; supports `%d`, `%s`, `%%`; up to 8 args |

### Reading input (stdin)

Falcon reads stdin through `_flr_read_line()` (both `x86-32-linux` and `x86-64-linux`). The real `std.fl` wraps it with friendlier names (`std.fl` is 32-bit; `std64.fl` has the same functions for 64-bit):

| Function | Description |
|----------|-------------|
| `read_line() -> str` | Next line of stdin, newline stripped |
| `stdin_eof() -> int` | 1 after a `read_line()` that hit EOF with no data |
| `input_str(prompt) -> str` | Print `prompt` (no newline), then `read_line()` |
| `read_int() -> int` | `read_line()` parsed as a decimal int (`str_to_int` rules: stops at the first non-digit, so `"12abc"` is 12, `"abc"` is 0, leading spaces give 0) |
| `input(prompt) -> int` | Print `prompt` (no newline), then `read_int()` |

```
import "std"
func main() -> void {
    age: int = input("age? ")
    name: str = input_str("name? ")
    print_str(name)
    print_int(age)
}
```

The built-in `import "std"` (no `std.fl`) provides the same names on both targets: `input`, `input_str`, `read_int`, `read_line`, `stdin_eof`. A function of the same name that you define yourself takes priority. A final line with no trailing newline is returned normally; the *next* call reports EOF. Reads are one byte per syscall (nothing is buffered), so stdin stays correct if you share it with another process.

### 64-bit std (`std64.fl` + `flrt64.s`)

`std.fl` and `flr.s` are the 32-bit pair. For `-arch x86-64-linux` use the 64-bit pair: `import "std64"` and link `flrt64.o` (the same runtime falconc emits for the built-in virtual `std`, with every symbol `.weak`). `std64.fl` has the same functions as `std.fl` (print, input, string, math, alloc, exit, assert) using 64-bit syscall numbers; `alloc` returns `long` and `free` takes `long`.

```
as flrt64.s -o flrt64.o            # or: make flrt64.o
./falconc prog.fl -arch x86-64-linux -I. -o prog.s
as prog.s -o prog.o
ld flrt64.o prog.o -o prog         # or: make run64 F=prog.fl
```

### `auto` and `generic`

Two type keywords:

- **`auto`** infers a *local* variable's type from its initializer (like Dart's `var`). It needs an initializer, and the type is fixed after that:

  ```
  n: auto = 41          # int
  s: auto = "hi"        # str
  d: auto = 2.5d        # double
  n = 2.5d              # error: cannot assign double to int
  ```

  `auto` is not allowed for globals, parameters, return types or struct fields.

- **`generic`** is a *parameter* type that accepts a value of any type. The function becomes a template: each distinct combination of argument types gets its own concrete, file-local copy at compile time (no runtime cost, no boxing), so the body is checked with the real types.

  ```
  func add(a: generic, b: generic) -> void { print_var(a + b) }
  add(1, 2)        # int copy      -> 3
  add(1.5d, 2)     # double/int    -> 3.500000
  ```

  `generic` is only valid on function parameters (not on variables, returns, fields or nested like `[generic]`). A variable's type is still fixed at compile time; `generic` does not make a dynamically typed variable. The older spelling `any` is still accepted and means exactly the same thing.

`print_var(x)` (in `std.fl`, `std64.fl`, and built into the virtual `std`) prints any value plus a newline by its type: int/long/bool/pointers as numbers, `str` as text, float/double with 6 decimals. It is a generic function over the `__print(x)` intrinsic, which works with the real std files too (they link `flr.o` / `flrt64.o`). The built-in `print` in the virtual `std` is unchanged.

### Libraries: `strings.fl` and `io.fl`

Two optional libraries, each a single file that works on both `x86-32-linux` and `x86-64-linux` (they hold addresses in `long` and pick syscall numbers with `__arch()`). Import them next to a std, with `-I.` pointing at the repo:

```
import "std"        # or "std64"
import "strings"
import "io"
```

They only need the runtime (`_flr_alloc`, `_flr_str_len`), so they work with the built-in std, `std.fl` + `flr.o`, or `std64.fl` + `flrt64.o`.

**`strings.fl`** — all functions return fresh heap strings and never modify their arguments; indexes are 0-based; bad indexes are clamped or give `0` / `-1`.

| Function | Description |
|----------|-------------|
| `str_at(s, i) -> int` | Byte at `i` (0 if out of range) |
| `str_sub(s, start, n) -> str` | Up to `n` bytes from `start` |
| `str_find(s, needle)` / `str_find_from(s, needle, from)` | First index, or `-1` |
| `str_contains`, `str_starts_with`, `str_ends_with` | `1` / `0` |
| `str_count(s, needle)` | Non-overlapping matches |
| `str_replace(s, old, new)` | Replace every non-overlapping `old` |
| `str_trim`, `str_ltrim`, `str_rtrim` | Strip space, `\t`, `\r`, `\n` |
| `str_upper`, `str_lower` | ASCII case conversion |
| `str_repeat(s, n)`, `str_reverse(s)` | |
| `str_cmp(a, b)` | `-1`, `0` or `1` by byte order |
| `str_from_char(c)` | One-byte string |
| `str_split_count(s, sep)`, `str_split_get(s, sep, i)` | Number of parts / the i-th part |
| `char_is_digit`, `char_is_alpha`, `char_is_alnum`, `char_is_space` | `1` / `0` |

**`io.fl`** — files are `int` file descriptors (`0`, `1`, `2` are stdin/stdout/stderr). Failures return a **negative errno** (for example `-2` = no such file). Strings are NUL-terminated, so the text helpers are for text files.

| Function | Description |
|----------|-------------|
| `fio_open(path, mode) -> int` | `"r"` `"w"` `"a"` `"r+"` `"w+"` `"a+"` as in C; fd or `-errno` |
| `fio_close(fd)` | |
| `fio_write(fd, s)`, `fio_write_line(fd, s)` | Bytes written, or `-errno` |
| `fio_read(fd, n)` | Up to `n` bytes (`""` at end of file) |
| `fio_read_line(fd)` / `fio_eof()` | One line, `\n` / `\r\n` stripped; `fio_eof()` is 1 after a read that hit EOF with no data |
| `fio_read_all(fd)`, `fio_read_file(path)` | Whole file |
| `fio_write_file(path, s)`, `fio_append_file(path, s)` | Create/overwrite, or append |
| `fio_exists(path)`, `fio_size(path)` | `1`/`0`, size or `-errno` |
| `fio_seek(fd, offset, whence)` | whence `0` start, `1` current, `2` end |
| `fio_remove`, `fio_rename`, `fio_mkdir`, `fio_rmdir` | `0` or `-errno` |
| `fio_error()` | Last `fio_open` failure (negative errno) |

```
import "std"
import "io"
import "strings"
func main() -> void {
    text: str = fio_read_file("notes.txt")
    print_var(str_count(text, "\n"))
    fio_write_file("out.txt", str_upper(text))
}
```

### The real `std.fl` / `std64.fl` match the built-in std

`std.fl` (32-bit) and `std64.fl` (64-bit) were rewritten to mirror the built-in `import "std"`: the same names and the same behaviour, so a program compiles the same way with either. `print(x)` and `print_var(x)` take any type (they are `generic` functions over `__print`), and `read_line`, `read_int`, `input`, `input_str` and `stdin_eof` are the same too. `len(array)`, `str_concat(a, b)` and `str_format(fmt, ...)` are compiler builtins that now also work with the real files (a function you define yourself with one of those names still wins). `flr.s` therefore also carries `_flr_str_concat` and `_flr_str_format`, and `len` now works on `x86-64-linux` as well.

The real files additionally keep a few convenience wrappers that the built-in std does not have: `print_str`, `print_int` (no newline), `str_len`, `str_eq`, `int_to_str`, `str_to_int`, `abs`, `min`, `max`, `alloc`, `free`, `exit`, `assert`. They are in a separate section at the bottom of each file. `lib/` and `rt/` hold copies of the libraries and runtimes; keep them in sync with the root files.

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
| `__arch()` | Target word size as a constant: `32` or `64` (lets one library pick per-target syscall numbers) |

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

## Compiler flags

| Flag | Description |
|------|-------------|
| `-o <file>` | Write assembly output to `<file>` instead of stdout |
| `-arch x86-32-linux` | Target 32-bit Linux, cdecl, `int 0x80` syscalls (default) |
| `-arch x86-64-linux` | Target 64-bit Linux, System V AMD64 ABI, `syscall` |
| `--freestanding` | No runtime, no `sys_exit`; `main` falls through to `hlt` loop |
| `-I<path>` | Add `<path>` to the import search path |

## Linking

```bash
# 32-bit hosted (the normal case)
as --32 out.s -o out.o
ld -m elf_i386 flr.o out.o -o prog

# 64-bit hosted
as out.s -o out.o
ld out.o -o prog

# freestanding (OS kernel, bootloader, etc.)
as --32 out.s -o out.o
ld -m elf_i386 -T link.ld out.o -o kernel.elf
```

`flr.o` provides `_start`, all `_flr_*` symbols, and the dynamic heap. It must come before user object files on the linker command line.

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
| `27_typedef.fl` | Type aliases (`typedef`) |
| `28_static_extern.fl` | File-local linkage with `static` (see README for a two-file `extern` example) |
| `29_static_local.fl` | Function-local `static` variable persisting across calls |

Run all of them at once:

```bash
bash test.sh
```

Or manually:

```bash
for f in examples/*.fl; do
    echo "=== $f ==="
    ./falconc "$f" -o /tmp/t.s && as --32 /tmp/t.s -o /tmp/t.o && \
    ld -m elf_i386 flr.o /tmp/t.o -o /tmp/t && /tmp/t
done
```

---

## Known limitations

- **Structs need manual allocation.** Declare `p: MyStruct = _flr_alloc(...)` before writing fields. Fields are 4 bytes each on 32-bit, except `long` and `double`, which take 8. On 64-bit every field is 8 bytes.
- **No bounds checking.** Indexing past the end of an array reads or writes whatever is in memory. `len(arr)` is available for manual checks.
- **Integer division by zero crashes.** It raises SIGFPE (exit status 136) rather than a Falcon error.
- **`int` does not wrap at 32 bits on `x86-64-linux`.** Values are computed in full 64-bit registers, so `1 << 31` is `2147483648` on 64-bit but `-2147483648` on 32-bit. This is intentional: `int` also holds heap addresses, and truncating it would break pointer arithmetic. Fixing it properly needs a separate pointer type.
- **String literals are limited to 16,384 bytes.** The lexer uses a fixed buffer.
- **`int_to_str` uses a static buffer.** The returned pointer is overwritten on the next call.
- **`str_format` output is capped at 512 bytes**, with at most 8 arguments.
- **No free.** The bump allocator never reclaims memory, and `_flr_free` is a no-op.
- **No closures, first-class functions, or modules beyond `import`.** `generic` parameters give compile-time templates, but a variable's type is always fixed at compile time.