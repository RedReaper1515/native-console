#!/usr/bin/env python3
"""Structured C for a lifted Game Boy function.

`gb_lift.py baseline --style structured` uses this. Where the mechanical
style writes one group of statements per instruction and a `goto` per jump,
this one:

- works out which flags are still needed after each instruction, and writes
  plain C (`ctx->b--`, `ctx->a &= 0x7F`) where none is;
- writes each conditional jump, return and call as a test of what the one
  instruction that set its flag compared (`if (ctx->a == 5)`), when there is
  one such instruction; where its operands are gone by then, the answer is
  kept in a local (`carry = ctx->a < ctx->b;`);
- turns jumps into `if`, `else`, `do ... while` and `for (;;)` where the code
  is laid out that way, and leaves a `goto` where it is not;
- names hardware registers, and RAM and functions when the export has symbols.

It stays a transliteration: registers are still `ctx->a`. What it writes is
checked like any other lifted source; nothing here is trusted.

Time is kept one of two ways. `exact`: every instruction ends with
`lift_step`, as in the mechanical style, so the game runs cycle for cycle as
before. `block`: time is added up over each straight run of statements and
passed at its end, which reads better and keeps the total, but moves when an
interrupt can arrive by up to the length of the run.
"""

from __future__ import annotations

REG_NAMES = ("b", "c", "d", "e", "h", "l", None, "a")
PAIR_NAMES = ("bc", "de", "hl", "sp")
BIT = {"b": 1, "c": 2, "d": 4, "e": 8, "h": 16, "l": 32, "a": 64}
FZ, FN, FH, FC = 256, 512, 1024, 2048
FLAGS = FZ | FN | FH | FC
ALL = 127 | FLAGS
PAIR_BITS = (BIT["b"] | BIT["c"], BIT["d"] | BIT["e"], BIT["h"] | BIT["l"], 0)
HL = BIT["h"] | BIT["l"]
FLAG_BY_NAME = {"z": FZ, "n": FN, "h": FH, "c": FC}

# Instructions whose one memory access the hardware makes in their last cycle.
LATE_ACCESS = frozenset(
    [0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77, 0x36,
     0x0A, 0x1A, 0x02, 0x12, 0xFA, 0xEA, 0xF0, 0xE0, 0xF2, 0xE2, 0x22, 0x2A, 0x32, 0x3A,
     0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE])

HARDWARE_NAMES = {
    0xFF00: "rP1", 0xFF01: "rSB", 0xFF02: "rSC", 0xFF04: "rDIV", 0xFF05: "rTIMA", 0xFF06: "rTMA",
    0xFF07: "rTAC", 0xFF0F: "rIF", 0xFF10: "rNR10", 0xFF11: "rNR11", 0xFF12: "rNR12",
    0xFF13: "rNR13", 0xFF14: "rNR14", 0xFF16: "rNR21", 0xFF17: "rNR22", 0xFF18: "rNR23",
    0xFF19: "rNR24", 0xFF1A: "rNR30", 0xFF1B: "rNR31", 0xFF1C: "rNR32", 0xFF1D: "rNR33",
    0xFF1E: "rNR34", 0xFF20: "rNR41", 0xFF21: "rNR42", 0xFF22: "rNR43", 0xFF23: "rNR44",
    0xFF24: "rNR50", 0xFF25: "rNR51", 0xFF26: "rNR52", 0xFF40: "rLCDC", 0xFF41: "rSTAT",
    0xFF42: "rSCY", 0xFF43: "rSCX", 0xFF44: "rLY", 0xFF45: "rLYC", 0xFF46: "rDMA", 0xFF47: "rBGP",
    0xFF48: "rOBP0", 0xFF49: "rOBP1", 0xFF4A: "rWY", 0xFF4B: "rWX", 0xFF4D: "rKEY1",
    0xFF4F: "rVBK", 0xFF51: "rHDMA1", 0xFF52: "rHDMA2", 0xFF53: "rHDMA3", 0xFF54: "rHDMA4",
    0xFF55: "rHDMA5", 0xFF56: "rRP", 0xFF68: "rBCPS", 0xFF69: "rBCPD", 0xFF6A: "rOCPS",
    0xFF6B: "rOCPD", 0xFF70: "rSVBK", 0xFFFF: "rIE",
}


class Unliftable(Exception):
    pass


def reg_bit(index: int) -> int:
    name = REG_NAMES[index & 7]
    return BIT[name] if name else 0


def operand_reads(index: int) -> int:
    return HL if (index & 7) == 6 else reg_bit(index)


def effects(data: bytes) -> tuple[int, int]:
    """(reads, writes) of one instruction, as register and flag bits."""
    op = data[0]
    if op == 0xCB:
        cb = data[1]
        reg, kind, group = cb & 7, (cb >> 3) & 7, cb >> 6
        reads = operand_reads(reg)
        if group == 0:
            return reads | (FC if kind in (2, 3) else 0), reg_bit(reg) | FLAGS
        if group == 1:
            return reads, FZ | FN | FH
        return reads, reg_bit(reg)
    if 0x40 <= op <= 0x7F:
        if op == 0x76:
            return 0, 0
        dst, src = (op >> 3) & 7, op & 7
        return operand_reads(src) | (HL if dst == 6 else 0), reg_bit(dst)
    if 0x80 <= op <= 0xBF:
        kind, src = (op >> 3) & 7, op & 7
        cancels = src == 7 and kind in (2, 3, 5, 7)
        reads = 0 if cancels else BIT["a"] | operand_reads(src)
        if kind in (1, 3):
            reads |= FC
        return reads, (0 if kind == 7 else BIT["a"]) | FLAGS
    if op < 0x40:
        reg, pair = (op >> 3) & 7, (op >> 4) & 3
        if (op & 0xC7) in (0x04, 0x05):
            return operand_reads(reg), reg_bit(reg) | FZ | FN | FH
        if (op & 0xC7) == 0x06:
            return (HL if reg == 6 else 0), reg_bit(reg)
        if (op & 0xCF) == 0x01:
            return 0, PAIR_BITS[pair]
        if (op & 0xCF) in (0x03, 0x0B):
            return PAIR_BITS[pair], PAIR_BITS[pair]
        if (op & 0xCF) == 0x09:
            return HL | PAIR_BITS[pair], HL | FN | FH | FC
        table = {
            0x02: (PAIR_BITS[0] | BIT["a"], 0), 0x12: (PAIR_BITS[1] | BIT["a"], 0),
            0x0A: (PAIR_BITS[0], BIT["a"]), 0x1A: (PAIR_BITS[1], BIT["a"]),
            0x22: (HL | BIT["a"], HL), 0x32: (HL | BIT["a"], HL),
            0x2A: (HL, HL | BIT["a"]), 0x3A: (HL, HL | BIT["a"]),
            0x07: (BIT["a"], BIT["a"] | FLAGS), 0x0F: (BIT["a"], BIT["a"] | FLAGS),
            0x17: (BIT["a"] | FC, BIT["a"] | FLAGS), 0x1F: (BIT["a"] | FC, BIT["a"] | FLAGS),
            0x20: (FZ, 0), 0x28: (FZ, 0), 0x30: (FC, 0), 0x38: (FC, 0),
            0x27: (BIT["a"] | FN | FH | FC, BIT["a"] | FZ | FH | FC),
            0x2F: (BIT["a"], BIT["a"] | FN | FH),
            0x37: (0, FN | FH | FC), 0x3F: (FC, FN | FH | FC),
        }
        return table.get(op, (0, 0))
    condition, pair = (op >> 3) & 3, (op >> 4) & 3
    flag = FC if condition & 2 else FZ
    if (op & 0xE7) in (0xC0, 0xC2, 0xC4):
        return flag, 0
    if (op & 0xCF) == 0xC1:
        return 0, (BIT["a"] | FLAGS) if pair == 3 else PAIR_BITS[pair]
    if (op & 0xCF) == 0xC5:
        return (BIT["a"] | FLAGS) if pair == 3 else PAIR_BITS[pair], 0
    if (op & 0xC7) == 0xC6:
        kind = (op >> 3) & 7
        return BIT["a"] | (FC if kind in (1, 3) else 0), (0 if kind == 7 else BIT["a"]) | FLAGS
    table = {
        0xE0: (BIT["a"], 0), 0xEA: (BIT["a"], 0), 0xF0: (0, BIT["a"]), 0xFA: (0, BIT["a"]),
        0xE2: (BIT["c"] | BIT["a"], 0), 0xF2: (BIT["c"], BIT["a"]),
        0xE8: (0, FLAGS), 0xF8: (0, HL | FLAGS), 0xF9: (HL, 0), 0xE9: (HL, 0),
    }
    return table.get(op, (0, 0))


