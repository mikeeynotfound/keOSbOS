#include "header/filesystem/disk.h"
#include "header/cpu/portio.h"

static void ctl_wait(void) {
    while ((in(0x1F7) & ATA_STATUS_BSY) || !(in(0x1F7) & ATA_STATUS_RDY));
}

void read_blocks(void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    ctl_wait();

    out(0x1F6, 0xE0 | ((logical_block_address >> 24) & 0x0F));
    out(0x1F2, block_count);
    out(0x1F3, (uint8_t) logical_block_address);
    out(0x1F4, (uint8_t) (logical_block_address >> 8));
    out(0x1F5, (uint8_t) (logical_block_address >> 16));
    out(0x1F7, 0x20);

    uint16_t *target = (uint16_t *) ptr;
    for (uint8_t i = 0; i < block_count; i++) {
        ctl_wait();
        for (uint32_t j = 0; j < HALF_BLOCK_SIZE; j++) {
            *target = in(0x1F0);
            target++;
        }
    }
}

void write_blocks(const void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    ctl_wait();

    out(0x1F6, 0xE0 | ((logical_block_address >> 24) & 0x0F));
    out(0x1F2, block_count);
    out(0x1F3, (uint8_t) logical_block_address);
    out(0x1F4, (uint8_t) (logical_block_address >> 8));
    out(0x1F5, (uint8_t) (logical_block_address >> 16));
    out(0x1F7, 0x30);

    const uint16_t *source = (const uint16_t *) ptr;
    for (uint8_t i = 0; i < block_count; i++) {
        ctl_wait();
        for (uint32_t j = 0; j < HALF_BLOCK_SIZE; j++) {
            out(0x1F0, *source);
            source++;
        }
    }
}
