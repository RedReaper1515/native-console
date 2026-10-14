/*
 * Cartridge real-time clock: a Seiko S-3511 on the cartridge GPIO port
 * (GBATEK "GBA Cart I/O Port (GPIO)" and "GBA Cart Real-Time Clock").
 * Pokemon Ruby/Sapphire/Emerald and Boktai use it.
 *
 * GPIO registers (16-bit, in the ROM area):
 *   0x080000C4  data       bit 0 SCK, bit 1 SIO, bit 2 CS (bit 3 unused)
 *   0x080000C6  direction  1 = the GBA drives the pin
 *   0x080000C8  control    bit 0: registers read back (else ROM data does)
 *
 * The game bit-bangs a serial protocol: CS high selects the chip, then each
 * bit is presented on SIO while SCK is low and latched on SCK's rising edge.
 * A command byte comes first, most significant bit first:
 *   0110 CCC R   C = command, R = 1 to read the parameters back
 * then the parameter bytes, least significant bit first. When reading, the
 * chip puts each bit on SIO at SCK's falling edge; the game samples it after
 * the rising edge. Commands:
 *   0 reset (none)   1 status (1 byte)   2 date and time (7)   3 time (3)
 *   4 alarm (2, accepted and ignored)
 * Date and time are BCD: year (2000-2099), month, day, day of week
 * (0 = Sunday), hour, minute, second. The hour's bit 7 is the PM flag, set
 * in 24-hour mode too (games mask it).
 *
 * Status register: bit 1 per-minute IRQ, bit 3 constant IRQ, bit 5 alarm
 * IRQ, bit 6 24-hour mode, bit 7 power lost (never set here: the battery
 * is always good). Interrupt output is not emulated.
 *
 * The time is the host's local clock for play (wall_clock), or, headless,
 * a fixed start plus emulated time so runs are reproducible. Time the game
 * sets is kept as an offset from that clock and saved with the battery
 * save, as is the status register.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gba.h"

enum { RTC_IDLE, RTC_COMMAND, RTC_WRITE, RTC_READ, RTC_DONE };

#define SCK 1u
#define SIO 2u
#define CS 4u

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

#define EPOCH_2000_DAYS 10957 /* days_from_civil(2000, 1, 1) */

static int64_t seconds_since_2000(int y, int mo, int d, int h, int mi, int s)
{
    return (days_from_civil(y, mo, d) - EPOCH_2000_DAYS) * 86400 + h * 3600 + mi * 60 + s;
}

bool gba_rtc_parse_time(const char *text, int64_t *seconds)
{
    int y, mo, d, h = 0, mi = 0, s = 0;
    int n = sscanf(text, "%d-%d-%d%*[ T]%d:%d:%d", &y, &mo, &d, &h, &mi, &s);
    if (n < 3 || y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 ||
        mi > 59 || s < 0 || s > 59)
        return false;
    *seconds = seconds_since_2000(y, mo, d, h, mi, s);
    return true;
}

static int64_t wall_now(void)
{
    time_t t = time(NULL);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return seconds_since_2000(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
}

static int64_t clock_now(const GbaMachine *m)
{
    if (m->rtc.wall_clock) return wall_now();
    return m->rtc.start + (int64_t)(m->cpu.cycles / GBA_CPU_HZ);
}

/* The chip counts 2000-01-01 through 2099-12-31 and wraps. */
#define CENTURY_SECONDS ((int64_t)36525 * 86400)

int64_t gba_rtc_now(const GbaMachine *m)
{
    int64_t t = (clock_now(m) + m->rtc.offset) % CENTURY_SECONDS;
    return t < 0 ? t + CENTURY_SECONDS : t;
}

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static int from_bcd(uint8_t v, int lo, int hi)
{
    int n = (v >> 4) * 10 + (v & 15);
    return n < lo ? lo : n > hi ? hi : n;
}

/* buf[0..6] = the current date and time in the chip's format. */
static void latch_time(GbaMachine *m, uint8_t *buf)
{
    int64_t t = gba_rtc_now(m);
    int64_t days = t / 86400 + EPOCH_2000_DAYS;
    int secs = (int)(t % 86400);
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);
    int hour = secs / 3600;
    buf[0] = bcd(y - 2000);
    buf[1] = bcd(mo);
    buf[2] = bcd(d);
    buf[3] = (uint8_t)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    buf[4] = (uint8_t)(((m->rtc.control & 0x40u) ? bcd(hour) : bcd(hour % 12)) | (hour >= 12 ? 0x80u : 0u));
    buf[5] = bcd(secs / 60 % 60);
    buf[6] = bcd(secs % 60);
}

