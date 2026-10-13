/*
 * The real-time clock, driven through its register the way libnds's
 * rtcTransaction() drives it: CS raised with the clock high, the command
 * byte most significant bit first, parameters least significant bit first,
 * each bit set up together with the clock going low and taken on the clock
 * going high; when reading, the data line is turned round and sampled
 * after each rising edge.
 *
 * Expected values are from GBATEK, "DS Real-Time Clock (RTC)": the register
 * formats, the meaning of the status bits, the alarm bytes. Dates are
 * worked out here, by counting days.
 *
 * Covered: every register read and written, the date and time against the
 * time supplied, rollovers (minute, day, month, leap day, year, century),
 * the day-of-week counter, 12-hour mode, setting the clock and the time,
 * the reset bit, commands that are not commands, the direction bits, and
 * the interrupt output for both alarms and the per-minute modes.
 */
#include <stdio.h>
#include <string.h>

#include "nds_rtc.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                \
    do {                                                \
        g_checks++;                                     \
        if (!(cond)) {                                  \
            g_failures++;                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

/* The register's bits, as libnds names them. */
enum {
    SIO = 0x01,
    SCK = 0x02,
    CS = 0x04,
    SIO_OUT = 0x10,
    SCK_OUT = 0x20,
    CS_OUT = 0x40,
    CS_0 = CS_OUT,
    CS_1 = CS_OUT | CS,
    SCK_0 = SCK_OUT,
    SCK_1 = SCK_OUT | SCK,
    SIO_1 = SIO_OUT | SIO,
};

/* Command bytes, Seiko's way round: 0110, register, read. */
enum {
    WRITE_STATUS1 = 0x60,
    READ_STATUS1 = 0x61,
    WRITE_STATUS2 = 0x62,
    READ_STATUS2 = 0x63,
    WRITE_DATE_TIME = 0x64,
    READ_DATE_TIME = 0x65,
    WRITE_TIME = 0x66,
    READ_TIME = 0x67,
    WRITE_INT1 = 0x68,
    READ_INT1 = 0x69,
    WRITE_INT2 = 0x6A,
    READ_INT2 = 0x6B,
    WRITE_ADJUST = 0x6C,
    READ_ADJUST = 0x6D,
    WRITE_FREE = 0x6E,
    READ_FREE = 0x6F,
};

static NdsRtc g_rtc;
static int64_t g_now;
static bool g_changed; /* a write reported a change of the interrupt line */

static void port(int value)
{
    if (nds_rtc_write(&g_rtc, (uint8_t)value, g_now)) g_changed = true;
}

static void begin(void)
{
    port(CS_0 | SCK_1 | SIO_1);
    port(CS_1 | SCK_1 | SIO_1);
}

static void end(void)
{
    port(CS_0 | SCK_1);
}

static void send_command(uint8_t code)
{
    for (int i = 7; i >= 0; i--) {
        int bit = (code >> i) & 1;
        port(CS_1 | SCK_0 | SIO_OUT | bit);
        port(CS_1 | SCK_1 | SIO_OUT | bit);
    }
}

static void send(uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        int bit = (value >> i) & 1;
        port(CS_1 | SCK_0 | SIO_OUT | bit);
        port(CS_1 | SCK_1 | SIO_OUT | bit);
    }
}

static uint8_t receive(void)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) {
        port(CS_1 | SCK_0);
        port(CS_1 | SCK_1);
        if (nds_rtc_read(&g_rtc) & SIO) value |= (uint8_t)(1u << i);
    }
    return value;
}

static void read_register(uint8_t command, uint8_t *out, int count)
{
    begin();
    send_command(command);
    for (int i = 0; i < count; i++) out[i] = receive();
    end();
}

static void write_register(uint8_t command, const uint8_t *in, int count)
{
    begin();
    send_command(command);
    for (int i = 0; i < count; i++) send(in[i]);
    end();
}

static uint8_t read1(uint8_t command)
{
    uint8_t value;
    read_register(command, &value, 1);
    return value;
}

static void write1(uint8_t command, uint8_t value)
{
    write_register(command, &value, 1);
}

/* ------------------------------------------------------------------ */

