/*
 * ndsrecomp's analyzer (recompiler-nds/src/analyzer.cpp) on images built
 * here: the rules that find code nothing calls directly.
 *
 *   Leaf functions   A function that pushes nothing (a getter, an empty
 *                    virtual function, a veneer) is only pointed at from
 *                    data. looks_like_leaf() must take the real ones and
 *                    refuse data and code that cannot be a function start,
 *                    each decoy below failing for one reason only.
 *   Trace keys       An instruction a run interpreted is taken only where
 *                    the image holds the opcode the run saw: overlays share
 *                    addresses.
 *
 * The images are ARM9 code at 0x02000000 written as words, with the
 * instruction each one is beside it.
 */
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "analyzer.h"

using namespace ndsrecomp;

static int g_checks;
static int g_failures;

static void check(bool ok, const char *what, int line)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

static const uint32_t kBase = 0x02000000u;

static void put32(CodeImage &image, uint32_t addr, uint32_t word)
{
    uint8_t *p = image.bytes.data() + (addr - kBase);
    p[0] = static_cast<uint8_t>(word);
    p[1] = static_cast<uint8_t>(word >> 8);
    p[2] = static_cast<uint8_t>(word >> 16);
    p[3] = static_cast<uint8_t>(word >> 24);
}

static void put_words(CodeImage &image, uint32_t addr, std::initializer_list<uint32_t> words)
{
    for (uint32_t word : words) {
        put32(image, addr, word);
        addr += 4;
    }
}

static const uint32_t kBxLr = 0xE12FFF1Eu;

/* Where things are in the leaf image. */
enum : uint32_t {
    kMain = kBase + 0x00,     /* push {r4, lr}; mov r0, #0; pop {r4, pc} */
    kGetter = kBase + 0x0C,   /* mov r0, #1; bx lr */
    kVeneer = kBase + 0x14,   /* ldr pc, [pc, #-4]; .word kMain */
    kAfterPool = kBase + 0x1C, /* mov r0, #2; bx lr: what is before it is the veneer's literal */
    kThumbLeaf = kBase + 0x24, /* Thumb: movs r0, #0; bx lr */
    kAnchor1 = kBase + 0x28,  /* bx lr (an entry: gives the decoy after it a boundary) */
    kCalls = kBase + 0x2C,    /* mov r0, #3; bl kMain; bx lr */
    kAnchor2 = kBase + 0x38,
    kUndefined = kBase + 0x3C, /* mov r0, #4; udf; bx lr */
    kAnchor3 = kBase + 0x48,
    kNumbers = kBase + 0x4C,  /* 5, 6, 7, 8 */
    kAnchor4 = kBase + 0x5C,
    kLong = kBase + 0x60,     /* thirty mov r0, #0, then bx lr */
    kAnchor5 = kBase + 0xDC,
    kText = kBase + 0xE0,     /* "TEXTDATA" */
    kThumbEntry = kBase + 0xE8, /* Thumb: bx lr, then two bytes of padding */
    kThumbPadded = kBase + 0xEC, /* Thumb: adds r0, #1; bx lr */
    kAnchor6 = kBase + 0xF0,
    kFrame = kBase + 0xF4,    /* Thumb: push {lr}; pop {pc} (has a frame: not a leaf) */
    kSize = 0x100,
};

