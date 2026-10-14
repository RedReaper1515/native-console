/*
 * gblift.h - the interface lifted Game Boy source is written against.
 *
 * A lifted function is ordinary C that does what one subroutine of the
 * original game does:
 *
 *     void lifted_02_4497(GBContext* ctx);
 *
 * On entry the registers hold what the game's code would see at the
 * subroutine's first instruction, with the caller's return address on the
 * guest stack. The function leaves the machine as the original would leave
 * it when control is back with the caller: it ends by calling lift_ret(), or
 * hands over to other guest code with lift_jump(), and then returns.
 *
 * Everything the function does to the machine goes through this header, so
 * the same source can be run against the original by the checker
 * (lift/checker, see docs/LIFTING.md) and compiled into the game.
 *
 * ctx->pc is the function's to keep or not. In the game an interrupt taken
 * inside lifted code returns to ctx->pc, and a save state made there records
 * it; execution loaded from that state carries on in the recompiled code at
 * that address. A mechanical lift sets it before every lift_tick(), which
 * makes both exact. Source that never sets it leaves it at the function's
 * first instruction, or the return address of its last call.
 */

#ifndef GBLIFT_H
#define GBLIFT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBLIFT_ABI_VERSION 1u

/*
 * CPU state. The names of the registers match the runtime's GBContext
 * (runtimes/runtime-gb/include/gbrt.h): the flags are kept unpacked in f_z, f_n, f_h and
 * f_c, each 0 or 1, and `f` is only meaningful after lift_pack_flags().
 *
 * A build that runs lifted source on the real runtime defines
 * GBLIFT_USE_RUNTIME_CONTEXT and gets the runtime's own GBContext.
 */
#ifdef GBLIFT_USE_RUNTIME_CONTEXT
#include "gbrt.h"
#else
typedef struct GBContext {
    union {
        struct { uint8_t f, a; };
        uint16_t af;
    };
    union {
        struct { uint8_t c, b; };
        uint16_t bc;
    };
    union {
        struct { uint8_t e, d; };
        uint16_t de;
    };
    union {
        struct { uint8_t l, h; };
        uint16_t hl;
    };
    uint16_t sp;
    uint16_t pc;
    uint8_t f_z;
    uint8_t f_n;
    uint8_t f_h;
    uint8_t f_c;
    uint8_t ime;
    void* machine; /* owned by whatever runs the function */
} GBContext;
#endif

typedef void (*GBLiftFunction)(GBContext* ctx);

/* Introduces a lifted function and says which subroutine of the ROM it is:
 *
 *     GBLIFT_FUNCTION(0x02, 0x4497, update_player_health) { ... }
 *
 * tools/gb_lift.py reads these lines to pair source with the original. */
#define GBLIFT_FUNCTION(bank_, address_, name_) void name_(GBContext* ctx)

/* ---- The machine ------------------------------------------------------ */

/* Memory as the CPU sees it, through the cartridge's current bank mapping. */
uint8_t lift_read8(GBContext* ctx, uint16_t address);
void lift_write8(GBContext* ctx, uint16_t address, uint8_t value);

/* Where a byte of work RAM (C000-DFFF) or high RAM (FF80-FFFE) is, so that
 * source can give the game's variables names and use them as C uses a
 * variable: `#define wCurMap (*lift_ram(ctx, 0xD35E))`. NULL for any other
 * address: the rest of the bus does something when it is read or written
 * (video memory, the cartridge, a hardware register), and goes through
 * lift_read8() and lift_write8().
 *
 * The bytes after it follow as far as its own stretch goes (C000-CFFF,
 * D000-DFFF, FF80-FFFE), and a pointer into D000-DFFF is of the bank a
 * Game Boy Color game has mapped there now. */
uint8_t* lift_ram(GBContext* ctx, uint16_t address);

/* Registers and flags the function leaves with nothing in them worth having.
 * Assembly leaves something in every register, and most of it is what was
 * left over: the last byte a loop looked at, the flags of its last compare.
 * A function written as C has results, and says here what the rest is:
 *
 *     ctx->a = Random(ctx->f_c);
 *     lift_undefined(ctx, LIFT_FLAGS);
 *
 * The checker then does not compare them. In the game they are given a
 * fixed value that is nobody's, so that code which did read one of them
 * goes wrong the same way every time, and a run compared with the original
 * shows it. */
