#include "header/user/cmdlib.h"

// mv <src> <dst> - move or rename a file (copy, then remove the source)
int main(int argc, char **argv) {
    if (argc < 3) {
        print_error("mv", NULL, "usage: mv <src> <dst>");
        return 1;
    }
    if (!copy_file("mv", argv[1], argv[2]))
        return 1;
    if (remove_path(argv[1], false) != 0) {
        print_error("mv", argv[1], "copied but failed to remove source");
        return 1;
    }
    return 0;
}
