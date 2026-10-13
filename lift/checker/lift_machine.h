/*
 * lift_machine.h - the machine the lift checker runs both sides on.
 *
 * It is a Game Boy with the time taken out. Memory, the cartridge mapper and
 * the CPU behave as the hardware does. The devices do not run: a write to a
 * hardware register is recorded in order, and a read of a register that the
 * hardware changes on its own (LY, STAT, DIV, the joypad, ...) returns a value
 * that depends only on the trial's seed and on how many times that register
 * has been read. Two pieces of code that make the same accesses therefore see
 * the same machine, whatever their timing, and a wait loop on such a register
 * ends on both sides after the same number of reads.
 *
 * No interrupt is ever delivered, and no handler runs. Code that waits for
 * one would wait for ever, so a HALT stands for "a handler ran": it changes
 * some of the bytes the code is polling. Those are the bytes of RAM it reads
 * again after a HALT, having read them before it and written them at
 * neither time. The side that runs first decides the changes from the
 * trial's seed, and the other side is shown the same ones at the same HALTs
 * (LiftWakes). A loop that waits without a HALT still never ends.
 */

#ifndef LIFT_MACHINE_H
#define LIFT_MACHINE_H

#include "gblift.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#define LIFT_IO_LOG_CAPACITY 4096u
#define LIFT_ERAM_MAX (16u * 0x2000u)
#define LIFT_CALL_DEPTH 64u
#define LIFT_PROBE_READS 8u

/* Entries in the ordered log of things the hardware would notice. */
#define LIFT_EVENT_HALT 0x10000u
#define LIFT_EVENT_STOP 0x10001u

typedef enum LiftStatus {
    LIFT_OK = 0,
    LIFT_TIMEOUT,          /* ran past the step or bus-access budget */
    LIFT_INVALID_OPCODE,   /* executed a byte that is no instruction */
    LIFT_BAD_RETURN,       /* reached the return address with the wrong SP */
    LIFT_FOREIGN_BANK,     /* code in the switchable area was returned to, or the function's
                              own addresses ran, with another ROM bank mapped */
    LIFT_LOG_OVERFLOW,     /* more hardware writes than the log holds */
    LIFT_BANK_PULLED,      /* code in the switchable area mapped another bank over itself */
    LIFT_TRANSFER,         /* probing: control went where a register or the stack said */
    LIFT_PARKED,           /* stopped at a HALT with the budget nearly spent: not a failure,
                              the two sides are compared there */
    LIFT_OWN_STACK,        /* read or wrote what it had pushed, through a pointer: a reason to
                              throw a state away where the two differ and the source does not
                              push what the original pushes (the checker decides, not the CPU) */
    LIFT_STATUS_COUNT
} LiftStatus;

typedef struct LiftIoEntry {
    uint32_t address; /* a bus address, or a LIFT_EVENT_* */
    uint8_t value;
} LiftIoEntry;

/* What the HALTs of one trial changed, and the bytes being watched for the
 * next one. Shared by the two sides of a trial; not part of the state. */
#define LIFT_WAKE_POLLS 256u
#define LIFT_WAKE_CHANGES 4096u

typedef struct LiftWakeChange {
    uint32_t halt;    /* which HALT of the trial, counted from 0 */
    uint16_t address;
    uint8_t value;
} LiftWakeChange;

typedef struct LiftWakes {
    /* Per address: (interval << 1) | written, for the last interval between
     * HALTs in which the code touched it. */
    uint32_t touched[0x10000];
    uint32_t interval;
    uint16_t polled[LIFT_WAKE_POLLS]; /* read in this interval and, unwritten, in the one before */
    uint32_t polled_count;
    LiftWakeChange changes[LIFT_WAKE_CHANGES];
    uint32_t change_count;
    uint64_t rng;
} LiftWakes;

