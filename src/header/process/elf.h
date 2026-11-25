#ifndef _ELF_H
#define _ELF_H

#include <stdint.h>

/**
 * Minimal ELF32 definitions used by the kernel loader (bonus: separate shell commands).
 * Only what is needed to load a static i386 executable is defined here.
 * Reference: System V ABI, "ELF Header" and "Program Header" chapters.
 */

#define ELF_MAGIC0         0x7F
#define ELF_MAGIC1         'E'
#define ELF_MAGIC2         'L'
#define ELF_MAGIC3         'F'

#define ELF_CLASS_32       1     // e_ident[EI_CLASS]
#define ELF_DATA_LSB       1     // e_ident[EI_DATA], little endian
#define ELF_TYPE_EXEC      2     // e_type, executable file
#define ELF_MACHINE_386    3     // e_machine, Intel 80386
#define ELF_PT_LOAD        1     // p_type, loadable segment

#define ELF_IDENT_SIZE     16
#define ELF_IDENT_CLASS    4
#define ELF_IDENT_DATA     5

struct ELF32Header {
    uint8_t  e_ident[ELF_IDENT_SIZE];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;      // virtual address of _start
    uint32_t e_phoff;      // file offset of the program header table
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;  // size of one program header
    uint16_t e_phnum;      // number of program headers
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct ELF32ProgramHeader {
    uint32_t p_type;       // ELF_PT_LOAD for segments that must be loaded to memory
    uint32_t p_offset;     // file offset of the segment data
    uint32_t p_vaddr;      // virtual address of the segment
    uint32_t p_paddr;
    uint32_t p_filesz;     // bytes copied from the file
    uint32_t p_memsz;      // bytes in memory, the rest (p_memsz - p_filesz) is zero filled (.bss)
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

_Static_assert(sizeof(struct ELF32Header) == 52, "ELF32 header must be 52 bytes");
_Static_assert(sizeof(struct ELF32ProgramHeader) == 32, "ELF32 program header must be 32 bytes");

#endif
