#!/usr/bin/env python3
"""
Automated Ground Truth Guided Recompilation Workflow

This script automates the entire process from capturing execution traces
to recompiling and verifying coverage.
"""

import sys
import os
import argparse
import subprocess
import tempfile
import shutil
from pathlib import Path

def run_command(cmd, cwd=None, capture_output=False, check=True):
    """Run a command with optional output capture"""
    try:
        print(f"Running: {' '.join(cmd)}")
        if capture_output:
            result = subprocess.run(cmd, cwd=cwd, check=check,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True)
            return result.returncode, result.stdout, result.stderr
        else:
            result = subprocess.run(cmd, cwd=cwd, check=check)
            return result.returncode, None, None
    except subprocess.CalledProcessError as e:
        print(f"Command failed with error code {e.returncode}")
        if e.output:
            print(f"Output: {e.output}")
        if e.stderr:
            print(f"Error: {e.stderr}")
        return e.returncode, None, None

def validate_rom(rom_path):
    """Validate that the ROM file exists and has the correct extension"""
    if not os.path.exists(rom_path):
        print(f"Error: ROM file not found: {rom_path}")
        return False
    
    ext = os.path.splitext(rom_path)[1].lower()
    if ext not in ['.gb', '.gbc']:
        print(f"Error: Invalid ROM extension '{ext}'. Expected .gb or .gbc")
        return False
    
    return True

def capture_ground_truth(rom_path, output_dir, frames=18000, use_random=True):
    """Capture ground truth execution trace using PyBoy"""
    print("\n=== Step 1: Capturing Ground Truth ===")
    
    trace_file = os.path.join(output_dir, "ground_truth.trace")
    
    cmd = [sys.executable, "tools/capture_ground_truth.py", rom_path,
           "-o", trace_file,
           "-f", str(frames)]
    
    if use_random:
        cmd.append("--random")
    
    returncode, stdout, stderr = run_command(cmd)
    
    if returncode != 0:
        print(f"Error capturing ground truth: {stderr}")
        return None
    
    if not os.path.exists(trace_file):
        print("Error: Trace file was not created")
        return None
    
    print(f"Successfully captured ground truth to: {trace_file}")
    return trace_file

def compile_with_trace(rom_path, output_dir, trace_file, single_function=False):
    """Recompile ROM using the captured trace"""
    print("\n=== Step 2: Compiling with Trace ===")
    
    # Create output directory
    compile_dir = os.path.join(output_dir, "compiled")
    if os.path.exists(compile_dir):
        print(f"Removing existing compile directory: {compile_dir}")
        shutil.rmtree(compile_dir)
    
    # Run recompiler
    cmd = ["./build/bin/gbrecomp", rom_path,
           "-o", compile_dir,
           "--use-trace", trace_file]
    
    if single_function:
        cmd.append("--single-function")
    
    returncode, stdout, stderr = run_command(cmd)
    
    if returncode != 0:
        print(f"Error compiling ROM: {stderr}")
        return None
    
    # Check if compiled successfully
    build_dir = os.path.join(compile_dir, "build")
    if not os.path.exists(build_dir):
        os.makedirs(build_dir)
    
    # Configure CMake
    cmake_cmd = ["cmake", "-G", "Ninja", ".."]
    returncode, stdout, stderr = run_command(cmake_cmd, cwd=build_dir)
    
    if returncode != 0:
        print(f"Error configuring CMake: {stderr}")
        return None
    
    # Build the project
    ninja_cmd = ["ninja"]
    returncode, stdout, stderr = run_command(ninja_cmd, cwd=build_dir)
    
    if returncode != 0:
        print(f"Error building project: {stderr}")
        return None
    
    print(f"Successfully compiled to: {compile_dir}")
    return compile_dir

def verify_coverage(trace_file, compile_dir):
    """Verify recompiler coverage against the ground truth trace"""
    print("\n=== Step 3: Verifying Coverage ===")
    
    cmd = [sys.executable, "tools/compare_ground_truth.py",
           "--trace", trace_file,
           compile_dir]
    
    returncode, stdout, stderr = run_command(cmd)
    
    if returncode != 0:
        print(f"Error verifying coverage: {stderr}")
        return False
    
    return True

