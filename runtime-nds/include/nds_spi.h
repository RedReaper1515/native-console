/*
 * The ARM7's SPI bus and the three devices on it (GBATEK, "DS Serial
 * Peripheral Interface Bus (SPI)"):
 *
 *   device 0  the power management chip    "DS Power Management Device"
 *   device 1  the firmware flash           "DS Firmware Serial Flash Memory"
 *   device 2  the touch screen controller  "DS Touch Screen Controller (TSC)"
 *
 * Two registers: SPICNT at 0x040001C0 selects a device, holds its chip
 * select across bytes and enables the interrupt; a write to SPIDATA at
 * 0x040001C2 sends one byte and receives one, which SPIDATA then reads.
 *
 * Transfers complete at once. The hardware takes 64 to 512 system cycles a
 * byte, by SPICNT's baud rate, and shows a busy flag meanwhile; here the
 * received byte is there when nds_spi_write() returns and busy is never
 * set. Software that polls busy sees the transfer already finished. The
 * flash's write and erase times are left out the same way: its "write in
 * progress" status bit is never set.
 *
 * The console described is the original DS (see nds_firmware.h): the power
 * chip has four registers and the flash is the 256 KiB part.
 */
#ifndef NDS_SPI_H
#define NDS_SPI_H

#include "nds_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_SPICNT_ADDRESS 0x040001C0u
#define NDS_SPIDATA_ADDRESS 0x040001C2u

/* SPICNT. Bits 2-6, 12 and 13 are unused and read 0. */
#define NDS_SPICNT_BAUD 0x0003u     /* 0-3: 4, 2, 1 MHz, 512 kHz */
#define NDS_SPICNT_BUSY 0x0080u     /* read-only; never set here */
#define NDS_SPICNT_DEVICE 0x0300u   /* NDS_SPI_DEVICE_* << 8 */
#define NDS_SPICNT_16BIT 0x0400u    /* "bugged": see nds_spi.c */
#define NDS_SPICNT_HOLD 0x0800u     /* keep chip select after the transfer */
#define NDS_SPICNT_IRQ 0x4000u      /* interrupt when a transfer finishes */
#define NDS_SPICNT_ENABLE 0x8000u
#define NDS_SPICNT_WRITABLE 0xCF03u

enum {
    NDS_SPI_DEVICE_POWER = 0,
    NDS_SPI_DEVICE_FIRMWARE = 1,
    NDS_SPI_DEVICE_TOUCH = 2,
    NDS_SPI_DEVICE_NONE = 3, /* reserved: nothing answers */
};

/* The power management chip's registers and the bits of register 0. */
enum {
    NDS_POWER_REG_CONTROL = 0,
    NDS_POWER_REG_BATTERY = 1,  /* read-only; bit 0: 0 = good, 1 = low */
    NDS_POWER_REG_MIC_AMP = 2,  /* bit 0: microphone amplifier on */
    NDS_POWER_REG_MIC_GAIN = 3, /* bits 0-1: gain 20, 40, 80, 160 */
};
#define NDS_POWER_SOUND_AMP 0x01u
#define NDS_POWER_SOUND_MUTE 0x02u
#define NDS_POWER_BACKLIGHT_LOWER 0x04u
#define NDS_POWER_BACKLIGHT_UPPER 0x08u
#define NDS_POWER_LED_BLINK 0x10u
#define NDS_POWER_LED_FAST 0x20u
#define NDS_POWER_SHUTDOWN 0x40u /* the program has switched the console off */

/* Bus state and the three devices' state. Plain data; the firmware image
 * the flash reads and writes is the caller's. */
typedef struct NdsSpi {
    /* The bus. */
    uint16_t control; /* SPICNT */
    uint8_t data;     /* the byte the last transfer received */
    uint8_t selected; /* the device whose chip select is held, plus 1; 0 = none */

    /* Power management. */
    uint8_t power_index;   /* the index byte of the access in progress */
    uint8_t power_indexed; /* an index byte has arrived since chip select */
    uint8_t power_reg[4];

    /* Touch screen controller. */
    uint16_t touch_shift;   /* the conversion still to be clocked out, from bit 15 */
    uint16_t touch_x;       /* 12-bit readings while the pen is down */
    uint16_t touch_y;
    uint16_t microphone;    /* the 12-bit reading of the AUX input */
    uint8_t touch_down;
    uint8_t touch_control;  /* the last control byte */

    /* Firmware flash. */
    uint32_t flash_address;      /* where the next data byte comes from or goes */
    uint8_t flash_instruction;
    uint8_t flash_state;
    uint8_t flash_count;         /* address bytes or ID bytes so far */
    uint8_t flash_write_enabled; /* the status register's WEL bit */
    uint8_t flash_wrote;         /* this instruction wrote or erased: WEL drops when it ends */
    uint8_t flash_powered_down;
    uint8_t flash_dirty;
    uint8_t flash_page[256];     /* the page as it was when a page program began */
} NdsSpi;

/* Power-on state, as the firmware leaves it when it starts a game: the bus
 * off, sound amplifier and both backlights on, microphone amplifier off,
 * the pen up, the microphone silent (800h), the flash not dirty. */
void nds_spi_reset(NdsSpi *spi);

/* 0x040001C0-0x040001C3, size 1, 2 or 4. */
uint32_t nds_spi_read(const NdsSpi *spi, uint32_t addr, int size);

/*
 * firmware is the NDS_FIRMWARE_SIZE image (the flash device reads and writes
 * it). Returns true when the write completed a transfer with the interrupt
 * enabled, so the machine raises the SPI interrupt (ARM7 interrupt 23).
 *
 * A write that covers SPIDATA's low byte (0x040001C2) is a transfer; a
 * write that covers both registers sets SPICNT first.
 */
bool nds_spi_write(NdsSpi *spi, uint8_t *firmware, uint32_t addr, uint32_t value, int size);

/*
 * What the touch controller and the microphone channel will report until
 * told otherwise. adc_x and adc_y are the 12-bit readings of a touch
 * (nds_touch_adc_from_screen() makes them from a pixel); with the pen up
 * they are ignored and the controller reports what an untouched panel does.
 * sample12 is unsigned, silence at 800h.
 */
void nds_spi_set_touch(NdsSpi *spi, bool down, uint16_t adc_x, uint16_t adc_y);
void nds_spi_set_microphone(NdsSpi *spi, uint16_t sample12);

/*
 * The controller's pen interrupt output, which is what EXTKEYIN bit 6
 * shows (there, 0 = pen down): true while the pen is down and the output
 * is enabled. A control byte with bit 0 of its power-down mode set turns
 * the output off until one with the bit clear arrives.
 */
bool nds_spi_pen_irq(const NdsSpi *spi);

/* A power management register, NDS_POWER_REG_*. The machine acts on
 * NDS_POWER_SHUTDOWN and may act on the backlights and the amplifiers. */
uint8_t nds_spi_power_register(const NdsSpi *spi, uint32_t index);

/* Battery status as register 1 reports it. The battery starts good. */
void nds_spi_set_battery_low(NdsSpi *spi, bool low);

/* A game wrote the flash (it does: user settings). The flag is set when a
 * byte of the image actually changes, and stays set until cleared here or
 * by nds_spi_reset(): a machine that keeps the image in a file looks at the
 * flag before it resets the bus. */
bool nds_spi_firmware_dirty(const NdsSpi *spi);
void nds_spi_clear_firmware_dirty(NdsSpi *spi);

#ifdef __cplusplus
}
#endif

#endif /* NDS_SPI_H */
