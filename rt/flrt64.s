# flrt64.s — Falcon Runtime, x86-64 Linux (System V AMD64 ABI, `syscall`, no libc)
#
# 64-bit counterpart of flr.s. Used with the real std64.fl:
#
#   falconc prog.fl -arch x86-64-linux -I. -o prog.s     # prog.fl: import "std64"
#   as prog.s -o prog.o
#   as flrt64.s -o flrt64.o
#   ld flrt64.o prog.o -o prog
#
# Extracted from the 64-bit runtime that falconc emits for the built-in virtual
# `std` (so behaviour is identical). Every _flr_* symbol is .weak, so a program
# that uses the virtual std (which emits its own copies) still links cleanly
# against this file. If you change the runtime in falconc.c, re-extract this file.
#
# Public symbols (arguments in rdi, rsi, rdx, ...; result in rax / xmm0):
#   _start, _flr_memset, _flr_memcpy, _flr_print_str, _flr_print_int,
#   _flr_print_int_nonl, _flr_print_float, _flr_print_double, _flr_exit,
#   _flr_strlen, _flr_str_len, _flr_str_eq, _flr_int_to_str, _flr_str_to_int,
#   _flr_abs, _flr_min, _flr_max, _flr_assert, _flr_alloc (-> long), _flr_free,
#   _flr_str_concat, _flr_str_format, _flr_read_line, _flr_stdin_eof

.section .text
.weak _start
_start:
    xorl %ebp,%ebp
    movq (%rsp),%r12
    leaq 8(%rsp),%r13
    movq %r12,%rax
    imulq $8,%rax,%rax
    addq $16,%rax
    movq %rax,%rdi
    call _flr_alloc
    movq %rax,%r14
    movq %r12,(%r14)
    movq %r12,8(%r14)
    xorq %rcx,%rcx
.Lflr_argv_copy64:
    cmpq %r12,%rcx
    jge .Lflr_argv_done64
    movq (%r13,%rcx,8),%rdx
    movq %rdx,16(%r14,%rcx,8)
    incq %rcx
    jmp .Lflr_argv_copy64
.Lflr_argv_done64:
    movq %r12,%rdi
    movq %r14,%rsi
    call main
    movl %eax,%edi
    movl $60,%eax
    syscall

# ── Falcon Runtime 64-bit (inline) ───────────────────────────
.weak _flr_memset
_flr_memset:
    movq %rdi,%rax
    movq %rdx,%rcx
    movb %sil,%al
    rep stosb
    ret

.weak _flr_memcpy
_flr_memcpy:
    movq %rdx,%rcx
    rep movsb
    ret

.weak _flr_print_str
_flr_print_str:
    pushq %rbx
    movq %rdi,%rbx
    movq %rdi,%rsi
.Lfps64_l:
    cmpb $0,(%rsi)
    je .Lfps64_d
    incq %rsi
    jmp .Lfps64_l
.Lfps64_d:
    subq %rbx,%rsi
    movq %rsi,%rdx
    movq $1,%rax
    movq $1,%rdi
    movq %rbx,%rsi
    syscall
    movq $1,%rax
    movq $1,%rdi
    leaq .Lflr_nl(%rip),%rsi
    movq $1,%rdx
    syscall
    popq %rbx
    ret

.weak _flr_print_int
_flr_print_int:
    pushq %rbx
    pushq %r12
    pushq %r13
    movq %rdi,%rax
    leaq .Lflr_ibuf+22(%rip),%r12
    movb $10,(%r12)
    decq %r12
    testq %rax,%rax
    jge .Lfpi64_pos
    negq %rax
    movq $1,%r13
    jmp .Lfpi64_l
.Lfpi64_pos:
    xorl %r13d,%r13d
.Lfpi64_l:
    movq $10,%rcx
    xorl %edx,%edx
    divq %rcx
    addb $48,%dl
    movb %dl,(%r12)
    decq %r12
    testq %rax,%rax
    jne .Lfpi64_l
    testq %r13,%r13
    je .Lfpi64_nom
    movb $45,(%r12)
    decq %r12
