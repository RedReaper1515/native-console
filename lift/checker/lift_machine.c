/*
 * lift_machine.c - memory, cartridge mapper and hardware-register model of
 * the lift checker's machine. See lift_machine.h for what it stands for.
 */

#include "lift_machine.h"

#include <string.h>

const char* lift_status_name(LiftStatus status) {
    switch (status) {
        case LIFT_OK: return "ok";
        case LIFT_TIMEOUT: return "timeout";
        case LIFT_INVALID_OPCODE: return "invalid_opcode";
        case LIFT_BAD_RETURN: return "bad_return";
        case LIFT_FOREIGN_BANK: return "another_bank_mapped";
        case LIFT_LOG_OVERFLOW: return "log_overflow";
        case LIFT_BANK_PULLED: return "bank_switched_under_code";
        case LIFT_TRANSFER: return "computed_transfer";
        case LIFT_PARKED: return "stopped_at_a_wait";
        case LIFT_OWN_STACK: return "used_its_stack_as_data";
    }
    return "unknown";
}

static uint64_t mix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

static void fail(LiftMachine* machine, LiftStatus status) {
    if (machine->status == LIFT_OK) machine->status = status;
    if (machine->escape) longjmp(*machine->escape, 1);
}

void lift_machine_count_access(LiftMachine* machine) {
    if (++machine->bus_accesses > machine->bus_limit && machine->bus_limit != 0) {
        fail(machine, LIFT_TIMEOUT);
    }
}

/* ---- Cartridge -------------------------------------------------------- */

static void remap(LiftMachine* machine) {
    const uint32_t count = machine->rom_bank_count ? machine->rom_bank_count : 1u;
    switch (machine->mapper) {
        case 1: {
            const uint32_t low = (machine->bank_low & 0x1Fu) ? (machine->bank_low & 0x1Fu) : 1u;
            const uint32_t high = (uint32_t)(machine->bank_high & 3u) << 5;
            machine->rom_bank = (high | low) % count;
            machine->rom_bank0 = machine->mbc1_mode ? (high % count) : 0u;
            machine->ram_bank = machine->mbc1_mode ? (machine->bank_high & 3u) : 0u;
            break;
        }
        case 2:
            machine->rom_bank = ((machine->bank_low & 0x0Fu) ? (machine->bank_low & 0x0Fu) : 1u) % count;
            machine->rom_bank0 = 0;
            machine->ram_bank = 0;
            break;
        case 3:
            machine->rom_bank = ((machine->bank_low & 0x7Fu) ? (machine->bank_low & 0x7Fu) : 1u) % count;
            machine->rom_bank0 = 0;
            machine->ram_bank = machine->bank_high & 0x0Fu;
            break;
        case 5:
            machine->rom_bank = (machine->bank_low & 0x1FFu) % count;
            machine->rom_bank0 = 0;
            machine->ram_bank = machine->bank_high & 0x0Fu;
            break;
        default:
            machine->rom_bank = count > 1u ? 1u : 0u;
            machine->rom_bank0 = 0;
            machine->ram_bank = 0;
            break;
    }
}

static void mapper_write(LiftMachine* machine, uint16_t address, uint8_t value) {
    if (address >= 0x2000 && address < 0x4000) machine->last_bank_value = value;
    switch (machine->mapper) {
        case 1:
            if (address < 0x2000) machine->ram_enable = (uint8_t)((value & 0x0Fu) == 0x0Au);
            else if (address < 0x4000) machine->bank_low = value & 0x1Fu;
            else if (address < 0x6000) machine->bank_high = value & 3u;
            else machine->mbc1_mode = value & 1u;
            break;
        case 2:
            if (address < 0x4000) {
                if (address & 0x0100u) machine->bank_low = value & 0x0Fu;
                else machine->ram_enable = (uint8_t)((value & 0x0Fu) == 0x0Au);
            }
            break;
        case 3:
            if (address < 0x2000) machine->ram_enable = (uint8_t)((value & 0x0Fu) == 0x0Au);
            else if (address < 0x4000) machine->bank_low = value & 0x7Fu;
            else if (address < 0x6000) machine->bank_high = value & 0x0Fu;
            break;
        case 5:
            if (address < 0x2000) machine->ram_enable = (uint8_t)((value & 0x0Fu) == 0x0Au);
            else if (address < 0x3000) machine->bank_low = (uint16_t)((machine->bank_low & 0x100u) | value);
            else if (address < 0x4000) machine->bank_low = (uint16_t)((machine->bank_low & 0xFFu) | ((value & 1u) << 8));
            else if (address < 0x6000) machine->bank_high = value & 0x0Fu;
            break;
        default:
            break;
    }
    remap(machine);
}

