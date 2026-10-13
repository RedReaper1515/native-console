/*
 * Cartridge backup memory: the serial save chips (see nds_save.h; GBATEK,
 * "DS Cartridge Backup" and, for FLASH, "DS Firmware Serial Flash Memory").
 *
 * The first byte after chip select is a command. Every family has:
 *
 *   06h WREN  set the write enable latch (WEL)
 *   04h WRDI  clear it
 *   05h RDSR  then read the status register, as often as wanted
 *
 * EEPROM and FRAM:
 *
 *   01h WRSR  then one byte: sets the protection bits. Needs WEL.
 *   03h RD    address, then read from there on
 *   02h WR    address, then the bytes to store. Needs WEL.
 *   9Fh RDID  "not supported ..., returns FFh-bytes"
 *
 *   The address is one byte for the 0.5 KiB EEPROM, whose ninth address
 *   bit is bit 3 of the command (0Bh reads and 0Ah writes 100h-1FFh), two
 *   bytes for the 8-64 KiB EEPROMs and FRAM, three for the 128 KiB EEPROM;
 *   most significant byte first. Address bits the chip does not have are
 *   ignored, so the memory repeats.
 *
 *   A write command changes one page at most: 16 bytes on the 0.5 KiB
 *   chip, 32 on the 8 KiB, 128 on the 64 KiB, 256 on the 128 KiB. Past the
 *   end of the page the address goes back to the page's start (the ST
 *   M95xxx datasheets). FRAM has no pages: the address runs through the
 *   whole memory. A read runs on in every family, from the last byte to
 *   the first.
 *
 *   Status register: bit 0 write in progress, bit 1 WEL, bits 2-3
 *   protection (0 none, 1 the upper quarter, 2 the upper half, 3 all of
 *   memory). On the 0.5 KiB EEPROM bits 4-7 read 1. On the others bits 4-6
 *   read 0 and bit 7 is a lock that only works with the chip's /W pin low,
 *   which the DS ties high: it is stored and read back, nothing more.
 *
 * FLASH has the firmware chip's commands instead of WRSR, RD and WR:
 *
 *   03h READ  address, then read          0Bh FAST  address, a dummy byte, read
 *   0Ah PW    address, then 1-256 bytes to store (page write)
 *   02h PP    address, then 1-256 bytes to AND into the page (page program)
 *   DBh PE    address: the page of 100h bytes becomes FFh
 *   D8h SE    address: the sector of 10000h bytes becomes FFh
 *   9Fh RDID  then read three ID bytes
 *   B9h DP    deep power-down: nothing but ABh RDP is obeyed after it
 *
 *   Addresses are three bytes; PW, PP, PE and SE need WEL. Its status
 *   register has only bits 0 and 1. The ID is that of the chips GBATEK
 *   lists: 20h, 40h and the capacity (12h = 256 KiB, 13h = 512 KiB, 14h =
 *   1 MiB: the byte is the base-2 logarithm of the size), and C2h, 20h, 17h
 *   for the 8 MiB chip.
 *
 * "WEL gets reset on Power-up, WRDI, WRSR, WRITE/LO/HI" and by the FLASH
 * write and erase commands, when the command ends. While the chip is not
 * sending anything the line reads FFh, as it does with no chip at all.
 *
 * Differences from the chips: writing takes no time, so "write in progress"
 * is never set; bytes are stored as they arrive and an erase is done when
 * its last address byte arrives, where the chips wait for chip select to
 * rise (nds_save_deselect() is not given the memory).
 *
 * Finding out an unknown chip
 * ---------------------------
 * A game not in the table starts as NDS_SAVE_UNKNOWN. Nothing in what a
 * program sends says outright which chip it expects, but the first command
 * that changes memory gives it away, because programs write whole pages and
 * every page size is a multiple of four:
 *
 *   - 9Fh, DBh, D8h, B9h or ABh: only FLASH has these. Recognised at once.
 *   - The first write command, 02h or 0Ah, is held back until chip select
 *     is released, and the n bytes that followed the command byte are
 *     counted. The address is taken to be n - 1 bytes long if n is 4 or
 *     less (one data byte), and otherwise n modulo 4 bytes (data in
 *     multiples of four). This is the rule DeSmuME's detection uses, here
 *     applied to writes only and checked against what each family allows:
 *       02h, 1 address byte, 16 data bytes or fewer   EEPROM 0.5 KiB
 *       02h, 2 address bytes, 128 data bytes or fewer EEPROM, taken as 64 KiB
 *       02h, 2 address bytes, more data than that     FRAM, taken as 32 KiB
 *       02h, 3 address bytes                          EEPROM 128 KiB
 *       0Ah, 3 address bytes                          FLASH
 *       0Ah otherwise, 16 data bytes or fewer         EEPROM 0.5 KiB
 *       0Ah otherwise                                 FLASH
 *     A command longer than any page (more than 259 bytes) is recognised
 *     while it is still arriving: FRAM for 02h, FLASH for 0Ah.
 *     A 02h command that fits none of these (a count that is a multiple of
 *     four, say) is dropped and the next one is tried.
 *   - The held command is stored once the chip is known, at the start of
 *     the next byte exchanged (the memory is not at hand when chip select
 *     is released). A program cannot see the delay: its next move is to
 *     poll the status register.
 *
 * Until then reads return FFh, which is what a new chip holds, and the
 * status register reads as the two-address-byte chips' does. A recognised
 * EEPROM with two address bytes gets no page limit, as its page size is not
 * known; a program that keeps to its chip's pages cannot tell.
 *
 * "FLASH has same 24bit bus-width as 128Kbyte EEPROM, but isn't compatible
 * on writing": a 02h write with three address bytes is the EEPROM's write
 * and FLASH's page program alike. It is taken for the EEPROM's; if one of
 * the FLASH-only commands comes later, the chip is FLASH after all and is
 * treated as such from there on. (On memory that was all FFh the two
 * commands leave the same bytes.)
 *
 * The rule is a guess and can be wrong: one address byte and two data
 * bytes look like two address bytes and one. That is what the table is
 * for; and a game with an existing save should be started with the type
 * nds_save_type_for_size() gives for it, not as unknown, or its first
 * reads would see an empty chip.
 */
