/*
 * lift_cpu.c - the SM83 interpreter that runs the original code on the lift
 * checker's machine.
 *
 * It is written from the instruction set alone and shares no code with the
 * helpers in gblift.h, so that a mechanical lift agreeing with it is two
 * implementations agreeing. tests/test_lift_cpu.c holds it to the runtime's
 * interpreter instruction by instruction.
 */

#include "lift_machine.h"

static const uint8_t kCycles[256] = {
    4, 12, 8, 8, 4, 4, 8, 4, 20, 8, 8, 8, 4, 4, 8, 4,
    4, 12, 8, 8, 4, 4, 8, 4, 12, 8, 8, 8, 4, 4, 8, 4,
    8, 12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
    8, 12, 8, 8, 12, 12, 12, 4, 8, 8, 8, 8, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
    8, 12, 12, 16, 12, 16, 8, 16, 8, 16, 12, 4, 12, 24, 8, 16,
    8, 12, 12, 4, 12, 16, 8, 16, 8, 16, 12, 4, 12, 4, 8, 16,
    12, 12, 8, 4, 4, 16, 8, 16, 16, 4, 16, 4, 4, 4, 8, 16,
    12, 12, 8, 4, 4, 16, 8, 16, 12, 8, 16, 4, 4, 4, 8, 16,
};

static uint8_t fetch8(LiftMachine* m, GBContext* c) {
    m->probe_fetching = 1;
    const uint8_t value = lift_machine_read(m, c->pc);
    m->probe_fetching = 0;
    c->pc = (uint16_t)(c->pc + 1u);
    return value;
}

