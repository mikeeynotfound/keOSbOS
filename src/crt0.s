global _start
extern main

SYSCALL_EXIT equ 12

section .text
_start:
    call main

    ; main returned, terminate this process and let the scheduler pick another
    mov eax, SYSCALL_EXIT
    int 0x30
    jmp $
