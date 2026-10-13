/*
 * The real-time clock, a Seiko S-35180 (see nds_rtc.h; GBATEK, "DS
 * Real-Time Clock (RTC)"). It is the same family as the clock on Game Boy
 * Advance cartridges, and this follows runtime-gba/src/gba_rtc.c.
 *
 * The serial protocol. The program raises CS with /SCK high, then clocks
 * bits: it sets /SCK low together with the data bit, then /SCK high. The
 * chip takes a bit from the program on the rising edge. When the chip is
 * the one sending, it puts each bit on SIO at the falling edge and holds it
 * until the next, so a program may sample before or after it raises the
 * clock. CS low ends the transfer.
 *
 * The first eight bits are the command. On the wire they are, in order:
 *
 *   0 1 1 0   a fixed code       c c c   the register   r   1 = read
 *
 * Seiko's datasheet writes that as the byte 0110cccr sent most significant
 * bit first; GBATEK's "Fwd" numbering is the same eight bits read as a byte
 * sent least significant bit first, which is why its register numbers are
 * Seiko's with the three bits reversed. Registers, in Seiko's numbering
 * (GBATEK's "Rev" column), with the bytes that follow, each sent least
 * significant bit first:
 *
 *   0  status register 1    1 byte
 *   1  status register 2    1 byte
 *   2  date and time        7 bytes: year, month, day, day of week, hour,
 *                           minute, second
 *   3  time                 3 bytes: hour, minute, second
 *   4  interrupt 1          3 bytes (alarm 1: day of week, hour, minute)
 *                           when status register 2 bit 2 is set, else 1
 *                           byte (frequency duty), which is the same
 *                           register as the alarm's minute
 *   5  interrupt 2          3 bytes: alarm 2
 *   6  clock adjustment     1 byte
 *   7  free register        1 byte
 *
 * A command that does not start with the fixed code is ignored. (melonDS
 * and DeSmuME also accept the fixed code as the last four bits, that is, a
 * command byte sent in the other bit order. The chip's datasheet does not,
 * and neither do we.)
 *
 * Numbers are BCD. The year is 00-99 for 2000-2099, after which the chip
 * starts again at 2000. The day of the week is not worked out from the
 * date: it is a counter from 0 to 6 that steps at midnight and holds
 * whatever the program gave it. Nintendo's software uses 0 for Sunday.
 * The hour's bit 6 is the PM flag. In 24-hour mode the hour is 00-23 and
 * the flag is set from noon on (it cannot be written); in 12-hour mode the
 * hour is 00-11, "12 o'clock is defined as 00h", and the flag tells the
 * halves of the day apart.
 *
 * Status register 1:
 *   bit 0    write 1 to reset the chip: every register 0, the clock at
 *            2000-01-01 00:00:00 in 12-hour mode
 *   bit 1    1 = 24-hour mode
 *   bit 2-3  general purpose
 *   bit 4, 5 interrupt 1 and 2 have occurred; cleared when read
 *   bit 6, 7 power was low, power was lost; never set here
 * Status register 2:
 *   bit 0-3  interrupt 1 mode (below)
 *   bit 4-5  general purpose
 *   bit 6    interrupt 2 (alarm 2) enable
 *   bit 7    test mode; stored only
 *
 * The interrupt output. Alarm bytes carry an enable in bit 7: a field takes
 * part in the comparison only if its enable is set, and with none set the
 * alarm matches every minute. The hour compares with the hour register,
 * PM flag included. The chip looks at the alarms when the minute changes,
 * and this model does the same, on the chip's own clock:
 *
 *   alarm 2 (status 2 bit 6), alarm 1 (mode 0100): at each change of
 *     minute /INT goes low if the alarm matches and is released if it does
 *     not, so it stays low for the minute that matches.
 *   per-minute edge (mode 0x10): low from the first change of minute until
 *     the mode is changed.
 *   per-minute steady 1 (mode 0011): low at each change of minute for 30
 *     seconds.
 *   per-minute steady 2 (mode 0111): low at each change of minute for 7.9
 *     ms. Time comes in whole seconds here, so the pulse is stretched to
 *     the first second of the minute, which is as short as a caller that
 *     looks once a second can see.
 *   selected frequency (mode 0x01) and 32 kHz (mode 1xxx): square waves of
 *     1 Hz and up. They cannot be told in whole seconds and are not
 *     produced: the pin stays high.
 *
 * Each time an interrupt pulls the pin low its flag in status register 1
 * is set. Changing interrupt 1's mode releases the pin from interrupt 1;
 * clearing the alarm 2 enable releases it from interrupt 2.
 *
 * Not modelled: the clock adjustment register's effect on the oscillator
 * (the value is kept), and the power flags.
 */
