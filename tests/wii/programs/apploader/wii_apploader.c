/*
 * An apploader of our own, for the test disc.
 *
 * Every disc carries a small program whose job is to load the game: the
 * console calls it for three functions, calls the second until it says it is
 * finished - each time reading what it asks for off the disc - and jumps to
 * the address the third returns (runtimes/runtime-gc/src/gc_apploader.c tells the
 * story from the other side). A retail disc's is Nintendo's and stays on the
 * disc it came on. This one is ours, so that a disc built in this
 * repository can boot the way a game does: through an encrypted partition,
 * an apploader, and a DOL.
 *
 * It does what the protocol needs and nothing else: reads the DOL's
 * sections to where they belong, clears its BSS, puts the filesystem at the
 * top of MEM1 and writes down where. It is freestanding - no library, no
 * stack of its own (the caller provides one), no small-data registers - and
 * keeps its state in initialised data so that the whole of it is in the
 * bytes the disc holds.
 *
 * On a Wii every disc offset the apploader deals in is a quarter of the
 * byte offset, as the disc stores them.
 */
typedef unsigned int u32;
typedef unsigned char u8;
typedef void (*Report)(const char *format, ...);

/* A DOL's header, in words: eighteen file offsets, eighteen addresses,
 * eighteen sizes, then the BSS and the entry point. */
enum { DOL_OFFSETS = 0, DOL_ADDRESSES = 18, DOL_SIZES = 36, DOL_BSS = 54, DOL_BSS_SIZE = 55, DOL_ENTRY = 56 };

struct State {
  u32 stage;
  u32 dol;      /* the DOL's place on the disc, in words */
  u32 fst;      /* the filesystem's */
  u32 fst_size; /* in bytes */
  u32 fst_max;
  u32 section;
  u32 entry;
  Report report;
};

/* Initialised, and not to zero, so the linker puts both in the image. */
static struct State state = {0xffffffffu, 0, 0, 0, 0, 0, 0, 0};
static u32 scratch[0x40] = {0xffffffffu};

static void app_init(Report report) {
  state.report = report;
  state.stage = 0;
}

static int app_main(void **address, u32 *length, u32 *offset) {
  switch (state.stage) {
    case 0:
      /* Where the DOL and the filesystem are: four words of the disc's
       * header. */
      *address = scratch;
      *length = 0x20;
      *offset = 0x420 >> 2;
      state.stage = 1;
      return 1;

    case 1:
      state.dol = scratch[0];
      state.fst = scratch[1];
      state.fst_size = scratch[2] << 2;
      state.fst_max = scratch[3] << 2;
      *address = scratch;
      *length = 0x100;
      *offset = state.dol;
      state.section = 0;
      state.stage = 2;
      return 1;

    case 2:
      /* A section at a time, straight to its address. */
      while (state.section < 18) {
        const u32 i = state.section++;
        if (!scratch[DOL_SIZES + i]) continue;
        *address = (void *)scratch[DOL_ADDRESSES + i];
        *length = scratch[DOL_SIZES + i];
        *offset = state.dol + (scratch[DOL_OFFSETS + i] >> 2);
        return 1;
      }
      {
        u8 *bss = (u8 *)scratch[DOL_BSS];
        u32 left = scratch[DOL_BSS_SIZE];
        while (left--) *bss++ = 0;
      }
      state.entry = scratch[DOL_ENTRY];
      {
        /* The filesystem at the top of MEM1, a cache line's alignment
         * down, and the three globals that say so: where it is, how big
         * it may get, and that the arena ends below it. */
        const u32 at = (0x81800000u - state.fst_max) & ~31u;
        *(volatile u32 *)0x80000038u = at;
        *(volatile u32 *)0x8000003cu = state.fst_max;
        *(volatile u32 *)0x80000034u = at;
        *address = (void *)at;
        *length = state.fst_size;
        *offset = state.fst;
      }
      state.stage = 3;
      return 1;

    default: break;
  }
  if (state.report) state.report("native console test apploader: loaded, entry %08x\n", state.entry);
  return 0;
}

static void *app_close(void) { return (void *)state.entry; }

/* The entry point: hand back the three functions. */
void app_entry(void (**init)(Report), int (**main)(void **, u32 *, u32 *), void *(**close)(void)) {
  *init = app_init;
  *main = app_main;
  *close = app_close;
}