def summary_bits(function: dict, key: str) -> int:
    bits = 0
    for name in function["registers"][key]:
        bits |= BIT[name]
    for name in function["flags"][key]:
        bits |= FLAG_BY_NAME[name]
    return bits


def read_through(function: dict | None, after: int) -> int:
    """What has to hold before control enters a function, given what is read
    after it returns: what it needs, and what it does not always overwrite.
    `needs` counts code the export could not follow as reading everything,
    so this holds for a function whose summary is incomplete too."""
    if function is None:
        return ALL
    if "needs" not in function["registers"]:
        # An export from before `needs`.
        if function["summary_incomplete"]:
            return ALL
        return summary_bits(function, "reads") | (after & ~summary_bits(function, "always_writes"))
    return summary_bits(function, "needs") | (after & ~summary_bits(function, "always_writes"))


class Instruction:
    __slots__ = ("address", "data", "asm", "cycles", "taken", "target", "following", "kind",
                 "reads", "writes", "successors", "exits", "live_in", "live_after", "callee",
                 "leaves_into", "goes_on_to", "through_table", "seen_going", "comes_back",
                 "comes_back_certain")

    def __init__(self, raw: dict):
        self.address = raw["address"]
        self.data = bytes.fromhex(raw["bytes"])
        self.asm = raw["asm"]
        self.cycles = raw["cycles"]
        self.taken = raw.get("cycles_taken", raw["cycles"])
        self.target = raw.get("target")
        self.following = (self.address + len(self.data)) & 0xFFFF
        self.reads, self.writes = effects(self.data)
        self.kind = control_kind(self.data)
        self.successors: list[int] = []
        self.exits = False
        self.live_in = 0
        self.live_after = ALL
        self.callee = None
        # Where control goes when it leaves the function here other than by a
        # return: one entry for a jump out, one for running off the end of the
        # block, each the function entered or None if that is not known.
        self.leaves_into: list[dict | None] = []
        # For a call or jump to a helper that takes its target in registers:
        # the function it goes on to from here, when the export knows.
        self.goes_on_to: dict | None = None
        # Or the table it picks from: (address, number of entries).
        self.through_table: tuple[int, int] | None = None
        # Where a run of the game went from here: a call to a helper, or a
        # jump the code does not spell out.
        self.seen_going: list[dict] = []
        # For a JP HL over an address the function pushed itself: the blocks
        # of the function what it jumps to returns to, and whether the push
        # is right there in the code (or the place is one it was seen to
        # come back to).
        self.comes_back: list[int] = []
        self.comes_back_certain = False


def control_kind(data: bytes) -> str:
    op = data[0]
    if op == 0x18 or op == 0xC3:
        return "jump"
    if op in (0x20, 0x28, 0x30, 0x38, 0xC2, 0xCA, 0xD2, 0xDA):
        return "jump_cc"
    if op == 0xCD:
        return "call"
    if op in (0xC4, 0xCC, 0xD4, 0xDC):
        return "call_cc"
    if op >= 0xC0 and (op & 0xC7) == 0xC7:
        return "rst"
    if op == 0xC9:
        return "ret"
    if op == 0xD9:
        return "reti"
    if op in (0xC0, 0xC8, 0xD0, 0xD8):
        return "ret_cc"
    if op == 0xE9:
        return "jp_hl"
    if op in (0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD):
        raise Unliftable(f"undefined opcode {op:02X}")
    return "plain"


CONDITIONS = ("!ctx->f_z", "ctx->f_z", "!ctx->f_c", "ctx->f_c")


