global process_context_switch

; Offsets in struct Context (header/process/process.h)
CTX_EDI     equ 0
CTX_ESI     equ 4
CTX_EBP     equ 8
CTX_ESP     equ 12
CTX_EBX     equ 16
CTX_EDX     equ 20
CTX_ECX     equ 24
CTX_EAX     equ 28
CTX_GS      equ 32
CTX_FS      equ 36
CTX_ES      equ 40
CTX_DS      equ 44
CTX_EIP     equ 48
CTX_EFLAGS  equ 52
CTX_PAGEDIR equ 56

USER_CODE_SELECTOR equ 0x18 | 0x3
USER_DATA_SELECTOR equ 0x20 | 0x3
KERNEL_VIRTUAL_BASE equ 0xC0000000

section .text

; void process_context_switch(struct Context ctx)
; ctx is passed by value on the stack, so [esp+4] is its first byte.
process_context_switch:
    lea ecx, [esp+4]

    ; Load the target address space. The page directory address is a kernel
    ; virtual address, CR3 needs the physical one.
    mov eax, [ecx + CTX_PAGEDIR]
    sub eax, KERNEL_VIRTUAL_BASE
    mov cr3, eax

    ; Build the inter-privilege iret frame for ring 3.
    push dword USER_DATA_SELECTOR
    push dword [ecx + CTX_ESP]
    push dword [ecx + CTX_EFLAGS]
    push dword USER_CODE_SELECTOR
    push dword [ecx + CTX_EIP]

    mov ax, USER_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; ecx still points at ctx, so restore it last.
    mov edi, [ecx + CTX_EDI]
    mov esi, [ecx + CTX_ESI]
    mov ebp, [ecx + CTX_EBP]
    mov ebx, [ecx + CTX_EBX]
    mov edx, [ecx + CTX_EDX]
    mov eax, [ecx + CTX_EAX]
    mov ecx, [ecx + CTX_ECX]
    iret
