/*
 * What the sound microcode plays, measured the way a listener would hear it.
 *
 * gc_ax_test checks the arithmetic case by case. This one builds a small but
 * complete scene - a stereo music stream looping in audio memory, the same
 * stream on a second pair of voices at a lower volume, and a sound effect in
 * ADPCM that starts and ends three times - runs three seconds of command
 * lists through it, and holds what comes out to the properties clean sound
 * has (tools/gc/audio_check.py holds a game's output to the same ones):
 *
 *  - nothing pinned at the rails and no clicks;
 *  - a tonal spectrum, little of it above 10 kHz;
 *  - the level the volumes say, within a decibel and a half.
 *
 * Each is one way this has been wrong. Main memory under the sample addresses
 * is filled with noise here, the way a game's code and data fill it, so a
 * decoder reading samples from main memory instead of audio memory - the bug
 * that played Shrek 2 as static - fails the first two. A send volume read as
 * signed plays the unity pair inverted against the other and fails the third.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc.h"
#include "gc_ax.h"

#define PI 3.14159265358979323846

#define LIST_AT 0x00300000u
#define SETUP_AT 0x00300100u
#define STEREO_AT 0x00300200u
#define SURROUND_AT 0x00300600u
#define VOICES_AT 0x00310000u
#define PB_BYTES 244u

/* Audio memory: the stream's two channels and the effect. */
#define STREAM_RATE 22050u
#define STREAM_L_AT 0x00100000u /* bytes */
#define STREAM_R_AT 0x00180000u
#define EFFECT_AT 0x00200000u

#define RATE 32000u
#define LISTS 600u /* three seconds */

enum {
  PB_NEXT = 0, PB_SRC_TYPE = 4, PB_MIXER_CONTROL = 6, PB_RUNNING = 7, PB_IS_STREAM = 8, PB_SENDS = 9,
  PB_CUR_VOLUME = 50, PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_ADDR = 57, PB_END_ADDR = 59, PB_CUR_ADDR = 61,
  PB_COEFS = 63, PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82, PB_RATIO = 83, PB_FRAC = 85,
  PB_LAST = 86, PB_LOOP_PRED_SCALE = 90
};

static unsigned failures;

static void check(int ok, const char *what) {
  printf("gc_ax_sound_test: %s %s\n", ok ? "ok    " : "FAILED", what);
  if (!ok) failures++;
}

static void put16(GcMachine *m, uint32_t address, uint16_t value) {
  uint8_t *p = gc_machine_ram(m, address, 2);
  p[0] = (uint8_t)(value >> 8);
  p[1] = (uint8_t)value;
}

static void put32(GcMachine *m, uint32_t address, uint32_t value) {
  put16(m, address, (uint16_t)(value >> 16));
  put16(m, address + 2u, (uint16_t)value);
}

static uint16_t get16(GcMachine *m, uint32_t address) {
  const uint8_t *p = gc_machine_ram(m, address, 2);
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t voice_at(unsigned index) { return VOICES_AT + index * PB_BYTES; }

static void pb(GcMachine *m, unsigned voice, unsigned field, uint16_t value) {
  put16(m, voice_at(voice) + field * 2u, value);
}

static void pb32(GcMachine *m, unsigned voice, unsigned field, uint32_t value) {
  pb(m, voice, field, (uint16_t)(value >> 16));
  pb(m, voice, field + 1u, (uint16_t)value);
}

/* The music: a chord whose every partial fits a whole number of cycles into
 * the one-second buffer, so the loop is seamless and any discontinuity in the
 * output is the mixer's. */
static int16_t chord(unsigned channel, unsigned i) {
  const double t = (double)i / STREAM_RATE;
  const double base = channel ? 330.0 : 220.0;
  return (int16_t)(3000.0 * sin(2 * PI * base * t) + 2000.0 * sin(2 * PI * 1.5 * base * t) +
                   1000.0 * sin(2 * PI * 2.0 * base * t));
}

static void stream_voice(GcMachine *m, unsigned voice, uint32_t bytes_at, uint16_t mixer, unsigned send,
                         uint16_t volume) {
  unsigned f;
  for (f = 0; f < 122u; f++) pb(m, voice, f, 0);
  pb(m, voice, PB_SRC_TYPE, 1);
  pb(m, voice, PB_MIXER_CONTROL, mixer);
  pb(m, voice, PB_RUNNING, 1);
  pb(m, voice, PB_IS_STREAM, 1);
  pb(m, voice, PB_SENDS + send * 2u, volume);
  pb(m, voice, PB_CUR_VOLUME, 0x7fff);
  pb(m, voice, PB_LOOPING, 1);
  pb(m, voice, PB_FORMAT, 0x000a);
  pb32(m, voice, PB_LOOP_ADDR, bytes_at / 2u);
  pb32(m, voice, PB_END_ADDR, bytes_at / 2u + STREAM_RATE - 1u);
  pb32(m, voice, PB_CUR_ADDR, bytes_at / 2u);
  pb(m, voice, PB_GAIN, 0x0800);
  pb32(m, voice, PB_RATIO, (uint32_t)((double)STREAM_RATE / RATE * 65536.0 + 0.5));
}

/* ------------------------------------------------------------ the effect */

/* A decaying 1 kHz tone, encoded to ADPCM here by trying every scale on every
 * frame against the decoder's own arithmetic. One coefficient pair, the
 * resonator for that frequency. */
#define EFFECT_SAMPLES 8400u /* 600 frames of 14 */
static const int16_t k_coef[2] = {4017, -2048};

static uint32_t encode_effect(GcMachine *m) {
  unsigned frame, i;
  int32_t yn1 = 0, yn2 = 0;
  for (frame = 0; frame < EFFECT_SAMPLES / 14u; frame++) {
    int best_scale = 0;
    int64_t best_error = -1;
    uint8_t best_nibbles[14];
    int32_t best_y1 = 0, best_y2 = 0;
    int scale;
    for (scale = 0; scale < 12; scale++) {
      int32_t y1 = yn1, y2 = yn2;
      int64_t error = 0;
      uint8_t nibbles[14];
      for (i = 0; i < 14u; i++) {
        const unsigned n = frame * 14u + i;
        const double t = (double)n / RATE;
        const int32_t x = (int32_t)(12000.0 * exp(-t * 20.0) * sin(2 * PI * 1000.0 * t));
        const int32_t prediction = (0x400 + k_coef[0] * y1 + k_coef[1] * y2) >> 11;
        int32_t residual = (int32_t)lround((double)(x - prediction) / (double)(1 << scale));
        int32_t out;
        residual = residual > 7 ? 7 : residual < -8 ? -8 : residual;
        out = residual * (1 << scale) + prediction;
        out = out > 32767 ? 32767 : out < -32768 ? -32768 : out;
        error += (int64_t)(x - out) * (x - out);
        nibbles[i] = (uint8_t)(residual & 0xf);
        y2 = y1;
        y1 = out;
      }
      if (best_error < 0 || error < best_error) {
        best_error = error;
        best_scale = scale;
        memcpy(best_nibbles, nibbles, sizeof nibbles);
        best_y1 = y1;
        best_y2 = y2;
      }
    }
    m->aram[EFFECT_AT + frame * 8u] = (uint8_t)best_scale; /* coefficient pair 0 */
    for (i = 0; i < 14u; i++) {
      uint8_t *byte = &m->aram[EFFECT_AT + frame * 8u + 1u + i / 2u];
      if (i % 2u == 0u) *byte = (uint8_t)(best_nibbles[i] << 4);
      else *byte = (uint8_t)(*byte | best_nibbles[i]);
    }
    yn1 = best_y1;
    yn2 = best_y2;
  }
  return EFFECT_SAMPLES / 14u;
}

static void start_effect(GcMachine *m, unsigned voice, unsigned frames) {
  unsigned f;
  for (f = 0; f < 122u; f++) pb(m, voice, f, 0);
  pb(m, voice, PB_SRC_TYPE, 1);
  pb(m, voice, PB_MIXER_CONTROL, 0x0003);
  pb(m, voice, PB_RUNNING, 1);
  pb(m, voice, PB_SENDS + 0u, 0x6000);
  pb(m, voice, PB_SENDS + 2u, 0x6000);
  pb(m, voice, PB_CUR_VOLUME, 0x7fff);
  pb(m, voice, PB_FORMAT, 0x0000);
  pb32(m, voice, PB_LOOP_ADDR, EFFECT_AT * 2u + 2u);
  pb32(m, voice, PB_END_ADDR, EFFECT_AT * 2u + frames * 16u - 1u);
  pb32(m, voice, PB_CUR_ADDR, EFFECT_AT * 2u + 2u);
  pb(m, voice, PB_COEFS + 0u, (uint16_t)k_coef[0]);
  pb(m, voice, PB_COEFS + 1u, (uint16_t)k_coef[1]);
  pb(m, voice, PB_PRED_SCALE, m->aram[EFFECT_AT]);
  pb32(m, voice, PB_RATIO, 0x00010000u);
}

/* ------------------------------------------------------------- measuring */

static void fft(double *re, double *im, unsigned n) {
  unsigned i, j, len;
  for (i = 1, j = 0; i < n; i++) {
    unsigned bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      double t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (len = 2; len <= n; len <<= 1) {
    const double angle = -2 * PI / len;
    for (i = 0; i < n; i += len) {
      for (j = 0; j < len / 2u; j++) {
        const double wr = cos(angle * j), wi = sin(angle * j);
        const double ur = re[i + j], ui = im[i + j];
        const double vr = re[i + j + len / 2u] * wr - im[i + j + len / 2u] * wi;
        const double vi = re[i + j + len / 2u] * wi + im[i + j + len / 2u] * wr;
        re[i + j] = ur + vr; im[i + j] = ui + vi;
        re[i + j + len / 2u] = ur - vr; im[i + j + len / 2u] = ui - vi;
      }
    }
  }
}

static int compare_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* Second-difference outliers, as audio_check.py counts them. */
static unsigned count_clicks(const int16_t *frames, unsigned count, unsigned channel) {
  enum { BLOCK = 256 };
  const unsigned blocks = count / BLOCK;
  double *rms = calloc(blocks + 1u, sizeof(double));
  unsigned b, i, clicks = 0;
  long last_hit = -1000;
  for (b = 0; b < blocks; b++) {
    double sum = 0;
    for (i = b * BLOCK + 2u; i < (b + 1u) * BLOCK && i < count; i++) {
      const double d2 = frames[i * 2 + channel] - 2.0 * frames[(i - 1) * 2 + channel] + frames[(i - 2) * 2 + channel];
      sum += d2 * d2;
    }
    rms[b] = sqrt(sum / BLOCK);
  }
  for (i = 2; i < blocks * BLOCK; i++) {
    const double d2 = fabs(frames[i * 2 + channel] - 2.0 * frames[(i - 1) * 2 + channel] +
                           frames[(i - 2) * 2 + channel]);
    double near[9];
    unsigned k, n = 0;
    const long here = (long)(i / BLOCK);
    for (k = 0; k < 9u; k++) {
      const long at = here + (long)k - 4;
      if (at >= 0 && at < (long)blocks) near[n++] = rms[at];
    }
    qsort(near, n, sizeof near[0], compare_double);
    if (d2 > 3000.0 && d2 > 8.0 * (near[n / 2] > 50.0 ? near[n / 2] : 50.0)) {
      if ((long)i - last_hit > 32) clicks++;
      last_hit = (long)i;
    }
  }
  free(rms);
  return clicks;
}

int main(void) {
  GcMachine *m = gc_machine_create();
  int16_t *out = calloc(LISTS * 160u * 2u, sizeof(int16_t));
  unsigned i, list, at = 0, frames, rails = 0, run = 0, clip_runs = 0;
  uint32_t state = 12345;
  char line[256];
  if (!m || !out) return 1;

  /* Main memory under the sample addresses: noise, as code and data are. */
  for (i = 0x00100000u; i < 0x00280000u; i++) {
    state = state * 1664525u + 1013904223u;
    *gc_machine_ram(m, i, 1) = (uint8_t)(state >> 24);
  }
  for (i = 0; i < STREAM_RATE; i++) {
    const int16_t l = chord(0, i), r = chord(1, i);
    m->aram[STREAM_L_AT + i * 2u] = (uint8_t)((uint16_t)l >> 8);
    m->aram[STREAM_L_AT + i * 2u + 1u] = (uint8_t)l;
    m->aram[STREAM_R_AT + i * 2u] = (uint8_t)((uint16_t)r >> 8);
    m->aram[STREAM_R_AT + i * 2u + 1u] = (uint8_t)r;
  }
  frames = encode_effect(m);

  /* Voices 0-1: the stream at unity (0x8000). Voices 2-3: the same stream at
   * half (0x4000), so the pair adds up to one and a half - or, with unity
   * read as -1, to minus a half. Voice 4: the effect. */
  stream_voice(m, 0, STREAM_L_AT, 0x0001, 0, 0x8000);
  stream_voice(m, 1, STREAM_R_AT, 0x0002, 1, 0x8000);
  stream_voice(m, 2, STREAM_L_AT, 0x0001, 0, 0x4000);
  stream_voice(m, 3, STREAM_R_AT, 0x0002, 1, 0x4000);
  for (i = 0; i < 4u; i++) pb32(m, i, PB_NEXT, voice_at(i + 1u));
  start_effect(m, 4, frames);

  for (i = 0; i < 27u; i++) put16(m, SETUP_AT + i * 2u, 0);
  put16(m, LIST_AT + at++ * 2u, 0x0000); put32(m, LIST_AT + at * 2u, SETUP_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x0002); put32(m, LIST_AT + at * 2u, VOICES_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x0003);
  put16(m, LIST_AT + at++ * 2u, 0x000e); put32(m, LIST_AT + at * 2u, SURROUND_AT); at += 2u;
  put32(m, LIST_AT + at * 2u, STEREO_AT); at += 2u;
  put16(m, LIST_AT + at++ * 2u, 0x000f);

  for (list = 0; list < LISTS; list++) {
    if (list == 100u || list == 300u || list == 500u) start_effect(m, 4, frames);
    if (gc_ax_run_list(m, LIST_AT, at) != 0) {
      check(0, "a command list ends at its END");
      break;
    }
    for (i = 0; i < 160u; i++) {
      out[(list * 160u + i) * 2u] = (int16_t)get16(m, STEREO_AT + i * 4u + 2u); /* left */
      out[(list * 160u + i) * 2u + 1u] = (int16_t)get16(m, STEREO_AT + i * 4u);  /* right */
    }
  }
  {
    FILE *dump = getenv("GC_AX_SOUND_DUMP") ? fopen(getenv("GC_AX_SOUND_DUMP"), "wb") : NULL;
    if (dump) {
      fwrite(out, sizeof(int16_t), LISTS * 160u * 2u, dump);
      fclose(dump);
    }
  }

  /* At the rails. */
  for (i = 0; i < LISTS * 160u * 2u; i++) {
    const int pinned = out[i] >= 32767 || out[i] <= -32767;
    rails += (unsigned)pinned;
    run = pinned ? run + 1u : 0u;
    if (run == 3u) clip_runs++;
  }
  snprintf(line, sizeof line, "%u runs pinned at the rails, %u samples (none)", clip_runs, rails);
  check(clip_runs == 0u, line);

  {
    const unsigned clicks = count_clicks(out, LISTS * 160u, 0) + count_clicks(out, LISTS * 160u, 1);
    snprintf(line, sizeof line, "%u clicks in three seconds, three sounds starting and ending (at most 3)", clicks);
    check(clicks <= 3u, line);
  }

  /* The spectrum, over the half-second windows between the effects. */
  {
    enum { N = 2048 };
    double re[N], im[N], flat[64], high[64];
    unsigned windows = 0, w;
    for (w = 0; w + N <= LISTS * 160u && windows < 64u; w += N) {
      double sum_log = 0, sum = 0, above = 0, level = 0;
      for (i = 0; i < N; i++) {
        const double x = (out[(w + i) * 2u] + out[(w + i) * 2u + 1u]) / 65536.0;
        re[i] = x * (0.5 - 0.5 * cos(2 * PI * i / (N - 1)));
        im[i] = 0;
        level += x * x;
      }
      if (level / N < 1e-6) continue;
      fft(re, im, N);
      for (i = 1; i < N / 2; i++) {
        const double power = re[i] * re[i] + im[i] * im[i] + 1e-20;
        sum_log += log(power);
        sum += power;
        if (i * (double)RATE / N > 10000.0) above += power;
      }
      flat[windows] = exp(sum_log / (N / 2 - 1)) / (sum / (N / 2 - 1));
      high[windows] = above / sum;
      windows++;
    }
    qsort(flat, windows, sizeof flat[0], compare_double);
    qsort(high, windows, sizeof high[0], compare_double);
    snprintf(line, sizeof line, "spectral flatness %.4f over %u windows (at most 0.05; noise is about 0.56)",
             windows ? flat[windows / 2] : 1.0, windows);
    check(windows > 20u && flat[windows / 2] <= 0.05, line);
    snprintf(line, sizeof line, "%.3f%% of the power above 10 kHz (at most 0.5%%)",
             windows ? high[windows / 2] * 100.0 : 100.0);
    check(windows > 20u && high[windows / 2] <= 0.005, line);
  }

  /* The level of the music alone, in the last quarter second (the effect has
   * decayed by then): the left channel's chord at 1.5 times, through the
   * envelope's 0x7fff. */
  {
    double got = 0, want = 0;
    const unsigned from = LISTS * 160u - RATE / 4u;
    for (i = from; i < LISTS * 160u; i++) {
      const double x = out[i * 2u];
      got += x * x;
    }
    for (i = 0; i < STREAM_RATE; i++) {
      const double x = chord(0, i) * 1.5;
      want += x * x;
    }
    got = sqrt(got / (LISTS * 160u - from));
    want = sqrt(want / STREAM_RATE) * (32767.0 / 32768.0);
    snprintf(line, sizeof line, "the music's level is %.1f dB from what its volumes add up to (within 1.5)",
             20 * log10((got + 1e-9) / want));
    check(fabs(20 * log10((got + 1e-9) / want)) <= 1.5, line);
  }

  gc_machine_destroy(m);
  free(out);
  if (failures) {
    fprintf(stderr, "gc_ax_sound_test: %u checks failed\n", failures);
    return 1;
  }
  printf("gc_ax_sound_test: the mix is clean\n");
  return 0;
}