#include <string.h>

#include "nds_rtc.h"

enum { RTC_IDLE, RTC_COMMAND, RTC_WRITE, RTC_READ, RTC_DONE };

/* Registers by command number. */
enum {
    REG_STATUS1,
    REG_STATUS2,
    REG_DATE_TIME,
    REG_TIME,
    REG_INT1,
    REG_INT2,
    REG_ADJUST,
    REG_FREE,
};

#define STATUS1_RESET 0x01u
#define STATUS1_24_HOUR 0x02u
#define STATUS1_WRITABLE 0x0Eu
#define STATUS1_INT1 0x10u
#define STATUS1_INT2 0x20u

#define STATUS2_INT1_MODE 0x0Fu
#define STATUS2_INT1_ALARM 0x04u /* the interrupt 1 register is an alarm: three bytes */
#define STATUS2_INT2_ENABLE 0x40u

#define LINE_INT1 0x01u
#define LINE_INT2 0x02u

#define HOUR_PM 0x40u
#define ALARM_ENABLE 0x80u

#define DAY_SECONDS 86400
#define WEEK_SECONDS (7 * DAY_SECONDS)
#define WEEK_MINUTES (7u * 1440u)
/* The chip counts 2000-01-01 through 2099-12-31 and starts again. */
#define CENTURY_SECONDS ((int64_t)36525 * DAY_SECONDS)
#define EPOCH_2000_DAYS 10957 /* days from 1970-01-01 to 2000-01-01 */
#define EPOCH_2000_SECONDS ((int64_t)EPOCH_2000_DAYS * DAY_SECONDS)

/* ------------------------------------------------------------------ */
/* Calendar                                                            */
/* ------------------------------------------------------------------ */

/* Days since 1970-01-01 of a proleptic Gregorian date (the usual
 * era-based conversion). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static uint8_t bcd(uint32_t v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* A BCD field of `mask` bits (the registers' unused bits are not part of
 * the number), brought into lo..hi. */
static int from_bcd(uint8_t v, uint8_t mask, int lo, int hi)
{
    int n = ((v & mask) >> 4) * 10 + (v & mask & 15);
    return n < lo ? lo : n > hi ? hi : n;
}

/* value modulo period, in 0 .. period - 1 whatever value's sign. */
static int64_t wrap(int64_t value, int64_t period)
{
    int64_t r = value % period;
    return r < 0 ? r + period : r;
}

/* ------------------------------------------------------------------ */
/* The clock                                                           */
/* ------------------------------------------------------------------ */

/* Seconds since 2000-01-01 00:00:00 that the chip shows at `now`. */
static int64_t clock_seconds(const NdsRtc *rtc, int64_t now)
{
    int64_t base = wrap(now, CENTURY_SECONDS) - EPOCH_2000_SECONDS;
    return wrap(base + wrap(rtc->offset, CENTURY_SECONDS), CENTURY_SECONDS);
}

/* The day-of-week counter and the time of day as seconds into its week of
 * seven days. It runs beside the clock: a century is not a whole number of
 * weeks, and the counter does not stop to match the date. */
