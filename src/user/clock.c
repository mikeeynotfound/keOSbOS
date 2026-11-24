#include <stdint.h>
#include <stdbool.h>
#include "header/user/syscall.h"

#define CLOCK_ROW         24     // last row of the screen
#define CLOCK_COL         72     // 80 - strlen("HH:MM:SS")
#define CLOCK_COLOR       0xE    // yellow
#define TIMEZONE_OFFSET   7      // WIB = UTC+7, QEMU RTC runs in UTC by default

static void write_two_digit(char *buf, uint8_t value) {
    buf[0] = '0' + value / 10;
    buf[1] = '0' + value % 10;
}

int main(void) {
    struct RTCTime time;
    uint8_t last_second = 0xFF;

    while (true) {
        sys_get_time(&time);
        if (time.second == last_second) {
            sys_yield(); // let the shell and other processes run between redraws
            continue;
        }
        last_second = time.second;

        uint8_t hour = (time.hour + TIMEZONE_OFFSET) % 24;

        char buf[8];
        write_two_digit(&buf[0], hour);
        buf[2] = ':';
        write_two_digit(&buf[3], time.minute);
        buf[5] = ':';
        write_two_digit(&buf[6], time.second);

        for (int i = 0; i < 8; i++)
            sys_putchar_at(buf[i], CLOCK_ROW, CLOCK_COL + i, CLOCK_COLOR);
    }
    return 0;
}
