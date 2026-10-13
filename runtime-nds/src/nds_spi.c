/*
 * The ARM7's SPI bus and its devices: the power management chip, the
 * firmware flash and the touch screen controller (see nds_spi.h).
 *
 * The bus (GBATEK, "DS Serial Peripheral Interface Bus (SPI)"). SPICNT
 * bits 8-9 pick whose chip select a transfer pulls low. A write to SPIDATA
 * clocks one byte out and one byte in. After the transfer the chip select
 * is released unless SPICNT bit 11 ("hold") is set, so a program sets hold,
 * sends all but the last byte, clears hold and sends the last. A device
 * treats the first byte after its chip select falls as the start of a
 * command and forgets the command when chip select rises.
 *
 * Chip select also rises when the bus is switched off: libnds ends each
 * firmware command by writing 0 to SPICNT with hold still set, and a page
 * write is only carried out if that ends it. We release it as well when
 * SPICNT is pointed at another device, each device having its own line.
 *
 * What a byte reads while the selected device is not driving the line
 * (during a command byte, say) is not documented; it is 00h here.
 */
#include <string.h>

#include "nds_firmware.h"
#include "nds_spi.h"

#define BUS_IDLE 0x00u

/* ------------------------------------------------------------------ */
/* Power management                                                    */
/* ------------------------------------------------------------------ */

/*
 * GBATEK, "DS Power Management Device". The first byte after chip select
 * is the index: bits 0-6 the register, bit 7 set to read. Each byte after
 * it reads or writes that register.
 *
 *   0  control: bit 0 sound amplifier, 1 mute, 2 lower backlight, 3 upper
 *      backlight, 4 power LED blink, 5 blink speed, 6 shut down; 7 unused
 *   1  battery status, read-only: bit 0 set when the battery is low
 *   2  microphone amplifier: bit 0 on
 *   3  microphone amplifier gain: bits 0-1
 *
 * "On Old-DS, registers 4..7Fh are mirrors of 0..3." Bits a register does
 * not have read 0. Programs read a register back after writing it and try
 * again until it holds what they wrote, so the writable bits must stick.
 */
static const uint8_t k_power_writable[4] = { 0x7F, 0x00, 0x01, 0x03 };

static uint8_t power_transfer(NdsSpi *spi, uint8_t out)
{
    if (!spi->power_indexed) {
        spi->power_index = out;
        spi->power_indexed = 1;
        return BUS_IDLE;
    }
    uint32_t reg = spi->power_index & 3u;
    if (spi->power_index & 0x80u) return spi->power_reg[reg];
    uint8_t writable = k_power_writable[reg];
    spi->power_reg[reg] = (uint8_t)((spi->power_reg[reg] & (uint8_t)~writable) | (out & writable));
    return BUS_IDLE;
}

uint8_t nds_spi_power_register(const NdsSpi *spi, uint32_t index)
{
    return spi->power_reg[index & 3u];
}

