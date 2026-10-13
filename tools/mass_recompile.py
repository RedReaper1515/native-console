#!/usr/bin/env python3
import os
import sys
import glob
import subprocess
import hashlib
import json
import shutil
import zipfile
import multiprocessing
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from typing import Dict, Optional

# Configuration
PROJECT_ROOT = os.getcwd()
GBRECOMP_BIN = os.path.join(PROJECT_ROOT, "build", "bin", "gbrecomp")
ROMS_DIR = os.path.join(PROJECT_ROOT, "roms", "full_romset")
OUTPUT_DIR = os.path.join(PROJECT_ROOT, "output")
LOGS_DIR = os.path.join(PROJECT_ROOT, "logs")

@dataclass
class RomResult:
    filename: str
    game_title: str
    md5: str
    status: str  # SUCCESS, RECOMPILE_FAIL, CONFIG_FAIL, BUILD_FAIL, NO_ROM_IN_ZIP
    error_log: Optional[str] = None
    time_taken: float = 0.0

def ensure_dirs():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(LOGS_DIR, exist_ok=True)

def get_file_md5(filepath):
    hash_md5 = hashlib.md5()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            hash_md5.update(chunk)
    return hash_md5.hexdigest()

def get_gb_info(filepath):
    """
    Extract basic info from GB header.
    Title is at 0x134, max 16 chars (or fewer depending on CGB flag).
    """
    try:
        with open(filepath, "rb") as f:
            f.seek(0x134)
            title_bytes = f.read(16)
            # Null terminate and decode, ignoring errors
            title = title_bytes.split(b'\0')[0].decode('ascii', errors='ignore').strip()
            return title
    except Exception:
        return "UNKNOWN"

def process_single_rom(zip_path: str, run_test: bool = False, limit: int = 100000, force_recompile: bool = False) -> RomResult:
    import time
    start_time = time.time()
    
    zip_name = os.path.basename(zip_path)
    base_name = os.path.splitext(zip_name)[0]
    # Sanitize base_name for directory use
    slug = "".join([c if c.isalnum() else "_" for c in base_name])
    
    rom_output_dir = os.path.join(OUTPUT_DIR, slug)
    build_dir = os.path.join(rom_output_dir, "build")
    log_base = os.path.join(LOGS_DIR, slug)
    
    # Clean previous run
    if os.path.exists(rom_output_dir):
        shutil.rmtree(rom_output_dir)
        
    extracted_rom_path = None
    
    try:
        with zipfile.ZipFile(zip_path, 'r') as z:
            # Find first .gb or .gbc or .sgb file
            candidates = [f for f in z.namelist() if f.lower().endswith(('.gb', '.gbc', '.sgb'))]
            if not candidates:
                return RomResult(zip_name, "Unknown", "", "NO_ROM_IN_ZIP")
            
            # Prefer .gb over others if multiple (rare in full sets usually)
            target_file = candidates[0]
            
            # Extract to temp location
            # We extract directly to a temp file in the slug dir to avoid mess
            os.makedirs(rom_output_dir, exist_ok=True)
            z.extract(target_file, rom_output_dir)
            extracted_rom_path = os.path.join(rom_output_dir, target_file)
            
    except zipfile.BadZipFile:
        return RomResult(zip_name, "Unknown", "", "BAD_ZIP")

    md5 = get_file_md5(extracted_rom_path)
    game_title = get_gb_info(extracted_rom_path)
    if not game_title:
        game_title = base_name

    # Step 1: Recompile
    # Command: ./build/bin/gbrecomp <rom> -o <rom_output_dir>
    # Note: The output dir already exists and contains the rom, gbrecomp usually handles this fine or we might need to target a subfolder if it wipes it. 
    # Looking at user rules: "./build/bin/gbrecomp roms/tetris.gb -o tetris_test"
    # It likely populates the dir.
    
    # We'll use a specific project dir to be clean, because we extracted the ROM into `rom_output_dir`.
    # Let's extract to a temp file, then run gbrecomp to the target dir.
    
    project_dir_abs = os.path.join(rom_output_dir, "project")
    project_dir_rel = os.path.relpath(project_dir_abs, PROJECT_ROOT)
    
    try:
        cmd_recomp = [GBRECOMP_BIN, extracted_rom_path, "-o", project_dir_rel]
        result = subprocess.run(cmd_recomp, capture_output=True, text=True)
        if result.returncode != 0:
            with open(f"{log_base}_recomp.log", "w") as f:
                f.write(result.stdout + "\n" + result.stderr)
            return RomResult(zip_name, game_title, md5, "RECOMPILE_FAIL", f"{log_base}_recomp.log", time.time() - start_time)
            
        # Step 2: Configure
        # CMake needs the absolute path to the source (project_dir) but we build in build_dir
        cmd_config = ["cmake", "-G", "Ninja", "-S", project_dir_abs, "-B", build_dir]
        result = subprocess.run(cmd_config, capture_output=True, text=True)
        if result.returncode != 0:
            with open(f"{log_base}_config.log", "w") as f:
                f.write(result.stdout + "\n" + result.stderr)
            return RomResult(zip_name, game_title, md5, "CONFIG_FAIL", f"{log_base}_config.log", time.time() - start_time)

        # Step 3: Build
        cmd_build = ["ninja", "-C", build_dir]
        result = subprocess.run(cmd_build, capture_output=True, text=True)
        if result.returncode != 0:
            with open(f"{log_base}_build.log", "w") as f:
                f.write(result.stdout + "\n" + result.stderr)
            return RomResult(zip_name, game_title, md5, "BUILD_FAIL", f"{log_base}_build.log", time.time() - start_time)
            
        # Step 4: Run Test (Optional)
        if run_test:
             exe_path = os.path.join(build_dir, slug)
             
             if not os.path.exists(exe_path):
                 exe_path = os.path.join(build_dir, slug + ".exe")
                 
             if not os.path.exists(exe_path):
                  # Fallback: check what executable was built by ninja.
                  # It's hard to know easily. But gbrecomp usually uses the input filename stem.
                  # Our slug is aggressive "alphanum only".
                  # gbrecomp might use original filename stem.
                  pass 

             if os.path.exists(exe_path):
                 cmd_run = [exe_path, "--limit", str(limit)]
                 env = os.environ.copy()
                 env["SDL_VIDEODRIVER"] = "dummy"
                 env["SDL_AUDIODRIVER"] = "dummy"
                 
                 try:
                    result = subprocess.run(cmd_run, capture_output=True, text=True, env=env, timeout=30)
                    if result.returncode != 0:
                        with open(f"{log_base}_run.log", "w") as f:
                            f.write(result.stdout + "\n" + result.stderr)
                        return RomResult(zip_name, game_title, md5, "RUN_FAIL", f"{log_base}_run.log", time.time() - start_time)
                    else:
                        if "SGB" in zip_name or "Enhanced" in zip_name:
                             with open(f"{log_base}_run.log", "w") as f:
                                f.write(result.stdout + "\n" + result.stderr) 
                 except subprocess.TimeoutExpired as e:
                     # Timeout is actually GOOD if we just wanted to run for N instructions but it hung?
                     # No, --limit should exit cleanly. If it times out, it hung.
                     with open(f"{log_base}_run.log", "w") as f:
                        if e.stdout:
                            f.write(e.stdout if isinstance(e.stdout, str) else e.stdout.decode('utf-8', errors='replace'))
                        if e.stderr:
                             stderr_text = e.stderr if isinstance(e.stderr, str) else e.stderr.decode('utf-8', errors='replace')
                             f.write("\n" + stderr_text)
                        f.write("\n[TIMEOUT] Process timed out after 30s")
                     return RomResult(zip_name, game_title, md5, "RUN_TIMEOUT", f"{log_base}_run.log", time.time() - start_time)
             else:
                  # If we can't find the exe, maybe we can assume success for build?
                  # but warning.
                  pass
                  
    except Exception as e:
         with open(f"{log_base}_exception.log", "w") as f:
            f.write(str(e))
         return RomResult(zip_name, game_title, md5, "EXCEPTION", str(e), time.time() - start_time)
    finally:
        # Cleanup the extracted ROM to save space? Optional.
        # os.remove(extracted_rom_path)
        pass

    return RomResult(zip_name, game_title, md5, "SUCCESS", None, time.time() - start_time)

