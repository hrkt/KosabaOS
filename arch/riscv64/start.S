# Minimal S-mode kernel: prints one line via legacy SBI console_putchar (EID 1),
# then parks in wfi. Runs under OpenSBI on QEMU virt, loaded at 0x80200000.
# Uses PC-relative addressing only (no GOT), so it works as a raw binary.
    .section .text
    .globl _start
_start:
.Lsp: auipc sp, %pcrel_hi(stack_top)
    addi sp, sp, %pcrel_lo(.Lsp)
.Lmsg: auipc s1, %pcrel_hi(msg)
    addi s1, s1, %pcrel_lo(.Lmsg)
1:  lbu a0, 0(s1)
    beqz a0, 2f
    li a7, 1          # SBI legacy: sbi_console_putchar, char in a0
    ecall
    addi s1, s1, 1
    j 1b
2:  wfi
    j 2b

    .section .rodata
msg:
    .string "hello from KosabaOS (S-mode)\n"

    .section .bss
    .align 4
stack:
    .space 4096
stack_top:
