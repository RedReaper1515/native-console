/*
 * Input scripts: a controller's states by field, as `--input` plays them
 * headless (gc_runner.h describes the format) and the SDL frontend's
 * `--play-input` plays them in a window.
 */
#include "gc_runner.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct GcPadStep {
  uint64_t field;
  GcPad pad;
} GcPadStep;

static int parse_buttons(const char *text, uint16_t *out) {
  static const struct {
    const char *name;
    uint16_t bit;
  } names[] = {
      {"A", GC_PAD_A},       {"B", GC_PAD_B},         {"X", GC_PAD_X},       {"Y", GC_PAD_Y},
      {"Z", GC_PAD_Z},       {"L", GC_PAD_L},         {"R", GC_PAD_R},       {"START", GC_PAD_START},
      {"UP", GC_PAD_UP},     {"DOWN", GC_PAD_DOWN},   {"LEFT", GC_PAD_LEFT}, {"RIGHT", GC_PAD_RIGHT},
  };
  char name[16];
  *out = 0;
  if (!strcmp(text, "-") || !strcmp(text, "none")) return 0;
  while (*text) {
    size_t length = 0, k;
    int found = 0;
    while (text[length] && text[length] != '+') length++;
    if (length == 0 || length >= sizeof(name)) return -1;
    for (k = 0; k < length; k++) name[k] = (char)toupper((unsigned char)text[k]);
    name[length] = 0;
    for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
      if (!strcmp(name, names[k].name)) {
        *out |= names[k].bit;
        found = 1;
      }
    }
    if (!found) return -1;
    text += length;
    if (*text == '+') text++;
  }
  return 0;
}

static int compare_steps(const void *x, const void *y) {
  const GcPadStep *a = (const GcPadStep *)x, *b = (const GcPadStep *)y;
  return a->field < b->field ? -1 : a->field > b->field ? 1 : 0;
}

static int load_steps(const char *name, const char *path, GcPadStep **steps, size_t *count) {
  FILE *file = fopen(path, "r");
  char line[512];
  unsigned number = 0;
  size_t capacity = 0;
  if (!file) {
    fprintf(stderr, "%s: cannot read the input script %s\n", name, path);
    return -1;
  }
  while (fgets(line, sizeof(line), file)) {
    char buttons[128];
    unsigned long long field;
    int values[6] = {0, 0, 0, 0, 0, 0};
    int got;
    char *hash = strchr(line, '#');
    GcPadStep step;
    number++;
    if (hash) *hash = 0;
    got = sscanf(line, "%llu %127s %d %d %d %d %d %d", &field, buttons, &values[0], &values[1], &values[2],
                 &values[3], &values[4], &values[5]);
    if (got <= 0) continue;
    memset(&step, 0, sizeof(step));
    step.field = field;
    step.pad.present = 1;
    if (got < 2 || parse_buttons(buttons, &step.pad.buttons) != 0) {
      fprintf(stderr, "%s: %s line %u: expected FIELD BUTTONS [SX SY [CX CY [L R]]]\n", name, path, number);
      fclose(file);
      return -1;
    }
    step.pad.stick_x = (int8_t)values[0];
    step.pad.stick_y = (int8_t)values[1];
    step.pad.c_stick_x = (int8_t)values[2];
    step.pad.c_stick_y = (int8_t)values[3];
    step.pad.trigger_l = (uint8_t)values[4];
    step.pad.trigger_r = (uint8_t)values[5];
    if (*count == capacity) {
      GcPadStep *grown;
      capacity = capacity ? capacity * 2u : 64u;
      grown = (GcPadStep *)realloc(*steps, capacity * sizeof(GcPadStep));
      if (!grown) {
        fclose(file);
        return -1;
      }
      *steps = grown;
    }
    (*steps)[(*count)++] = step;
  }
  fclose(file);
  if (*steps) qsort(*steps, *count, sizeof(GcPadStep), compare_steps);
  return 0;
}

/* The controller for a field: the last script line at or before it, or a
 * controller at rest before the first. */
static void steps_pad(const GcPadStep *steps, size_t count, uint64_t field, GcPad *pad) {
  size_t low = 0, high = count;
  memset(pad, 0, sizeof(*pad));
  pad->present = 1;
  while (low < high) {
    const size_t mid = (low + high) / 2u;
    if (steps[mid].field <= field) low = mid + 1u;
    else high = mid;
  }
  if (low) *pad = steps[low - 1u].pad;
}

struct GcInputScript {
  GcPadStep *steps;
  size_t count;
};

GcInputScript *gc_input_script_load(const char *name, const char *path) {
  GcInputScript *s = (GcInputScript *)calloc(1, sizeof(GcInputScript));
  if (!s) return NULL;
  if (load_steps(name, path, &s->steps, &s->count) != 0) {
    gc_input_script_free(s);
    return NULL;
  }
  return s;
}

void gc_input_script_pad(const GcInputScript *s, uint64_t field, GcPad *pad) {
  steps_pad(s->steps, s->count, field, pad);
}

void gc_input_script_free(GcInputScript *s) {
  if (!s) return;
  free(s->steps);
  free(s);
}