#define LIFT_REG_A 0x001u
#define LIFT_REG_B 0x002u
#define LIFT_REG_C 0x004u
#define LIFT_REG_D 0x008u
#define LIFT_REG_E 0x010u
#define LIFT_REG_H 0x020u
#define LIFT_REG_L 0x040u
#define LIFT_FLAG_Z 0x080u
#define LIFT_FLAG_N 0x100u
#define LIFT_FLAG_H 0x200u
#define LIFT_FLAG_C 0x400u
#define LIFT_REG_BC (LIFT_REG_B | LIFT_REG_C)
#define LIFT_REG_DE (LIFT_REG_D | LIFT_REG_E)
#define LIFT_REG_HL (LIFT_REG_H | LIFT_REG_L)
#define LIFT_FLAGS (LIFT_FLAG_Z | LIFT_FLAG_N | LIFT_FLAG_H | LIFT_FLAG_C)
void lift_undefined(GBContext* ctx, unsigned what);

/* The function running keeps its own stack: it does not push what the
 * original pushes. A function written as C saves a register in a local
 * where the assembly pushes it, and calls other C without a return address
 * under the call. What it does to the game is the original's all the same,
 * except where code looks at the stack itself, and that is what this says
 * the function does not promise:
 *
 *     GBLIFT_FUNCTION(0x00, 0x3A87, AddNTimes_binding) {
 *         lift_own_stack(ctx);
 *         ctx->hl = AddNTimes(ctx, ctx->a, ctx->bc, ctx->hl);
 *         ...
 *
 * The checker then compares a state stopped at a wait without the stack
 * pointer and without what lies between it and where the function was
 * entered, and throws away a state in which the original read or wrote what
 * it had pushed as if it were data, where the two differ. Where the two come
 * out of such a state alike and took another time over it, the time is not
 * held against the function either: the original went one way or the other
 * over a byte it had pushed (`not_compared` in the report). Everything else
 * is compared as for any function, the time included. In the game it does
 * nothing. Say it before anything else, in every state. */
void lift_own_stack(GBContext* ctx);

/* Call the guest subroutine at `address` and come back when it returns.
 * `return_address` is the guest address of the instruction after the
 * original CALL; it is what the callee finds on the stack.
 *
 * Returns 0 when the callee returned there. Assembly can also return past
 * its caller (`pop af / ret` in the callee drops one return address): then
 * control is back with this function's own caller, the result is nonzero,
 * and the lifted function must return at once, without lift_ret(). */
int lift_call(GBContext* ctx, uint16_t address, uint16_t return_address);

/* A jump that is a call. Assembly calls through a register by pushing the
 * address to come back to and jumping: `ld de, back / push de / jp hl`. With
 * `return_address` already on top of the guest stack, this runs the code at
 * `address` until it returns there, as lift_call() does after its own push,
 * and gives the same answer: 0 when it came back, nonzero when control is
 * with this function's caller and the lifted function must return at once.
 *
 * Only for an address this function pushed itself. A jump with the caller's
 * return address on top is lift_jump(). */
int lift_call_pushed(GBContext* ctx, uint16_t address, uint16_t return_address);

/* Continue in guest code at `address` (a JP out of the function, or running
 * into the next one) until it returns to this function's caller. The lifted
 * function must return right after this, without lift_ret(). */
void lift_jump(GBContext* ctx, uint16_t address);

/* RET: pop the address on top of the guest stack and continue there. That
 * is the caller, unless the code put another address on the stack first
 * (`push hl / ret` is a jump). Either way the lifted function is over and
 * must return right after this. */
void lift_ret(GBContext* ctx);

/* RETI: lift_ret() that also enables interrupts. They come on as the
 * instruction ends, so call lift_tick() for its 16 cycles first. */
void lift_reti(GBContext* ctx);

/* Account for guest time: `cycles` T-cycles (4 per machine cycle). This is
 * also where an instruction ends: a pending interrupt is taken here, and its
 * handler has run by the time lift_tick() returns. */
void lift_tick(GBContext* ctx, uint32_t cycles);

/* Time passing inside one instruction, before its memory access: no
 * interrupt is taken. `lift_advance(ctx, 11); x = lift_read8(...);
 * lift_tick(ctx, 1);` reads on the cycle the hardware does. */
void lift_advance(GBContext* ctx, uint32_t cycles);

/* EI and DI. EI takes effect after the instruction that follows it, so it
 * is not the same as setting ctx->ime. */
void lift_ei(GBContext* ctx);
void lift_di(GBContext* ctx);

/* HALT and STOP, with the 4 cycles the instruction takes. They return once
 * the CPU is running again and the interrupt that woke it has been handled. */
void lift_halt(GBContext* ctx);
void lift_stop(GBContext* ctx);

/* ---- Helpers that only use the above ---------------------------------- */

/* An instruction is done: the original would now be at `next_pc`, `cycles`
 * later. Sets ctx->pc first, so that an interrupt taken here returns there. */
static inline void lift_step(GBContext* ctx, uint16_t next_pc, uint32_t cycles) {
    ctx->pc = next_pc;
    lift_tick(ctx, cycles);
}

/* A conditional jump is done. Returns `taken`, having accounted for where
 * the original went and how long that took, so it can sit in an `if` or a
 * `while`: `do { ... } while (lift_branch(ctx, ctx->b != 0, 0x4000, 12, 0x4008, 8));` */
static inline int lift_branch(GBContext* ctx, int taken, uint16_t target, uint32_t taken_cycles,
                              uint16_t next_pc, uint32_t cycles) {
    ctx->pc = taken ? target : next_pc;
    lift_tick(ctx, taken ? taken_cycles : cycles);
    return taken;
}

static inline uint16_t lift_read16(GBContext* ctx, uint16_t address) {
    const uint8_t low = lift_read8(ctx, address);
    const uint8_t high = lift_read8(ctx, (uint16_t)(address + 1u));
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static inline void lift_write16(GBContext* ctx, uint16_t address, uint16_t value) {
    lift_write8(ctx, address, (uint8_t)(value & 0xFFu));
    lift_write8(ctx, (uint16_t)(address + 1u), (uint8_t)(value >> 8));
}

static inline void lift_push16(GBContext* ctx, uint16_t value) {
    ctx->sp = (uint16_t)(ctx->sp - 1u);
    lift_write8(ctx, ctx->sp, (uint8_t)(value >> 8));
    ctx->sp = (uint16_t)(ctx->sp - 1u);
    lift_write8(ctx, ctx->sp, (uint8_t)(value & 0xFFu));
}

static inline uint16_t lift_pop16(GBContext* ctx) {
    const uint8_t low = lift_read8(ctx, ctx->sp);
    ctx->sp = (uint16_t)(ctx->sp + 1u);
    const uint8_t high = lift_read8(ctx, ctx->sp);
    ctx->sp = (uint16_t)(ctx->sp + 1u);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static inline void lift_pack_flags(GBContext* ctx) {
    ctx->f = (uint8_t)((ctx->f_z ? 0x80u : 0u) | (ctx->f_n ? 0x40u : 0u) |
                       (ctx->f_h ? 0x20u : 0u) | (ctx->f_c ? 0x10u : 0u));
}

static inline void lift_unpack_flags(GBContext* ctx) {
    ctx->f_z = (uint8_t)((ctx->f >> 7) & 1u);
    ctx->f_n = (uint8_t)((ctx->f >> 6) & 1u);
    ctx->f_h = (uint8_t)((ctx->f >> 5) & 1u);
    ctx->f_c = (uint8_t)((ctx->f >> 4) & 1u);
    ctx->f &= 0xF0u;
}

static inline void lift_push_af(GBContext* ctx) {
    lift_pack_flags(ctx);
    lift_push16(ctx, ctx->af);
}

static inline void lift_pop_af(GBContext* ctx) {
    ctx->af = lift_pop16(ctx);
    lift_unpack_flags(ctx);
}

/* ---- The SM83's arithmetic, flags included ---------------------------- *
 * A first, mechanical lift needs these to stay exact. Readable source
 * replaces them with plain C wherever the flags are not used afterwards. */

static inline void lift_add8(GBContext* ctx, uint8_t value) {
    const unsigned result = (unsigned)ctx->a + value;
    ctx->f_h = (uint8_t)(((ctx->a & 0x0Fu) + (value & 0x0Fu)) > 0x0Fu);
    ctx->f_c = (uint8_t)(result > 0xFFu);
    ctx->f_n = 0;
    ctx->a = (uint8_t)result;
    ctx->f_z = (uint8_t)(ctx->a == 0);
}

static inline void lift_adc8(GBContext* ctx, uint8_t value) {
    const unsigned carry = ctx->f_c ? 1u : 0u;
    const unsigned result = (unsigned)ctx->a + value + carry;
    ctx->f_h = (uint8_t)(((ctx->a & 0x0Fu) + (value & 0x0Fu) + carry) > 0x0Fu);
    ctx->f_c = (uint8_t)(result > 0xFFu);
    ctx->f_n = 0;
    ctx->a = (uint8_t)result;
    ctx->f_z = (uint8_t)(ctx->a == 0);
}

static inline uint8_t lift_compare8(GBContext* ctx, uint8_t value, unsigned borrow) {
    const unsigned subtrahend = (unsigned)value + borrow;
    ctx->f_h = (uint8_t)((ctx->a & 0x0Fu) < ((value & 0x0Fu) + borrow));
    ctx->f_c = (uint8_t)((unsigned)ctx->a < subtrahend);
    ctx->f_n = 1;
    const uint8_t result = (uint8_t)(ctx->a - subtrahend);
    ctx->f_z = (uint8_t)(result == 0);
    return result;
}

static inline void lift_sub8(GBContext* ctx, uint8_t value) {
    ctx->a = lift_compare8(ctx, value, 0u);
}

static inline void lift_sbc8(GBContext* ctx, uint8_t value) {
    ctx->a = lift_compare8(ctx, value, ctx->f_c ? 1u : 0u);
}

static inline void lift_cp8(GBContext* ctx, uint8_t value) {
    (void)lift_compare8(ctx, value, 0u);
}

static inline void lift_and8(GBContext* ctx, uint8_t value) {
    ctx->a &= value;
    ctx->f_z = (uint8_t)(ctx->a == 0);
    ctx->f_n = 0;
    ctx->f_h = 1;
    ctx->f_c = 0;
}

static inline void lift_xor8(GBContext* ctx, uint8_t value) {
    ctx->a ^= value;
    ctx->f_z = (uint8_t)(ctx->a == 0);
    ctx->f_n = 0;
    ctx->f_h = 0;
    ctx->f_c = 0;
}

static inline void lift_or8(GBContext* ctx, uint8_t value) {
    ctx->a |= value;
    ctx->f_z = (uint8_t)(ctx->a == 0);
    ctx->f_n = 0;
    ctx->f_h = 0;
    ctx->f_c = 0;
}

static inline uint8_t lift_inc8(GBContext* ctx, uint8_t value) {
    const uint8_t result = (uint8_t)(value + 1u);
    ctx->f_z = (uint8_t)(result == 0);
    ctx->f_n = 0;
    ctx->f_h = (uint8_t)((value & 0x0Fu) == 0x0Fu);
    return result;
}

static inline uint8_t lift_dec8(GBContext* ctx, uint8_t value) {
    const uint8_t result = (uint8_t)(value - 1u);
    ctx->f_z = (uint8_t)(result == 0);
    ctx->f_n = 1;
    ctx->f_h = (uint8_t)((value & 0x0Fu) == 0);
    return result;
}

static inline void lift_add_hl(GBContext* ctx, uint16_t value) {
    const unsigned result = (unsigned)ctx->hl + value;
    ctx->f_n = 0;
    ctx->f_h = (uint8_t)(((ctx->hl & 0x0FFFu) + (value & 0x0FFFu)) > 0x0FFFu);
    ctx->f_c = (uint8_t)(result > 0xFFFFu);
    ctx->hl = (uint16_t)result;
}

/* SP plus a signed byte, as ADD SP,e and LD HL,SP+e compute it. */
static inline uint16_t lift_sp_plus(GBContext* ctx, uint8_t offset) {
    ctx->f_z = 0;
    ctx->f_n = 0;
    ctx->f_h = (uint8_t)(((ctx->sp & 0x0Fu) + (offset & 0x0Fu)) > 0x0Fu);
    ctx->f_c = (uint8_t)(((ctx->sp & 0xFFu) + offset) > 0xFFu);
    return (uint16_t)(ctx->sp + (uint16_t)(int16_t)(int8_t)offset);
}

static inline void lift_rlca(GBContext* ctx) {
    const uint8_t carry = (uint8_t)(ctx->a >> 7);
    ctx->a = (uint8_t)((ctx->a << 1) | carry);
    ctx->f_z = 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = carry;
}

static inline void lift_rrca(GBContext* ctx) {
    const uint8_t carry = (uint8_t)(ctx->a & 1u);
    ctx->a = (uint8_t)((ctx->a >> 1) | (carry << 7));
    ctx->f_z = 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = carry;
}

static inline void lift_rla(GBContext* ctx) {
    const uint8_t carry = (uint8_t)(ctx->a >> 7);
    ctx->a = (uint8_t)((ctx->a << 1) | (ctx->f_c ? 1u : 0u));
    ctx->f_z = 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = carry;
}

static inline void lift_rra(GBContext* ctx) {
    const uint8_t carry = (uint8_t)(ctx->a & 1u);
    ctx->a = (uint8_t)((ctx->a >> 1) | (ctx->f_c ? 0x80u : 0u));
    ctx->f_z = 0; ctx->f_n = 0; ctx->f_h = 0; ctx->f_c = carry;
}

static inline void lift_daa(GBContext* ctx) {
    uint8_t adjust = 0;
    uint8_t carry = ctx->f_c ? 1u : 0u;
    if (!ctx->f_n) {
        if (ctx->f_h || (ctx->a & 0x0Fu) > 0x09u) adjust |= 0x06u;
        if (ctx->f_c || ctx->a > 0x99u) {
            adjust |= 0x60u;
            carry = 1;
        }
        ctx->a = (uint8_t)(ctx->a + adjust);
    } else {
        if (ctx->f_h) adjust |= 0x06u;
        if (ctx->f_c) adjust |= 0x60u;
        ctx->a = (uint8_t)(ctx->a - adjust);
    }
    ctx->f_z = (uint8_t)(ctx->a == 0);
    ctx->f_h = 0;
    ctx->f_c = carry;
}

static inline void lift_shift_flags(GBContext* ctx, uint8_t result, uint8_t carry) {
    ctx->f_z = (uint8_t)(result == 0);
    ctx->f_n = 0;
    ctx->f_h = 0;
    ctx->f_c = carry;
}

static inline uint8_t lift_rlc(GBContext* ctx, uint8_t value) {
    const uint8_t carry = (uint8_t)(value >> 7);
    const uint8_t result = (uint8_t)((value << 1) | carry);
    lift_shift_flags(ctx, result, carry);
    return result;
}

static inline uint8_t lift_rrc(GBContext* ctx, uint8_t value) {
    const uint8_t carry = (uint8_t)(value & 1u);
    const uint8_t result = (uint8_t)((value >> 1) | (carry << 7));
    lift_shift_flags(ctx, result, carry);
    return result;
}

static inline uint8_t lift_rl(GBContext* ctx, uint8_t value) {
    const uint8_t carry = (uint8_t)(value >> 7);
    const uint8_t result = (uint8_t)((value << 1) | (ctx->f_c ? 1u : 0u));
    lift_shift_flags(ctx, result, carry);
    return result;
}

static inline uint8_t lift_rr(GBContext* ctx, uint8_t value) {
    const uint8_t carry = (uint8_t)(value & 1u);
    const uint8_t result = (uint8_t)((value >> 1) | (ctx->f_c ? 0x80u : 0u));
    lift_shift_flags(ctx, result, carry);
    return result;
}

static inline uint8_t lift_sla(GBContext* ctx, uint8_t value) {
    const uint8_t result = (uint8_t)(value << 1);
    lift_shift_flags(ctx, result, (uint8_t)(value >> 7));
    return result;
}

static inline uint8_t lift_sra(GBContext* ctx, uint8_t value) {
    const uint8_t result = (uint8_t)((value >> 1) | (value & 0x80u));
    lift_shift_flags(ctx, result, (uint8_t)(value & 1u));
    return result;
}

static inline uint8_t lift_srl(GBContext* ctx, uint8_t value) {
    const uint8_t result = (uint8_t)(value >> 1);
    lift_shift_flags(ctx, result, (uint8_t)(value & 1u));
    return result;
}

static inline uint8_t lift_swap(GBContext* ctx, uint8_t value) {
    const uint8_t result = (uint8_t)((value << 4) | (value >> 4));
    lift_shift_flags(ctx, result, 0);
    return result;
}

static inline void lift_bit(GBContext* ctx, unsigned bit, uint8_t value) {
    ctx->f_z = (uint8_t)(((value >> bit) & 1u) == 0);
    ctx->f_n = 0;
    ctx->f_h = 1;
}

#ifdef __cplusplus
}
#endif

#endif /* GBLIFT_H */
