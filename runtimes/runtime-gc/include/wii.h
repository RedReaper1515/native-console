/*
 * What a Wii has that a GameCube does not.
 *
 * The two consoles are one design. The Wii's processor (Broadway) is the
 * GameCube's (Gekko) run half as fast again, with four more BAT pairs; its
 * graphics chip (Hollywood) contains the GameCube's (Flipper) unchanged, so
 * the command processor, the pixel engine, the video interface, the sound
 * processor, the controller ports and the expansion bus are the registers
 * runtime-gc already implements. What is new:
 *
 *   - MEM2: 64 MB more main memory, at physical 0x10000000 (gc.h has how
 *     the two blocks are held as one).
 *   - Hollywood's own registers at physical 0x0d000000, mirrored at
 *     0x0d800000: the mailbox to the second processor, an interrupt
 *     controller that feeds one new line of the GameCube's, a timer, and
 *     general-purpose pins (the disc slot's light, the sensor bar's power,
 *     the video encoder's serial bus).
 *   - The disc drive, controller ports, expansion bus and audio interface
 *     answer at 0x0d006000 as well as at 0x0c006000.
 *   - No audio RAM. The sound processor's sample memory is main memory.
 *   - IOS (wii_ios.h): everything else - the disc, the flash filesystem,
 *     the remotes - is reached by asking the second processor.
 *
 * A GcMachine is a Wii when its processor's `machine` pointer leads to a
 * WiiState. That pointer already existed, was never used by the runtime, is
 * the host's rather than the machine's in a save state, and travels with
 * every copy of the machine (the graphics thread's shadow included) - so a
 * Wii adds nothing to GcMachine's layout, and a GameCube game's save states
 * from before the Wii existed still load.
 *
 * Which console a process runs is chosen once, before the first machine is
 * made (gc_console_select), because the sizes and clock rates that differ
 * are compiled into too many loops to carry in every structure.
 */
#ifndef WII_H
#define WII_H

#include "gc.h"
#include "wii_ios.h"
#include "wii_remote.h"
#include "wii_bt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WII_CPU_HZ 729000000u
#define WII_BUS_HZ 243000000u
#define WII_CYCLES_PER_VI_CLOCK 27u

/* Hollywood's registers: physical 0x0d000000 and its mirror 0x0d800000. */
#define WII_PHYS_HOLLYWOOD 0x0d000000u
#define WII_HOLLYWOOD_SIZE 0x400u

/* The line Hollywood's interrupt controller drives on the processor
 * interface. */
#define GC_INT_HOLLYWOOD 14

/* Hollywood's own interrupt numbers: bits of HW_PPCIRQFLAG. */
enum {
  WII_IRQ_TIMER = 0,
  WII_IRQ_NAND = 1,
  WII_IRQ_AES = 2,
  WII_IRQ_SHA1 = 3,
  WII_IRQ_EHCI = 4,
  WII_IRQ_OHCI0 = 5,
  WII_IRQ_OHCI1 = 6,
  WII_IRQ_SDHC = 7,
  WII_IRQ_WIFI = 8,
  WII_IRQ_GPIO_BROADWAY = 10,
  WII_IRQ_GPIO_STARLET = 11,
  WII_IRQ_RESET_BUTTON = 17,
  WII_IRQ_DI = 18,
  WII_IRQ_IPC_BROADWAY = 30,
  WII_IRQ_IPC_STARLET = 31
};

/* The general-purpose pins, as bits of the GPIO registers. */
enum {
  WII_GPIO_POWER = 1u << 0,
  WII_GPIO_SHUTDOWN = 1u << 1,
  WII_GPIO_FAN = 1u << 2,
  WII_GPIO_DC_DC = 1u << 3,
  WII_GPIO_DI_SPIN = 1u << 4,
  WII_GPIO_SLOT_LED = 1u << 5,
  WII_GPIO_EJECT_BUTTON = 1u << 6,
  WII_GPIO_SLOT_IN = 1u << 7,
  WII_GPIO_SENSOR_BAR = 1u << 8,
  WII_GPIO_DO_EJECT = 1u << 9,
  WII_GPIO_EEP_CS = 1u << 10,
  WII_GPIO_EEP_CLK = 1u << 11,
  WII_GPIO_EEP_MOSI = 1u << 12,
  WII_GPIO_EEP_MISO = 1u << 13,
  WII_GPIO_AVE_SCL = 1u << 14,
  WII_GPIO_AVE_SDA = 1u << 15
};

/* What the power-button device is holding: the request a game leaves with
 * it to be told about the reset and power buttons, answered only when one
 * is pressed or the game takes the request back. */
typedef struct WiiStm {
  uint32_t hook_request; /* zero when none is held */
} WiiStm;

