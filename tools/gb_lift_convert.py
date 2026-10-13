#!/usr/bin/env python3
"""Lifted functions turned into C functions with arguments, by a program.

`gb_lift.py` uses this for its `convert` command.

A lifted function is the original's instructions on the machine's registers:
`GBLIFT_FUNCTION(0x00, 0x3A87, AddNTimes)` takes a context and that is all
its text says of what it is given. `signatures.txt` says the rest: what the
function takes, gives and keeps. This writes each function again as what
that line describes,

    AddNTimes_r AddNTimes(GBContext* ctx, uint8_t a, uint16_t bc, uint16_t hl);

with the registers it works in local to it (`r.hl`), every call it makes
written as a call with arguments and results, and a few lines of binding
where the lifted function stood, which hand the machine's registers in, the
results back, and leave the rest as the line says: kept, or undefined.

Nothing here is believed. Each converted function is given to the checker
held to its signature, as `build` gives it a lifted one, and the report
says how many still do what the ROM's code does.

A function comes out one of two ways.

**As C.** Its registers are locals and nothing else. A pair it pushes and
takes off again is a local (`s0 = r.bc; ... r.de = s0;`), and one it only
saves to put back is not kept at all. A call of another function that C can
call is a C call with the arguments and nothing under it, and a jump into
such a function is a call of it and a return. Such a function does not push
what the original pushes, and its binding says so (`lift_own_stack`).

**On the machine's stack.** Anything else: its pushes and pops are the
machine's, and so is the return address under every call it makes. To keep
the stack the original's in every state, it starts from the machine's
registers for what it does not take and leaves them as its own are, and
what a callee may save is worked out before the call. A jump out of it
hands every register to the machine and leaves.

What decides, and what makes the C right, is none of it something the
checker can say, because each is about a state the checker was not in or
cannot be in. Each was learnt from a game that went another way than the
original with every function matching:

- *What it has pushed* (`pushed`): the same by whichever way an instruction
  is reached, never less than none, none when it leaves; and what it calls
  with something pushed leaves the stack as it found it.
- *Whether what it goes to comes back* (`well_nested`): assembly can take a
  return address off and return one caller further, and C that calls with
  nothing under the call, or makes a jump a call, changes what such code
  finds. C calls directly, and turns a jump into a call, only where the
  export says the function gone to returns to its caller with the stack as
  it found it, whatever it calls.
- *What it may read* (`may_read`): a line says what was seen to matter, and
  a C function has what it is passed and nothing else, so it is passed
  whatever its code may read on any path.
- *What it gives and keeps* (`unchanged`, `wants`): it keeps what its code
  is sure to leave as it found it, gives what its line calls kept without
  the code bearing that out, and gives what a caller may read after it.
- *When an interrupt is taken* (`passed`): from C to C the time of a call
  or a return is passed with lift_tick, which takes one that is due; the
  machine does that itself for a call through it.

The second, third and fourth are worked out over all the functions
together, in `convert_all`, until a pass changes nothing.

A call is written with arguments where `build` has held the callee to its
line. A callee C cannot call directly is called through the machine, by its
address, with the return address the original pushes (`Name_call`), and
that carries every register across both ways, since what runs is the
machine's code and may save a register and put it back. A callee nothing is
known of gets every register and gives every one back (`lift_call_all`).

Not converted: a function that jumps through a register, one that returns
from an interrupt, one that reckons with the stack pointer, one somebody
has changed, and one `build` has not held to a line and found to match.
Time is one step an instruction, in every case.
"""
from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import time
from pathlib import Path

import gb_lift_codegen as codegen
import gb_lift_signatures as signatures
import gb_lift_source as source

HEADER = "converted.h"
SUPPORT = "converted_support.c"
REPORT = "convert.json"
ROUNDS = 4  # how often functions that did not hold as C are written the other way and all checked again
# For finding out which function a game that plays wrong has it from: functions to keep on the
# machine's stack whatever they could be. Set from a script, never by the tool.
KEEP_ON_MACHINE: set[str] = set()

# A function is left as it is where it does one of these.
STACK_POINTER = frozenset((0x31, 0x33, 0x3B, 0x39, 0xE8, 0xF8, 0xF9, 0x08))
PUSHES = frozenset((0xC5, 0xD5, 0xE5, 0xF5))
POPS = frozenset((0xC1, 0xD1, 0xE1, 0xF1))

# gblift.h's arithmetic works on the machine's registers. A converted function
# has its registers in a local, and the same functions written for that are
# made from the header's own text, so that there is one place they are right.
ARITHMETIC_FROM = "/* ---- The SM83's arithmetic"
ARITHMETIC_TO = "#ifdef __cplusplus"
REGISTERS_ONLY = ("add8", "adc8", "sub8", "sbc8", "cp8", "and8", "xor8", "or8", "inc8", "dec8", "add_hl",
                  "rlca", "rrca", "rla", "rra", "daa", "rlc", "rrc", "rl", "rr", "sla", "sra", "srl", "swap", "bit")

SUPPORT_HEAD = """\
/* The registers a converted function works in. The machine's own are in the
 * context, and are what a call through the machine hands over. */
typedef struct LiftRegs {
    union { struct { uint8_t f, a; }; uint16_t af; };
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };
    uint8_t f_z, f_n, f_h, f_c;
} LiftRegs;

static inline void liftr_from(LiftRegs* r, const GBContext* ctx) {
    r->a = ctx->a; r->bc = ctx->bc; r->de = ctx->de; r->hl = ctx->hl;
    r->f_z = ctx->f_z; r->f_n = ctx->f_n; r->f_h = ctx->f_h; r->f_c = ctx->f_c;
}

static inline void liftr_to(GBContext* ctx, const LiftRegs* r) {
    ctx->a = r->a; ctx->bc = r->bc; ctx->de = r->de; ctx->hl = r->hl;
    ctx->f_z = r->f_z; ctx->f_n = r->f_n; ctx->f_h = r->f_h; ctx->f_c = r->f_c;
}

/* A binding is the lifted function of something converted: it hands the
 * machine's registers in and the results back. Code of the ROM's that a
 * converted function calls through the machine can drop its return address
 * and go back to the caller's caller, and a converted function on the
 * machine's stack can jump away for good: then everything the binding was
 * doing is left at once, with nothing more done, which is what the assembly
 * does. */
typedef struct LiftFrame {
    jmp_buf left;
    struct LiftFrame* outer;
} LiftFrame;

extern LiftFrame* lift_frame; /* the binding that is running, innermost */

#define LIFT_ENTER(frame)                       \\
    (frame).outer = lift_frame;                 \\
    lift_frame = &(frame);                      \\
    if (setjmp((frame).left)) {                 \\
        lift_frame = (frame).outer;             \\
        return;                                 \\
    }
#define LIFT_LEAVE(frame) (lift_frame = (frame).outer)

#if defined(_MSC_VER)
#define LIFT_NORETURN __declspec(noreturn)
#else
#define LIFT_NORETURN __attribute__((noreturn))
#endif

LIFT_NORETURN static inline void lift_left(void) {
    longjmp(lift_frame->left, 1);
}

/* A call with every register handed over and every one taken back: for a
 * callee nothing says more of. */
static inline void lift_call_all(GBContext* ctx, LiftRegs* r, uint16_t address, uint16_t return_address) {
    liftr_to(ctx, r);
    if (lift_call(ctx, address, return_address)) lift_left();
    liftr_from(r, ctx);
}

/* A jump out of a function on the machine's stack: the machine goes on from
 * there with the registers as they are, and comes back to this function's
 * caller. */
LIFT_NORETURN static inline void lift_jump_all(GBContext* ctx, const LiftRegs* r, uint16_t address) {
    liftr_to(ctx, r);
    lift_jump(ctx, address);
    lift_left();
}
"""

DECLARATIONS_HEAD = """\
/* For each function: what it returns where that is more than one thing, its
 * prototype where it is converted, and the call of it through the machine.
 * `Name_call` runs the function by its address, with the return address the
 * original pushes. Its arguments are what the function takes and its result
 * what it gives, and it carries every other register across as well, both
 * ways: what is run is the machine's code, which may save a register and
 * put it back, and what it puts back has to be what the caller had. */

"""

TO_CODEGEN = [(mine, theirs) for theirs, mine in signatures._FROM_CODEGEN]


def to_codegen(bits: int) -> int:
    return sum(theirs for mine, theirs in TO_CODEGEN if bits & mine)


def fields(bits: int) -> list[tuple[str, str, str]]:
    """The registers and flags of a signature's word as C: (the name of an
    argument or a result, its type, the member of the registers it is)."""
    out = []
    if bits & 0x001:
        out.append(("a", "uint8_t", "a"))
    for pair, high, low in (("bc", 0x002, 0x004), ("de", 0x008, 0x010), ("hl", 0x020, 0x040)):
        if bits & high and bits & low:
            out.append((pair, "uint16_t", pair))
        elif bits & high:
            out.append((pair[0], "uint8_t", pair[0]))
        elif bits & low:
            out.append((pair[1], "uint8_t", pair[1]))
    for name, bit in (("fz", 0x080), ("fn", 0x100), ("fh", 0x200), ("fc", 0x400)):
        if bits & bit:
            out.append((name, "uint8_t", f"f_{name[1]}"))
    return out


def result_type(name: str, signature: signatures.Signature) -> str:
    given = fields(signature.gives)
    return "void" if not given else given[0][1] if len(given) == 1 else f"{name}_r"


def prototype(name: str, signature: signatures.Signature, through_machine: bool = False) -> str:
    """The function's prototype, or that of the call of it through the machine."""
    arguments = ["GBContext* ctx"] + (["LiftRegs* r", "uint16_t return_address"] if through_machine else [])
    arguments += [f"{ctype} {argument}" for argument, ctype, _ in fields(signature.takes)]
    return (f"{result_type(name, signature)} {name}{'_call' if through_machine else ''}"
            f"({', '.join(arguments)})")


def result_of(name: str, signature: signatures.Signature, registers: str) -> str:
    """The expression a function returns: what it gives, out of `registers`
    (`r.` for a local, `ctx->` for the machine's)."""
    given = fields(signature.gives)
    if not given:
        return ""
    if len(given) == 1:
        return f"{registers}{given[0][2]}"
    return f"({name}_r){{{', '.join(f'{registers}{member}' for _, _, member in given)}}}"


def taking(name: str, signature: signatures.Signature, call: str, into: str) -> str:
    """A statement that makes `call` and puts what it gives into `into`."""
    given = fields(signature.gives)
    if not given:
        return f"{call};"
    if len(given) == 1:
        return f"{into}{given[0][2]} = {call};"
    return (f"{{ const {name}_r out = {call}; "
            + " ".join(f"{into}{member} = out.{field};" for field, _, member in given) + " }")


def arithmetic_for_locals(lift) -> str:
    """gblift.h's arithmetic, written for a function's local registers."""
    text = (lift.LIFT_INCLUDE / "gblift.h").read_text(encoding="utf-8")
    start, stop = text.find(ARITHMETIC_FROM), text.rfind(ARITHMETIC_TO)
    if start < 0 or stop < start:
        raise lift.LiftError("gblift.h has no arithmetic section where this looks for it")
    section = text[text.index("\n", text.index("*/", start)) + 1:stop]
    section = re.sub(r"/\* SP plus.*?\n}\n\n", "", section, flags=re.S)
    flags = "".join(re.search(rf"static inline void lift_{name}_flags\(GBContext\* ctx\) \{{.*?\n}}\n\n", text,
                              flags=re.S).group(0) for name in ("pack", "unpack"))
    made = re.sub(r"\blift_", "liftr_", (flags + section).replace("GBContext* ctx", "LiftRegs* ctx"))
    if re.search(r"ctx->(sp|pc)|liftr_(read|write|tick|advance)", made):
        raise lift.LiftError("gblift.h's arithmetic uses the machine: it cannot be written for local registers")
    return made.rstrip() + "\n"


# What a function may do and still be known to come back to its caller with
# the stack as it found it.
NESTS_WITH = frozenset(("stop", "halt", "interrupt_control", "bank_switch", "reads_volatile_io"))
SLOT = 1 << 12  # the generator's bits for registers and flags end below this: one bit a pair pushed


def site_nests(site: dict | None, nested: set[str], index: dict[str, dict]) -> bool:
    """Does what this call or jump goes to come back with the stack as it
    found it, as far as the export knows where it goes?"""
    if site is None or "reaches_seen" in site:
        return False
    helper = index.get(site.get("function") or "")
    if "reaches_table" in site:
        return helper is not None and helper["stack_balanced"] and \
            all(entry.get("function") in nested for entry in site["reaches_table"]["entries"])
    if "reaches" in site:
        return helper is not None and helper["stack_balanced"] and site["reaches"].get("function") in nested
    return site.get("function") in nested


def well_nested(export: dict, index: dict[str, dict]) -> set[str]:
    """The functions that return to their caller with the stack as they found
    it, whatever they call: nothing in them or under them takes a return
    address off to go back further, reads what its caller pushed, or goes
    where the export cannot follow. C can call such a function with nothing
    under the call, and one that jumps to it can call it and return.

    This is what the code says, not what a run showed: the checker cannot
    say it, because a state in which the original does not come back to its
    caller is one it throws away."""
    nested = {function["id"] for function in export["functions"]
              if function["stack_balanced"] and set(function["hazards"]) <= NESTS_WITH}
    changed = True
    while changed:
        changed = False
        for function in export["functions"]:
            if function["id"] in nested and not all(
                    site_nests(site, nested, index)
                    for site in list(function["calls"]) + list(function.get("jumps_out", ()))):
                nested.discard(function["id"])
                changed = True
    return nested


class Converter(codegen.Generator):
    """The structured C of a function, read knowing what it and the
    functions it calls take, give and keep."""

    def __init__(self, function: dict, index: dict[str, dict], names: dict[int, str], operands: dict[int, str],
                 signed: dict[str, tuple[int, int, int]], as_c: bool, untyped: set[tuple[str, int]],
                 nested: set[str]):
        self.signed = signed              # function -> (takes, gives, keeps), as the generator's bits
        self.mine = signed[function["id"]]
        self.as_c = as_c
        self.nested = nested
        # What the export says of each call and each way out, by the same names.
        self.sites: dict[tuple[str, int], dict] = {("call", site["site"]): site for site in function["calls"]}
        self.slots: dict[int, int] | None = None  # as C: how many pairs are pushed when an instruction starts
        # What is read after a call that the function called is not said to give or keep.
        self.wants: dict[str, int] = {}
        self.untyped = untyped            # calls and ways out that hand every register over
        self.conflicts: set[tuple[str, int]] = set()  # ...because what the callee leaves undefined is read after
        # The ways out of the function into other code, an instruction: (what
        # it is known by, the function gone to if the export has one, where).
        self.outs: list[list[tuple[tuple[str, int], dict | None, int]]] = []
        self.number: dict[int, int] = {}
        super().__init__(function, index, names, "exact", operands)

    def link(self) -> None:
        super().link()
        self.outs = []
        for i, instruction in enumerate(self.instructions):
            self.number[id(instruction)] = i
            outs = []
            into = list(instruction.leaves_into)
            leaving = {jump["kind"]: jump for jump in self.function.get("jumps_out", ())
                       if jump["site"] == instruction.address}
            if instruction.kind in ("jump", "jump_cc") and not self.stays(instruction):
                outs.append((("jump", instruction.address), into.pop(0), instruction.target))
                if "jump" in leaving:
                    self.sites[outs[-1][0]] = leaving["jump"]
            if instruction.kind not in ("jump", "ret", "reti", "jp_hl") and i in self.last_of_block and \
                    not self.falls_inside(i):
                outs.append((("fall", self.block_end[i]), into.pop(0), self.block_end[i]))
                if "fallthrough" in leaving:
                    self.sites[outs[-1][0]] = leaving["fallthrough"]
            self.outs.append(outs)

    def nests(self, site: tuple[str, int]) -> bool:
        """Does what is gone to here come back with the stack as it found it?"""
        return site_nests(self.sites.get(site), self.nested, self.index)

    def typed(self, site: tuple[str, int], callee: dict | None) -> tuple[int, int, int] | None:
        """What the function gone to here takes, gives and keeps, where the
        call can be written with arguments."""
        if site in self.untyped or callee is None:
            return None
        return self.signed.get(callee["id"])

    def before(self, site: tuple[str, int], callee: dict | None, after: int) -> int:
        """What has to hold before a call, given what is read after it."""
        signature = self.typed(site, callee)
        if signature:
            takes, gives, keeps = signature
            if after & ~(gives | keeps):
                self.conflicts.add(site)
                self.wants[callee["id"]] = self.wants.get(callee["id"], 0) | (after & ~(gives | keeps))
            live = takes | (after & ~gives)
            if not self.as_c:
                # And what the callee may read without its mattering to what it
                # does: a register or the flags it saves and puts back. What it
                # pushes is the original's only if they are the original's here.
                live |= codegen.read_through(callee, 0)
            return live
        live = codegen.read_through(callee, after)
        known = self.signed.get(callee["id"]) if callee else None
        return live | known[0] if known else live

    def read_on_leaving(self, instruction: codegen.Instruction) -> int:  # type: ignore[override]
        # After a return, what the function gives. A conditional return can
        # be the last instruction before the function runs on into the next
        # one: then it is a way out of both kinds.
        outs = self.outs[self.number[id(instruction)]]
        read = self.mine[1] if instruction.kind in ("ret", "ret_cc") else 0
        if instruction.kind == "ret":
            return read
        if not self.as_c:
            return read | (codegen.ALL if outs or instruction.kind != "ret_cc" else 0)
        # Into another function, as C: a call of it and a return, where it is
        # known to come back; and every register handed to the machine where
        # it is not.
        for site, callee, _ in outs:
            read |= self.before(site, callee, self.mine[1]) if self.nests(site) else codegen.ALL
        return read

    def liveness(self) -> None:
        self.conflicts = set()
        self.wants = {}
        if self.as_c and self.slots is None:
            depths = self.pushed()
            self.slots = depths if isinstance(depths, dict) else {}
        slots = self.slots or {}
        changed = True
        while changed:
            changed = False
            for i in range(len(self.instructions) - 1, -1, -1):
                instruction = self.instructions[i]
                after = self.read_on_leaving(instruction) if instruction.exits else 0
                for successor in instruction.successors:
                    after |= self.instructions[successor].live_in
                kind = instruction.kind
                if kind in ("call", "call_cc", "rst"):
                    live = self.before(("call", instruction.address), instruction.callee, after)
                    if kind == "call_cc":
                        live |= after
                    if i not in self.conditions:
                        live |= instruction.reads
                elif i in self.conditions:
                    live = after
                elif kind == "plain" and instruction.data[0] in PUSHES and instruction.address in slots:
                    # As C, a push is a local given the pair, and read only if
                    # something takes it out again and uses what it took.
                    slot = SLOT << slots[instruction.address]
                    live = (after & ~slot) | (instruction.reads if after & slot else 0)
                elif kind == "plain" and instruction.data[0] in POPS and instruction.address in slots:
                    slot = SLOT << (slots[instruction.address] - 1)
                    live = (after & ~instruction.writes) | (slot if after & instruction.writes else 0)
                else:
                    live = (after & ~instruction.writes) | instruction.reads
                if after != instruction.live_after or live != instruction.live_in:
                    instruction.live_after = after
                    instruction.live_in = live
                    changed = True

    def pushed(self) -> dict[int, int] | str:
        """How many pairs the function has pushed when each instruction starts,
        by address; or why it cannot keep them in locals."""
        depth = {0: 0}
        work = [0]
        while work:
            i = work.pop()
            instruction = self.instructions[i]
            after = depth[i]
            if instruction.kind == "plain" and instruction.data[0] in PUSHES:
                after += 1
            elif instruction.kind == "plain" and instruction.data[0] in POPS:
                after -= 1
                if after < 0:
                    return "it pops what it did not push"
            if instruction.exits and after:
                return "it leaves with something still pushed"
            for successor in instruction.successors:
                if successor not in depth:
                    depth[successor] = after
                    work.append(successor)
                elif depth[successor] != after:
                    return "what it has pushed depends on the way an instruction is reached"
        if len(depth) != len(self.instructions):
            return "part of it is not reached from its start"
        for i, instruction in enumerate(self.instructions):
            # With something pushed, what is called has to leave the stack
            # alone: the original's callee would find the pushes there, and
            # this one's does not.
            if instruction.kind in ("call", "call_cc", "rst") and depth[i] and \
                    not self.nests(("call", instruction.address)):
                return "it calls, with something pushed, what may not leave the stack as it found it"
        return {self.instructions[i].address: pushed for i, pushed in depth.items()}


    def unchanged(self, proved: dict[str, int]) -> int:
        """The registers and flags the function, written as C, is sure to leave
        as it found them, whichever way it goes: never written, or pushed
        with what they came with and taken off again into the same pair, and
        left so by everything it calls (`proved`, by function).

        A line of signatures.txt says "keeps" of what came out as it went in
        from every state the checker tried. That is not this: a path none of
        them took may change it."""
        if not self.as_c or not self.slots:
            return 0
        everything = codegen.ALL
        states: dict[int, tuple[int, tuple[int, ...]]] = {0: (everything, ())}
        work = [0]
        left = everything
        while work:
            i = work.pop()
            instruction = self.instructions[i]
            held, pushed = states[i]
            kind = instruction.kind
            if kind == "plain" and instruction.data[0] in PUSHES:
                pair = instruction.reads
                pushed = pushed + ((pair if not pair & ~held else 0),)
            elif kind == "plain" and instruction.data[0] in POPS:
                came = pushed[-1] if pushed else 0
                pushed = pushed[:-1]
                held = held | instruction.writes if came == instruction.writes else held & ~instruction.writes
            elif kind in ("call", "call_cc", "rst"):
                held &= proved.get(instruction.callee["id"], 0) if instruction.callee else 0
            else:
                held &= ~instruction.writes
            if instruction.exits:
                out = held
                for site, callee, _ in self.outs[i]:
                    out &= proved.get(callee["id"], 0) if callee is not None and self.nests(site) else 0
                left &= out
            for successor in instruction.successors:
                if successor not in states:
                    states[successor] = (held, pushed)
                    work.append(successor)
                    continue
                was, was_pushed = states[successor]
                merged = (was & held, tuple(a if a == b else 0 for a, b in zip(was_pushed, pushed)))
                if merged != states[successor]:
                    states[successor] = merged
                    work.append(successor)
        return left


REGISTER = re.compile(r"ctx->(af|bc|de|hl|f_z|f_n|f_h|f_c|a|b|c|d|e|h|l|f)\b")
ARITHMETIC = re.compile(r"\blift_(" + "|".join(REGISTERS_ONLY) + r")\(ctx\b")
# A call, a return and a jump out, as the structured style writes them, each
# with the time it passes first where it passes any.
TIME = r"(?:lift_advance\(ctx, (\d+)\); )?"
CALL = re.compile(TIME + r"if \(lift_call\(ctx, (0x[0-9A-F]{4})( /\*.*?\*/)?, (0x[0-9A-F]{4})\)\) return;")
RETURN = re.compile(TIME + r"lift_ret\(ctx\); return;")
JUMP = re.compile(TIME + r"lift_jump\(ctx, (0x[0-9A-F]{4})( /\*.*?\*/)?\); return;")
PUSH = re.compile(r"lift_push16\(ctx, r\.(bc|de|hl)\);")
POP = re.compile(r"r\.(bc|de|hl) = lift_pop16\(ctx\);")


def why_not(function: dict) -> str | None:
    """Why this is not converted, where the function's own code says so."""
    for block in function["blocks"]:
        for raw in block["instructions"]:
            data = bytes.fromhex(raw["bytes"])
            kind = codegen.control_kind(data)
            if kind == "jp_hl":
                return "it jumps through a register"
            if kind == "reti":
                return "it returns from an interrupt"
            if kind == "plain" and data[0] in STACK_POINTER:
                return "it reckons with the stack pointer"
    return None


class Converted:
    """One function, converted."""

    def __init__(self) -> None:
        self.text = ""
        self.as_c = False
        self.why_not_c = ""       # why it is on the machine's stack
        self.callable = False     # as C, and it comes back with the stack as it found it: C can call it
        self.may_read = 0         # what its code may read of what it is entered with, as a signature's bits
        self.unchanged = 0        # as C: what it is sure to leave as it found it, as the generator's bits
        self.wants: dict[str, int] = {}  # what it reads after a call that the callee is not said to give or keep
        self.through: set[str] = set()   # the functions it calls through the machine with arguments
        self.counts = {"direct": 0, "typed": 0, "all": 0, "jumps": 0, "away": 0, "returns": 0}


def converted(function: dict, index: dict[str, dict], names: dict[int, str], operands: dict[int, str],
              signed: dict[str, tuple[int, int, int]], lines_of: dict[str, signatures.Signature],
              c_names: dict[str, str], may_be_c: bool, callable_: set[str], nested: set[str],
              proved: dict[str, int]) -> Converted:
    """The function as C with arguments and its binding. `may_be_c` is false
    for one to keep on the machine's stack; `callable_` are the functions C
    can call, and `nested` the ones known to come back to their caller with
    the stack as they found it. `lines_of` has what each function takes as
    wide as its code may read, which is what the C passes, and `proved` what
    each function is sure to leave as it found it."""
    made = Converted()
    as_c = may_be_c
    while True:
        untyped: set[tuple[str, int]] = set()
        wants: dict[str, int] | None = None
        while True:
            generator = Converter(function, index, names, operands, signed, as_c, untyped, nested)
            if wants is None:
                wants = dict(generator.wants)  # before any call was made to hand everything over for it
            if generator.conflicts <= untyped:
                break
            untyped |= generator.conflicts
        depths = generator.pushed() if as_c else {}
        if not isinstance(depths, str):
            break
        made.why_not_c, as_c = depths, False
    made.as_c = as_c
    made.may_read = signatures._bits(generator.instructions[0].live_in & codegen.ALL)
    made.wants = {callee: signatures._bits(bits) for callee, bits in (wants or {}).items()}
    made.unchanged = generator.unchanged(proved)
    live = {instruction.address: instruction for instruction in generator.instructions}
    name = c_names[function["id"]]
    signature = lines_of[function["id"]]
    counts = made.counts
    at = {instruction.address: instruction for instruction in generator.instructions}
    ways_out = {site[1]: (site, callee) for outs in generator.outs for site, callee, _ in outs}

    def near(callee: dict) -> bool:
        """Is the callee's code mapped whenever this function runs?"""
        return callee["bank"] == 0 or (callee["bank"] == function["bank"] and function["address"] >= 0x4000)

    def passed(cycles: str | None, with_machine: bool) -> str:
        """The time an instruction that hands control on takes. Through the
        machine it is only added: the machine takes an interrupt that is due
        before it runs anything. From C to C nothing else would, until the
        next instruction ends, and the handler would run one instruction
        late: so there the time is passed with lift_tick, which takes it."""
        if cycles is None:
            return ""
        return f"lift_advance(ctx, {cycles}); " if with_machine else f"lift_tick(ctx, {cycles}); "

    def call(site: tuple[str, int], callee: dict | None, cycles: str | None, target: str, note: str,
             back: str) -> str:
        if generator.typed(site, callee) is None:
            counts["all"] += 1
            return passed(cycles, True) + f"lift_call_all(ctx, &r, {target}{note}, {back});"
        callee_id = callee["id"]
        callee_name, line = c_names[callee_id], lines_of[callee_id]
        arguments = "".join(f", r.{member}" for _, _, member in fields(line.takes))
        if as_c and callee_id in callable_ and near(callee):
            counts["direct"] += 1
            return passed(cycles, False) + taking(callee_name, line, f"{callee_name}(ctx{arguments})", "r.")
        counts["typed"] += 1
        made.through.add(callee_id)
        return passed(cycles, True) + taking(callee_name, line,
                                             f"{callee_name}_call(ctx, &r, {back}{arguments})", "r.")

    def returns(match: re.Match) -> str:
        counts["returns"] += 1
        result = result_of(name, signature, "r.")
        if as_c:
            return passed(match.group(1), False) + (f"return {result};" if result else "return;")
        # On the machine's stack, the machine's registers are left as the
        # function's are: what it keeps is what it has at the end, which in a
        # state where code wrote over its own stack is not what it had at the
        # start.
        return passed(match.group(1), True) + "liftr_to(ctx, &r); " + (f"return {result};" if result else "return;")

    def rewrite(text: str, address: int) -> str:
        text = REGISTER.sub(r"r.\1", text)
        text = ARITHMETIC.sub(r"liftr_\1(&r", text)
        if as_c:
            # What it pushes is kept in locals: the first pair pushed in s0, one
            # pushed on top of it in s1.
            # A pair pushed only to be put back, with nothing reading it in
            # between that the function gives, is not kept at all: a caller's
            # registers are its own, and the function's are these.
            depth = depths.get(address, 0)
            instruction = live.get(address)
            kept = instruction is not None and bool(instruction.live_after & (SLOT << depth))
            used = instruction is not None and bool(instruction.live_after & instruction.writes)
            text = PUSH.sub(lambda match: f"s{depth} = r.{match.group(1)};" if kept else "", text)
            text = POP.sub(lambda match: f"r.{match.group(1)} = s{depth - 1};" if used else "", text)
            text = text.replace("lift_push_af(ctx);", f"liftr_pack_flags(&r); s{depth} = r.af;" if kept else "")
            text = text.replace("lift_pop_af(ctx);", f"r.af = s{depth - 1}; liftr_unpack_flags(&r);" if used else "")
            text = text.strip()
        else:
            text = text.replace("lift_push_af(ctx);", "liftr_pack_flags(&r); lift_push16(ctx, r.af);")
            text = text.replace("lift_pop_af(ctx);", "r.af = lift_pop16(ctx); liftr_unpack_flags(&r);")
        if address in at:
            instruction = at[address]
            text = CALL.sub(lambda match: call(("call", address), instruction.callee, match.group(1),
                                               match.group(2), match.group(3) or "", match.group(4)), text)
        text = RETURN.sub(returns, text)

        def jumps(match: re.Match) -> str:
            counts["jumps"] += 1
            site, callee = ways_out[address]
            if not as_c or not generator.nests(site):
                # The machine goes on from there for good. As C that is only
                # right with nothing under this function but its caller's
                # return address, which is so when its binding called it.
                counts["away"] += 1
                return passed(match.group(1), True) + \
                    f"lift_jump_all(ctx, &r, {match.group(2)}{match.group(3) or ''});"
            # As C, into a function known to come back: a call of it, and a
            # return. What it finds on the stack to come back to is this
            # place.
            result = result_of(name, signature, "r.")
            return (call(site, callee, match.group(1), match.group(2), match.group(3) or "", f"0x{address:04X}")
                    + (f" return {result};" if result else " return;"))

        return JUMP.sub(jumps, text) if address in ways_out else text

    nodes = generator.build_nodes()
    for node in nodes:
        for key in ("lines", "flush"):
            if key in node:
                node[key] = [rewrite(line, node["address"]) for line in node[key]]
        for key in ("branch", "step", "condition"):
            if node.get(key):
                node[key] = rewrite(node[key], node["address"])
    taken = fields(signature.takes)
    if as_c:
        body = ["    LiftRegs r = {0};"]
        deepest = max(depths.values(), default=0) + 1
        slots = [f"s{slot}" for slot in range(deepest)
                 if any(f"s{slot} = " in line for node in nodes for line in node.get("lines", ()))]
    else:
        # The registers start as the machine has them. What the function takes
        # is what it is given; of the rest it may still save one and put it
        # back, and then what it pushes has to be what the original pushes:
        # other code can read the stack (a copy that runs over it does).
        body = ["    LiftRegs r;", "    liftr_from(&r, ctx);"]
        slots = []
    if taken:
        body.append("    " + " ".join(f"r.{member} = {argument};" for argument, _, member in taken))
    if slots:
        body.append(f"    uint16_t {', '.join(slots)};")
    locals_ = [generator.capture_names[key] for key in sorted(generator.capture_names)]
    if locals_:
        body.append(f"    bool {', '.join(locals_)};")
    used: set[int] = set()
    body += codegen.emit(codegen.structure(nodes), used)
    made.callable = as_c and function["id"] in nested and not counts["away"]

    left = signatures.EVERYTHING & ~(signature.gives | signature.keeps)
    call_it = f"{name}(ctx{''.join(f', in.{member}' for _, _, member in taken)})"
    binding = [f"GBLIFT_FUNCTION(0x{function['bank']:02X}, 0x{function['address']:04X}, {name}_binding) {{",
               "    LiftRegs in;",
               "    LiftFrame frame;"]
    if as_c:
        binding.append("    lift_own_stack(ctx);")
    binding += ["    liftr_from(&in, ctx);",
                "    LIFT_ENTER(frame);",
                "    " + taking(name, signature, call_it, "ctx->"),
                "    LIFT_LEAVE(frame);"]
    kept = fields(signature.keeps)
    if as_c and kept:
        # As C it has not touched the machine's registers, and what it called
        # through the machine has: what it keeps is what came in.
        binding.append("    " + " ".join(f"ctx->{member} = in.{member};" for _, _, member in kept))
    if left:
        binding.append(f"    lift_undefined(ctx, 0x{left:03X}u);  /* {signatures.words(left)} */")
    binding += ["    lift_ret(ctx);", "}"]
    header = [f"/* {function['id']}  {codegen.display(function)}  ({function['instruction_count']} instructions) */",
              f"/* takes {signatures.words(signature.takes)}  gives {signatures.words(signature.gives)}  "
              f"keeps {signatures.words(signature.keeps)} */"]
    text = "\n".join(header + [f"{prototype(name, signature)} {{"] + body + ["}", ""] + binding) + "\n"
    made.text = source.tidy(text.split("\n"))
    return made


def declarations(function_ids: list[str], through: set[str], index: dict[str, dict],
                 lines_of: dict[str, signatures.Signature], c_names: dict[str, str]) -> str:
    out: list[str] = []
    for function_id in sorted(set(function_ids) | through):
        name, signature, function = c_names[function_id], lines_of[function_id], index[function_id]
        given = fields(signature.gives)
        out.append(f"/* {source.reference(function_id)}  takes {signatures.words(signature.takes)}  "
                   f"gives {signatures.words(signature.gives)}  keeps {signatures.words(signature.keeps)} */")
        if len(given) > 1:
            out.append(f"typedef struct {name}_r {{ "
                       + " ".join(f"{ctype} {field};" for field, ctype, _ in given) + f" }} {name}_r;")
        if function_id in function_ids:
            out.append(prototype(name, signature) + ";")
        hand = " ".join(f"ctx->{member} = {argument};" for argument, _, member in fields(signature.takes))
        result = result_of(name, signature, "ctx->")
        call = f"    if (lift_call(ctx, 0x{function['address']:04X}, return_address)) lift_left();"
        if function_id in through:
            out += [f"static inline {prototype(name, signature, True)} {{", "    liftr_to(ctx, r);"] + \
                   ([f"    {hand}"] if hand else []) + [call, "    liftr_from(r, ctx);"] + \
                   ([f"    return {result};"] if result else []) + ["}"]
        out.append("")
    return "\n".join(out)


def shape(made: Converted) -> str:
    if not made.as_c:
        return "on the machine's stack"
    return "as C that C can call" if made.callable else "as C, for the machine to call"


def command_convert(args, lift) -> int:
    folder = Path(args.folder).resolve()
    record = source.load_record(folder, lift)
    export_path = folder / source.TOOL / "export.json"
    export = lift.load_export(export_path)
    index = lift.function_index(export)
    _, units = source.read_folder(folder, lift)
    rom = args.rom or record.get("rom_path")
    if not rom or not Path(rom).is_file():
        raise lift.LiftError("no ROM: give --rom" if not rom else f"no ROM at {rom}: give --rom")
    signed_lines = signatures.read(folder, lift)
    if not signed_lines:
        raise lift.LiftError(f"{folder} has no {signatures.FILE}: run `gb_lift.py signatures` on it first")
    out = Path(args.output).resolve() if args.output else folder / source.TOOL / "convert"
    out.mkdir(parents=True, exist_ok=True)

    # The names the folder's source goes by.
    names_path = folder / source.NAMES
    if names_path.is_file():
        source.apply_names(export, source.read_names(names_path.read_text(encoding="utf-8", errors="replace")), lift)
    c_names = lift.function_names(export)
    names = lift.memory_names(export, set(c_names.values()))
    lines_of = {function_id: line[0] for function_id, line in signed_lines.items() if function_id in index}
    operands: dict[str, dict[int, str]] = {}
    operands_path = folder / source.TOOL / source.OPERANDS
    if operands_path.is_file():
        for entry in json.loads(operands_path.read_text(encoding="utf-8"))["operands"]:
            offset = entry["offset"] - 1
            operands.setdefault(entry["function"], {})[offset if offset < 0x4000 else 0x4000 + offset % 0x4000] = \
                entry["name"]

    functions = record["functions"]
    # A line is believed where `build` has held the function to it and found a
    # match: only such a function is converted, and only such a one is called
    # with arguments.
    held = {function_id for function_id, line in signed_lines.items()
            if function_id in lines_of and functions.get(function_id, {}).get("verdict") == "match" and
            functions[function_id].get("signature") == source.digest(line[1])}
    signed = {function_id: (to_codegen(lines_of[function_id].takes), to_codegen(lines_of[function_id].gives),
                            to_codegen(lines_of[function_id].keeps)) for function_id in held}
    wanted = {lift.parse_function_ref(text) for text in args.function}
    for function_id in sorted(wanted - set(units)):
        raise lift.LiftError(f"{source.reference(function_id)} is not a function of the folder")

    left_out: dict[str, list[str]] = {}
    candidates: list[str] = []
    for function_id in sorted(wanted or units):
        function = index.get(function_id)
        entry = functions.get(function_id, {})
        text = units[function_id][1].text
        if function is None:
            why = "the export has no such function"
        elif any(hazard in function["hazards"] for hazard in lift.UNLIFTABLE):
            why = "it cannot be lifted"
        elif entry.get("written") != source.digest(text) and \
                (not entry.get("written") or source.comments_added(text, entry["written"]) is None):
            why = "somebody has changed its code"
        elif entry.get("verdict") != "match":
            why = "the checker has not found it to match (run `build`)"
        elif function_id not in lines_of:
            why = f"it has no line in {signatures.FILE}"
        elif function_id not in held:
            why = f"`build` has not held it to its line in {signatures.FILE}"
        else:
            try:
                why = why_not(function)
            except (codegen.Unliftable, lift.Unliftable) as reason:
                why = str(reason)
        if why:
            left_out.setdefault(why, []).append(function_id)
        else:
            candidates.append(function_id)

    nested = well_nested(export, index)
    # What a function is given, in C. Its line says what was seen to matter,
    # from the states the checker tried, and a register read only on a path
    # none of them took is not in it. A lifted function has the register all
    # the same. A C function has what it is passed and nothing else, so it is
    # passed whatever its code may read on any path: what the line says, and
    # what the code and the functions it calls say more. A function that is
    # not converted may read whatever the export says it needs.
    wide = dict(lines_of)
    for function_id in held - set(candidates):
        needs = signatures._bits(codegen.read_through(index[function_id], 0))
        wide[function_id] = signatures.Signature(lines_of[function_id].takes | needs, lines_of[function_id].gives,
                                                 lines_of[function_id].keeps)

    # What a function is sure to leave as it found it. A line says "keeps" of
    # what came out as it went in from every state that was tried; C that is
    # called with arguments has to be sure, since its caller goes on with the
    # value it had. For a function written as C it is worked out from its
    # code (`unchanged`), starting from everything and taking away. For any
    # other it is what the export never saw written, in it or under it.
    def never_written(function: dict) -> int:
        return 0 if function["summary_incomplete"] else codegen.ALL & ~codegen.summary_bits(function, "writes")

    proved = {function["id"]: never_written(function) for function in export["functions"]}
    for function_id in candidates:
        proved[function_id] = codegen.ALL

    def convert_all(not_c: set[str]) -> dict[str, Converted]:
        """Every candidate converted, the ones in `not_c` on the machine's
        stack. The interface each has in C is worked out over all of them
        together, and only ever grows: a function is passed what its code may
        read, gives what a caller may read after it and what its line calls
        kept without its code bearing that out, and keeps what its code is
        sure to leave alone. Which functions C can call follows from which
        came out as C. It is done when a pass changes none of that."""
        callable_: set[str] = set()
        while True:
            for function_id in held:
                signed[function_id] = (to_codegen(wide[function_id].takes), to_codegen(wide[function_id].gives),
                                       to_codegen(wide[function_id].keeps))
            made = {}
            for function_id in candidates:
                try:
                    made[function_id] = converted(index[function_id], index, names, operands.get(function_id, {}),
                                                  signed, wide, c_names, not args.machine_stack and
                                                  function_id not in not_c, callable_, nested, proved)
                except (codegen.Unliftable, lift.Unliftable) as reason:
                    left_out.setdefault(str(reason), []).append(function_id)
            for function_id in [function_id for function_id in candidates if function_id not in made]:
                candidates.remove(function_id)
            wanted: dict[str, int] = {}
            for one in made.values():
                for callee, bits in one.wants.items():
                    wanted[callee] = wanted.get(callee, 0) | bits
            grew = False
            for function_id, one in made.items():
                line, now = lines_of[function_id], wide[function_id]
                sure = one.unchanged if one.as_c else never_written(index[function_id])
                if proved[function_id] != sure:
                    proved[function_id] = sure
                    grew = True
                gives = now.gives | wanted.get(function_id, 0)
                if one.as_c:
                    gives |= line.keeps & ~signatures._bits(sure)
                new = signatures.Signature(now.takes | one.may_read, gives, line.keeps & ~gives)
                if not new.same(now):
                    wide[function_id] = new
                    grew = True
            now_callable = {function_id for function_id, one in made.items() if one.callable}
            if not grew and now_callable == callable_:
                return made
            callable_ = now_callable

    def write(made: dict[str, Converted]) -> list[Path]:
        through: set[str] = set()
        for one in made.values():
            through |= one.through
        head = ("/* What the converted functions are written against: made by tools/gb_lift.py convert\n"
                " * (tools/gb_lift_convert.py), and made again every time it runs. */\n"
                "#ifndef CONVERTED_H\n#define CONVERTED_H\n\n"
                f'#include "gblift.h"\n#include "{lift.NAMES_HEADER}"\n\n#include <setjmp.h>\n#include <stdbool.h>\n\n')
        source.write_text(out / HEADER, head + SUPPORT_HEAD + "\n/* gblift.h's arithmetic, on local registers. */\n\n"
                          + arithmetic_for_locals(lift) + "\n" + DECLARATIONS_HEAD
                          + declarations(list(made), through, index, wide, c_names) + "#endif\n")
        source.write_text(out / SUPPORT, f'#include "{HEADER}"\n\nLiftFrame* lift_frame;\n')
        by_bank: dict[int, list[str]] = {}
        for function_id in made:
            by_bank.setdefault(index[function_id]["bank"], []).append(function_id)
        for stale in out.glob("converted_bank_*.c"):
            stale.unlink()
        written = []
        for bank, function_ids in sorted(by_bank.items()):
            path = out / f"converted_bank_{bank:02x}.c"
            preamble = (f"/* Bank {bank:02X} of {export['rom']['title'] or 'a ROM'}: its lifted functions as C with "
                        "arguments.\n * Written by tools/gb_lift.py convert from the source folder and its "
                        f"{signatures.FILE}. */\n#include \"{HEADER}\"\n")
            source.write_text(path, preamble + "\n" + "\n".join(made[function_id].text for function_id in function_ids))
            written.append(path)
        return written

    def check(written: list[Path], function_ids: list[str]) -> dict[str, str]:
        """Held to the ROM's code, each to its line, as `build` holds a lifted function."""
        scratch = source.read_scratch(folder, lift)
        terms = {}
        for function_id in function_ids:
            term = signatures.contract_entry(lines_of[function_id])
            if scratch:
                term["ignore"] = scratch
            terms[source.reference(function_id)] = term
        contract = out / "contract.json"
        source.write_text(contract, json.dumps({"functions": terms}, indent=1) + "\n")
        check_report = out / "check.json"
        check_report.unlink(missing_ok=True)
        command = ["check", str(export_path), "--rom", str(rom), "--work", str(out / "check"), "--json",
                   str(check_report), "--trials", str(args.trials), "--seed", str(args.seed), "--contract",
                   str(contract), "--support", str(out / SUPPORT), "--include", str(folder / source.SOURCES),
                   "--include", str(out)]
        for path in written:
            command += ["--source", str(path)]
        for image in sorted((folder / "images").glob("*.img")):
            command += ["--image", str(image.resolve())]
        for entries in sorted((folder / "entries").glob("*.entries")):
            command += ["--entries", str(entries.resolve())]
        if args.cc:
            command += ["--cc", args.cc]
        if args.jobs:
            command += ["--jobs", str(args.jobs)]
        run = lift.build_parser().parse_args(command)
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            run.run(run)
        source.write_text(out / "check.log", said.getvalue())
        results = {entry["function"]: entry
                   for entry in json.loads(check_report.read_text(encoding="utf-8"))["results"]}
        return {function_id: results.get(function_id, {}).get("verdict", "inconclusive")
                for function_id in function_ids}

    not_c: set[str] = set(KEEP_ON_MACHINE)
    made = convert_all(not_c)
    written = write(made)
    verdicts: dict[str, str] = {}
    started = time.monotonic()
    rounds = 0
    if made and not args.no_check:
        print(f"checking {len(made)} converted functions against the ROM")
        sys.stdout.flush()
        while True:
            verdicts = check(written, list(made))
            rounds += 1
            # A function that does not hold as C is put on the machine's stack,
            # where the conversion changes less, and everything is checked
            # again: what called it as C now calls it through the machine.
            failed = {function_id for function_id, verdict in verdicts.items()
                      if verdict != "match" and made[function_id].as_c}
            if not failed or rounds >= ROUNDS:
                break
            print(f"  {len(failed)} do not hold as C: written again on the machine's stack, and all checked again")
            sys.stdout.flush()
            not_c |= failed
            made = convert_all(not_c)
            written = write(made)

    totals = {key: sum(one.counts[key] for one in made.values())
              for key in ("direct", "typed", "all", "jumps", "away")}
    wider = sum(1 for function_id in made if wide[function_id].takes != lines_of[function_id].takes)
    gives_more = sum(1 for function_id in made if wide[function_id].gives != lines_of[function_id].gives)
    shapes = {function_id: shape(one) for function_id, one in made.items()}
    print(f"{len(made)} of {len(wanted or units)} functions written as C with arguments, in {out}")
    for why, function_ids in sorted(left_out.items(), key=lambda item: -len(item[1])):
        print(f"  {len(function_ids):5d} left as they are: {why}")
    by_shape: dict[str, list[int]] = {}
    for function_id, said in shapes.items():
        tally = by_shape.setdefault(said, [0, 0])
        tally[0] += 1
        tally[1] += verdicts.get(function_id) == "match"
    for said, (count, matching) in sorted(by_shape.items(), key=lambda item: -item[1][0]):
        print(f"  {count:5d} {said}" + (f": {matching} match" if verdicts else ""))
    reasons: dict[str, int] = {}
    for function_id, one in made.items():
        if not one.as_c:
            why = "it did not hold as C" if function_id in not_c else one.why_not_c or "it was asked for"
            reasons[why] = reasons.get(why, 0) + 1
    for why, count in sorted(reasons.items(), key=lambda item: -item[1]):
        print(f"        {count:5d} of those because {why}")
    print(f"  calls: {totals['direct']} from C to C, {totals['typed']} with arguments through the machine, "
          f"{totals['all']} with every register handed over; {totals['jumps'] - totals['away']} of them where a "
          f"function left by a jump, and {totals['away']} jumps that stay jumps")
    print(f"  {wider} functions are passed more than their line says they take: what their code may read on a "
          f"way the checker's states did not go")
    print(f"  {gives_more} give more than their line says: what a caller may read after them, and what the line "
          f"calls kept without the code bearing it out")
    report = {"schema": "gbrecomp.lift-convert", "version": 2, "converted": sorted(made),
              "shapes": shapes, "left_out": {why: sorted(ids) for why, ids in left_out.items()},
              "not_as_c": {function_id: ("it did not hold as C" if function_id in not_c else one.why_not_c)
                           for function_id, one in made.items() if not one.as_c},
              "passed": {function_id: signatures.words(wide[function_id].takes) for function_id in made
                         if wide[function_id].takes != lines_of[function_id].takes},
              "given": {function_id: signatures.words(wide[function_id].gives) for function_id in made
                        if wide[function_id].gives != lines_of[function_id].gives},
              "calls": totals}
    if not verdicts:
        source.write_text(out / REPORT, json.dumps(report, indent=1) + "\n")
        return 0
    counts = {word: sum(1 for verdict in verdicts.values() if verdict == word)
              for word in ("match", "mismatch", "inconclusive")}
    print(f"  {counts['match']} match the ROM's code, {counts['mismatch']} differ, {counts['inconclusive']} the "
          f"checker could not run ({time.monotonic() - started:.0f} s, checked {rounds} "
          f"time{'' if rounds == 1 else 's'})")
    differing = sorted(function_id for function_id, verdict in verdicts.items() if verdict != "match")
    for function_id in differing[:20]:
        print(f"  {verdicts[function_id]}: {source.reference(function_id)}  {c_names[function_id]}")
    if len(differing) > 20:
        print(f"  and {len(differing) - 20} more ({out / REPORT})")
    report["verdicts"] = verdicts
    report["summary"] = counts
    source.write_text(out / REPORT, json.dumps(report, indent=1) + "\n")
    if args.write:
        put = write_into(folder, out, {function_id: one.text for function_id, one in made.items()
                                       if verdicts[function_id] == "match"}, lift)
        print(f"  {put} of them put into {folder / source.SOURCES} where their lifted functions stood, with "
              f"{INTO}/ beside them: `build` checks them again and builds the game with them")
    return 1 if differing else 0


