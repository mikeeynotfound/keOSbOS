#ifndef _SWAP_H
#define _SWAP_H

#include <stdint.h>
#include <stdbool.h>
#include "header/memory/paging.h"

/**
 * Swap space (Book 3.2): user pages can be moved to disk when physical
 * frames run out, and are brought back by the page fault handler.
 *
 * Layout: the swap area lives on the same disk, right after the 4 MiB EXT2
 * area (storage.bin is 8 MiB). Slot i is 4 KiB (8 sectors) at
 * SWAP_LBA_START + i * SWAP_SECTORS_PER_PAGE.
 *
 * A swapped page keeps its page table entry with present_bit = 0, the
 * PTE_SWAPPED_BIT set in the "ignored" (available to OS) bits, and the
 * swap slot number in physical_page_base_address.
 */
#define PAGE_FRAME_ON_SWAP_MAX_COUNT 1024      // 4 MiB of swap
#define SWAP_LBA_START               (4194304u / 512u)
#define SWAP_SECTORS_PER_PAGE        (PAGE_FRAME_SIZE / 512u)
#define PTE_SWAPPED_BIT              0x1       // bit 9 of the PTE (first "ignored" bit)

struct SwapManager {
    bool     swap_used[PAGE_FRAME_ON_SWAP_MAX_COUNT];
    uint32_t free_count;
} __attribute__((packed));

extern struct SwapManager swap_manager;

/**
 * Read a swapped page back from disk into a free frame and map it again.
 * page_dir must be the active page directory.
 *
 * @param page_dir     Page directory to read
 * @param virtual_addr Virtual address that caused the page fault
 * @return             True if the page was swapped in
 */
bool swap_in(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * Write a present user page to a free swap slot and release its frame.
 * page_dir must be the active page directory.
 *
 * @param page_dir     Page directory to read
 * @param virtual_addr Page aligned virtual address to swap out
 * @return             True if the page is now on swap
 */
bool swap_out(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * Page replacement: choose a resident user page of an active process with
 * the clock (second chance) algorithm and swap it out, freeing one frame.
 *
 * @return True if a frame was freed
 */
bool swap_evict_one(void);

/**
 * Release the swap slot of a page that is no longer needed (process exit).
 */
void swap_free_slot(uint32_t slot);

// Read page fault address from CR2
static inline uint32_t read_cr2(void) {
    uint32_t fault_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
    return fault_address;
}

/**
 * Handle a page fault (interrupt 14). Swaps the page in when it is on swap.
 *
 * @param error_code Error code pushed by the CPU
 * @return           True if handled and the faulting instruction can be retried,
 *                   false for a real fault (unmapped address or protection violation)
 */
bool handle_page_fault(uint32_t error_code);

#endif