#include <string.h>

#include "nds_save.h"

#define BUS_IDLE 0xFFu

#define STATUS_WEL 0x02u
#define STATUS_NOT_WEL 0xFDu
#define STATUS_PROTECT 0x0Cu
#define STATUS_LOCK 0x80u

enum {
    CMD_WRSR = 0x01,
    CMD_WRITE = 0x02, /* FLASH: page program */
    CMD_READ = 0x03,
    CMD_WRDI = 0x04,
    CMD_RDSR = 0x05,
    CMD_WREN = 0x06,
    CMD_WRITE_HIGH = 0x0A, /* FLASH: page write */
    CMD_READ_HIGH = 0x0B,  /* FLASH: fast read */
    CMD_RDID = 0x9F,
    CMD_RDP = 0xAB,
    CMD_DP = 0xB9,
    CMD_SE = 0xD8,
    CMD_PE = 0xDB,
};

enum {
    STATE_COMMAND, /* chip select has fallen: the next byte is a command */
    STATE_ADDRESS,
    STATE_DUMMY,
    STATE_READ,
    STATE_WRITE,
    STATE_STATUS,
    STATE_WRITE_STATUS,
    STATE_ID,
    STATE_HOLD,   /* unknown chip: a write command is being held back */
    STATE_IGNORE, /* nothing more until chip select rises */
};

enum { IR_COMMAND, IR_SAVE, IR_OTHER };

#define FLASH_PAGE 0x100u
#define FLASH_SECTOR 0x10000u