static CodeImage leaf_image()
{
    CodeImage image;
    image.name = "leaf";
    image.base = kBase;
    image.bytes.assign(kSize, 0);
    put_words(image, kMain, { 0xE92D4010u, 0xE3A00000u, 0xE8BD8010u });
    put_words(image, kGetter, { 0xE3A00001u, kBxLr });
    put_words(image, kVeneer, { 0xE51FF004u, kMain });
    put_words(image, kAfterPool, { 0xE3A00002u, kBxLr });
    put32(image, kThumbLeaf, 0x47702000u); /* 2000 movs r0, #0; 4770 bx lr */
    put32(image, kAnchor1, kBxLr);
    /* bl kMain from kCalls + 4: offset (kMain - (kCalls + 4 + 8)) / 4. */
    put_words(image, kCalls,
              { 0xE3A00003u, 0xEB000000u | (((kMain - (kCalls + 12)) >> 2) & 0xFFFFFFu), kBxLr });
    put32(image, kAnchor2, kBxLr);
    put_words(image, kUndefined, { 0xE3A00004u, 0xE7F000F0u, kBxLr });
    put32(image, kAnchor3, kBxLr);
    put_words(image, kNumbers, { 5, 6, 7, 8 });
    put32(image, kAnchor4, kBxLr);
    for (uint32_t i = 0; i < 30; i++) put32(image, kLong + 4 * i, 0xE3A00000u);
    put32(image, kLong + 4 * 30, kBxLr);
    put32(image, kAnchor5, kBxLr);
    put_words(image, kText, { 0x54584554u, 0x41544144u });
    put32(image, kThumbEntry, 0x00004770u);  /* 4770 bx lr; 0000 padding */
    put32(image, kThumbPadded, 0x47703001u); /* 3001 adds r0, #1; 4770 bx lr */
    put32(image, kAnchor6, kBxLr);
    put32(image, kFrame, 0xBD00B500u);       /* b500 push {lr}; bd00 pop {pc} */
    return image;
}

static void test_leaf_functions()
{
    const CodeImage image = leaf_image();
    AnalyzerOptions options;
    options.entries = { make_key(kMain, false),    make_key(kAnchor1, false), make_key(kAnchor2, false),
                        make_key(kAnchor3, false), make_key(kAnchor4, false), make_key(kAnchor5, false),
                        make_key(kAnchor6, false), make_key(kThumbEntry, true) };
    Analyzer analyzer(image, options);
    analyzer.seed();

    /* The prologue rule knows the function with a frame and nothing else. */
    CHECK(analyzer.looks_like_function(kMain));
    CHECK(!analyzer.looks_like_function(kGetter));
    CHECK(analyzer.looks_like_function(kFrame | 1u));

    /* Real leaf functions. Each needs what is before it to be known: the
     * getter follows main, and the others follow one another. */
    CHECK(analyzer.looks_like_leaf(kGetter));
    CHECK(!analyzer.looks_like_leaf(kVeneer));    /* the getter is not explored yet */
    CHECK(analyzer.add_entry(make_key(kGetter, false)));
    CHECK(analyzer.looks_like_leaf(kVeneer));
    CHECK(!analyzer.looks_like_leaf(kAfterPool)); /* the veneer's literal is not known as data yet */
    CHECK(analyzer.add_entry(make_key(kVeneer, false)));
    CHECK(analyzer.looks_like_leaf(kAfterPool));
    CHECK(!analyzer.looks_like_leaf(kThumbLeaf | 1u));
    CHECK(analyzer.add_entry(make_key(kAfterPool, false)));
    CHECK(analyzer.looks_like_leaf(kThumbLeaf | 1u));
    CHECK(analyzer.looks_like_leaf(kThumbPadded | 1u)); /* after a Thumb return and its padding */

    /* Decoys: every one starts where something ends, and fails on what it
     * holds. */
    CHECK(!analyzer.looks_like_leaf(kCalls));     /* a call would lose the return address */
    CHECK(!analyzer.looks_like_leaf(kUndefined)); /* an undefined instruction */
    CHECK(!analyzer.looks_like_leaf(kNumbers));   /* small numbers: conditional instructions */
    CHECK(!analyzer.looks_like_leaf(kLong));      /* no return within reach */
    CHECK(!analyzer.looks_like_leaf(kText));      /* text: conditional instructions */
    CHECK(!analyzer.looks_like_leaf(kFrame | 1u)); /* a stack frame: the other rule's */

    /* Not where something ends, misaligned, in data, or outside. */
    CHECK(!analyzer.looks_like_leaf(kMain + 4));      /* inside main, after its push */
    CHECK(!analyzer.looks_like_leaf(kGetter + 2));    /* a misaligned ARM address */
    CHECK(!analyzer.looks_like_leaf(kVeneer + 4));    /* the veneer's literal */
    CHECK(!analyzer.looks_like_leaf(kBase + kSize));  /* just past the image */
    CHECK(!analyzer.looks_like_leaf(kBase - 4));
    CHECK(!analyzer.looks_like_leaf(0));

    /* A leaf taken as an entry becomes a function of its own. */
    CHECK(analyzer.add_entry(make_key(kThumbLeaf, true)));
    analyzer.finish();
    bool getter = false, veneer = false, thumb = false;
    for (const Function &fn : analyzer.functions) {
        getter = getter || (fn.entry == make_key(kGetter, false) && fn.insns.size() == 2);
        veneer = veneer || (fn.entry == make_key(kVeneer, false) && fn.insns.size() == 1);
        thumb = thumb || (fn.entry == make_key(kThumbLeaf, true) && fn.insns.size() == 2);
    }
    CHECK(getter);
    CHECK(veneer);
    CHECK(thumb);
}

