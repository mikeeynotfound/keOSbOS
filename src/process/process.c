#include "header/process/process.h"
#include "header/memory/paging.h"
#include "header/stdlib/string.h"
#include "header/cpu/gdt.h"
#include "header/process/elf.h"

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

#define PROCESS_ROOT_INODE 1u

// Map one page in the active page directory, clear it and record it in the PCB
static bool process_map_page(struct ProcessControlBlock *pcb, uint32_t virtual_addr) {
    if (pcb->memory.page_frame_used_count >= PROCESS_PAGE_FRAME_COUNT_MAX)
        return false;
    if (!paging_allocate_user_page_frame(pcb->context.page_directory_virtual_addr, (void *) virtual_addr))
        return false;
    pcb->memory.virtual_addr_used[pcb->memory.page_frame_used_count++] = (void *) virtual_addr;
    memset((void *) virtual_addr, 0, PAGE_FRAME_SIZE);
    return true;
}

static bool process_page_is_mapped(struct ProcessControlBlock *pcb, uint32_t virtual_addr) {
    for (uint32_t i = 0; i < pcb->memory.page_frame_used_count; i++) {
        if ((uint32_t) pcb->memory.virtual_addr_used[i] == virtual_addr)
            return true;
    }
    return false;
}

// Map every page of [start, end) that is not mapped yet (ELF segments may share a page)
static bool process_map_range(struct ProcessControlBlock *pcb, uint32_t start, uint32_t end) {
    for (uint32_t page = start & ~(PAGE_FRAME_SIZE - 1); page < end; page += PAGE_FRAME_SIZE) {
        if (!process_page_is_mapped(pcb, page) && !process_map_page(pcb, page))
            return false;
    }
    return true;
}

/* -- Arguments: copied to kernel memory first, the caller's address space is not active later -- */

static struct {
    uint32_t argc;
    uint32_t length[EXEC_ARGC_MAX];      // without '\0'
    uint32_t offset[EXEC_ARGC_MAX];      // into data
    char     data[EXEC_ARGS_SIZE_MAX];
} process_args;

static bool process_args_add(const char *str, uint32_t len) {
    uint32_t used = process_args.argc == 0 ? 0
                  : process_args.offset[process_args.argc - 1] + process_args.length[process_args.argc - 1] + 1;
    if (process_args.argc >= EXEC_ARGC_MAX || used + len + 1 > EXEC_ARGS_SIZE_MAX)
        return false;
    memcpy(process_args.data + used, str, len);
    process_args.data[used + len] = '\0';
    process_args.offset[process_args.argc] = used;
    process_args.length[process_args.argc] = len;
    process_args.argc++;
    return true;
}

static bool process_args_load(struct EXT2DriverRequest *request, uint32_t argc, char *const *argv) {
    process_args.argc = 0;
    if (argc == 0 || argv == NULL)
        return process_args_add(request->name, request->name_len);   // argv[0] = file name
    if ((uint32_t) argv >= KERNEL_VIRTUAL_ADDRESS_BASE)
        return false;

    for (uint32_t i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL || (uint32_t) arg >= KERNEL_VIRTUAL_ADDRESS_BASE)
            return false;
        uint32_t len = 0;
        while (len < EXEC_ARGS_SIZE_MAX && arg[len] != '\0')
            len++;
        if (!process_args_add(arg, len))
            return false;
    }
    return true;
}

/**
 * Build the initial user stack (new address space must be active):
 *   ESP -> argc, ESP+4 -> argv[0], ..., ESP+4*argc -> argv[argc-1], ESP+4*(argc+1) -> NULL
 * The argument strings themselves are stored at the very top of the stack.
 */
static uint32_t process_setup_user_stack(void) {
    uint32_t sp = PROCESS_STACK_TOP;
    uint32_t string_addr[EXEC_ARGC_MAX];

    for (int32_t i = (int32_t) process_args.argc - 1; i >= 0; i--) {
        uint32_t len = process_args.length[i];
        sp -= len + 1;
        memcpy((void *) sp, process_args.data + process_args.offset[i], len + 1);
        string_addr[i] = sp;
    }

    sp &= ~0x3u;
    sp -= 4 * (process_args.argc + 2);   // argc, argv[0..argc-1], NULL
    uint32_t *stack = (uint32_t *) sp;
    stack[0] = process_args.argc;
    for (uint32_t i = 0; i < process_args.argc; i++)
        stack[1 + i] = string_addr[i];
    stack[1 + process_args.argc] = 0;
    return sp;
}

