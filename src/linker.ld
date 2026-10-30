ENTRY(loader)
SECTIONS {
    . = 0xC0100000;
    _linker_kernel_virtual_addr_start  = .;
    _linker_kernel_physical_addr_start = . - 0xC0000000;

    .multiboot ALIGN (0x1000) : AT (ADDR (.multiboot) - 0xC0000000) { *(.multiboot) }
    .setup.text ALIGN (0x1000) : AT (ADDR (.setup.text) - 0xC0000000) { *(.setup.text) }
    .text   ALIGN (0x1000) : AT (ADDR (.text)   - 0xC0000000) { *(.text) }
    .rodata ALIGN (0x1000) : AT (ADDR (.rodata) - 0xC0000000) { *(.rodata*) }
    .data   ALIGN (0x1000) : AT (ADDR (.data)   - 0xC0000000) { *(.data) }
    .bss    ALIGN (0x1000) : AT (ADDR (.bss)    - 0xC0000000) {
        *(COMMON)
        *(.bss)
        bin/kernel-entrypoint.o(.bss)
        _linker_kernel_stack_top = .;
    }
    _linker_kernel_virtual_addr_end  = .;
    _linker_kernel_physical_addr_end = . - 0xC0000000;
}