typedef struct LiftMachine {
    /* Cartridge */
    const uint8_t* rom;
    size_t rom_size;
    uint32_t rom_bank_count;
    uint8_t mapper;          /* 0 none, 1 MBC1, 2 MBC2, 3 MBC3, 5 MBC5 */
    uint8_t cgb;
    uint32_t eram_size;

    /* Mapper registers */
    uint8_t ram_enable;
    uint16_t bank_low;       /* the ROM bank register as written */
    uint8_t bank_high;       /* MBC1 upper bits, or the RAM bank register */
    uint8_t mbc1_mode;
    uint32_t rom_bank;       /* bank mapped at 0x4000-0x7FFF */
    uint32_t rom_bank0;      /* bank mapped at 0x0000-0x3FFF */
    uint32_t ram_bank;
    uint8_t last_bank_value; /* the value most recently written to the ROM bank register */

    /* Calls made from the switchable area that have not returned: where each
     * return address is on the stack, and the bank mapped when it was pushed.
     * Deepest last. */
    uint16_t call_sp[LIFT_CALL_DEPTH];
    uint16_t call_bank[LIFT_CALL_DEPTH];
    uint8_t call_count;

    /* Memory */
    uint8_t eram[LIFT_ERAM_MAX];
    uint8_t wram[8][0x1000];
    uint8_t vram[2][0x2000];
    uint8_t oam[0xA0];
    uint8_t unusable[0x60];
    uint8_t io[0x80];
    uint8_t hram[0x7F];
    uint8_t ie;
    uint8_t wram_bank;
    uint8_t vram_bank;

    /* What the hardware would have seen, in order */
    LiftIoEntry io_log[LIFT_IO_LOG_CAPACITY];
    uint32_t io_log_count;

    /* Volatile register model */
    uint64_t seed;
    uint32_t io_reads[0x80];

    /* Bookkeeping */
    uint64_t cycles;         /* guest time accounted for */
    uint64_t steps;          /* instructions the interpreter executed */
    uint64_t bus_accesses;
    uint64_t step_limit;
    uint64_t bus_limit;
    uint16_t min_sp;         /* lowest SP a write went through */
    /* Was anything the code pushed read or written as data? `frame_top` is
     * the stack pointer the function was entered with (0: not looked for);
     * what lies between the lowest SP so far and it is what the function and
     * the code it calls have pushed. */
    uint16_t frame_top;
    uint8_t on_stack;        /* the access being made is a push or a pop */
    uint8_t frame_used;
    uint8_t executed_ram;    /* the interpreter fetched an opcode outside the ROM */
    LiftStatus status;

    /* Shared across the trials of one function; not part of the state. */
    uint8_t* read_seen;      /* 0x10000 flags: a RAM or register address was read */
    uint16_t* read_list;     /* those addresses, in the order first read */
    unsigned* read_count;
    uint8_t* executed;       /* rom_size flags: an opcode was fetched here */

    /* Waiting (lift_machine_start_waits). Null `wakes`: a HALT changes nothing. */
    LiftWakes* wakes;
    uint8_t wake_deciding;   /* this side decides the changes; the other is shown them */
    uint32_t halts;          /* HALTs so far in the trial */
    uint32_t wake_cursor;    /* the next change to show the other side */
    /* Code that never returns still waits. The deciding side stops at the
     * first HALT after three quarters of any budget is gone (LIFT_PARKED);
     * the other side is told which HALT that was and stops there too. Both
     * finish the HALT, so they stand at the same place. */
    uint8_t park_set;
    uint32_t park_halt;

    /* The checked function, when it lives in the switchable ROM area: its
     * bank and the addresses it spans. Code there with another bank mapped
     * is not the function. Zero own_bank turns the check off. */
    uint32_t own_bank;
    uint16_t own_low;
    uint16_t own_high;

    /* Probing (lift_probe.c). With `probe` set, a run ends with
     * LIFT_TRANSFER at the first control transfer whose target the code
     * does not spell out: a JP HL, or a RET that pops something no CALL
     * pushed. The JP HL is not executed; the RET is. */
    uint8_t probe;
    uint8_t probe_is_ret;
    uint16_t probe_at;       /* address of the instruction */
    uint16_t probe_target;   /* where it goes */
    uint32_t probe_bank;     /* the ROM bank mapped at 0x4000 when it goes */
    uint16_t probe_call_sp[LIFT_CALL_DEPTH]; /* where return addresses pushed by CALLs are */
    uint8_t probe_calls;
    /* The last bytes read from the ROM as data (not fetched as code), newest
     * last: a target that came out of a table is two of them. */
    uint8_t probe_fetching;
    uint8_t probe_reads;
    uint16_t probe_read_address[LIFT_PROBE_READS];
    uint16_t probe_read_bank[LIFT_PROBE_READS];
    uint8_t probe_read_value[LIFT_PROBE_READS];

    /* Where a lifted function's lift_jump() ends, and how it bails out.
     * `return_is_code`: the return address is the one the game had on its
     * stack (a state a function was really entered in), where code lives
     * and may run at another depth of the stack; unset, it is an address
     * made up for the trial, where nothing may arrive but the function
     * returning. */
    uint16_t return_pc;
    uint16_t return_sp;
    uint8_t return_is_code;
    jmp_buf* escape;
} LiftMachine;

/* Set up an empty machine around a ROM image (not copied). */
void lift_machine_init(LiftMachine* machine, const uint8_t* rom, size_t rom_size);

/* Map `bank` at 0x4000-0x7FFF by setting the mapper's registers. */
void lift_machine_select_rom_bank(LiftMachine* machine, uint32_t bank);

/* Bus access with every side effect and all bookkeeping. */
uint8_t lift_machine_read(LiftMachine* machine, uint16_t address);
void lift_machine_write(LiftMachine* machine, uint16_t address, uint8_t value);

/* Look at or set a byte of RAM with no side effects and no bookkeeping.
 * Addresses below 0x8000 read the ROM and ignore writes. */
uint8_t lift_machine_peek(const LiftMachine* machine, uint16_t address);
void lift_machine_poke(LiftMachine* machine, uint16_t address, uint8_t value);

/* Charge one access to the budget that stops a runaway loop. */
void lift_machine_count_access(LiftMachine* machine);

/* Record a LIFT_EVENT_* in the hardware log. A HALT also changes the bytes
 * the code is polling, when the trial's waits were started. */
void lift_machine_log_event(LiftMachine* machine, uint32_t event);

/* Start a trial's waits on one side. `deciding` on the side that runs first:
 * it picks what each HALT changes and writes that down in `wakes`. Without
 * it, the side is shown what was written down. */
void lift_machine_start_waits(LiftMachine* machine, LiftWakes* wakes, int deciding);

/* Execute one instruction. Returns false once machine->status is not LIFT_OK. */
int lift_cpu_step(LiftMachine* machine, GBContext* ctx);

/* Run until PC is `stop_pc` with SP equal to `stop_sp`. */
LiftStatus lift_cpu_run_until(LiftMachine* machine,
                              GBContext* ctx,
                              uint16_t stop_pc,
                              uint16_t stop_sp);

const char* lift_status_name(LiftStatus status);

#endif /* LIFT_MACHINE_H */
