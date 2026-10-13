#!/usr/bin/env python3

from __future__ import annotations

import argparse
import bisect
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


PROJECT_ROOT = Path(__file__).resolve().parents[1]
ROM_EXTENSIONS = {".gb", ".gbc", ".sgb"}

FRAME_RE = re.compile(
    r"^\[FRAME\] #(?P<frame>\d+).*? "
    r"fallbacks=(?P<fallbacks>\d+) "
    r"interp_instr=(?P<interp_instr>\d+) "
    r"interp_cycles=(?P<interp_cycles>\d+) "
    r"first=(?P<first_bank>[0-9A-F]{3}):(?P<first_addr>[0-9A-F]{4}) "
    r"last=(?P<last_bank>[0-9A-F]{3}):(?P<last_addr>[0-9A-F]{4}) "
    r"total_fallbacks=(?P<total_fallbacks>\d+)",
    re.IGNORECASE,
)
SUMMARY_RE = re.compile(
    r"^\[INTERP\] Summary: "
    r"fallbacks=(?P<fallbacks>\d+) "
    r"interpreter_entries=(?P<entries>\d+) "
    r"interpreter_instructions=(?P<instructions>\d+) "
    r"interpreter_cycles=(?P<cycles>\d+)",
    re.IGNORECASE,
)
HOTSPOT_RE = re.compile(
    r"^\[INTERP\] Hotspot #(?P<rank>\d+) "
    r"(?P<bank>[0-9A-F]{3}):(?P<addr>[0-9A-F]{4}) "
    r"entries=(?P<entries>\d+) "
    r"instructions=(?P<instructions>\d+) "
    r"cycles=(?P<cycles>\d+) "
    r"last_frame=(?P<last_frame>\d+)",
    re.IGNORECASE,
)
GAP_RE = re.compile(
    r"^\[INTERP\] Coverage gap: opcode=(?P<opcode>[0-9A-F]{2}) "
    r"at (?P<bank>[0-9A-F]{3}):(?P<addr>[0-9A-F]{4})",
    re.IGNORECASE,
)
NO_FALLBACK_RE = re.compile(r"^\[INTERP\] No interpreter fallback recorded\.$")