.Lfpi64_nom:
    incq %r12
    leaq .Lflr_ibuf+23(%rip),%rdx
    subq %r12,%rdx
    movq $1,%rax
    movq $1,%rdi
    movq %r12,%rsi
    syscall
    popq %r13
    popq %r12
    popq %rbx
    ret

.weak _flr_exit
_flr_exit:
    movl $60,%eax
    syscall

.weak _flr_print_float
_flr_print_float:
    cvtss2sd %xmm0,%xmm0
    call _flr_print_double
    ret

.weak _flr_print_double
_flr_print_double:
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    movq %xmm0,%rax
    movq %rax,%rcx
    shrq $52,%rcx
    andl $0x7ff,%ecx
    cmpl $0x7ff,%ecx
    je .Lfpd64_special
    testq %rax,%rax
    jns .Lfpd64_pos
    btrq $63,%rax
    movq %rax,%xmm0
    movq $1,%rax
    movq $1,%rdi
    leaq .Lflr_minus(%rip),%rsi
    movq $1,%rdx
    syscall
.Lfpd64_pos:
    movsd .Lflr_big(%rip),%xmm1
    ucomisd %xmm1,%xmm0
    jae .Lfpd64_big
    mulsd .Lflr_1e6(%rip),%xmm0
    cvtsd2si %xmm0,%rax
    xorl %edx,%edx
    movq $1000000,%rcx
    divq %rcx
    movq %rdx,%r14
    movq %rax,%rdi
    jmp .Lfpd64_emit
.Lfpd64_big:
    cvttsd2si %xmm0,%rdi
    xorl %r14d,%r14d
    testq %rdi,%rdi
    jns .Lfpd64_bok
    movabsq $0x7fffffffffffffff,%rdi
    jmp .Lfpd64_emit
.Lfpd64_bok:
    cvtsi2sdq %rdi,%xmm1
    subsd %xmm1,%xmm0
    mulsd .Lflr_1e6(%rip),%xmm0
    cvtsd2si %xmm0,%r14
    cmpq $1000000,%r14
    jb .Lfpd64_emit
    xorl %r14d,%r14d
    incq %rdi
.Lfpd64_emit:
    call _flr_print_int_nonl
    movq $1,%rax
    movq $1,%rdi
    leaq .Lflr_dot(%rip),%rsi
    movq $1,%rdx
    syscall
    movq %r14,%rax
    leaq .Lflr_ibuf+12(%rip),%r12
    decq %r12
    movl $6,%ecx
.Lfpd64_fl:
    movq $10,%r13
    xorl %edx,%edx
    divq %r13
    addb $48,%dl
    movb %dl,(%r12)
    decq %r12
    loop .Lfpd64_fl
    incq %r12
    movq $1,%rax
    movq $1,%rdi
    movq %r12,%rsi
    movq $6,%rdx
    syscall
    movq $1,%rax
    movq $1,%rdi
    leaq .Lflr_nl(%rip),%rsi
    movq $1,%rdx
    syscall
    jmp .Lfpd64_ret
.Lfpd64_special:
    movq %xmm0,%rax
    movabsq $0x000fffffffffffff,%rcx
    andq %rcx,%rax
    jne .Lfpd64_nan
    movq %xmm0,%rax
    testq %rax,%rax
    js .Lfpd64_ninf
    leaq .Lflr_inf(%rip),%rsi
    movq $4,%rdx
    jmp .Lfpd64_sp_w
.Lfpd64_ninf:
    leaq .Lflr_ninf(%rip),%rsi
    movq $5,%rdx
    jmp .Lfpd64_sp_w
.Lfpd64_nan:
    leaq .Lflr_nan(%rip),%rsi
    movq $4,%rdx
.Lfpd64_sp_w:
    movq $1,%rax
    movq $1,%rdi
    syscall
.Lfpd64_ret:
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    ret

.weak _flr_print_int_nonl
_flr_print_int_nonl:
    pushq %rbx
    pushq %r12
    pushq %r13
    movq %rdi,%rax
    leaq .Lflr_ibuf+22(%rip),%r12
    movb $0,(%r12)
    decq %r12
    testq %rax,%rax
    jge .Lpni64_pos
    negq %rax
    movq $1,%r13
    jmp .Lpni64_l
.Lpni64_pos:
    xorl %r13d,%r13d
