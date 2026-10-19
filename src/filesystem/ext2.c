#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/stdlib/string.h"
#include "header/filesystem/ext2.h"

const uint8_t fs_signature[BLOCK_SIZE] = {
    'C', 'o', 'u', 'r', 's', 'e', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',  ' ',
    'D', 'e', 's', 'i', 'g', 'n', 'e', 'd', ' ', 'b', 'y', ' ', ' ', ' ', ' ',  ' ',
    'L', 'a', 'b', ' ', 'S', 'i', 's', 't', 'e', 'r', ' ', 'I', 'T', 'B', ' ',  ' ',
    'M', 'a', 'd', 'e', ' ', 'w', 'i', 't', 'h', ' ', '<', '3', ' ', ' ', ' ',  ' ',
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '2', '0', '2', '5', '\n',
    [BLOCK_SIZE-2] = 'O',
    [BLOCK_SIZE-1] = 'k',
};

/* -- Filesystem layout (all sizes derived from macros in ext2.h) --
 * Block 0 : boot sector (fs_signature)
 * Block 1 : superblock
 * Block 2 : block group descriptor table
 * Group g spans blocks [g * BLOCKS_PER_GROUP, (g + 1) * BLOCKS_PER_GROUP).
 * Each group: block bitmap, inode bitmap, INODES_TABLE_BLOCK_COUNT inode table blocks, data blocks.
 * Group 0 places that metadata right after block 2, other groups at their first block.
 * Inode numbers start at 1 (root = 1), pointers in i_block are absolute block numbers (0 = unused).
 */
#define SUPERBLOCK_LBA      1u
#define BGD_TABLE_LBA       2u
#define ROOT_INODE          1u
#define POINTERS_PER_BLOCK  (BLOCK_SIZE / sizeof(uint32_t))
#define DIRECT_BLOCKS       12u
#define INDIRECT_SLOT       12u
#define DOUBLY_SLOT         13u

static struct EXT2Superblock sb;
static struct EXT2BlockGroupDescriptorTable bgd_table;

static uint32_t get_block_pointer(const struct EXT2Inode *node, uint32_t idx);

/* -- Directory entry helpers -- */

char *get_entry_name(void *entry) {
    return (char*) entry + sizeof(struct EXT2DirectoryEntry);
}

struct EXT2DirectoryEntry *get_directory_entry(void *ptr, uint32_t offset) {
    return (struct EXT2DirectoryEntry*) ((uint8_t*) ptr + offset);
}

struct EXT2DirectoryEntry *get_next_directory_entry(struct EXT2DirectoryEntry *entry) {
    return (struct EXT2DirectoryEntry*) ((uint8_t*) entry + entry->rec_len);
}

uint16_t get_entry_record_len(uint8_t name_len) {
    // entries are aligned on 4 bytes boundaries
    return (uint16_t) ((sizeof(struct EXT2DirectoryEntry) + name_len + 3u) & ~3u);
}

uint32_t get_dir_first_child_offset(void *ptr) {
    struct EXT2DirectoryEntry *dot    = get_directory_entry(ptr, 0);
    struct EXT2DirectoryEntry *dotdot = get_next_directory_entry(dot);
    return dot->rec_len + dotdot->rec_len;
}

/* -- Inode addressing -- */

uint32_t inode_to_bgd(uint32_t inode) {
    return (inode - 1) / INODES_PER_GROUP;
}

uint32_t inode_to_local(uint32_t inode) {
    return (inode - 1) % INODES_PER_GROUP;
}

/* -- Internal helpers -- */

static bool bit_is_set(const uint8_t *bitmap, uint32_t bit) {
    return bitmap[bit / 8] & (1u << (bit % 8));
}

static void bit_set(uint8_t *bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t) (1u << (bit % 8));
}

// Commit superblock and bgd table to storage
static void sync_metadata(void) {
    struct BlockBuffer b;
    memset(&b, 0, sizeof(b));
    memcpy(&b, &sb, sizeof(sb));
    write_blocks(&b, SUPERBLOCK_LBA, 1);

    memset(&b, 0, sizeof(b));
    memcpy(&b, &bgd_table, sizeof(bgd_table));
    write_blocks(&b, BGD_TABLE_LBA, 1);
}

