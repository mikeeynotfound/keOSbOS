#include "header/process/process.h"
#include "header/memory/paging.h"
#include "header/stdlib/string.h"
#include "header/cpu/gdt.h"

struct ProcessControlBlock _process_list[PROCESS_COUNT_MAX] = {0};

struct ProcessManagerState process_manager_state = {
    .active_process_count = 0,
    .last_pid             = 0,
};

// Executable is read here first: the target address space is not active while reading the request
static uint8_t process_binary_buffer[PROCESS_BINARY_MAX_SIZE];

static uint32_t ceil_div(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

static int32_t process_list_get_inactive_index(void) {
    for (int32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        if (_process_list[i].metadata.state == PROCESS_STATE_INACTIVE)
            return i;
    }
    return -1;
}

static uint32_t process_generate_new_pid(void) {
    // PID 0 is reserved for the init process and PIDs are never reused.
    return process_manager_state.last_pid++;
}

struct ProcessControlBlock* process_get_pcb_by_pid(uint32_t pid) {
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        struct ProcessControlBlock *pcb = &_process_list[i];
        if (pcb->metadata.state != PROCESS_STATE_INACTIVE && pcb->metadata.pid == pid)
            return pcb;
    }
    return NULL;
}

struct ProcessControlBlock* process_get_current_running_pcb_pointer(void) {
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        if (_process_list[i].metadata.state == PROCESS_STATE_RUNNING)
            return &_process_list[i];
    }
    return NULL;
}

// Map one page in the active page directory and record it in the PCB
static bool process_map_page(struct ProcessControlBlock *pcb, uint32_t virtual_addr) {
    if (pcb->memory.page_frame_used_count >= PROCESS_PAGE_FRAME_COUNT_MAX)
        return false;
    if (!paging_allocate_user_page_frame(pcb->context.page_directory_virtual_addr, (void *) virtual_addr))
        return false;
    pcb->memory.virtual_addr_used[pcb->memory.page_frame_used_count++] = (void *) virtual_addr;
    return true;
}

