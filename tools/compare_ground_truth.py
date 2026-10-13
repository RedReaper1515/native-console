#!/usr/bin/env python3
import os
import sys
import argparse
import re
import shutil
import subprocess
import glob
import importlib.util

def parse_disassembly(disasm_dir):
    """
    Parses mgbdis output with --print-hex.
    Returns a set of (bank, address) tuples representing the start of each instruction.
    """
    instructions = set()
    
    # Regex to find lines with address comments: ; $0000: $c3 $0c $02
    addr_regex = re.compile(r'; \$([0-9a-fA-F]+):')
    
    asm_files = glob.glob(os.path.join(disasm_dir, "bank_*.asm"))
    
    for asm_file in asm_files:
        # Extract bank from filename (bank_000.asm -> 0)
        basename = os.path.basename(asm_file)
        try:
            bank = int(basename.split('_')[1].split('.')[0])
        except (IndexError, ValueError):
            print(f"Warning: Could not parse bank from filename {basename}")
            continue
            
        with open(asm_file, 'r') as f:
            for line in f:
                match = addr_regex.search(line)
                if match:
                    addr_str = match.group(1)
                    addr = int(addr_str, 16)
                    instructions.add((bank, addr))
                    
    return instructions

def parse_trace_file(trace_path):
    """
    Parses a direct .trace file in bank:addr format.
    """
    instructions = set()
    with open(trace_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            try:
                bank_str, addr_str = line.split(':')
                bank = int(bank_str)
                addr = int(addr_str, 16)
                instructions.add((bank, addr))
            except (ValueError, IndexError):
                continue
    return instructions

def parse_recompiled_code(recompiled_dir):
    """
    Parses recompiled C code.
    Returns a set of (bank, address) tuples representing implemented instructions.
    Looks for comments like /* 01:4000 */ or /* 4000 */ (implicit bank 0)
    """
    instructions = set()
    
    # Regex for /* Bank:Addr */ or /* Addr */
    # Matches: /* 01:4000 */, /* 0000 */
    # C emitter format: /* BB:AAAA */ or /* AAAA */ (if bank 0 matches, or maybe 00:AAAA)
    # Based on tetris.c: /* 01:68d1 */
    # Let's support both formats.
    
    # Regex: /* (HH:)?HHHH */
    # Group 1: Bank (optional, with colon)
    # Group 2: Address
    # Regex: /* BB:AAAA */ or /* AAAA */
    # We require the comment to consist ONLY of the address to avoid matching instruction descriptions.
    comment_regex = re.compile(r'/\*\s+([0-9a-fA-F]{2}:)?([0-9a-fA-F]{4})\s+\*/')
    
    c_files = glob.glob(os.path.join(recompiled_dir, "*.c"))
    
    for c_file in c_files:
        with open(c_file, 'r') as f:
            content = f.read()
            # We iterate matches
            for match in comment_regex.finditer(content):
                bank_str = match.group(1)
                addr_str = match.group(2)
                
                addr = int(addr_str, 16)
                
                if bank_str:
                    bank = int(bank_str[:-1], 16) # remove colon
                else:
                    # If no bank specified, infer from address?
                    # Or assume bank 0? 
                    # The recompiler seems to always output bank if > 0.
                    # /* Function at 0000 */ -> bank 0?
                    # Let's verify tetris.c format for bank 0.
                    # Line 79 in c_emitter.cpp: if (bank > 0) output bank.
                    # So bank 0 won't have prefix.
                    bank = 0
                    
                instructions.add((bank, addr))
                
    return instructions

def resolve_mgbdis_command(explicit_path=None):
    """
    Resolve an external mgbdis installation.
    Returns a command prefix list, or None if unavailable.
    """
    candidates = []
    if explicit_path:
        candidates.append(explicit_path)

    env_path = os.environ.get("MGBDIS")
    if env_path:
        candidates.append(env_path)

    path_script = shutil.which("mgbdis.py")
    if path_script:
        candidates.append(path_script)

    path_executable = shutil.which("mgbdis")
    if path_executable:
        candidates.append(path_executable)

    seen = set()
    for candidate in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)

        resolved = candidate
        if not os.path.isabs(resolved) and os.path.sep not in resolved:
            resolved = shutil.which(resolved) or resolved

        if not os.path.exists(resolved):
            continue

        if resolved.endswith(".py"):
            return [sys.executable, resolved]
        return [resolved]

    if importlib.util.find_spec("mgbdis") is not None:
        return [sys.executable, "-m", "mgbdis"]

    return None