void nds_spi_set_battery_low(NdsSpi *spi, bool low)
{
    spi->power_reg[NDS_POWER_REG_BATTERY] = low ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* Touch screen controller                                             */
/* ------------------------------------------------------------------ */

/*
 * A TSC2046 (GBATEK, "DS Touch Screen Controller (TSC)"): a 12-bit
 * converter with eight inputs. A byte with bit 7 set is a control byte and
 * starts a conversion:
 *
 *   bit 7    start
 *   bit 4-6  channel
 *   bit 3    1 = 8-bit conversion, 0 = 12-bit
 *   bit 2    reference: 0 = differential, 1 = single-ended
 *   bit 0-1  power-down mode; bit 0 set turns the pen interrupt output off
 *
 * The result follows the control byte: "one dummy bit (zero), followed by
 * the 8bit or 12bit conversion result (MSB first), followed by endless
 * padding (zero)". In whole bytes that is the top seven bits in the next
 * byte and the rest at the top of the one after. The second of those may
 * itself be the next control byte: the reply still carries the old result's
 * low bits, which is how programs read a run of samples back to back.
 *
 * Channels:
 *   0  temperature, diode voltage           5  X position
 *   1  Y position                           6  AUX: the microphone
 *   2  battery: tied to ground, reads 000h  7  temperature, second diode
 *   3  Z1, 4  Z2: pressure                     current
 *
 * With the pen up X reads 000h and Y reads FFFh (GBATEK). The pressure
 * channels with the pen up follow from how they are measured (below): Z1
 * 000h, Z2 FFFh.
 *
 * Pressure. Y+ is driven to the reference and X- to ground; current runs
 * down the Y plate to the point of contact, through the contact, and along
 * the X plate to X-. Z1 is the voltage at the X plate's end of the contact
 * and Z2 at the Y plate's. That is where GBATEK's two formulas come from:
 *   Rtouch = Rx_plate * X/4096 * (Z2/Z1 - 1)
 *   Rtouch = Rx_plate * X/4096 * (4096/Z1 - 1) - Ry_plate * (1 - Y/4096)
 * We report the Z1 and Z2 of a firm press: a contact of a quarter of a
 * plate's resistance, both plates alike. With no contact no current flows:
 * the X plate sits at ground and the Y plate at the reference.
 *
 * Temperature. GBATEK: K = (TP1 - TP0) * 8568 / 4096. The readings are
 * fixed at TP0 = 2E2h (the diode's 600 mV against a 3.33 V reference) and
 * TP1 = TP0 + 143, which is 299 K, 26 degrees C.
 *
 * Not modelled: the reference select bit. On the DS the microphone,
 * battery and temperature inputs give no valid reading in differential
 * mode; here every channel answers in either mode, as on a DSi.
 */
#define TOUCH_TEMP0 0x2E2u
#define TOUCH_TEMP1 (TOUCH_TEMP0 + 143u)
#define TOUCH_PLATE 4096u   /* a plate's resistance, in arbitrary units */
#define TOUCH_CONTACT 1024u /* the contact's, in the same units */

static uint32_t touch_conversion(const NdsSpi *spi, uint32_t channel)
{
    bool down = spi->touch_down != 0;
    uint32_t x = spi->touch_x & 0xFFFu;
    uint32_t y = spi->touch_y & 0xFFFu;
    /* The current's path: the Y plate above the contact, the contact, the
     * X plate below it. */
    uint32_t path = (TOUCH_PLATE - y) + TOUCH_CONTACT + x;
    uint32_t z2;

    switch (channel) {
    case 0: return TOUCH_TEMP0;
    case 1: return down ? y : 0xFFFu;
    case 2: return 0x000u;
    case 3: return down ? 4096u * x / path : 0x000u;
    case 4:
        if (!down) return 0xFFFu;
        z2 = 4096u * (x + TOUCH_CONTACT) / path;
        return z2 > 0xFFFu ? 0xFFFu : z2;
    case 5: return down ? x : 0x000u;
    case 6: return spi->microphone & 0xFFFu;
    default: return TOUCH_TEMP1;
    }
}

static uint8_t touch_transfer(NdsSpi *spi, uint8_t out)
{
    uint8_t in = (uint8_t)(spi->touch_shift >> 8);
    spi->touch_shift = (uint16_t)(spi->touch_shift << 8);
    if (out & 0x80u) {
        uint32_t value = touch_conversion(spi, (out >> 4) & 7u);
        /* An 8-bit conversion is the 12-bit one's top eight bits, and they
         * come out in the same place. */
        if (out & 0x08u) value &= 0xFF0u;
        spi->touch_control = out;
        spi->touch_shift = (uint16_t)(value << 3); /* bit 15, the dummy bit, is 0 */
    }
    return in;
}

void nds_spi_set_touch(NdsSpi *spi, bool down, uint16_t adc_x, uint16_t adc_y)
{
    spi->touch_down = down ? 1u : 0u;
    spi->touch_x = adc_x & 0xFFFu;
    spi->touch_y = adc_y & 0xFFFu;
}

void nds_spi_set_microphone(NdsSpi *spi, uint16_t sample12)
{
    spi->microphone = sample12 & 0xFFFu;
}

/* "Power Down Mode": /PENIRQ is enabled in modes 0 and 2, disabled in 1
 * and 3. The output is on after power-up. */
bool nds_spi_pen_irq(const NdsSpi *spi)
{
    return spi->touch_down && !(spi->touch_control & 0x01u);
}

/* ------------------------------------------------------------------ */
/* Firmware flash                                                      */
/* ------------------------------------------------------------------ */

/*
 * An ST M45PE20 (GBATEK, "DS Firmware Serial Flash Memory"): 256 KiB in
 * pages of 256 bytes and sectors of 64 KiB. The first byte after chip
 * select is an instruction; addresses are three bytes, most significant
 * first.
 *
 *   06h WREN  set the write enable latch (WEL)
 *   04h WRDI  clear it
 *   9Fh RDID  then read 20h, 40h, 12h: ST, M45PE, 2^18 bytes
 *   05h RDSR  then read the status register, as often as wanted:
 *             bit 0 write in progress, bit 1 WEL
 *   03h READ  address, then read from there on
 *   0Bh FAST  address, one dummy byte, then read
 *   0Ah PW    address, then 1-256 bytes to store (page write)
 *   02h PP    address, then 1-256 bytes to AND into the page (page
 *             program: bits can only go from 1 to 0)
 *   DBh PE    address: the page becomes FFh
 *   D8h SE    address: the sector becomes FFh
 *   B9h DP    deep power-down: "ignores all further instructions, except
 *             RDP"
 *   ABh RDP   release from deep power-down
 *
 * PW, PP, PE and SE are obeyed only with WEL set, and clear it when they
 * finish. A write stays inside its page: past the end of the page the
 * address goes back to the page's start, and of more than 256 bytes only
 * the last 256 count (the ST datasheet). A read runs on, from the last
 * byte of the chip to the first.
 *
 * The chip's /W pin "makes first 256 pages read-only". The DS ties it low
 * unless the jumper SL1 is bridged, which is how firmware replacements are
 * installed (GBATEK, "DS XBOO Connection Notes"), so 00000h-0FFFFh, where
 * the header and the Wi-Fi calibration are, cannot be written: PW, PP, PE
 * and SE addressed there do nothing and leave WEL set. The user settings
 * and the access points are in the last sector.
 *
 * Differences from the chip: writing and erasing take no time, so bit 0 of
 * the status register is never set. The chip erases, and discards a page
 * write of no bytes, when chip select rises; here an erase is done when its
 * last address byte arrives, and bytes are stored as they come.
 */
enum {
    FLASH_WREN = 0x06,
    FLASH_WRDI = 0x04,
    FLASH_RDID = 0x9F,
    FLASH_RDSR = 0x05,
    FLASH_READ = 0x03,
    FLASH_FAST = 0x0B,
    FLASH_PW = 0x0A,
    FLASH_PP = 0x02,
    FLASH_PE = 0xDB,
    FLASH_SE = 0xD8,
    FLASH_DP = 0xB9,
    FLASH_RDP = 0xAB,
};

enum {
    FLASH_STATE_INSTRUCTION, /* chip select has fallen: the next byte is an instruction */
    FLASH_STATE_ADDRESS,
    FLASH_STATE_DUMMY,
    FLASH_STATE_READ,
    FLASH_STATE_WRITE,
    FLASH_STATE_STATUS,
    FLASH_STATE_ID,
    FLASH_STATE_IGNORE, /* nothing more until chip select rises */
};

#define FLASH_MASK ((uint32_t)NDS_FIRMWARE_SIZE - 1u)
#define FLASH_PAGE 0x100u
#define FLASH_SECTOR 0x10000u
#define FLASH_PROTECTED (256u * FLASH_PAGE)

static void flash_erase(NdsSpi *spi, uint8_t *firmware, uint32_t start, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        uint8_t *cell = &firmware[(start + i) & FLASH_MASK];
        if (*cell != 0xFFu) {
            *cell = 0xFFu;
            spi->flash_dirty = 1;
        }
    }
    spi->flash_wrote = 1;
}

