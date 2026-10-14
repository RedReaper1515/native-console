/*
 * The frontend's sound output. See gc_audio_out.h for why it is a ring read
 * from the device's callback, and docs/GC-PLAN.md ("Clean sound") for the
 * measurements that set the numbers below.
 */
#include "gc_audio_out.h"

#include <SDL.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

/* What is asked of the device: its own rate is fine (the stream converts),
 * and so is its own chunk size if it has a preference - WASAPI's shared-mode
 * period is ten milliseconds and asking for something else only adds a
 * buffer in between. 512 frames at 48 kHz is 10.7 ms. */
constexpr int kWantRate = 48000;
constexpr int kWantFrames = 512;

/* How much is kept ahead of the device, measured right after a field's sound
 * is pushed. The ring falls by a field's worth (16.7 ms) and a device chunk
 * before the next push, so 40 ms leaves about 13 ms of margin for a field that
 * is late - and averages about 30 ms in the ring. Raised in steps when the
 * device runs dry while the machine is keeping up, never past kTargetMaxMs,
 * and eased back when that stops. */
constexpr double kTargetStartMs = 40.0;
constexpr double kTargetStepMs = 8.0;
constexpr double kTargetMaxMs = 104.0;
constexpr double kTargetEaseMs = 4.0;
constexpr double kEaseAfterSeconds = 20.0;

/* A fade either side of a gap. Three milliseconds turns a step into a slope
 * too gentle to hear as a click and too short to hear as a fade. */
constexpr double kFadeMs = 3.0;

constexpr uint32_t kRingFrames = 1u << 17; /* 2.7 s at 48 kHz */

} // namespace

struct GcAudioOut {
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec have = {};
    SDL_AudioStream *stream = nullptr;
    unsigned stream_rate = 0;

    int16_t ring[kRingFrames * 2] = {};
    std::atomic<uint32_t> write_pos{0}, read_pos{0}; /* running frame counts; the ring index is their low bits */
    std::atomic<uint32_t> target_frames{0};

    /* The callback's own state. */
    bool playing = false;
    uint32_t fade_in_left = 0;
    uint32_t fade_frames = 1;
    std::atomic<bool> primed{false};
    std::atomic<uint64_t> underruns{0}, dry_frames{0}, dropped{0};
    Uint64 last_callback = 0;
    double period_sum = 0.0, period_max = 0.0;
    std::atomic<uint64_t> periods{0};

    /* The main thread's. */
    double target_ms = kTargetStartMs, target_max_ms = kTargetStartMs;
    double steer = 0.0;
    uint64_t seen_underruns = 0;
    uint64_t fields_on_time = 0;     /* consecutive pushes with the machine keeping up */
    double seconds_since_raise = 0.0;
    double fill_sum = 0.0, fill_min = 1e9;
    uint64_t pushes = 0;
    int16_t *scratch = nullptr;
    size_t scratch_bytes = 0;
};

static uint32_t ring_used(const GcAudioOut *out)
{
    return out->write_pos.load(std::memory_order_acquire) - out->read_pos.load(std::memory_order_acquire);
}