class Generator:
    def __init__(self, function: dict, index: dict[str, dict], names: dict[int, str],
                 time: str = "block", operands: dict[int, str] | None = None):
        self.function = function
        self.index = index
        self.names = names
        self.time = time
        # Instructions whose 16-bit operand is the address of something in
        # the ROM, with the name the source has for it: the address of the
        # instruction -> the name.
        self.operands = operands or {}
        blocks = sorted(function["blocks"], key=lambda block: block["address"])
        if not blocks:
            raise Unliftable("it has no decoded blocks")
        # Read from the entry on. Code placed before it in the ROM (the part
        # a function jumps back to at its end) follows.
        entry = function["address"]
        blocks = ([block for block in blocks if block["address"] >= entry] +
                  [block for block in blocks if block["address"] < entry])
        self.block_starts = {block["address"] for block in blocks}
        # Jumps and fall-through the export lists as leaving the function. A
        # jump from the home bank to an address in the switchable area goes
        # to whichever bank is mapped; that this function has a block at the
        # same address in one bank does not make it a jump to that block.
        self.jumps_away = {jump["site"] for jump in function.get("jumps_out", ())
                           if jump["kind"] == "jump"}
        self.falls_away = {jump["site"] for jump in function.get("jumps_out", ())
                           if jump["kind"] == "fallthrough"}
        self.block_end = {}
        self.instructions: list[Instruction] = []
        self.first_of_block: dict[int, int] = {}
        self.last_of_block: set[int] = set()
        for block in blocks:
            self.first_of_block[block["address"]] = len(self.instructions)
            for raw in block["instructions"]:
                self.instructions.append(Instruction(raw))
            self.last_of_block.add(len(self.instructions) - 1)
            last = self.instructions[-1]
            if last.kind == "jp_hl":
                last.comes_back = [address for address in block.get("jump_returns_to", ())
                                   if address in self.block_starts]
                last.comes_back_certain = bool(block.get("jump_returns_certain")) and len(last.comes_back) == 1
            self.block_end[len(self.instructions) - 1] = block["end"] & 0xFFFF
        self.link()
        self.resolve_conditions()
        self.liveness()
        self.settle_captures()
        # Where control arrives other than by running on from the statement
        # before: jump targets, and blocks reached by falling out of a block
        # that is not the one before them.
        self.labels: set[int] = set()
        for i, instruction in enumerate(self.instructions):
            if instruction.kind in ("jump", "jump_cc") and self.stays(instruction):
                self.labels.add(instruction.target)
            if i in self.last_of_block:
                end = self.block_end[i]
                if self.falls_inside(i) and self.first_of_block[end] != i + 1:
                    self.labels.add(end)
            for address in instruction.comes_back:
                if not (instruction.comes_back_certain and self.first_of_block[address] == i + 1):
                    self.labels.add(address)

    # ---- control flow and liveness ---------------------------------------

    def stays(self, jump: Instruction) -> bool:
        """Does this jump go to one of the function's own blocks?"""
        return jump.target in self.first_of_block and jump.address not in self.jumps_away

    def falls_inside(self, i: int) -> bool:
        """Does running off the end of the block instruction i ends stay in the function?"""
        return self.block_end[i] in self.first_of_block and \
            self.instructions[i].address not in self.falls_away

    def fallthrough(self, i: int) -> int | None:
        """Index of the instruction after i, or None if control leaves."""
        if i in self.last_of_block:
            return self.first_of_block[self.block_end[i]] if self.falls_inside(i) else None
        return i + 1

    def link(self) -> None:
        leaving = {}
        through = {}
        for jump in self.function.get("jumps_out", ()):
            if jump["function"] and jump["kind"] in ("jump", "fallthrough"):
                leaving[(jump["site"], jump["kind"] == "jump")] = self.index.get(jump["function"])
        tables = {}
        seen = {}
        for site in list(self.function["calls"]) + list(self.function.get("jumps_out", ())):
            if "reaches" in site:
                through[site["site"]] = self.index.get(site["reaches"]["function"])
            if "reaches_table" in site:
                tables[site["site"]] = (site["reaches_table"]["address"], len(site["reaches_table"]["entries"]))
            if "reaches_seen" in site:
                seen[site["site"]] = site["reaches_seen"]
        for jump in self.function.get("seen_jumps", ()):
            seen[jump["site"]] = jump["targets"]
        for i, instruction in enumerate(self.instructions):
            kind = instruction.kind
            if kind in ("call", "call_cc", "rst", "jump", "jump_cc"):
                instruction.goes_on_to = through.get(instruction.address)
                instruction.through_table = tables.get(instruction.address)
            instruction.seen_going = [function for function in
                                      (self.index.get(entry["function"]) for entry in seen.get(instruction.address, ()))
                                      if function]
            if kind in ("jump", "jump_cc") and not self.stays(instruction):
                instruction.leaves_into.append(leaving.get((instruction.address, True)))
            if kind not in ("jump", "ret", "reti", "jp_hl") and i in self.last_of_block and \
                    not self.falls_inside(i):
                instruction.leaves_into.append(leaving.get((instruction.address, False)))
            def add(target_index):
                if target_index is None:
                    instruction.exits = True
                else:
                    instruction.successors.append(target_index)
            if kind in ("ret", "reti", "jp_hl"):
                instruction.exits = True
                # Where a JP HL comes back to, once other code has run.
                for address in instruction.comes_back:
                    instruction.successors.append(self.first_of_block[address])
            elif kind == "jump":
                add(self.first_of_block[instruction.target] if self.stays(instruction) else None)
            elif kind == "jump_cc":
                add(self.first_of_block[instruction.target] if self.stays(instruction) else None)
                add(self.fallthrough(i))
            elif kind == "ret_cc":
                instruction.exits = True
                add(self.fallthrough(i))
            else:
                add(self.fallthrough(i))
            if kind in ("call", "call_cc", "rst"):
                bank = None
                target = instruction.target if kind != "rst" else instruction.data[0] & 0x38
                for call in self.function["calls"]:
                    if call["site"] == instruction.address:
                        bank = call["function"]
                        break
                instruction.callee = self.index.get(bank) if bank else None
                instruction.target = target

    @staticmethod
    def read_on_leaving(instruction: Instruction) -> int:
        """What may be read once control has left the function here. After a
        return, anything: who called is not known. Into another function,
        what that one reads, and what it does not always overwrite."""
        if instruction.kind in ("ret", "reti", "ret_cc", "jp_hl") or not instruction.leaves_into:
            return ALL
        read = 0
        for into in instruction.leaves_into:
            read |= read_through(into, ALL)
        return read

    def liveness(self) -> None:
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
                    live = read_through(instruction.callee, after)
                    if kind == "call_cc":
                        live |= after
                    if i not in self.conditions:
                        live |= instruction.reads
                elif i in self.conditions:
                    live = after
                else:
                    live = (after & ~instruction.writes) | instruction.reads
                if after != instruction.live_after or live != instruction.live_in:
                    instruction.live_after = after
                    instruction.live_in = live
                    changed = True

    # ---- expressions -------------------------------------------------------

    def address(self, value: int) -> str:
        name = self.names.get(value)
        return name if name else f"0x{value:04X}"

    def read8(self, index: int) -> str:
        return "lift_read8(ctx, ctx->hl)" if index == 6 else f"ctx->{REG_NAMES[index]}"

    def write8(self, index: int, value: str) -> str:
        if index == 6:
            return f"lift_write8(ctx, ctx->hl, {value});"
        return f"ctx->{REG_NAMES[index]} = {value};"

    def plain(self, instruction: Instruction) -> list[str]:
        """Statements for an instruction that does not transfer control."""
        data = instruction.data
        op = data[0]
        imm8 = data[1] if len(data) > 1 else 0
        imm16 = data[1] | (data[2] << 8) if len(data) > 2 else 0
        named = self.operands.get(instruction.address)  # what the operand leads to, by name
        needed = instruction.live_after & instruction.writes & FLAGS

        if op == 0xCB:
            cb = data[1]
            reg, bit, group = cb & 7, (cb >> 3) & 7, cb >> 6
            value = self.read8(reg)
            if group == 0:
                names = ("rlc", "rrc", "rl", "rr", "sla", "sra", "swap", "srl")
                if needed:
                    return [self.write8(reg, f"lift_{names[bit]}(ctx, {value})")]
                plain = {
                    0: f"(uint8_t)(({value} << 1) | ({value} >> 7))",
                    1: f"(uint8_t)(({value} >> 1) | ({value} << 7))",
                    2: f"(uint8_t)(({value} << 1) | ctx->f_c)",
                    3: f"(uint8_t)(({value} >> 1) | (ctx->f_c << 7))",
                    4: f"(uint8_t)({value} << 1)",
                    5: f"(uint8_t)(({value} >> 1) | ({value} & 0x80))",
                    6: f"(uint8_t)(({value} << 4) | ({value} >> 4))",
                    7: f"(uint8_t)({value} >> 1)",
                }[bit]
                if reg == 6:
                    # One read of the memory operand, as the instruction makes.
                    return [f"{{ const uint8_t value = {value}; "
                            + self.write8(6, plain.replace(value, "value")) + " }"]
                return [self.write8(reg, plain)]
            if group == 1:
                if needed:
                    return [f"lift_bit(ctx, {bit}, {value});"]
                return [f"(void){value};"] if reg == 6 else []
            if group == 2:
                return [self.write8(reg, f"(uint8_t)({value} & 0x{0xFF ^ (1 << bit):02X})")]
            return [self.write8(reg, f"(uint8_t)({value} | 0x{1 << bit:02X})")]

        if 0x40 <= op <= 0x7F:
            if op == 0x76:
                return ["lift_halt(ctx);"]
            dst, src = (op >> 3) & 7, op & 7
            if dst == src:
                return []
            return [self.write8(dst, self.read8(src))]

        if 0x80 <= op <= 0xBF or (op >= 0xC0 and (op & 0xC7) == 0xC6):
            kind = (op >> 3) & 7
            if op >= 0xC0:
                operand, is_a = f"0x{imm8:02X}", False
            else:
                operand, is_a = self.read8(op & 7), (op & 7) == 7
            helper = ("add8", "adc8", "sub8", "sbc8", "and8", "xor8", "or8", "cp8")[kind]
            if needed:
                return [f"lift_{helper}(ctx, {operand});"]
            if kind == 0:
                return [f"ctx->a = (uint8_t)(ctx->a + {operand});"]
            if kind == 1:
                return [f"ctx->a = (uint8_t)(ctx->a + {operand} + ctx->f_c);"]
            if kind == 2:
                return ["ctx->a = 0;"] if is_a else [f"ctx->a = (uint8_t)(ctx->a - {operand});"]
            if kind == 3:
                return [f"ctx->a = (uint8_t)(ctx->a - {operand} - ctx->f_c);"]
            if kind == 4:
                return [] if is_a else [f"ctx->a &= {operand};"]
            if kind == 5:
                return ["ctx->a = 0;"] if is_a else [f"ctx->a ^= {operand};"]
            if kind == 6:
                return [] if is_a else [f"ctx->a |= {operand};"]
            return [f"(void){operand};"] if "lift_read8" in operand else []

        if op < 0x40:
            reg, pair = (op >> 3) & 7, (op >> 4) & 3
            low = op & 0x0F
            if low == 0x1:
                return [f"ctx->{PAIR_NAMES[pair]} = {named or self.pointer(imm16)};"]
            if low == 0x3:
                return [f"ctx->{PAIR_NAMES[pair]}++;"]
            if low == 0xB:
                return [f"ctx->{PAIR_NAMES[pair]}--;"]
            if low == 0x9:
                if needed:
                    return [f"lift_add_hl(ctx, ctx->{PAIR_NAMES[pair]});"]
                return [f"ctx->hl = (uint16_t)(ctx->hl + ctx->{PAIR_NAMES[pair]});"]
            if (op & 7) in (4, 5):
                helper, sign = ("lift_inc8", "+") if (op & 7) == 4 else ("lift_dec8", "-")
                if needed:
                    return [self.write8(reg, f"{helper}(ctx, {self.read8(reg)})")]
                if reg == 6:
                    return [self.write8(6, f"(uint8_t)({self.read8(6)} {sign} 1)")]
                return [f"ctx->{REG_NAMES[reg]}{sign}{sign};"]
            if (op & 7) == 6:
                return [self.write8(reg, f"0x{imm8:02X}")]
            flagless = {0x07: "(uint8_t)((ctx->a << 1) | (ctx->a >> 7))",
                        0x0F: "(uint8_t)((ctx->a >> 1) | (ctx->a << 7))",
                        0x17: "(uint8_t)((ctx->a << 1) | ctx->f_c)",
                        0x1F: "(uint8_t)((ctx->a >> 1) | (ctx->f_c << 7))"}
            if op in flagless:
                helper = {0x07: "rlca", 0x0F: "rrca", 0x17: "rla", 0x1F: "rra"}[op]
                return [f"lift_{helper}(ctx);"] if needed else [f"ctx->a = {flagless[op]};"]
            if op == 0x2F:
                lines = ["ctx->a = (uint8_t)~ctx->a;"]
                if needed & FN:
                    lines.append("ctx->f_n = 1;")
                if needed & FH:
                    lines.append("ctx->f_h = 1;")
                return lines
            if op == 0x37:
                return ([f"ctx->f_n = 0;"] if needed & FN else []) + \
                       ([f"ctx->f_h = 0;"] if needed & FH else []) + \
                       ([f"ctx->f_c = 1;"] if needed & FC else [])
            if op == 0x3F:
                carry = ["ctx->f_c = (uint8_t)!ctx->f_c;"] if needed & FC else []
                return ([f"ctx->f_n = 0;"] if needed & FN else []) + \
                       ([f"ctx->f_h = 0;"] if needed & FH else []) + carry
            simple = {
                0x00: [],
                0x02: ["lift_write8(ctx, ctx->bc, ctx->a);"],
                0x12: ["lift_write8(ctx, ctx->de, ctx->a);"],
                0x0A: ["ctx->a = lift_read8(ctx, ctx->bc);"],
                0x1A: ["ctx->a = lift_read8(ctx, ctx->de);"],
                0x22: ["lift_write8(ctx, ctx->hl++, ctx->a);"],
                0x32: ["lift_write8(ctx, ctx->hl--, ctx->a);"],
                0x2A: ["ctx->a = lift_read8(ctx, ctx->hl++);"],
                0x3A: ["ctx->a = lift_read8(ctx, ctx->hl--);"],
                0x08: [f"lift_write16(ctx, {self.address(imm16)}, ctx->sp);"],
                0x10: ["lift_stop(ctx);"],
                0x27: ["lift_daa(ctx);"],
            }
            if op in simple:
                return simple[op]
            raise Unliftable(f"unexpected opcode {op:02X}")

        pair = (op >> 4) & 3
        if op in (0xC1, 0xD1, 0xE1):
            return [f"ctx->{PAIR_NAMES[pair]} = lift_pop16(ctx);"]
        if op == 0xF1:
            return ["lift_pop_af(ctx);"]
        if op in (0xC5, 0xD5, 0xE5):
            return [f"lift_push16(ctx, ctx->{PAIR_NAMES[pair]});"]
        if op == 0xF5:
            return ["lift_push_af(ctx);"]
        table = {
            0xE0: [f"lift_write8(ctx, {self.address(0xFF00 + imm8)}, ctx->a);"],
            0xF0: [f"ctx->a = lift_read8(ctx, {self.address(0xFF00 + imm8)});"],
            0xE2: ["lift_write8(ctx, (uint16_t)(0xFF00 + ctx->c), ctx->a);"],
            0xF2: ["ctx->a = lift_read8(ctx, (uint16_t)(0xFF00 + ctx->c));"],
            0xEA: [f"lift_write8(ctx, {named or self.address(imm16)}, ctx->a);"],
            0xFA: [f"ctx->a = lift_read8(ctx, {named or self.address(imm16)});"],
            0xE8: [f"ctx->sp = lift_sp_plus(ctx, 0x{imm8:02X});"],
            0xF8: [f"ctx->hl = lift_sp_plus(ctx, 0x{imm8:02X});"],
            0xF9: ["ctx->sp = ctx->hl;"],
            0xF3: ["lift_di(ctx);"],
            0xFB: ["lift_ei(ctx);"],
        }
        if op in table:
            return table[op]
        raise Unliftable(f"unexpected opcode {op:02X}")

    def pointer(self, value: int) -> str:
        """A 16-bit constant loaded into a register pair: an address, if it has a name."""
        if value >= 0x8000 and value in self.names:
            return self.names[value]
        return f"0x{value:04X}"

    # ---- conditions from the instruction that set the flag -----------------

    @staticmethod
    def tested_flag(instruction: Instruction) -> int:
        if instruction.kind not in ("jump_cc", "ret_cc", "call_cc"):
            return 0
        return FC if (instruction.data[0] >> 3) & 2 else FZ

    def flag_test(self, setter: Instruction, flag: int) -> tuple[str, int] | None:
        """C for "this instruction left the flag set", in terms of the registers
        as it leaves them, and which registers that reads. None when the flag
        is not a plain function of those."""
        data = setter.data
        op = data[0]
        imm8 = data[1] if len(data) > 1 else 0
        if op == 0xCB:
            reg, bit, group = data[1] & 7, (data[1] >> 3) & 7, data[1] >> 6
            if reg == 6:
                return None
            name = REG_NAMES[reg]
            if flag == FC:
                if group == 0 and bit in (0, 1):  # RLC, RRC
                    return f"ctx->{name} & 0x{0x01 if bit == 0 else 0x80:02X}", BIT[name]
                return None
            if group == 0:
                return f"ctx->{name} == 0", BIT[name]
            if group == 1:
                return f"!(ctx->{name} & 0x{1 << bit:02X})", BIT[name]
            return None
        if op in (0x07, 0x0F) and flag == FC:
            # RLCA and RRCA: the bit that went to carry also went round.
            return f"ctx->a & 0x{0x01 if op == 0x07 else 0x80:02X}", BIT["a"]
        if 0x80 <= op <= 0xBF or (op >= 0xC0 and (op & 0xC7) == 0xC6):
            kind = (op >> 3) & 7
            immediate = op >= 0xC0
            src = None if immediate else op & 7
            if immediate:
                operand, bits = f"0x{imm8:02X}", BIT["a"]
            elif src == 6:
                operand, bits = None, BIT["a"]
            else:
                operand, bits = f"ctx->{REG_NAMES[src]}", BIT["a"] | BIT[REG_NAMES[src]]
            if kind == 7:
                # CP leaves A alone: the flags compare it with the operand.
                if operand is None or src == 7:
                    return None
                return (f"ctx->a == {operand}" if flag == FZ else f"ctx->a < {operand}"), bits
            if flag == FZ:
                return "ctx->a == 0", BIT["a"]
            if kind == 0 and operand is not None and src != 7:
                # ADD carried exactly when the sum came out below what was added.
                return f"ctx->a < {operand}", bits
            return None
        if op < 0x40 and (op & 7) in (4, 5):
            reg = (op >> 3) & 7
            if reg == 6 or flag != FZ:
                return None
            return f"ctx->{REG_NAMES[reg]} == 0", BIT[REG_NAMES[reg]]
        if op in (0x09, 0x19) and flag == FC:
            pair = (op >> 4) & 3
            return f"ctx->hl < ctx->{PAIR_NAMES[pair]}", HL | PAIR_BITS[pair]
        return None

    def resolve_conditions(self) -> None:
        """For each conditional jump, return and call: its test written out
        from the instruction that set the flag.

        That takes exactly one instruction whose write of the flag can reach
        the test, one this knows how to read (a compare, arithmetic whose
        result is still in its register), and nothing on any way from it to
        the test that changes the registers the comparison reads. A flag a
        call returned, one the function was entered with, and one that two
        different instructions may have set stay a test of the flag itself.
        """
        self.conditions: dict[int, str] = {}
        # Setter -> {flag: the instructions that test it through a local}.
        self.captures: dict[int, dict[int, list[int]]] = {}
        self.capture_names: dict[tuple[int, int], str] = {}
        count = len(self.instructions)
        predecessors: list[list[int]] = [[] for _ in range(count)]
        for i, instruction in enumerate(self.instructions):
            for successor in instruction.successors:
                predecessors[successor].append(i)
        entry = -1
        for flag in (FZ, FC):
            tests = [i for i, instruction in enumerate(self.instructions)
                     if self.tested_flag(instruction) == flag]
            if not tests:
                continue
            # Which instructions' writes of the flag can still stand when
            # each instruction starts.
            reach: list[set[int]] = [set() for _ in range(count)]
            reach[0].add(entry)
            changed = True
            while changed:
                changed = False
                for i in range(count):
                    incoming = reach[i]
                    before = len(incoming)
                    for p in predecessors[i]:
                        writer = self.instructions[p]
                        if writer.kind in ("call", "rst", "jp_hl"):
                            incoming.add(p)
                        elif writer.kind == "call_cc":
                            incoming.add(p)
                            incoming |= reach[p]
                        elif writer.writes & flag:
                            incoming.add(p)
                        else:
                            incoming |= reach[p]
                    if len(incoming) != before:
                        changed = True
            for i in tests:
                if len(reach[i]) != 1:
                    continue
                d = next(iter(reach[i]))
                if d == entry or self.instructions[d].kind != "plain":
                    continue
                found = self.flag_test(self.instructions[d], flag)
                if found is not None:
                    expression, operands = found
                    # Everything that can run between the two.
                    between: set[int] = set()
                    stack = list(predecessors[i])
                    while stack:
                        n = stack.pop()
                        if n == d or n in between:
                            continue
                        between.add(n)
                        stack.extend(predecessors[n])
                    if any(self.instructions[n].writes & operands for n in between):
                        found = None
                if found is None:
                    if self.capture_form(self.instructions[d], flag) is None:
                        continue
                    self.captures.setdefault(d, {}).setdefault(flag, []).append(i)
                    continue
                taken_when_set = (self.instructions[i].data[0] >> 3) & 1
                self.conditions[i] = expression if taken_when_set else negate(expression)
        self.name_captures()

    def name_captures(self) -> None:
        self.capture_names = {}
        numbers = {FZ: 0, FC: 0}
        for d in sorted(self.captures):
            for flag in (FZ, FC):
                tests = self.captures[d].get(flag)
                if not tests:
                    continue
                numbers[flag] += 1
                base = "zero" if flag == FZ else "carry"
                name = base if numbers[flag] == 1 else f"{base}{numbers[flag]}"
                self.capture_names[(d, flag)] = name
                for i in tests:
                    taken_when_set = (self.instructions[i].data[0] >> 3) & 1
                    self.conditions[i] = name if taken_when_set else f"!{name}"

    def settle_captures(self) -> None:
        """A local earns its place by letting the instruction drop its flags.
        Where something else still needs them (a caller may, after a return),
        the flag itself is as good a thing to test."""
        while True:
            dropped = False
            for d in list(self.captures):
                setter = self.instructions[d]
                if setter.live_after & setter.writes & FLAGS:
                    for tests in self.captures.pop(d).values():
                        for i in tests:
                            del self.conditions[i]
                    dropped = True
            if not dropped:
                return
            self.name_captures()
            for instruction in self.instructions:
                instruction.live_in, instruction.live_after = 0, ALL
            self.liveness()

    def capture_form(self, setter: Instruction, flag: int) -> tuple[str, str] | None:
        """How to keep "this instruction left the flag set" in a local:
        ("post", expression) over the registers as it leaves them, ("pre", ...)
        as it finds them, or ("value", ...) over `value`, the byte it reads
        from memory (for INC and DEC of [hl], the byte it writes)."""
        found = self.flag_test(setter, flag)
        if found is not None:
            return "post", found[0]
        data = setter.data
        op = data[0]
        imm8 = data[1] if len(data) > 1 else 0
        if op == 0xCB:
            reg, bit, group = data[1] & 7, (data[1] >> 3) & 7, data[1] >> 6
            if reg == 6:
                if group == 1 and flag == FZ:
                    return "value", f"!(value & 0x{1 << bit:02X})"
                return None
            if group == 0 and flag == FC and bit in (2, 3, 4, 5, 7):
                # RL and SLA push bit 7 out; RR, SRA and SRL bit 0.
                return "pre", f"(ctx->{REG_NAMES[reg]} & 0x{0x80 if bit in (2, 4) else 0x01:02X}) != 0"
            return None
        if 0x80 <= op <= 0xBF or (op >= 0xC0 and (op & 0xC7) == 0xC6):
            kind = (op >> 3) & 7
            immediate = op >= 0xC0
            memory = not immediate and (op & 7) == 6
            if immediate:
                operand = f"0x{imm8:02X}"
            else:
                operand = "value" if memory else f"ctx->{REG_NAMES[op & 7]}"
            when = "value" if memory else "pre"
            if kind == 7:
                if not memory:
                    return None
                return when, (f"ctx->a == {operand}" if flag == FZ else f"ctx->a < {operand}")
            if flag != FC:
                return None
            if kind == 0:
                return when, f"ctx->a + {operand} > 0xFF"
            if kind == 1:
                return when, f"ctx->a + {operand} + ctx->f_c > 0xFF"
            if kind == 2:
                return when, f"ctx->a < {operand}"
            if kind == 3:
                return when, f"ctx->a < {operand} + ctx->f_c"
            return None
        if op in (0x34, 0x35) and flag == FZ:
            return "value", "value == 0"
        if op in (0x17, 0x1F) and flag == FC:
            return "pre", f"(ctx->a & 0x{0x80 if op == 0x17 else 0x01:02X}) != 0"
        return None

    def capture(self, i: int, statements: list[str]) -> list[str]:
        """The statements of instruction i, with the locals its tests read."""
        wanted = self.captures.get(i)
        if not wanted:
            return statements
        setter = self.instructions[i]
        pre: list[str] = []
        post: list[str] = []
        value: list[str] = []
        for flag in (FZ, FC):
            if flag not in wanted:
                continue
            when, expression = self.capture_form(setter, flag)
            line = f"{self.capture_names[(i, flag)]} = {expression};"
            {"pre": pre, "post": post, "value": value}[when].append(line)
        if value:
            read = "lift_read8(ctx, ctx->hl)"
            op = setter.data[0]
            if op in (0x34, 0x35):
                sign = "+" if op == 0x34 else "-"
                statements = [f"{{ const uint8_t value = (uint8_t)({read} {sign} 1); "
                              f"lift_write8(ctx, ctx->hl, value); {' '.join(value)} }}"]
            else:
                # One read of the memory operand, as the instruction makes.
                body = [line.replace(read, "value") for line in statements if line != f"(void){read};"]
                if not body and len(value) == 1:
                    statements = [value[0].replace("value", read)]
                else:
                    statements = [f"{{ const uint8_t value = {read}; {' '.join(value + body)} }}"]
        return pre + statements + post

    def onward(self, instruction: Instruction) -> str:
        """`: on to F`, for a call to a helper the export has followed."""
        if instruction.through_table:
            address, count = instruction.through_table
            return f": on to one of the {count} in the table at {self.address(address)}"
        if instruction.goes_on_to:
            return f": on to {display(instruction.goes_on_to)}"
        return f": {self.seen_note(instruction)}" if instruction.seen_going else ""

    @staticmethod
    def seen_note(instruction: Instruction) -> str:
        """Where a run of the game went from here. Not the whole answer."""
        names = [display(function) for function in instruction.seen_going[:3]]
        more = len(instruction.seen_going) - len(names)
        return "seen going on to " + ", ".join(names) + (f" and {more} more" if more else "")

    def helper_note(self, instruction: Instruction) -> str:
        """A comment naming the helper a jump goes to and where that takes it."""
        if not (instruction.goes_on_to or instruction.through_table or instruction.seen_going) or \
                not instruction.leaves_into or instruction.leaves_into[0] is None:
            return ""
        return f" /* {display(instruction.leaves_into[0])}{self.onward(instruction)} */"

    # ---- nodes ---------------------------------------------------------------

    def build_nodes(self) -> list[dict]:
        nodes: list[dict] = []
        pending = 0  # block-mode time not yet passed

        def flush(next_pc: int) -> list[str]:
            nonlocal pending
            if self.time != "block" or pending == 0:
                return []
            lines = [f"lift_step(ctx, 0x{next_pc:04X}, {pending});"]
            pending = 0
            return lines

        def passing(cycles: int) -> str:
            """Time handed on with the machine (before a call, a return)."""
            nonlocal pending
            total = cycles + (pending if self.time == "block" else 0)
            pending = 0
            return f"lift_advance(ctx, {total});"

        def branch(condition: str, target: int, instruction: Instruction) -> str:
            nonlocal pending
            extra = pending if self.time == "block" else 0
            return (f"lift_branch(ctx, {condition}, 0x{target:04X}, {instruction.taken + extra}, "
                    f"0x{instruction.following:04X}, {instruction.cycles + extra})")

        for i, instruction in enumerate(self.instructions):
            comment = f"/* {instruction.address:04X}: {instruction.asm} */"
            if instruction.address in self.labels:
                nodes.append({"kind": "label", "address": instruction.address,
                              "flush": flush(instruction.address)})
            kind = instruction.kind
            op = instruction.data[0]
            following = instruction.following
            lines: list[str] = []
            node: dict | None = None
            after: list[dict] = []  # nodes that follow this instruction's own

            if kind == "plain":
                statements = self.capture(i, self.plain(instruction))
                if op in (0x76, 0x10):
                    lines = flush(instruction.address) + [f"ctx->pc = 0x{following:04X};"] + statements
                elif self.time == "exact":
                    if op in LATE_ACCESS and instruction.cycles > 1:
                        lines = [f"lift_advance(ctx, {instruction.cycles - 1});"] + statements + \
                                [f"lift_step(ctx, 0x{following:04X}, 1);"]
                    else:
                        lines = statements + [f"lift_step(ctx, 0x{following:04X}, {instruction.cycles});"]
                    # One instruction, one line, while it stays readable.
                    if sum(len(line) + 1 for line in lines) <= 100:
                        lines = [" ".join(lines)]
                else:
                    lines = statements
                    pending += instruction.cycles
                node = {"kind": "stmt", "lines": lines}
            elif kind in ("call", "call_cc", "rst"):
                callee = instruction.callee
                note = f" /* {display(callee)}{self.onward(instruction)} */" if callee else ""
                if kind == "call_cc":
                    condition = self.conditions.get(i, CONDITIONS[(op >> 3) & 3])
                    saved = pending
                    taken = passing(instruction.taken)
                    pending = saved
                    untaken = (f"lift_step(ctx, 0x{following:04X}, "
                               f"{instruction.cycles + (pending if self.time == 'block' else 0)});")
                    pending = 0
                    lines = [f"if ({condition}) {{ {taken} "
                             f"if (lift_call(ctx, 0x{instruction.target:04X}{note}, 0x{following:04X})) return; }} "
                             f"else {{ {untaken} }}"]
                else:
                    lines = [f"{passing(instruction.cycles)} "
                             f"if (lift_call(ctx, 0x{instruction.target:04X}{note}, 0x{following:04X})) return;"]
                node = {"kind": "stmt", "lines": lines}
            elif kind in ("ret", "reti"):
                call = "lift_reti(ctx);" if kind == "reti" else "lift_ret(ctx);"
                node = {"kind": "stmt", "lines": [f"{passing(instruction.cycles)} {call} return;"],
                        "ends": True}
            elif kind == "ret_cc":
                condition = self.conditions.get(i, CONDITIONS[(op >> 3) & 3])
                saved = pending
                taken = passing(instruction.taken)
                pending = saved
                lines = [f"if ({condition}) {{ {taken} lift_ret(ctx); return; }}"]
                if self.time == "exact":
                    lines.append(f"lift_step(ctx, 0x{following:04X}, {instruction.cycles});")
                else:
                    pending += instruction.cycles
                node = {"kind": "stmt", "lines": lines}
            elif kind == "jp_hl":
                seen = f" /* {self.seen_note(instruction)} */" if instruction.seen_going else ""
                back = instruction.comes_back
                came_back = "if (lift_call_pushed(ctx, ctx->hl{0}, 0x{1:04X})) return;"
                if instruction.comes_back_certain:
                    # The address pushed just above: a call, and on from
                    # where it comes back.
                    node = {"kind": "stmt", "ends": True,
                            "lines": [f"{passing(instruction.cycles)} {came_back.format(seen, back[0])}"]}
                    if self.first_of_block[back[0]] != i + 1:
                        after.append({"kind": "goto", "target": back[0], "step": "", "comment": "",
                                      "address": instruction.address})
                elif back:
                    # A call when the address on the stack is one it has
                    # been seen to come back to; a jump away otherwise.
                    lines = [passing(instruction.cycles), "switch (lift_read16(ctx, ctx->sp)) {"]
                    lines += [f"case 0x{address:04X}: {came_back.format('', address)} goto L_{address:04X};"
                              for address in back]
                    lines += [f"default: lift_jump(ctx, ctx->hl{seen}); return;", "}"]
                    node = {"kind": "stmt", "ends": True, "lines": lines, "targets": list(back)}
                else:
                    node = {"kind": "stmt",
                            "lines": [f"{passing(instruction.cycles)} lift_jump(ctx, ctx->hl{seen}); return;"],
                            "ends": True}
            elif kind == "jump":
                if self.stays(instruction):
                    extra = pending if self.time == "block" else 0
                    pending = 0
                    node = {"kind": "goto", "target": instruction.target,
                            "step": f"lift_step(ctx, 0x{instruction.target:04X}, {instruction.cycles + extra});"}
                else:
                    node = {"kind": "stmt", "ends": True,
                            "lines": [f"{passing(instruction.cycles)} "
                                      f"lift_jump(ctx, 0x{instruction.target:04X}{self.helper_note(instruction)}); "
                                      f"return;"]}
            elif kind == "jump_cc":
                condition = self.conditions.get(i, CONDITIONS[(op >> 3) & 3])
                if self.stays(instruction):
                    node = {"kind": "if_goto", "target": instruction.target, "condition": condition,
                            "branch": branch(condition, instruction.target, instruction)}
                    pending = 0
                else:
                    saved = pending
                    taken = passing(instruction.taken)
                    pending = saved
                    lines = [f"if ({condition}) {{ {taken} "
                             f"lift_jump(ctx, 0x{instruction.target:04X}{self.helper_note(instruction)}); return; }}"]
                    if self.time == "exact":
                        lines.append(f"lift_step(ctx, 0x{following:04X}, {instruction.cycles});")
                    else:
                        pending += instruction.cycles
                    node = {"kind": "stmt", "lines": lines}
            node["comment"] = comment
            node["address"] = instruction.address
            nodes.append(node)
            nodes.extend(after)

            # Control runs off the end of the block into code that is not ours.
            if i in self.last_of_block and not node.get("ends") and node["kind"] != "goto":
                end = self.block_end[i]
                if not self.falls_inside(i):
                    time = [passing(0)] if (self.time == "block" and pending) else []
                    nodes.append({"kind": "stmt", "ends": True, "comment": "", "address": end,
                                  "lines": time + [f"lift_jump(ctx, 0x{end:04X}); return;"]})
                elif self.first_of_block[end] != i + 1:
                    extra = pending if self.time == "block" else 0
                    pending = 0
                    step = f"lift_step(ctx, 0x{end:04X}, {extra});" if extra else ""
                    nodes.append({"kind": "goto", "target": end, "step": step, "comment": "",
                                  "address": end})
        return nodes