/* ------------------------------------------------------------------ */
/* Setting up                                                          */
/* ------------------------------------------------------------------ */

/* The largest power of two that is not more than n. */
static uint32_t floor_power_of_two(uint32_t n)
{
    uint32_t power = n ? 1u : 0u;
    while (power && power <= n / 2u) power *= 2u;
    return power;
}

/* Make the chip one of the families with real memory, of `size` bytes or as
 * many as the family can have. */
static void set_type(NdsSave *save, NdsSaveType type, uint32_t size)
{
    uint32_t most = 0;
    uint32_t page = 0;

    switch (type) {
    case NDS_SAVE_EEPROM_SMALL:
        most = 0x200u;
        page = 16;
        break;
    case NDS_SAVE_EEPROM:
    case NDS_SAVE_FRAM:
        most = 0x10000u; /* two address bytes */
        break;
    case NDS_SAVE_EEPROM_LARGE:
    case NDS_SAVE_FLASH:
        most = 0x1000000u; /* three */
        page = 256;
        break;
    default: break; /* no chip on the port */
    }
    if (size > most) size = most;
    if (type == NDS_SAVE_EEPROM) page = size <= 0x2000u ? 32u : size <= 0x8000u ? 64u : 128u;
    save->type = (uint8_t)(size ? type : NDS_SAVE_NONE);
    save->size = size;
    save->page = (uint16_t)page;
}

void nds_save_init(NdsSave *save, NdsSaveType type, uint32_t size)
{
    memset(save, 0, sizeof *save);
    save->capacity = floor_power_of_two(size);
    if (type == NDS_SAVE_UNKNOWN) {
        save->type = (uint8_t)(save->capacity ? NDS_SAVE_UNKNOWN : NDS_SAVE_NONE);
        save->size = save->capacity;
    } else {
        set_type(save, type, save->capacity);
    }
}

void nds_save_reset(NdsSave *save)
{
    /* "WEL gets reset on Power-up". A first write already recognised and
     * waiting to be stored still needs the latch it arrived under; storing
     * it clears the latch. A command cut off by the power going is
     * forgotten: the next byte starts one. (The command byte is left alone:
     * it is the waiting write's.) */
    uint8_t kept = STATUS_PROTECT | STATUS_LOCK;
    if (save->pending) kept |= STATUS_WEL;
    save->status &= kept;
    save->state = STATE_COMMAND;
    save->wrote = 0;
    save->asleep = 0;
    save->ir_state = IR_COMMAND;
}

void nds_save_set_infrared(NdsSave *save, bool present)
{
    save->infrared = present ? 1u : 0u;
    save->ir_state = IR_COMMAND;
}

bool nds_save_has_infrared(const char game_code[4])
{
    return game_code[0] == 'I';
}

/*
 * For the two-address-byte sizes the answer is FRAM, which differs from the
 * EEPROM only in having no page limit: a save file does not say what the
 * chip's page size was, and without a limit a program written for either
 * chip works.
 */
bool nds_save_type_for_size(uint32_t size, NdsSaveType *type)
{
    if (size == 0x200u) *type = NDS_SAVE_EEPROM_SMALL;
    else if (size == 0x2000u || size == 0x4000u || size == 0x8000u || size == 0x10000u) *type = NDS_SAVE_FRAM;
    else if (size == 0x20000u) *type = NDS_SAVE_EEPROM_LARGE;
    else if (size >= 0x40000u && size <= 0x1000000u && floor_power_of_two(size) == size) *type = NDS_SAVE_FLASH;
    else return false;
    return true;
}