/* -- ELF32 loader (bonus: separate shell commands) -- */

static bool elf_is_elf_file(const uint8_t *binary, uint32_t size) {
    return size >= sizeof(struct ELF32Header)
        && binary[0] == ELF_MAGIC0 && binary[1] == ELF_MAGIC1
        && binary[2] == ELF_MAGIC2 && binary[3] == ELF_MAGIC3;
}

static const struct ELF32ProgramHeader *elf_program_header(const uint8_t *binary, uint16_t index) {
    const struct ELF32Header *header = (const struct ELF32Header *) binary;
    return (const struct ELF32ProgramHeader *) (binary + header->e_phoff + index * header->e_phentsize);
}

/**
 * Check the ELF header and every PT_LOAD segment against the user memory layout.
 * On success *page_count is an upper bound of the pages needed by the segments.
 */
static bool elf_validate(const uint8_t *binary, uint32_t size, uint32_t *page_count) {
    const struct ELF32Header *header = (const struct ELF32Header *) binary;
    if (header->e_ident[ELF_IDENT_CLASS] != ELF_CLASS_32 || header->e_ident[ELF_IDENT_DATA] != ELF_DATA_LSB
        || header->e_type != ELF_TYPE_EXEC || header->e_machine != ELF_MACHINE_386
        || header->e_phentsize < sizeof(struct ELF32ProgramHeader) || header->e_phnum == 0
        || header->e_phoff > size || (uint32_t) header->e_phnum * header->e_phentsize > size - header->e_phoff)
        return false;

    const uint32_t user_limit = PROCESS_STACK_TOP - PROCESS_STACK_SIZE;   // segments must stay below the stack
    if (header->e_entry >= user_limit)
        return false;

    bool entry_loaded = false;
    *page_count = 0;
    for (uint16_t i = 0; i < header->e_phnum; i++) {
        const struct ELF32ProgramHeader *ph = elf_program_header(binary, i);
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset > size || ph->p_filesz > size - ph->p_offset
            || ph->p_vaddr >= user_limit || ph->p_memsz > user_limit - ph->p_vaddr)
            return false;

        uint32_t first_page = ph->p_vaddr & ~(PAGE_FRAME_SIZE - 1);
        *page_count += ceil_div(ph->p_vaddr + ph->p_memsz - first_page, PAGE_FRAME_SIZE);
        if (header->e_entry >= ph->p_vaddr && header->e_entry < ph->p_vaddr + ph->p_memsz)
            entry_loaded = true;
    }
    return entry_loaded;
}

// Copy every PT_LOAD segment to its virtual address (new address space must be active)
static bool elf_load_segments(struct ProcessControlBlock *pcb, const uint8_t *binary) {
    const struct ELF32Header *header = (const struct ELF32Header *) binary;
    for (uint16_t i = 0; i < header->e_phnum; i++) {
        const struct ELF32ProgramHeader *ph = elf_program_header(binary, i);
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0)
            continue;
        // Pages are zeroed when mapped, so [p_filesz, p_memsz) (.bss) is already 0
        if (!process_map_range(pcb, ph->p_vaddr, ph->p_vaddr + ph->p_memsz))
            return false;
        memcpy((void *) ph->p_vaddr, binary + ph->p_offset, ph->p_filesz);
    }
    return true;
}

/* -- Process creation -- */

int32_t process_create_user_process(struct EXT2DriverRequest request) {
    return process_create_user_process_with_args(request, 0, NULL, PROCESS_ROOT_INODE, NULL);
}