def negate(condition: str) -> str:
    for a, b in ((" == ", " != "), (" < ", " >= ")):
        if a in condition:
            return condition.replace(a, b)
    if condition.startswith("!(") and condition.endswith(")"):
        return condition[1:]
    if condition.startswith("!"):
        return condition[1:]
    return f"!({condition})" if " " in condition else f"!{condition}"


def display(function: dict) -> str:
    return function.get("symbol") or function["name"]


# --------------------------------------------------------------------------
# Structuring: nested if / else / do-while / for(;;) over the node list
# --------------------------------------------------------------------------

def structure(nodes: list[dict]) -> list:
    """Turn jumps into nested constructs where the layout allows it.

    Returns a tree: a list of nodes and ("if", condition_node, body),
    ("do", body, condition_node) and ("loop", body, goto_node) tuples. Jumps
    that fit no construct stay as goto nodes.
    """
    label_at = {node["address"]: i for i, node in enumerate(nodes) if node["kind"] == "label"}
    jumps = [i for i, node in enumerate(nodes) if node["kind"] in ("goto", "if_goto")]
    references: dict[int, list[int]] = {}
    for i in jumps:
        references.setdefault(label_at[nodes[i]["target"]], []).append(i)
    # Jumps a statement makes by itself: no construct is built on them, and
    # none may be built across them.
    for i, node in enumerate(nodes):
        for target in node.get("targets", ()):
            references.setdefault(label_at[target], []).append(i)

    def enters_from_outside(low: int, high: int, allowed: set[int]) -> bool:
        """Is a label strictly inside (low, high] a target of a jump outside [low, high]?"""
        for label, sources in references.items():
            if low < label <= high:
                for source in sources:
                    if source in allowed:
                        continue
                    if not low <= source <= high:
                        return True
        return False

    candidates = []
    for i in jumps:
        node = nodes[i]
        j = label_at[node["target"]]
        if node["kind"] == "if_goto" and j > i:
            # if (!cond) { i+1 .. j-1 }
            candidates.append((j - i, "if", i, j))
        elif j <= i:
            candidates.append((i - j + 1, "do" if node["kind"] == "if_goto" else "loop", j, i))
    candidates.sort()

    accepted: list[tuple[str, int, int]] = []

    def conflicts(low: int, high: int) -> bool:
        for _, a, b in accepted:
            if (low < a <= high < b) or (a < low <= b < high):
                return True
            if (a, b) == (low, high):
                return True
        return False

    for _, kind, low, high in candidates:
        if kind == "if":
            inside_low, inside_high = low + 1, high - 1
            if inside_high < inside_low:
                continue
            if conflicts(low, high - 1) or enters_from_outside(low, inside_high, {low}):
                continue
            accepted.append(("if", low, high - 1))
        else:
            if conflicts(low, high) or enters_from_outside(low, high, set()):
                continue
            accepted.append((kind, low, high))

    by_start: dict[int, list[tuple[str, int, int]]] = {}
    for item in accepted:
        by_start.setdefault(item[1], []).append(item)
    for items in by_start.values():
        items.sort(key=lambda item: -(item[2] - item[1]))

    def build(low: int, high: int, skip: tuple | None = None) -> list:
        out = []
        i = low
        while i <= high:
            constructs = [c for c in by_start.get(i, []) if c != skip and c[2] <= high]
            if constructs:
                construct = constructs[0]
                kind, a, b = construct
                if kind == "if":
                    out.append(("if", nodes[a], build(a + 1, b, None)))
                elif kind == "do":
                    out.append(("do", build(a, b - 1, construct), nodes[b]))
                else:
                    out.append(("loop", build(a, b - 1, construct), nodes[b]))
                i = b + 1
                continue
            out.append(nodes[i])
            i += 1
        return out

    return build(0, len(nodes) - 1)