// Get a free block (first fit, starting from prefered_bgd), mark it used. Returns 0 when disk is full
static uint32_t allocate_block(uint32_t prefered_bgd) {
    struct BlockBuffer bitmap;
    for (uint32_t k = 0; k < GROUPS_COUNT; k++) {
        uint32_t g = (prefered_bgd + k) % GROUPS_COUNT;
        if (bgd_table.table[g].bg_free_blocks_count == 0)
            continue;

        read_blocks(&bitmap, bgd_table.table[g].bg_block_bitmap, 1);
        for (uint32_t bit = 0; bit < BLOCKS_PER_GROUP; bit++) {
            if (bit_is_set(bitmap.buf, bit))
                continue;
            bit_set(bitmap.buf, bit);
            write_blocks(&bitmap, bgd_table.table[g].bg_block_bitmap, 1);
            bgd_table.table[g].bg_free_blocks_count--;
            sb.s_free_blocks_count--;
            return g * BLOCKS_PER_GROUP + bit;
        }
    }
    return 0;
}

// Return table if it already exists, otherwise allocate a zeroed pointer block. Returns 0 if disk is full
static uint32_t get_or_allocate_table(uint32_t table, uint32_t prefered_bgd) {
    if (table != 0)
        return table;

    uint32_t blk = allocate_block(prefered_bgd);
    if (blk != 0) {
        struct BlockBuffer zero;
        memset(&zero, 0, sizeof(zero));
        write_blocks(&zero, blk, 1);
    }
    return blk;
}

// Store data block number blk as the idx-th block of node. Returns false if disk is full / idx out of range
static bool set_block_pointer(struct EXT2Inode *node, uint32_t idx, uint32_t blk, uint32_t prefered_bgd) {
    uint32_t table[POINTERS_PER_BLOCK];

    if (idx < DIRECT_BLOCKS) {
        node->i_block[idx] = blk;
        return true;
    }
    idx -= DIRECT_BLOCKS;

    if (idx < POINTERS_PER_BLOCK) {
        uint32_t ind = get_or_allocate_table(node->i_block[INDIRECT_SLOT], prefered_bgd);
        if (ind == 0)
            return false;
        node->i_block[INDIRECT_SLOT] = ind;
        read_blocks(table, ind, 1);
        table[idx] = blk;
        write_blocks(table, ind, 1);
        return true;
    }
    idx -= POINTERS_PER_BLOCK;

    if (idx < POINTERS_PER_BLOCK * POINTERS_PER_BLOCK) {
        uint32_t dbl = get_or_allocate_table(node->i_block[DOUBLY_SLOT], prefered_bgd);
        if (dbl == 0)
            return false;
        node->i_block[DOUBLY_SLOT] = dbl;
        read_blocks(table, dbl, 1);
        uint32_t before = table[idx / POINTERS_PER_BLOCK];
        uint32_t ind = get_or_allocate_table(before, prefered_bgd);
        if (ind == 0)
            return false;
        if (before != ind) {
            table[idx / POINTERS_PER_BLOCK] = ind;
            write_blocks(table, dbl, 1);
        }

        read_blocks(table, ind, 1);
        table[idx % POINTERS_PER_BLOCK] = blk;
        write_blocks(table, ind, 1);
        return true;
    }
    return false;
}

// Locate the inode table block and the byte offset of an inode inside it
static uint32_t inode_block(uint32_t inode) {
    uint32_t local = inode_to_local(inode);
    return bgd_table.table[inode_to_bgd(inode)].bg_inode_table + local / INODES_PER_TABLE;
}

static uint32_t inode_offset(uint32_t inode) {
    return (inode_to_local(inode) % INODES_PER_TABLE) * INODE_SIZE;
}

/* -- Filesystem core -- */

bool is_empty_storage(void) {
    struct BlockBuffer boot;
    read_blocks(&boot, BOOT_SECTOR, 1);
    return memcmp(&boot, fs_signature, BLOCK_SIZE) != 0;
}

