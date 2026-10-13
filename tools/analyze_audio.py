
import struct
import wave
import os
import math

RAW_FILE = "debug_audio.raw"
WAV_FILE = "debug_audio.wav"
SAMPLE_RATE = 44100
CHANNELS = 2
SAMPLE_WIDTH = 2 # 16-bit

def main():
    if not os.path.exists(RAW_FILE):
        print(f"Error: {RAW_FILE} not found. Run the game for at least a few seconds first.")
        return

    print(f"Processing {RAW_FILE}...")
    
    with open(RAW_FILE, "rb") as f_in:
        raw_data = f_in.read()

    num_samples = len(raw_data) // (CHANNELS * SAMPLE_WIDTH)
    print(f"Read {len(raw_data)} bytes ({num_samples} samples)")

    # Analyze
    max_val = 0
    min_val = 0
    rms_sum = 0
    
    # Simple parsing to find peaks (slow for large files, but fine for 10s)
    # 16-bit signed is 'h'
    fmt = f"<{num_samples*CHANNELS}h"
    try:
        samples = struct.unpack(fmt, raw_data)
        
        for s in samples:
            if s > max_val: max_val = s
            if s < min_val: min_val = s
            rms_sum += s * s
            
        rms = math.sqrt(rms_sum / len(samples))
        
        print(f"Analysis:")
        print(f"  Peak Amplitude: {max_val} / {min_val}")
        print(f"  RMS Level: {rms:.2f}")
        
        if max_val == 0 and min_val == 0:
            print("  WARNING: Audio is completely silent.")
            
    except Exception as e:
        print(f"Analysis failed: {e}")

    # Write WAV
    print(f"Writing {WAV_FILE}...")
    with wave.open(WAV_FILE, "wb") as f_out:
        f_out.setnchannels(CHANNELS)
        f_out.setsampwidth(SAMPLE_WIDTH)
        f_out.setframerate(SAMPLE_RATE)
        f_out.writeframes(raw_data)
        
    print("Done. You can now play debug_audio.wav in any media player.")

if __name__ == "__main__":
    main()
