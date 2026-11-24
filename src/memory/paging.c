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

/**
 * Find the first free physical page frame (first-fit).
 * Returns PAGE_FRAME_MAX_COUNT when no frame is available.
 */
static uint32_t paging_find_free_page_frame(void) {
    for (uint32_t page_frame_number = KERNEL_RESERVED_PAGE_FRAME_COUNT;
         page_frame_number < PAGE_FRAME_MAX_COUNT;
         page_frame_number++) {
        if (!page_manager_state.page_frame_map[page_frame_number])
            return page_frame_number;
    }
    return PAGE_FRAME_MAX_COUNT;
}

struct PageTable *paging_get_page_table(struct PageDirectory *page_dir, uint32_t page_directory_index) {
    //this helper function is given from kit but you can always modified it if you think theres a bug (spoiler alert) or you think theres a better approach

    if (page_directory_index == RECURSIVE_PAGE_DIRECTORY_INDEX || page_dir != paging_get_current_page_directory_addr()) return NULL;

    struct PageDirectoryEntry *dir_entry = &page_dir->table[page_directory_index];

    if (dir_entry->flag.present_bit) {
        return (struct PageTable *)(RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS + page_directory_index * PAGE_FRAME_SIZE);
    }

    uint32_t page_frame_number = paging_find_free_page_frame();
    if (page_frame_number == PAGE_FRAME_MAX_COUNT) return NULL;

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

/* --- Multiple virtual address space --- */
#define KERNEL_PAGE_DIRECTORY_INDEX (KERNEL_VIRTUAL_ADDRESS_BASE >> 22)   // 0x300

__attribute__((aligned(0x1000))) static struct PageDirectory page_directory_list[PAGING_DIRECTORY_TABLE_MAX_COUNT];

static struct {
    bool page_directory_used[PAGING_DIRECTORY_TABLE_MAX_COUNT];
} page_directory_manager = {
    .page_directory_used = {false},
};

struct PageDirectory* paging_create_new_page_directory(void) {
    for (uint32_t i = 0; i < PAGING_DIRECTORY_TABLE_MAX_COUNT; i++) {
        if (page_directory_manager.page_directory_used[i])
            continue;

        page_directory_manager.page_directory_used[i] = true;
        struct PageDirectory *page_dir = &page_directory_list[i];
        memset(page_dir, 0, sizeof(struct PageDirectory));

        // Kernel higher half: pakai page table 4 KiB milik kernel (CR4.PSE dimatikan,
        // jadi tidak boleh pakai entry 4 MiB seperti di TODO kit)
        page_dir->table[KERNEL_PAGE_DIRECTORY_INDEX] =
            _paging_kernel_page_directory.table[KERNEL_PAGE_DIRECTORY_INDEX];

        // Recursive mapping: PDE[1023] menunjuk ke PD ini sendiri (dipakai paging_get_page_table)
        uint32_t page_dir_phys_addr = (uint32_t) page_dir - KERNEL_VIRTUAL_ADDRESS_BASE;
        page_dir->table[RECURSIVE_PAGE_DIRECTORY_INDEX].flag = (struct PageDirectoryEntryFlag) {
            .present_bit = true,
            .write_bit   = true,
        };
        page_dir->table[RECURSIVE_PAGE_DIRECTORY_INDEX].page_table_base_address = page_dir_phys_addr >> 12;

        return page_dir;
    }
    return NULL;
}

bool paging_free_page_directory(struct PageDirectory *page_dir) {
    // Jangan pernah free PD yang sedang aktif di CR3
    if (page_dir == NULL || page_dir == paging_get_current_page_directory_addr())
        return false;

    for (uint32_t i = 0; i < PAGING_DIRECTORY_TABLE_MAX_COUNT; i++) {
        if (&page_directory_list[i] != page_dir)
            continue;
        if (!page_directory_manager.page_directory_used[i])
            return false;

        // Kembalikan frame page table bagian user ke page frame manager
        for (uint32_t pd_index = 0; pd_index < KERNEL_PAGE_DIRECTORY_INDEX; pd_index++) {
            if (!page_dir->table[pd_index].flag.present_bit)
                continue;
            uint32_t frame = page_dir->table[pd_index].page_table_base_address;
            if (frame >= KERNEL_RESERVED_PAGE_FRAME_COUNT && frame < PAGE_FRAME_MAX_COUNT &&
                page_manager_state.page_frame_map[frame]) {
                page_manager_state.page_frame_map[frame] = false;
                page_manager_state.free_page_frame_count++;
            }
        }

        memset(page_dir, 0, sizeof(struct PageDirectory));
        page_directory_manager.page_directory_used[i] = false;
        return true;
    }
    return false;
}

void paging_use_page_directory(struct PageDirectory *page_dir_virtual_addr) {
    uint32_t physical_addr_page_dir = (uint32_t) page_dir_virtual_addr;
    if ((uint32_t) page_dir_virtual_addr >= KERNEL_VIRTUAL_ADDRESS_BASE)
        physical_addr_page_dir -= KERNEL_VIRTUAL_ADDRESS_BASE;
    __asm__ volatile("mov %0, %%cr3" : /* <Empty> */ : "r"(physical_addr_page_dir) : "memory");
}

/* --- Memory Management --- */
bool paging_allocate_check(uint32_t amount) {
    // ceil(amount / PAGE_FRAME_SIZE) without overflowing on large amounts.
    uint32_t required_frames = amount / PAGE_FRAME_SIZE;
    if (amount % PAGE_FRAME_SIZE != 0)
        required_frames++;

    return required_frames <= page_manager_state.free_page_frame_count;
}


bool paging_allocate_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    if (page_dir == NULL) return false;

    uint32_t va = (uint32_t) virtual_addr;

    // User space only (below kernel higher half) and must be page aligned
    if (va >= KERNEL_VIRTUAL_ADDRESS_BASE || (va & (PAGE_FRAME_SIZE - 1))) return false;

    uint32_t page_directory_index = (va >> 22) & 0x3FF;
    uint32_t page_table_index = (va >> 12) & 0x3FF;

    // Reserve the data frame first so a failed page-table allocation can be
    // rolled back without leaking a frame.
    uint32_t frame = paging_find_free_page_frame();
    if (frame == PAGE_FRAME_MAX_COUNT) return false;

    page_manager_state.page_frame_map[frame] = true;
    page_manager_state.free_page_frame_count--;

    // Get page table (will be created if not exist yet)
    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL || page_table->table[page_table_index].flag.present_bit) {
        // Allocation failed or the virtual address is already mapped.
        page_manager_state.page_frame_map[frame] = false;
        page_manager_state.free_page_frame_count++;
        return false;
    }

    page_table->table[page_table_index].flag = (struct PageTableEntryFlag) {
        .present_bit = true,
        .write_bit   = true,
        .us_bit      = true,
    };
    page_table->table[page_table_index].physical_page_base_address = frame;
    flush_single_tlb(virtual_addr);
    return true;
}

bool paging_free_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    if (page_dir == NULL) return false;

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

    // Guard against double-free / an inconsistent page frame map.
    if (!page_manager_state.page_frame_map[frame]) return false;

    page_manager_state.page_frame_map[frame] = false;
    page_manager_state.free_page_frame_count++;

    memset(&page_table->table[page_table_index], 0, sizeof(struct PageTableEntry));
    flush_single_tlb(virtual_addr);
    return true;
}