int32_t process_create_user_process(struct EXT2DriverRequest request) {
    int32_t retcode = PROCESS_CREATE_SUCCESS;

    if (process_manager_state.active_process_count >= PROCESS_COUNT_MAX) {
        retcode = PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED;
        goto exit_cleanup;
    }

    // Entrypoint must be a user virtual address and page aligned
    uint32_t entrypoint = (uint32_t) request.buf;
    if (entrypoint >= KERNEL_VIRTUAL_ADDRESS_BASE || (entrypoint & (PAGE_FRAME_SIZE - 1))) {
        retcode = PROCESS_CREATE_FAIL_INVALID_ENTRYPOINT;
        goto exit_cleanup;
    }

    // Resolve the file while the caller's address space (which owns request.name) is still active
    uint32_t binary_size = 0;
    if (get_file_size(request, &binary_size) != 0 || binary_size == 0) {
        retcode = PROCESS_CREATE_FAIL_FS_READ_FAILURE;
        goto exit_cleanup;
    }
    if (binary_size > PROCESS_BINARY_MAX_SIZE
        || entrypoint + binary_size + PROCESS_HEAP_SLACK > PROCESS_STACK_TOP - PROCESS_STACK_SIZE) {
        retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
        goto exit_cleanup;
    }

    // Reserve frames for the image, the user stack, and the page table itself
    uint32_t image_size = ceil_div(binary_size + PROCESS_HEAP_SLACK, PAGE_FRAME_SIZE) * PAGE_FRAME_SIZE;
    uint32_t page_frame_count_needed = (image_size + PROCESS_STACK_SIZE) / PAGE_FRAME_SIZE;
    if (page_frame_count_needed > PROCESS_PAGE_FRAME_COUNT_MAX
        || !paging_allocate_check((page_frame_count_needed + 2) * PAGE_FRAME_SIZE)) {
        retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
        goto exit_cleanup;
    }

    struct EXT2DriverRequest read_request = request;
    read_request.buf         = process_binary_buffer;
    read_request.buffer_size = PROCESS_BINARY_MAX_SIZE;
    if (read(read_request) != 0) {
        retcode = PROCESS_CREATE_FAIL_FS_READ_FAILURE;
        goto exit_cleanup;
    }

    int32_t p_index = process_list_get_inactive_index();
    struct PageDirectory *page_dir = paging_create_new_page_directory();
    if (p_index < 0 || page_dir == NULL) {
        retcode = p_index < 0 ? PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED : PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
        if (page_dir != NULL)
            paging_free_page_directory(page_dir);
        goto exit_cleanup;
    }

    struct ProcessControlBlock *new_pcb = &_process_list[p_index];
    memset(new_pcb, 0, sizeof(struct ProcessControlBlock));
    new_pcb->context.page_directory_virtual_addr = page_dir;

    uint32_t name_len = request.name_len < PROCESS_NAME_LENGTH_MAX - 1
                      ? request.name_len : PROCESS_NAME_LENGTH_MAX - 1;
    memcpy(new_pcb->metadata.name, request.name, name_len);
    new_pcb->metadata.name[name_len] = '\0';

    // Recursive page table mapping only works for the active directory
    struct PageDirectory *previous_page_dir = paging_get_current_page_directory_addr();
    paging_use_page_directory(page_dir);

    bool mapped = true;
    for (uint32_t offset = 0; mapped && offset < image_size; offset += PAGE_FRAME_SIZE)
        mapped = process_map_page(new_pcb, entrypoint + offset);
    for (uint32_t addr = PROCESS_STACK_TOP - PROCESS_STACK_SIZE;
         mapped && addr < PROCESS_STACK_TOP; addr += PAGE_FRAME_SIZE)
        mapped = process_map_page(new_pcb, addr);

    if (mapped) {
        memcpy((void *) entrypoint, process_binary_buffer, binary_size);
        memset((uint8_t *) entrypoint + binary_size, 0, image_size - binary_size);
        memset((void *) (PROCESS_STACK_TOP - PROCESS_STACK_SIZE), 0, PROCESS_STACK_SIZE);
    }

    if (!mapped) {
        // paging_free_page_directory() only releases page tables; release the
        // data frames while their directory is still active.
        for (uint32_t i = 0; i < new_pcb->memory.page_frame_used_count; i++)
            paging_free_user_page_frame(page_dir, new_pcb->memory.virtual_addr_used[i]);
    }
    paging_use_page_directory(previous_page_dir);

    if (!mapped) {
        paging_free_page_directory(page_dir);
        memset(new_pcb, 0, sizeof(struct ProcessControlBlock));
        retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
        goto exit_cleanup;
    }

    // Initial ring 3 context: user segments, stack at top of the stack region, interrupts enabled
    uint32_t user_data_selector = GDT_USER_DATA_SEGMENT_SELECTOR | 0x3;
    new_pcb->context.cpu.segment.ds = user_data_selector;
    new_pcb->context.cpu.segment.es = user_data_selector;
    new_pcb->context.cpu.segment.fs = user_data_selector;
    new_pcb->context.cpu.segment.gs = user_data_selector;
    new_pcb->context.cpu.stack.esp  = PROCESS_STACK_TOP - 4;
    new_pcb->context.cpu.stack.ebp  = PROCESS_STACK_TOP - 4;
    new_pcb->context.eip            = entrypoint;
    new_pcb->context.eflags         = CPU_EFLAGS_BASE_FLAG | CPU_EFLAGS_FLAG_INTERRUPT_ENABLE;

    new_pcb->metadata.pid   = process_generate_new_pid();
    new_pcb->metadata.state = PROCESS_STATE_READY;
    process_manager_state.active_process_count++;

exit_cleanup:
    return retcode;
}

bool process_destroy(uint32_t pid) {
    struct ProcessControlBlock *pcb = process_get_pcb_by_pid(pid);
    if (pcb == NULL)
        return false;

    struct PageDirectory *target_page_dir = pcb->context.page_directory_virtual_addr;
    struct PageDirectory *current_page_dir = paging_get_current_page_directory_addr();

    // paging_get_page_table() is recursive-mapping based and only works for
    // the active directory. Free the process data frames first, then switch
    // away so paging_free_page_directory() accepts the now inactive target.
    paging_use_page_directory(target_page_dir);
    for (uint32_t i = 0; i < pcb->memory.page_frame_used_count; i++)
        paging_free_user_page_frame(target_page_dir, pcb->memory.virtual_addr_used[i]);

    struct PageDirectory *restore_page_dir = current_page_dir == target_page_dir
                                           ? &_paging_kernel_page_directory
                                           : current_page_dir;
    paging_use_page_directory(restore_page_dir);

    paging_free_page_directory(target_page_dir);
    memset(pcb, 0, sizeof(struct ProcessControlBlock));
    process_manager_state.active_process_count--;
    return true;
}

uint32_t process_get_info_list(struct ProcessInfo *info, uint32_t max_count) {
    if (info == NULL)
        return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX && count < max_count; i++) {
        struct ProcessControlBlock *pcb = &_process_list[i];
        if (pcb->metadata.state == PROCESS_STATE_INACTIVE)
            continue;
        info[count].pid              = pcb->metadata.pid;
        info[count].state            = pcb->metadata.state;
        info[count].page_frame_count = pcb->memory.page_frame_used_count;
        memcpy(info[count].name, pcb->metadata.name, PROCESS_NAME_LENGTH_MAX);
        count++;
    }
    return count;
}
