.section .text

.globl print_str
print_str:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call str_len
    addl $4,%esp
    pushl %eax
    movl 8(%ebp),%eax
    pushl %eax
    movl $1,%eax
    pushl %eax
    movl $4,%eax
    pushl %eax
    popl %eax
    popl %ebx
    popl %ecx
    popl %edx
    xorl %esi,%esi
    xorl %edi,%edi
    int $0x80
    popl %ebx
    popl %edi
    popl %esi
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl print_int
print_int:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call int_to_str
    addl $4,%esp
    pushl %eax
    call str_len
    addl $4,%esp
    pushl %eax
    movl 8(%ebp),%eax
    pushl %eax
    call int_to_str
    addl $4,%esp
    pushl %eax
    movl $1,%eax
    pushl %eax
    movl $4,%eax
    pushl %eax
    popl %eax
    popl %ebx
    popl %ecx
    popl %edx
    xorl %esi,%esi
    xorl %edi,%edi
    int $0x80
    popl %ebx
    popl %edi
    popl %esi
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl str_len
str_len:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_str_len
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl str_eq
str_eq:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 12(%ebp),%eax
    pushl %eax
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_str_eq
    addl $8,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl int_to_str
int_to_str:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_int_to_str
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl str_to_int
str_to_int:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_str_to_int
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl abs
abs:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_abs
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl min
min:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 12(%ebp),%eax
    pushl %eax
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_min
    addl $8,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl max
max:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 12(%ebp),%eax
    pushl %eax
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_max
    addl $8,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl alloc
alloc:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_alloc
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl free
free:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_free
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl exit
exit:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_exit
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl assert
assert:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    movl 12(%ebp),%eax
    pushl %eax
    movl 8(%ebp),%eax
    pushl %eax
    call _flr_assert
    addl $8,%esp
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.globl main
main:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    leal .Lstr0,%eax
    pushl %eax
    call _flr_print_str
    addl $4,%esp
    popl %ebx
    popl %edi
    popl %esi
    xorl %ebx,%ebx
    movl $1,%eax
    int $0x80

# ── Falcon Runtime (inline) ───────────────────────────
_flr_memset:
    pushl %ebp
    movl %esp,%ebp
    pushl %edi
    movl 8(%ebp),%edi
    movl 12(%ebp),%eax
    movl 16(%ebp),%ecx
    rep stosb
    popl %edi
    leave
    ret

_flr_memcpy:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    movl 8(%ebp),%edi
    movl 12(%ebp),%esi
    movl 16(%ebp),%ecx
    rep movsb
    popl %edi
    popl %esi
    leave
    ret

_flr_print_str:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    movl 8(%ebp),%esi
    movl %esi,%ecx
.Lfps_l:
    cmpb $0,(%ecx)
    je .Lfps_d
    incl %ecx
    jmp .Lfps_l
.Lfps_d:
    subl %esi,%ecx
    movl %ecx,%edx
    movl %esi,%ecx
    movl $4,%eax
    movl $1,%ebx
    int $0x80
    movl $4,%eax
    movl $1,%ebx
    leal .Lflr_nl,%ecx
    movl $1,%edx
    int $0x80
    popl %esi
    leave
    ret

_flr_print_int:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    movl 8(%ebp),%eax
    leal .Lflr_ibuf+11,%edi
    movb $10,(%edi)
    decl %edi
    testl %eax,%eax
    jge .Lfpi_pos
    negl %eax
    movl $1,%esi
    jmp .Lfpi_l
.Lfpi_pos:
    xorl %esi,%esi
.Lfpi_l:
    movl $10,%ecx
    xorl %edx,%edx
    divl %ecx
    addb $48,%dl
    movb %dl,(%edi)
    decl %edi
    testl %eax,%eax
    jne .Lfpi_l
    testl %esi,%esi
    je .Lfpi_nom
    movb $45,(%edi)
    decl %edi
.Lfpi_nom:
    incl %edi
    leal .Lflr_ibuf+12,%edx
    subl %edi,%edx
    movl %edi,%ecx
    movl $4,%eax
    movl $1,%ebx
    int $0x80
    popl %edi
    popl %esi
    leave
    ret