def refine_trace(rom_path, compile_dir, frames=2000000):
    """Refine the execution trace using the recompiled binary"""
    print("\n=== Step 4: Refining Trace (Optional) ===")
    
    # Find the compiled executable
    build_dir = os.path.join(compile_dir, "build")
    if not os.path.exists(build_dir):
        print("Error: Build directory not found")
        return None
    
    # Find the executable file
    exe_files = list(Path(build_dir).glob("*"))
    if not exe_files:
        print("Error: No executable files found in build directory")
        return None
    
    # Try to find the main executable (excluding lib files and CMake files)
    exe = None
    for f in exe_files:
        if f.suffix not in [".a", ".lib", ".cmake"] and not f.name.startswith("CMake"):
            exe = str(f)
            break
    
    if not exe:
        print("Error: Could not find the main executable")
        return None
    
    # Run to capture refined trace
    refined_trace = os.path.join(os.path.dirname(compile_dir), "refined_trace.trace")
    
    cmd = [exe,
           "--trace-entries", refined_trace,
           "--limit", str(frames)]
    
    print(f"Running executable to refine trace: {exe}")
    
    # This might fail if the ROM requires input or crashes, but that's expected
    returncode, stdout, stderr = run_command(cmd, check=False)
    
    if os.path.exists(refined_trace):
        print(f"Successfully captured refined trace to: {refined_trace}")
        return refined_trace
    else:
        print("Warning: No refined trace was generated (ROM may have crashed)")
        return None

def merge_traces(trace1, trace2, output_file):
    """Merge two trace files, removing duplicates"""
    print(f" Merging traces: {trace1} + {trace2}")
    
    # Read both trace files
    visited = set()
    
    for trace_file in [trace1, trace2]:
        if os.path.exists(trace_file):
            with open(trace_file, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line and ':' in line:
                        visited.add(line)
    
    # Write merged trace
    with open(output_file, 'w') as f:
        for entry in sorted(visited):
            f.write(f"{entry}\n")
    
    print(f"Merged {len(visited)} unique entries to: {output_file}")
    return output_file

def main():
    parser = argparse.ArgumentParser(
        description="Automated ground truth guided recompilation workflow",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example usage:
  python run_ground_truth.py roms/game.gb
  
This script will:
1. Capture a ground truth execution trace using PyBoy
2. Recompile the ROM with trace-guided analysis
3. Verify the coverage of the recompiled code
4. Optionally refine and merge traces for better coverage
        """
    )
    
    parser.add_argument("rom", help="Path to the ROM file (.gb or .gbc)")
    parser.add_argument("-o", "--output-dir", help="Output directory for compilation")
    parser.add_argument("-f", "--frames", type=int, default=18000,
                        help="Number of frames to run for initial trace capture (default: 18000)")
    parser.add_argument("--no-random", action="store_true",
                        help="Disable random input automation")
    parser.add_argument("--refine", action="store_true",
                        help="Enable trace refinement using recompiled binary")
    parser.add_argument("--single-function", action="store_true",
                        help="Use single function mode for recompilation")
    parser.add_argument("--keep-temp", action="store_true",
                        help="Keep temporary files after execution")
    
    args = parser.parse_args()
    
    # Validate input
    if not validate_rom(args.rom):
        return 1
    
    # Create output directory
    if args.output_dir:
        output_dir = args.output_dir
    else:
        rom_name = os.path.splitext(os.path.basename(args.rom))[0]
        output_dir = f"{rom_name}_output"
    
    if os.path.exists(output_dir) and not args.keep_temp:
        print(f"Removing existing output directory: {output_dir}")
        shutil.rmtree(output_dir)
    
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # Check if recompiler is built
    if not os.path.exists("./build/bin/gbrecomp") and not os.path.exists("./build/bin/gbrecomp.exe"):
        print("\nRecompiler not found! Building...")
        cmd = ["cmake", "-G", "Ninja", "-B", "build", "."]
        if run_command(cmd)[0] != 0:
            return 1
        
        cmd = ["ninja", "-C", "build"]
        if run_command(cmd)[0] != 0:
            return 1
    
    # Step 1: Capture ground truth
    trace_file = capture_ground_truth(
        args.rom,
        output_dir,
        args.frames,
        not args.no_random
    )
    
    if not trace_file:
        return 1
    
    # Step 2: Compile with trace
    compile_dir = compile_with_trace(
        args.rom,
        output_dir,
        trace_file,
        args.single_function
    )
    
    if not compile_dir:
        return 1
    
    # Step 3: Verify coverage
    if not verify_coverage(trace_file, compile_dir):
        return 1
    
    # Step 4: Refine trace (optional)
    if args.refine:
        refined_trace = refine_trace(args.rom, compile_dir)
        
        if refined_trace:
            merged_trace = os.path.join(output_dir, "merged_trace.trace")
            merge_traces(trace_file, refined_trace, merged_trace)
            
            # Recompile with merged trace
            print("\n=== Step 5: Recompiling with Merged Trace ===")
            compile_dir2 = compile_with_trace(
                args.rom,
                os.path.join(output_dir, "compiled_refined"),
                merged_trace,
                args.single_function
            )
            
            if compile_dir2:
                verify_coverage(merged_trace, compile_dir2)
    
    print("\n=== Workflow Complete ===")
    print(f"Output directory: {output_dir}")
    print(f"Build directory: {os.path.join(compile_dir, 'build')}")
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