uint32_t allocate_node(void) {
    struct BlockBuffer bitmap;
    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        if (bgd_table.table[g].bg_free_inodes_count == 0)
            continue;

        read_blocks(&bitmap, bgd_table.table[g].bg_inode_bitmap, 1);
        for (uint32_t bit = 0; bit < INODES_PER_GROUP; bit++) {
            if (bit_is_set(bitmap.buf, bit))
                continue;
            bit_set(bitmap.buf, bit);
            write_blocks(&bitmap, bgd_table.table[g].bg_inode_bitmap, 1);
            bgd_table.table[g].bg_free_inodes_count--;
            sb.s_free_inodes_count--;
            sync_metadata();
            return g * INODES_PER_GROUP + bit + 1;
        }
    }
    return 0;
}

void allocate_node_blocks(void *ptr, struct EXT2Inode *node, uint32_t prefered_bgd) {
    uint32_t wanted  = node->i_blocks;
    uint32_t written = 0;

    for (; written < wanted; written++) {
        uint32_t blk = allocate_block(prefered_bgd);
        if (blk == 0)
            break;
        if (!set_block_pointer(node, written, blk, prefered_bgd))
            break;

        uint32_t done = written * BLOCK_SIZE;
        if (done + BLOCK_SIZE <= node->i_size) {
            write_blocks((uint8_t*) ptr + done, blk, 1);
        } else {
            // last partial block: pad with zeros instead of reading past the end of ptr
            struct BlockBuffer tail;
            memset(&tail, 0, sizeof(tail));
            memcpy(&tail, (uint8_t*) ptr + done, node->i_size - done);
            write_blocks(&tail, blk, 1);
        }
    }

    node->i_blocks = written; // reflect what was really allocated
    sync_metadata();
}

void sync_node(struct EXT2Inode *node, uint32_t inode) {
    struct BlockBuffer b;
    uint32_t lba = inode_block(inode);
    read_blocks(&b, lba, 1);
    memcpy(b.buf + inode_offset(inode), node, INODE_SIZE);
    write_blocks(&b, lba, 1);
}

void init_directory_table(struct EXT2Inode *node, uint32_t inode, uint32_t parent_inode) {
    struct BlockBuffer b;
    memset(&b, 0, sizeof(b));

    // Entry 0: "." (itself)
    struct EXT2DirectoryEntry *dot = get_directory_entry(b.buf, 0);
    dot->inode     = inode;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->rec_len   = get_entry_record_len(1);
    memcpy(get_entry_name(dot), ".", 1);

    // Entry 1: ".." (parent, root points to itself)
    struct EXT2DirectoryEntry *dotdot = get_next_directory_entry(dot);
    dotdot->inode     = parent_inode;
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->rec_len   = get_entry_record_len(2);
    memcpy(get_entry_name(dotdot), "..", 2);

    // Entry 2: empty entry spanning the rest of the block, so is_directory_empty() can look at it
    struct EXT2DirectoryEntry *free_entry = get_next_directory_entry(dotdot);
    free_entry->inode     = 0;
    free_entry->name_len  = 0;
    free_entry->file_type = EXT2_FT_UNKNOWN;
    free_entry->rec_len   = (uint16_t) (BLOCK_SIZE - get_dir_first_child_offset(b.buf));

    memset(node, 0, sizeof(*node));
    node->i_mode   = EXT2_S_IFDIR;
    node->i_size   = BLOCK_SIZE;
    node->i_blocks = 1;

    bgd_table.table[inode_to_bgd(inode)].bg_used_dirs_count++;
    allocate_node_blocks(b.buf, node, inode_to_bgd(inode)); // also commits metadata
    sync_node(node, inode);
}

bool is_directory_empty(uint32_t inode) {
    struct EXT2Inode node;
    struct BlockBuffer dir;

    read_blocks(&dir, inode_block(inode), 1);
    memcpy(&node, dir.buf + inode_offset(inode), INODE_SIZE);

    // Deleting the first child can leave a hole before later entries, so look at every entry
    for (uint32_t i = 0; i < node.i_blocks; i++) {
        uint32_t blk = get_block_pointer(&node, i);
        if (blk == 0)
            continue;
        read_blocks(&dir, blk, 1);

        uint32_t offset = (i == 0) ? get_dir_first_child_offset(dir.buf) : 0;
        while (offset + sizeof(struct EXT2DirectoryEntry) <= BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(dir.buf, offset);
            if (entry->rec_len < sizeof(struct EXT2DirectoryEntry))
                break;
            if (entry->inode != 0)
                return false;
            offset += entry->rec_len;
        }
    }
    return true;
}

