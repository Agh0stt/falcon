# flr.s — Falcon Runtime
# Pure i386 assembly, no libc, Linux int 0x80 syscalls only.
#
# Assemble:  as --32 flr.s -o flr.o
# Link user: ld -m elf_i386 flr.o code.o -o prog
#
# Public symbols:
#   _flr_print_int(n:int)           -> void
#   _flr_print_str(s:str)           -> void
#   _flr_str_len(s:str)             -> int
#   _flr_strlen(s:str)              -> int   (alias)
#   _flr_str_eq(a:str, b:str)       -> int   (1=equal)
#   _flr_int_to_str(n:int)          -> str   (static buf – use immediately)
#   _flr_str_to_int(s:str)          -> int
#   _flr_memset(ptr, val, n)        -> void
#   _flr_memcpy(dst, src, n)        -> void
#   _flr_abs(n:int)                 -> int
#   _flr_min(a:int, b:int)          -> int
#   _flr_max(a:int, b:int)          -> int
#   _flr_exit(code:int)             -> void
#   _flr_assert(cond:int, msg:str)  -> void
#   _flr_alloc(size:int)            -> ptr   (bump allocator over brk, 0 on OOM)
#   _flr_free(ptr)                  -> void  (no-op)
#   _flr_read_line()                -> str   (one line from stdin, newline stripped)
#   _flr_stdin_eof()                -> int   (1 if last read_line hit EOF with no data)

    .section .text

# ─────────────────────────────────────────────────────────────────────
# _start — ELF entry point
#   Calls main(), then exits with its return value (0 for void main).
# ─────────────────────────────────────────────────────────────────────
.weak _start
_start:
    xorl  %ebp, %ebp            # mark outermost frame (ABI convention)
    movl  (%esp), %eax          # argc is at top of stack
    leal  4(%esp), %ecx         # argv = esp+4 (pointer to argv[0])
    pushl %ecx                  # push argv (cdecl arg2)
    pushl %eax                  # push argc (cdecl arg1)
    call  main
    addl  $8, %esp
    movl  %eax, %ebx            # exit code = return value of main
    movl  $1,   %eax            # sys_exit
    int   $0x80

# ─────────────────────────────────────────────────────────────────────
# memset(ptr, val, n)  — cdecl, clobbers eax/ecx/edi
# ─────────────────────────────────────────────────────────────────────
.weak _flr_memset
_flr_memset:
    pushl %ebp
    movl  %esp, %ebp
    pushl %edi
    movl  8(%ebp),  %edi    # dst
    movl  12(%ebp), %eax    # fill byte
    movl  16(%ebp), %ecx    # count
    rep stosb
    popl  %edi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# memcpy(dst, src, n)
# ─────────────────────────────────────────────────────────────────────
.weak _flr_memcpy
_flr_memcpy:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  8(%ebp),  %edi    # dst
    movl  12(%ebp), %esi    # src
    movl  16(%ebp), %ecx    # count
    rep movsb
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# print_str(s)  — writes s + newline to stdout (fd 1)
# ─────────────────────────────────────────────────────────────────────
.weak _flr_print_str
_flr_print_str:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %ebx
    movl  8(%ebp), %esi     # s
    movl  %esi, %ecx
.Lfps_l:
    cmpb  $0, (%ecx)
    je    .Lfps_d
    incl  %ecx
    jmp   .Lfps_l
.Lfps_d:
    subl  %esi, %ecx        # ecx = length
    movl  %ecx, %edx
    movl  %esi, %ecx
    movl  $4,   %eax        # sys_write
    movl  $1,   %ebx        # fd=stdout
    int   $0x80
    # write newline
    movl  $4,   %eax
    movl  $1,   %ebx
    leal  .Lflr_nl, %ecx
    movl  $1,   %edx
    int   $0x80
    popl  %ebx
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# print_int(n)  — converts n, writes decimal + newline to stdout
# ─────────────────────────────────────────────────────────────────────
.weak _flr_print_int
_flr_print_int:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl  8(%ebp), %eax
    leal  .Lflr_ibuf+11, %edi
    movb  $10, (%edi)           # newline sentinel at end
    decl  %edi
    testl %eax, %eax
    jge   .Lfpi_pos
    negl  %eax
    movl  $1, %esi              # negative flag
    jmp   .Lfpi_l