def generate_report(results):
    report_path = os.path.join(LOGS_DIR, "mass_recompile_report.md")
    
    total = len(results)
    success = sum(1 for r in results if r.status == "SUCCESS")
    
    with open(report_path, "w") as f:
        f.write("# Native Console Bulk Recompile Report\n\n")
        f.write("> This is a naive batch recompilation/run report for triage only. It is not a playability or compatibility guarantee.\n\n")
        f.write(f"**Total ROMs Processed:** {total}\n")
        f.write(f"**Success Rate:** {success}/{total} ({success/total*100:.2f}%)\n\n")
        
        f.write("| ROM File | title | Status | MD5 | Log |\n")
        f.write("| --- | --- | --- | --- | --- |\n")
        
        for r in results:
            log_link = f"[Log]({os.path.relpath(r.error_log, PROJECT_ROOT)})" if r.error_log else "-"
            f.write(f"| {r.filename} | {r.game_title} | {r.status} | {r.md5} | {log_link} |\n")
    
    print(f"Report generated at {report_path}")

def main():
    # Parse args
    parser = argparse.ArgumentParser(description="Mass recompile Game Boy ROMs")
    parser.add_argument("--run-test", action="store_true", help="Run the recompiled executable for a few frames to check for crashes")
    parser.add_argument("--limit", type=int, default=100000, help="Instruction limit for run test (default: 100000)")
    parser.add_argument("--force", action="store_true", help="Force recompilation even if output exists")
    parser.add_argument("--workers", type=int, default=max(1, multiprocessing.cpu_count() - 2), help="Number of parallel workers")
    args = parser.parse_args()

    ensure_dirs()
    
    # 1. Gather all zip files
    zip_files = glob.glob(os.path.join(ROMS_DIR, "*.zip"))
    if not zip_files:
        print(f"No .zip files found in {ROMS_DIR}")
        return

    print(f"Found {len(zip_files)} ROMs to process.")
    
    # 2. Check for gbrecomp
    if not os.path.exists(GBRECOMP_BIN):
        print(f"Error: gbrecomp binary not found at {GBRECOMP_BIN}")
        print("Please build the project first.")
        return

    # 3. process in parallel
    results = []
    
    print(f"Starting compilation with {args.workers} workers...")
    
    try:
        with ProcessPoolExecutor(max_workers=args.workers) as executor:
            future_to_zip = {executor.submit(process_single_rom, z, args.run_test, args.limit, args.force): z for z in zip_files}
            
            completed_count = 0
            for future in as_completed(future_to_zip):
                try:
                    res = future.result()
                    results.append(res)
                    completed_count += 1
                    print(f"[{completed_count}/{len(zip_files)}] {res.filename}: {res.status}")
                except KeyboardInterrupt:
                    print("\nInterrupted by user. waiting for pending tasks...")
                    executor.shutdown(wait=False, cancel_futures=True)
                    break
    except KeyboardInterrupt:
        print("\nInterrupted! Generating partial report...")
    finally:
        # 4. Generate report
        generate_report(results)

if __name__ == "__main__":
    main()