static void flash_instruction(NdsSpi *spi, uint8_t instruction)
{
    spi->flash_instruction = instruction;
    spi->flash_count = 0;
    spi->flash_address = 0;
    spi->flash_state = FLASH_STATE_IGNORE;
    if (spi->flash_powered_down) {
        if (instruction == FLASH_RDP) spi->flash_powered_down = 0;
        return;
    }
    switch (instruction) {
    case FLASH_WREN: spi->flash_write_enabled = 1; break;
    case FLASH_WRDI: spi->flash_write_enabled = 0; break;
    case FLASH_RDSR: spi->flash_state = FLASH_STATE_STATUS; break;
    case FLASH_RDID: spi->flash_state = FLASH_STATE_ID; break;
    case FLASH_READ:
    case FLASH_FAST:
        spi->flash_state = FLASH_STATE_ADDRESS;
        break;
    case FLASH_PW:
    case FLASH_PP:
    case FLASH_PE:
    case FLASH_SE:
        if (spi->flash_write_enabled) spi->flash_state = FLASH_STATE_ADDRESS;
        break;
    case FLASH_DP: spi->flash_powered_down = 1; break;
    default: break; /* RDP when awake, and anything that is not an instruction */
    }
}

/* The third address byte has arrived. */
static void flash_addressed(NdsSpi *spi, uint8_t *firmware)
{
    uint32_t address = spi->flash_address & FLASH_MASK;
    spi->flash_address = address;
    spi->flash_state = FLASH_STATE_IGNORE;
    switch (spi->flash_instruction) {
    case FLASH_READ: spi->flash_state = FLASH_STATE_READ; break;
    case FLASH_FAST: spi->flash_state = FLASH_STATE_DUMMY; break;
    case FLASH_PW:
    case FLASH_PP:
        if (address < FLASH_PROTECTED) break;
        memcpy(spi->flash_page, firmware + (address & ~(FLASH_PAGE - 1u)), FLASH_PAGE);
        spi->flash_state = FLASH_STATE_WRITE;
        break;
    case FLASH_PE:
        if (address >= FLASH_PROTECTED) flash_erase(spi, firmware, address & ~(FLASH_PAGE - 1u), FLASH_PAGE);
        break;
    case FLASH_SE:
        if (address >= FLASH_PROTECTED) flash_erase(spi, firmware, address & ~(FLASH_SECTOR - 1u), FLASH_SECTOR);
        break;
    default: break;
    }
}