_flr_exit:
    movl 4(%esp),%ebx
    movl $1,%eax
    int $0x80

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

_flr_print_double:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    pushl %ebx
    fldl 8(%ebp)
    fxam
    fnstsw %ax
    testw $0x0200,%ax
    je .Lfpd_pos
    fchs
    movl $4,%eax
    movl $1,%ebx
    leal .Lflr_minus,%ecx
    movl $1,%edx
    int $0x80
.Lfpd_pos:
    fld %st(0)
    subl $4,%esp
    fistpl (%esp)
    popl %eax
    pushl %eax
    call _flr_print_int
    addl $4,%esp
    movl $4,%eax
    movl $1,%ebx
    leal .Lflr_dot,%ecx
    movl $1,%edx
    int $0x80
    fld %st(0)
    subl $4,%esp
    fistpl (%esp)
    filds (%esp)
    addl $4,%esp
    fsubrp
    fldl .Lflr_1e6
    fmulp
    subl $4,%esp
    fistpl (%esp)
    popl %eax
    testl %eax,%eax
    jge .Lfpd_fpos
    negl %eax
.Lfpd_fpos:
    leal .Lflr_ibuf+12,%edi
    movb $10,(%edi)
    movl $6,%ecx
    decl %edi
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
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

_flr_strlen:
    movl 4(%esp),%ecx
    movl %ecx,%eax
.Lflrsl:
    cmpb $0,(%ecx)
    je .Lflrsld
    incl %ecx
    jmp .Lflrsl
.Lflrsld:
    subl 4(%esp),%ecx
    movl %ecx,%eax
    ret

_flr_str_len:
    jmp _flr_strlen

_flr_str_eq:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    movl 8(%ebp),%esi
    movl 12(%ebp),%edi
.Lfseq:
    movzbl (%esi),%eax
    movzbl (%edi),%ecx
    cmpl %ecx,%eax
    jne .Lfseq_no
    testl %eax,%eax
    je .Lfseq_yes
    incl %esi
    incl %edi
    jmp .Lfseq
.Lfseq_yes:
    movl $1,%eax
    jmp .Lfseq_ret
.Lfseq_no:
    xorl %eax,%eax
.Lfseq_ret:
    popl %edi
    popl %esi
    leave
    ret

_flr_int_to_str:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    pushl %edi
    movl 8(%ebp),%eax
    leal .Lflr_ibuf+11,%edi
    movb $0,(%edi)
    decl %edi
    testl %eax,%eax
    jge .Lfits_p
    negl %eax
    movl $1,%esi
    jmp .Lfits_l
.Lfits_p:
    xorl %esi,%esi
.Lfits_l:
    movl $10,%ecx
    xorl %edx,%edx
    divl %ecx
    addb $48,%dl
    movb %dl,(%edi)
    decl %edi
    testl %eax,%eax
    jne .Lfits_l
    testl %esi,%esi
    je .Lfits_n
    movb $45,(%edi)
    decl %edi
.Lfits_n:
    incl %edi
    movl %edi,%eax
    popl %edi
    popl %esi
    leave
    ret

_flr_str_to_int:
    pushl %ebp
    movl %esp,%ebp
    pushl %esi
    movl 8(%ebp),%esi
    xorl %eax,%eax
    xorl %ecx,%ecx
    cmpb $45,(%esi)
    jne .Lfsti_l
    movl $1,%ecx
    incl %esi
.Lfsti_l:
    movzbl (%esi),%edx
    cmpl $48,%edx
    jl .Lfsti_d
    cmpl $57,%edx
    jg .Lfsti_d
    imull $10,%eax
    subl $48,%edx
    addl %edx,%eax
    incl %esi
    jmp .Lfsti_l
.Lfsti_d:
    testl %ecx,%ecx
    je .Lfsti_r
    negl %eax
.Lfsti_r:
    popl %esi
    leave
    ret

_flr_abs:
    movl 4(%esp),%eax
    testl %eax,%eax
    jge .Labs_ok
    negl %eax