void create_ext2(void) {
    struct BlockBuffer b;

    // Boot sector: signature only
    write_blocks(fs_signature, BOOT_SECTOR, 1);

    // Superblock
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count      = INODES_PER_GROUP * GROUPS_COUNT;
    sb.s_blocks_count      = BLOCKS_PER_GROUP * GROUPS_COUNT;
    sb.s_first_data_block  = SUPERBLOCK_LBA;
    sb.s_first_ino         = ROOT_INODE;
    sb.s_blocks_per_group  = BLOCKS_PER_GROUP;
    sb.s_frags_per_group   = BLOCKS_PER_GROUP;
    sb.s_inodes_per_group  = INODES_PER_GROUP;
    sb.s_magic             = EXT2_SUPER_MAGIC;
    sb.s_free_inodes_count = sb.s_inodes_count;

    // Block group descriptors, bitmaps and (empty) inode tables
    memset(&bgd_table, 0, sizeof(bgd_table));
    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        uint32_t base = g * BLOCKS_PER_GROUP;
        uint32_t meta = (g == 0) ? BGD_TABLE_LBA + 1 : 0; // blocks before this group's own metadata

        struct EXT2BlockGroupDescriptor *bgd = &bgd_table.table[g];
        bgd->bg_block_bitmap = base + meta;
        bgd->bg_inode_bitmap = base + meta + 1;
        bgd->bg_inode_table  = base + meta + 2;
        uint32_t data_start  = meta + 2 + INODES_TABLE_BLOCK_COUNT; // local index of the first data block

        bgd->bg_free_blocks_count = (uint16_t) (BLOCKS_PER_GROUP - data_start);
        bgd->bg_free_inodes_count = INODES_PER_GROUP;
        sb.s_free_blocks_count   += bgd->bg_free_blocks_count;

        // Block bitmap: everything before data_start is metadata
        memset(&b, 0, sizeof(b));
        for (uint32_t bit = 0; bit < data_start; bit++)
            bit_set(b.buf, bit);
        write_blocks(&b, bgd->bg_block_bitmap, 1);

        // Inode bitmap and inode table start empty
        memset(&b, 0, sizeof(b));
        write_blocks(&b, bgd->bg_inode_bitmap, 1);
        for (uint32_t i = 0; i < INODES_TABLE_BLOCK_COUNT; i++)
            write_blocks(&b, bgd->bg_inode_table + i, 1);
    }
    sync_metadata();

    // Root directory: parent is itself
    struct EXT2Inode root;
    uint32_t root_inode = allocate_node();
    init_directory_table(&root, root_inode, root_inode);
}

void initialize_filesystem_ext2(void) {
    if (is_empty_storage()) {
        create_ext2();
        return;
    }

    struct BlockBuffer b;
    read_blocks(&b, SUPERBLOCK_LBA, 1);
    memcpy(&sb, &b, sizeof(sb));
    read_blocks(&b, BGD_TABLE_LBA, 1);
    memcpy(&bgd_table, &b, sizeof(bgd_table));
}

/* -- Lookup helpers shared by the CRUD interfaces -- */

// Fetch inode metadata from storage
static void read_inode(uint32_t inode, struct EXT2Inode *node) {
    struct BlockBuffer b;
    read_blocks(&b, inode_block(inode), 1);
    memcpy(node, b.buf + inode_offset(inode), INODE_SIZE);
}

// An inode is valid when it is in range and marked used in its group's inode bitmap
static bool is_inode_in_use(uint32_t inode) {
    if (inode < ROOT_INODE || inode > sb.s_inodes_count)
        return false;

    struct BlockBuffer bitmap;
    read_blocks(&bitmap, bgd_table.table[inode_to_bgd(inode)].bg_inode_bitmap, 1);
    return bit_is_set(bitmap.buf, inode_to_local(inode));
}