def emit(tree: list, used_labels: set[int]) -> list[str]:
    """C lines for a structured tree. First pass collects goto targets."""
    lines: list[str] = []

    def comment_of(node: dict) -> str:
        return f"  {node['comment']}" if node.get("comment") else ""

    def walk(items: list, depth: int) -> None:
        pad = "    " * depth
        for position, item in enumerate(items):
            if isinstance(item, tuple):
                kind = item[0]
                if kind in ("do", "loop"):
                    # Time owed on the way into the loop is passed once,
                    # before it, not at the top of every turn. Loops that
                    # share a head are nested; the label is in the innermost.
                    head = item[1][0] if item[1] else None
                    while isinstance(head, tuple) and head[0] in ("do", "loop") and head[1]:
                        head = head[1][0]
                    if isinstance(head, dict) and head["kind"] == "label" and head["flush"]:
                        for line in head["flush"]:
                            lines.append(f"{pad}{line}")
                        head["flush"] = []
                if kind == "if":
                    _, node, body = item
                    following = items[position + 1] if position + 1 < len(items) else None
                    # The label may be the head of a loop that starts there.
                    while isinstance(following, tuple) and following[0] in ("do", "loop") and following[1]:
                        following = following[1][0]
                    if isinstance(following, dict) and following["kind"] == "label" and following["flush"]:
                        # The time owed at the label after the if was run up
                        # inside it; the path that jumps there has paid.
                        innermost = body
                        while innermost and isinstance(innermost[-1], tuple) and innermost[-1][0] == "if":
                            innermost = innermost[-1][2]
                        innermost.append({"kind": "stmt", "lines": following["flush"], "comment": ""})
                        following["flush"] = []
                    lines.append(f"{pad}if (!{node['branch']}) {{{comment_of(node)}")
                    walk(body, depth + 1)
                    lines.append(f"{pad}}}")
                elif kind == "do":
                    _, body, node = item
                    lines.append(f"{pad}do {{")
                    walk(body, depth + 1)
                    lines.append(f"{pad}}} while ({node['branch']});{comment_of(node)}")
                else:
                    _, body, node = item
                    lines.append(f"{pad}for (;;) {{")
                    walk(body, depth + 1)
                    if node.get("step"):
                        lines.append(f"{pad}    {node['step']}{comment_of(node)}")
                    lines.append(f"{pad}}}")
                continue
            node = item
            if node["kind"] == "label":
                for line in node["flush"]:
                    lines.append(f"{pad}{line}")
                lines.append(("@label", node["address"], depth))
            elif node["kind"] == "stmt":
                used_labels.update(node.get("targets", ()))
                first = True
                for line in node["lines"]:
                    lines.append(f"{pad}{line}{comment_of(node) if first else ''}")
                    first = False
                if not node["lines"] and node.get("comment"):
                    lines.append(f"{pad}{node['comment']}")
            elif node["kind"] == "goto":
                following = items[position + 1] if position + 1 < len(items) else None
                if isinstance(following, dict) and following["kind"] == "label" and \
                        following["address"] == node["target"]:
                    if node.get("step"):
                        lines.append(f"{pad}{node['step']}{comment_of(node)}")
                    continue
                used_labels.add(node["target"])
                step = f"{node['step']} " if node.get("step") else ""
                lines.append(f"{pad}{step}goto L_{node['target']:04X};{comment_of(node)}")
            elif node["kind"] == "if_goto":
                used_labels.add(node["target"])
                lines.append(f"{pad}if ({node['branch']}) goto L_{node['target']:04X};{comment_of(node)}")

    walk(tree, 1)
    out: list[str] = []
    for line in lines:
        if isinstance(line, tuple):
            _, address, depth = line
            if address in used_labels:
                out.append(f"{'    ' * (depth - 1)}L_{address:04X}:;")
        else:
            out.append(line)
    return out


