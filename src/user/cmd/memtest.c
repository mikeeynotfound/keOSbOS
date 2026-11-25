#include "header/user/cmdlib.h"

// memtest [rounds] - fill 384 KiB with a pattern, give the CPU away, check it again.
// Several memtest at once with a small SYSTEM_MEMORY_MB force pages to swap (Book 3.2).
#define MEMTEST_PAGES 96
#define WORDS_PER_PAGE 1024

static uint32_t data[MEMTEST_PAGES * WORDS_PER_PAGE];

static void out_uint(uint32_t value) {
    char     buf[10];
    uint32_t len = 0;
    do {
        buf[len++] = (char) ('0' + value % 10);
        value /= 10;
    } while (value);
    while (len--)
        out_putchar(buf[len], COLOR_TEXT);
}

static uint32_t pattern(uint32_t pid, uint32_t round, uint32_t i) {
    return (pid * 2654435761u) ^ (round * 40503u) ^ (i * 2246822519u);
}

int main(int argc, char **argv) {
    uint32_t rounds = 10;
    if (argc >= 2) {
        rounds = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            rounds = rounds * 10 + (uint32_t) (*p - '0');
        if (rounds == 0)
            rounds = 1;
    }

    uint32_t pid = sys_get_pid();
    for (uint32_t r = 0; r < rounds; r++) {
        for (uint32_t i = 0; i < MEMTEST_PAGES * WORDS_PER_PAGE; i++)
            data[i] = pattern(pid, r, i);
        sys_yield();                               // let other processes take frames meanwhile
        for (uint32_t i = 0; i < MEMTEST_PAGES * WORDS_PER_PAGE; i++) {
            if (data[i] != pattern(pid, r, i)) {
                out_print("memtest ", COLOR_ERROR);
                out_uint(pid);
                out_print(": FAIL at word ", COLOR_ERROR);
                out_uint(i);
                out_putchar('\n', COLOR_ERROR);
                return 1;
            }
        }
    }
    out_print("memtest ", COLOR_TEXT);
    out_uint(pid);
    out_print(": OK, ", COLOR_TEXT);
    out_uint(MEMTEST_PAGES * 4);
    out_print(" KiB checked ", COLOR_TEXT);
    out_uint(rounds);
    out_print(" times\n", COLOR_TEXT);
    return 0;
}
