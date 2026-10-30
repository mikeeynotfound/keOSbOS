#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/memory/paging.h"
#include "header/stdlib/string.h"

/**
 * _paging_kernel_page_directory and _paging_kernel_page_table is initialized on kernel-entrypoint.s
 */
__attribute__((aligned(0x1000))) struct PageDirectory _paging_kernel_page_directory;

__attribute__((aligned(0x1000))) struct PageTable _paging_kernel_page_table;

/**
 * Global page frame manager state tracking free/busy frames.
 */
struct PageManagerState page_manager_state = {
    .page_frame_map = {
        [0 ... KERNEL_RESERVED_PAGE_FRAME_COUNT - 1] = true,
        [KERNEL_RESERVED_PAGE_FRAME_COUNT ... PAGE_FRAME_MAX_COUNT - 1] = false
    },
    .free_page_frame_count = PAGE_FRAME_MAX_COUNT - KERNEL_RESERVED_PAGE_FRAME_COUNT,
};

struct PageTable *paging_get_page_table(struct PageDirectory *page_dir, uint32_t page_directory_index) {
    //this helper function is given from kit but you can always modified it if you think theres a bug (spoiler alert) or you think theres a better approach

    if (page_directory_index == RECURSIVE_PAGE_DIRECTORY_INDEX || page_dir != paging_get_current_page_directory_addr()) return NULL;

    struct PageDirectoryEntry *dir_entry = &page_dir->table[page_directory_index];

    if (dir_entry->flag.present_bit) {
        return (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
    }
    for (uint32_t page_frame_number = KERNEL_RESERVED_PAGE_FRAME_COUNT;
         page_frame_number < PAGE_FRAME_MAX_COUNT;
         page_frame_number++) {
        if (!page_manager_state.page_frame_map[page_frame_number]) {
            page_manager_state.page_frame_map[page_frame_number] = true;
            page_manager_state.free_page_frame_count--;
            dir_entry->flag = (struct PageDirectoryEntryFlag) {
                .present_bit = true,
                .write_bit = true,
                .us_bit = true,
            };
            dir_entry->page_table_base_address = page_frame_number;
            struct PageTable *page_table = (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
            flush_single_tlb(page_table);
            memset(page_table, 0, PAGE_FRAME_SIZE);
            return page_table;
        }
    }
    return NULL;
}

void update_page_directory_entry(
    struct PageDirectory *page_dir,
    void *physical_addr, 
    void *virtual_addr, 
    struct PageTableEntryFlag flag
) {
    uint32_t page_directory_index = ((uint32_t)virtual_addr >> 22) & 0x3FF;
    uint32_t page_table_index = ((uint32_t)virtual_addr >> 12) & 0x3FF;
    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL) return;
    page_table->table[page_table_index].flag = flag;
    page_table->table[page_table_index].physical_page_base_address = ((uint32_t) physical_addr >> 12);
    flush_single_tlb(virtual_addr);
}

void flush_single_tlb(void *virtual_addr) {
    asm volatile("invlpg (%0)" : /* <Empty> */ : "b"(virtual_addr): "memory");
}

struct PageDirectory* paging_get_current_page_directory_addr(void) {
    uint32_t current_page_directory_phys_addr;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_page_directory_phys_addr));
    return (struct PageDirectory*) (current_page_directory_phys_addr + KERNEL_VIRTUAL_ADDRESS_BASE);
}



/* --- Memory Management --- */
// TODO: Implement
bool paging_allocate_check(uint32_t amount) {
    // TODO: Check whether requested amount is available
    uint32_t required_frames = (amount + PAGE_FRAME_SIZE - 1) / PAGE_FRAME_SIZE;
    return required_frames <= page_manager_state.free_page_frame_count;
}


bool paging_allocate_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    /**
     * TODO: Find free physical frame and map virtual frame into it
     * - Find free physical frame in page_manager_state.page_frame_map[] using any strategies
     * - Mark page_manager_state.page_frame_map[]
     * - Update page directory with user flags:
     *     > present bit    true
     *     > write bit      true
     *     > user bit       true
     *     > pagesize 4 mb  true
     */ 
    uint32_t va = (uint32_t) virtual_addr;

    // User space only (below kernel higher half) and must be page aligned
    if (va >= KERNEL_VIRTUAL_ADDRESS_BASE || (va & (PAGE_FRAME_SIZE - 1))) return false;

    uint32_t page_directory_index = (va >> 22) & 0x3FF;
    uint32_t page_table_index = (va >> 12) & 0x3FF;

    // Get page table (will be created if not exist yet)
    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL) return false;

    // Virtual address is already mapped
    if (page_table->table[page_table_index].flag.present_bit) return false;

    // First-fit search for free physical frame
    for (uint32_t frame = KERNEL_RESERVED_PAGE_FRAME_COUNT; frame < PAGE_FRAME_MAX_COUNT; frame++) {
        if (page_manager_state.page_frame_map[frame]) continue;

        page_manager_state.page_frame_map[frame] = true;
        page_manager_state.free_page_frame_count--;

        struct PageTableEntryFlag flag = {
            .present_bit = true,
            .write_bit   = true,
            .us_bit      = true,
        };
        update_page_directory_entry(page_dir, (void *) (frame * PAGE_FRAME_SIZE), virtual_addr, flag);
        return true;
    }
    return false;
}

bool paging_free_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    /* 
     * TODO: Deallocate a physical frame from respective virtual address
     * - Use the page_dir.table values to check mapped physical frame
     * - Remove the entry by setting it into 0
     */
    uint32_t va = (uint32_t) virtual_addr;

    // User space only (below kernel higher half) and must be page aligned
    if (va >= KERNEL_VIRTUAL_ADDRESS_BASE || (va & (PAGE_FRAME_SIZE - 1))) return false;

    uint32_t page_directory_index = (va >> 22) & 0x3FF;
    uint32_t page_table_index = (va >> 12) & 0x3FF;

    // Page table not exist, nothing to free
    if (!page_dir->table[page_directory_index].flag.present_bit) return false;

    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL || !page_table->table[page_table_index].flag.present_bit) return false;

    uint32_t frame = page_table->table[page_table_index].physical_page_base_address;

    // Never touch kernel reserved frames or frames outside physical memory
    if (frame < KERNEL_RESERVED_PAGE_FRAME_COUNT || frame >= PAGE_FRAME_MAX_COUNT) return false;

    page_manager_state.page_frame_map[frame] = false;
    page_manager_state.free_page_frame_count++;

    memset(&page_table->table[page_table_index], 0, sizeof(struct PageTableEntry));
    flush_single_tlb(virtual_addr);
    return true;
}