bool nds_save_lookup(const char game_code[4], NdsSaveType *type, uint32_t *size)
{
    uint32_t key = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)game_code[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'Z') digit = (uint32_t)(c - 'A') + 10u;
        else return false;
        key = key * 36u + digit;
    }

    uint32_t low = 0;
    uint32_t high = nds_save_db_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uint32_t code = nds_save_db[middle] >> 4;
        if (code == key) {
            uint32_t kind = nds_save_db[middle] & 15u;
            if (kind >= nds_save_db_kind_count) return false;
            *type = (NdsSaveType)nds_save_db_kinds[kind].type;
            *size = nds_save_db_kinds[kind].size;
            return true;
        }
        if (code < key) low = middle + 1u;
        else high = middle;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* A chip of a known family                                            */
/* ------------------------------------------------------------------ */

static bool is_flash(const NdsSave *save)
{
    return save->type == NDS_SAVE_FLASH;
}

static uint32_t address_bytes(const NdsSave *save)
{
    switch (save->type) {
    case NDS_SAVE_EEPROM_SMALL: return 1;
    case NDS_SAVE_EEPROM:
    case NDS_SAVE_FRAM:
        return 2;
    default: return 3;
    }
}

static uint8_t status_value(const NdsSave *save)
{
    switch (save->type) {
    case NDS_SAVE_EEPROM_SMALL: return (uint8_t)(0xF0u | (save->status & (STATUS_PROTECT | STATUS_WEL)));
    case NDS_SAVE_FLASH: return (uint8_t)(save->status & STATUS_WEL);
    default: return (uint8_t)(save->status & (STATUS_LOCK | STATUS_PROTECT | STATUS_WEL));
    }
}

/* The EEPROMs' and FRAM's protection bits: "0=None, 1=Upper quarter,
 * 2=Upper Half, 3=All memory". */
static bool is_protected(const NdsSave *save, uint32_t address)
{
    if (is_flash(save)) return false;
    switch ((save->status & STATUS_PROTECT) >> 2) {
    case 1: return address >= save->size - save->size / 4u;
    case 2: return address >= save->size / 2u;
    case 3: return true;
    default: return false;
    }
}

static uint8_t flash_id(const NdsSave *save, uint32_t index)
{
    bool macronix = save->size == 0x800000u;
    uint32_t capacity = 0;
    while (capacity < 31u && (1u << capacity) < save->size) capacity++;
    if (index == 0) return macronix ? 0xC2u : 0x20u;
    if (index == 1) return macronix ? 0x20u : 0x40u;
    return (uint8_t)capacity;
}

static void flash_erase(NdsSave *save, uint8_t *data, uint32_t start, uint32_t length, bool *dirty)
{
    uint32_t mask = save->size - 1u;
    if (length > save->size) length = save->size;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t *cell = &data[(start + i) & mask];
        if (*cell != 0xFFu) {
            *cell = 0xFFu;
            *dirty = true;
        }
    }
    save->wrote = 1;
}

static void begin_command(NdsSave *save, uint8_t command)
{
    bool writable = (save->status & STATUS_WEL) != 0;
    bool small = save->type == NDS_SAVE_EEPROM_SMALL;

    save->command = command;
    save->count = 0;
    save->address = 0;
    save->state = STATE_IGNORE;
    if (save->asleep) {
        if (command == CMD_RDP) save->asleep = 0;
        return;
    }
    switch (command) {
    case CMD_WREN: save->status |= STATUS_WEL; break;
    case CMD_WRDI: save->status &= STATUS_NOT_WEL; break;
    case CMD_RDSR: save->state = STATE_STATUS; break;
    case CMD_WRSR:
        if (!is_flash(save) && writable) save->state = STATE_WRITE_STATUS;
        break;
    case CMD_READ: save->state = STATE_ADDRESS; break;
    case CMD_WRITE:
        if (writable) save->state = STATE_ADDRESS;
        break;
    case CMD_READ_HIGH:
    case CMD_WRITE_HIGH:
        /* The 0.5 KiB EEPROM's upper half, or FLASH's fast read and page
         * write; the other chips have neither. */
        if (!small && !is_flash(save)) break;
        if (command == CMD_WRITE_HIGH && !writable) break;
        save->address = small ? 1u : 0u; /* the ninth address bit */
        save->state = STATE_ADDRESS;
        break;
    case CMD_PE:
    case CMD_SE:
        if (is_flash(save) && writable) save->state = STATE_ADDRESS;
        break;
    case CMD_RDID:
        if (is_flash(save)) save->state = STATE_ID;
        break;
    case CMD_DP:
        if (is_flash(save)) save->asleep = 1;
        break;
    default: break;
    }
}