static uint8_t flash_transfer(NdsSpi *spi, uint8_t *firmware, uint8_t out)
{
    static const uint8_t id[3] = { 0x20, 0x40, 0x12 };
    uint32_t address = spi->flash_address & FLASH_MASK;

    switch (spi->flash_state) {
    case FLASH_STATE_INSTRUCTION:
        flash_instruction(spi, out);
        return BUS_IDLE;
    case FLASH_STATE_ADDRESS:
        spi->flash_address = ((spi->flash_address << 8) | out) & 0xFFFFFFu;
        if (++spi->flash_count >= 3) flash_addressed(spi, firmware);
        return BUS_IDLE;
    case FLASH_STATE_DUMMY:
        spi->flash_state = FLASH_STATE_READ;
        return BUS_IDLE;
    case FLASH_STATE_READ:
        spi->flash_address = (address + 1u) & FLASH_MASK;
        return firmware[address];
    case FLASH_STATE_WRITE: {
        uint32_t offset = address & (FLASH_PAGE - 1u);
        /* Page program ANDs into the page as it was when the instruction
         * began, so a byte sent twice counts once, the last time. */
        uint8_t value = spi->flash_instruction == FLASH_PP ? (uint8_t)(spi->flash_page[offset] & out) : out;
        if (firmware[address] != value) {
            firmware[address] = value;
            spi->flash_dirty = 1;
        }
        spi->flash_wrote = 1;
        spi->flash_address = (address & ~(FLASH_PAGE - 1u)) | ((offset + 1u) & (FLASH_PAGE - 1u));
        return BUS_IDLE;
    }
    case FLASH_STATE_STATUS: return spi->flash_write_enabled ? 0x02u : 0x00u;
    case FLASH_STATE_ID:
        if (spi->flash_count < 3) return id[spi->flash_count++];
        return BUS_IDLE;
    default: return BUS_IDLE;
    }
}

/* Chip select rises: the instruction is over. "The WEL bit is automatically
 * cleared ... on completion of WRDI/PW/PP/PE/SE instructions." */
static void flash_deselect(NdsSpi *spi)
{
    if (spi->flash_wrote) spi->flash_write_enabled = 0;
    spi->flash_wrote = 0;
    spi->flash_state = FLASH_STATE_INSTRUCTION;
}

bool nds_spi_firmware_dirty(const NdsSpi *spi)
{
    return spi->flash_dirty != 0;
}

void nds_spi_clear_firmware_dirty(NdsSpi *spi)
{
    spi->flash_dirty = 0;
}

/* ------------------------------------------------------------------ */
/* The bus                                                             */
/* ------------------------------------------------------------------ */

