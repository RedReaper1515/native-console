/*
 * Cartridge backup memory: the save chip on the card's AUXSPI port (GBATEK,
 * "DS Cartridge Backup"; the port itself is AUXSPICNT at 0x040001A0 and
 * AUXSPIDATA at 0x040001A2, "DS Cartridge I/O Ports").
 *
 * A card has one serial chip for its save, of one of these families:
 *
 *   EEPROM 0.5 KiB  one address byte, a ninth address bit in the command
 *   EEPROM 8-64 KiB two address bytes
 *   EEPROM 128 KiB  three address bytes
 *   FRAM 8-32 KiB   as the two-byte EEPROM, without its limit on how much
 *                   one write command can store
 *   FLASH 256 KiB+  three address bytes and the command set of the firmware
 *                   flash: page write, page program, page and sector erase
 *
 * The machine owns the port's registers and the save memory. It hands the
 * chip each byte the program sends while the chip is selected and tells it
 * when chip select is released, which ends the command:
 *
 *   - a write to AUXSPIDATA with the port enabled in serial mode
 *     (AUXSPICNT bits 15 and 13) is one nds_save_transfer();
 *   - if AUXSPICNT bit 6 ("hold") was clear for that write, the chip is
 *     deselected straight after it: nds_save_deselect();
 *   - the chip is also deselected when AUXSPICNT is rewritten with the port
 *     or its serial mode off. libnds ends every command that way, with hold
 *     still set.
 *
 * Transfers and writes complete at once: the chips' "write in progress"
 * status bit is never set.
 *
 * Cards with an infrared port (game codes starting with 'I') have a small
 * processor between the port and the chip: see nds_save_set_infrared().
 */
#ifndef NDS_SAVE_H
#define NDS_SAVE_H

#include "nds_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NdsSaveType {
    NDS_SAVE_NONE,
    NDS_SAVE_EEPROM_SMALL, /* 0.5 KiB, 1 address byte */
    NDS_SAVE_EEPROM,       /* 2 address bytes */
    NDS_SAVE_EEPROM_LARGE, /* 3 address bytes */
    NDS_SAVE_FLASH,
    NDS_SAVE_FRAM,
    NDS_SAVE_UNKNOWN, /* found out from what the program sends: see nds_save.c */
    NDS_SAVE_NAND,    /* the save is part of the card's ROM chip: nothing on AUXSPI */
} NdsSaveType;

/* The longest write command the detection of an unknown chip holds back. */
#define NDS_SAVE_BUFFER 260

/*
 * Type, size and protocol state. Plain data; the memory itself is the
 * caller's.
 *
 * type and size are what the chip is. For NDS_SAVE_UNKNOWN they change
 * once, when the chip is recognised: type to the family found and size to
 * that family's size (never more than the size given to nds_save_init()).
 */
typedef struct NdsSave {
    uint32_t size;      /* bytes of memory: a power of two, or 0 */
    uint32_t capacity;  /* the size given to nds_save_init(), as a power of two */
    uint32_t address;   /* where the next data byte comes from or goes */
    uint16_t page;      /* bytes one write command can change; 0 = no limit */
    uint16_t held;      /* detection: bytes of a write command held back */
    uint8_t type;       /* NdsSaveType */
    uint8_t state;
    uint8_t command;
    uint8_t count;      /* address bytes or ID bytes so far */
    uint8_t status;     /* status register: write enable latch, protection bits */
    uint8_t wrote;      /* this command wrote: the latch drops when it ends */
    uint8_t asleep;     /* FLASH deep power-down */
    uint8_t pending;    /* detection: the held command waits to be stored */
    uint8_t guessed;    /* the type was found out, not given */
    uint8_t infrared;   /* the card has the infrared processor */
    uint8_t ir_state;
    uint8_t ir_command;
    uint8_t buffer[NDS_SAVE_BUFFER]; /* a FLASH page as a page program found it, or the held command */
} NdsSave;

/*
 * Type and size for a game code from the built-in table; false if the game
 * is not in it (start such a game as NDS_SAVE_UNKNOWN).
 *
 * NDS_SAVE_NONE and NDS_SAVE_NAND both mean that no chip answers on the
 * save port. For NDS_SAVE_NAND the size is that of the card's writable
 * area, which is reached through card commands and is not this module's;
 * most games listed that way are demo and distribution cards that save
 * nothing at all (see tools/nds/gen_save_db.py).
 */
bool nds_save_lookup(const char game_code[4], NdsSaveType *type, uint32_t *size);

/*
 * size is the size of the caller's memory in bytes and should be a power of
 * two (if it is not, the chip uses the largest power of two that fits). A
 * family that cannot be that large uses less.
 *
 * For NDS_SAVE_UNKNOWN, size is the most the chip may turn out to have (1
 * MiB covers every chip but one 8 MiB FLASH), and the memory should be
 * filled with FFh, as a new chip is.
 */
void nds_save_init(NdsSave *save, NdsSaveType type, uint32_t size);

/*
 * The console was switched off and on with the card in it. The chip is the
 * same chip over the same memory, so call this, not nds_save_init(), when
 * the machine is reset: nds_save_init() is for a card being put in (a ROM
 * or a save file loaded), and must be called then, as the state describes
 * the memory's size.
 *
 * The chip comes up between commands, awake, with the write enable latch
 * clear. What is kept: its type and size, what has been found out about an
 * unknown chip, its protection bits (they are non-volatile), and the
 * infrared setting.
 */
void nds_save_reset(NdsSave *save);

/* One byte exchanged while chip select is held. data is the save memory
 * (size bytes, as given to nds_save_init()). Sets *dirty when memory
 * changed; never clears it. */
uint8_t nds_save_transfer(NdsSave *save, uint8_t *data, uint8_t in, bool *dirty);

/* Chip select released: the command ends. */
void nds_save_deselect(NdsSave *save);

/*
 * Cards with an infrared port pass the save chip's bytes through their
 * infrared processor, and a command for the chip must be led by a 00h byte
 * (GBATEK, "DS Cart Infrared Cartridge SPI Commands"). With this set, the
 * first byte after chip select goes to that processor: 00h hands the rest
 * of the command to the chip, 08h is answered with the processor's version,
 * AAh, and anything else gets zeros (no infrared data ever arrives).
 */
void nds_save_set_infrared(NdsSave *save, bool present);

/* Whether a game's card has the infrared port: such cards have a game code
 * with I as its first letter (GBATEK, "IR Cart Detection"). */
bool nds_save_has_infrared(const char game_code[4]);

/*
 * A family that reads and writes a save file of this length, for a game
 * that is not in the table but already has a save; false for a length no
 * chip has. A length that an EEPROM with two address bytes and FRAM share
 * gives FRAM, which is the same chip to a program but for its having no
 * limit on the length of a write.
 */
bool nds_save_type_for_size(uint32_t size, NdsSaveType *type);

/*
 * The table (nds_save_db.c, generated by tools/nds/gen_save_db.py): one word
 * for each game, in ascending order. Bits 4 and up are the game code read
 * as a four-digit number in base 36 ('0'-'9' then 'A'-'Z'); bits 0-3 are an
 * index into the kinds.
 */
typedef struct NdsSaveKind {
    uint32_t size;
    uint8_t type; /* NdsSaveType */
} NdsSaveKind;

extern const uint32_t nds_save_db[];
extern const uint32_t nds_save_db_count;
extern const NdsSaveKind nds_save_db_kinds[];
extern const uint32_t nds_save_db_kind_count;

#ifdef __cplusplus
}
#endif

#endif /* NDS_SAVE_H */