/* Commands FLASH has and the 128 KiB EEPROM does not. */
static bool flash_only(uint8_t command)
{
    switch (command) {
    case CMD_WRITE_HIGH:
    case CMD_READ_HIGH:
    case CMD_RDID:
    case CMD_RDP:
    case CMD_DP:
    case CMD_SE:
    case CMD_PE:
        return true;
    default: return false;
    }
}

static void recognise(NdsSave *save, NdsSaveType type);

/* The last address byte has arrived. */
static void addressed(NdsSave *save, uint8_t *data, bool *dirty)
{
    uint32_t mask = save->size - 1u;
    uint32_t address = save->address & mask;

    save->address = address;
    save->state = STATE_IGNORE;
    switch (save->command) {
    case CMD_READ: save->state = STATE_READ; break;
    case CMD_READ_HIGH: save->state = is_flash(save) ? STATE_DUMMY : STATE_READ; break;
    case CMD_WRITE:
        if (is_flash(save)) {
            /* Page program ANDs into the page as it is now. */
            uint32_t start = address & ~(FLASH_PAGE - 1u);
            for (uint32_t i = 0; i < FLASH_PAGE; i++) save->buffer[i] = data[(start + i) & mask];
        }
        save->state = STATE_WRITE;
        break;
    case CMD_WRITE_HIGH: save->state = STATE_WRITE; break;
    case CMD_PE: flash_erase(save, data, address & ~(FLASH_PAGE - 1u), FLASH_PAGE, dirty); break;
    case CMD_SE: flash_erase(save, data, address & ~(FLASH_SECTOR - 1u), FLASH_SECTOR, dirty); break;
    default: break;
    }
}

static void write_byte(NdsSave *save, uint8_t *data, uint8_t in, bool *dirty)
{
    uint32_t mask = save->size - 1u;
    uint32_t address = save->address & mask;
    uint8_t value = in;

    if (is_flash(save) && save->command == CMD_WRITE) {
        value = (uint8_t)(save->buffer[address & (FLASH_PAGE - 1u)] & in);
    }
    if (!is_protected(save, address)) {
        if (data[address] != value) {
            data[address] = value;
            *dirty = true;
        }
        save->wrote = 1;
    }
    if (save->page) {
        uint32_t within = (uint32_t)save->page - 1u;
        save->address = (address & ~within) | ((address + 1u) & within);
    } else {
        save->address = (address + 1u) & mask;
    }
}

static uint8_t chip_transfer(NdsSave *save, uint8_t *data, uint8_t in, bool *dirty)
{
    uint32_t mask = save->size - 1u;

    switch (save->state) {
    case STATE_COMMAND:
        if (save->guessed && save->type == NDS_SAVE_EEPROM_LARGE && flash_only(in)) recognise(save, NDS_SAVE_FLASH);
        begin_command(save, in);
        return BUS_IDLE;
    case STATE_ADDRESS:
        save->address = ((save->address << 8) | in) & 0x1FFFFFFu;
        if (++save->count >= address_bytes(save)) addressed(save, data, dirty);
        return BUS_IDLE;
    case STATE_DUMMY:
        save->state = STATE_READ;
        return BUS_IDLE;
    case STATE_READ: {
        uint32_t address = save->address & mask;
        save->address = (address + 1u) & mask;
        return data[address];
    }
    case STATE_WRITE:
        write_byte(save, data, in, dirty);
        return BUS_IDLE;
    case STATE_STATUS: return status_value(save);
    case STATE_WRITE_STATUS: {
        /* "The WRSR command allows to change ONLY the two WP bits, and the
         * SRWD bit (if any)". */
        uint8_t writable = save->type == NDS_SAVE_EEPROM_SMALL ? STATUS_PROTECT : (STATUS_PROTECT | STATUS_LOCK);
        save->status = (uint8_t)((save->status & (uint8_t)~writable) | (in & writable));
        save->wrote = 1;
        save->state = STATE_IGNORE;
        return BUS_IDLE;
    }
    case STATE_ID: {
        uint8_t id = BUS_IDLE;
        if (save->count < 3) {
            id = flash_id(save, save->count);
            save->count++;
        }
        return id;
    }
    default: return BUS_IDLE;
    }
}

