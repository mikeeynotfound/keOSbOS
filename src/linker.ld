ENTRY(loader)

SECTIONS {
    . = 0x00100000;

    .multiboot ALIGN (0x1000) :
    {
        *(.multiboot)
    }

    .text ALIGN (0x1000) :
    {
        *(.text)
    }

    .rodata ALIGN (0x1000) :
    {
        *(.rodata*)
    }

    .data ALIGN (0x1000) :
    {
        *(.data)
    }

    .bss ALIGN (0x1000) :
    {
        *(COMMON)
        *(.bss)
    }
}