SPECIAL_OPCODES: Dict[int, Tuple[str, int]] = {
    0x00: ("NOP", 1),
    0x01: ("LD BC,{d16}", 3),
    0x02: ("LD (BC),A", 1),
    0x03: ("INC BC", 1),
    0x04: ("INC B", 1),
    0x05: ("DEC B", 1),
    0x06: ("LD B,{d8}", 2),
    0x07: ("RLCA", 1),
    0x08: ("LD ({a16}),SP", 3),
    0x09: ("ADD HL,BC", 1),
    0x0A: ("LD A,(BC)", 1),
    0x0B: ("DEC BC", 1),
    0x0C: ("INC C", 1),
    0x0D: ("DEC C", 1),
    0x0E: ("LD C,{d8}", 2),
    0x0F: ("RRCA", 1),
    0x10: ("STOP", 2),
    0x11: ("LD DE,{d16}", 3),
    0x12: ("LD (DE),A", 1),
    0x13: ("INC DE", 1),
    0x14: ("INC D", 1),
    0x15: ("DEC D", 1),
    0x16: ("LD D,{d8}", 2),
    0x17: ("RLA", 1),
    0x18: ("JR {rel}", 2),
    0x19: ("ADD HL,DE", 1),
    0x1A: ("LD A,(DE)", 1),
    0x1B: ("DEC DE", 1),
    0x1C: ("INC E", 1),
    0x1D: ("DEC E", 1),
    0x1E: ("LD E,{d8}", 2),
    0x1F: ("RRA", 1),
    0x20: ("JR NZ,{rel}", 2),
    0x21: ("LD HL,{d16}", 3),
    0x22: ("LD (HL+),A", 1),
    0x23: ("INC HL", 1),
    0x24: ("INC H", 1),
    0x25: ("DEC H", 1),
    0x26: ("LD H,{d8}", 2),
    0x27: ("DAA", 1),
    0x28: ("JR Z,{rel}", 2),
    0x29: ("ADD HL,HL", 1),
    0x2A: ("LD A,(HL+)", 1),
    0x2B: ("DEC HL", 1),
    0x2C: ("INC L", 1),
    0x2D: ("DEC L", 1),
    0x2E: ("LD L,{d8}", 2),
    0x2F: ("CPL", 1),
    0x30: ("JR NC,{rel}", 2),
    0x31: ("LD SP,{d16}", 3),
    0x32: ("LD (HL-),A", 1),
    0x33: ("INC SP", 1),
    0x34: ("INC (HL)", 1),
    0x35: ("DEC (HL)", 1),
    0x36: ("LD (HL),{d8}", 2),
    0x37: ("SCF", 1),
    0x38: ("JR C,{rel}", 2),
    0x39: ("ADD HL,SP", 1),
    0x3A: ("LD A,(HL-)", 1),
    0x3B: ("DEC SP", 1),
    0x3C: ("INC A", 1),
    0x3D: ("DEC A", 1),
    0x3E: ("LD A,{d8}", 2),
    0x3F: ("CCF", 1),
    0xC0: ("RET NZ", 1),
    0xC1: ("POP BC", 1),
    0xC2: ("JP NZ,{a16}", 3),
    0xC3: ("JP {a16}", 3),
    0xC4: ("CALL NZ,{a16}", 3),
    0xC5: ("PUSH BC", 1),
    0xC6: ("ADD A,{d8}", 2),
    0xC7: ("RST $00", 1),
    0xC8: ("RET Z", 1),
    0xC9: ("RET", 1),
    0xCA: ("JP Z,{a16}", 3),
    0xCB: ("PREFIX CB", 2),
    0xCC: ("CALL Z,{a16}", 3),
    0xCD: ("CALL {a16}", 3),
    0xCE: ("ADC A,{d8}", 2),
    0xCF: ("RST $08", 1),
    0xD0: ("RET NC", 1),
    0xD1: ("POP DE", 1),
    0xD2: ("JP NC,{a16}", 3),
    0xD3: ("DB $D3", 1),
    0xD4: ("CALL NC,{a16}", 3),
    0xD5: ("PUSH DE", 1),
    0xD6: ("SUB {d8}", 2),
    0xD7: ("RST $10", 1),
    0xD8: ("RET C", 1),
    0xD9: ("RETI", 1),
    0xDA: ("JP C,{a16}", 3),
    0xDB: ("DB $DB", 1),
    0xDC: ("CALL C,{a16}", 3),
    0xDD: ("DB $DD", 1),
    0xDE: ("SBC A,{d8}", 2),
    0xDF: ("RST $18", 1),
    0xE0: ("LDH ({a8}),A", 2),
    0xE1: ("POP HL", 1),
    0xE2: ("LD (C),A", 1),
    0xE3: ("DB $E3", 1),
    0xE4: ("DB $E4", 1),
    0xE5: ("PUSH HL", 1),
    0xE6: ("AND {d8}", 2),
    0xE7: ("RST $20", 1),
    0xE8: ("ADD SP,{s8}", 2),
    0xE9: ("JP HL", 1),
    0xEA: ("LD ({a16}),A", 3),
    0xEB: ("DB $EB", 1),
    0xEC: ("DB $EC", 1),
    0xED: ("DB $ED", 1),
    0xEE: ("XOR {d8}", 2),
    0xEF: ("RST $28", 1),
    0xF0: ("LDH A,({a8})", 2),
    0xF1: ("POP AF", 1),
    0xF2: ("LD A,(C)", 1),
    0xF3: ("DI", 1),
    0xF4: ("DB $F4", 1),
    0xF5: ("PUSH AF", 1),
    0xF6: ("OR {d8}", 2),
    0xF7: ("RST $30", 1),
    0xF8: ("LD HL,SP+{s8}", 2),
    0xF9: ("LD SP,HL", 1),
    0xFA: ("LD A,({a16})", 3),
    0xFB: ("EI", 1),
    0xFC: ("DB $FC", 1),
    0xFD: ("DB $FD", 1),
    0xFE: ("CP {d8}", 2),
    0xFF: ("RST $38", 1),
}


@dataclass(frozen=True, order=True)
class Address:
    bank: int
    addr: int

    def __str__(self) -> str:
        return f"{self.bank:03X}:{self.addr:04X}"

    def region(self) -> str:
        if self.addr < 0x4000:
            return "ROM0"
        if self.addr < 0x8000:
            return "ROMX"
        if self.addr < 0xA000:
            return "VRAM"
        if self.addr < 0xC000:
            return "SRAM"
        if self.addr < 0xE000:
            return "WRAM"
        if self.addr < 0xFE00:
            return "ECHO"
        if self.addr < 0xFEA0:
            return "OAM"
        if self.addr < 0xFF00:
            return "UNUSABLE"
        if self.addr < 0xFF80:
            return "IO/FFXX"
        if self.addr < 0xFFFF:
            return "HRAM"
        return "IE"