/* What chip select rising does to a chip. */
static void end_command(NdsSave *save)
{
    if (save->wrote) save->status &= STATUS_NOT_WEL;
    save->wrote = 0;
    save->state = STATE_COMMAND;
}

/* ------------------------------------------------------------------ */
/* An unknown chip                                                     */
/* ------------------------------------------------------------------ */

/* The chip is of this family. An EEPROM found this way gets no page limit
 * (see the top of the file). */
static void recognise(NdsSave *save, NdsSaveType type)
{
    uint32_t size = save->capacity;
    save->guessed = 1;
    if (type == NDS_SAVE_EEPROM_LARGE && size > 0x20000u) size = 0x20000u;
    if (type == NDS_SAVE_FRAM && size > 0x8000u) size = 0x8000u;
    set_type(save, type, size);
    if (type == NDS_SAVE_EEPROM) save->page = 0;
}

/* Send the held write command to the chip it turned out to be for. With
 * `ended` the command was complete; otherwise it is still arriving. */
static void replay(NdsSave *save, uint8_t *data, bool *dirty, bool ended)
{
    uint8_t bytes[NDS_SAVE_BUFFER];
    uint32_t count = save->held <= NDS_SAVE_BUFFER ? save->held : NDS_SAVE_BUFFER;
    uint8_t command = save->command;

    memcpy(bytes, save->buffer, count);
    save->held = 0;
    save->pending = 0;
    save->state = STATE_COMMAND;
    chip_transfer(save, data, command, dirty);
    for (uint32_t i = 0; i < count; i++) chip_transfer(save, data, bytes[i], dirty);
    if (ended) end_command(save);
}

/* Chip select has risen on a held write command of save->held bytes. */
static void recognise_from_write(NdsSave *save)
{
    uint32_t n = save->held;
    uint32_t width = n <= 4u ? (n ? n - 1u : 0u) : (n & 3u);
    NdsSaveType type = NDS_SAVE_UNKNOWN;

    if (n >= 2u) {
        if (save->command == CMD_WRITE_HIGH) {
            if (width == 3u) type = NDS_SAVE_FLASH;
            else type = n - 1u <= 16u ? NDS_SAVE_EEPROM_SMALL : NDS_SAVE_FLASH;
        } else if (width == 1u) {
            if (n - 1u <= 16u) type = NDS_SAVE_EEPROM_SMALL;
        } else if (width == 2u) {
            type = n - 2u <= 128u ? NDS_SAVE_EEPROM : NDS_SAVE_FRAM;
        } else if (width == 3u) {
            type = NDS_SAVE_EEPROM_LARGE;
        }
    }
    if (type == NDS_SAVE_UNKNOWN) {
        save->held = 0; /* no telling: the command is dropped */
        return;
    }
    recognise(save, type);
    save->pending = 1;
}