def register_words(registers: list[str], flags: list[str]) -> str:
    """`a, bc, hl, flags z c` for a list of registers and one of flags."""
    have = set(registers)
    words = ["a"] if "a" in have else []
    for pair in ("bc", "de", "hl"):
        if pair[0] in have and pair[1] in have:
            words.append(pair)
        else:
            words += [name for name in pair if name in have]
    if flags:
        words.append(("flag " if len(flags) == 1 else "flags ") + " ".join(flags))
    return ", ".join(words) if words else "nothing"


def signature_note(function: dict) -> str | None:
    """What the export worked out about how the function is used, as a
    comment: the registers it reads before writing them and, where every
    caller is one the export found, what those callers read afterwards."""
    registers, flags = function["registers"], function["flags"]
    if "read_after" not in registers:
        return None  # an export from before these
    note = "takes " + register_words(registers["reads"], flags["reads"])
    if function["summary_incomplete"]:
        note += " (and what the code it runs on into takes)"
    if not function["entered_by"] and function["stack_balanced"]:
        left = register_words([name for name in registers["read_after"] if name in registers["writes"]],
                              [name for name in flags["read_after"] if name in flags["writes"]])
        note += f"; of what it writes, the callers found read {left}"
    return f"/* {note} */"


def structured_source(function: dict, index: dict[str, dict], names: dict[int, str],
                      c_name: str, time: str = "block", operands: dict[int, str] | None = None) -> str:
    generator = Generator(function, index, names, time, operands)
    nodes = generator.build_nodes()
    entry = function["address"]
    body: list[str] = []
    used: set[int] = set()
    tree = structure(nodes)
    locals_ = [generator.capture_names[key] for key in sorted(generator.capture_names)]
    if locals_:
        body.append(f"    bool {', '.join(locals_)};")
    body += emit(tree, used)
    header = [f"/* {function['id']}  {display(function)}"
              f"  ({function['instruction_count']} instructions) */"]
    note = signature_note(function)
    if note:
        header.append(note)
    return "\n".join(header +
                     [f"GBLIFT_FUNCTION(0x{function['bank']:02X}, 0x{entry:04X}, {c_name}) {{"]
                     + body + ["}"]) + "\n"