static uint32_t week_seconds(const NdsRtc *rtc, int64_t now)
{
    return (uint32_t)wrap(wrap(now, WEEK_SECONDS) + wrap(rtc->week_offset, WEEK_SECONDS), WEEK_SECONDS);
}

/* Make the clock show `target` now. */
static void set_clock(NdsRtc *rtc, int64_t now, int64_t target)
{
    int64_t base = wrap(now, CENTURY_SECONDS) - EPOCH_2000_SECONDS;
    rtc->offset = wrap(wrap(target, CENTURY_SECONDS) - base, CENTURY_SECONDS);
}

static void set_week(NdsRtc *rtc, int64_t now, int64_t target)
{
    rtc->week_offset = (int32_t)wrap(wrap(target, WEEK_SECONDS) - wrap(now, WEEK_SECONDS), WEEK_SECONDS);
}

/* An hour of the day, 0-23, as the hour register holds it. */
static uint8_t hour_register(const NdsRtc *rtc, uint32_t hour)
{
    uint32_t shown = (rtc->status1 & STATUS1_24_HOUR) ? hour : hour % 12u;
    return (uint8_t)(bcd(shown) | (hour >= 12u ? HOUR_PM : 0u));
}

static int hour_from_register(const NdsRtc *rtc, uint8_t v)
{
    if (rtc->status1 & STATUS1_24_HOUR) return from_bcd(v, 0x3F, 0, 23); /* the flag is read-only */
    return from_bcd(v, 0x3F, 0, 11) + ((v & HOUR_PM) ? 12 : 0);
}

/* buf[0..6] = the date and time in the chip's format. */
static void latch_time(const NdsRtc *rtc, int64_t now, uint8_t *buf)
{
    int64_t t = clock_seconds(rtc, now);
    uint32_t seconds = (uint32_t)(t % DAY_SECONDS);
    int y, mo, d;
    civil_from_days(t / DAY_SECONDS + EPOCH_2000_DAYS, &y, &mo, &d);
    buf[0] = bcd((uint32_t)(y - 2000));
    buf[1] = bcd((uint32_t)mo);
    buf[2] = bcd((uint32_t)d);
    buf[3] = (uint8_t)(week_seconds(rtc, now) / DAY_SECONDS);
    buf[4] = hour_register(rtc, seconds / 3600u);
    buf[5] = bcd(seconds / 60u % 60u);
    buf[6] = bcd(seconds % 60u);
}

/* ------------------------------------------------------------------ */
/* The interrupt output                                                */
/* ------------------------------------------------------------------ */

/* week_minute: minutes into the day-of-week counter's week. */
static bool alarm_matches(const NdsRtc *rtc, const uint8_t alarm[3], uint32_t week_minute)
{
    uint32_t weekday = week_minute / 1440u;
    uint32_t hour = week_minute % 1440u / 60u;
    uint32_t minute = week_minute % 60u;
    if ((alarm[0] & ALARM_ENABLE) && (alarm[0] & 7u) != weekday) return false;
    if ((alarm[1] & ALARM_ENABLE) && (alarm[1] & 0x7Fu) != hour_register(rtc, hour)) return false;
    if ((alarm[2] & ALARM_ENABLE) && (alarm[2] & 0x7Fu) != bcd(minute)) return false;
    return true;
}

static void assert_line(NdsRtc *rtc, uint8_t line)
{
    rtc->lines = (uint8_t)(rtc->lines | line);
    rtc->status1 = (uint8_t)(rtc->status1 | (line == LINE_INT1 ? STATUS1_INT1 : STATUS1_INT2));
}

static void release_line(NdsRtc *rtc, uint8_t line)
{
    rtc->lines = (uint8_t)(rtc->lines & (LINE_INT1 | LINE_INT2) & ~(uint32_t)line);
}