// Load parent_inode into node, false if it is not a usable directory
static bool load_parent_directory(uint32_t parent_inode, struct EXT2Inode *node) {
    if (!is_inode_in_use(parent_inode))
        return false;
    read_inode(parent_inode, node);
    return (node->i_mode & EXT2_S_IFDIR) == EXT2_S_IFDIR;
}

// Block number of the idx-th data block of node (direct, indirect, doubly indirect), 0 if none
static uint32_t get_block_pointer(const struct EXT2Inode *node, uint32_t idx) {
    uint32_t table[POINTERS_PER_BLOCK];

    if (idx < DIRECT_BLOCKS)
        return node->i_block[idx];
    idx -= DIRECT_BLOCKS;

    if (idx < POINTERS_PER_BLOCK) {
        if (node->i_block[INDIRECT_SLOT] == 0)
            return 0;
        read_blocks(table, node->i_block[INDIRECT_SLOT], 1);
        return table[idx];
    }
    idx -= POINTERS_PER_BLOCK;

    if (idx < POINTERS_PER_BLOCK * POINTERS_PER_BLOCK) {
        if (node->i_block[DOUBLY_SLOT] == 0)
            return 0;
        read_blocks(table, node->i_block[DOUBLY_SLOT], 1);
        uint32_t ind = table[idx / POINTERS_PER_BLOCK];
        if (ind == 0)
            return 0;
        read_blocks(table, ind, 1);
        return table[idx % POINTERS_PER_BLOCK];
    }
    return 0;
}

/**
 * Search a directory for an entry called name.
 * @param file_type EXT2_FT_REG_FILE / EXT2_FT_DIR to match that type only, EXT2_FT_UNKNOWN to match any
 * @return inode of the entry, 0 if not found
 */
static uint32_t find_entry(const struct EXT2Inode *dir, const char *name, uint8_t name_len, uint8_t file_type) {
    struct BlockBuffer b;

    for (uint32_t i = 0; i < dir->i_blocks; i++) {
        uint32_t blk = get_block_pointer(dir, i);
        if (blk == 0)
            continue;
        read_blocks(&b, blk, 1);

        uint32_t offset = 0;
        while (offset + sizeof(struct EXT2DirectoryEntry) <= BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(b.buf, offset);
            if (entry->rec_len < sizeof(struct EXT2DirectoryEntry)) // corrupted entry, avoid looping forever
                break;

            if (entry->inode != 0 && entry->name_len == name_len
                && offset + sizeof(struct EXT2DirectoryEntry) + name_len <= BLOCK_SIZE
                && (file_type == EXT2_FT_UNKNOWN || entry->file_type == file_type)
                && memcmp(get_entry_name(entry), name, name_len) == 0)
                return entry->inode;

            offset += entry->rec_len;
        }
    }
    return 0;
}

// Copy `size` bytes of node's data blocks into buf; the last partial block never writes past size
static void read_node_data(const struct EXT2Inode *node, void *buf, uint32_t size) {
    struct BlockBuffer tail;
    uint8_t *dst = (uint8_t*) buf;

    for (uint32_t i = 0; i * BLOCK_SIZE < size; i++) {
        uint32_t blk = get_block_pointer(node, i);
        uint32_t remaining = size - i * BLOCK_SIZE;
        if (remaining >= BLOCK_SIZE) {
            read_blocks(dst + i * BLOCK_SIZE, blk, 1);
        } else {
            read_blocks(&tail, blk, 1);
            memcpy(dst + i * BLOCK_SIZE, tail.buf, remaining);
        }
    }
}

/* -- CRUD: read -- */