.Lpni64_l:
    movq $10,%rcx
    xorl %edx,%edx
    divq %rcx
    addb $48,%dl
    movb %dl,(%r12)
    decq %r12
    testq %rax,%rax
    jne .Lpni64_l
    testq %r13,%r13
    je .Lpni64_nom
    movb $45,(%r12)
    decq %r12
.Lpni64_nom:
    incq %r12
    leaq .Lflr_ibuf+22(%rip),%rdx
    subq %r12,%rdx
    movq $1,%rax
    movq $1,%rdi
    movq %r12,%rsi
    syscall
    popq %r13
    popq %r12
    popq %rbx
    ret

.weak _flr_strlen
_flr_strlen:
    movq %rdi,%rax
.Lflrsl64:
    cmpb $0,(%rax)
    je .Lflrsld64
    incq %rax
    jmp .Lflrsl64
.Lflrsld64:
    subq %rdi,%rax
    ret

.weak _flr_str_len
_flr_str_len:
    jmp _flr_strlen

.weak _flr_str_eq
_flr_str_eq:
.Lfseq64:
    movzbl (%rdi),%eax
    movzbl (%rsi),%ecx
    cmpl %ecx,%eax
    jne .Lfseq64_no
    testl %eax,%eax
    je .Lfseq64_yes
    incq %rdi
    incq %rsi
    jmp .Lfseq64
.Lfseq64_yes:
    movl $1,%eax
    ret
.Lfseq64_no:
    xorl %eax,%eax
    ret

.weak _flr_int_to_str
_flr_int_to_str:
    pushq %rbx
    pushq %r12
    movq %rdi,%rax
    movl .Lflr_iti(%rip),%ecx
    incl %ecx
    andl $7,%ecx
    movl %ecx,.Lflr_iti(%rip)
    shlq $5,%rcx
    leaq .Lflr_its(%rip),%r12
    addq %rcx,%r12
    addq $31,%r12
    movb $0,(%r12)
    decq %r12
    testq %rax,%rax
    jge .Lfits64_p
    negq %rax
    movq $1,%rbx
    jmp .Lfits64_l
.Lfits64_p:
    xorl %ebx,%ebx
.Lfits64_l:
    movq $10,%rcx
    xorl %edx,%edx
    divq %rcx
    addb $48,%dl
    movb %dl,(%r12)
    decq %r12
    testq %rax,%rax
    jne .Lfits64_l
    testq %rbx,%rbx
    je .Lfits64_n
    movb $45,(%r12)
    decq %r12
.Lfits64_n:
    incq %r12
    movq %r12,%rax
    popq %r12
    popq %rbx
    ret

.weak _flr_str_to_int
_flr_str_to_int:
    xorl %eax,%eax
    xorl %ecx,%ecx
    cmpb $45,(%rdi)
    jne .Lfsti64_l
    movl $1,%ecx
    incq %rdi
.Lfsti64_l:
    movzbl (%rdi),%edx
    cmpl $48,%edx
    jl .Lfsti64_d
    cmpl $57,%edx
    jg .Lfsti64_d
    imulq $10,%rax
    subl $48,%edx
    addq %rdx,%rax
    incq %rdi
    jmp .Lfsti64_l
.Lfsti64_d:
    testl %ecx,%ecx
    je .Lfsti64_r
    negq %rax
.Lfsti64_r:
    ret

.weak _flr_abs
_flr_abs:
    movq %rdi,%rax
    testq %rax,%rax
    jge .Labs64_ok
    negq %rax
.Labs64_ok:
    ret

.weak _flr_min
_flr_min:
    movq %rdi,%rax
    cmpq %rsi,%rax
    jle .Lmin64_ok
    movq %rsi,%rax
.Lmin64_ok:
    ret

.weak _flr_max
_flr_max:
    movq %rdi,%rax
    cmpq %rsi,%rax
    jge .Lmax64_ok
    movq %rsi,%rax
.Lmax64_ok:
    ret