static void deselect(NdsSpi *spi)
{
    switch (spi->selected) {
    case NDS_SPI_DEVICE_POWER + 1: spi->power_indexed = 0; break;
    case NDS_SPI_DEVICE_FIRMWARE + 1: flash_deselect(spi); break;
    case NDS_SPI_DEVICE_TOUCH + 1: spi->touch_shift = 0; break; /* the conversion is abandoned */
    default: break;
    }
    spi->selected = 0;
}

static uint8_t exchange(NdsSpi *spi, uint8_t *firmware, uint32_t device, uint8_t out)
{
    switch (device) {
    case NDS_SPI_DEVICE_POWER: return power_transfer(spi, out);
    case NDS_SPI_DEVICE_FIRMWARE: return flash_transfer(spi, firmware, out);
    case NDS_SPI_DEVICE_TOUCH: return touch_transfer(spi, out);
    default: return BUS_IDLE;
    }
}

static void set_control(NdsSpi *spi, uint16_t value)
{
    value &= NDS_SPICNT_WRITABLE;
    if (spi->selected) {
        uint32_t device = (uint32_t)(value & NDS_SPICNT_DEVICE) >> 8;
        if (!(value & NDS_SPICNT_ENABLE) || device + 1u != spi->selected) deselect(spi);
    }
    spi->control = value;
}

/*
 * One write to SPIDATA. In the "bugged" 16-bit mode (SPICNT bit 10) the bus
 * clocks sixteen bits, but "the SPIDATA register is restricted to 8bit, so
 * that only each 2nd byte will appear in SPIDATA": the device gets the byte
 * written and then a zero byte, and SPIDATA keeps the reply to the second.
 * (Which of the two bytes goes first is not documented; no game is known
 * to use the mode.)
 */
static bool transfer(NdsSpi *spi, uint8_t *firmware, uint8_t out)
{
    if (!(spi->control & NDS_SPICNT_ENABLE)) return false;
    uint32_t device = (uint32_t)(spi->control & NDS_SPICNT_DEVICE) >> 8;
    spi->selected = (uint8_t)(device + 1u);
    uint8_t in = exchange(spi, firmware, device, out);
    if (spi->control & NDS_SPICNT_16BIT) in = exchange(spi, firmware, device, 0);
    spi->data = in;
    if (!(spi->control & NDS_SPICNT_HOLD)) deselect(spi);
    return (spi->control & NDS_SPICNT_IRQ) != 0;
}

void nds_spi_reset(NdsSpi *spi)
{
    memset(spi, 0, sizeof *spi);
    spi->power_reg[NDS_POWER_REG_CONTROL] =
        NDS_POWER_SOUND_AMP | NDS_POWER_BACKLIGHT_LOWER | NDS_POWER_BACKLIGHT_UPPER;
    spi->microphone = 0x800u;
}

uint32_t nds_spi_read(const NdsSpi *spi, uint32_t addr, int size)
{
    uint32_t value = 0;
    uint32_t first = addr & 3u;
    for (int i = 0; i < size && i < 4; i++) {
        uint32_t byte = 0;
        switch (first + (uint32_t)i) {
        case 0: byte = spi->control & 0xFFu; break;
        case 1: byte = (uint32_t)spi->control >> 8; break;
        /* SPIDATA holds the received byte while the bus is on. */
        case 2: byte = (spi->control & NDS_SPICNT_ENABLE) ? spi->data : 0u; break;
        default: break; /* SPIDATA's high byte is always zero */
        }
        value |= byte << (8 * i);
    }
    return value;
}

bool nds_spi_write(NdsSpi *spi, uint8_t *firmware, uint32_t addr, uint32_t value, int size)
{
    uint32_t first = addr & 3u;
    uint32_t control = spi->control;
    bool control_written = false;
    bool data_written = false;
    uint8_t data = 0;

    for (int i = 0; i < size && i < 4; i++) {
        uint32_t byte = (value >> (8 * i)) & 0xFFu;
        switch (first + (uint32_t)i) {
        case 0:
            control = (control & 0xFF00u) | byte;
            control_written = true;
            break;
        case 1:
            control = (control & 0x00FFu) | (byte << 8);
            control_written = true;
            break;
        case 2:
            data = (uint8_t)byte;
            data_written = true;
            break;
        default: break;
        }
    }
    if (control_written) set_control(spi, (uint16_t)control);
    return data_written ? transfer(spi, firmware, data) : false;
}