static void SDLCALL device_callback(void *userdata, Uint8 *stream, int len)
{
    GcAudioOut *out = (GcAudioOut *)userdata;
    const uint32_t want = (uint32_t)len / 4u;
    int16_t *dst = (int16_t *)stream;

    const Uint64 now = SDL_GetPerformanceCounter();
    if (out->primed.load(std::memory_order_relaxed) && out->last_callback) {
        const double ms = (now - out->last_callback) * 1000.0 / SDL_GetPerformanceFrequency();
        out->period_sum += ms;
        if (ms > out->period_max) out->period_max = ms;
        out->periods.fetch_add(1, std::memory_order_relaxed);
    }
    out->last_callback = now;

    uint32_t read = out->read_pos.load(std::memory_order_relaxed);
    uint32_t avail = out->write_pos.load(std::memory_order_acquire) - read;
    const uint32_t target = out->target_frames.load(std::memory_order_relaxed);

    /* Far too much queued - a machine that ran fast with nobody taking it.
     * Keep the newest, as the machine's own ring does. */
    if (avail > target * 3u + (uint32_t)out->have.freq / 5u) {
        const uint32_t skip = avail - target;
        read += skip;
        avail -= skip;
        out->dropped.fetch_add(skip, std::memory_order_relaxed);
    }

    if (!out->playing) {
        /* After a gap, wait until a steady amount is there. */
        if (avail < target || avail < want) {
            memset(stream, 0, (size_t)len);
            if (out->primed.load(std::memory_order_relaxed)) out->dry_frames.fetch_add(want, std::memory_order_relaxed);
            out->read_pos.store(read, std::memory_order_release);
            return;
        }
        out->playing = true;
        out->fade_in_left = out->fade_frames;
    }

    const uint32_t n = avail < want ? avail : want;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t at = ((read + i) & (kRingFrames - 1u)) * 2u;
        int32_t l = out->ring[at], r = out->ring[at + 1u];
        if (out->fade_in_left) {
            const int32_t g = (int32_t)(((out->fade_frames - out->fade_in_left) << 12) / out->fade_frames);
            l = (l * g) >> 12;
            r = (r * g) >> 12;
            out->fade_in_left--;
        }
        dst[i * 2u] = (int16_t)l;
        dst[i * 2u + 1u] = (int16_t)r;
    }
    read += n;
    if (n < want) {
        /* Ran dry: slope what there was down to nothing, and play silence
         * until the ring has refilled. */
        const uint32_t fade = n < out->fade_frames ? n : out->fade_frames;
        for (uint32_t i = 0; i < fade; i++) {
            const uint32_t k = n - fade + i;
            const int32_t g = (int32_t)(((fade - i) << 12) / (fade + 1u));
            dst[k * 2u] = (int16_t)((dst[k * 2u] * g) >> 12);
            dst[k * 2u + 1u] = (int16_t)((dst[k * 2u + 1u] * g) >> 12);
        }
        memset(dst + n * 2u, 0, (size_t)(want - n) * 4u);
        out->playing = false;
        if (out->primed.load(std::memory_order_relaxed)) {
            out->underruns.fetch_add(1, std::memory_order_relaxed);
            out->dry_frames.fetch_add(want - n, std::memory_order_relaxed);
        }
    }
    out->read_pos.store(read, std::memory_order_release);
}

GcAudioOut *gc_audio_out_open(void)
{
    GcAudioOut *out = new (std::nothrow) GcAudioOut;
    if (!out) return nullptr;
    SDL_AudioSpec want = {};
    want.freq = kWantRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = kWantFrames;
    want.callback = device_callback;
    want.userdata = out;
    out->device = SDL_OpenAudioDevice(NULL, 0, &want, &out->have,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!out->device) {
        delete out;
        return nullptr;
    }
    out->fade_frames = (uint32_t)(out->have.freq * kFadeMs / 1000.0);
    if (out->fade_frames < 1u) out->fade_frames = 1u;
    out->target_frames.store((uint32_t)(out->have.freq * out->target_ms / 1000.0));
    SDL_PauseAudioDevice(out->device, 0);
    return out;
}

void gc_audio_out_close(GcAudioOut *out)
{
    if (!out) return;
    SDL_CloseAudioDevice(out->device);
    if (out->stream) SDL_FreeAudioStream(out->stream);
    free(out->scratch);
    delete out;
}

static void ring_write(GcAudioOut *out, const int16_t *frames, uint32_t count)
{
    uint32_t write = out->write_pos.load(std::memory_order_relaxed);
    const uint32_t used = write - out->read_pos.load(std::memory_order_acquire);
    uint32_t room = kRingFrames - 1u - used;
    if (count > room) {
        out->dropped.fetch_add(count - room, std::memory_order_relaxed);
        count = room;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = ((write + i) & (kRingFrames - 1u)) * 2u;
        out->ring[at] = frames[i * 2u];
        out->ring[at + 1u] = frames[i * 2u + 1u];
    }
    out->write_pos.store(write + count, std::memory_order_release);
}

/* Converts and writes; returns false when there is no stream to be had. */
static bool convert(GcAudioOut *out, const int16_t *samples, unsigned frames, unsigned rate)
{
    if (!out->stream || out->stream_rate != rate) {
        if (out->stream) SDL_FreeAudioStream(out->stream);
        out->stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)rate, out->have.format, out->have.channels,
                                         out->have.freq);
        out->stream_rate = rate;
        if (!out->stream) return false;
    }
    if (frames) SDL_AudioStreamPut(out->stream, samples, (int)(frames * 4u));
    const int ready = SDL_AudioStreamAvailable(out->stream);
    if (ready <= 0) return true;
    if ((size_t)ready > out->scratch_bytes) {
        int16_t *grown = (int16_t *)realloc(out->scratch, (size_t)ready);
        if (!grown) return false;
        out->scratch = grown;
        out->scratch_bytes = (size_t)ready;
    }
    const int got = SDL_AudioStreamGet(out->stream, out->scratch, ready);
    if (got > 0) ring_write(out, out->scratch, (uint32_t)got / 4u);
    return true;
}