/* The disc drive as IOS keeps it: whether the game partition has been
 * opened (reads are refused until it has), the error the drive is holding
 * for the next request that asks, and how much has been read. */
typedef struct WiiDi {
  uint32_t partition_open;
  uint32_t error;
  uint64_t reads;
  uint64_t bytes;
} WiiDi;

/* What belongs to the process rather than to the machine: never written to
 * a save state, and kept across a load. */
typedef struct WiiHost {
  /* The flash filesystem's contents (wii_nand.h), or null. */
  void *nand;
  /* The remotes' host side - where their buttons come from. */
  void *remotes;
} WiiHost;

/* Everything a Wii adds to the machine. Plain data up to `host`: a save
 * state writes it whole. Add fields before `host`; do not reorder them. */
typedef struct WiiState {
  /* The registers that are only storage, a word each. The ones with
   * behaviour are kept below and never read from here. */
  uint32_t hollywood[WII_HOLLYWOOD_SIZE / 4u];

  /* The mailbox. The processor writes a request block's address to its
   * message register and sets X1; Starlet answers Y2 (taken) and later Y1
   * (answered, with the block's address in its own message register).
   * IY1 and IY2 say which of those may interrupt. */
  uint32_t ipc_ppc_message;
  uint32_t ipc_arm_message;
  uint8_t ipc_x1, ipc_x2, ipc_y1, ipc_y2, ipc_iy1, ipc_iy2;
  uint8_t reserved0[2];

  /* The interrupt controller, as the processor sees it. */
  uint32_t irq_flags;
  uint32_t irq_mask;

  /* The free-running timer: what it read at a known cycle. */
  uint64_t timer_cycles;
  uint32_t timer_value;
  uint32_t reserved1;

  /* Broadway's extra registers: HID4, whose SBE bit turns the second four
   * BAT pairs on, and those pairs, upper word then lower as GcCpu holds the
   * first four. */
  uint32_t hid4;
  uint32_t ibat_high[8];
  uint32_t dbat_high[8];

  WiiIos ios;
  WiiStm stm;
  WiiDi di;

  /* The title the machine is running, for the devices that are asked. */
  uint64_t title_id;

  WiiBluetooth bluetooth;

  /* What each remote is doing, as whoever supplies input last said
   * (wii_remote.h). */
  WiiRemoteInput remote_input[WII_REMOTES];

  WiiHost host;
} WiiState;

#define WII_HID4_SBE 0x02000000u

/* Choose the console this process runs: 0 for a GameCube (the default), 1
 * for a Wii. Call it before the first gc_machine_create; machines made
 * afterwards are of that kind. */
void gc_console_select(int wii);
int gc_console_is_wii(void);

/* The machine's Wii half, or NULL on a GameCube. */
static inline WiiState *wii_state(const GcMachine *m) { return (WiiState *)m->cpu.machine; }

/* Called by gc.c at the points the GameCube machine has: make, destroy,
 * reset, every time the clocks are brought up to date, every field, and
 * when the next event is being worked out. */
int wii_machine_create(GcMachine *m);
void wii_machine_destroy(GcMachine *m);
void wii_machine_reset(GcMachine *m);
void wii_machine_advance(GcMachine *m);
void wii_machine_field(GcMachine *m);
uint64_t wii_machine_next_event(GcMachine *m, uint64_t next);

/* Hollywood's register block, by physical address. */
int wii_io_claims(uint32_t physical);
uint32_t wii_io_read(GcMachine *m, uint32_t physical, unsigned size);
void wii_io_write(GcMachine *m, uint32_t physical, unsigned size, uint32_t value);

/* Raise or drop one of Hollywood's interrupts. */
void wii_set_irq(GcMachine *m, unsigned irq, int raised);
/* The mailbox's state changed: work out the interrupt lines again. */
void wii_ipc_update(GcMachine *m);

/* Broadway's own special-purpose registers. Return non-zero when handled. */
int wii_spr_write(GcMachine *m, uint32_t spr, uint32_t value);
int wii_spr_read(GcMachine *m, uint32_t spr, uint32_t *value);

/* What /dev/es knows of the title that is running (wii_ios_es.c): its id,
 * the IOS its disc asks for as the version word a game reads (0 when there
 * is no disc to say), and the maker's two letters, its group. */
uint64_t wii_es_title(GcMachine *m);
uint32_t wii_es_disc_ios(GcMachine *m);
uint16_t wii_es_group(GcMachine *m);

/* What the system menu and the disc's apploader leave in memory and in the
 * processor before a game's first instruction. */
void wii_install_boot_state(GcMachine *m);
/* After the apploader has run: the globals that depend on the disc. */
void wii_after_apploader(GcMachine *m);

#ifdef __cplusplus
}
#endif

#endif /* WII_H */
