; Minimal startup routine for separated shell commands (bonus, book 4.3.6).
; Based on crt0-echo.s from the book, adapted to this kernel:
; - argv is passed to main as a pointer (main(int argc, char **argv))
; - exit uses SYSCALL_EXIT (12) from header/syscall-number.h
; - cli / hlt are privileged in ring 3, so the unreachable tail just loops
global _start
extern main

SYSCALL_EXIT equ 12

section .text
_start:
    ; Kernel stack setup (process_setup_user_stack in process.c):
    ; ESP -> argc
    ; ESP+4 -> argv[0]
    ; ESP+8 -> argv[1]
    ; ...
    ; ESP+4*(argc+1) -> NULL

    ; Base pointer
    mov ebp, esp

    mov eax, [esp]          ; argc
    lea ebx, [esp + 4]      ; argv, the pointer array starts right after argc

    ; Clear the remaining general purpose registers
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi

    ; Call main(argc, argv)
    push ebx
    push eax
    call main

    ; Exit program (terminate process), ebx = exit code
    mov ebx, eax
    mov eax, SYSCALL_EXIT
    int 0x30

    ; Is it possible to reach here? The kernel never resumes an exited process.
.hang:
    jmp .hang

; Mark the stack as non executable (silences the ld warning)
section .note.GNU-stack noalloc noexec nowrite progbits