.Lfpi_pos:
    xorl  %esi, %esi
.Lfpi_l:
    movl  $10, %ecx
    xorl  %edx, %edx
    divl  %ecx
    addb  $48, %dl
    movb  %dl, (%edi)
    decl  %edi
    testl %eax, %eax
    jne   .Lfpi_l
    testl %esi, %esi
    je    .Lfpi_nom
    movb  $45, (%edi)           # '-'
    decl  %edi
.Lfpi_nom:
    incl  %edi
    leal  .Lflr_ibuf+12, %edx
    subl  %edi, %edx            # length including newline
    movl  %edi, %ecx
    movl  $4, %eax
    movl  $1, %ebx
    int   $0x80
    popl  %ebx
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# exit(code)
# ─────────────────────────────────────────────────────────────────────
.weak _flr_exit
_flr_exit:
    movl  4(%esp), %ebx
    movl  $1, %eax
    int   $0x80

# ─────────────────────────────────────────────────────────────────────
# strlen(s) / str_len(s)  -> int
# ─────────────────────────────────────────────────────────────────────
.weak _flr_strlen
_flr_strlen:
    movl  4(%esp), %ecx
    movl  %ecx, %eax
.Lflrsl:
    cmpb  $0, (%ecx)
    je    .Lflrsld
    incl  %ecx
    jmp   .Lflrsl
.Lflrsld:
    subl  4(%esp), %ecx
    movl  %ecx, %eax
    ret

.weak _flr_str_len
_flr_str_len:
    jmp   _flr_strlen

# ─────────────────────────────────────────────────────────────────────
# str_eq(a, b) -> int  (1 = equal, 0 = different)
# ─────────────────────────────────────────────────────────────────────
.weak _flr_str_eq
_flr_str_eq:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  8(%ebp),  %esi
    movl  12(%ebp), %edi
.Lfseq:
    movzbl (%esi), %eax
    movzbl (%edi), %ecx
    cmpl  %ecx, %eax
    jne   .Lfseq_no
    testl %eax, %eax
    je    .Lfseq_yes
    incl  %esi
    incl  %edi
    jmp   .Lfseq
.Lfseq_yes:
    movl  $1, %eax
    jmp   .Lfseq_ret
.Lfseq_no:
    xorl  %eax, %eax
.Lfseq_ret:
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# int_to_str(n) -> str  (pointer into static buffer — use before next call)
# ─────────────────────────────────────────────────────────────────────
.weak _flr_int_to_str
_flr_int_to_str:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    pushl %edi
    movl  8(%ebp), %eax
    movl  .Lflr_iti, %ecx       # rotate through 8 slots of 32 bytes
    incl  %ecx
    andl  $7, %ecx
    movl  %ecx, .Lflr_iti
    shll  $5, %ecx
    leal  .Lflr_its(%ecx), %edi
    addl  $31, %edi
    movb  $0, (%edi)            # NUL terminator
    decl  %edi
    testl %eax, %eax
    jge   .Lfits_p
    negl  %eax
    movl  $1, %esi
    jmp   .Lfits_l
.Lfits_p:
    xorl  %esi, %esi
.Lfits_l:
    movl  $10, %ecx
    xorl  %edx, %edx
    divl  %ecx
    addb  $48, %dl
    movb  %dl, (%edi)
    decl  %edi
    testl %eax, %eax
    jne   .Lfits_l
    testl %esi, %esi
    je    .Lfits_n
    movb  $45, (%edi)           # '-'
    decl  %edi
.Lfits_n:
    incl  %edi
    movl  %edi, %eax            # return pointer to start of string
    popl  %edi
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# str_to_int(s) -> int
# ─────────────────────────────────────────────────────────────────────
.weak _flr_str_to_int
_flr_str_to_int:
    pushl %ebp
    movl  %esp, %ebp
    pushl %esi
    movl  8(%ebp), %esi
    xorl  %eax, %eax
    xorl  %ecx, %ecx
    cmpb  $45, (%esi)           # '-'?
    jne   .Lfsti_l
    movl  $1, %ecx
    incl  %esi