.Labs_ok:
    ret

_flr_min:
    movl 4(%esp),%eax
    movl 8(%esp),%ecx
    cmpl %ecx,%eax
    jle .Lmin_ok
    movl %ecx,%eax
.Lmin_ok:
    ret

_flr_max:
    movl 4(%esp),%eax
    movl 8(%esp),%ecx
    cmpl %ecx,%eax
    jge .Lmax_ok
    movl %ecx,%eax
.Lmax_ok:
    ret

_flr_assert:
    pushl %ebp
    movl %esp,%ebp
    movl 8(%ebp),%eax
    testl %eax,%eax
    jne .Lassert_ok
    movl $4,%eax
    movl $2,%ebx
    leal .Lflr_assert_msg,%ecx
    movl $19,%edx
    int $0x80
    movl 12(%ebp),%esi
    movl %esi,%ecx
.Lasssl:
    cmpb $0,(%ecx)
    je .Lasssd
    incl %ecx
    jmp .Lasssl
.Lasssd:
    subl %esi,%ecx
    movl %ecx,%edx
    movl %esi,%ecx
    movl $4,%eax
    movl $2,%ebx
    int $0x80
    movl $4,%eax
    movl $2,%ebx
    leal .Lflr_nl,%ecx
    movl $1,%edx
    int $0x80
    movl $1,%ebx
    movl $1,%eax
    int $0x80
.Lassert_ok:
    leave
    ret

_flr_alloc:
    pushl %ebp
    movl %esp,%ebp
    pushl %ebx
    movl $45,%eax
    xorl %ebx,%ebx
    int $0x80
    movl %eax,%ecx
    movl 8(%ebp),%edx
    addl $3,%edx
    andl $-4,%edx
    movl $45,%eax
    leal (%ecx,%edx),%ebx
    int $0x80
    movl %ecx,%eax
    popl %ebx
    leave
    ret

_flr_free:
    ret

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
    jmp .Lsfmt_copy
.Lsfmt_d:
    incl %esi
    testl %ecx,%ecx
    je .Lsfmt_l
    pushl %ecx
    pushl %esi
    pushl %edi
    pushl %ebx
    movl (%ebx),%eax
    addl $4,%ebx
    decl %ecx
    pushl %eax
    call _flr_int_to_str
    addl $4,%esp
    movl %eax,%esi
.Lsfmt_dc:
    movzbl (%esi),%eax
    testl %eax,%eax
    je .Lsfmt_dr
    movb %al,(%edi)
    incl %esi
    incl %edi
    jmp .Lsfmt_dc
.Lsfmt_dr:
    popl %ebx
    popl %edi
    popl %esi
    popl %ecx
    jmp .Lsfmt_l
.Lsfmt_s:
    incl %esi
    testl %ecx,%ecx
    je .Lsfmt_l
    pushl %ecx
    pushl %esi
    pushl %edi
    pushl %ebx
    movl (%ebx),%esi
    addl $4,%ebx
    decl %ecx
.Lsfmt_sc:
    movzbl (%esi),%eax
    testl %eax,%eax
    je .Lsfmt_sr
    movb %al,(%edi)
    incl %esi
    incl %edi
    jmp .Lsfmt_sc
.Lsfmt_sr:
    popl %ebx
    popl %edi
    popl %esi
    popl %ecx
    jmp .Lsfmt_l
.Lsfmt_copy:
    movb %al,(%edi)
    incl %esi
    incl %edi
    jmp .Lsfmt_l
.Lsfmt_end:
    movb $0,(%edi)
    popl %eax
    popl %ebx
    popl %edi
    popl %esi
    leave
    ret

.section .data
.Lflr_ibuf:
    .space 32
.Lflr_nl:
    .byte 10
.Lflr_dot:
    .ascii "."
.Lflr_minus:
    .ascii "-"
.Lflr_1e6:
    .long 0
    .long 1093567616
.Lflr_assert_msg:
    .ascii "assertion failed: "
.section .rodata
.Lstr0:
    .ascii "Hello, World!\0"
.Lfl_zero:
    .long 0
    .long 0