int8_t read_directory(struct EXT2DriverRequest *prequest) {
    if (prequest == NULL || prequest->buf == NULL || prequest->name == NULL || prequest->name_len == 0)
        return -1;

    struct EXT2Inode parent, target;
    if (!load_parent_directory(prequest->parent_inode, &parent))
        return 3;

    uint32_t inode = find_entry(&parent, prequest->name, prequest->name_len, EXT2_FT_DIR);
    if (inode == 0) {
        // Name exists but as a file -> not a folder, otherwise it does not exist
        return find_entry(&parent, prequest->name, prequest->name_len, EXT2_FT_UNKNOWN) != 0 ? 1 : 2;
    }

    read_inode(inode, &target);
    uint32_t size = target.i_blocks * BLOCK_SIZE; // directory table is always whole blocks
    if (prequest->buffer_size < size)
        return -1; // read_directory has no dedicated "buffer too small" code

    read_node_data(&target, prequest->buf, size);
    return 0;
}

int8_t read(struct EXT2DriverRequest request) {
    if (request.buf == NULL || request.name == NULL || request.name_len == 0)
        return -1;

    struct EXT2Inode parent, target;
    if (!load_parent_directory(request.parent_inode, &parent))
        return 4;

    uint32_t inode = find_entry(&parent, request.name, request.name_len, EXT2_FT_REG_FILE);
    if (inode == 0) {
        // Name exists but as a folder -> not a file, otherwise it does not exist
        return find_entry(&parent, request.name, request.name_len, EXT2_FT_UNKNOWN) != 0 ? 1 : 3;
    }

    read_inode(inode, &target);
    if (request.buffer_size < target.i_size)
        return 2;

    read_node_data(&target, request.buf, target.i_size);
    return 0;
}

/* -- Write -- */

// Total blocks (data + pointer tables) a node with `data_blocks` data blocks occupies
static uint32_t blocks_with_tables(uint32_t data_blocks) {
    uint32_t total = data_blocks;
    if (data_blocks > DIRECT_BLOCKS)
        total++; // indirect table
    if (data_blocks > DIRECT_BLOCKS + POINTERS_PER_BLOCK) {
        uint32_t rest = data_blocks - DIRECT_BLOCKS - POINTERS_PER_BLOCK;
        total += 1 + (rest + POINTERS_PER_BLOCK - 1) / POINTERS_PER_BLOCK; // doubly table + its indirect tables
    }
    return total;
}

// Find room for a new entry of `need` bytes in dir. Returns true and sets blk_idx / offset if found
static bool find_entry_slot(const struct EXT2Inode *dir, uint16_t need, uint32_t *blk_idx, uint32_t *offset) {
    struct BlockBuffer b;

    for (uint32_t i = 0; i < dir->i_blocks; i++) {
        uint32_t blk = get_block_pointer(dir, i);
        if (blk == 0)
            continue;
        read_blocks(&b, blk, 1);

        uint32_t off = 0;
        while (off + sizeof(struct EXT2DirectoryEntry) <= BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(b.buf, off);
            if (entry->rec_len < sizeof(struct EXT2DirectoryEntry) || off + entry->rec_len > BLOCK_SIZE)
                break;

            uint16_t used = (entry->inode == 0) ? 0 : get_entry_record_len((uint8_t) entry->name_len);
            if (entry->rec_len - used >= need) {
                *blk_idx = i;
                *offset  = off;
                return true;
            }
            off += entry->rec_len;
        }
    }
    return false;
}

// Put a new entry into the slot found by find_entry_slot, splitting the entry that owns the space
static void insert_entry(uint32_t blk, uint32_t offset, uint32_t inode, const char *name, uint8_t name_len, uint8_t file_type) {
    struct BlockBuffer b;
    uint16_t need = get_entry_record_len(name_len);

    read_blocks(&b, blk, 1);
    struct EXT2DirectoryEntry *host = get_directory_entry(b.buf, offset);
    uint16_t total = host->rec_len;
    uint16_t used  = (host->inode == 0) ? 0 : get_entry_record_len((uint8_t) host->name_len);

    struct EXT2DirectoryEntry *entry = host;
    if (used != 0) { // shrink the live entry, new one takes the tail
        host->rec_len = used;
        entry = get_next_directory_entry(host);
    }

    uint16_t span = (uint16_t) (total - used);
    // keep the leftover as a free entry only when it can hold a header, otherwise absorb it
    uint16_t rec_len = (span - need >= get_entry_record_len(0)) ? need : span;

    entry->inode     = inode;
    entry->name_len  = name_len;
    entry->file_type = file_type;
    entry->rec_len   = rec_len;
    memcpy(get_entry_name(entry), name, name_len);

    if (rec_len != span) {
        struct EXT2DirectoryEntry *rest = get_next_directory_entry(entry);
        rest->inode     = 0;
        rest->name_len  = 0;
        rest->file_type = EXT2_FT_UNKNOWN;
        rest->rec_len   = (uint16_t) (span - rec_len);
    }
    write_blocks(&b, blk, 1);
}