@dataclass
class FrameFallback:
    frame: int
    fallbacks: int
    interp_instr: int
    interp_cycles: int
    first: Address
    last: Address
    total_fallbacks: int


@dataclass
class InterpreterSummary:
    fallbacks: int
    interpreter_entries: int
    interpreter_instructions: int
    interpreter_cycles: int


@dataclass
class InterpreterHotspot:
    rank: int
    location: Address
    entries: int
    instructions: int
    cycles: int
    last_frame: int


@dataclass
class CoverageGap:
    opcode: int
    location: Address


@dataclass
class ParsedLog:
    path: Path
    frames: List[FrameFallback] = field(default_factory=list)
    summary: Optional[InterpreterSummary] = None
    hotspots: List[InterpreterHotspot] = field(default_factory=list)
    coverage_gaps: List[CoverageGap] = field(default_factory=list)
    no_fallback_recorded: bool = False


@dataclass
class ResolvedLocation:
    function_name: Optional[str]
    exact_name: Optional[str]
    provenance: Optional[str]

    def best_name(self) -> Optional[str]:
        return self.exact_name or self.function_name


@dataclass
class FunctionEntry:
    start: int
    name: str
    provenance: Optional[str]


@dataclass
class AddressSiteStats:
    location: Address
    first_frames: int = 0
    last_frames: int = 0
    frame_interp_instr: int = 0
    frame_interp_cycles: int = 0
    interpreter_entries: int = 0
    interpreter_instructions: int = 0
    interpreter_cycles: int = 0
    last_frame_seen: int = 0

    def total_frame_hits(self) -> int:
        return self.first_frames + self.last_frames


def normalize_name(value: str) -> str:
    return "".join(ch for ch in value.lower() if ch.isalnum())


def collect_name_hints(raw_value: str) -> List[str]:
    hints = []
    if raw_value:
        hints.append(raw_value)
    markers = (
        "_interpreter",
        "_summary",
        "_smoke",
        "_perf",
        "_run",
        "_test",
        "_session",
        "_verify",
        "_limit",
        "_frames",
    )
    for hint in list(hints):
        for marker in markers:
            if marker in hint:
                prefix = hint.split(marker, 1)[0]
                if prefix and prefix not in hints:
                    hints.append(prefix)
    return hints


def parse_hex_address(bank_text: str, addr_text: str) -> Address:
    return Address(int(bank_text, 16), int(addr_text, 16))


def parse_log(path: Path) -> ParsedLog:
    parsed = ParsedLog(path=path)
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line:
                continue

            match = FRAME_RE.match(line)
            if match:
                parsed.frames.append(
                    FrameFallback(
                        frame=int(match.group("frame")),
                        fallbacks=int(match.group("fallbacks")),
                        interp_instr=int(match.group("interp_instr")),
                        interp_cycles=int(match.group("interp_cycles")),
                        first=parse_hex_address(match.group("first_bank"), match.group("first_addr")),
                        last=parse_hex_address(match.group("last_bank"), match.group("last_addr")),
                        total_fallbacks=int(match.group("total_fallbacks")),
                    )
                )
                continue

            match = SUMMARY_RE.match(line)
            if match:
                parsed.summary = InterpreterSummary(
                    fallbacks=int(match.group("fallbacks")),
                    interpreter_entries=int(match.group("entries")),
                    interpreter_instructions=int(match.group("instructions")),
                    interpreter_cycles=int(match.group("cycles")),
                )
                continue

            match = HOTSPOT_RE.match(line)
            if match:
                parsed.hotspots.append(
                    InterpreterHotspot(
                        rank=int(match.group("rank")),
                        location=parse_hex_address(match.group("bank"), match.group("addr")),
                        entries=int(match.group("entries")),
                        instructions=int(match.group("instructions")),
                        cycles=int(match.group("cycles")),
                        last_frame=int(match.group("last_frame")),
                    )
                )
                continue

            match = GAP_RE.match(line)
            if match:
                parsed.coverage_gaps.append(
                    CoverageGap(
                        opcode=int(match.group("opcode"), 16),
                        location=parse_hex_address(match.group("bank"), match.group("addr")),
                    )
                )
                continue

            if NO_FALLBACK_RE.match(line):
                parsed.no_fallback_recorded = True

    return parsed


def choose_display_name(entry: dict) -> Optional[str]:
    return entry.get("source_symbol") or entry.get("emitted_name")


