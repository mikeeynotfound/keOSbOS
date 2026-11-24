#ifndef _CMOS_H
#define _CMOS_H

#include <stdint.h>

// CMOS / RTC I/O ports
#define CMOS_ADDRESS_PORT 0x70
#define CMOS_DATA_PORT    0x71

// CMOS RTC registers
#define CMOS_REG_SECOND   0x00
#define CMOS_REG_MINUTE   0x02
#define CMOS_REG_HOUR     0x04
#define CMOS_REG_DAY      0x07
#define CMOS_REG_MONTH    0x08
#define CMOS_REG_YEAR     0x09
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B

/**
 * Date and time read from the CMOS real-time clock.
 * Shared by the kernel driver and user programs (SYSCALL_GET_TIME).
 */
struct RTCTime {
    uint8_t  second;
    uint8_t  minute;
    uint8_t  hour;
    uint8_t  day;
    uint8_t  month;
    uint16_t year;
} __attribute__((packed));

/**
 * Read current date and time from CMOS RTC (binary, 24-hour format).
 *
 * @param out_time Destination of the time value
 */
void cmos_read_rtc(struct RTCTime *out_time);

#endif
