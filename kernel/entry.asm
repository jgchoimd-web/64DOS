; 64DOS long-mode kernel entry.
; Released into the public domain with the rest of 64DOS.

bits 64

global _start
extern kmain
extern __bss_start
extern __bss_end

section .text
_start:
    cld
    mov rsp, stack_top
    xor rbp, rbp
    mov rbx, rdi

    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    xor eax, eax
    rep stosb

    mov rdi, rbx
    call kmain

.halt:
    cli
    hlt
    jmp .halt

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
