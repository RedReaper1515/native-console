/*
 * Cartridge real-time clock through the GPIO port, driven the way the
 * Pokemon RTC library drives it: select with SCK high then CS high, command
 * byte most significant bit first, parameters least significant bit first,
 * each bit set up with SCK low and latched on the rising edge; reads sample
 * SIO after the rising edge. Covers detection, ROM show-through, the status
 * register, date/time and time reads and writes (24- and 12-hour), the
 * century wrap, reset, emulated time and the saved state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba.h"
#include "gba_bus.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

static GbaMachine *m;

static void gpio(uint32_t off, uint16_t v) { gba_bus_write16(&m->cpu, 0x08000000u + off, v, GBA_ACCESS_NSEQ); }
static uint16_t gpio_data(void) { return (uint16_t)gba_bus_read16(&m->cpu, 0x080000C4u, GBA_ACCESS_NSEQ); }

static void command(uint8_t code)
{
    gpio(0xC4, 1); /* SCK high, CS low */
    gpio(0xC4, 5); /* CS high: selected */
    gpio(0xC6, 7); /* the GBA drives SCK, SIO and CS */
    for (int i = 7; i >= 0; i--) {
        uint16_t bit = (uint16_t)(((code >> i) & 1u) << 1);
        gpio(0xC4, bit | 4u);
        gpio(0xC4, bit | 5u);
    }
}

static void send(uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        uint16_t bit = (uint16_t)(((v >> i) & 1u) << 1);
        gpio(0xC4, bit | 4u);
        gpio(0xC4, bit | 5u);
    }
}

static uint8_t receive(void)
{
    uint8_t v = 0;
    gpio(0xC6, 5); /* SIO is now an input */
    for (int i = 0; i < 8; i++) {
        gpio(0xC4, 4);
        gpio(0xC4, 5);
        v |= (uint8_t)(((gpio_data() >> 1) & 1u) << i);
    }
    return v;
}

static void deselect(void)
{
    gpio(0xC4, 1);
    gpio(0xC6, 7);
}

static uint8_t read_status(void)
{
    command(0x63);
    uint8_t v = receive();
    deselect();
    return v;
}

static void write_status(uint8_t v)
{
    command(0x62);
    send(v);
    deselect();
}

static void read_datetime(uint8_t out[7])
{
    command(0x65);
    for (int i = 0; i < 7; i++) out[i] = receive();
    deselect();
}

static void write_datetime(const uint8_t in[7])
{
    command(0x64);
    for (int i = 0; i < 7; i++) send(in[i]);
    deselect();
}

static void expect_datetime(int line, const uint8_t want[7], bool check_weekday)
{
    uint8_t got[7];
    read_datetime(got);
    bool same = true;
    for (int i = 0; i < 7; i++) {
        if (i == 3 && !check_weekday) continue;
        if (got[i] != want[i]) same = false;
    }
    if (!same) {
        printf("FAIL %s:%d: date/time %02X-%02X-%02X wd%X %02X:%02X:%02X, expected %02X-%02X-%02X wd%X %02X:%02X:%02X\n",
               __FILE__, line, got[0], got[1], got[2], got[3], got[4], got[5], got[6], want[0], want[1], want[2],
               want[3], want[4], want[5], want[6]);
        failures++;
    }
}

static uint8_t rom[0x1000];

static GbaMachine *machine(const char *id, const char *code)
{
    memset(rom, 0, sizeof rom);
    rom[0xB2] = 0x96;
    rom[0] = 0xFE; rom[1] = 0xFF; rom[2] = 0xFF; rom[3] = 0xEA; /* b . */
    rom[0xC4] = 0x34; rom[0xC5] = 0x12;                          /* ROM data under the port */
    memcpy(rom + 0x200, "FLASH1M_V103", 12);
    if (id) memcpy(rom + 0x400, id, strlen(id));
    if (code) memcpy(rom + 0xAC, code, 4);
    GbaMachine *g = gba_create();
    if (!gba_load_rom(g, rom, sizeof rom)) exit(2);
    gba_reset(g, true);
    return g;
}

