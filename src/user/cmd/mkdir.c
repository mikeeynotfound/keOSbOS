#include "header/user/cmdlib.h"

// mkdir <dir> - create an empty directory
int main(int argc, char **argv) {
    if (argc < 2) {
        print_error("mkdir", NULL, "missing operand");
        return 1;
    }
    uint32_t    parent, name_len;
    const char *name;
    if (!split_path(argv[1], &parent, &name, &name_len)) {
        print_error("mkdir", argv[1], "invalid path");
        return 1;
    }

    struct EXT2DriverRequest req = {
        .buf          = NULL,
        .name         = (char*) name,
        .name_len     = name_len,
        .parent_inode = parent,
        .buffer_size  = 0,
        .is_directory = true,
    };
    int8_t ret = sys_write(&req);
    if (ret == 1)
        print_error("mkdir", argv[1], "already exists");
    else if (ret != 0)
        print_error("mkdir", argv[1], "failed (disk full?)");
    return ret == 0 ? 0 : 1;
}