.weak _flr_assert
_flr_assert:
    testq %rdi,%rdi
    jne .Lassert64_ok
    pushq %rsi
    movq $1,%rax
    movq $2,%rdi
    leaq .Lflr_assert_msg(%rip),%rsi
    movq $18,%rdx
    syscall
    movq (%rsp),%rdi
    call _flr_strlen
    movq %rax,%rdx
    movq (%rsp),%rsi
    movq $1,%rax
    movq $2,%rdi
    syscall
    movq $1,%rax
    movq $2,%rdi
    leaq .Lflr_nl(%rip),%rsi
    movq $1,%rdx
    syscall
    movq $60,%rax
    movq $1,%rdi
    syscall
.Lassert64_ok:
    ret

.weak _flr_alloc
_flr_alloc:
    pushq %rbx
    movq %rdi,%rsi
    addq $7,%rsi
    jc .Lfal64_fail
    andq $-8,%rsi
    jnz .Lfal64_nz
    movl $8,%esi
.Lfal64_nz:
    movq .Lflr_hp(%rip),%rax
    movq %rax,%rcx
    addq %rsi,%rcx
    jc .Lfal64_fail
    cmpq .Lflr_he(%rip),%rcx
    ja .Lfal64_grow
    movq %rcx,.Lflr_hp(%rip)
    popq %rbx
    ret
.Lfal64_grow:
    movq %rsi,%rbx
    movq %rsi,%rcx
    cmpq $0x100000,%rcx
    jae .Lfal64_sz
    movl $0x100000,%ecx
.Lfal64_sz:
    addq $4095,%rcx
    jc .Lfal64_fail
    andq $-4096,%rcx
    movq %rcx,%rsi
    xorl %edi,%edi
    movl $3,%edx
    movl $0x22,%r10d
    movq $-1,%r8
    xorl %r9d,%r9d
    movl $9,%eax
    syscall
    cmpq $-4095,%rax
    jae .Lfal64_fail
    movq %rax,.Lflr_hp(%rip)
    leaq (%rax,%rsi),%rcx
    movq %rcx,.Lflr_he(%rip)
    movq %rbx,%rsi
    jmp .Lfal64_nz
.Lfal64_fail:
    xorl %eax,%eax
    popq %rbx
    ret

.weak _flr_free
_flr_free:
    ret

.weak _flr_str_concat
_flr_str_concat:
    pushq %rbx
    pushq %r12
    pushq %r13
    movq %rdi,%r12
    movq %rsi,%r13
    call _flr_strlen
    movq %rax,%rbx
    movq %r13,%rdi
    call _flr_strlen
    addq %rbx,%rax
    incq %rax
    movq %rax,%rdi
    call _flr_alloc
    pushq %rax
    movq %rax,%rdi
    movq %r12,%rsi
.Lsca64_l:
    movzbl (%rsi),%ecx
    testl %ecx,%ecx
    je .Lsca64_d
    movb %cl,(%rdi)
    incq %rsi
    incq %rdi
    jmp .Lsca64_l
.Lsca64_d:
    movq %r13,%rsi
.Lscb64_l:
    movzbl (%rsi),%ecx
    movb %cl,(%rdi)
    testl %ecx,%ecx
    je .Lscb64_d
    incq %rsi
    incq %rdi
    jmp .Lscb64_l
.Lscb64_d:
    popq %rax
    popq %r13
    popq %r12
    popq %rbx
    ret

.weak _flr_str_format
_flr_str_format:
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rdi,%r12
    movq %rsi,%r13
    leaq 48(%rsp),%r14
    movq $512,%rdi
    call _flr_alloc
    movq %rax,%r15
    pushq %rax
    leaq 511(%rax),%rdx
    movq %rdx,.Lflr_sfend(%rip)
    movq %rax,%rbx
.Lsfmt64_l:
    movzbl (%r12),%eax
    testl %eax,%eax
    je .Lsfmt64_end
    cmpl $37,%eax
    jne .Lsfmt64_copy
    incq %r12
    movzbl (%r12),%eax
    cmpl $37,%eax
    je .Lsfmt64_copy
    cmpl $100,%eax
    je .Lsfmt64_d
    cmpl $115,%eax
    je .Lsfmt64_s
    decq %r12
    movl $37,%eax
    jmp .Lsfmt64_copy
.Lsfmt64_d:
    incq %r12
    testq %r13,%r13
    je .Lsfmt64_l
    movq (%r14),%rdi
    addq $8,%r14
    decq %r13
    call _flr_int_to_str
    movq %rax,%rsi
