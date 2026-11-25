/*
 * Linker script for separated shell commands (bonus, book 4.3.6).
 * Same as the book's linker.ld, output is an ELF32 executable loaded by the kernel ELF loader.
 * Modified: .rodata* and COMMON are included so string literals and common
 * symbols land in the expected sections instead of orphan sections.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x0;

    .text : ALIGN(4K)
    {
        *(.text)
        *(.rodata*)
    }

    .data : ALIGN(4K)
    {
        *(.data)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
