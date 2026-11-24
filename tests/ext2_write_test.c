/* Host-side test for EXT2 write(), verified through read() / read_directory().
 * Replaces the ATA driver with a 4 MiB array and includes ext2.c directly to reach its static helpers.
 *
 * Run: gcc -g -Wall -Wextra -fsanitize=address,undefined -Isrc tests/ext2_write_test.c -o /tmp/ext2_write_test && /tmp/ext2_write_test
 */
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
void read_blocks(void *p, uint32_t lba, uint8_t n) { assert((lba + n) * 512u <= sizeof disk); memcpy(p, disk + lba * 512, n * 512); }
void write_blocks(const void *p, uint32_t lba, uint8_t n) { assert((lba + n) * 512u <= sizeof disk); memcpy(disk + lba * 512, p, n * 512); }

static int checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void pattern(uint8_t *p, uint32_t size, uint32_t seed) {
    for (uint32_t i = 0; i < size; i++) p[i] = (uint8_t) ((i * 31 + (i >> 9) * 17 + seed) & 0xFF);
}

/* exact-size heap buffers so ASan flags any read/write past the end */
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

static uint32_t lookup(uint32_t parent, const char *name, uint8_t type) {
    struct EXT2Inode p; read_inode(parent, &p);
    return find_entry(&p, name, (uint8_t) strlen(name), type);
}