static uint8_t unknown_transfer(NdsSave *save, uint8_t *data, uint8_t in, bool *dirty)
{
    switch (save->state) {
    case STATE_COMMAND:
        save->command = in;
        save->held = 0;
        save->state = STATE_IGNORE;
        switch (in) {
        case CMD_WREN: save->status |= STATUS_WEL; break;
        case CMD_WRDI: save->status &= STATUS_NOT_WEL; break;
        case CMD_RDSR: save->state = STATE_STATUS; break;
        case CMD_WRSR:
            if (save->status & STATUS_WEL) save->state = STATE_WRITE_STATUS;
            break;
        case CMD_WRITE:
        case CMD_WRITE_HIGH:
            save->state = STATE_HOLD;
            break;
        case CMD_RDID:
        case CMD_PE:
        case CMD_SE:
        case CMD_DP:
        case CMD_RDP:
            recognise(save, NDS_SAVE_FLASH);
            save->state = STATE_COMMAND;
            return chip_transfer(save, data, in, dirty);
        default: break; /* a read: a new chip holds FFh, which is the idle line */
        }
        return BUS_IDLE;
    case STATE_HOLD:
        if (save->held < NDS_SAVE_BUFFER) save->buffer[save->held++] = in;
        if (save->held >= NDS_SAVE_BUFFER) {
            /* More than three address bytes and a 256-byte page. */
            recognise(save, save->command == CMD_WRITE_HIGH ? NDS_SAVE_FLASH : NDS_SAVE_FRAM);
            replay(save, data, dirty, false);
        }
        return BUS_IDLE;
    case STATE_STATUS: return (uint8_t)(save->status & (STATUS_LOCK | STATUS_PROTECT | STATUS_WEL));
    case STATE_WRITE_STATUS:
        save->status = (uint8_t)((save->status & STATUS_WEL) | (in & (STATUS_PROTECT | STATUS_LOCK)));
        save->wrote = 1;
        save->state = STATE_IGNORE;
        return BUS_IDLE;
    default: return BUS_IDLE;
    }
}

/* ------------------------------------------------------------------ */
/* The port                                                            */
/* ------------------------------------------------------------------ */

static bool has_chip(const NdsSave *save)
{
    switch (save->type) {
    case NDS_SAVE_EEPROM_SMALL:
    case NDS_SAVE_EEPROM:
    case NDS_SAVE_EEPROM_LARGE:
    case NDS_SAVE_FLASH:
    case NDS_SAVE_FRAM:
        return save->size != 0;
    default: return false;
    }
}

uint8_t nds_save_transfer(NdsSave *save, uint8_t *data, uint8_t in, bool *dirty)
{
    bool unused = false;
    if (!dirty) dirty = &unused;

    if (save->pending && has_chip(save)) replay(save, data, dirty, true);
    save->pending = 0;

    /*
     * The infrared processor (GBATEK, "DS Cart Infrared Cartridge SPI
     * Commands") takes the first byte: "00h,cmd,params[...] Savedata
     * access", "08h,ver NEW: Returns version (ver=AAh)". Its other commands
     * read and send infrared data, of which there is none: a receive
     * reports a length of zero.
     */
    if (save->infrared) {
        if (save->ir_state == IR_COMMAND) {
            save->ir_command = in;
            save->ir_state = in == 0x00u ? IR_SAVE : IR_OTHER;
            return 0x00u;
        }
        if (save->ir_state != IR_SAVE) return save->ir_command == 0x08u ? 0xAAu : 0x00u;
    }

    if (has_chip(save)) return chip_transfer(save, data, in, dirty);
    if (save->type == NDS_SAVE_UNKNOWN) return unknown_transfer(save, data, in, dirty);
    return BUS_IDLE; /* no chip: GBATEK has RDSR and RDID both reading FFh */
}

void nds_save_deselect(NdsSave *save)
{
    if (save->type == NDS_SAVE_UNKNOWN && save->state == STATE_HOLD) recognise_from_write(save);
    end_command(save);
    save->ir_state = IR_COMMAND;
}
