#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define read   ext2_read   // avoid clashing with libc read()/write()
#define write  ext2_write
#define delete ext2_delete
#include "header/filesystem/ext2.h"
#include "filesystem/ext2.c"

static uint8_t disk[DISK_SPACE];
void read_blocks(void *p, uint32_t lba, uint8_t n) {
    assert((lba + n) * 512u <= sizeof disk);
    memcpy(p, disk + lba * 512, n * 512);
}
void write_blocks(const void *p, uint32_t lba, uint8_t n) {
    assert((lba + n) * 512u <= sizeof disk);
    memcpy(disk + lba * 512, p, n * 512);
}

static int checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void pattern(uint8_t *p, uint32_t size, uint32_t seed) {
    for (uint32_t i = 0; i < size; i++)
        p[i] = (uint8_t) ((i * 31 + (i >> 9) * 17 + seed) & 0xFF);
}

static int8_t do_write(uint32_t parent, const char *name, const uint8_t *data, uint32_t size) {
    uint8_t *copy = size ? malloc(size) : NULL;
    if (size) memcpy(copy, data, size);
    struct EXT2DriverRequest r = { .buf = copy, .name = (char*) name, .name_len = (uint8_t) strlen(name),
                                   .parent_inode = parent, .buffer_size = size, .is_directory = size == 0 };
    int8_t ret = write(&r);
    free(copy);
    return ret;
}
static int8_t do_mkdir(uint32_t parent, const char *name) { return do_write(parent, name, NULL, 0); }

static int8_t do_read(uint32_t parent, const char *name, void *buf, uint32_t bufsize) {
    struct EXT2DriverRequest r = { .buf = buf, .name = (char*) name, .name_len = (uint8_t) strlen(name),
                                   .parent_inode = parent, .buffer_size = bufsize, .is_directory = false };
    return read(r);
}

static int8_t do_delete(uint32_t parent, const char *name, bool is_directory) {
    struct EXT2DriverRequest r = { .buf = NULL, .name = (char*) name, .name_len = (uint8_t) strlen(name),
                                   .parent_inode = parent, .buffer_size = 0, .is_directory = is_directory };
    return delete(r);
}

static uint32_t lookup(uint32_t parent, const char *name, uint8_t type) {
    struct EXT2Inode p;
    read_inode(parent, &p);
    return find_entry(&p, name, (uint8_t) strlen(name), type);
}

static bool file_matches(uint32_t parent, const char *name, uint32_t size, uint32_t seed) {
    uint8_t *want = malloc(size), *got = malloc(size);
    pattern(want, size, seed);
    bool ok = do_read(parent, name, got, size) == 0 && memcmp(want, got, size) == 0;
    free(want);
    free(got);
    return ok;
}

/* Bitmaps, group counters and superblock counters must agree; every directory block must tile exactly */
static void check_consistency(void) {
    uint32_t free_blocks = 0, free_inodes = 0;
    struct BlockBuffer bm;
    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        uint32_t fb = 0, fi = 0;
        read_blocks(&bm, bgd_table.table[g].bg_block_bitmap, 1);
        for (uint32_t i = 0; i < BLOCKS_PER_GROUP; i++) if (!bit_is_set(bm.buf, i)) fb++;
        read_blocks(&bm, bgd_table.table[g].bg_inode_bitmap, 1);
        for (uint32_t i = 0; i < INODES_PER_GROUP; i++) if (!bit_is_set(bm.buf, i)) fi++;
        CHECK(fb == bgd_table.table[g].bg_free_blocks_count);
        CHECK(fi == bgd_table.table[g].bg_free_inodes_count);
        free_blocks += fb;
        free_inodes += fi;
    }
    CHECK(free_blocks == sb.s_free_blocks_count);
    CHECK(free_inodes == sb.s_free_inodes_count);

    for (uint32_t ino = 1; ino <= sb.s_inodes_count; ino++) {
        if (!is_inode_in_use(ino)) continue;
        struct EXT2Inode n;
        read_inode(ino, &n);
        if (!(n.i_mode & EXT2_S_IFDIR)) continue;
        for (uint32_t i = 0; i < n.i_blocks; i++) {
            struct BlockBuffer b;
            read_blocks(&b, get_block_pointer(&n, i), 1);
            uint32_t off = 0;
            while (off < BLOCK_SIZE) {
                struct EXT2DirectoryEntry *e = get_directory_entry(b.buf, off);
                CHECK(e->rec_len >= 12 && e->rec_len % 4 == 0);
                if (e->inode) CHECK(e->rec_len >= get_entry_record_len((uint8_t) e->name_len));
                off += e->rec_len;
            }
            CHECK(off == BLOCK_SIZE);
        }
    }
}

