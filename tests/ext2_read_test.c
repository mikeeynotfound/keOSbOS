/* Host-side test for EXT2 read() / read_directory().
 * Replaces the ATA driver with a 4 MiB array and includes ext2.c directly to reach its static helpers.
 *
 * Run: gcc -g -Wall -Wextra -fsanitize=address,undefined -Isrc tests/ext2_read_test.c -o /tmp/ext2_read_test && /tmp/ext2_read_test
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

/* Build an entry the way write() will: put it into the free entry of the parent's first directory block */
static void add_entry(uint32_t parent, const char *name, uint8_t type, uint32_t inode) {
    struct EXT2Inode p; read_inode(parent, &p);
    struct BlockBuffer b; read_blocks(&b, p.i_block[0], 1);
    uint32_t off = get_dir_first_child_offset(b.buf);
    struct EXT2DirectoryEntry *free_e = get_directory_entry(b.buf, off), *e;
    while (free_e->inode != 0) { off += free_e->rec_len; free_e = get_directory_entry(b.buf, off); }
    uint16_t total = free_e->rec_len, need = get_entry_record_len((uint8_t) strlen(name));
    e = free_e;
    e->inode = inode; e->name_len = (uint16_t) strlen(name); e->file_type = type; e->rec_len = need;
    memcpy(get_entry_name(e), name, strlen(name));
    struct EXT2DirectoryEntry *rest = get_next_directory_entry(e);
    rest->inode = 0; rest->name_len = 0; rest->file_type = 0; rest->rec_len = (uint16_t) (total - need);
    write_blocks(&b, p.i_block[0], 1);
}

static uint32_t make_file(uint32_t parent, const char *name, const uint8_t *data, uint32_t size) {
    uint32_t inode = allocate_node();
    struct EXT2Inode n; memset(&n, 0, sizeof n);
    n.i_mode = EXT2_S_IFREG; n.i_size = size; n.i_blocks = (size + 511) / 512;
    uint8_t *padded = calloc(n.i_blocks + 1, 512);
    memcpy(padded, data, size);
    allocate_node_blocks(padded, &n, inode_to_bgd(inode));
    free(padded);
    sync_node(&n, inode);
    add_entry(parent, name, EXT2_FT_REG_FILE, inode);
    return inode;
}

static uint32_t make_dir(uint32_t parent, const char *name) {
    uint32_t inode = allocate_node();
    struct EXT2Inode n;
    init_directory_table(&n, inode, parent);
    add_entry(parent, name, EXT2_FT_DIR, inode);
    return inode;
}

static void pattern(uint8_t *p, uint32_t size, uint32_t seed) {
    for (uint32_t i = 0; i < size; i++) p[i] = (uint8_t) ((i * 31 + (i >> 9) * 17 + seed) & 0xFF);
    if (size) { p[0] = 0xAA; p[size - 1] = 0x55; }
    if (size > 2047) p[2047] = 0xEE;
}