.Lfsti_l:
    movzbl (%esi), %edx
    cmpl  $48, %edx
    jl    .Lfsti_d
    cmpl  $57, %edx
    jg    .Lfsti_d
    imull $10, %eax
    subl  $48, %edx
    addl  %edx, %eax
    incl  %esi
    jmp   .Lfsti_l
.Lfsti_d:
    testl %ecx, %ecx
    je    .Lfsti_r
    negl  %eax
.Lfsti_r:
    popl  %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# abs(n) -> int
# ─────────────────────────────────────────────────────────────────────
.weak _flr_abs
_flr_abs:
    movl  4(%esp), %eax
    testl %eax, %eax
    jge   .Labs_ok
    negl  %eax
.Labs_ok:
    ret

# ─────────────────────────────────────────────────────────────────────
# min(a, b) -> int
# ─────────────────────────────────────────────────────────────────────
.weak _flr_min
_flr_min:
    movl  4(%esp), %eax
    movl  8(%esp), %ecx
    cmpl  %ecx, %eax
    jle   .Lmin_ok
    movl  %ecx, %eax
.Lmin_ok:
    ret

# ─────────────────────────────────────────────────────────────────────
# max(a, b) -> int
# ─────────────────────────────────────────────────────────────────────
.weak _flr_max
_flr_max:
    movl  4(%esp), %eax
    movl  8(%esp), %ecx
    cmpl  %ecx, %eax
    jge   .Lmax_ok
    movl  %ecx, %eax
.Lmax_ok:
    ret

# ─────────────────────────────────────────────────────────────────────
# assert(cond, msg)  — prints "assertion failed: <msg>\n" to stderr
#                       then calls sys_exit(1) if cond == 0
# ─────────────────────────────────────────────────────────────────────
.weak _flr_assert
_flr_assert:
    pushl %ebp
    movl  %esp, %ebp
    movl  8(%ebp), %eax
    testl %eax, %eax
    jne   .Lassert_ok
    # write "assertion failed: " to stderr (fd 2)
    movl  $4,  %eax
    movl  $2,  %ebx
    leal  .Lflr_assert_msg, %ecx
    movl  $18, %edx
    int   $0x80
    # write user message
    movl  12(%ebp), %esi
    movl  %esi, %ecx
.Lasssl:
    cmpb  $0, (%ecx)
    je    .Lasssd
    incl  %ecx
    jmp   .Lasssl
.Lasssd:
    subl  %esi, %ecx
    movl  %ecx, %edx
    movl  %esi, %ecx
    movl  $4,   %eax
    movl  $2,   %ebx
    int   $0x80
    # write newline
    movl  $4,  %eax
    movl  $2,  %ebx
    leal  .Lflr_nl, %ecx
    movl  $1,  %edx
    int   $0x80
    # exit(1)
    movl  $1, %ebx
    movl  $1, %eax
    int   $0x80
.Lassert_ok:
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# alloc(size) -> ptr   bump allocator over brk-grown chunks (zeroed, 4-aligned)
#   Returns 0 when out of memory.  The break is raised by max(size, 1 MB) at a
#   time, so there is no fixed heap size.
# free(ptr)           no-op
# ─────────────────────────────────────────────────────────────────────
.weak _flr_alloc
_flr_alloc:
    pushl %ebp
    movl  %esp, %ebp
    pushl %ebx
    movl  8(%ebp), %edx
    addl  $3, %edx
    jc    .Lfal_fail
    andl  $-4, %edx
    jnz   .Lfal_nz
    movl  $4, %edx
.Lfal_nz:
    movl  _flr_heap_ptr, %eax
    movl  %eax, %ecx
    addl  %edx, %ecx
    jc    .Lfal_fail
    cmpl  _flr_heap_end, %ecx
    ja    .Lfal_grow
    movl  %ecx, _flr_heap_ptr
    popl  %ebx
    leave
    ret
.Lfal_grow:
    pushl %edx
    movl  $45, %eax             # sys_brk(0) -> current break
    xorl  %ebx, %ebx
    int   $0x80
    popl  %edx
    movl  %eax, %ecx
    movl  %edx, %ebx
    cmpl  $0x100000, %ebx
    jae   .Lfal_sz
    movl  $0x100000, %ebx
