/*
 * The real-time clock: a Seiko S-35180 on three lines of the ARM7's
 * register at 0x04000138 (GBATEK, "DS Real-Time Clock (RTC)").
 *
 *   bit 0  data (SIO)      bit 4  data direction: 1 = the DS drives the line
 *   bit 1  clock (/SCK)    bit 5  clock direction
 *   bit 2  select (CS)     bit 6  select direction
 *   bit 3  an unused line  bit 7  its direction
 *
 * The program bit-bangs the chip's serial protocol through it: see
 * nds_rtc.c. The register is 16 bits wide; the upper byte is four more
 * unused lines and their directions, which the machine keeps itself.
 *
 * The time comes from the caller. `now` is seconds since 1970-01-01 00:00
 * in the clock's local time, so that 0 reads back as midnight on 1 January
 * 1970 whatever the time zone: the machine supplies emulated time, never
 * the host's clock, in headless runs. The chip counts 2000 to 2099. When a
 * program sets the date or time the difference from `now` is kept, so the
 * clock goes on from what was set.
 *
 * The chip's interrupt output, /INT, is one pin for both of its interrupts
 * and goes to the SIO unit's SI input, which raises ARM7 interrupt 7 when
 * the pin falls and RCNT allows it (GBATEK, "Interrupt"). The pin is
 * reported as a level: nds_rtc_interrupt() is true while the chip holds it
 * low, and nds_rtc_write() and nds_rtc_update() return true when a call
 * changed it. What drives it is described in nds_rtc.c.
 */
#ifndef NDS_RTC_H
#define NDS_RTC_H

#include "nds_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_RTC_ADDRESS 0x04000138u

#define NDS_RTC_DATA 0x01u
#define NDS_RTC_CLOCK 0x02u
#define NDS_RTC_SELECT 0x04u
#define NDS_RTC_DATA_OUT 0x10u
#define NDS_RTC_CLOCK_OUT 0x20u
#define NDS_RTC_SELECT_OUT 0x40u

/* Plain data. Everything a program can set (the clock, the 12/24-hour
 * mode, the alarms, the free register) is in here: the chip has its own
 * battery, so a machine that keeps anything between runs keeps this. */
typedef struct NdsRtc {
    int64_t offset;     /* seconds the clock is ahead of `now`, modulo a century */
    int64_t seen;       /* `now` when the interrupt output was last worked out */
    int32_t week_offset; /* the same for the day-of-week counter, modulo a week */
    uint8_t started;    /* seen is valid */

    /* The port and the three lines. */
    uint8_t port;       /* the register as written */
    uint8_t pins;       /* levels: NDS_RTC_DATA, NDS_RTC_CLOCK, NDS_RTC_SELECT */

    /* The serial transfer. */
    uint8_t state;
    uint8_t command;
    uint8_t shift;
    uint8_t bits;
    uint8_t index;
    uint8_t length;
    uint8_t buffer[7];

    /* The chip's registers. */
    uint8_t status1;    /* bits 1-3 as written, bits 4-5 the interrupt flags */
    uint8_t status2;
    uint8_t alarm1[3];  /* day of week, hour, minute; [2] is also the frequency register */
    uint8_t alarm2[3];
    uint8_t adjust;
    uint8_t free_register;
    uint8_t lines;      /* bit 0: interrupt 1 holds /INT low; bit 1: interrupt 2 does */
} NdsRtc;

/* The state the firmware leaves the chip in: 24-hour mode, no interrupts,
 * the clock showing `now` and the day of the week that goes with the date
 * (0 = Sunday). */
void nds_rtc_reset(NdsRtc *rtc);

/* The low byte of 0x04000138. */
uint8_t nds_rtc_read(const NdsRtc *rtc);

/* Returns true if the write changed the level of the interrupt line. */
bool nds_rtc_write(NdsRtc *rtc, uint8_t value, int64_t now);

/*
 * The interrupt line follows the clock, not only the program: an alarm
 * comes due between writes. Call this when emulated time has moved on (once
 * a second is enough; a machine that does not care about the clock's
 * interrupts need not call it at all). Returns true if the level changed.
 */
bool nds_rtc_update(NdsRtc *rtc, int64_t now);

/* True while the chip holds /INT low. */
bool nds_rtc_interrupt(const NdsRtc *rtc);

#ifdef __cplusplus
}
#endif

#endif /* NDS_RTC_H */
