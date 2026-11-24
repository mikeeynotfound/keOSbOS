#include "header/cpu/gdt.h"
#include "header/cpu/interrupt.h"

/**
 * global_descriptor_table, predefined GDT.
 * Table entry : [{Null}, {Kernel Code}, {Kernel Data}, {User Code},
 *                {User Data}, {TSS}].
 * The order is significant because the selectors in gdt.h are derived from it.
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
         .base_high = 0},
        {// User Code Segment
         .segment_low = 0xFFFF,
         .base_low = 0,
         .base_mid = 0,
         .type_bit = 0xA, // 0b1010: Code, Readable, Not Conforming, Not Accessed
         .non_system = 1,
         .dpl = 3, // Ring 3
         .p = 1,
         .segment_high = 0xF,
         .avl = 0,
         .long_mode = 0,
         .db = 1,
         .granularity = 1,
         .base_high = 0},
        {// User Data Segment
         .segment_low = 0xFFFF,
         .base_low = 0,
         .base_mid = 0,
         .type_bit = 0x2, // 0b0010: Data, Writable, Direction Up, Not Accessed
         .non_system = 1,
         .dpl = 3, // Ring 3
         .p = 1,
         .segment_high = 0xF,
         .avl = 0,
         .long_mode = 0,
         .db = 1,
         .granularity = 1,
         .base_high = 0},
        {// Task State Segment (32-bit available TSS)
         .segment_low = sizeof(struct TSSEntry) - 1,
         .base_low = 0, // Filled by gdt_install_tss()
         .base_mid = 0, // Filled by gdt_install_tss()
         .type_bit = 0x9, // 0b1001: 32-bit available TSS
         .non_system = 0, // System segment
         .dpl = 0,
         .p = 1,
         .segment_high = ((sizeof(struct TSSEntry) - 1) >> 16) & 0xF,
         .avl = 0,
         .long_mode = 0,
         .db = 0,          // Reserved for system descriptors
         .granularity = 0, // TSS limit is a byte limit
         .base_high = 0    // Filled by gdt_install_tss()
        }}};

/**
 * gdt_install_tss, fill the TSS descriptor with the runtime address of the
 * single kernel TSS. The GDT itself is already loaded by load_gdt(), so only
 * the descriptor contents need to be updated before loading the task register.
 */
void gdt_install_tss(void) {
    uint32_t base = (uint32_t) &_interrupt_tss_entry;
    uint32_t limit = sizeof(struct TSSEntry) - 1;
    struct SegmentDescriptor *tss_descriptor =
        &global_descriptor_table.table[GDT_TSS_SELECTOR >> 3];

    tss_descriptor->segment_low = limit & 0xFFFF;
    tss_descriptor->segment_high = (limit >> 16) & 0xF;
    tss_descriptor->base_low = base & 0xFFFF;
    tss_descriptor->base_mid = (base >> 16) & 0xFF;
    tss_descriptor->base_high = (base >> 24) & 0xFF;
}

/**
 * _gdt_gdtr, predefined system GDTR.
 */
struct GDTR _gdt_gdtr = {
    .size = sizeof(global_descriptor_table) - 1,
    .address = &global_descriptor_table};