.Lfal_sz:
    addl  $4095, %ebx
    jc    .Lfal_fail
    andl  $-4096, %ebx
    addl  %ecx, %ebx
    jc    .Lfal_fail
    pushl %ecx
    pushl %edx
    pushl %ebx
    movl  $45, %eax             # sys_brk(new break)
    int   $0x80
    popl  %ebx
    popl  %edx
    popl  %ecx
    cmpl  %ebx, %eax
    jb    .Lfal_fail
    cmpl  _flr_heap_end, %ecx
    je    .Lfal_ext
    movl  %ecx, _flr_heap_ptr
.Lfal_ext:
    movl  %ebx, _flr_heap_end
    jmp   .Lfal_nz
.Lfal_fail:
    xorl  %eax, %eax
    popl  %ebx
    leave
    ret

.weak _flr_free
_flr_free:
    ret

.weak _flr_read_line
_flr_read_line:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    subl $8,%esp
    movl $0,.Lflr_eof
    movl $128,-16(%ebp)
    pushl $128
    call _flr_alloc
    addl $4,%esp
    movl %eax,%esi
    xorl %edi,%edi
.Lrl_loop:
    movl $3,%eax
    xorl %ebx,%ebx
    leal -20(%ebp),%ecx
    movl $1,%edx
    int $0x80
    cmpl $-4,%eax
    je .Lrl_loop
    testl %eax,%eax
    jle .Lrl_eof
    movzbl -20(%ebp),%eax
    cmpl $10,%eax
    je .Lrl_done
    leal 1(%edi),%edx
    cmpl -16(%ebp),%edx
    jb .Lrl_store
    movl -16(%ebp),%edx
    addl %edx,%edx
    movl %edx,-16(%ebp)
    pushl %edx
    call _flr_alloc
    addl $4,%esp
    xorl %edx,%edx
.Lrl_cp:
    cmpl %edi,%edx
    jae .Lrl_cpd
    movb (%esi,%edx),%bl
    movb %bl,(%eax,%edx)
    incl %edx
    jmp .Lrl_cp
.Lrl_cpd:
    movl %eax,%esi
.Lrl_store:
    movzbl -20(%ebp),%eax
    movb %al,(%esi,%edi)
    incl %edi
    jmp .Lrl_loop
.Lrl_eof:
    testl %edi,%edi
    jne .Lrl_done
    movl $1,.Lflr_eof
.Lrl_done:
    testl %edi,%edi
    je .Lrl_term
    cmpb $13,-1(%esi,%edi)
    jne .Lrl_term
    decl %edi
.Lrl_term:
    movb $0,(%esi,%edi)
    movl %esi,%eax
    addl $8,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.weak _flr_stdin_eof
_flr_stdin_eof:
    movl .Lflr_eof,%eax
    ret

# print_float / print_double / u64_nonl (extracted from falconc's 32-bit runtime)
.weak _flr_print_float
_flr_print_float:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    flds 8(%ebp)
    subl $8,%esp
    fstpl (%esp)
    pushl 4(%esp)
    pushl 4(%esp)
    call _flr_print_double
    addl $8,%esp
    addl $8,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.weak _flr_print_double
_flr_print_double:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 12(%ebp),%eax
    movl %eax,%ecx
    shrl $20,%ecx
    andl $0x7ff,%ecx
    cmpl $0x7ff,%ecx
    je .Lfpd_special
    testl %eax,%eax
    jns .Lfpd_pos
    movl $4,%eax
    movl $1,%ebx
    leal .Lflr_minus,%ecx
    movl $1,%edx
    int $0x80
.Lfpd_pos:
    fldl 8(%ebp)
    fabs
    fldl .Lflr_big
    fucomip %st(1),%st
    jbe .Lfpd_big
    fldl .Lflr_1e6
    fmulp
    subl $8,%esp
    fistpll (%esp)
    popl %eax
    popl %edx
    movl %eax,%ebx
    movl %edx,%eax
    xorl %edx,%edx
    movl $1000000,%ecx
    divl %ecx
    movl %eax,%esi
    movl %ebx,%eax
    divl %ecx
    movl %edx,%edi
    movl %esi,%edx
    jmp .Lfpd_emit
.Lfpd_big:
    fld1
    fldl 8(%ebp)
    fabs
