#!/usr/bin/env python3
import sys
import os
import random
import argparse
from pyboy import PyBoy

def main():
    parser = argparse.ArgumentParser(description="Capture unique (Bank, PC) execution points using PyBoy.")
    parser.add_argument("rom", help="Path to the ROM file")
    parser.add_argument("-o", "--output", default="ground_truth.trace", help="Output trace file")
    parser.add_argument("-f", "--frames", type=int, default=3600, help="Number of frames to run (default 3600 = 1 minute)")
    parser.add_argument("--random", action="store_true", help="Enable random input automation")
    parser.add_argument("--speed", type=int, default=0, help="Emulation speed (0 for unlimited)")
    
    args = parser.parse_args()
    
    if not os.path.exists(args.rom):
        print(f"Error: ROM not found: {args.rom}")
        return

    visited = set()
    
    # Initialize PyBoy
    print(f"Initializing PyBoy for {args.rom}...")
    with PyBoy(args.rom, window="null") as pyboy:
        pyboy.set_emulation_speed(args.speed)
        
        print(f"Running for {args.frames} frames...")
        
        buttons = ["a", "b", "start", "select", "up", "down", "left", "right"]
        
        for frame in range(args.frames):
            if frame % 600 == 0:
                print(f"  Frame {frame}/{args.frames}, unique addresses: {len(visited)}")
            
            # Note: PyBoy.tick() advances by 1 frame (approx 17556 cycles)
            # Sampling only at the end of the frame is okay for many points, 
            # but we rely on random input to discover more code paths.
            try:
                pc = pyboy.register_file.PC
                bank = 0
                if pc >= 0x4000 and pc < 0x8000:
                    try:
                        bank = pyboy.cartridge.active_rom_bank
                    except:
                        bank = 1 
                visited.add((bank, pc))
            except:
                pass

            # Random automation
            if frame % 10 == 0:
                if frame < 500: # Force start/A at beginning to skip intro
                    btn = "start" if frame % 20 == 0 else "a"
                    pyboy.button(btn, 5)
                elif args.random:
                    # More aggressive random mashing
                    if random.random() < 0.3:
                        btn = random.choice(buttons)
                        pyboy.button(btn, 15)
            
            # Run one frame
            if not pyboy.tick():
                break
                
        print(f"Finished. Captured {len(visited)} unique addresses.")
        
        # Save to file
        with open(args.output, "w") as f:
            for bank, addr in sorted(list(visited)):
                f.write(f"{bank}:{addr:04x}\n")
        
        print(f"Trace saved to {args.output}")

if __name__ == "__main__":
    main()