static int hour_from_register(const GbaMachine *m, uint8_t v)
{
    int h = from_bcd(v & 0x3Fu, 0, 23);
    if (!(m->rtc.control & 0x40u)) h = h % 12 + ((v & 0x80u) ? 12 : 0);
    return h;
}

/* Move the clock so it reads `target` now. */
static void set_clock(GbaMachine *m, int64_t target)
{
    m->rtc.offset = target - clock_now(m);
    m->save_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Serial protocol                                                     */
/* ------------------------------------------------------------------ */

static const uint8_t k_param_bytes[8] = { 0, 1, 7, 3, 2, 0, 0, 0 };

static void begin_command(GbaMachine *m)
{
    GbaRtc *r = &m->rtc;
    uint8_t code = r->shift;
    if ((code >> 4) != 0x6u) {
        r->state = RTC_DONE; /* not a command: ignore until deselected */
        return;
    }
    r->command = (uint8_t)((code >> 1) & 7u);
    r->length = k_param_bytes[r->command];
    r->index = 0;
    r->bits = 0;
    r->shift = 0;
    if (r->command == 0) {
        /* Reset: status cleared, clock to 2000-01-01 00:00:00. */
        r->control = 0;
        set_clock(m, 0);
        r->state = RTC_DONE;
        return;
    }
    if (!r->length) {
        r->state = RTC_DONE;
        return;
    }
    if (code & 1u) {
        if (r->command == 1) r->buf[0] = r->control;
        else if (r->command == 2) latch_time(m, r->buf);
        else if (r->command == 3) {
            uint8_t full[7];
            latch_time(m, full);
            memcpy(r->buf, full + 4, 3);
        } else {
            memset(r->buf, 0, sizeof r->buf);
        }
        r->state = RTC_READ;
    } else {
        r->state = RTC_WRITE;
    }
}

static void finish_write(GbaMachine *m)
{
    GbaRtc *r = &m->rtc;
    const uint8_t *b = r->buf;
    switch (r->command) {
    case 1:
        r->control = b[0] & 0x6Au;
        m->save_dirty = true;
        break;
    case 2: {
        int y = 2000 + from_bcd(b[0], 0, 99), mo = from_bcd(b[1], 1, 12), d = from_bcd(b[2], 1, 31);
        set_clock(m, seconds_since_2000(y, mo, d, hour_from_register(m, b[4]), from_bcd(b[5], 0, 59),
                                        from_bcd(b[6], 0, 59)));
        break;
    }
    case 3: {
        int64_t day_start = gba_rtc_now(m) / 86400 * 86400;
        set_clock(m, day_start + hour_from_register(m, b[0]) * 3600 + from_bcd(b[1], 0, 59) * 60 +
                         from_bcd(b[2], 0, 59));
        break;
    }
    default:
        break; /* alarm: accepted, not emulated */
    }
}

/* The chip reacts to the pin levels the GBA drives. */
static void chip_pins(GbaMachine *m, uint8_t pins)
{
    GbaRtc *r = &m->rtc;
    uint8_t prev = r->pins;
    r->pins = pins;
    if (!(pins & CS)) {
        r->state = RTC_IDLE;
        r->sio_out = 0;
        return;
    }
    if (!(prev & CS)) {
        r->state = RTC_COMMAND;
        r->shift = 0;
        r->bits = 0;
        return;
    }
    bool rising = !(prev & SCK) && (pins & SCK);
    bool falling = (prev & SCK) && !(pins & SCK);
    uint8_t sio = (pins & SIO) ? 1u : 0u;
    switch (r->state) {
    case RTC_COMMAND:
        if (!rising) break;
        r->shift = (uint8_t)((r->shift << 1) | sio);
        if (++r->bits == 8) begin_command(m);
        break;
    case RTC_WRITE:
        if (!rising) break;
        r->shift |= (uint8_t)(sio << r->bits);
        if (++r->bits == 8) {
            r->buf[r->index++] = r->shift;
            r->shift = 0;
            r->bits = 0;
            if (r->index == r->length) {
                finish_write(m);
                r->state = RTC_DONE;
            }
        }
        break;
    case RTC_READ:
        if (!falling) break;
        r->sio_out = (r->buf[r->index] >> r->bits) & 1u;
        if (++r->bits == 8) {
            r->bits = 0;
            if (++r->index == r->length) r->state = RTC_DONE; /* the last bit stays on SIO */
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* GPIO registers                                                      */
/* ------------------------------------------------------------------ */

static uint16_t gpio_reg(const GbaMachine *m, uint32_t off)
{
    const GbaRtc *r = &m->rtc;
    switch (off) {
    case 0xC4: {
        uint8_t chip = (uint8_t)(r->state == RTC_READ || r->state == RTC_DONE ? r->sio_out << 1 : 0u);
        return (uint16_t)(((r->gpio_data & r->gpio_dir) | (chip & ~r->gpio_dir)) & 0xFu);
    }
    case 0xC6: return r->gpio_dir;
    case 0xC8: return r->gpio_ctrl;
    default: return 0;
    }
}

uint32_t gba_gpio_read(GbaMachine *m, uint32_t addr, int size, uint32_t rom_value)
{
    if (!(m->rtc.gpio_ctrl & 1u)) return rom_value;
    uint32_t off = addr & 0x01FFFFFEu;
    uint32_t v = gpio_reg(m, off);
    if (size == 4) return v | ((off + 2u < 0xCAu ? (uint32_t)gpio_reg(m, off + 2u) : rom_value >> 16) << 16);
    return size == 2 ? v : (v >> ((addr & 1u) * 8u)) & 0xFFu;
}

static void gpio_write16(GbaMachine *m, uint32_t off, uint16_t v)
{
    GbaRtc *r = &m->rtc;
    switch (off) {
    case 0xC4:
        r->gpio_data = v & 0xFu;
        chip_pins(m, (uint8_t)(r->gpio_data & r->gpio_dir));
        break;
    case 0xC6:
        r->gpio_dir = v & 0xFu;
        break;
    case 0xC8:
        r->gpio_ctrl = v & 1u;
        break;
    default:
        break;
    }
}

void gba_gpio_write(GbaMachine *m, uint32_t addr, uint32_t value, int size)
{
    uint32_t off = addr & 0x01FFFFFEu;
    if (size == 1) value <<= (addr & 1u) * 8u; /* the high byte is always 0 */
    gpio_write16(m, off, (uint16_t)value);
    if (size == 4 && off + 2u < 0xCAu) gpio_write16(m, off + 2u, (uint16_t)(value >> 16));
}

/* ------------------------------------------------------------------ */
/* Setup and persistence                                               */
/* ------------------------------------------------------------------ */

bool gba_rtc_detect(const uint8_t *rom, size_t size)
{
    /* Nintendo's RTC library links in its version string; ROM hacks keep it. */
    static const char id[] = "SIIRTC_V";
    for (size_t i = 0; i + sizeof id - 1 <= size; i += 4) {
        if (rom[i] == 'S' && memcmp(rom + i, id, sizeof id - 1) == 0) return true;
    }
    /* Otherwise by game code (the first three characters; the fourth is the region). */
    static const char *const codes[] = {
        "AXV", "AXP", "BPE", /* Pokemon Ruby, Sapphire, Emerald */
        "U3I", "U32", "U33", /* Boktai 1-3 */
        "BR4",               /* Rockman EXE 4.5 Real Operation */
        "BKA",               /* Sennen Kazoku */
    };
    if (size < 0xB0) return false;
    for (size_t k = 0; k < sizeof codes / sizeof codes[0]; k++) {
        if (memcmp(rom + 0xAC, codes[k], 3) == 0) return true;
    }
    return false;
}

void gba_rtc_reset(GbaMachine *m)
{
    /* The clock is battery-backed: offset, status and the clock source
     * survive a console reset; the port and the transfer do not. */
    GbaRtc *r = &m->rtc;
    r->gpio_data = r->gpio_dir = r->gpio_ctrl = 0;
    r->pins = 0;
    r->sio_out = 0;
    r->state = RTC_IDLE;
}

/* Explicit stores: MSVC 19.51 /O2 miscompiles some loops of "v >> (8 * i)"
 * byte stores (see wav_header in gba_runner.c). */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* "RTC" 0x01, status register, 3 zero bytes, offset (int64, little-endian). */
void gba_rtc_save_state(const GbaMachine *m, uint8_t out[GBA_RTC_STATE_SIZE])
{
    memset(out, 0, GBA_RTC_STATE_SIZE);
    memcpy(out, "RTC\x01", 4);
    out[4] = m->rtc.control;
    uint64_t v = (uint64_t)m->rtc.offset;
    put_le32(out + 8, (uint32_t)v);
    put_le32(out + 12, (uint32_t)(v >> 32));
}

bool gba_rtc_load_state(GbaMachine *m, const uint8_t *in, size_t size)
{
    if (size < GBA_RTC_STATE_SIZE || memcmp(in, "RTC\x01", 4) != 0) return false;
    m->rtc.control = in[4] & 0x6Au;
    m->rtc.offset = (int64_t)(get_le32(in + 8) | ((uint64_t)get_le32(in + 12) << 32));
    return true;
}
