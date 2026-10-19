#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/stdlib/string.h"
#include "header/filesystem/ext2.h"

static void read_inode_from_disk(uint32_t inode_num, struct EXT2Inode *out_inode) {
    if (inode_num == 0 || out_inode == NULL) return;

    struct EXT2BlockGroupDescriptorTable bgd_table;
    read_blocks(&bgd_table, 2, 1);

    uint32_t group = inode_to_bgd(inode_num);
    uint32_t local = inode_to_local(inode_num);

    uint32_t inode_table_start = bgd_table.table[group].bg_inode_table;
    uint32_t byte_offset = local * sizeof(struct EXT2Inode);
    uint32_t target_block = inode_table_start + (byte_offset / BLOCK_SIZE);
    uint32_t in_block_offset = byte_offset % BLOCK_SIZE;

    uint8_t buf[BLOCK_SIZE];
    read_blocks(buf, target_block, 1);
    memcpy(out_inode, buf + in_block_offset, sizeof(struct EXT2Inode));
}

int8_t delete(struct EXT2DriverRequest request) {
    if (request.parent_inode == 0) {
        return 3;
    }

    struct EXT2Inode parent_inode;
    read_inode_from_disk(request.parent_inode, &parent_inode);

    if ((parent_inode.i_mode & EXT2_S_IFDIR) == 0) {
        return 3;
    }

    uint32_t target_inode_num = 0;
    uint32_t target_block_num = 0;
    uint32_t target_entry_offset = 0;
    bool found = false;

    uint8_t block_buf[BLOCK_SIZE];

    for (int i = 0; i < 15 && parent_inode.i_block[i] != 0; i++) {
        uint32_t block_id = parent_inode.i_block[i];
        read_blocks(block_buf, block_id, 1);

        uint32_t offset = 0;
        while (offset < BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(block_buf, offset);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0) {
                char *entry_name = get_entry_name(entry);
                bool entry_is_dir = (entry->file_type == EXT2_FT_DIR);

                if (entry->name_len == request.name_len &&
                    memcmp(entry_name, request.name, request.name_len) == 0 &&
                    request.is_directory == entry_is_dir) {

                    target_inode_num = entry->inode;
                    target_block_num = block_id;
                    target_entry_offset = offset;
                    found = true;
                    break;
                }
            }

            offset += entry->rec_len;
        }

        if (found) break;
    }

    if (!found) {
        return 1;
    }

    if (request.is_directory) {
        if (!is_directory_empty(target_inode_num)) {
            return 2;
        }
    }

    deallocate_node(target_inode_num);

    read_blocks(block_buf, target_block_num, 1);
    struct EXT2DirectoryEntry *entry_to_delete = get_directory_entry(block_buf, target_entry_offset);
    
    entry_to_delete->inode = 0;

    write_blocks(block_buf, target_block_num, 1);

    return 0;
}