int32_t process_create_user_process_with_args(struct EXT2DriverRequest request, uint32_t argc,
                                              char *const *argv, uint32_t cwd_inode, uint32_t *pid_out) {
    int32_t retcode = PROCESS_CREATE_SUCCESS;

    if (process_manager_state.active_process_count >= PROCESS_COUNT_MAX) {
        retcode = PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED;
        goto exit_cleanup;
    }

    // Copy the arguments while the caller's address space (which owns argv) is still active
    if (argc > EXEC_ARGC_MAX || request.name == NULL || request.name_len == 0
        || !process_args_load(&request, argc, argv)) {
        retcode = PROCESS_CREATE_FAIL_INVALID_ARGUMENT;
        goto exit_cleanup;
    }

    // Flat binary entrypoint must be a user virtual address and page aligned
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
    if (binary_size > PROCESS_BINARY_MAX_SIZE) {
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

    // Decide the layout: ELF segments, or a flat binary copied to the entrypoint
    bool     is_elf     = elf_is_elf_file(process_binary_buffer, binary_size);
    uint32_t image_size = 0;            // flat binary only
    uint32_t image_pages;
    if (is_elf) {
        if (!elf_validate(process_binary_buffer, binary_size, &image_pages)) {
            retcode = PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
            goto exit_cleanup;
        }
        entrypoint = ((struct ELF32Header *) process_binary_buffer)->e_entry;
    } else {
        if (entrypoint + binary_size + PROCESS_HEAP_SLACK > PROCESS_STACK_TOP - PROCESS_STACK_SIZE) {
            retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
            goto exit_cleanup;
        }
        image_size  = ceil_div(binary_size + PROCESS_HEAP_SLACK, PAGE_FRAME_SIZE) * PAGE_FRAME_SIZE;
        image_pages = image_size / PAGE_FRAME_SIZE;
    }

    // Reserve frames for the image, the user stack, and the page table itself
    uint32_t page_frame_count_needed = image_pages + PROCESS_STACK_SIZE / PAGE_FRAME_SIZE;
    if (page_frame_count_needed > PROCESS_PAGE_FRAME_COUNT_MAX
        || !paging_allocate_check((page_frame_count_needed + 2) * PAGE_FRAME_SIZE)) {
        retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
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
    new_pcb->metadata.cwd_inode = cwd_inode != 0 ? cwd_inode : PROCESS_ROOT_INODE;

    uint32_t name_len = request.name_len < PROCESS_NAME_LENGTH_MAX - 1
                      ? request.name_len : PROCESS_NAME_LENGTH_MAX - 1;
    memcpy(new_pcb->metadata.name, request.name, name_len);
    new_pcb->metadata.name[name_len] = '\0';

    // Recursive page table mapping only works for the active directory
    struct PageDirectory *previous_page_dir = paging_get_current_page_directory_addr();
    paging_use_page_directory(page_dir);

    bool mapped;
    if (is_elf) {
        mapped = elf_load_segments(new_pcb, process_binary_buffer);
    } else {
        mapped = process_map_range(new_pcb, entrypoint, entrypoint + image_size);
        if (mapped)
            memcpy((void *) entrypoint, process_binary_buffer, binary_size);
    }
    if (mapped)
        mapped = process_map_range(new_pcb, PROCESS_STACK_TOP - PROCESS_STACK_SIZE, PROCESS_STACK_TOP);

    uint32_t user_esp = 0;
    if (mapped)
        user_esp = process_setup_user_stack();

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

    // Initial ring 3 context: user segments, stack holding argc / argv, interrupts enabled
    uint32_t user_data_selector = GDT_USER_DATA_SEGMENT_SELECTOR | 0x3;
    new_pcb->context.cpu.segment.ds = user_data_selector;
    new_pcb->context.cpu.segment.es = user_data_selector;
    new_pcb->context.cpu.segment.fs = user_data_selector;
    new_pcb->context.cpu.segment.gs = user_data_selector;
    new_pcb->context.cpu.stack.esp  = user_esp;
    new_pcb->context.cpu.stack.ebp  = user_esp;
    new_pcb->context.eip            = entrypoint;
    new_pcb->context.eflags         = CPU_EFLAGS_BASE_FLAG | CPU_EFLAGS_FLAG_INTERRUPT_ENABLE;

    new_pcb->metadata.pid   = process_generate_new_pid();
    new_pcb->metadata.state = PROCESS_STATE_READY;
    process_manager_state.active_process_count++;
    if (pid_out != NULL)
        *pid_out = new_pcb->metadata.pid;

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