.Lfpd_bfp:
    fprem
    fnstsw %ax
    testb $4,%ah
    jnz .Lfpd_bfp
    fstp %st(1)
    fldl .Lflr_1e6
    fmulp
    subl $4,%esp
    fistpl (%esp)
    popl %edi
    subl $16,%esp
    fnstcw 8(%esp)
    movzwl 8(%esp),%eax
    orl $0x0c00,%eax
    movw %ax,10(%esp)
    fldcw 10(%esp)
    fistpll (%esp)
    fldcw 8(%esp)
    movl (%esp),%eax
    movl 4(%esp),%edx
    addl $16,%esp
    testl %edx,%edx
    jns .Lfpd_bok
    movl $0xffffffff,%eax
    movl $0x7fffffff,%edx
    xorl %edi,%edi
    jmp .Lfpd_emit
.Lfpd_bok:
    cmpl $1000000,%edi
    jb .Lfpd_emit
    xorl %edi,%edi
    addl $1,%eax
    adcl $0,%edx
.Lfpd_emit:
    call _flr_u64_nonl
    movl $4,%eax
    movl $1,%ebx
    leal .Lflr_dot,%ecx
    movl $1,%edx
    int $0x80
    movl %edi,%eax
    leal .Lflr_ibuf+11,%edi
    movl $6,%ecx
.Lfpd_fl:
    movl $10,%esi
    xorl %edx,%edx
    divl %esi
    addb $48,%dl
    movb %dl,(%edi)
    decl %edi
    loop .Lfpd_fl
    incl %edi
    movl $6,%edx
    movl %edi,%ecx
    movl $4,%eax
    movl $1,%ebx
    int $0x80
    movl $4,%eax
    movl $1,%ebx
    leal .Lflr_nl,%ecx
    movl $1,%edx
    int $0x80
    jmp .Lfpd_ret
.Lfpd_special:
    movl 12(%ebp),%eax
    andl $0x000fffff,%eax
    orl 8(%ebp),%eax
    jne .Lfpd_nan
    cmpl $0,12(%ebp)
    jl .Lfpd_ninf
    leal .Lflr_inf,%ecx
    movl $4,%edx
    jmp .Lfpd_sp_w
.Lfpd_ninf:
    leal .Lflr_ninf,%ecx
    movl $5,%edx
    jmp .Lfpd_sp_w
.Lfpd_nan:
    leal .Lflr_nan,%ecx
    movl $4,%edx
.Lfpd_sp_w:
    movl $4,%eax
    movl $1,%ebx
    int $0x80
.Lfpd_ret:
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.weak _flr_u64_nonl
_flr_u64_nonl:
    pushl %esi
    pushl %edi
    pushl %ebx
    movl %edx,%ebx
    movl %eax,%esi
    leal .Lflr_ibuf+31,%edi
.Lu64_l:
    movl $10,%ecx
    movl %ebx,%eax
    xorl %edx,%edx
    divl %ecx
    movl %eax,%ebx
    movl %esi,%eax
    divl %ecx
    movl %eax,%esi
    addb $48,%dl
    movb %dl,(%edi)
    decl %edi
    movl %ebx,%eax
    orl %esi,%eax
    jne .Lu64_l
    leal .Lflr_ibuf+31,%edx
    subl %edi,%edx
    incl %edi
    movl %edi,%ecx
    movl $4,%eax
    movl $1,%ebx
    int $0x80
    popl %ebx
    popl %edi
    popl %esi
    ret

.weak _flr_input_str
_flr_input_str:
    movl 4(%esp),%ecx
    movl %ecx,%edx
.Lris_l:
    cmpb $0,(%edx)
    je .Lris_d
    incl %edx
    jmp .Lris_l
.Lris_d:
    subl %ecx,%edx
    pushl %ebx
    movl $4,%eax
    movl $1,%ebx
    int $0x80
    popl %ebx
    jmp _flr_read_line

# str_concat / str_format (extracted from falconc's 32-bit runtime)
.weak _flr_str_concat
_flr_str_concat:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%esi
    movl 12(%ebp),%edi
    pushl %esi
    call _flr_strlen
    addl $4,%esp
    movl %eax,%ebx
    pushl %edi
    call _flr_strlen
    addl $4,%esp
    addl %ebx,%eax
    incl %eax
    pushl %eax
    call _flr_alloc
    addl $4,%esp
    pushl %eax
    movl %eax,%edi