static uint16_t fetch16(LiftMachine* m, GBContext* c) {
    const uint8_t low = fetch8(m, c);
    const uint8_t high = fetch8(m, c);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static uint8_t get_r(LiftMachine* m, GBContext* c, unsigned index) {
    switch (index & 7u) {
        case 0: return c->b;
        case 1: return c->c;
        case 2: return c->d;
        case 3: return c->e;
        case 4: return c->h;
        case 5: return c->l;
        case 6: return lift_machine_read(m, c->hl);
        default: return c->a;
    }
}

static void set_r(LiftMachine* m, GBContext* c, unsigned index, uint8_t value) {
    switch (index & 7u) {
        case 0: c->b = value; break;
        case 1: c->c = value; break;
        case 2: c->d = value; break;
        case 3: c->e = value; break;
        case 4: c->h = value; break;
        case 5: c->l = value; break;
        case 6: lift_machine_write(m, c->hl, value); break;
        default: c->a = value; break;
    }
}

static uint16_t get_rr(const GBContext* c, unsigned index) {
    switch (index & 3u) {
        case 0: return c->bc;
        case 1: return c->de;
        case 2: return c->hl;
        default: return c->sp;
    }
}

static void set_rr(GBContext* c, unsigned index, uint16_t value) {
    switch (index & 3u) {
        case 0: c->bc = value; break;
        case 1: c->de = value; break;
        case 2: c->hl = value; break;
        default: c->sp = value; break;
    }
}

static void push16(LiftMachine* m, GBContext* c, uint16_t value) {
    m->on_stack = 1;
    c->sp = (uint16_t)(c->sp - 1u);
    lift_machine_write(m, c->sp, (uint8_t)(value >> 8));
    c->sp = (uint16_t)(c->sp - 1u);
    lift_machine_write(m, c->sp, (uint8_t)value);
    m->on_stack = 0;
    if (c->sp < m->min_sp) m->min_sp = c->sp;
}

static uint16_t pop16(LiftMachine* m, GBContext* c) {
    m->on_stack = 1;
    const uint8_t low = lift_machine_read(m, c->sp);
    c->sp = (uint16_t)(c->sp + 1u);
    const uint8_t high = lift_machine_read(m, c->sp);
    c->sp = (uint16_t)(c->sp + 1u);
    m->on_stack = 0;
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static int condition(const GBContext* c, unsigned index) {
    switch (index & 3u) {
        case 0: return !c->f_z;
        case 1: return c->f_z != 0;
        case 2: return !c->f_c;
        default: return c->f_c != 0;
    }
}

static void alu(GBContext* c, unsigned kind, uint8_t value) {
    const int a = c->a;
    const int carry = c->f_c ? 1 : 0;
    int result;
    switch (kind & 7u) {
        case 0: /* ADD */
            result = a + value;
            c->f_n = 0;
            c->f_h = (uint8_t)(((a & 15) + (value & 15)) > 15);
            c->f_c = (uint8_t)(result > 255);
            c->a = (uint8_t)result;
            break;
        case 1: /* ADC */
            result = a + value + carry;
            c->f_n = 0;
            c->f_h = (uint8_t)(((a & 15) + (value & 15) + carry) > 15);
            c->f_c = (uint8_t)(result > 255);
            c->a = (uint8_t)result;
            break;
        case 2: /* SUB */
        case 7: /* CP */
            result = a - value;
            c->f_n = 1;
            c->f_h = (uint8_t)((a & 15) - (value & 15) < 0);
            c->f_c = (uint8_t)(result < 0);
            if ((kind & 7u) == 2) c->a = (uint8_t)result;
            c->f_z = (uint8_t)((result & 255) == 0);
            return;
        case 3: /* SBC */
            result = a - value - carry;
            c->f_n = 1;
            c->f_h = (uint8_t)((a & 15) - (value & 15) - carry < 0);
            c->f_c = (uint8_t)(result < 0);
            c->a = (uint8_t)result;
            break;
        case 4: /* AND */
            c->a = (uint8_t)(a & value);
            c->f_n = 0; c->f_h = 1; c->f_c = 0;
            break;
        case 5: /* XOR */
            c->a = (uint8_t)(a ^ value);
            c->f_n = 0; c->f_h = 0; c->f_c = 0;
            break;
        default: /* OR */
            c->a = (uint8_t)(a | value);
            c->f_n = 0; c->f_h = 0; c->f_c = 0;
            break;
    }
    c->f_z = (uint8_t)(c->a == 0);
}

static uint8_t rotate(GBContext* c, unsigned kind, uint8_t value) {
    const unsigned old_carry = c->f_c ? 1u : 0u;
    unsigned result;
    unsigned carry;
    switch (kind & 7u) {
        case 0: carry = value >> 7; result = (unsigned)(value << 1) | carry; break;           /* RLC */
        case 1: carry = value & 1u; result = (unsigned)(value >> 1) | (carry << 7); break;    /* RRC */
        case 2: carry = value >> 7; result = (unsigned)(value << 1) | old_carry; break;       /* RL */
        case 3: carry = value & 1u; result = (unsigned)(value >> 1) | (old_carry << 7); break; /* RR */
        case 4: carry = value >> 7; result = (unsigned)(value << 1); break;                   /* SLA */
        case 5: carry = value & 1u; result = (unsigned)(value >> 1) | (value & 0x80u); break; /* SRA */
        case 6: carry = 0; result = (unsigned)((value << 4) | (value >> 4)); break;           /* SWAP */
        default: carry = value & 1u; result = (unsigned)(value >> 1); break;                  /* SRL */
    }
    result &= 0xFFu;
    c->f_z = (uint8_t)(result == 0);
    c->f_n = 0;
    c->f_h = 0;
    c->f_c = (uint8_t)carry;
    return (uint8_t)result;
}

static uint16_t sp_plus(GBContext* c, uint8_t offset) {
    const unsigned sp = c->sp;
    c->f_z = 0;
    c->f_n = 0;
    c->f_h = (uint8_t)(((sp & 15u) + (offset & 15u)) > 15u);
    c->f_c = (uint8_t)(((sp & 255u) + offset) > 255u);
    return (uint16_t)(sp + (unsigned)(int)(int8_t)offset);
}

static void daa(GBContext* c) {
    int a = c->a;
    if (!c->f_n) {
        if (c->f_c || a > 0x99) {
            a += 0x60;
            c->f_c = 1;
        }
        if (c->f_h || (a & 0x0F) > 0x09) a += 0x06;
    } else {
        if (c->f_c) a -= 0x60;
        if (c->f_h) a -= 0x06;
    }
    c->a = (uint8_t)a;
    c->f_z = (uint8_t)(c->a == 0);
    c->f_h = 0;
}

static unsigned step_cb(LiftMachine* m, GBContext* c) {
    const uint8_t op = fetch8(m, c);
    const unsigned reg = op & 7u;
    const unsigned bit = (op >> 3) & 7u;
    const uint8_t value = get_r(m, c, reg);
    switch (op >> 6) {
        case 0:
            set_r(m, c, reg, rotate(c, bit, value));
            return reg == 6 ? 16u : 8u;
        case 1:
            c->f_z = (uint8_t)(((value >> bit) & 1u) == 0);
            c->f_n = 0;
            c->f_h = 1;
            return reg == 6 ? 12u : 8u;
        case 2:
            set_r(m, c, reg, (uint8_t)(value & ~(1u << bit)));
            return reg == 6 ? 16u : 8u;
        default:
            set_r(m, c, reg, (uint8_t)(value | (1u << bit)));
            return reg == 6 ? 16u : 8u;
    }
}

/* Banked code calls a helper in the home bank; the helper maps another
 * bank and returns. In the game it maps the caller's bank back first. Here
 * what it maps can come out of a made-up state, and then what the return
 * lands on is some other bank's bytes, not the code after the call. Keep
 * the bank each call from the switchable area was made under, and refuse a
 * state in which its return finds another. */
static void track_calls(LiftMachine* m, const GBContext* c, uint8_t op, uint16_t at,
                        uint16_t sp_before, uint32_t bank_before) {
    const int call = op == 0xCD || (op & 0xE7u) == 0xC4u || (op & 0xC7u) == 0xC7u;
    const int ret = op == 0xC9 || op == 0xD9 || (op & 0xE7u) == 0xC0u;
    if (call && c->sp == (uint16_t)(sp_before - 2u) && at >= 0x4000 && at < 0x8000) {
        /* Anything recorded at or below this slot belongs to frames that are gone. */
        while (m->call_count && m->call_sp[m->call_count - 1u] <= c->sp) m->call_count--;
        if (m->call_count < LIFT_CALL_DEPTH) {
            m->call_sp[m->call_count] = c->sp;
            m->call_bank[m->call_count] = (uint16_t)bank_before;
            m->call_count++;
        }
    } else if (ret && c->sp == (uint16_t)(sp_before + 2u)) {
        while (m->call_count && m->call_sp[m->call_count - 1u] < sp_before) m->call_count--;
        if (m->call_count && m->call_sp[m->call_count - 1u] == sp_before) {
            m->call_count--;
            if (c->pc >= 0x4000 && c->pc < 0x8000 && m->call_bank[m->call_count] != m->rom_bank) {
                m->status = LIFT_FOREIGN_BANK;
            }
        }
    }
}

/* Probing: tell a RET that goes back where a CALL came from, from one that
 * pops an address the code put there itself, which is a jump. */
static void probe_calls(LiftMachine* m, const GBContext* c, uint8_t op, uint16_t at,
                        uint16_t sp_before) {
    const int call = op == 0xCD || (op & 0xE7u) == 0xC4u || (op & 0xC7u) == 0xC7u;
    const int ret = op == 0xC9 || op == 0xD9 || (op & 0xE7u) == 0xC0u;
    if (call && c->sp == (uint16_t)(sp_before - 2u)) {
        while (m->probe_calls && m->probe_call_sp[m->probe_calls - 1u] <= c->sp) m->probe_calls--;
        if (m->probe_calls < LIFT_CALL_DEPTH) m->probe_call_sp[m->probe_calls++] = c->sp;
    } else if (ret && c->sp == (uint16_t)(sp_before + 2u)) {
        while (m->probe_calls && m->probe_call_sp[m->probe_calls - 1u] < sp_before) m->probe_calls--;
        if (m->probe_calls && m->probe_call_sp[m->probe_calls - 1u] == sp_before) {
            m->probe_calls--;
            return;
        }
        m->probe_is_ret = 1;
        m->probe_at = at;
        m->probe_target = c->pc;
        m->probe_bank = m->rom_bank;
        m->status = LIFT_TRANSFER;
    }
}

int lift_cpu_step(LiftMachine* m, GBContext* c) {
    if (m->status != LIFT_OK) return 0;
    if (++m->steps > m->step_limit && m->step_limit != 0) {
        m->status = LIFT_TIMEOUT;
        return 0;
    }

    const uint16_t at = c->pc;
    const uint32_t bank_before = m->rom_bank;
    const uint16_t sp_before = c->sp;
    if (m->own_bank != 0 && at >= m->own_low && at <= m->own_high && m->rom_bank != m->own_bank) {
        /* A callee came back, or the function itself wrote the mapper, with
         * a different bank at these addresses. What runs now is that other
         * bank's code. In the game the bank is restored first; here the
         * value restored came from a made-up state. */
        m->status = LIFT_FOREIGN_BANK;
        return 0;
    }
    if (at < 0x8000) {
        if (m->executed) {
            const size_t offset =
                (size_t)(at < 0x4000 ? m->rom_bank0 : m->rom_bank) * 0x4000u + (at & 0x3FFFu);
            if (offset < m->rom_size) m->executed[offset] = 1;
        }
    } else {
        m->executed_ram = 1;
    }

    const uint8_t op = fetch8(m, c);
    unsigned cycles = kCycles[op];

    if (m->probe && op == 0xE9) {
        m->probe_is_ret = 0;
        m->probe_at = at;
        m->probe_target = c->hl;
        m->probe_bank = m->rom_bank;
        m->status = LIFT_TRANSFER;
        c->pc = at;
        return 0;
    }

    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) {
            lift_machine_log_event(m, LIFT_EVENT_HALT);
        } else {
            set_r(m, c, (op >> 3) & 7u, get_r(m, c, op & 7u));
        }
    } else if (op >= 0x80 && op <= 0xBF) {
        alu(c, (op >> 3) & 7u, get_r(m, c, op & 7u));
    } else if (op < 0x40) {
        const unsigned reg = (op >> 3) & 7u;
        const unsigned pair = (op >> 4) & 3u;
        switch (op & 0x0Fu) {
            case 0x1: set_rr(c, pair, fetch16(m, c)); goto done;
            case 0x3: set_rr(c, pair, (uint16_t)(get_rr(c, pair) + 1u)); goto done;
            case 0xB: set_rr(c, pair, (uint16_t)(get_rr(c, pair) - 1u)); goto done;
            case 0x9: {
                const unsigned hl = c->hl;
                const unsigned value = get_rr(c, pair);
                c->f_n = 0;
                c->f_h = (uint8_t)(((hl & 0xFFFu) + (value & 0xFFFu)) > 0xFFFu);
                c->f_c = (uint8_t)((hl + value) > 0xFFFFu);
                c->hl = (uint16_t)(hl + value);
                goto done;
            }
            default: break;
        }
        switch (op & 7u) {
            case 4: {
                const uint8_t value = get_r(m, c, reg);
                const uint8_t result = (uint8_t)(value + 1u);
                c->f_z = (uint8_t)(result == 0);
                c->f_n = 0;
                c->f_h = (uint8_t)((value & 15u) == 15u);
                set_r(m, c, reg, result);
                goto done;
            }
            case 5: {
                const uint8_t value = get_r(m, c, reg);
                const uint8_t result = (uint8_t)(value - 1u);
                c->f_z = (uint8_t)(result == 0);
                c->f_n = 1;
                c->f_h = (uint8_t)((value & 15u) == 0);
                set_r(m, c, reg, result);
                goto done;
            }
            case 6: set_r(m, c, reg, fetch8(m, c)); goto done;
            default: break;
        }
        switch (op) {
            case 0x00: break;
            case 0x02: lift_machine_write(m, c->bc, c->a); break;
            case 0x12: lift_machine_write(m, c->de, c->a); break;
            case 0x0A: c->a = lift_machine_read(m, c->bc); break;
            case 0x1A: c->a = lift_machine_read(m, c->de); break;
            case 0x22: lift_machine_write(m, c->hl, c->a); c->hl = (uint16_t)(c->hl + 1u); break;
            case 0x32: lift_machine_write(m, c->hl, c->a); c->hl = (uint16_t)(c->hl - 1u); break;
            case 0x2A: c->a = lift_machine_read(m, c->hl); c->hl = (uint16_t)(c->hl + 1u); break;
            case 0x3A: c->a = lift_machine_read(m, c->hl); c->hl = (uint16_t)(c->hl - 1u); break;
            case 0x07: {
                const unsigned carry = c->a >> 7;
                c->a = (uint8_t)((c->a << 1) | carry);
                c->f_z = 0; c->f_n = 0; c->f_h = 0; c->f_c = (uint8_t)carry;
                break;
            }
            case 0x0F: {
                const unsigned carry = c->a & 1u;
                c->a = (uint8_t)((c->a >> 1) | (carry << 7));
                c->f_z = 0; c->f_n = 0; c->f_h = 0; c->f_c = (uint8_t)carry;
                break;
            }
            case 0x17: {
                const unsigned carry = c->a >> 7;
                c->a = (uint8_t)((c->a << 1) | (c->f_c ? 1u : 0u));
                c->f_z = 0; c->f_n = 0; c->f_h = 0; c->f_c = (uint8_t)carry;
                break;
            }
            case 0x1F: {
                const unsigned carry = c->a & 1u;
                c->a = (uint8_t)((c->a >> 1) | (c->f_c ? 0x80u : 0u));
                c->f_z = 0; c->f_n = 0; c->f_h = 0; c->f_c = (uint8_t)carry;
                break;
            }
            case 0x08: {
                const uint16_t address = fetch16(m, c);
                lift_machine_write(m, address, (uint8_t)c->sp);
                lift_machine_write(m, (uint16_t)(address + 1u), (uint8_t)(c->sp >> 8));
                break;
            }
            case 0x10:
                lift_machine_log_event(m, LIFT_EVENT_STOP);
                c->pc = (uint16_t)(c->pc + 1u);
                break;
            case 0x18: {
                const int8_t offset = (int8_t)fetch8(m, c);
                c->pc = (uint16_t)(c->pc + offset);
                break;
            }
            case 0x20: case 0x28: case 0x30: case 0x38: {
                const int8_t offset = (int8_t)fetch8(m, c);
                if (condition(c, (op >> 3) & 3u)) {
                    c->pc = (uint16_t)(c->pc + offset);
                    cycles += 4;
                }
                break;
            }
            case 0x27: daa(c); break;
            case 0x2F: c->a = (uint8_t)~c->a; c->f_n = 1; c->f_h = 1; break;
            case 0x37: c->f_n = 0; c->f_h = 0; c->f_c = 1; break;
            case 0x3F: c->f_n = 0; c->f_h = 0; c->f_c = (uint8_t)!c->f_c; break;
            default: break;
        }
    } else {
        const unsigned cc = (op >> 3) & 3u;
        const unsigned pair = (op >> 4) & 3u;
        switch (op) {
            case 0xC0: case 0xC8: case 0xD0: case 0xD8:
                if (condition(c, cc)) {
                    c->pc = pop16(m, c);
                    cycles += 12;
                }
                break;
            case 0xC1: case 0xD1: case 0xE1: set_rr(c, pair, pop16(m, c)); break;
            case 0xF1: {
                const uint16_t value = pop16(m, c);
                c->a = (uint8_t)(value >> 8);
                c->f_z = (uint8_t)((value >> 7) & 1u);
                c->f_n = (uint8_t)((value >> 6) & 1u);
                c->f_h = (uint8_t)((value >> 5) & 1u);
                c->f_c = (uint8_t)((value >> 4) & 1u);
                break;
            }
            case 0xC5: case 0xD5: case 0xE5: push16(m, c, get_rr(c, pair)); break;
            case 0xF5:
                push16(m, c, (uint16_t)(((uint16_t)c->a << 8) | (c->f_z ? 0x80u : 0u) |
                                        (c->f_n ? 0x40u : 0u) | (c->f_h ? 0x20u : 0u) |
                                        (c->f_c ? 0x10u : 0u)));
                break;
            case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
                const uint16_t target = fetch16(m, c);
                if (condition(c, cc)) {
                    c->pc = target;
                    cycles += 4;
                }
                break;
            }
            case 0xC3: c->pc = fetch16(m, c); break;
            case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
                const uint16_t target = fetch16(m, c);
                if (condition(c, cc)) {
                    push16(m, c, c->pc);
                    c->pc = target;
                    cycles += 12;
                }
                break;
            }
            case 0xCD: {
                const uint16_t target = fetch16(m, c);
                push16(m, c, c->pc);
                c->pc = target;
                break;
            }
            case 0xC6: case 0xCE: case 0xD6: case 0xDE:
            case 0xE6: case 0xEE: case 0xF6: case 0xFE:
                alu(c, (op >> 3) & 7u, fetch8(m, c));
                break;
            case 0xC7: case 0xCF: case 0xD7: case 0xDF:
            case 0xE7: case 0xEF: case 0xF7: case 0xFF:
                push16(m, c, c->pc);
                c->pc = (uint16_t)(op & 0x38u);
                break;
            case 0xC9: c->pc = pop16(m, c); break;
            case 0xD9: c->pc = pop16(m, c); c->ime = 1; break;
            case 0xCB: cycles = step_cb(m, c); break;
            case 0xE0: lift_machine_write(m, (uint16_t)(0xFF00u + fetch8(m, c)), c->a); break;
            case 0xF0: c->a = lift_machine_read(m, (uint16_t)(0xFF00u + fetch8(m, c))); break;
            case 0xE2: lift_machine_write(m, (uint16_t)(0xFF00u + c->c), c->a); break;
            case 0xF2: c->a = lift_machine_read(m, (uint16_t)(0xFF00u + c->c)); break;
            case 0xEA: lift_machine_write(m, fetch16(m, c), c->a); break;
            case 0xFA: c->a = lift_machine_read(m, fetch16(m, c)); break;
            case 0xE8: c->sp = sp_plus(c, fetch8(m, c)); break;
            case 0xF8: c->hl = sp_plus(c, fetch8(m, c)); break;
            case 0xF9: c->sp = c->hl; break;
            case 0xE9: c->pc = c->hl; break;
            case 0xF3: c->ime = 0; break;
            case 0xFB: c->ime = 1; break;
            default:
                m->status = LIFT_INVALID_OPCODE;
                c->pc = at;
                return 0;
        }
    }