static bool file_matches(uint32_t parent, const char *name, uint32_t size, uint32_t seed) {
    uint8_t *want = malloc(size), *got = malloc(size);
    pattern(want, size, seed);
    bool ok = do_read(parent, name, got, size) == 0 && memcmp(want, got, size) == 0;
    free(want); free(got);
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
        free_blocks += fb; free_inodes += fi;
    }
    CHECK(free_blocks == sb.s_free_blocks_count);
    CHECK(free_inodes == sb.s_free_inodes_count);

    for (uint32_t ino = 1; ino <= sb.s_inodes_count; ino++) {
        if (!is_inode_in_use(ino)) continue;
        struct EXT2Inode n; read_inode(ino, &n);
        if (!(n.i_mode & EXT2_S_IFDIR)) continue;
        for (uint32_t i = 0; i < n.i_blocks; i++) {
            struct BlockBuffer b; read_blocks(&b, get_block_pointer(&n, i), 1);
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

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    initialize_filesystem_ext2();

    /* --- files of many sizes, read back byte for byte --- */
    uint32_t sizes[] = { 1, 100, 511, 512, 513, 1024, 2047, 2048, 2049, 6144, 6145, 70000, 400 * 512 - 3 };
    char name[32];
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        uint8_t *d = malloc(sizes[i]); pattern(d, sizes[i], i);
        snprintf(name, sizeof name, "f%u", i);
        CHECK(do_write(ROOT_INODE, name, d, sizes[i]) == 0);
        free(d);
        CHECK(file_matches(ROOT_INODE, name, sizes[i], i));
        check_consistency();
    }
    /* size recorded and i_blocks correct */
    struct EXT2Inode n;
    read_inode(lookup(ROOT_INODE, "f4", EXT2_FT_REG_FILE), &n);
    CHECK(n.i_size == 513 && n.i_blocks == 2 && (n.i_mode & EXT2_S_IFREG));

    /* --- folders, nesting, "." and ".." --- */
    CHECK(do_mkdir(ROOT_INODE, "folder1") == 0);
    uint32_t f1 = lookup(ROOT_INODE, "folder1", EXT2_FT_DIR);
    CHECK(f1 != 0);
    CHECK(is_directory_empty(f1));
    CHECK(do_mkdir(f1, "nested") == 0);
    CHECK(!is_directory_empty(f1));
    uint32_t nested = lookup(f1, "nested", EXT2_FT_DIR);
    CHECK(lookup(nested, "..", EXT2_FT_DIR) == f1);
    CHECK(lookup(nested, ".", EXT2_FT_DIR) == nested);
    uint8_t data[300]; pattern(data, sizeof data, 9);
    CHECK(do_write(nested, "deep", data, sizeof data) == 0);
    CHECK(file_matches(nested, "deep", sizeof data, 9));
    CHECK(lookup(ROOT_INODE, "deep", EXT2_FT_UNKNOWN) == 0); // not leaked into another directory
    check_consistency();

    /* --- error codes --- */
    CHECK(do_mkdir(ROOT_INODE, "folder1") == 1);
    CHECK(do_write(ROOT_INODE, "f0", data, sizeof data) == 1);
    CHECK(do_mkdir(0, "x") == 2);
    CHECK(do_mkdir(9999, "x") == 2);
    CHECK(do_mkdir(50, "x") == 2);                                  // unused inode
    CHECK(do_mkdir(lookup(ROOT_INODE, "f0", EXT2_FT_REG_FILE), "x") == 2); // a file is not a parent
    struct EXT2DriverRequest bad = { .buf = data, .name = NULL, .name_len = 1, .parent_inode = 1, .buffer_size = 1 };
    CHECK(write(NULL) == -1);
    CHECK(write(&bad) == -1);
    bad.name = "a"; bad.name_len = 0;
    CHECK(write(&bad) == -1);
    bad.name_len = 1; bad.buf = NULL; bad.buffer_size = 10;
    CHECK(write(&bad) == -1);

    /* same name is allowed once as file and once as folder */
    CHECK(do_write(ROOT_INODE, "twin", data, sizeof data) == 0);
    CHECK(do_mkdir(ROOT_INODE, "twin") == 0);
    CHECK(do_write(ROOT_INODE, "twin", data, sizeof data) == 1);
    CHECK(do_mkdir(ROOT_INODE, "twin") == 1);
    CHECK(lookup(ROOT_INODE, "twin", EXT2_FT_REG_FILE) != lookup(ROOT_INODE, "twin", EXT2_FT_DIR));
    /* prefixes are different names */
    CHECK(do_write(ROOT_INODE, "tw", data, sizeof data) == 0);
    CHECK(do_write(ROOT_INODE, "twinn", data, sizeof data) == 0);
    check_consistency();

    /* --- long names, and a directory that grows past one block, then past the direct pointers --- */
    char longname[256]; memset(longname, 'n', 255); longname[255] = 0;
    CHECK(do_write(f1, longname, data, sizeof data) == 0);
    CHECK(file_matches(f1, longname, sizeof data, 9));
    CHECK(do_mkdir(ROOT_INODE, "big") == 0);
    uint32_t big = lookup(ROOT_INODE, "big", EXT2_FT_DIR);
    for (int i = 0; i < 700; i++) {
        snprintf(name, sizeof name, "file_%d", i);
        uint8_t b = (uint8_t) i;
        CHECK(do_write(big, name, &b, 1) == 0);
    }
    read_inode(big, &n);
    CHECK(n.i_blocks > DIRECT_BLOCKS && n.i_size == n.i_blocks * BLOCK_SIZE);
    for (int i = 0; i < 700; i++) {
        snprintf(name, sizeof name, "file_%d", i);
        uint8_t b = 0xFF;
        CHECK(do_read(big, name, &b, 1) == 0 && b == (uint8_t) i);
    }
    check_consistency();

    /* --- state survives a re-init (metadata really written to disk) --- */
    uint32_t free_before = sb.s_free_blocks_count;
    memset(&sb, 0, sizeof sb); memset(&bgd_table, 0, sizeof bgd_table);
    initialize_filesystem_ext2();
    CHECK(sb.s_free_blocks_count == free_before);
    CHECK(file_matches(ROOT_INODE, "f12", 400 * 512 - 3, 12));
    CHECK(file_matches(nested, "deep", sizeof data, 9));
    check_consistency();

    /* --- disk full: failed write changes nothing --- */
    int8_t r = 0; int count = 0;
    uint8_t *chunk = malloc(100000); pattern(chunk, 100000, 3);
    while (r == 0) { snprintf(name, sizeof name, "fill%d", count++); r = do_write(ROOT_INODE, name, chunk, 100000); check_consistency(); }
    CHECK(r == -1);
    uint32_t free_full = sb.s_free_blocks_count, free_inodes_full = sb.s_free_inodes_count;
    CHECK(do_write(ROOT_INODE, "nofit", chunk, 100000) == -1);
    CHECK(lookup(ROOT_INODE, "nofit", EXT2_FT_UNKNOWN) == 0);
    CHECK(sb.s_free_blocks_count == free_full && sb.s_free_inodes_count == free_inodes_full);
    check_consistency();
    CHECK(file_matches(ROOT_INODE, "f12", 400 * 512 - 3, 12));
    /* a small file may still fit in the remaining blocks */
    if (free_full > 3) CHECK(do_write(ROOT_INODE, "tiny", data, 10) == 0);
    free(chunk);

    printf("ALL OK (%d checks)\n", checks);
    return 0;
}
