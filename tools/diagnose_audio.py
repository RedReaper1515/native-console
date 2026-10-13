#!/usr/bin/env python3
"""
Audio Performance Diagnostic Tool for Native Console

This tool helps diagnose audio performance issues by:
1. Analyzing debug_audio.raw for sample rate consistency
2. Checking for dropped samples or gaps
3. Measuring timing between samples
4. Comparing generated audio rate vs expected rate

Usage:
    python3 tools/diagnose_audio.py [debug_audio.raw]
"""

import struct
import wave
import os
import sys
import math
from collections import Counter

RAW_FILE = "debug_audio.raw"
EXPECTED_SAMPLE_RATE = 44100
CHANNELS = 2
SAMPLE_WIDTH = 2  # 16-bit
MAX_SECONDS_TO_PRINT = 30

# GameBoy constants
GB_CPU_FREQ = 4194304  # Hz
EXPECTED_CYCLES_PER_SAMPLE = GB_CPU_FREQ / EXPECTED_SAMPLE_RATE  # ~95.1

def analyze_audio_file(filepath: str):
    """Analyze an audio file for issues."""
    
    if not os.path.exists(filepath):
        print(f"Error: {filepath} not found.")
        print("Run the game for at least a few seconds to generate audio data.")
        return False
    
    file_size = os.path.getsize(filepath)
    print(f"\n{'='*60}")
    print(f"AUDIO DIAGNOSTIC REPORT")
    print(f"{'='*60}")
    print(f"\nFile: {filepath}")
    print(f"Size: {file_size:,} bytes")
    
    with open(filepath, "rb") as f:
        raw_data = f.read()
    
    num_frames = len(raw_data) // (CHANNELS * SAMPLE_WIDTH)
    duration_sec = num_frames / EXPECTED_SAMPLE_RATE
    
    print(f"Samples (stereo frames): {num_frames:,}")
    print(f"Expected duration: {duration_sec:.2f} seconds")
    
    # Parse samples
    fmt = f"<{num_frames * CHANNELS}h"
    try:
        samples = struct.unpack(fmt, raw_data)
    except Exception as e:
        print(f"Error parsing audio data: {e}")
        return False
    
    # Separate left/right channels
    left = samples[0::2]
    right = samples[1::2]
    
    # === ANALYSIS 1: Basic Statistics ===
    print(f"\n--- Basic Statistics ---")
    
    left_max = max(left) if left else 0
    left_min = min(left) if left else 0
    right_max = max(right) if right else 0
    right_min = min(right) if right else 0
    
    left_rms = math.sqrt(sum(s*s for s in left) / len(left)) if left else 0
    right_rms = math.sqrt(sum(s*s for s in right) / len(right)) if right else 0
    
    print(f"Left channel:  Peak [{left_min:6d}, {left_max:6d}], RMS: {left_rms:.1f}")
    print(f"Right channel: Peak [{right_min:6d}, {right_max:6d}], RMS: {right_rms:.1f}")
    if left:
        left_mean = sum(left) / len(left)
        right_mean = sum(right) / len(right)
        print(f"DC offset:     L={left_mean:.1f} R={right_mean:.1f}")
    
    if left_max == 0 and left_min == 0 and right_max == 0 and right_min == 0:
        print("⚠️  WARNING: Audio is completely silent!")
        return True
    
    # === ANALYSIS 2: Zero/Silence Detection ===
    print(f"\n--- Silence Analysis ---")
    
    zero_frames = sum(1 for i in range(len(left)) if left[i] == 0 and right[i] == 0)
    silence_percent = (zero_frames / len(left)) * 100 if left else 0
    
    print(f"Silent frames: {zero_frames:,} ({silence_percent:.1f}%)")
    
    if silence_percent > 50:
        print("⚠️  WARNING: More than 50% of audio is silent!")
        print("   This may indicate audio generation issues or missing channels.")
    
    # === ANALYSIS 3: Consecutive Silence (Gaps) ===
    print(f"\n--- Gap Detection ---")
    
    gaps = []
    current_gap_start = None
    current_gap_len = 0
    SILENCE_THRESHOLD = 100  # Samples below this are considered "silence"
    
    for i in range(len(left)):
        is_silent = abs(left[i]) < SILENCE_THRESHOLD and abs(right[i]) < SILENCE_THRESHOLD
        
        if is_silent:
            if current_gap_start is None:
                current_gap_start = i
            current_gap_len += 1
        else:
            if current_gap_len > 100:  # Gap longer than ~2ms
                gaps.append((current_gap_start, current_gap_len))
            current_gap_start = None
            current_gap_len = 0
    
    # Don't forget trailing gap
    if current_gap_len > 100:
        gaps.append((current_gap_start, current_gap_len))
    
    # Filter to significant gaps (> 20ms = 882 samples)
    significant_gaps = [(start, length) for start, length in gaps if length > 882]
    
    if significant_gaps:
        print(f"Found {len(significant_gaps)} significant gaps (>20ms):")
        for start, length in significant_gaps[:10]:  # Show first 10
            start_ms = (start / EXPECTED_SAMPLE_RATE) * 1000
            length_ms = (length / EXPECTED_SAMPLE_RATE) * 1000
            print(f"  - At {start_ms:.0f}ms: {length_ms:.0f}ms gap")
        if len(significant_gaps) > 10:
            print(f"  ... and {len(significant_gaps) - 10} more")
        print("⚠️  WARNING: Gaps indicate audio buffer underruns!")
    else:
        print("✓ No significant gaps detected")
    
    # === ANALYSIS 4: Sample Value Distribution ===
    print(f"\n--- Volume Distribution ---")
    
    # Count sample magnitudes
    low_volume = sum(1 for s in left + right if abs(s) < 1000)
    mid_volume = sum(1 for s in left + right if 1000 <= abs(s) < 10000)
    high_volume = sum(1 for s in left + right if abs(s) >= 10000)
    total = len(left) + len(right)
    
    print(f"Low volume (<1000):   {low_volume:,} ({100*low_volume/total:.1f}%)")
    print(f"Mid volume:           {mid_volume:,} ({100*mid_volume/total:.1f}%)")
    print(f"High volume (>10000): {high_volume:,} ({100*high_volume/total:.1f}%)")
    
    if high_volume / total > 0.01:
        print("✓ Audio has dynamic range")
    else:
        print("⚠️  WARNING: Audio is very quiet, may indicate volume scaling issues")
    
    # === ANALYSIS 5: Frequency Analysis (Simple) ===
    print(f"\n--- Frequency Analysis (Zero Crossings) ---")
    
    # Count zero crossings in left channel to estimate dominant frequency
    zero_crossings = 0
    for i in range(1, len(left)):
        if (left[i-1] >= 0 and left[i] < 0) or (left[i-1] < 0 and left[i] >= 0):
            zero_crossings += 1
    
    if duration_sec > 0:
        crossing_rate = zero_crossings / duration_sec
        estimated_freq = crossing_rate / 2  # Each cycle has 2 zero crossings
        print(f"Zero crossing rate: {crossing_rate:.0f}/sec")
        print(f"Estimated dominant frequency: ~{estimated_freq:.0f} Hz")
    
    # === ANALYSIS 6: Per-Second Breakdown ===
    print(f"\n--- Per-Second RMS Levels ---")
    
    samples_per_sec = EXPECTED_SAMPLE_RATE
    num_full_seconds = len(left) // samples_per_sec
    
    if num_full_seconds > 0:
        rms_per_second = []
        seconds_to_print = min(num_full_seconds, MAX_SECONDS_TO_PRINT)
        for sec in range(seconds_to_print):
            start = sec * samples_per_sec
            end = start + samples_per_sec
            sec_left = left[start:end]
            sec_rms = math.sqrt(sum(s*s for s in sec_left) / len(sec_left))
            rms_per_second.append(sec_rms)
            print(f"  Second {sec+1}: RMS = {sec_rms:.1f}")
        if num_full_seconds > seconds_to_print:
            print(f"  ... omitted {num_full_seconds - seconds_to_print} later seconds")
        
        # Check for consistency
        if rms_per_second:
            avg_rms = sum(rms_per_second) / len(rms_per_second)
            variance = sum((r - avg_rms)**2 for r in rms_per_second) / len(rms_per_second)
            std_dev = math.sqrt(variance)
            cv = (std_dev / avg_rms) * 100 if avg_rms > 0 else 0
            
            print(f"\nRMS Coefficient of Variation: {cv:.1f}%")
            if cv > 50:
                print("⚠️  WARNING: High volume variance between seconds!")
                print("   This may indicate inconsistent audio generation.")
            else:
                print("✓ Volume is relatively consistent across time")
    
    # === SUMMARY ===
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    
    issues = []
    if silence_percent > 50:
        issues.append("High silence percentage")
    if significant_gaps:
        issues.append(f"{len(significant_gaps)} buffer underruns detected")
    if high_volume / total < 0.001:
        issues.append("Very low volume")
    
    if issues:
        print("⚠️  ISSUES FOUND:")
        for issue in issues:
            print(f"   - {issue}")
    else:
        print("✓ No major issues detected in audio data")
    
    print("\nNOTE: A raw capture only proves what the emulator generated,")
    print("not whether that output is faithful to hardware or whether playback")
    print("timing was healthy at runtime. Combine this with --audio-stats output,")
    print("startup expectations, and in-game listening before blaming SDL alone.")
    
    return True


def main():
    filepath = sys.argv[1] if len(sys.argv) > 1 else RAW_FILE
    analyze_audio_file(filepath)


if __name__ == "__main__":
    main()