def main():
    parser = argparse.ArgumentParser(description="Compare recompiled code code against ground truth disassembly.")
    parser.add_argument("rom", help="Path to the original ROM file (optional if --trace is used)", nargs='?', default=None)
    parser.add_argument("recompiled_dir", help="Directory containing recompiled C code")
    parser.add_argument("--trace", help="Path to a .trace file to use as ground truth instead of disassembly")
    parser.add_argument("--mgbdis", help="Path to an external mgbdis executable or mgbdis.py script")
    parser.add_argument("--temp-dir", default="temp_ground_truth", help="Temporary directory for disassembly")
    parser.add_argument("--keep-temp", action="store_true", help="Keep temporary disassembly files")
    
    args = parser.parse_args()
    
    rom_path = os.path.abspath(args.rom) if args.rom else None
    recompiled_dir = os.path.abspath(args.recompiled_dir)
    temp_dir = os.path.abspath(args.temp_dir)
    
    if rom_path and not os.path.exists(rom_path):
        print(f"Error: ROM not found at {rom_path}")
        sys.exit(1)
        
    if not os.path.exists(recompiled_dir):
        print(f"Error: Recompiled directory not found at {recompiled_dir}")
        sys.exit(1)

    # 1/2. Get Ground Truth
    if args.trace:
        print(f"Loading ground truth from trace: {args.trace}...")
        asm_instrs = parse_trace_file(args.trace)
    else:
        if not args.rom:
            print("Error: ROM path required when not using --trace")
            sys.exit(1)
            
        # 1. Disassemble ROM
        print(f"Disassembling {rom_path}...")
        if os.path.exists(temp_dir):
            shutil.rmtree(temp_dir)
        os.makedirs(temp_dir)
        
        mgbdis_cmd = resolve_mgbdis_command(args.mgbdis)
        if not mgbdis_cmd:
            print("Error: Could not find mgbdis.")
            print("Install mgbdis separately and pass --mgbdis /path/to/mgbdis.py,")
            print("set the MGBDIS environment variable, or use --trace to skip disassembly.")
            sys.exit(1)

        cmd = mgbdis_cmd + [rom_path, "--output-dir", temp_dir, "--print-hex", "--overwrite"]
        subprocess.check_call(cmd)
        
        # 2. Parse Disassembly
        print("Parsing disassembly...")
        asm_instrs = parse_disassembly(temp_dir)

    print(f"Found {len(asm_instrs)} instructions in ground truth.")
    
    # 3. Parse Recompiled Code
    print("Parsing recompiled code...")
    c_instrs = parse_recompiled_code(recompiled_dir)
    print(f"Found {len(c_instrs)} implemented instructions in C code.")
    
    # 4. Compare
    missing = asm_instrs - c_instrs
    extra = c_instrs - asm_instrs # Instructions in C but not in ASM (e.g. alignment NOPs or false positives)
    
    total = len(asm_instrs)
    found = len(asm_instrs) - len(missing)
    coverage = (found / total * 100) if total > 0 else 0
    
    print("\nComparison Results:")
    print(f"Coverage: {coverage:.2f}% ({found}/{total})")
    print(f"Missing instructions: {len(missing)}")
    
    if missing:
        print("\nMissing Ranges (Bank:Addr):")
        # Group by contiguous ranges for cleaner output
        sorted_missing = sorted(list(missing))
        if not sorted_missing:
            return

        # Group into ranges
        ranges = []
        if not sorted_missing:
            return
            
        current_start = sorted_missing[0]
        current_prev = sorted_missing[0]
        
        for i in range(1, len(sorted_missing)):
            bank, addr = sorted_missing[i]
            prev_bank, prev_addr = current_prev
            
            # Simple heuristic: if same bank and addr is close to prev_addr
            # Since we don't know instruction length, we assume if it's within 4 bytes it's contiguous sequence
            # But sorted_missing are instruction starts. 
            # If we have 0003, 0004, 0005 -> contiguous.
            # If we have 0003, 0005 (2 byte instr) -> contiguous.
            # Let's say if gap <= 4 bytes.
            if bank == prev_bank and (addr - prev_addr) <= 4:
                current_prev = (bank, addr)
            else:
                ranges.append((current_start, current_prev))
                current_start = (bank, addr)
                current_prev = (bank, addr)
        ranges.append((current_start, current_prev))
        
        print(f"\nMissing Ranges ({len(ranges)} chunks):")
        for start, end in ranges[:50]: # Print first 50 chunks
            if start == end:
                print(f"  {start[0]:02X}:{start[1]:04X}")
            else:
                print(f"  {start[0]:02X}:{start[1]:04X} - {end[1]:04X}")
        
        if len(ranges) > 50:
            print(f"... and {len(ranges) - 50} more chunks.")

    if not args.keep_temp and os.path.exists(temp_dir):
        shutil.rmtree(temp_dir)

if __name__ == "__main__":
    main()