.Lsca_l:
    movzbl (%esi),%ecx
    testl %ecx,%ecx
    je .Lsca_d
    movb %cl,(%edi)
    incl %esi
    incl %edi
    jmp .Lsca_l
.Lsca_d:
    movl 12(%ebp),%esi
.Lscb_l:
    movzbl (%esi),%ecx
    movb %cl,(%edi)
    testl %ecx,%ecx
    je .Lscb_d
    incl %esi
    incl %edi
    jmp .Lscb_l
.Lscb_d:
    popl %eax
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.weak _flr_str_format
_flr_str_format:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    pushl $512
    call _flr_alloc
    addl $4,%esp
    movl %eax,%edi
    pushl %edi
    leal 511(%edi),%edx
    movl %edx,.Lflr_sfend
    movl 8(%ebp),%esi
    movl 12(%ebp),%ecx
    leal 16(%ebp),%ebx
.Lsfmt_l:
    movzbl (%esi),%eax
    testl %eax,%eax
    je .Lsfmt_end
    cmpl $37,%eax
    jne .Lsfmt_copy
    incl %esi
    movzbl (%esi),%eax
    cmpl $37,%eax
    je .Lsfmt_copy
    cmpl $100,%eax
    je .Lsfmt_d
    cmpl $115,%eax
    je .Lsfmt_s
    decl %esi
    movl $37,%eax
    jmp .Lsfmt_copy
.Lsfmt_d:
    incl %esi
    testl %ecx,%ecx
    je .Lsfmt_l
    pushl %esi
    movl (%ebx),%eax
    addl $4,%ebx
    decl %ecx
    pushl %ecx
    pushl %eax
    call _flr_int_to_str
    addl $4,%esp
    popl %ecx
    movl %eax,%esi
.Lsfmt_dc:
    movzbl (%esi),%eax
    testl %eax,%eax
    je .Lsfmt_dr
    cmpl .Lflr_sfend,%edi
    jae 1f
    movb %al,(%edi)
    incl %edi
1:
    incl %esi
    jmp .Lsfmt_dc
.Lsfmt_dr:
    popl %esi
    jmp .Lsfmt_l
.Lsfmt_s:
    incl %esi
    testl %ecx,%ecx
    je .Lsfmt_l
    pushl %esi
    movl (%ebx),%esi
    addl $4,%ebx
    decl %ecx
.Lsfmt_sc:
    movzbl (%esi),%eax
    testl %eax,%eax
    je .Lsfmt_sr
    cmpl .Lflr_sfend,%edi
    jae 1f
    movb %al,(%edi)
    incl %edi
1:
    incl %esi
    jmp .Lsfmt_sc
.Lsfmt_sr:
    popl %esi
    jmp .Lsfmt_l
.Lsfmt_copy:
    cmpl .Lflr_sfend,%edi
    jae 1f
    movb %al,(%edi)
    incl %edi
1:
    incl %esi
    jmp .Lsfmt_l
.Lsfmt_end:
    movb $0,(%edi)
    popl %eax
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

# ─────────────────────────────────────────────────────────────────────
# Data
# ─────────────────────────────────────────────────────────────────────
    .section .data
    .align 4
.Lflr_ibuf:
    .space 32                   # scratch buffer for print_int
.Lflr_its:
    .space 256                  # 8 x 32-byte slots used by int_to_str
.Lflr_iti:
    .long 0
.Lflr_sfend:
    .long 0                     # str_format output limit
.Lflr_eof:
    .long 0                     # set by _flr_read_line on end of input
.Lflr_nl:
    .byte 10
.Lflr_dot:
    .ascii "."
.Lflr_minus:
    .ascii "-"
.Lflr_1e6:
    .long 0
    .long 1093567616
.Lflr_big:
    .long 2602565632
    .long 1117806323
.Lflr_nan:
    .ascii "nan\n"
.Lflr_inf:
    .ascii "inf\n"
.Lflr_ninf:
    .ascii "-inf\n"                    # newline character
.Lflr_assert_msg:
    .ascii "assertion failed: "
_flr_heap_ptr:
    .long 0
_flr_heap_end:
    .long 0
