global loader       ; the entry symbol for ELF
global load_gdt     ; load GDT table
extern kernel_setup ; kernel

KERNEL_STACK_SIZE equ 4096           ; size of stack in bytes
MAGIC_NUMBER      equ 0x1BADB002     ; define the magic number constant
FLAGS             equ 0x0            ; multiboot flags
CHECKSUM          equ -MAGIC_NUMBER  ; calculate the checksum
                                     ; (magic number + checksum + flags should equal 0)

section .bss
align 4                              ; align at 4 bytes
kernel_stack:                        ; label points to beginning of memory
    resb KERNEL_STACK_SIZE           ; reserve stack for the kernel

section .multiboot                   ; GNU GRUB Multiboot header
align 4                              ; the code must be 4 byte aligned
    dd MAGIC_NUMBER                  ; write the magic number to the machine code,
    dd FLAGS                         ; the flags,
    dd CHECKSUM                      ; and the checksum


section .text                                  ; start of the text (code) 
loader:                                        ; the loader label (defined as entry point in linker script)
    mov  esp, kernel_stack + KERNEL_STACK_SIZE ; setup stack register to proper location
    call kernel_setup
.loop:
    jmp .loop                                  ; loop forever


; More details: https://en.wikibooks.org/wiki/X86_Assembly/Protected_Mode

load_gdt:
    push ebp
    mov  ebp, esp

    mov  eax, [ebp+8]      ; ambil pointer ke GDTR (parameter pertama)
    lgdt [eax]              ; 1. load GDT

    mov  eax, cr0
    or   eax, 0x1
    mov  cr0, eax           ; 2. set Protected Mode bit di CR0

    jmp  0x08:.flush_cs     ; 3. far jump ke kernel code segment

.flush_cs:
    mov  ax, 0x10           ; 4. sesuaikan data segment register ke kernel data
    mov  ds, ax
    mov  es, ax
    mov  ss, ax

    mov  esp, ebp
    pop  ebp
    ret
