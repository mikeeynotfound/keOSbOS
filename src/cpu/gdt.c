#include "header/cpu/gdt.h"

/**
 * global_descriptor_table, predefined GDT.
 * Table entry : [{Null Descriptor}, {Kernel Code}, {Kernel Data (variable, etc)}].
 */
struct GlobalDescriptorTable global_descriptor_table = {
    .table = {
        {// Null Descriptor - semua 0
         .segment_low = 0,
         .base_low = 0,
         .base_mid = 0,
         .type_bit = 0,
         .non_system = 0,
         .dpl = 0,
         .p = 0,
         .segment_high = 0,
         .avl = 0,
         .long_mode = 0,
         .db = 0,
         .granularity = 0,
         .base_high = 0},
        {// Kernel Code Segment
         .segment_low = 0xFFFF,
         .base_low = 0,
         .base_mid = 0,
         .type_bit = 0xA, // 0b1010: Code, Readable, Not Conforming, Not Accessed
         .non_system = 1, // Code/Data segment
         .dpl = 0,        // Kernel
         .p = 1,          // Present
         .segment_high = 0xF,
         .avl = 0,
         .long_mode = 0,
         .db = 1,          // 32-bit segment
         .granularity = 1, // 4 KB granularity
         .base_high = 0},
        {// Kernel Data Segment
         .segment_low = 0xFFFF,
         .base_low = 0,
         .base_mid = 0,
         .type_bit = 0x2, // 0b0010: Data, Writable, Direction Up, Not Accessed
         .non_system = 1,
         .dpl = 0,
         .p = 1,
         .segment_high = 0xF,
         .avl = 0,
         .long_mode = 0,
         .db = 1,
         .granularity = 1,
         .base_high = 0}}};

/**
 * _gdt_gdtr, predefined system GDTR.
 */
struct GDTR _gdt_gdtr = {
    .size = sizeof(global_descriptor_table) - 1,
    .address = &global_descriptor_table};