int8_t write(struct EXT2DriverRequest *request) {
    if (request == NULL || request->name == NULL || request->name_len == 0)
        return -1;

    bool is_dir = (request->buffer_size == 0);
    if (!is_dir && request->buf == NULL)
        return -1;

    struct EXT2Inode parent;
    if (!load_parent_directory(request->parent_inode, &parent))
        return 2;

    uint8_t file_type = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (find_entry(&parent, request->name, request->name_len, file_type) != 0)
        return 1;

    // Reserve everything up front so a failed write never leaves a half-created file behind
    uint32_t data_blocks = is_dir ? 1 : (request->buffer_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (data_blocks > DIRECT_BLOCKS + POINTERS_PER_BLOCK + POINTERS_PER_BLOCK * POINTERS_PER_BLOCK)
        return -1;
    uint32_t needed = blocks_with_tables(data_blocks);

    uint16_t need_len = get_entry_record_len(request->name_len);
    uint32_t slot_idx = 0, slot_off = 0;
    bool has_slot = find_entry_slot(&parent, need_len, &slot_idx, &slot_off);
    if (!has_slot)
        needed += blocks_with_tables(parent.i_blocks + 1) - blocks_with_tables(parent.i_blocks);

    if (sb.s_free_inodes_count == 0 || sb.s_free_blocks_count < needed)
        return -1;

    uint32_t parent_bgd = inode_to_bgd(request->parent_inode);
    if (!has_slot) { // directory is full, append a fresh block holding one free entry
        uint32_t blk = allocate_block(parent_bgd);
        if (blk == 0 || !set_block_pointer(&parent, parent.i_blocks, blk, parent_bgd))
            return -1;

        struct BlockBuffer fresh;
        memset(&fresh, 0, sizeof(fresh));
        get_directory_entry(fresh.buf, 0)->rec_len = BLOCK_SIZE;
        write_blocks(&fresh, blk, 1);

        slot_idx = parent.i_blocks;
        slot_off = 0;
        parent.i_blocks++;
        parent.i_size = parent.i_blocks * BLOCK_SIZE;
    }

    uint32_t inode = allocate_node();
    if (inode == 0)
        return -1;

    struct EXT2Inode node;
    if (is_dir) {
        init_directory_table(&node, inode, request->parent_inode);
    } else {
        memset(&node, 0, sizeof(node));
        node.i_mode   = EXT2_S_IFREG;
        node.i_size   = request->buffer_size;
        node.i_blocks = data_blocks;
        allocate_node_blocks(request->buf, &node, inode_to_bgd(inode));
        sync_node(&node, inode);
    }

    insert_entry(get_block_pointer(&parent, slot_idx), slot_off, inode, request->name, request->name_len, file_type);
    sync_node(&parent, request->parent_inode);
    sync_metadata();
    return 0;
}

void deallocate_node(uint32_t inode_idx) {
    (void) inode_idx;
}

int8_t delete(struct EXT2DriverRequest request) {
    struct EXT2Inode parent_inode;
    read_inode(request.parent_inode, &parent_inode);
    uint32_t target_inode_idx = find_entry(&parent_inode, request.name, request.name_len, request.is_directory ? EXT2_FT_DIR : EXT2_FT_REG_FILE);
    if (target_inode_idx == 0) {
        return 1;
    }

    struct EXT2Inode target_inode;
    read_inode(target_inode_idx, &target_inode);
    if (request.is_directory && !is_directory_empty(target_inode_idx)) {
        return 2;
    }
    deallocate_node(target_inode_idx);
    
    return 0;
}