/* The clock's minute has just changed to week_minute. */
static void minute_carry(NdsRtc *rtc, uint32_t week_minute)
{
    uint32_t mode = rtc->status2 & STATUS2_INT1_MODE;

    if (mode == 0x4u) {
        if (alarm_matches(rtc, rtc->alarm1, week_minute)) assert_line(rtc, LINE_INT1);
        else release_line(rtc, LINE_INT1);
    } else if ((mode & 0xBu) == 0x2u || mode == 0x3u || mode == 0x7u) {
        assert_line(rtc, LINE_INT1); /* per-minute edge, steady 1, steady 2 */
    }
    if (rtc->status2 & STATUS2_INT2_ENABLE) {
        if (alarm_matches(rtc, rtc->alarm2, week_minute)) assert_line(rtc, LINE_INT2);
        else release_line(rtc, LINE_INT2);
    }
}

/*
 * Bring the interrupt output up to `now`: every change of minute since the
 * last call, oldest first, then the releases that go by the second. After
 * a gap of more than a week only the last week's minutes are gone through;
 * every alarm comes round within one.
 */
static void advance(NdsRtc *rtc, int64_t now)
{
    uint64_t elapsed = 0;
    if (rtc->started && now > rtc->seen) elapsed = (uint64_t)now - (uint64_t)rtc->seen;
    rtc->started = 1;
    rtc->seen = now;

    uint32_t week = week_seconds(rtc, now);
    uint32_t second = week % 60u;
    uint32_t minute = week / 60u;
    /* Seconds into its minute the clock was at the last call. */
    uint32_t second_before = (second + 60u - (uint32_t)(elapsed % 60u)) % 60u;
    uint64_t carries = (elapsed + second_before) / 60u;
    if (carries > WEEK_MINUTES) carries = WEEK_MINUTES;
    for (uint32_t back = (uint32_t)carries; back > 0; back--) {
        minute_carry(rtc, (minute + WEEK_MINUTES - (back - 1u)) % WEEK_MINUTES);
    }

    uint32_t mode = rtc->status2 & STATUS2_INT1_MODE;
    if (mode == 0x3u && second >= 30u) release_line(rtc, LINE_INT1);
    if (mode == 0x7u && second >= 1u) release_line(rtc, LINE_INT1);
}

bool nds_rtc_interrupt(const NdsRtc *rtc)
{
    return rtc->lines != 0;
}