done:
    if (c->sp < m->min_sp) m->min_sp = c->sp;
    m->cycles += cycles;
    if (m->status == LIFT_OK) track_calls(m, c, op, at, sp_before, bank_before);
    if (m->probe && m->status == LIFT_OK) probe_calls(m, c, op, at, sp_before);
    if (at >= 0x4000 && at < 0x8000 && m->rom_bank != bank_before && m->status == LIFT_OK) {
        /* An instruction in the switchable area wrote the mapper, and the
         * bytes after it are now another bank's. A game does not do that to
         * itself; a pointer out of a made-up state does. Nothing that reads
         * this bank's code can say what runs next. */
        m->status = LIFT_BANK_PULLED;
    }
    return m->status == LIFT_OK;
}

LiftStatus lift_cpu_run_until(LiftMachine* m, GBContext* c, uint16_t stop_pc, uint16_t stop_sp) {
    while (m->status == LIFT_OK) {
        if (c->pc == stop_pc && c->sp == stop_sp) break;
        /* Arriving at the checked function's own return address with its
         * stack frame gone is the function returning, even from inside a
         * callee. Where that address is one made up for the trial, somewhere
         * no code lives, any other way of arriving is a return gone wrong.
         * Where it is the address the game had on its stack, code lives
         * there, and a callee may well run through it deeper down: a
         * function entered from a helper that it calls itself. */
        if (c->pc == m->return_pc) {
            if (c->sp == m->return_sp) break;
            if (!m->return_is_code) {
                m->status = LIFT_BAD_RETURN;
                break;
            }
        }
        if (!lift_cpu_step(m, c)) break;
    }
    return m->status;
}