/* Seconds from 1970-01-01 00:00 to a date and time, by counting. */
static bool leap(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

static int64_t at(int year, int month, int day, int hour, int minute, int second)
{
    static const int lengths[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t days = 0;
    for (int y = 1970; y < year; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < month; m++) days += lengths[m - 1] + (m == 2 && leap(year) ? 1 : 0);
    days += day - 1;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static void expect_date_time(int line, const char *what, const uint8_t want[7])
{
    uint8_t got[7];
    read_register(READ_DATE_TIME, got, 7);
    g_checks++;
    if (memcmp(got, want, 7) != 0) {
        g_failures++;
        printf("FAIL %s:%d: %s: %02X-%02X-%02X day %X %02X:%02X:%02X, expected %02X-%02X-%02X day %X %02X:%02X:%02X\n",
               __FILE__, line, what, got[0], got[1], got[2], got[3], got[4], got[5], got[6], want[0], want[1], want[2],
               want[3], want[4], want[5], want[6]);
    }
}

static void set_date_time(uint8_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour, uint8_t minute,
                          uint8_t second)
{
    const uint8_t bytes[7] = { year, month, day, weekday, hour, minute, second };
    write_register(WRITE_DATE_TIME, bytes, 7);
}

/* ------------------------------------------------------------------ */

static void test_clock(void)
{
    uint8_t got[7];

    nds_rtc_reset(&g_rtc);
    g_now = at(2026, 10, 6, 14, 30, 45); /* a Tuesday */

    /* As the firmware leaves it: 24-hour mode; no power flags, which a
     * program takes as a reason to reset the chip. */
    CHECK(read1(READ_STATUS1) == 0x02, "status register 1 after reset %02X, expected 02", read1(READ_STATUS1));
    CHECK(read1(READ_STATUS2) == 0x00, "status register 2 after reset %02X, expected 00", read1(READ_STATUS2));

    /* Year, month, day, day of week (0 = Sunday), hour, minute, second in
     * BCD; in 24-hour mode the hour's bit 6 is set from noon on. */
    static const uint8_t start[7] = { 0x26, 0x10, 0x06, 0x02, 0x54, 0x30, 0x45 };
    expect_date_time(__LINE__, "2026-10-06 14:30:45", start);
    read_register(READ_TIME, got, 3);
    CHECK(got[0] == 0x54 && got[1] == 0x30 && got[2] == 0x45, "time %02X:%02X:%02X, expected 54:30:45", got[0], got[1],
          got[2]);

    /* GBATEK's "Fwd" numbering is the same bits on the wire: the byte 86h
     * sent least significant bit first is read status register 1, and A6h
     * read date and time. */
    begin();
    send(0x86);
    got[0] = receive();
    end();
    CHECK(got[0] == 0x02, "status register 1 through the command 86h, LSB first: %02X", got[0]);
    begin();
    send(0xA6);
    for (int i = 0; i < 7; i++) got[i] = receive();
    end();
    CHECK(memcmp(got, start, 7) == 0, "date and time through the command A6h, LSB first");

    /* A program may sample the data line while the clock is still low: the
     * chip puts each bit out on the falling edge and holds it. */
    begin();
    send_command(READ_TIME);
    for (int n = 0; n < 3; n++) {
        got[n] = 0;
        for (int i = 0; i < 8; i++) {
            port(CS_1 | SCK_0);
            if (nds_rtc_read(&g_rtc) & SIO) got[n] |= (uint8_t)(1u << i);
            port(CS_1 | SCK_1);
        }
    }
    end();
    CHECK(got[0] == 0x54 && got[1] == 0x30 && got[2] == 0x45, "time sampled with the clock low: %02X:%02X:%02X", got[0],
          got[1], got[2]);

    /* The clock runs on the time it is given. */
    g_now += 20;
    static const uint8_t later[7] = { 0x26, 0x10, 0x06, 0x02, 0x54, 0x31, 0x05 };
    expect_date_time(__LINE__, "20 seconds on", later);
    g_now = at(2026, 10, 6, 9, 5, 7);
    static const uint8_t morning[7] = { 0x26, 0x10, 0x06, 0x02, 0x09, 0x05, 0x07 };
    expect_date_time(__LINE__, "a morning hour has no PM flag", morning);
    g_now = at(2026, 10, 6, 12, 0, 0);
    static const uint8_t noon[7] = { 0x26, 0x10, 0x06, 0x02, 0x52, 0x00, 0x00 };
    expect_date_time(__LINE__, "noon", noon);

    /* Rollovers: the year (31 December 2026 is a Thursday), ... */
    g_now = at(2026, 12, 31, 23, 59, 58);
    static const uint8_t eve[7] = { 0x26, 0x12, 0x31, 0x04, 0x63, 0x59, 0x58 };
    expect_date_time(__LINE__, "2026-12-31 23:59:58", eve);
    g_now += 3;
    static const uint8_t new_year[7] = { 0x27, 0x01, 0x01, 0x05, 0x00, 0x00, 0x01 };
    expect_date_time(__LINE__, "three seconds later", new_year);
    /* ... the end of February in a common year and in a leap year, ... */
    g_now = at(2027, 2, 28, 23, 59, 59) + 1;
    static const uint8_t march[7] = { 0x27, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00 }; /* a Monday */
    expect_date_time(__LINE__, "the day after 2027-02-28", march);
    g_now = at(2028, 2, 28, 23, 59, 59) + 1;
    static const uint8_t leap_day[7] = { 0x28, 0x02, 0x29, 0x02, 0x00, 0x00, 0x00 }; /* a Tuesday */
    expect_date_time(__LINE__, "the day after 2028-02-28", leap_day);
    /* ... a 30-day month, ... */
    g_now = at(2026, 4, 30, 23, 59, 59) + 1;
    static const uint8_t may[7] = { 0x26, 0x05, 0x01, 0x05, 0x00, 0x00, 0x00 }; /* a Friday */
    expect_date_time(__LINE__, "the day after 2026-04-30", may);
    /* ... and the first and last days the chip has. */
    g_now = at(2000, 1, 1, 0, 0, 0);
    static const uint8_t first[7] = { 0x00, 0x01, 0x01, 0x06, 0x00, 0x00, 0x00 }; /* a Saturday */
    expect_date_time(__LINE__, "2000-01-01 00:00:00", first);
    g_now = at(2099, 12, 31, 23, 59, 59);
    static const uint8_t last[7] = { 0x99, 0x12, 0x31, 0x04, 0x63, 0x59, 0x59 }; /* a Thursday */
    expect_date_time(__LINE__, "2099-12-31 23:59:59", last);
    /* After 2099 the year is 00 again; the day of the week just goes on. */
    g_now += 1;
    static const uint8_t wrapped[7] = { 0x00, 0x01, 0x01, 0x05, 0x00, 0x00, 0x00 };
    expect_date_time(__LINE__, "the second after 2099-12-31 23:59:59", wrapped);
}

static void test_registers(void)
{
    uint8_t got[7];

    nds_rtc_reset(&g_rtc);
    g_now = at(2026, 10, 6, 14, 30, 45);

    /* Status register 1: bits 1-3 read and write; bits 4-7 are the chip's
     * flags and cannot be written (bit 0, reset, is tested further on). */
    write1(WRITE_STATUS1, 0x0E);
    CHECK(read1(READ_STATUS1) == 0x0E, "status register 1 after writing 0E reads %02X", read1(READ_STATUS1));
    write1(WRITE_STATUS1, 0xF2);
    CHECK(read1(READ_STATUS1) == 0x02, "status register 1 after writing F2 reads %02X, expected 02",
          read1(READ_STATUS1));

    /* Status register 2: all eight bits. (B0h: test mode and the two
     * general purpose bits, no interrupt.) */
    write1(WRITE_STATUS2, 0xB0);
    CHECK(read1(READ_STATUS2) == 0xB0, "status register 2 after writing B0 reads %02X", read1(READ_STATUS2));
    write1(WRITE_STATUS2, 0x00);
    CHECK(read1(READ_STATUS2) == 0x00, "status register 2 after writing 00 reads %02X", read1(READ_STATUS2));

    /* Interrupt register 1 is one byte while status register 2 bit 2 is
     * clear (the frequency setting; 20h is a general purpose bit) ... */
    write1(WRITE_INT1, 0x20);
    CHECK(read1(READ_INT1) == 0x20, "interrupt register 1 (one byte) reads %02X, expected 20", read1(READ_INT1));
    /* ... and three with it set: alarm 1's day of week, hour and minute. */
    write1(WRITE_STATUS2, 0x04);
    static const uint8_t alarm1[3] = { 0x83, 0xD2, 0xA5 };
    write_register(WRITE_INT1, alarm1, 3);
    read_register(READ_INT1, got, 3);
    CHECK(memcmp(got, alarm1, 3) == 0, "alarm 1 reads %02X %02X %02X, expected 83 D2 A5", got[0], got[1], got[2]);
    /* "There is only one register shared as 'Selected Frequency Steady
     * Interrupt' ... and as 'Alarm1 Minute'". */
    write1(WRITE_STATUS2, 0x00);
    CHECK(read1(READ_INT1) == 0xA5, "the one-byte interrupt register 1 reads %02X, expected alarm 1's minute, A5",
          read1(READ_INT1));
    write1(WRITE_INT1, 0x1F);
    write1(WRITE_STATUS2, 0x04);
    read_register(READ_INT1, got, 3);
    CHECK(got[0] == 0x83 && got[1] == 0xD2 && got[2] == 0x1F, "alarm 1 reads %02X %02X %02X, expected 83 D2 1F", got[0],
          got[1], got[2]);
    /* The day-of-week byte: bits 0-2 and the enable in bit 7; "3-6 Not
     * used (always zero)". */
    static const uint8_t ones[3] = { 0xFF, 0xFF, 0xFF };
    write_register(WRITE_INT1, ones, 3);
    read_register(READ_INT1, got, 3);
    CHECK(got[0] == 0x87 && got[1] == 0xFF && got[2] == 0xFF, "alarm 1 after writing FF FF FF reads %02X %02X %02X",
          got[0], got[1], got[2]);
    static const uint8_t zeros[3] = { 0, 0, 0 };
    write_register(WRITE_INT1, zeros, 3);
    write1(WRITE_STATUS2, 0x00);

    /* Interrupt register 2: alarm 2, always three bytes. */
    static const uint8_t alarm2[3] = { 0x06, 0x51, 0x59 };
    write_register(WRITE_INT2, alarm2, 3);
    read_register(READ_INT2, got, 3);
    CHECK(memcmp(got, alarm2, 3) == 0, "alarm 2 reads %02X %02X %02X, expected 06 51 59", got[0], got[1], got[2]);
    write_register(WRITE_INT1, alarm1, 1);
    read_register(READ_INT2, got, 3);
    CHECK(memcmp(got, alarm2, 3) == 0, "writing interrupt register 1 changed alarm 2");
    write_register(WRITE_INT2, zeros, 3);

    /* The clock adjustment and free registers: a byte each. */
    write1(WRITE_ADJUST, 0x5A);
    write1(WRITE_FREE, 0xC3);
    CHECK(read1(READ_ADJUST) == 0x5A, "clock adjustment register reads %02X, expected 5A", read1(READ_ADJUST));
    CHECK(read1(READ_FREE) == 0xC3, "free register reads %02X, expected C3", read1(READ_FREE));
    write1(WRITE_ADJUST, 0x00);
    CHECK(read1(READ_FREE) == 0xC3 && read1(READ_ADJUST) == 0x00, "the adjustment and free registers are one register");

    /* None of that touched the clock. */
    static const uint8_t start[7] = { 0x26, 0x10, 0x06, 0x02, 0x54, 0x30, 0x45 };
    expect_date_time(__LINE__, "after writing the other registers", start);

    /* Reset: status register 1 bit 0. "All registers are reset to 00h (or
     * 01h)": the date 00-01-01, day 0, midnight, 12-hour mode. */
    write1(WRITE_STATUS2, 0x30);
    write1(WRITE_STATUS1, 0x01);
    CHECK(read1(READ_STATUS1) == 0x00, "status register 1 after a reset %02X, expected 00", read1(READ_STATUS1));
    CHECK(read1(READ_STATUS2) == 0x00, "status register 2 after a reset %02X, expected 00", read1(READ_STATUS2));
    CHECK(read1(READ_FREE) == 0x00, "free register after a reset %02X, expected 00", read1(READ_FREE));
    static const uint8_t zero[7] = { 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00 };
    expect_date_time(__LINE__, "after a reset", zero);
    g_now += 61;
    static const uint8_t ticking[7] = { 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01 };
    expect_date_time(__LINE__, "61 seconds after a reset", ticking);
    /* What libnds's rtcReset() writes when it finds a power flag: the
     * status it read, with the reset bit. */
    write1(WRITE_STATUS1, 0x03);
    CHECK(read1(READ_STATUS1) == 0x02, "status register 1 after writing 03 reads %02X, expected 02",
          read1(READ_STATUS1));
}

static void test_setting(void)
{
    uint8_t got[7];

    nds_rtc_reset(&g_rtc);
    g_now = at(2026, 10, 6, 14, 30, 45);

    /* Set the date and time; the chip keeps the day of week it is given,
     * whatever the date. */
    set_date_time(0x31, 0x07, 0x04, 0x05, 0x08, 0x09, 0x10);
    static const uint8_t set[7] = { 0x31, 0x07, 0x04, 0x05, 0x08, 0x09, 0x10 };
    expect_date_time(__LINE__, "after setting 2031-07-04 08:09:10, day 5", set);
    /* It goes on from there: three days and five seconds later, and the
     * day-of-week counter has gone 5, 6, 0, 1. */
    g_now += 3 * 86400 + 5;
    static const uint8_t on[7] = { 0x31, 0x07, 0x07, 0x01, 0x08, 0x09, 0x15 };
    expect_date_time(__LINE__, "three days and five seconds after setting", on);
    /* The whole state is plain data: a copy reads the same. */
    NdsRtc copy;
    memcpy(&copy, &g_rtc, sizeof copy);
    memset(&g_rtc, 0xEE, sizeof g_rtc);
    memcpy(&g_rtc, &copy, sizeof g_rtc);
    expect_date_time(__LINE__, "from a copy of the state", on);

    /* Setting the time alone keeps the date and the day of the week. */
    static const uint8_t time[3] = { 0x23, 0x58, 0x30 };
    write_register(WRITE_TIME, time, 3);
    static const uint8_t night[7] = { 0x31, 0x07, 0x07, 0x01, 0x63, 0x58, 0x30 };
    expect_date_time(__LINE__, "after setting the time to 23:58:30", night);
    g_now += 100;
    static const uint8_t next[7] = { 0x31, 0x07, 0x08, 0x02, 0x00, 0x00, 0x10 };
    expect_date_time(__LINE__, "100 seconds later, across midnight", next);

    /* A written PM flag means nothing in 24-hour mode ("AM/PM flag is read
     * only"). */
    static const uint8_t flagged[3] = { 0x49, 0x00, 0x00 };
    write_register(WRITE_TIME, flagged, 3);
    read_register(READ_TIME, got, 3);
    CHECK(got[0] == 0x09, "hour written as 49 in 24-hour mode reads %02X, expected 09", got[0]);

    /* The century: 2099-12-31 23:59:58, day 4, then three seconds. */
    set_date_time(0x99, 0x12, 0x31, 0x04, 0x23, 0x59, 0x58);
    g_now += 3;
    static const uint8_t wrapped[7] = { 0x00, 0x01, 0x01, 0x05, 0x00, 0x00, 0x01 };
    expect_date_time(__LINE__, "three seconds after 2099-12-31 23:59:58", wrapped);

    /* The registers' unused bits are not part of the numbers: month has
     * five bits, day six, minute and second seven. */
    set_date_time(0x26, 0xE3, 0xC9, 0xFA, 0x12, 0x85, 0x86);
    static const uint8_t masked[7] = { 0x26, 0x03, 0x09, 0x02, 0x52, 0x05, 0x06 };
    expect_date_time(__LINE__, "after setting bytes with unused bits set", masked);

    /* 12-hour mode: the hour is 00-11, "12 o'clock is defined as 00h", and
     * bit 6 tells afternoon from morning. */
    nds_rtc_reset(&g_rtc);
    write1(WRITE_STATUS1, 0x00);
    static const struct {
        int hour;
        uint8_t reg;
    } hours[] = { { 0, 0x00 }, { 1, 0x01 }, { 9, 0x09 }, { 11, 0x11 }, { 12, 0x40 }, { 13, 0x41 }, { 14, 0x42 },
                  { 22, 0x50 }, { 23, 0x51 } };
    for (size_t i = 0; i < sizeof hours / sizeof hours[0]; i++) {
        g_now = at(2026, 10, 6, hours[i].hour, 30, 45);
        read_register(READ_TIME, got, 3);
        CHECK(got[0] == hours[i].reg && got[1] == 0x30 && got[2] == 0x45, "%d:30:45 in 12-hour mode reads %02X:%02X:%02X",
              hours[i].hour, got[0], got[1], got[2]);
    }
    /* Set 3 PM in 12-hour mode; in 24-hour mode that is 15, with the flag. */
    static const uint8_t three_pm[3] = { 0x43, 0x20, 0x00 };
    write_register(WRITE_TIME, three_pm, 3);
    read_register(READ_TIME, got, 3);
    CHECK(got[0] == 0x43 && got[1] == 0x20, "3:20 PM set in 12-hour mode reads %02X:%02X", got[0], got[1]);
    write1(WRITE_STATUS1, 0x02);
    read_register(READ_TIME, got, 3);
    CHECK(got[0] == 0x55, "3 PM read in 24-hour mode is %02X, expected 55", got[0]);
    write1(WRITE_STATUS1, 0x00);
    static const uint8_t nine_am[3] = { 0x09, 0x00, 0x00 };
    write_register(WRITE_TIME, nine_am, 3);
    write1(WRITE_STATUS1, 0x02);
    read_register(READ_TIME, got, 3);
    CHECK(got[0] == 0x09, "9 AM set in 12-hour mode reads %02X in 24-hour mode, expected 09", got[0]);
    /* Midnight and noon in 12-hour mode are 00 and 00 with the flag. */
    write1(WRITE_STATUS1, 0x00);
    static const uint8_t twelve_pm[3] = { 0x40, 0x00, 0x00 };
    write_register(WRITE_TIME, twelve_pm, 3);
    write1(WRITE_STATUS1, 0x02);
    read_register(READ_TIME, got, 3);
    CHECK(got[0] == 0x52, "noon set in 12-hour mode reads %02X in 24-hour mode, expected 52", got[0]);
}

static void test_protocol(void)
{
    uint8_t got[3];

    nds_rtc_reset(&g_rtc);
    g_now = at(2026, 10, 6, 14, 30, 45);
    write1(WRITE_FREE, 0xA5);

    /* The register reads back its direction bits and the lines' levels. */
    port(CS_0 | SCK_1 | SIO_1);
    CHECK(nds_rtc_read(&g_rtc) == (CS_0 | SCK_1 | SIO_1), "the register reads %02X after writing %02X",
          nds_rtc_read(&g_rtc), CS_0 | SCK_1 | SIO_1);
    port(CS_0 | SCK_0 | SIO_OUT);
    CHECK(nds_rtc_read(&g_rtc) == (CS_0 | SCK_0 | SIO_OUT), "the register reads %02X after writing %02X",
          nds_rtc_read(&g_rtc), CS_0 | SCK_0 | SIO_OUT);

    /* A command that does not begin with the fixed code 0110 is ignored:
     * the chip sends nothing and takes nothing. */
    begin();
    send_command(0x1F);
    got[0] = receive();
    end();
    write1(0x9E, 0x00);
    write1(0xEE, 0x00);
    CHECK(read1(READ_FREE) == 0xA5, "a byte that is not a command wrote the free register (%02X)", read1(READ_FREE));
    /* That includes the fixed code arriving last (6Eh sent least
     * significant bit first). */
    begin();
    send(WRITE_FREE);
    send(0x00);
    end();
    CHECK(read1(READ_FREE) == 0xA5, "a command with the fixed code last was obeyed (%02X)", read1(READ_FREE));

    /* CS low ends a transfer: a write cut short changes nothing, and the
     * next transfer starts with a command. */
    begin();
    send_command(WRITE_FREE);
    for (int i = 0; i < 7; i++) { /* seven of the byte's eight bits */
        port(CS_1 | SCK_0 | SIO_OUT);
        port(CS_1 | SCK_1 | SIO_OUT);
    }
    end();
    CHECK(read1(READ_FREE) == 0xA5, "a write cut short by CS changed the register (%02X)", read1(READ_FREE));
    static const uint8_t partial[3] = { 0x11, 0x22, 0x33 };
    write_register(WRITE_DATE_TIME, partial, 3); /* three of seven bytes */
    read_register(READ_TIME, got, 3);
    CHECK(got[0] == 0x54 && got[1] == 0x30 && got[2] == 0x45, "a date and time write of three bytes changed the clock");

    /* Bits are taken on the clock's rising edge: the data line may change
     * while the clock is low. */
    begin();
    for (int i = 7; i >= 0; i--) {
        int bit = (WRITE_FREE >> i) & 1;
        port(CS_1 | SCK_0 | SIO_OUT | (bit ^ 1));
        port(CS_1 | SCK_0 | SIO_OUT | bit);
        port(CS_1 | SCK_1 | SIO_OUT | bit);
    }
    for (int i = 0; i < 8; i++) {
        int bit = (0x3C >> i) & 1;
        port(CS_1 | SCK_0 | SIO_OUT | (bit ^ 1));
        port(CS_1 | SCK_0 | SIO_OUT | bit);
        port(CS_1 | SCK_1 | SIO_OUT | bit);
    }
    end();
    CHECK(read1(READ_FREE) == 0x3C, "free register %02X, expected 3C: a bit was taken before the rising edge",
          read1(READ_FREE));

    /* The direction bits: a clock or select the DS is not driving does not
     * reach the chip. */
    begin();
    send_command(WRITE_FREE);
    for (int i = 0; i < 8; i++) {
        port(CS_OUT | CS | SIO_OUT | SIO);       /* clock bit low, but not driven */
        port(CS_OUT | CS | SIO_OUT | SIO | SCK); /* high, not driven */
    }
    port(SCK_1 | SIO_1); /* select bit low, but not driven: still selected */
    send(0x99);
    end();
    CHECK(read1(READ_FREE) == 0x99, "free register %02X, expected 99: undriven lines moved the chip", read1(READ_FREE));

    /* While the DS drives the data line, it reads its own bit. */
    begin();
    send_command(READ_FREE);
    port(CS_1 | SCK_0 | SIO_OUT);
    CHECK((nds_rtc_read(&g_rtc) & SIO) == 0, "the data line reads 1 while the DS drives it low");
    port(CS_1 | SCK_1 | SIO_OUT | SIO);
    CHECK((nds_rtc_read(&g_rtc) & SIO) != 0, "the data line reads 0 while the DS drives it high");
    end();

    /* Reading status register 1 does not disturb its writable bits. */
    write1(WRITE_STATUS1, 0x0A);
    CHECK(read1(READ_STATUS1) == 0x0A && read1(READ_STATUS1) == 0x0A, "status register 1 changed by being read");
}

/* ------------------------------------------------------------------ */

static bool tick(int seconds)
{
    g_now += seconds;
    return nds_rtc_update(&g_rtc, g_now);
}

static void test_interrupts(void)
{
    nds_rtc_reset(&g_rtc);
    g_now = at(2026, 10, 6, 14, 30, 45); /* Tuesday: day 2 */
    CHECK(!nds_rtc_update(&g_rtc, g_now) && !nds_rtc_interrupt(&g_rtc), "interrupt line low after reset");

    /* Alarm 2: day 2, 14:31. Each byte's bit 7 enables its comparison; the
     * hour carries the PM flag, 54h for 14 in 24-hour mode. */
    static const uint8_t alarm[3] = { 0x82, 0xD4, 0xB1 };
    write_register(WRITE_INT2, alarm, 3);
    g_changed = false;
    write1(WRITE_STATUS2, 0x40);
    CHECK(!g_changed && !nds_rtc_interrupt(&g_rtc), "enabling an alarm that is not due moved the line");
    CHECK(!tick(14) && !nds_rtc_interrupt(&g_rtc), "the alarm went off at 14:30:59");
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "the alarm did not go off at 14:31:00");
    /* Status register 1 bit 5 says interrupt 2 occurred, once. */
    CHECK(read1(READ_STATUS1) == 0x22, "status register 1 after alarm 2: %02X, expected 22", read1(READ_STATUS1));
    CHECK(read1(READ_STATUS1) == 0x02, "status register 1 read again: %02X, expected 02", read1(READ_STATUS1));
    CHECK(nds_rtc_interrupt(&g_rtc), "reading the flag released the line");
    /* The line stays low for the minute that matches. */
    CHECK(!tick(59) && nds_rtc_interrupt(&g_rtc), "the line changed within the minute");
    CHECK(tick(1) && !nds_rtc_interrupt(&g_rtc), "the line was not released at 14:32:00");
    CHECK(read1(READ_STATUS1) == 0x02, "a flag was set by the alarm's release: %02X", read1(READ_STATUS1));
    /* A week later, to the minute, it matches again; a day later it does
     * not. */
    g_now = at(2026, 10, 7, 14, 30, 59);
    nds_rtc_update(&g_rtc, g_now);
    CHECK(!tick(1) && !nds_rtc_interrupt(&g_rtc), "the alarm went off on the wrong day of the week");
    g_now = at(2026, 10, 13, 14, 30, 59);
    nds_rtc_update(&g_rtc, g_now);
    read1(READ_STATUS1);
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "the alarm did not go off a week later");
    /* Clearing the enable releases the line; the write that does it says
     * so. */
    g_changed = false;
    write1(WRITE_STATUS2, 0x00);
    CHECK(g_changed && !nds_rtc_interrupt(&g_rtc), "disabling alarm 2 did not release the line");
    read1(READ_STATUS1);

    /* With the day's enable clear it matches every day; with no enables,
     * every minute. */
    static const uint8_t daily[3] = { 0x00, 0xD4, 0xB1 };
    write_register(WRITE_INT2, daily, 3);
    write1(WRITE_STATUS2, 0x40);
    g_now = at(2026, 10, 15, 14, 30, 59);
    nds_rtc_update(&g_rtc, g_now);
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "an alarm for every day did not go off on a Thursday");
    static const uint8_t always[3] = { 0x02, 0x54, 0x31 };
    write_register(WRITE_INT2, always, 3);
    CHECK(!tick(60) && nds_rtc_interrupt(&g_rtc), "an alarm with nothing enabled did not match the next minute");
    CHECK(!tick(60) && nds_rtc_interrupt(&g_rtc), "an alarm with nothing enabled did not match the minute after");
    write1(WRITE_STATUS2, 0x00);
    read1(READ_STATUS1);

    /* An alarm that came and went while nobody looked leaves its flag. */
    write_register(WRITE_INT2, alarm, 3);
    write1(WRITE_STATUS2, 0x40);
    g_now = at(2026, 10, 20, 14, 0, 0);
    nds_rtc_update(&g_rtc, g_now);
    read1(READ_STATUS1);
    g_now = at(2026, 10, 20, 15, 0, 0);
    CHECK(!nds_rtc_update(&g_rtc, g_now) && !nds_rtc_interrupt(&g_rtc), "the line is low an hour after the alarm");
    CHECK(read1(READ_STATUS1) == 0x22, "status register 1 an hour after a missed alarm: %02X, expected 22",
          read1(READ_STATUS1));
    write1(WRITE_STATUS2, 0x00);

    /* Alarm 1 is interrupt 1's mode 0100; its flag is bit 4. In 12-hour
     * mode the hour to match is 02 with the PM flag. */
    write1(WRITE_STATUS1, 0x00);
    write1(WRITE_STATUS2, 0x04);
    static const uint8_t alarm1[3] = { 0x82, 0xC2, 0xB1 };
    write_register(WRITE_INT1, alarm1, 3);
    g_now = at(2026, 10, 27, 14, 30, 59);
    nds_rtc_update(&g_rtc, g_now);
    CHECK(!nds_rtc_interrupt(&g_rtc), "alarm 1 is low before its time");
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "alarm 1 did not go off at 2:31 PM");
    CHECK(read1(READ_STATUS1) == 0x10, "status register 1 after alarm 1: %02X, expected 10", read1(READ_STATUS1));
    CHECK(tick(60) && !nds_rtc_interrupt(&g_rtc), "alarm 1 was not released a minute later");
    /* 2:31 AM is another hour: any day, 2:31 PM. */
    static const uint8_t afternoon[3] = { 0x00, 0xC2, 0xB1 };
    write_register(WRITE_INT1, afternoon, 3);
    g_now = at(2026, 10, 28, 2, 30, 59);
    nds_rtc_update(&g_rtc, g_now);
    CHECK(!tick(1) && !nds_rtc_interrupt(&g_rtc), "alarm 1 for 2:31 PM went off at 2:31 AM");
    g_now = at(2026, 10, 28, 14, 30, 59);
    nds_rtc_update(&g_rtc, g_now);
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "alarm 1 for 2:31 PM did not go off at 2:31 PM");
    write1(WRITE_STATUS2, 0x00);
    write1(WRITE_STATUS1, 0x02);

    /* Both alarms on the one pin: it is low while either holds it. */
    static const uint8_t first[3] = { 0x00, 0x00, 0x80 | 0x10 };  /* minute 10 */
    static const uint8_t second[3] = { 0x00, 0x00, 0x80 | 0x11 }; /* minute 11 */
    write1(WRITE_STATUS2, 0x04);
    write_register(WRITE_INT1, first, 3);
    write_register(WRITE_INT2, second, 3);
    write1(WRITE_STATUS2, 0x44);
    g_now = at(2026, 10, 29, 8, 9, 59);
    nds_rtc_update(&g_rtc, g_now);
    read1(READ_STATUS1);
    CHECK(!nds_rtc_interrupt(&g_rtc), "an alarm for minute 10 or 11 is low at minute 9");
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "alarm 1 at minute 10");
    CHECK(!tick(60) && nds_rtc_interrupt(&g_rtc), "the line rose between alarm 1's minute and alarm 2's");
    CHECK(read1(READ_STATUS1) == 0x32, "status register 1 after both alarms: %02X, expected 32", read1(READ_STATUS1));
    CHECK(tick(60) && !nds_rtc_interrupt(&g_rtc), "the line was not released after alarm 2's minute");
    write1(WRITE_STATUS2, 0x00);
    write_register(WRITE_INT2, alarm, 3);

    /* Per-minute edge (0x10): low from the next change of minute until
     * the mode is changed. */
    g_now = at(2026, 10, 29, 9, 0, 30);
    nds_rtc_update(&g_rtc, g_now);
    read1(READ_STATUS1);
    g_changed = false;
    write1(WRITE_STATUS2, 0x02);
    CHECK(!g_changed && !nds_rtc_interrupt(&g_rtc), "per-minute edge: low as soon as it was selected");
    CHECK(!tick(29) && tick(1) && nds_rtc_interrupt(&g_rtc), "per-minute edge: not low at the change of minute");
    CHECK(!tick(45) && !tick(45) && nds_rtc_interrupt(&g_rtc), "per-minute edge: released by itself");
    CHECK(read1(READ_STATUS1) == 0x12, "per-minute edge: status register 1 %02X, expected 12", read1(READ_STATUS1));
    g_changed = false;
    write1(WRITE_STATUS2, 0x00);
    CHECK(g_changed && !nds_rtc_interrupt(&g_rtc), "per-minute edge: not released by changing the mode");
    g_now = at(2026, 10, 29, 9, 9, 59);
    nds_rtc_update(&g_rtc, g_now);
    write1(WRITE_STATUS2, 0x06); /* 0110 is the same mode */
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "per-minute edge (0110): not low at the change of minute");
    write1(WRITE_STATUS2, 0x00);

    /* Per-minute steady 1 (0011): low for the first 30 seconds of every
     * minute. */
    g_now = at(2026, 10, 29, 9, 19, 59);
    nds_rtc_update(&g_rtc, g_now);
    write1(WRITE_STATUS2, 0x03);
    CHECK(!nds_rtc_interrupt(&g_rtc), "steady 1: low before the first change of minute");
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "steady 1: not low at second 0");
    CHECK(!tick(29) && nds_rtc_interrupt(&g_rtc), "steady 1: not low at second 29");
    CHECK(tick(1) && !nds_rtc_interrupt(&g_rtc), "steady 1: not released at second 30");
    CHECK(!tick(29) && tick(1) && nds_rtc_interrupt(&g_rtc), "steady 1: not low again at the next minute");
    write1(WRITE_STATUS2, 0x00);

    /* Per-minute steady 2 (0111): a pulse at every change of minute, here
     * one second long. */
    g_now = at(2026, 10, 29, 9, 29, 59);
    nds_rtc_update(&g_rtc, g_now);
    write1(WRITE_STATUS2, 0x07);
    CHECK(tick(1) && nds_rtc_interrupt(&g_rtc), "steady 2: not low at second 0");
    CHECK(tick(1) && !nds_rtc_interrupt(&g_rtc), "steady 2: not released at second 1");
    CHECK(!tick(58) && tick(1) && nds_rtc_interrupt(&g_rtc), "steady 2: no pulse at the next minute");
    write1(WRITE_STATUS2, 0x00);

    /* The modes that are square waves are not produced: selected frequency
     * (0x01, the frequency register asking for 1 Hz) and 32 kHz (1xxx). */
    write1(WRITE_INT1, 0x01);
    write1(WRITE_STATUS2, 0x01);
    read1(READ_STATUS1);
    bool moved = false;
    for (int i = 0; i < 130; i++) moved = tick(1) || moved;
    write1(WRITE_STATUS2, 0x08);
    for (int i = 0; i < 130; i++) moved = tick(1) || moved;
    CHECK(!moved && !nds_rtc_interrupt(&g_rtc), "a square-wave mode moved the line");
    write1(WRITE_STATUS2, 0x00);

    /* Mode 0, and time standing still or going back, do nothing. */
    read1(READ_STATUS1);
    moved = false;
    for (int i = 0; i < 200; i++) moved = tick(1) || moved;
    moved = tick(0) || moved;
    g_now -= 500;
    moved = nds_rtc_update(&g_rtc, g_now) || moved;
    CHECK(!moved && !nds_rtc_interrupt(&g_rtc), "the line moved with interrupts off");
    CHECK(read1(READ_STATUS1) == 0x02, "a flag was set with interrupts off: %02X", read1(READ_STATUS1));

    /* The alarm goes by the clock as the program set it, not by `now`. */
    write_register(WRITE_INT2, alarm, 3); /* day 2, 14:31 */
    write1(WRITE_STATUS2, 0x40);
    g_now = at(2026, 11, 1, 3, 0, 0);
    nds_rtc_update(&g_rtc, g_now);
    set_date_time(0x30, 0x01, 0x01, 0x02, 0x14, 0x30, 0x58);
    CHECK(!nds_rtc_interrupt(&g_rtc), "setting the clock set off the alarm");
    CHECK(!tick(1) && tick(1) && nds_rtc_interrupt(&g_rtc), "the alarm did not follow the clock as set");
    /* A chip reset turns the interrupts off. */
    g_changed = false;
    write1(WRITE_STATUS1, 0x03);
    CHECK(g_changed && !nds_rtc_interrupt(&g_rtc), "a reset did not release the line");
}

int main(void)
{
    test_clock();
    test_registers();
    test_setting();
    test_protocol();
    test_interrupts();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks ran (fail closed)\n");
        return 1;
    }
    return g_failures ? 1 : 0;
}