INTO = "converted"  # the folder under src/ the converted functions' header and support go into


def write_into(folder: Path, out: Path, texts: dict[str, str], lift) -> int:
    """Put converted functions into a source folder, each where its lifted
    function stands, and what they are written against into a folder under
    src/. From then on they are functions somebody changed, as ones written
    by hand are: `source` keeps them as they are, and `build` checks them
    against the ROM with that folder's code compiled in. They call each
    other by name, so one cannot be put back as lifted by itself."""
    sources = folder / source.SOURCES
    (sources / INTO).mkdir(parents=True, exist_ok=True)
    for name in (HEADER, SUPPORT):
        source.write_text(sources / INTO / name, (out / name).read_text(encoding="utf-8"))
    include = f'#include "{INTO}/{HEADER}"'
    put = 0
    for path in sorted(sources.glob("*.c")):
        preamble, units, tail = source.parse_source(path.read_text(encoding="utf-8"), f"{source.SOURCES}/{path.name}",
                                                    lift)
        if not any(unit.id in texts for unit in units):
            continue
        # Said with each function: a file's first lines are the file's, and may be somebody's.
        placed = [(include + "\n" + texts[unit.id] if unit.id in texts else unit.text).rstrip("\n")
                  for unit in units]
        put += sum(1 for unit in units if unit.id in texts)
        source.write_text(path, source.assemble(preamble, placed, tail))
    return put
