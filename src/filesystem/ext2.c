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

int32_t get_directory_entry(struct EXT2DirectoryEntry *dir_entry, uint32_t parent_inode, const char *name) {
    (void) dir_entry;
    (void) parent_inode;
    (void) name;
    return -1;
}

bool is_directory_empty(const struct EXT2Inode *inode) {
    (void) inode;
    return true;
}

void deallocate_node(uint32_t inode_idx, bool is_directory) {
    (void) inode_idx;
    (void) is_directory;
}

void initialize_filesystem_ext2(void) {
    // Inisialisasi filesystem kosong
}

int8_t delete(struct EXT2DriverRequest request) {
    struct EXT2Inode parent_inode;
    read_inode_from_disk(&parent_inode, request.parent_inode);

    struct EXT2DirectoryEntry target_entry;
    int32_t target_idx = get_directory_entry(&target_entry, request.parent_inode, request.name);
    if (target_idx < 0) {
        return 1; // Not Found
    }

    struct EXT2Inode target_inode;
    read_inode_from_disk(&target_inode, target_entry.inode);

    if (request.is_directory && !is_directory_empty(&target_inode)) {
        return 2; // Directory not empty
    }

    deallocate_node(target_entry.inode, request.is_directory);
    return 0;
}