static int8_t do_read(uint32_t parent, const char *name, void *buf, uint32_t bufsize) {
    struct EXT2DriverRequest r = { .buf = buf, .name = (char*) name, .name_len = (uint8_t) strlen(name),
                                   .parent_inode = parent, .buffer_size = bufsize, .is_directory = false };
    return read(r);
}
static int8_t do_readdir(uint32_t parent, const char *name, void *buf, uint32_t bufsize) {
    struct EXT2DriverRequest r = { .buf = buf, .name = (char*) name, .name_len = (uint8_t) strlen(name),
                                   .parent_inode = parent, .buffer_size = bufsize, .is_directory = true };
    return read_directory(&r);
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    initialize_filesystem_ext2();

    /* --- file contents, including block boundaries and every indirection level --- */
    uint32_t sizes[] = { 1, 2, 511, 512, 513, 1024, 2047, 2048, 2049, 6000, 12 * 512, 12 * 512 + 1,
                         140 * 512, 140 * 512 + 7, 400 * 512 - 3 };
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        uint32_t size = sizes[s];
        char name[16]; snprintf(name, sizeof name, "f%u", size);
        uint8_t *data = malloc(size), *out = malloc(size + 8);
        pattern(data, size, s);
        make_file(ROOT_INODE, name, data, size);

        memset(out, 0xCD, size + 8);
        assert(do_read(ROOT_INODE, name, out, size) == 0);
        assert(memcmp(out, data, size) == 0);
        for (int k = 0; k < 8; k++) assert(out[size + k] == 0xCD); // never writes past the file size

        memset(out, 0xCD, size + 8);
        assert(do_read(ROOT_INODE, name, out, size + 8) == 0);   // larger buffer is fine
        assert(memcmp(out, data, size) == 0 && out[size] == 0xCD);
        assert(do_read(ROOT_INODE, name, out, size - 1) == 2);   // too small
        free(data); free(out);
    }
    printf("file contents OK (%zu sizes)\n", sizeof sizes / sizeof *sizes);

    /* --- tree like the spec: folder1/{nestedf1/, tpazolite, xi}, folder2/, laur, sasakure --- */
    uint8_t d[700]; pattern(d, sizeof d, 9);
    uint32_t folder1 = make_dir(ROOT_INODE, "folder1");
    make_dir(ROOT_INODE, "folder2");
    uint32_t nested = make_dir(folder1, "nestedf1");
    make_file(folder1, "tpazolite", d, 700);
    make_file(folder1, "xi", d, 33);
    make_file(ROOT_INODE, "laur", d, 700);
    uint8_t out[1024];

    assert(do_read(folder1, "tpazolite", out, sizeof out) == 0 && !memcmp(out, d, 700));
    assert(do_read(folder1, "xi", out, sizeof out) == 0 && !memcmp(out, d, 33));
    assert(do_read(ROOT_INODE, "tpazolite", out, sizeof out) == 3);  // lives in folder1, not root
    assert(do_read(ROOT_INODE, "laur", out, sizeof out) == 0);
    assert(do_read(ROOT_INODE, "nothing", out, sizeof out) == 3);
    assert(do_read(ROOT_INODE, "folder1", out, sizeof out) == 1);    // a folder is not a file
    assert(do_read(ROOT_INODE, "..", out, sizeof out) == 1);
    printf("lookup / not found / not a file OK\n");

    /* --- invalid parent --- */
    struct EXT2Inode root; read_inode(ROOT_INODE, &root);
    uint32_t laur = find_entry(&root, "laur", 4, EXT2_FT_REG_FILE);
    assert(do_read(0, "laur", out, sizeof out) == 4);
    assert(do_read(100000, "laur", out, sizeof out) == 4);
    assert(do_read(500, "laur", out, sizeof out) == 4);              // in range but unused
    assert(do_read(laur, "x", out, sizeof out) == 4);                // a file cannot be a parent
    assert(do_readdir(0, "folder1", out, sizeof out) == 3);
    assert(do_readdir(laur, "x", out, sizeof out) == 3);
    assert(do_readdir(500, "x", out, sizeof out) == 3);
    printf("invalid parent OK\n");

    /* --- bad arguments --- */
    assert(do_read(ROOT_INODE, "laur", NULL, 100) == -1);
    assert(read((struct EXT2DriverRequest){ .buf = out, .name = NULL, .name_len = 4, .parent_inode = 1, .buffer_size = 700 }) == -1);
    assert(do_read(ROOT_INODE, "", out, sizeof out) == -1);
    assert(read_directory(NULL) == -1);

    /* --- read_directory --- */
    uint8_t dirbuf[1024]; memset(dirbuf, 0xCD, sizeof dirbuf);
    assert(do_readdir(ROOT_INODE, "folder1", dirbuf, sizeof dirbuf) == 0);
    struct EXT2Inode f1; read_inode(folder1, &f1);
    assert(!memcmp(dirbuf, disk + f1.i_block[0] * 512, 512));        // exact copy of its table
    assert(dirbuf[512] == 0xCD);
    struct EXT2DirectoryEntry *e = get_directory_entry(dirbuf, 0);
    assert(e->inode == folder1 && e->name_len == 1);                 // "."
    e = get_next_directory_entry(e);
    assert(e->inode == ROOT_INODE && e->name_len == 2);              // ".."
    int found = 0; uint32_t off = get_dir_first_child_offset(dirbuf);
    for (e = get_directory_entry(dirbuf, off); e->inode != 0; e = get_next_directory_entry(e)) found++;
    assert(found == 3);                                              // nestedf1, tpazolite, xi
    assert(do_readdir(folder1, "nestedf1", dirbuf, sizeof dirbuf) == 0);
    assert(is_directory_empty(nested));
    assert(do_readdir(folder1, "..", dirbuf, sizeof dirbuf) == 0);   // ".." resolves back to root
    assert(((struct EXT2DirectoryEntry*) dirbuf)->inode == ROOT_INODE);
    assert(do_readdir(ROOT_INODE, ".", dirbuf, sizeof dirbuf) == 0);
    assert(do_readdir(ROOT_INODE, "laur", dirbuf, sizeof dirbuf) == 1);   // file, not folder
    assert(do_readdir(ROOT_INODE, "missing", dirbuf, sizeof dirbuf) == 2);
    assert(do_readdir(ROOT_INODE, "folder1", dirbuf, 511) == -1);         // buffer too small
    printf("read_directory OK\n");

    /* --- same name used by a file and a folder --- */
    make_dir(ROOT_INODE, "dup");
    make_file(ROOT_INODE, "dup", d, 10);
    assert(do_read(ROOT_INODE, "dup", out, sizeof out) == 0 && !memcmp(out, d, 10));
    assert(do_readdir(ROOT_INODE, "dup", dirbuf, sizeof dirbuf) == 0);
    assert(find_entry(&root, "dup", 3, EXT2_FT_DIR) != find_entry(&root, "dup", 3, EXT2_FT_REG_FILE));
    // prefix names must not match
    assert(do_read(ROOT_INODE, "du", out, sizeof out) == 3 && do_read(ROOT_INODE, "dupx", out, sizeof out) == 3);
    printf("same-name file/folder OK\n");

    /* --- survives re-initialisation (no remount side effects) --- */
    initialize_filesystem_ext2();
    assert(do_read(folder1, "xi", out, sizeof out) == 0 && !memcmp(out, d, 33));
    puts("ALL OK");
    return 0;
}
