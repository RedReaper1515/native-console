#ifndef GC_AUDIO_OUT_H
#define GC_AUDIO_OUT_H

#include <cstdint>

/*
 * The frontend's end of the sound: what the machine played, taken once a
 * field, converted to the device's rate and handed to the device from a ring
 * that the device's own thread reads.
 *
 * Why a ring and a callback rather than SDL's queue. A queue can only be
 * topped up; it cannot say what happens when it runs out, and what happens
 * then is SDL playing silence straight after the last sample - a step, which
 * is a click, and a few of those a second is static. Reading the ring from the
 * callback lets the last few milliseconds before a gap fade out and the first
 * few after it fade in, and lets a gap last until there is enough to play
 * steadily again instead of a stutter of one-field fragments.
 *
 * It also makes the latency a number this file owns: how much is kept ahead
 * of the device (the target, a few tens of milliseconds, raised only when the
 * machine keeps up and the device still runs dry), the device's own buffer,
 * and nothing else.
 */

struct GcAudioOut;

struct GcAudioOutStats {
    const char *driver;
    int device_rate;
    int device_frames;      /* the callback's chunk: the device buffer SDL asks for */
    double target_ms;       /* what the ring is held at now */
    double target_start_ms; /* and what it started at */
    double target_max_ms;   /* the highest it was raised to */
    double fill_mean_ms;    /* ring fill after each push */
    double fill_min_ms;
    uint64_t pushes;
    uint64_t underruns;     /* times the device found less than a chunk while playing */
    double dry_ms;          /* silence played because the ring was empty, after the first push */
    uint64_t dropped_frames; /* thrown away because the ring was far over its target */
    double callback_period_mean_ms, callback_period_max_ms;
};

/* Opens the default output device, or returns null when there is none. */
GcAudioOut *gc_audio_out_open(void);
void gc_audio_out_close(GcAudioOut *out);

/* Frames the machine played - `frames` interleaved stereo pairs at `rate` -
 * after the frontend's own volume. `behind` says the machine is running late
 * against the field clock, which is when running dry says nothing about the
 * buffer being too small. */
void gc_audio_out_push(GcAudioOut *out, const int16_t *samples, unsigned frames, unsigned rate, bool behind);

/* Silence for one field, for a machine whose sound engine is not running:
 * nothing is wrong, and the device should not be told it ran dry. */
void gc_audio_out_push_silence(GcAudioOut *out, unsigned frames, unsigned rate);

/* Held still while the menu is open; emptied when a state is loaded, since
 * what was queued belongs to the moment the game left. */
void gc_audio_out_pause(GcAudioOut *out, bool paused);
void gc_audio_out_clear(GcAudioOut *out);

/* How far the ring is from its target as a fraction of it, smoothed, -1..4:
 * what the field clock is steered by (0.5 % a unit), so the device's clock
 * and the counter's cannot drift apart. */
double gc_audio_out_steer(GcAudioOut *out);

double gc_audio_out_queued_ms(const GcAudioOut *out);
void gc_audio_out_stats(const GcAudioOut *out, GcAudioOutStats *stats);

#endif
