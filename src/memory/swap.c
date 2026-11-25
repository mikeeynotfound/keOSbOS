#include "header/memory/swap.h"
#include "header/memory/paging.h"
#include "header/filesystem/disk.h"
#include "header/process/process.h"
#include "header/stdlib/string.h"

struct SwapManager swap_manager = {
    .swap_used  = {false},
    .free_count = PAGE_FRAME_ON_SWAP_MAX_COUNT,
};

// Kernel copy of one page: a page is copied here before its directory changes
static uint8_t swap_buffer[PAGE_FRAME_SIZE] __attribute__((aligned(PAGE_FRAME_SIZE)));

// Clock hand for page replacement: (process slot, index in its virtual_addr_used)
static uint32_t clock_process = 0;
static uint32_t clock_page    = 0;

static struct PageTableEntry *swap_get_pte(struct PageDirectory *page_dir, uint32_t virtual_addr) {
    uint32_t pd_index = (virtual_addr >> 22) & 0x3FF;
    if (!page_dir->table[pd_index].flag.present_bit)
        return NULL;
    struct PageTable *page_table = (struct PageTable *) (RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + pd_index * PAGE_FRAME_SIZE);
    return &page_table->table[(virtual_addr >> 12) & 0x3FF];
}

static uint32_t swap_alloc_slot(void) {
    for (uint32_t i = 0; i < PAGE_FRAME_ON_SWAP_MAX_COUNT; i++) {
        if (!swap_manager.swap_used[i]) {
            swap_manager.swap_used[i] = true;
            swap_manager.free_count--;
            return i;
        }
    }
    return PAGE_FRAME_ON_SWAP_MAX_COUNT;
}

void swap_free_slot(uint32_t slot) {
    if (slot < PAGE_FRAME_ON_SWAP_MAX_COUNT && swap_manager.swap_used[slot]) {
        swap_manager.swap_used[slot] = false;
        swap_manager.free_count++;
    }
}

static void swap_disk_write(uint32_t slot, const void *page) {
    for (uint32_t i = 0; i < SWAP_SECTORS_PER_PAGE; i++)
        write_blocks((const uint8_t *) page + i * 512, SWAP_LBA_START + slot * SWAP_SECTORS_PER_PAGE + i, 1);
}

static void swap_disk_read(uint32_t slot, void *page) {
    for (uint32_t i = 0; i < SWAP_SECTORS_PER_PAGE; i++)
        read_blocks((uint8_t *) page + i * 512, SWAP_LBA_START + slot * SWAP_SECTORS_PER_PAGE + i, 1);
}

bool swap_out(struct PageDirectory *page_dir, void *virtual_addr) {
    uint32_t va = (uint32_t) virtual_addr;
    struct PageTableEntry *pte = swap_get_pte(page_dir, va);
    if (pte == NULL || !pte->flag.present_bit)
        return false;

    uint32_t slot = swap_alloc_slot();
    if (slot == PAGE_FRAME_ON_SWAP_MAX_COUNT)
        return false;                                   // swap is full

    // 1. save the page, 2. remember where, 3. mark not present, 4. free the frame
    memcpy(swap_buffer, (void *) va, PAGE_FRAME_SIZE);
    swap_disk_write(slot, swap_buffer);

    uint32_t frame = pte->physical_page_base_address;
    memset(pte, 0, sizeof(struct PageTableEntry));
    pte->ignored                    = PTE_SWAPPED_BIT;
    pte->physical_page_base_address = slot;
    flush_single_tlb(virtual_addr);

    paging_release_frame(frame);
    return true;
}

bool swap_in(struct PageDirectory *page_dir, void *virtual_addr) {
    uint32_t va = (uint32_t) virtual_addr & ~(PAGE_FRAME_SIZE - 1);
    struct PageTableEntry *pte = swap_get_pte(page_dir, va);
    if (pte == NULL || pte->flag.present_bit || !(pte->ignored & PTE_SWAPPED_BIT))
        return false;
    uint32_t slot = pte->physical_page_base_address;

    // 1. get a frame (may swap another page out, that uses swap_buffer, so do it first)
    uint32_t frame = paging_take_free_frame();
    if (frame == PAGE_FRAME_MAX_COUNT)
        return false;

    // 2. read the page from disk, 3. map it again and copy the data in
    swap_disk_read(slot, swap_buffer);
    pte = swap_get_pte(page_dir, va);                   // directory is unchanged, entry is still valid
    pte->ignored                    = 0;
    pte->physical_page_base_address = frame;
    pte->flag = (struct PageTableEntryFlag) {
        .present_bit  = true,
        .write_bit    = true,
        .us_bit       = true,
        .accessed_bit = true,                           // second chance before being evicted again
    };
    flush_single_tlb((void *) va);
    memcpy((void *) va, swap_buffer, PAGE_FRAME_SIZE);
    swap_free_slot(slot);
    return true;
}

bool swap_evict_one(void) {
    struct PageDirectory *original = paging_get_current_page_directory_addr();
    uint32_t total = 0;
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++)
        total += _process_list[i].memory.page_frame_used_count;

    // Clock: a page with the accessed bit gets a second chance (bit cleared),
    // the first page without it is swapped out. Two full rounds are enough.
    bool evicted = false;
    for (uint32_t step = 0; step <= 2 * total && !evicted; step++) {
        if (clock_process >= PROCESS_COUNT_MAX)
            clock_process = 0;
        struct ProcessControlBlock *pcb = &_process_list[clock_process];

        // Only READY / RUNNING processes. A process being created is still INACTIVE,
        // so its pages are never taken while the kernel is filling them.
        if (pcb->metadata.state == PROCESS_STATE_INACTIVE || clock_page >= pcb->memory.page_frame_used_count) {
            clock_process++;
            clock_page = 0;
            continue;
        }

        uint32_t va = (uint32_t) pcb->memory.virtual_addr_used[clock_page++];
        struct PageDirectory *page_dir = pcb->context.page_directory_virtual_addr;
        if (page_dir != paging_get_current_page_directory_addr())
            paging_use_page_directory(page_dir);         // recursive mapping needs it active

        struct PageTableEntry *pte = swap_get_pte(page_dir, va);
        if (pte != NULL && pte->flag.present_bit) {
            if (pte->flag.accessed_bit) {
                pte->flag.accessed_bit = false;
                flush_single_tlb((void *) va);
            } else {
                evicted = swap_out(page_dir, (void *) va);
            }
        }
    }

    if (paging_get_current_page_directory_addr() != original)
        paging_use_page_directory(original);
    return evicted;
}

bool handle_page_fault(uint32_t error_code) {
    uint32_t address = read_cr2();
    if (error_code & 0x1)                                // page was present: protection violation
        return false;
    if (address >= KERNEL_VIRTUAL_ADDRESS_BASE)
        return false;
    return swap_in(paging_get_current_page_directory_addr(), (void *) address);
}