/* An image and the instructions a run reports for it. */
static void test_trace_keys()
{
    CodeImage image;
    image.name = "trace";
    image.base = kBase;
    image.bytes.assign(0x40, 0);
    put_words(image, kBase + 0x00, { 0xE92D4010u, 0xE8BD8010u });      /* an entry: push; pop */
    put_words(image, kBase + 0x08, { 0xE3A00001u, kBxLr });            /* reached only by the trace */
    put_words(image, kBase + 0x10, { 0xE3A00002u, kBxLr });            /* the trace saw other code here */
    put_words(image, kBase + 0x18, { 0xE3A00003u, kBxLr });            /* a trace line without an opcode */
    put32(image, kBase + 0x20, 0x47702005u);                           /* Thumb: movs r0, #5; bx lr */

    AnalyzerOptions options;
    options.entries = { make_key(kBase, false) };
    auto trace = [&](uint32_t addr, bool thumb, uint32_t opcode, bool has_opcode) {
        TraceKey key;
        key.key = make_key(addr, thumb);
        key.opcode = opcode;
        key.has_opcode = has_opcode;
        options.trace_keys.push_back(key);
    };
    trace(kBase + 0x08, false, 0xE3A00001u, true);  /* matches */
    trace(kBase + 0x0C, false, kBxLr, true);        /* the same function's second instruction */
    trace(kBase + 0x10, false, 0xE1A00000u, true);  /* another overlay's code at this address */
    trace(kBase + 0x18, false, 0, false);           /* an old trace: taken on trust */
    trace(kBase + 0x20, true, 0x2005u, true);       /* Thumb, matches */
    trace(kBase + 0x22, true, 0x4771u, true);       /* Thumb, does not match */
    trace(kBase + 0x0A, false, 0, false);           /* a misaligned ARM address */
    trace(kBase + 0x1000, false, 0xE3A00001u, true); /* outside the image */

    Analyzer analyzer(image, options);
    analyzer.seed();
    analyzer.finish();

    CHECK(analyzer.function_entries.count(make_key(kBase + 0x08, false)) == 1);
    CHECK(analyzer.function_entries.count(make_key(kBase + 0x0C, false)) == 0); /* part of the function before it */
    CHECK(analyzer.labels.count(make_key(kBase + 0x0C, false)) == 1);           /* but a place to enter at */
    CHECK(analyzer.insns.count(make_key(kBase + 0x10, false)) == 0);
    CHECK(analyzer.function_entries.count(make_key(kBase + 0x18, false)) == 1);
    CHECK(analyzer.function_entries.count(make_key(kBase + 0x20, true)) == 1);
    CHECK(analyzer.labels.count(make_key(kBase + 0x22, true)) == 0);
    CHECK(analyzer.insns.count(make_key(kBase + 0x22, true)) == 1); /* reached from 0x20 all the same */
    CHECK(analyzer.trace_entries == 3);
}

int main()
{
    test_leaf_functions();
    test_trace_keys();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
