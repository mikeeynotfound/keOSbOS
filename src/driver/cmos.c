#include "header/driver/cmos.h"
#include "header/cpu/portio.h"
#include <stdbool.h>

static uint8_t cmos_read_register(uint8_t reg) {
    out(CMOS_ADDRESS_PORT, reg);
    return in(CMOS_DATA_PORT);
}

// Bit 7 of status register A is set while the RTC is updating its values
static bool cmos_update_in_progress(void) {
    return cmos_read_register(CMOS_REG_STATUS_A) & 0x80;
}

static void cmos_read_raw(struct RTCTime *t) {
    while (cmos_update_in_progress());
    t->second = cmos_read_register(CMOS_REG_SECOND);
    t->minute = cmos_read_register(CMOS_REG_MINUTE);
    t->hour   = cmos_read_register(CMOS_REG_HOUR);
    t->day    = cmos_read_register(CMOS_REG_DAY);
    t->month  = cmos_read_register(CMOS_REG_MONTH);
    t->year   = cmos_read_register(CMOS_REG_YEAR);
}

static bool cmos_time_equal(struct RTCTime *a, struct RTCTime *b) {
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year;
}

static uint8_t bcd_to_binary(uint8_t value) {
    return (value & 0x0F) + ((value >> 4) * 10);
}

void cmos_read_rtc(struct RTCTime *out_time) {
    struct RTCTime prev;

    // Read twice until both results match, so we never get a half-updated value
    cmos_read_raw(out_time);
    do {
        prev = *out_time;
        cmos_read_raw(out_time);
    } while (!cmos_time_equal(&prev, out_time));

    uint8_t status_b = cmos_read_register(CMOS_REG_STATUS_B);

    // Bit 2 of status B clear: values are in BCD
    if (!(status_b & 0x04)) {
        out_time->second = bcd_to_binary(out_time->second);
        out_time->minute = bcd_to_binary(out_time->minute);
        out_time->hour   = bcd_to_binary(out_time->hour & 0x7F) | (out_time->hour & 0x80);
        out_time->day    = bcd_to_binary(out_time->day);
        out_time->month  = bcd_to_binary(out_time->month);
        out_time->year   = bcd_to_binary(out_time->year);
    }

    // Bit 1 of status B clear: 12-hour format, bit 7 of hour is the PM flag
    if (!(status_b & 0x02) && (out_time->hour & 0x80))
        out_time->hour = ((out_time->hour & 0x7F) + 12) % 24;

    out_time->year += 2000;
}