static void after_push(GcAudioOut *out, unsigned frames, unsigned rate, bool behind)
{
    out->primed.store(true, std::memory_order_relaxed);
    const double field_seconds = rate ? (double)frames / rate : 0.0;
    const double fill = ring_used(out) * 1000.0 / out->have.freq;
    out->fill_sum += fill;
    if (out->pushes > 60 && fill < out->fill_min) out->fill_min = fill;
    out->pushes++;

    /* Raise the target when the device ran dry although the machine has
     * been keeping up for a while: that is jitter the buffer is too small
     * for. Running dry while the machine is behind is the machine's speed,
     * and a bigger buffer would only add latency. */
    const uint64_t runs = out->underruns.load(std::memory_order_relaxed);
    out->fields_on_time = behind ? 0 : out->fields_on_time + 1;
    out->seconds_since_raise += field_seconds;
    if (runs != out->seen_underruns) {
        if (out->fields_on_time > 30 && out->target_ms < kTargetMaxMs) {
            out->target_ms += kTargetStepMs;
            if (out->target_ms > out->target_max_ms) out->target_max_ms = out->target_ms;
            out->seconds_since_raise = 0.0;
        }
        out->seen_underruns = runs;
    } else if (out->seconds_since_raise > kEaseAfterSeconds && out->target_ms > kTargetStartMs) {
        out->target_ms -= kTargetEaseMs;
        out->seconds_since_raise = 0.0;
    }
    out->target_frames.store((uint32_t)(out->have.freq * out->target_ms / 1000.0), std::memory_order_relaxed);

    /* Short of the target the steer stops at -1 (0.5 % fast): a ring that
     * runs short is ridden out by the callback. Over it, up to 4 (2 % slow),
     * so what a late stretch left queued drains in seconds rather than
     * sitting there as latency for half a minute. */
    double error = (fill - out->target_ms) / out->target_ms;
    error = error > 4.0 ? 4.0 : error < -1.0 ? -1.0 : error;
    out->steer += 0.1 * (error - out->steer);
}

void gc_audio_out_push(GcAudioOut *out, const int16_t *samples, unsigned frames, unsigned rate, bool behind)
{
    if (!out) return;
    convert(out, samples, frames, rate);
    after_push(out, frames, rate, behind);
}

void gc_audio_out_push_silence(GcAudioOut *out, unsigned frames, unsigned rate)
{
    if (!out) return;
    static const int16_t zeros[4096 * 2] = {};
    unsigned left = frames;
    while (left) {
        const unsigned n = left < 4096u ? left : 4096u;
        convert(out, zeros, n, rate);
        left -= n;
    }
    after_push(out, frames, rate, false);
}

void gc_audio_out_pause(GcAudioOut *out, bool paused)
{
    if (out) SDL_PauseAudioDevice(out->device, paused ? 1 : 0);
}

void gc_audio_out_clear(GcAudioOut *out)
{
    if (!out) return;
    SDL_LockAudioDevice(out->device);
    out->read_pos.store(out->write_pos.load());
    out->playing = false;
    SDL_UnlockAudioDevice(out->device);
    if (out->stream) SDL_AudioStreamClear(out->stream);
    out->steer = 0.0;
}

double gc_audio_out_steer(GcAudioOut *out)
{
    return out ? out->steer : 0.0;
}

double gc_audio_out_queued_ms(const GcAudioOut *out)
{
    if (!out) return 0.0;
    double ms = ring_used(out) * 1000.0 / out->have.freq;
    if (out->stream && out->stream_rate) ms += SDL_AudioStreamAvailable(out->stream) / 4.0 * 1000.0 / out->have.freq;
    return ms;
}

void gc_audio_out_stats(const GcAudioOut *out, GcAudioOutStats *s)
{
    memset(s, 0, sizeof *s);
    if (!out) return;
    s->driver = SDL_GetCurrentAudioDriver();
    s->device_rate = out->have.freq;
    s->device_frames = out->have.samples;
    s->target_ms = out->target_ms;
    s->target_start_ms = kTargetStartMs;
    s->target_max_ms = out->target_max_ms;
    s->fill_mean_ms = out->pushes ? out->fill_sum / (double)out->pushes : 0.0;
    s->fill_min_ms = out->fill_min > 1e8 ? 0.0 : out->fill_min;
    s->pushes = out->pushes;
    s->underruns = out->underruns.load();
    s->dry_ms = out->dry_frames.load() * 1000.0 / out->have.freq;
    s->dropped_frames = out->dropped.load();
    const uint64_t periods = out->periods.load();
    s->callback_period_mean_ms = periods ? out->period_sum / (double)periods : 0.0;
    s->callback_period_max_ms = out->period_max;
}
