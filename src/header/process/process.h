#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "header/cpu/interrupt.h"
#include "header/memory/paging.h"
#include "header/filesystem/ext2.h"
#include "header/process/process-info.h"
#include "header/process/exec-request.h"

#define CPU_EFLAGS_BASE_FLAG               0x2
#define CPU_EFLAGS_FLAG_CARRY              0x1
#define CPU_EFLAGS_FLAG_PARITY             0x4
#define CPU_EFLAGS_FLAG_AUX_CARRY          0x10
#define CPU_EFLAGS_FLAG_ZERO               0x40
#define CPU_EFLAGS_FLAG_SIGN               0x80
#define CPU_EFLAGS_FLAG_TRAP               0x100
#define CPU_EFLAGS_FLAG_INTERRUPT_ENABLE   0x200
#define CPU_EFLAGS_FLAG_DIRECTION          0x400
#define CPU_EFLAGS_FLAG_OVERFLOW           0x800
#define CPU_EFLAGS_FLAG_IO_PRIVILEGE       0x3000
#define CPU_EFLAGS_FLAG_NESTED_TASK        0x4000
#define CPU_EFLAGS_FLAG_MODE               0x8000
#define CPU_EFLAGS_FLAG_RESUME             0x10000
#define CPU_EFLAGS_FLAG_VIRTUAL_8086       0x20000
#define CPU_EFLAGS_FLAG_ALIGNMENT_CHECK    0x40000
#define CPU_EFLAGS_FLAG_VINTERRUPT_FLAG    0x80000
#define CPU_EFLAGS_FLAG_VINTERRUPT_PENDING 0x100000
#define CPU_EFLAGS_FLAG_CPUID_INSTRUCTION  0x200000
#define CPU_EFLAGS_FLAG_AES_SCHEDULE_LOAD  0x40000000
#define CPU_EFLAGS_FLAG_ALTER_INSTRUCTION  0x80000000

/**
 * User process virtual memory layout (flat binary linked at 0, see user-linker.ld):
 *   [0, binary size + PROCESS_HEAP_SLACK)                  code, data, bss
 *   [PROCESS_STACK_TOP - PROCESS_STACK_SIZE, STACK_TOP)    user stack
 */
#define PROCESS_BINARY_MAX_SIZE     (512u * 1024u)
#define PROCESS_HEAP_SLACK          (64u * 1024u)
#define PROCESS_STACK_SIZE          (64u * 1024u)
#define PROCESS_STACK_TOP           0x400000u
#define PROCESS_PAGE_FRAME_COUNT_MAX \
    ((PROCESS_BINARY_MAX_SIZE + PROCESS_HEAP_SLACK + PROCESS_STACK_SIZE) / PAGE_FRAME_SIZE)

// Return codes of process_create_user_process() (PROCESS_CREATE_*) are in header/process/exec-request.h

/**
 * Contain information needed for a task to be able to get interrupted and resumed later
 *
 * @param cpu                         All CPU registers (esp holds the user stack pointer)
 * @param eip                         Instruction pointer to resume to
 * @param eflags                      CPU flags register
 * @param page_directory_virtual_addr Page directory of the process (kernel virtual address)
 */
struct Context {
    struct CPURegister    cpu;
    uint32_t              eip;
    uint32_t              eflags;
    struct PageDirectory *page_directory_virtual_addr;
} __attribute__((packed));

_Static_assert(sizeof(struct CPURegister) == 48, "CPURegister layout is used by context-switch.s");
_Static_assert(sizeof(struct Context) == 60, "Context layout is used by context-switch.s");

/**
 * Process Control Block, all information about one process
 *
 * @param metadata Process identity & state
 * @param context  Saved CPU state, restored by process_context_switch()
 * @param memory   User page frames owned by the process
 */
struct ProcessControlBlock {
    struct {
        uint32_t pid;
        char     name[PROCESS_NAME_LENGTH_MAX];
        uint32_t state;
        uint32_t cwd_inode;   // working directory given by the parent (SYSCALL_GET_CWD)
    } metadata;

    struct Context context;

    struct {
        void     *virtual_addr_used[PROCESS_PAGE_FRAME_COUNT_MAX];
        uint32_t page_frame_used_count;
    } memory;
};

/**
 * @param active_process_count Number of PCB in use
 * @param last_pid             Last generated PID, PID is never reused
 */
struct ProcessManagerState {
    uint32_t active_process_count;
    uint32_t last_pid;
};

extern struct ProcessControlBlock _process_list[PROCESS_COUNT_MAX];
extern struct ProcessManagerState process_manager_state;

/**
 * Get currently running process PCB pointer
 *
 * @return Will return NULL if there's no running process
 */
struct ProcessControlBlock* process_get_current_running_pcb_pointer(void);

/**
 * Create new user process and setup the virtual address space.
 * All available return code is defined with macro "PROCESS_CREATE_*"
 *
 * @note          This procedure assumes no reentrancy in ISR
 * @param request Appropriate read request for the executable, request.buf is the entrypoint
 * @return        Process creation return code
 */
int32_t process_create_user_process(struct EXT2DriverRequest request);

/**
 * Create new user process with arguments (bonus: separate shell commands).
 * The executable can be a flat binary or an ELF32 executable. argc / argv are
 * placed on the new user stack as described in the book:
 *   ESP -> argc, ESP+4 -> argv[0], ..., ESP+4*(argc+1) -> NULL
 *
 * @note          argv must still be readable, call this from the caller's address space
 * @param request Read request of the executable, request.buf is the entrypoint for flat binaries
 * @param argc    Number of arguments (at most EXEC_ARGC_MAX), 0 means argv[0] = file name
 * @param argv    Argument strings
 * @param cwd_inode Working directory of the new process
 * @param pid_out PID of the new process when creation succeeds, can be NULL
 * @return        Process creation return code
 */
int32_t process_create_user_process_with_args(struct EXT2DriverRequest request, uint32_t argc,
                                              char *const *argv, uint32_t cwd_inode, uint32_t *pid_out);

/**
 * Destroy process then release page directory and process control block
 *
 * @param pid Process ID to delete
 * @return    True if process destruction success
 */
bool process_destroy(uint32_t pid);

/**
 * Find PCB by PID
 *
 * @return PCB pointer, NULL if no active process has that PID
 */
struct ProcessControlBlock* process_get_pcb_by_pid(uint32_t pid);

/**
 * Fill info with every active process
 *
 * @param info      Output array
 * @param max_count Capacity of info
 * @return          Number of entries written
 */
uint32_t process_get_info_list(struct ProcessInfo *info, uint32_t max_count);

#endif
