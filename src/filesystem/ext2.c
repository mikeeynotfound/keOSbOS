#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/stdlib/string.h"
#include "header/filesystem/ext2.h"
#include "header/filesystem/disk.h"

const uint8_t fs_signature[BLOCK_SIZE] = {
    'C', 'o', 'u', 'r', 's', 'e', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',  ' ',
    'D', 'e', 's', 'i', 'g', 'n', 'e', 'd', ' ', 'b', 'y', ' ', ' ', ' ', ' ',  ' ',
    'L', 'a', 'b', ' ', 'S', 'i', 's', 't', 'e', 'r', ' ', 'I', 'T', 'B', ' ',  ' ',
    'M', 'a', 'd', 'e', ' ', 'w', 'i', 't', 'h', ' ', '<', '3', ' ', ' ', ' ',  ' ',
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '2', '0', '2', '5', '\n',
    [BLOCK_SIZE-2] = 'O',
    [BLOCK_SIZE-1] = 'k',
};

void read_inode_from_disk(struct EXT2Inode *inode, uint32_t inode_idx) {
    (void) inode;
    (void) inode_idx;
}

uint32_t inode_to_bgd(uint32_t inode_idx) {
    (void) inode_idx;
    return 0;
}

uint32_t inode_to_local(uint32_t inode_idx) {
    (void) inode_idx;
    return 0;
}

// Menyesuaikan parameter dengan ext2.h
struct EXT2DirectoryEntry* get_directory_entry(void *ptr, uint32_t offset) {
    (void) ptr;
    (void) offset;
    return NULL;
}

// Menyesuaikan parameter dengan ext2.h
bool is_directory_empty(uint32_t inode) {
    (void) inode;
    return true;
}

// Menyesuaikan parameter dengan ext2.h
void deallocate_node(uint32_t inode_idx) {
    (void) inode_idx;
}

void initialize_filesystem_ext2(void) {
    // Inisialisasi filesystem kosong
}

int8_t delete(struct EXT2DriverRequest request) {
    struct EXT2Inode parent_inode;
    read_inode_from_disk(&parent_inode, request.parent_inode);

    // Menggunakan fungsi sesuai signature header asli
    struct EXT2DirectoryEntry* target_entry = get_directory_entry(NULL, 0);
    if (target_entry == NULL) {
        return 1; // Not Found
    }

    struct EXT2Inode target_inode;
    read_inode_from_disk(&target_inode, target_entry->inode);

    if (request.is_directory && !is_directory_empty(target_entry->inode)) {
        return 2; // Directory not empty
    }

    deallocate_node(target_entry->inode);
    return 0;
}