typedef uint8_t BitmapSet[GROUPS_COUNT][BLOCK_SIZE];

static void save_bitmaps(BitmapSet blocks, BitmapSet inodes) {
    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        read_blocks(blocks[g], bgd_table.table[g].bg_block_bitmap, 1);
        read_blocks(inodes[g], bgd_table.table[g].bg_inode_bitmap, 1);
    }
}

static bool bitmaps_equal(const BitmapSet a_blocks, const BitmapSet a_inodes,
                          const BitmapSet b_blocks, const BitmapSet b_inodes) {
    return memcmp(a_blocks, b_blocks, sizeof(BitmapSet)) == 0
        && memcmp(a_inodes, b_inodes, sizeof(BitmapSet)) == 0;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    initialize_filesystem_ext2();

    const uint32_t base_free_blocks = sb.s_free_blocks_count;
    const uint32_t base_free_inodes = sb.s_free_inodes_count;
    const uint16_t base_used_dirs = bgd_table.table[inode_to_bgd(ROOT_INODE)].bg_used_dirs_count;
    uint8_t data[300];
    pattern(data, sizeof data, 42);

    /* --- deleting a large file must unlink entry, free inode and every data/pointer block --- */
    BitmapSet blocks_before, inodes_before;
    save_bitmaps(blocks_before, inodes_before);
    uint32_t big_size = (DIRECT_BLOCKS + POINTERS_PER_BLOCK + 50u) * BLOCK_SIZE + 17u;
    uint8_t *big = malloc(big_size);
    pattern(big, big_size, 7);
    CHECK(do_write(ROOT_INODE, "large", big, big_size) == 0);
    free(big);
    CHECK(file_matches(ROOT_INODE, "large", big_size, 7));
    uint32_t large_ino = lookup(ROOT_INODE, "large", EXT2_FT_REG_FILE);
    CHECK(large_ino != 0);
    CHECK(is_inode_in_use(large_ino));
    CHECK(sb.s_free_blocks_count < base_free_blocks);
    CHECK(sb.s_free_inodes_count == base_free_inodes - 1);
    check_consistency();

    CHECK(do_delete(ROOT_INODE, "large", false) == 0);
    CHECK(lookup(ROOT_INODE, "large", EXT2_FT_UNKNOWN) == 0);
    uint8_t dummy;
    CHECK(do_read(ROOT_INODE, "large", &dummy, 1) == 3);
    CHECK(!is_inode_in_use(large_ino));
    CHECK(sb.s_free_blocks_count == base_free_blocks);
    CHECK(sb.s_free_inodes_count == base_free_inodes);
    /* The allocation bitmaps must be exactly back to their pre-write state. */
    BitmapSet blocks_after, inodes_after;
    save_bitmaps(blocks_after, inodes_after);
    CHECK(bitmaps_equal(blocks_after, inodes_after, blocks_before, inodes_before));
    check_consistency();

    /* --- deleting a freed inode again is a no-op (no double free / counter corruption) --- */
    CHECK(do_delete(ROOT_INODE, "large", false) == 1);
    deallocate_node(large_ino);
    CHECK(sb.s_free_blocks_count == base_free_blocks && sb.s_free_inodes_count == base_free_inodes);
    check_consistency();

    /* --- small files, repeated create/delete reuses resources without leaking --- */
    for (int i = 0; i < 8; i++) {
        CHECK(do_write(ROOT_INODE, "cycle", data, sizeof data) == 0);
        CHECK(file_matches(ROOT_INODE, "cycle", sizeof data, 42));
        CHECK(do_delete(ROOT_INODE, "cycle", false) == 0);
        CHECK(lookup(ROOT_INODE, "cycle", EXT2_FT_UNKNOWN) == 0);
        CHECK(sb.s_free_blocks_count == base_free_blocks && sb.s_free_inodes_count == base_free_inodes);
        check_consistency();
    }

    /* --- type matters: a folder request must not delete a file with the same name --- */
    CHECK(do_write(ROOT_INODE, "twin", data, sizeof data) == 0);
    CHECK(do_delete(ROOT_INODE, "twin", true) == 1);
    CHECK(file_matches(ROOT_INODE, "twin", sizeof data, 42));
    CHECK(do_mkdir(ROOT_INODE, "twin") == 0);
    CHECK(do_delete(ROOT_INODE, "twin", false) == 0);
    CHECK(lookup(ROOT_INODE, "twin", EXT2_FT_REG_FILE) == 0);
    CHECK(lookup(ROOT_INODE, "twin", EXT2_FT_DIR) != 0);
    CHECK(do_delete(ROOT_INODE, "twin", true) == 0);
    CHECK(sb.s_free_blocks_count == base_free_blocks && sb.s_free_inodes_count == base_free_inodes);
    check_consistency();

    /* --- empty vs non-empty directory --- */
    CHECK(do_mkdir(ROOT_INODE, "dir") == 0);
    uint32_t dir = lookup(ROOT_INODE, "dir", EXT2_FT_DIR);
    CHECK(dir != 0);
    CHECK(bgd_table.table[inode_to_bgd(dir)].bg_used_dirs_count == base_used_dirs + 1);
    CHECK(is_directory_empty(dir));
    CHECK(do_delete(ROOT_INODE, "dir", true) == 0);
    CHECK(lookup(ROOT_INODE, "dir", EXT2_FT_UNKNOWN) == 0);
    CHECK(bgd_table.table[inode_to_bgd(dir)].bg_used_dirs_count == base_used_dirs);

    CHECK(do_mkdir(ROOT_INODE, "dir") == 0);
    dir = lookup(ROOT_INODE, "dir", EXT2_FT_DIR);
    CHECK(do_write(dir, "child", data, sizeof data) == 0);
    CHECK(!is_directory_empty(dir));
    CHECK(do_delete(ROOT_INODE, "dir", true) == 2);
    CHECK(lookup(ROOT_INODE, "dir", EXT2_FT_DIR) == dir);
    CHECK(file_matches(dir, "child", sizeof data, 42));
    CHECK(do_delete(dir, "child", false) == 0);
    CHECK(is_directory_empty(dir));
    CHECK(do_delete(ROOT_INODE, "dir", true) == 0);
    CHECK(lookup(ROOT_INODE, "dir", EXT2_FT_UNKNOWN) == 0);
    CHECK(bgd_table.table[inode_to_bgd(dir)].bg_used_dirs_count == base_used_dirs);
    CHECK(sb.s_free_blocks_count == base_free_blocks && sb.s_free_inodes_count == base_free_inodes);
    check_consistency();

    /* --- entries scattered over a multi-block / indirect directory are unlinked correctly --- */
    CHECK(do_mkdir(ROOT_INODE, "many") == 0);
    uint32_t many = lookup(ROOT_INODE, "many", EXT2_FT_DIR);
    char name[32];
    for (int i = 0; i < 700; i++) {
        snprintf(name, sizeof name, "file_%d", i);
        uint8_t byte = (uint8_t) i;
        CHECK(do_write(many, name, &byte, 1) == 0);
    }
    struct EXT2Inode many_node;
    read_inode(many, &many_node);
    CHECK(many_node.i_blocks > DIRECT_BLOCKS); /* directory reached its indirect block */
    check_consistency();
    for (int i = 0; i < 700; i++) {
        snprintf(name, sizeof name, "file_%d", i);
        CHECK(do_delete(many, name, false) == 0);
        CHECK(lookup(many, name, EXT2_FT_UNKNOWN) == 0);
    }
    CHECK(is_directory_empty(many));
    check_consistency();
    CHECK(do_delete(ROOT_INODE, "many", true) == 0);
    CHECK(sb.s_free_blocks_count == base_free_blocks && sb.s_free_inodes_count == base_free_inodes);
    check_consistency();

    /* --- error codes and safety rails --- */
    CHECK(do_delete(ROOT_INODE, "does_not_exist", false) == 1);
    CHECK(do_delete(0, "x", false) == 3);
    CHECK(do_delete(9999, "x", false) == 3);
    CHECK(do_delete(50, "x", false) == 3); /* in range but never allocated */
    CHECK(do_write(ROOT_INODE, "plain", data, sizeof data) == 0);
    uint32_t plain = lookup(ROOT_INODE, "plain", EXT2_FT_REG_FILE);
    CHECK(do_delete(plain, "x", false) == 3); /* a file cannot be a parent */
    CHECK(do_delete(ROOT_INODE, ".", true) == 1);
    CHECK(do_delete(ROOT_INODE, "..", true) == 1);
    CHECK(lookup(ROOT_INODE, ".", EXT2_FT_DIR) == ROOT_INODE);
    CHECK(lookup(ROOT_INODE, "..", EXT2_FT_DIR) == ROOT_INODE);
    struct EXT2DriverRequest bad = { .buf = NULL, .name = NULL, .name_len = 1,
                                     .parent_inode = ROOT_INODE, .buffer_size = 0, .is_directory = false };
    CHECK(delete(bad) == -1);
    bad.name = "x";
    bad.name_len = 0;
    CHECK(delete(bad) == -1);
    check_consistency();

    /* --- deleted state survives a filesystem re-init; live data is untouched --- */
    CHECK(do_mkdir(ROOT_INODE, "keep") == 0);
    uint32_t keep = lookup(ROOT_INODE, "keep", EXT2_FT_DIR);
    CHECK(do_write(keep, "inside", data, sizeof data) == 0);
    CHECK(do_write(ROOT_INODE, "gone", data, sizeof data) == 0);
    CHECK(do_delete(ROOT_INODE, "gone", false) == 0);
    uint32_t free_before_reinit = sb.s_free_blocks_count;
    uint32_t inodes_before_reinit = sb.s_free_inodes_count;
    memset(&sb, 0, sizeof sb);
    memset(&bgd_table, 0, sizeof bgd_table);
    initialize_filesystem_ext2();
    CHECK(sb.s_free_blocks_count == free_before_reinit);
    CHECK(sb.s_free_inodes_count == inodes_before_reinit);
    CHECK(lookup(ROOT_INODE, "gone", EXT2_FT_UNKNOWN) == 0);
    CHECK(file_matches(keep, "inside", sizeof data, 42));
    CHECK(file_matches(ROOT_INODE, "plain", sizeof data, 42));
    check_consistency();

    /* --- same name can be reused after deletion --- */
    CHECK(do_delete(ROOT_INODE, "plain", false) == 0);
    uint8_t data2[64];
    pattern(data2, sizeof data2, 99);
    CHECK(do_write(ROOT_INODE, "plain", data2, sizeof data2) == 0);
    CHECK(file_matches(ROOT_INODE, "plain", sizeof data2, 99));
    CHECK(sb.s_free_blocks_count == free_before_reinit && sb.s_free_inodes_count == inodes_before_reinit);
    check_consistency();

    printf("ALL OK (%d checks)\n", checks);
    return 0;
}