void lift_machine_select_rom_bank(LiftMachine* machine, uint32_t bank) {
    switch (machine->mapper) {
        case 1:
            machine->bank_low = (uint16_t)(bank & 0x1Fu);
            machine->bank_high = (uint8_t)((bank >> 5) & 3u);
            break;
        case 2: machine->bank_low = (uint16_t)(bank & 0x0Fu); break;
        case 3: machine->bank_low = (uint16_t)(bank & 0x7Fu); break;
        case 5: machine->bank_low = (uint16_t)(bank & 0x1FFu); break;
        default: break;
    }
    remap(machine);
}

void lift_machine_init(LiftMachine* machine, const uint8_t* rom, size_t rom_size) {
    memset(machine, 0, sizeof(*machine));
    machine->rom = rom;
    machine->rom_size = rom_size;
    machine->rom_bank_count = (uint32_t)((rom_size + 0x3FFFu) / 0x4000u);
    const uint8_t type = rom_size > 0x147 ? rom[0x147] : 0;
    if (type >= 0x01 && type <= 0x03) machine->mapper = 1;
    else if (type == 0x05 || type == 0x06) machine->mapper = 2;
    else if (type >= 0x0F && type <= 0x13) machine->mapper = 3;
    else if (type >= 0x19 && type <= 0x1E) machine->mapper = 5;
    else machine->mapper = 0;
    machine->cgb = (uint8_t)(rom_size > 0x143 && (rom[0x143] & 0x80u) != 0);
    static const uint32_t kRamSizes[6] = {0u, 0x800u, 0x2000u, 0x8000u, 0x20000u, 0x10000u};
    const uint8_t ram_code = rom_size > 0x149 ? rom[0x149] : 0;
    machine->eram_size = ram_code < 6 ? kRamSizes[ram_code] : 0u;
    if (machine->mapper == 2) machine->eram_size = 0x200u;
    if (machine->eram_size > LIFT_ERAM_MAX) machine->eram_size = LIFT_ERAM_MAX;
    machine->ram_enable = 1;
    machine->bank_low = 1;
    machine->wram_bank = 1;
    machine->min_sp = 0xFFFF;
    machine->step_limit = 200000;
    machine->bus_limit = 4000000;
    remap(machine);
}

/* ---- Hardware registers ----------------------------------------------- */

static uint8_t io_read(LiftMachine* machine, uint8_t reg) {
    const uint32_t count = machine->io_reads[reg]++;
    const uint64_t noise = mix64(machine->seed ^ ((uint64_t)reg << 40) ^ count);
    const uint8_t stored = machine->io[reg];
    switch (reg) {
        case 0x00: /* P1: the selected button lines */
            return (uint8_t)(0xC0u | (stored & 0x30u) | (noise & 0x0Fu));
        case 0x01: /* SB */
            return (uint8_t)noise;
        case 0x02: /* SC: the transfer ends at some point */
            return (uint8_t)(0x7Cu | (stored & 0x03u) | ((noise & 3u) == 0 ? 0x80u : 0u));
        case 0x04: /* DIV */
        case 0x05: /* TIMA */
            return (uint8_t)noise;
        case 0x0F: /* IF */
            return (uint8_t)(0xE0u | (noise & 0x1Fu));
        case 0x26: /* NR52: which channels are still sounding */
            return (uint8_t)(0x70u | (stored & 0x80u) | (noise & 0x0Fu));
        case 0x41: /* STAT: the mode and the coincidence bit */
            return (uint8_t)(0x80u | (stored & 0x78u) | (noise & 0x07u));
        case 0x44: /* LY: one line further on every read */
            return (uint8_t)(((machine->seed >> 8) + count) % 154u);
        case 0x4D: /* KEY1 */
            return (uint8_t)(0x7Eu | (stored & 0x01u) | ((noise & 1u) ? 0x80u : 0u));
        case 0x55: /* HDMA5: done, or blocks still to go */
            return (uint8_t)((noise & 1u) ? 0xFFu : (noise >> 8) & 0x7Fu);
        case 0x56: /* RP */
            return (uint8_t)((stored & 0xC1u) | 0x3Cu | (noise & 0x02u));
        default:
            return stored;
    }
}