def load_metadata(path: Path) -> Tuple[Dict[Address, Tuple[str, Optional[str]]], Dict[int, List[FunctionEntry]], Optional[str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    exact_names: Dict[Address, Tuple[str, Optional[str]]] = {}
    functions_by_bank: Dict[int, List[FunctionEntry]] = {}

    for entry in data.get("functions", []):
        name = choose_display_name(entry)
        if not name:
            continue
        address = Address(int(entry["bank"]), int(entry["address"], 16))
        provenance = entry.get("provenance")
        exact_names[address] = (name, provenance)
        functions_by_bank.setdefault(address.bank, []).append(
            FunctionEntry(start=address.addr, name=name, provenance=provenance)
        )

    for entry in data.get("labels", []):
        name = choose_display_name(entry)
        if not name:
            continue
        address = Address(int(entry["bank"]), int(entry["address"], 16))
        exact_names.setdefault(address, (name, entry.get("provenance")))

    for entries in functions_by_bank.values():
        entries.sort(key=lambda item: item.start)

    return exact_names, functions_by_bank, data.get("rom_name")


def load_sym_file(path: Path) -> Dict[Address, Tuple[str, Optional[str]]]:
    results: Dict[Address, Tuple[str, Optional[str]]] = {}
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith(";"):
                continue
            line = line.split(";", 1)[0].rstrip()
            parts = line.split(maxsplit=1)
            if len(parts) != 2 or ":" not in parts[0]:
                continue
            bank_text, addr_text = parts[0].split(":", 1)
            try:
                address = parse_hex_address(bank_text, addr_text)
            except ValueError:
                continue
            results[address] = (parts[1].strip(), "sym")
    return results


class SymbolResolver:
    def __init__(
        self,
        exact_names: Optional[Dict[Address, Tuple[str, Optional[str]]]] = None,
        functions_by_bank: Optional[Dict[int, List[FunctionEntry]]] = None,
    ) -> None:
        self.exact_names = exact_names or {}
        self.functions_by_bank = functions_by_bank or {}

    def resolve(self, location: Address) -> ResolvedLocation:
        exact = self.exact_names.get(location)
        function_name: Optional[str] = None
        provenance: Optional[str] = None

        if location.addr < 0x8000:
            candidates = self.functions_by_bank.get(location.bank)
            if candidates:
                starts = [entry.start for entry in candidates]
                index = bisect.bisect_right(starts, location.addr) - 1
                if index >= 0:
                    function_name = candidates[index].name
                    provenance = candidates[index].provenance

        if exact is not None:
            return ResolvedLocation(function_name=function_name, exact_name=exact[0], provenance=exact[1])
        return ResolvedLocation(function_name=function_name, exact_name=None, provenance=provenance)


def autodetect_metadata(log_path: Path) -> Optional[Path]:
    output_root = PROJECT_ROOT / "output"
    if not output_root.exists():
        return None

    hint_keys = {normalize_name(hint) for hint in collect_name_hints(log_path.stem)}
    candidates: List[Path] = []
    for metadata_path in output_root.rglob("*_metadata.json"):
        path_keys = {
            normalize_name(metadata_path.stem.removesuffix("_metadata")),
            normalize_name(metadata_path.parent.name),
        }
        score = 0
        for hint in hint_keys:
            if not hint:
                continue
            if hint in path_keys:
                score = max(score, 3)
            elif any(hint in path_key for path_key in path_keys if path_key):
                score = max(score, 2)
            elif any(path_key and path_key in hint for path_key in path_keys):
                score = max(score, 1)
        if score > 0:
            candidates.append((score, -len(metadata_path.name), metadata_path))

    if not candidates:
        return None

    candidates.sort(reverse=True)
    best_score = candidates[0][0]
    best = [candidate for score, _, candidate in candidates if score == best_score]
    return best[0] if len(best) == 1 else None


def autodetect_rom(log_path: Path, rom_name_hint: Optional[str], metadata_path: Optional[Path]) -> Optional[Path]:
    rom_root = PROJECT_ROOT / "roms"
    if not rom_root.exists():
        return None

    hints: List[str] = []
    if rom_name_hint:
        hints.extend(collect_name_hints(rom_name_hint))
    if metadata_path is not None:
        hints.extend(collect_name_hints(metadata_path.stem.removesuffix("_metadata")))
        if metadata_path.parent.name:
            hints.extend(collect_name_hints(metadata_path.parent.name))
    hints.extend(collect_name_hints(log_path.stem))

    normalized_hints = {normalize_name(hint) for hint in hints if hint}
    candidates = [path for path in rom_root.rglob("*") if path.suffix.lower() in ROM_EXTENSIONS]
    scored: List[Tuple[int, int, Path]] = []
    for candidate in candidates:
        stem_key = normalize_name(candidate.stem)
        name_key = normalize_name(candidate.name)
        score = 0
        for hint in normalized_hints:
            if not hint:
                continue
            if hint == stem_key or hint == name_key:
                score = max(score, 3)
            elif hint and hint in stem_key:
                score = max(score, 2)
            elif stem_key and stem_key in hint:
                score = max(score, 1)
        if score > 0:
            scored.append((score, -len(candidate.name), candidate))

    if not scored:
        return None

    scored.sort(reverse=True)
    best_score = scored[0][0]
    best = [candidate for score, _, candidate in scored if score == best_score]
    return best[0] if len(best) == 1 else None


def rom_offset_for(location: Address) -> Optional[int]:
    if location.addr < 0x4000:
        return location.addr
    if location.addr < 0x8000:
        return location.bank * 0x4000 + (location.addr - 0x4000)
    return None


def signed_byte(value: int) -> int:
    return value - 0x100 if value & 0x80 else value


def fmt_u8(value: int) -> str:
    return f"${value:02X}"


def fmt_u16(value: int) -> str:
    return f"${value:04X}"


def format_template(template: str, location: Address, data: bytes) -> str:
    replacements = {
        "{d8}": fmt_u8(data[1]) if len(data) > 1 else "<?>",
        "{a8}": fmt_u8(data[1]) if len(data) > 1 else "<?>",
        "{s8}": f"{signed_byte(data[1]):+d}" if len(data) > 1 else "<?>",
        "{d16}": fmt_u16(data[1] | (data[2] << 8)) if len(data) > 2 else "<?>",
        "{a16}": fmt_u16(data[1] | (data[2] << 8)) if len(data) > 2 else "<?>",
        "{rel}": fmt_u16((location.addr + 2 + signed_byte(data[1])) & 0xFFFF) if len(data) > 1 else "<?>",
    }
    result = template
    for key, value in replacements.items():
        result = result.replace(key, value)
    return result


def decode_cb_opcode(cb_opcode: int) -> str:
    regs = ["B", "C", "D", "E", "H", "L", "(HL)", "A"]
    rotates = ["RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"]
    reg = regs[cb_opcode & 0x07]
    group = cb_opcode >> 6
    bit = (cb_opcode >> 3) & 0x07
    if group == 0:
        return f"{rotates[bit]} {reg}"
    if group == 1:
        return f"BIT {bit},{reg}"
    if group == 2:
        return f"RES {bit},{reg}"
    return f"SET {bit},{reg}"


def decode_instruction(rom_data: bytes, location: Address) -> Tuple[Optional[str], Optional[str]]:
    offset = rom_offset_for(location)
    if offset is None:
        return None, f"{location.region()} runtime code"
    if offset < 0 or offset >= len(rom_data):
        return None, "ROM offset out of range"

    opcode = rom_data[offset]
    if opcode == 0xCB:
        if offset + 1 >= len(rom_data):
            return "CB ??", "Truncated CB opcode"
        cb_opcode = rom_data[offset + 1]
        return decode_cb_opcode(cb_opcode), None

    if opcode in SPECIAL_OPCODES:
        template, length = SPECIAL_OPCODES[opcode]
        data = rom_data[offset : min(len(rom_data), offset + length)]
        return format_template(template, location, data), None

    regs = ["B", "C", "D", "E", "H", "L", "(HL)", "A"]
    alu_ops = [
        "ADD A,{}",
        "ADC A,{}",
        "SUB {}",
        "SBC A,{}",
        "AND {}",
        "XOR {}",
        "OR {}",
        "CP {}",
    ]
    if 0x40 <= opcode <= 0x7F:
        if opcode == 0x76:
            return "HALT", None
        dst = regs[(opcode >> 3) & 0x07]
        src = regs[opcode & 0x07]
        return f"LD {dst},{src}", None
    if 0x80 <= opcode <= 0xBF:
        return alu_ops[(opcode >> 3) & 0x07].format(regs[opcode & 0x07]), None

    return f"DB ${opcode:02X}", "Unknown opcode pattern"


def read_rom(path: Optional[Path]) -> Optional[bytes]:
    if path is None:
        return None
    try:
        return path.read_bytes()
    except OSError:
        return None


def aggregate_sites(parsed: ParsedLog) -> Dict[Address, AddressSiteStats]:
    sites: Dict[Address, AddressSiteStats] = {}

    def get_site(location: Address) -> AddressSiteStats:
        site = sites.get(location)
        if site is None:
            site = AddressSiteStats(location=location)
            sites[location] = site
        return site

    for frame in parsed.frames:
        first_site = get_site(frame.first)
        first_site.first_frames += 1
        first_site.frame_interp_instr += frame.interp_instr
        first_site.frame_interp_cycles += frame.interp_cycles
        first_site.last_frame_seen = max(first_site.last_frame_seen, frame.frame)

        last_site = get_site(frame.last)
        last_site.last_frames += 1
        if frame.last != frame.first:
            last_site.frame_interp_instr += frame.interp_instr
            last_site.frame_interp_cycles += frame.interp_cycles
        last_site.last_frame_seen = max(last_site.last_frame_seen, frame.frame)

    for hotspot in parsed.hotspots:
        site = get_site(hotspot.location)
        site.interpreter_entries += hotspot.entries
        site.interpreter_instructions += hotspot.instructions
        site.interpreter_cycles += hotspot.cycles
        site.last_frame_seen = max(site.last_frame_seen, hotspot.last_frame)

    return sites


def aggregate_pairs(frames: Iterable[FrameFallback]) -> List[Tuple[Tuple[Address, Address], dict]]:
    pair_stats: Dict[Tuple[Address, Address], dict] = {}
    for frame in frames:
        key = (frame.first, frame.last)
        item = pair_stats.setdefault(
            key,
            {
                "frames": 0,
                "fallbacks": 0,
                "interp_instr": 0,
                "interp_cycles": 0,
                "first_frame": frame.frame,
                "last_frame": frame.frame,
            },
        )
        item["frames"] += 1
        item["fallbacks"] += frame.fallbacks
        item["interp_instr"] += frame.interp_instr
        item["interp_cycles"] += frame.interp_cycles
        item["last_frame"] = frame.frame
    return sorted(
        pair_stats.items(),
        key=lambda item: (
            item[1]["fallbacks"],
            item[1]["interp_cycles"],
            item[1]["frames"],
        ),
        reverse=True,
    )


def aggregate_functions(
    sites: Dict[Address, AddressSiteStats],
    resolver: SymbolResolver,
) -> List[Tuple[str, dict]]:
    function_stats: Dict[str, dict] = {}
    for site in sites.values():
        resolved = resolver.resolve(site.location)
        name = resolved.function_name or resolved.best_name()
        if not name:
            name = f"[{site.location.region()}]"
        item = function_stats.setdefault(
            name,
            {
                "sites": 0,
                "frame_hits": 0,
                "interpreter_entries": 0,
                "interpreter_instructions": 0,
                "interpreter_cycles": 0,
            },
        )
        item["sites"] += 1
        item["frame_hits"] += site.total_frame_hits()
        item["interpreter_entries"] += site.interpreter_entries
        item["interpreter_instructions"] += site.interpreter_instructions
        item["interpreter_cycles"] += site.interpreter_cycles

    return sorted(
        function_stats.items(),
        key=lambda item: (
            item[1]["interpreter_cycles"],
            item[1]["frame_hits"],
        ),
        reverse=True,
    )


def aggregate_instructions(
    sites: Dict[Address, AddressSiteStats],
    rom_data: Optional[bytes],
) -> List[Tuple[str, dict]]:
    instruction_stats: Dict[str, dict] = {}
    for site in sites.values():
        if rom_data is None:
            mnemonic = f"[{site.location.region()}]"
        else:
            decoded, note = decode_instruction(rom_data, site.location)
            mnemonic = decoded or f"[{site.location.region()}]"
            if note and decoded is None:
                mnemonic = f"{mnemonic} ({note})"
        item = instruction_stats.setdefault(
            mnemonic,
            {
                "sites": 0,
                "frame_hits": 0,
                "interpreter_entries": 0,
                "interpreter_cycles": 0,
            },
        )
        item["sites"] += 1
        item["frame_hits"] += site.total_frame_hits()
        item["interpreter_entries"] += site.interpreter_entries
        item["interpreter_cycles"] += site.interpreter_cycles

    return sorted(
        instruction_stats.items(),
        key=lambda item: (
            item[1]["interpreter_cycles"],
            item[1]["frame_hits"],
        ),
        reverse=True,
    )


def format_site_line(
    site: AddressSiteStats,
    resolver: SymbolResolver,
    rom_data: Optional[bytes],
) -> str:
    resolved = resolver.resolve(site.location)
    pieces = [str(site.location)]

    best_name = resolved.best_name()
    if best_name:
        pieces.append(best_name)
    else:
        pieces.append(f"[{site.location.region()}]")

    parts = [
        f"first_frames={site.first_frames}",
        f"last_frames={site.last_frames}",
    ]
    if site.interpreter_entries > 0 or site.interpreter_cycles > 0:
        parts.append(f"entries={site.interpreter_entries}")
        parts.append(f"interp_instr={site.interpreter_instructions}")
        parts.append(f"cycles={site.interpreter_cycles}")

    if rom_data is not None:
        decoded, note = decode_instruction(rom_data, site.location)
        if decoded:
            parts.append(f"opcode={decoded}")
        elif note:
            parts.append(note)
    return "  - " + " | ".join([" ".join(pieces)] + parts)


def format_totals(parsed: ParsedLog) -> List[str]:
    total_frame_fallbacks = sum(frame.fallbacks for frame in parsed.frames)
    total_frame_interp_instr = sum(frame.interp_instr for frame in parsed.frames)
    total_frame_interp_cycles = sum(frame.interp_cycles for frame in parsed.frames)
    last_total = parsed.frames[-1].total_fallbacks if parsed.frames else 0

    lines = [
        f"  - frame fallback lines: {len(parsed.frames)}",
        f"  - sampled frame fallbacks: {total_frame_fallbacks}",
    ]
    if parsed.summary is not None:
        lines.append(f"  - total dispatch fallbacks: {parsed.summary.fallbacks}")
        lines.append(f"  - interpreter entries: {parsed.summary.interpreter_entries}")
        lines.append(f"  - interpreter instructions: {parsed.summary.interpreter_instructions}")
        lines.append(f"  - interpreter cycles: {parsed.summary.interpreter_cycles}")
    elif last_total:
        lines.append(f"  - last reported total_fallbacks: {last_total}")

    if total_frame_interp_instr or total_frame_interp_cycles:
        lines.append(f"  - frame-interpreted instructions: {total_frame_interp_instr}")
        lines.append(f"  - frame-interpreted cycles: {total_frame_interp_cycles}")

    lines.append(f"  - hotspot rows: {len(parsed.hotspots)}")
    lines.append(f"  - coverage gaps: {len(parsed.coverage_gaps)}")
    return lines


def summarize(parsed: ParsedLog, resolver: SymbolResolver, rom_data: Optional[bytes], top_n: int) -> str:
    lines = [f"Interpreter log summary: {parsed.path}"]
    lines.append("")

    if parsed.no_fallback_recorded and not parsed.frames and not parsed.hotspots and not parsed.coverage_gaps:
        lines.append("No interpreter fallback recorded.")
        return "\n".join(lines)

    lines.append("Totals")
    lines.extend(format_totals(parsed))

    sites = aggregate_sites(parsed)
    top_sites = sorted(
        sites.values(),
        key=lambda site: (
            site.interpreter_cycles,
            site.total_frame_hits(),
        ),
        reverse=True,
    )
    if top_sites:
        lines.append("")
        lines.append("Top Fallback Sites")
        for site in top_sites[:top_n]:
            lines.append(format_site_line(site, resolver, rom_data))

    pairs = aggregate_pairs(parsed.frames)
    if pairs:
        lines.append("")
        lines.append("Top Frame Fallback Pairs")
        for (first, last), stats in pairs[:top_n]:
            first_name = resolver.resolve(first).best_name() or f"[{first.region()}]"
            last_name = resolver.resolve(last).best_name() or f"[{last.region()}]"
            lines.append(
                "  - "
                f"{first} ({first_name}) -> {last} ({last_name}) | "
                f"frames={stats['frames']} frame_fallbacks={stats['fallbacks']} "
                f"interp_instr={stats['interp_instr']} interp_cycles={stats['interp_cycles']}"
            )

    functions = aggregate_functions(sites, resolver)
    if functions:
        lines.append("")
        lines.append("Top Fallback Functions")
        for name, stats in functions[:top_n]:
            lines.append(
                "  - "
                f"{name} | sites={stats['sites']} frame_hits={stats['frame_hits']} "
                f"entries={stats['interpreter_entries']} "
                f"interp_instr={stats['interpreter_instructions']} interp_cycles={stats['interpreter_cycles']}"
            )

    instructions = aggregate_instructions(sites, rom_data)
    if instructions:
        lines.append("")
        lines.append("Top Fallback Instructions")
        for mnemonic, stats in instructions[:top_n]:
            lines.append(
                "  - "
                f"{mnemonic} | sites={stats['sites']} frame_hits={stats['frame_hits']} "
                f"entries={stats['interpreter_entries']} "
                f"interp_cycles={stats['interpreter_cycles']}"
            )

    if parsed.hotspots:
        lines.append("")
        lines.append("Interpreter Hotspots")
        for hotspot in parsed.hotspots[:top_n]:
            resolved = resolver.resolve(hotspot.location)
            name = resolved.best_name() or f"[{hotspot.location.region()}]"
            instruction_suffix = ""
            if rom_data is not None:
                decoded, _ = decode_instruction(rom_data, hotspot.location)
                if decoded:
                    instruction_suffix = f" instr={decoded}"
            lines.append(
                "  - "
                f"{hotspot.location} {name} | entries={hotspot.entries} "
                f"instructions={hotspot.instructions} cycles={hotspot.cycles} "
                f"last_frame={hotspot.last_frame}{instruction_suffix}"
            )

    if parsed.coverage_gaps:
        gap_counter = Counter((gap.opcode, gap.location) for gap in parsed.coverage_gaps)
        lines.append("")
        lines.append("Coverage Gaps")
        for (opcode, location), count in gap_counter.most_common(top_n):
            resolved = resolver.resolve(location)
            name = resolved.best_name() or f"[{location.region()}]"
            opcode_name = f"${opcode:02X}"
            if rom_data is not None:
                decoded, _ = decode_instruction(bytes([opcode]), Address(0, 0))
                if decoded and not decoded.startswith("DB "):
                    opcode_name += f" ({decoded})"
            lines.append(f"  - {location} {name} | opcode={opcode_name} count={count}")

    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Condense generated-to-interpreter fallback logs into hotspot summaries."
    )
    parser.add_argument("log_file", type=Path, help="Path to the runtime .log file to summarize")
    parser.add_argument(
        "--metadata",
        type=Path,
        default=None,
        help="Optional generated *_metadata.json file for symbol/function resolution",
    )
    parser.add_argument(
        "--sym",
        type=Path,
        default=None,
        help="Optional .sym file for exact address names when metadata is unavailable",
    )
    parser.add_argument(
        "--rom",
        type=Path,
        default=None,
        help="Optional ROM file used to decode the fallback opcode at each site",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=8,
        help="How many rows to print in each section (default: 8)",
    )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    log_path = args.log_file.resolve()
    if not log_path.is_file():
        parser.error(f"log file not found: {log_path}")

    metadata_path = args.metadata.resolve() if args.metadata else autodetect_metadata(log_path)
    sym_path = args.sym.resolve() if args.sym else None

    metadata_exact: Dict[Address, Tuple[str, Optional[str]]] = {}
    functions_by_bank: Dict[int, List[FunctionEntry]] = {}
    rom_name_hint: Optional[str] = None
    if metadata_path is not None:
        if not metadata_path.is_file():
            parser.error(f"metadata file not found: {metadata_path}")
        metadata_exact, functions_by_bank, rom_name_hint = load_metadata(metadata_path)

    sym_exact: Dict[Address, Tuple[str, Optional[str]]] = {}
    if sym_path is not None:
        if not sym_path.is_file():
            parser.error(f"symbol file not found: {sym_path}")
        sym_exact = load_sym_file(sym_path)

    combined_exact = dict(sym_exact)
    combined_exact.update(metadata_exact)
    resolver = SymbolResolver(combined_exact, functions_by_bank)

    rom_path = args.rom.resolve() if args.rom else autodetect_rom(log_path, rom_name_hint, metadata_path)
    rom_data = read_rom(rom_path)

    parsed = parse_log(log_path)

    preface = []
    if metadata_path is not None:
        preface.append(f"Resolved metadata: {metadata_path}")
    if sym_path is not None:
        preface.append(f"Resolved symbols: {sym_path}")
    if rom_path is not None and rom_data is not None:
        preface.append(f"Resolved ROM: {rom_path}")
    elif args.rom is not None and rom_data is None:
        preface.append(f"ROM unavailable: {rom_path}")

    output = summarize(parsed, resolver, rom_data, max(args.top, 1))
    if preface:
        print("\n".join(preface))
        print("")
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