.Lsfmt64_dc:
    movzbl (%rsi),%eax
    testl %eax,%eax
    je .Lsfmt64_dr
    cmpq .Lflr_sfend(%rip),%rbx
    jae 1f
    movb %al,(%rbx)
    incq %rbx
1:
    incq %rsi
    jmp .Lsfmt64_dc
.Lsfmt64_dr:
    jmp .Lsfmt64_l
.Lsfmt64_s:
    incq %r12
    testq %r13,%r13
    je .Lsfmt64_l
    movq (%r14),%rsi
    addq $8,%r14
    decq %r13
.Lsfmt64_sc:
    movzbl (%rsi),%eax
    testl %eax,%eax
    je .Lsfmt64_sr
    cmpq .Lflr_sfend(%rip),%rbx
    jae 1f
    movb %al,(%rbx)
    incq %rbx
1:
    incq %rsi
    jmp .Lsfmt64_sc
.Lsfmt64_sr:
    jmp .Lsfmt64_l
.Lsfmt64_copy:
    cmpq .Lflr_sfend(%rip),%rbx
    jae 1f
    movb %al,(%rbx)
    incq %rbx
1:
    incq %r12
    jmp .Lsfmt64_l
.Lsfmt64_end:
    movb $0,(%rbx)
    popq %rax
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    ret

.weak _flr_read_line
_flr_read_line:
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    subq $24,%rsp
    movl $0,.Lflr_eof(%rip)
    movq $128,%r14
    movq %r14,%rdi
    call _flr_alloc
    movq %rax,%r12
    xorl %r13d,%r13d
.Lrl64_loop:
    xorl %eax,%eax
    xorl %edi,%edi
    movq %rsp,%rsi
    movl $1,%edx
    syscall
    cmpq $-4,%rax
    je .Lrl64_loop
    testq %rax,%rax
    jle .Lrl64_eof
    movzbl (%rsp),%eax
    cmpl $10,%eax
    je .Lrl64_done
    leaq 1(%r13),%rdx
    cmpq %r14,%rdx
    jb .Lrl64_store
    addq %r14,%r14
    movq %r14,%rdi
    call _flr_alloc
    xorl %ecx,%ecx
.Lrl64_cp:
    cmpq %r13,%rcx
    jae .Lrl64_cpd
    movb (%r12,%rcx),%dl
    movb %dl,(%rax,%rcx)
    incq %rcx
    jmp .Lrl64_cp
.Lrl64_cpd:
    movq %rax,%r12
.Lrl64_store:
    movzbl (%rsp),%eax
    movb %al,(%r12,%r13)
    incq %r13
    jmp .Lrl64_loop
.Lrl64_eof:
    testq %r13,%r13
    jne .Lrl64_done
    movl $1,.Lflr_eof(%rip)
.Lrl64_done:
    testq %r13,%r13
    je .Lrl64_term
    cmpb $13,-1(%r12,%r13)
    jne .Lrl64_term
    decq %r13
.Lrl64_term:
    movb $0,(%r12,%r13)
    movq %r12,%rax
    addq $24,%rsp
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    ret
.weak _flr_stdin_eof
_flr_stdin_eof:
    movl .Lflr_eof(%rip),%eax
    ret

.weak _flr_input_str
_flr_input_str:
    movq %rdi,%rsi
    movq %rdi,%rdx
.Lris64_l:
    cmpb $0,(%rdx)
    je .Lris64_d
    incq %rdx
    jmp .Lris64_l
.Lris64_d:
    subq %rsi,%rdx
    movl $1,%eax
    movl $1,%edi
    syscall
    jmp _flr_read_line

.section .data
.Lflr_ibuf:
    .space 32
    .balign 8
.Lflr_its:
    .space 256
.Lflr_iti:
    .long 0
    .balign 8
.Lflr_hp:
    .quad 0
.Lflr_he:
    .quad 0
.Lflr_sfend:
    .quad 0
.Lflr_eof:
    .long 0
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
    .ascii "-inf\n"
.Lflr_assert_msg:
    .ascii "assertion failed: "
.section .rodata
.Lfl_zero:
    .long 0
    .long 0