static void log_entry(LiftMachine* machine, uint32_t address, uint8_t value) {
    if (machine->io_log_count >= LIFT_IO_LOG_CAPACITY) {
        fail(machine, LIFT_LOG_OVERFLOW);
        return;
    }
    machine->io_log[machine->io_log_count].address = address;
    machine->io_log[machine->io_log_count].value = value;
    machine->io_log_count++;
}

/* ---- Waiting ----------------------------------------------------------- */

void lift_machine_start_waits(LiftMachine* machine, LiftWakes* wakes, int deciding) {
    machine->wakes = wakes;
    machine->wake_deciding = deciding ? 1u : 0u;
    machine->halts = 0;
    machine->wake_cursor = 0;
    if (wakes && deciding) {
        wakes->interval += 2u; /* nothing of the trial before is "the interval before" */
        wakes->polled_count = 0;
        wakes->change_count = 0;
        wakes->rng = machine->seed ^ 0x9E3779B97F4A7C15ull;
    }
}

static uint64_t wake_random(LiftWakes* wakes) {
    uint64_t x = wakes->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    wakes->rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* The first read of a byte since the last HALT. If the same was true of it
 * in the interval before, and it was not written then either, the code has
 * come back to look at it across a HALT: it is polling it. */
static void wake_read(LiftMachine* machine, uint16_t address) {
    LiftWakes* wakes = machine->wakes;
    if ((wakes->touched[address] >> 1) == wakes->interval) return;
    const int again = wakes->touched[address] == ((wakes->interval - 1u) << 1);
    wakes->touched[address] = wakes->interval << 1;
    if (again && wakes->polled_count < LIFT_WAKE_POLLS) wakes->polled[wakes->polled_count++] = address;
}

static void wake_write(LiftMachine* machine, uint16_t address) {
    machine->wakes->touched[address] = (machine->wakes->interval << 1) | 1u;
}

/* A byte a handler could have changed under the code: RAM that is still
 * only read in this interval. Not a hardware register, which has its own
 * model, and not the function's stack. */
static int wake_polled(const LiftMachine* machine, uint16_t address) {
    const LiftWakes* wakes = machine->wakes;
    if (wakes->touched[address] != (wakes->interval << 1)) return 0;
    if (address >= 0xFE00 && address < 0xFF80) return 0;
    if (address == 0xFFFF) return 0;
    const uint16_t frame = (uint16_t)(machine->return_sp - 2u);
    const uint16_t low = machine->min_sp < frame ? machine->min_sp : frame;
    return !(address >= low && address < machine->return_sp);
}

static void wake(LiftMachine* machine) {
    LiftWakes* wakes = machine->wakes;
    const uint32_t halt = machine->halts++;
    if (!machine->wake_deciding) {
        while (machine->wake_cursor < wakes->change_count &&
               wakes->changes[machine->wake_cursor].halt == halt) {
            const LiftWakeChange* change = &wakes->changes[machine->wake_cursor++];
            lift_machine_poke(machine, change->address, change->value);
        }
        return;
    }

    unsigned polls = 0;
    for (uint32_t i = 0; i < wakes->polled_count; ++i) {
        if (wake_polled(machine, wakes->polled[i])) polls++;
    }
    for (uint32_t i = 0; i < wakes->polled_count; ++i) {
        const uint16_t address = wakes->polled[i];
        if (!wake_polled(machine, address)) continue;
        const uint64_t r = wake_random(wakes);
        /* The one byte a wait loop watches changes every other time. Among
         * many, two or so do: a handler does not rewrite everything. */
        if (polls <= 4u ? (r & 1u) != 0 : (unsigned)((r >> 1) % polls) >= 2u) continue;
        /* Zero ends most waits ("until it is cleared"); anything else ends
         * the rest ("until a button is down"). */
        const uint8_t value = (r >> 40) & 1u ? 0u : (uint8_t)(r >> 48);
        if (wakes->change_count >= LIFT_WAKE_CHANGES) {
            fail(machine, LIFT_LOG_OVERFLOW);
            return;
        }
        wakes->changes[wakes->change_count].halt = halt;
        wakes->changes[wakes->change_count].address = address;
        wakes->changes[wakes->change_count].value = value;
        wakes->change_count++;
        lift_machine_poke(machine, address, value);
    }
    wakes->interval++;
    wakes->polled_count = 0;
}

/* Most of some budget is gone: the log, the instructions, the bus accesses
 * or the changes HALTs may make. */
static int nearly_spent(const LiftMachine* machine) {
    return machine->io_log_count >= LIFT_IO_LOG_CAPACITY / 4u * 3u ||
           (machine->step_limit != 0 && machine->steps >= machine->step_limit / 4u * 3u) ||
           (machine->bus_limit != 0 && machine->bus_accesses >= machine->bus_limit / 4u * 3u) ||
           machine->wakes->change_count >= LIFT_WAKE_CHANGES / 4u * 3u;
}

void lift_machine_log_event(LiftMachine* machine, uint32_t event) {
    log_entry(machine, event, 0);
    if (event != LIFT_EVENT_HALT || !machine->wakes || machine->status != LIFT_OK) return;
    if (machine->wake_deciding ? nearly_spent(machine)
                               : machine->park_set && machine->halts == machine->park_halt) {
        /* Not through fail(): the caller finishes the HALT before it stops,
         * on either side, so the time and the PC come out the same. */
        machine->status = LIFT_PARKED;
        return;
    }
    wake(machine);
}

static void io_write(LiftMachine* machine, uint8_t reg, uint8_t value) {
    machine->io[reg] = value;
    switch (reg) {
        case 0x46: /* DMA: the transfer is treated as instantaneous */
            for (uint16_t i = 0; i < 0xA0; ++i) {
                machine->oam[i] = lift_machine_peek(machine, (uint16_t)(((uint16_t)value << 8) + i));
            }
            break;
        case 0x4F:
            if (machine->cgb) machine->vram_bank = value & 1u;
            break;
        case 0x70:
            if (machine->cgb) machine->wram_bank = (value & 7u) ? (value & 7u) : 1u;
            break;
        case 0x55:
            if (machine->cgb) {
                const uint16_t source = (uint16_t)(((machine->io[0x51] << 8) | machine->io[0x52]) & 0xFFF0u);
                const uint16_t dest = (uint16_t)(0x8000u | (((machine->io[0x53] << 8) | machine->io[0x54]) & 0x1FF0u));
                const uint16_t length = (uint16_t)(((value & 0x7Fu) + 1u) * 0x10u);
                for (uint16_t i = 0; i < length; ++i) {
                    const uint16_t target = (uint16_t)(dest + i);
                    if (target < 0x8000u || target >= 0xA000u) break;
                    machine->vram[machine->vram_bank][target - 0x8000u] =
                        lift_machine_peek(machine, (uint16_t)(source + i));
                }
            }
            break;
        default:
            break;
    }
}

/* ---- Bus -------------------------------------------------------------- */

static uint8_t* ram_cell(LiftMachine* machine, uint16_t address) {
    if (address < 0x8000) return NULL;
    if (address < 0xA000) return &machine->vram[machine->vram_bank][address - 0x8000u];
    if (address < 0xC000) {
        if (machine->eram_size == 0) return NULL;
        return &machine->eram[(machine->ram_bank * 0x2000u + (address - 0xA000u)) % machine->eram_size];
    }
    if (address < 0xD000) return &machine->wram[0][address - 0xC000u];
    if (address < 0xE000) return &machine->wram[machine->wram_bank][address - 0xD000u];
    if (address < 0xFE00) return ram_cell(machine, (uint16_t)(address - 0x2000u));
    if (address < 0xFEA0) return &machine->oam[address - 0xFE00u];
    if (address < 0xFF00) return &machine->unusable[address - 0xFEA0u];
    if (address < 0xFF80) return &machine->io[address - 0xFF00u];
    if (address < 0xFFFF) return &machine->hram[address - 0xFF80u];
    return &machine->ie;
}

static uint8_t rom_byte(const LiftMachine* machine, uint16_t address) {
    const uint32_t bank = address < 0x4000 ? machine->rom_bank0 : machine->rom_bank;
    const size_t offset = (size_t)bank * 0x4000u + (address & 0x3FFFu);
    return offset < machine->rom_size ? machine->rom[offset] : 0xFFu;
}

uint8_t lift_machine_peek(const LiftMachine* machine, uint16_t address) {
    if (address < 0x8000) return rom_byte(machine, address);
    const uint8_t* cell = ram_cell((LiftMachine*)machine, address);
    return cell ? *cell : 0xFFu;
}

void lift_machine_poke(LiftMachine* machine, uint16_t address, uint8_t value) {
    uint8_t* cell = ram_cell(machine, address);
    if (cell) *cell = value;
}

/* A pointer into what the code itself has pushed: saved registers and
 * return addresses, looked at or written as if they were data. */
static void note_frame_use(LiftMachine* machine, uint16_t address) {
    if (address >= 0xE000 && address < 0xFE00) address = (uint16_t)(address - 0x2000u);
    if (address < machine->frame_top && address >= machine->min_sp) machine->frame_used = 1;
}

uint8_t lift_machine_read(LiftMachine* machine, uint16_t address) {
    lift_machine_count_access(machine);
    if (machine->frame_top && !machine->on_stack) note_frame_use(machine, address);
    if (address < 0x8000) {
        const uint8_t value = rom_byte(machine, address);
        if (machine->probe && !machine->probe_fetching) {
            if (machine->probe_reads == LIFT_PROBE_READS) {
                for (unsigned i = 1; i < LIFT_PROBE_READS; ++i) {
                    machine->probe_read_address[i - 1u] = machine->probe_read_address[i];
                    machine->probe_read_bank[i - 1u] = machine->probe_read_bank[i];
                    machine->probe_read_value[i - 1u] = machine->probe_read_value[i];
                }
                machine->probe_reads--;
            }
            machine->probe_read_address[machine->probe_reads] = address;
            machine->probe_read_bank[machine->probe_reads] =
                (uint16_t)(address < 0x4000 ? machine->rom_bank0 : machine->rom_bank);
            machine->probe_read_value[machine->probe_reads] = value;
            machine->probe_reads++;
        }
        return value;
    }
    if (machine->read_seen && !machine->read_seen[address]) {
        machine->read_seen[address] = 1;
        if (machine->read_list) machine->read_list[(*machine->read_count)++] = address;
    }
    /* Code that runs from RAM is not polling its own bytes. */
    if (machine->wakes && machine->wake_deciding && !machine->probe_fetching) wake_read(machine, address);
    if (address >= 0xA000 && address < 0xC000 && !machine->ram_enable) return 0xFFu;
    if (address >= 0xFF00 && address < 0xFF80) return io_read(machine, (uint8_t)(address - 0xFF00u));
    const uint8_t* cell = ram_cell(machine, address);
    return cell ? *cell : 0xFFu;
}

void lift_machine_write(LiftMachine* machine, uint16_t address, uint8_t value) {
    lift_machine_count_access(machine);
    if (machine->frame_top && !machine->on_stack) note_frame_use(machine, address);
    if (address < 0x8000) {
        log_entry(machine, address, value);
        mapper_write(machine, address, value);
        return;
    }
    if (machine->wakes && machine->wake_deciding) wake_write(machine, address);
    if (address >= 0xA000 && address < 0xC000 && !machine->ram_enable) return;
    if (address >= 0xFF00 && address < 0xFF80) {
        log_entry(machine, address, value);
        io_write(machine, (uint8_t)(address - 0xFF00u), value);
        return;
    }
    uint8_t* cell = ram_cell(machine, address);
    if (cell) *cell = value;
}