int main(void)
{
    /* Detection. */
    m = machine(NULL, NULL);
    CHECK(!m->rtc.present, "RTC detected in a ROM without one");
    gpio(0xC8, 1);
    CHECK(gpio_data() == 0x1234, "ROM without an RTC: port reads %04X, expected ROM data 1234", gpio_data());
    gba_destroy(m);
    m = machine(NULL, "BPEE");
    CHECK(m->rtc.present, "RTC not detected from game code BPEE");
    gba_destroy(m);
    m = machine("SIIRTC_V001", NULL);
    CHECK(m->rtc.present, "RTC not detected from the SIIRTC_V library string");

    /* The port reads as ROM until reading is enabled. */
    CHECK(gpio_data() == 0x1234, "port readable before 0x080000C8 bit 0 is set (%04X)", gpio_data());
    gpio(0xC8, 1);

    /* Status: 24-hour mode, battery good; only bits 1, 3, 5, 6 are writable. */
    CHECK(read_status() == 0x40, "initial status %02X, expected 40", read_status());
    write_status(0xFF);
    CHECK(read_status() == 0x6A, "status after writing FF is %02X, expected 6A", read_status());
    write_status(0x40);

    /* Headless clock: 2026-01-01 12:00:00, a Thursday; PM flag in 24-hour mode. */
    const uint8_t start[7] = { 0x26, 0x01, 0x01, 0x04, 0x92, 0x00, 0x00 };
    expect_datetime(__LINE__, start, true);

    /* It follows emulated time. */
    gba_run_cycles(m, GBA_CPU_HZ * 2u + 1000u);
    const uint8_t later[7] = { 0x26, 0x01, 0x01, 0x04, 0x92, 0x00, 0x02 };
    expect_datetime(__LINE__, later, true);

    /* Set the date and time, then cross the century boundary. */
    const uint8_t eve[7] = { 0x99, 0x12, 0x31, 0x04, 0x23, 0x59, 0x58 };
    write_datetime(eve);
    const uint8_t eve_read[7] = { 0x99, 0x12, 0x31, 0x04, 0xA3, 0x59, 0x58 };
    expect_datetime(__LINE__, eve_read, false);
    CHECK(m->save_dirty, "setting the clock did not mark the save dirty");
    gba_run_cycles(m, GBA_CPU_HZ * 3u);
    const uint8_t wrapped[7] = { 0x00, 0x01, 0x01, 0x06, 0x00, 0x00, 0x01 }; /* 2000-01-01 was a Saturday */
    expect_datetime(__LINE__, wrapped, true);

    /* Time only (command 3), and 12-hour mode. */
    command(0x66);
    send(0x07); send(0x08); send(0x09);
    deselect();
    command(0x67);
    uint8_t t[3]; /* a loop, not an initializer list: C does not order its elements */
    for (int i = 0; i < 3; i++) t[i] = receive();
    deselect();
    CHECK(t[0] == 0x07 && t[1] == 0x08 && t[2] == 0x09, "time %02X:%02X:%02X, expected 07:08:09", t[0], t[1], t[2]);
    write_status(0x00); /* 12-hour mode */
    command(0x66);
    send(0x83); send(0x00); send(0x00); /* 3 PM */
    deselect();
    command(0x67);
    uint8_t h12 = receive();
    deselect();
    CHECK(h12 == 0x83, "12-hour 3 PM reads %02X, expected 83", h12);
    write_status(0x40);
    command(0x67);
    uint8_t h24 = receive();
    deselect();
    CHECK(h24 == 0x95, "3 PM in 24-hour mode reads %02X, expected 95", h24);

    /* The clock's state survives in the save. */
    uint8_t state[GBA_RTC_STATE_SIZE];
    uint8_t before[7];
    read_datetime(before);
    gba_rtc_save_state(m, state);
    GbaMachine *first = m;
    m = machine("SIIRTC_V001", NULL);
    /* Same underlying clock (as the host clock would be); the saved offset
     * must then give the same reading. */
    m->rtc.start = first->rtc.start + (int64_t)(first->cpu.cycles / GBA_CPU_HZ);
    CHECK(gba_rtc_load_state(m, state, sizeof state), "saved clock state not accepted");
    gpio(0xC8, 1);
    expect_datetime(__LINE__, before, true);
    CHECK(read_status() == 0x40, "status not restored (%02X)", read_status());
    gba_destroy(first);

    /* Reset: status cleared, clock at 2000-01-01 00:00:00. */
    command(0x60);
    deselect();
    CHECK(read_status() == 0x00, "status after reset %02X, expected 00", read_status());
    const uint8_t zero[7] = { 0x00, 0x01, 0x01, 0x06, 0x00, 0x00, 0x00 };
    expect_datetime(__LINE__, zero, true);

    /* Not a command (fixed code is not 0110): ignored until deselected. */
    command(0x13);
    deselect();
    CHECK(read_status() == 0x00, "a bad command changed the chip (status %02X)", read_status());

    gba_destroy(m);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("rtc: all checks passed\n");
    return 0;
}