bool nds_rtc_update(NdsRtc *rtc, int64_t now)
{
    bool before = rtc->lines != 0;
    advance(rtc, now);
    return (rtc->lines != 0) != before;
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static void chip_reset(NdsRtc *rtc, int64_t now)
{
    rtc->status1 = 0;
    rtc->status2 = 0;
    memset(rtc->alarm1, 0, sizeof rtc->alarm1);
    memset(rtc->alarm2, 0, sizeof rtc->alarm2);
    rtc->adjust = 0;
    rtc->free_register = 0;
    rtc->lines = 0;
    set_clock(rtc, now, 0); /* 2000-01-01 00:00:00 */
    set_week(rtc, now, 0);  /* day of week 0 */
}

static uint8_t register_length(const NdsRtc *rtc, uint8_t command)
{
    switch (command) {
    case REG_DATE_TIME: return 7;
    case REG_TIME: return 3;
    case REG_INT1: return (rtc->status2 & STATUS2_INT1_ALARM) ? 3 : 1;
    case REG_INT2: return 3;
    default: return 1;
    }
}

/* A read command has arrived: the register's bytes go into the buffer. */
static void load_register(NdsRtc *rtc, int64_t now)
{
    uint8_t time[7];

    memset(rtc->buffer, 0, sizeof rtc->buffer);
    switch (rtc->command) {
    case REG_STATUS1:
        rtc->buffer[0] = rtc->status1;
        rtc->status1 = (uint8_t)(rtc->status1 & STATUS1_WRITABLE); /* the flags are "auto-cleared on read" */
        break;
    case REG_STATUS2: rtc->buffer[0] = rtc->status2; break;
    case REG_DATE_TIME: latch_time(rtc, now, rtc->buffer); break;
    case REG_TIME:
        latch_time(rtc, now, time);
        memcpy(rtc->buffer, time + 4, 3);
        break;
    case REG_INT1:
        if (rtc->status2 & STATUS2_INT1_ALARM) memcpy(rtc->buffer, rtc->alarm1, 3);
        else rtc->buffer[0] = rtc->alarm1[2];
        break;
    case REG_INT2: memcpy(rtc->buffer, rtc->alarm2, 3); break;
    case REG_ADJUST: rtc->buffer[0] = rtc->adjust; break;
    default: rtc->buffer[0] = rtc->free_register; break;
    }
}

/* The last byte of a write command has arrived. */
static void store_register(NdsRtc *rtc, int64_t now)
{
    const uint8_t *b = rtc->buffer;

    switch (rtc->command) {
    case REG_STATUS1:
        if (b[0] & STATUS1_RESET) chip_reset(rtc, now);
        rtc->status1 = (uint8_t)((rtc->status1 & (STATUS1_INT1 | STATUS1_INT2)) | (b[0] & STATUS1_WRITABLE));
        break;
    case REG_STATUS2:
        if ((b[0] ^ rtc->status2) & STATUS2_INT1_MODE) release_line(rtc, LINE_INT1);
        if (!(b[0] & STATUS2_INT2_ENABLE)) release_line(rtc, LINE_INT2);
        rtc->status2 = b[0];
        break;
    case REG_DATE_TIME: {
        int y = 2000 + from_bcd(b[0], 0xFF, 0, 99);
        int mo = from_bcd(b[1], 0x1F, 1, 12);
        int d = from_bcd(b[2], 0x3F, 1, 31);
        int time = hour_from_register(rtc, b[4]) * 3600 + from_bcd(b[5], 0x7F, 0, 59) * 60 +
                   from_bcd(b[6], 0x7F, 0, 59);
        set_clock(rtc, now, (days_from_civil(y, mo, d) - EPOCH_2000_DAYS) * DAY_SECONDS + time);
        set_week(rtc, now, (int64_t)((b[3] & 7u) % 7u) * DAY_SECONDS + time);
        break;
    }
    case REG_TIME: {
        int time = hour_from_register(rtc, b[0]) * 3600 + from_bcd(b[1], 0x7F, 0, 59) * 60 +
                   from_bcd(b[2], 0x7F, 0, 59);
        int64_t day = clock_seconds(rtc, now) / DAY_SECONDS;
        int64_t weekday = week_seconds(rtc, now) / DAY_SECONDS;
        set_clock(rtc, now, day * DAY_SECONDS + time);
        set_week(rtc, now, weekday * DAY_SECONDS + time);
        break;
    }
    case REG_INT1:
        if (rtc->status2 & STATUS2_INT1_ALARM) {
            rtc->alarm1[0] = b[0] & 0x87u; /* bits 3-6 "not used (always zero)" */
            rtc->alarm1[1] = b[1];
            rtc->alarm1[2] = b[2];
        } else {
            rtc->alarm1[2] = b[0];
        }
        break;
    case REG_INT2:
        rtc->alarm2[0] = b[0] & 0x87u;
        rtc->alarm2[1] = b[1];
        rtc->alarm2[2] = b[2];
        break;
    case REG_ADJUST: rtc->adjust = b[0]; break;
    default: rtc->free_register = b[0]; break;
    }
}

/* ------------------------------------------------------------------ */
/* Serial protocol                                                     */
/* ------------------------------------------------------------------ */

/* Eight command bits are in rtc->shift, the first received in bit 7. */
static void begin_command(NdsRtc *rtc, int64_t now)
{
    uint8_t code = rtc->shift;

    rtc->index = 0;
    rtc->bits = 0;
    rtc->shift = 0;
    if ((code >> 4) != 0x6u) {
        rtc->state = RTC_DONE; /* not a command: ignored until deselected */
        return;
    }
    rtc->command = (uint8_t)((code >> 1) & 7u);
    rtc->length = register_length(rtc, rtc->command);
    if (code & 1u) {
        load_register(rtc, now);
        rtc->state = RTC_READ;
    } else {
        rtc->state = RTC_WRITE;
    }
}

/* The chip reacts to the levels of its three lines. */
static void chip_pins(NdsRtc *rtc, uint8_t pins, int64_t now)
{
    uint8_t prev = rtc->pins;

    rtc->pins = pins;
    if (!(pins & NDS_RTC_SELECT)) {
        rtc->state = RTC_IDLE;
        return;
    }
    if (!(prev & NDS_RTC_SELECT)) {
        rtc->state = RTC_COMMAND;
        rtc->shift = 0;
        rtc->bits = 0;
        return;
    }
    bool rising = !(prev & NDS_RTC_CLOCK) && (pins & NDS_RTC_CLOCK);
    bool falling = (prev & NDS_RTC_CLOCK) && !(pins & NDS_RTC_CLOCK);
    uint8_t sio = (pins & NDS_RTC_DATA) ? 1u : 0u;

    switch (rtc->state) {
    case RTC_COMMAND:
        if (!rising) break;
        rtc->shift = (uint8_t)((rtc->shift << 1) | sio);
        if (++rtc->bits == 8) begin_command(rtc, now);
        break;
    case RTC_WRITE:
        if (!rising) break;
        rtc->shift = (uint8_t)(rtc->shift | (sio << (rtc->bits & 7u)));
        if (++rtc->bits == 8) {
            if (rtc->index < sizeof rtc->buffer) rtc->buffer[rtc->index] = rtc->shift;
            rtc->index++;
            rtc->shift = 0;
            rtc->bits = 0;
            if (rtc->index >= rtc->length) {
                store_register(rtc, now);
                rtc->state = RTC_DONE;
            }
        }
        break;
    case RTC_READ:
        if (!falling) break;
        /* The chip drives SIO unless the DS is driving it too. */
        if (!(rtc->port & NDS_RTC_DATA_OUT) && rtc->index < sizeof rtc->buffer) {
            uint8_t bit = (uint8_t)((rtc->buffer[rtc->index] >> (rtc->bits & 7u)) & 1u);
            rtc->pins = (uint8_t)((rtc->pins & (NDS_RTC_CLOCK | NDS_RTC_SELECT)) | bit);
        }
        if (++rtc->bits == 8) {
            rtc->bits = 0;
            if (++rtc->index >= rtc->length) rtc->state = RTC_DONE; /* the last bit stays on SIO */
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* The port                                                            */
/* ------------------------------------------------------------------ */

void nds_rtc_reset(NdsRtc *rtc)
{
    memset(rtc, 0, sizeof *rtc);
    rtc->status1 = STATUS1_24_HOUR;
    /* 1970-01-01 was a Thursday, day 4 with Sunday as 0. */
    rtc->week_offset = 4 * DAY_SECONDS;
}

/* The three lines read as their levels; the rest of the byte reads as
 * written. */
uint8_t nds_rtc_read(const NdsRtc *rtc)
{
    return (uint8_t)((rtc->port & 0xF8u) | (rtc->pins & 0x07u));
}

bool nds_rtc_write(NdsRtc *rtc, uint8_t value, int64_t now)
{
    bool before = rtc->lines != 0;
    uint32_t pins = rtc->pins;

    advance(rtc, now);
    rtc->port = value;
    /* A line the DS is not driving keeps its level. */
    if (value & NDS_RTC_DATA_OUT) pins = (pins & ~(uint32_t)NDS_RTC_DATA) | (value & NDS_RTC_DATA);
    if (value & NDS_RTC_CLOCK_OUT) pins = (pins & ~(uint32_t)NDS_RTC_CLOCK) | (value & NDS_RTC_CLOCK);
    if (value & NDS_RTC_SELECT_OUT) pins = (pins & ~(uint32_t)NDS_RTC_SELECT) | (value & NDS_RTC_SELECT);
    chip_pins(rtc, (uint8_t)(pins & 7u), now);
    return (rtc->lines != 0) != before;
}
