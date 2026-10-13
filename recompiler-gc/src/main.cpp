/*
 * gcrecomp: a GameCube executable into a C project that is that program.
 *
 *   gcrecomp INPUT -o DIR [--name ID] [--trace FILE]...
 *   gcrecomp --version | --version-json
 *
 * Roots are the entry point, the exception vectors, and every code address
 * the program stores as data or builds in a register (analyzer.h,
 * pointer_roots; --no-pointer-roots leaves those out). Static analysis
 * follows flow from them; a function reached only through an address
 * computed some other way runs interpreted until a trace names it.
 * --trace FILE adds the addresses a run of the generated program saw the
 * interpreter entered at (its --trace-out) as roots. An address
 * outside the executable (low-memory vectors the OS writes, code copied at
 * run time) is ignored, as any root outside the image is.
 *
 * INPUT is a disc image, a `.dol` or an ELF. ELF is accepted because every program in
 * tests/gc/programs is one, and the phase gate is that those programs give
 * the same verdicts recompiled as interpreted - a gate that required a
 * commercial disc image would be a gate nobody could run.
 */
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <string>
#include <vector>

#include "analyzer.h"
#include "emitter.h"
#include "image.h"

#ifndef GCRECOMP_VERSION
#define GCRECOMP_VERSION "0.0.0"
#endif

namespace {

/* Machine-readable identity, recorded in release manifests. */
void print_version_json() {
  printf("{\"schema\":\"gcrecomp.version\",\"schema_version\":1,\"version\":\"%s\",\"abis\":{},"
         "\"features\":[\"embedded-image\",\"external-runtime-snapshot\",\"trace\"]}\n",
         GCRECOMP_VERSION);
}

/* One hexadecimal address a line; `#` lines are comments. Returns false when
 * the file will not open. */
bool read_trace(const std::string &path, std::vector<uint32_t> *roots) {
  FILE *file = fopen(path.c_str(), "r");
  char line[256];
  if (!file) return false;
  while (fgets(line, sizeof(line), file)) {
    char *end = nullptr;
    unsigned long value;
    if (line[0] == '#') continue;
    value = std::strtoul(line, &end, 16);
    if (end != line) roots->push_back((uint32_t)value);
  }
  fclose(file);
  return true;
}

int usage() {
  fprintf(stderr,
          "usage: gcrecomp INPUT -o DIR [--name ID] [--runtime DIR] [--part-insns N] [--files N]\n"
          "                [--trace FILE]... [--no-pointer-roots] [--wii]\n"
          "       gcrecomp --version | --version-json\n"
          "  INPUT         a disc image, a .dol or an ELF\n"
          "  -o DIR        where to write the generated project\n"
          "  --name ID     what to call the program (default GAME)\n"
          "  --runtime     the runtime-gc directory copied into the project\n"
          "  --part-insns  most instructions in one generated function (default 0 = no limit)\n"
          "  --part-cost N most estimated compile cost in one generated function (default 400, 0 = no limit)\n"
          "  --fp-inline   inline the heavy floating-point handlers rather than call one copy\n"
          "  --files N     how many block files (default: from the estimated compile cost)\n"
          "  --trace FILE  entry points a run found (the generated program's --trace-out)\n"
          "  --no-pointer-roots  only the entry point, the vectors and traces as roots, not the\n"
          "                code addresses the program stores in data or builds in a register\n"
          "  --wii         INPUT is a Wii program (a Wii disc says so itself)\n"
          "  --key-file F  the Wii common key, for a Wii disc: 16 bytes, 32 hex digits, or a keys.bin\n"
          "                (also found in WII_COMMON_KEY, WII_KEY_FILE, or beside the disc)\n");
  return 2;
}

/* The runtime-gc tree to copy into the project: --runtime if given; else
 * $GCRECOMP_RUNTIME_DIR, runtime-gc beside the executable (a release), two
 * levels up (build/bin in a checkout), then the checkout it was built
 * from. */
std::string find_runtime_dir(const char *argv0, const std::string &explicit_dir) {
  namespace fs = std::filesystem;
  auto usable = [](const fs::path &dir) {
    std::error_code ec;
    return !dir.empty() && fs::is_regular_file(dir / "include" / "gc.h", ec);
  };
  if (!explicit_dir.empty()) return usable(explicit_dir) ? explicit_dir : std::string();
  std::vector<fs::path> candidates;
  if (const char *env = std::getenv("GCRECOMP_RUNTIME_DIR"); env && *env) candidates.emplace_back(env);
  std::error_code ec;
  const fs::path exe_dir = fs::absolute(argv0, ec).parent_path();
  if (!ec) {
    candidates.push_back(exe_dir / "runtime-gc");
    candidates.push_back(exe_dir.parent_path().parent_path() / "runtime-gc");
  }
  candidates.emplace_back(GCRECOMP_DEFAULT_RUNTIME_DIR);
  for (const fs::path &dir : candidates) {
    if (!usable(dir)) continue;
    const fs::path canonical = fs::weakly_canonical(dir, ec);
    return (ec ? dir : canonical).string();
  }
  return {};
}

}  // namespace

int main(int argc, char **argv) {
  gcrecomp::Options options;
  std::string input;
  std::string error;
  gcrecomp::Image image;

  std::string runtime_arg;
  std::vector<std::string> traces;
  bool pointer_roots = true;
  bool wii = false;
  std::string key_file;

  if (argc == 2 && !strcmp(argv[1], "--version")) {
    printf("%s\n", GCRECOMP_VERSION);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--version-json")) {
    print_version_json();
    return 0;
  }

  for (int i = 1; i < argc; i++) {
    const std::string argument = argv[i];
    if (argument == "-o" && i + 1 < argc) {
      options.out_dir = argv[++i];
    } else if (argument == "--name" && i + 1 < argc) {
      options.name = argv[++i];
    } else if (argument == "--runtime" && i + 1 < argc) {
      runtime_arg = argv[++i];
    } else if (argument == "--part-insns" && i + 1 < argc) {
      options.block.part_insns = (unsigned)std::strtoul(argv[++i], nullptr, 10);
    } else if (argument == "--part-cost" && i + 1 < argc) {
      options.block.part_cost = (unsigned)std::strtoul(argv[++i], nullptr, 10);
    } else if (argument == "--fp-inline") {
      options.block.fp_inline = true;
    } else if (argument == "--psq-inline") {
      options.block.psq_inline = true;
    } else if (argument == "--lazy-inline") {
      options.block.lazy_inline = true;
    } else if (argument == "--no-loops") {
      options.block.loops = false;
    } else if (argument == "--no-returns") {
      options.block.returns = false;
    } else if (argument == "--no-fpr-locals") {
      options.block.fpr_locals = false;
    } else if (argument == "--kernels-inline") {
      options.block.kernels_inline = true;
    } else if (argument == "--trace-insns" && i + 1 < argc) {
      options.block.trace_insns = (unsigned)std::strtoul(argv[++i], nullptr, 10);
    } else if (argument == "--trace" && i + 1 < argc) {
      traces.push_back(argv[++i]);
    } else if (argument == "--no-pointer-roots") {
      pointer_roots = false;
    } else if (argument == "--wii") {
      wii = true;
    } else if (argument == "--key-file" && i + 1 < argc) {
      key_file = argv[++i];
    } else if (argument == "--files" && i + 1 < argc) {
      options.files = (unsigned)std::strtoul(argv[++i], nullptr, 10);
    } else if (!argument.empty() && argument[0] == '-') {
      return usage();
    } else if (input.empty()) {
      input = argument;
    } else {
      return usage();
    }
  }
  if (input.empty() || options.out_dir.empty()) return usage();
  options.runtime_dir = find_runtime_dir(argv[0], runtime_arg);
  if (options.runtime_dir.empty()) {
    fprintf(stderr, "gcrecomp: no runtime-gc directory found%s%s\n",
            runtime_arg.empty() ? "" : " at ", runtime_arg.c_str());
    return 1;
  }

  /* Make the output directory rather than asking the caller to. A tool
   * that fails after doing all its work because a folder was missing is a
   * tool nobody forgives twice. */
  std::error_code made;
  std::filesystem::create_directories(options.out_dir, made);

  if (!gcrecomp::read_image(input, &image, &error, key_file)) {
    fprintf(stderr, "gcrecomp: %s\n", error.c_str());
    return 1;
  }
  if (wii) image.wii = true;
  printf("gcrecomp: %s, %s, entry %08x, %u segments\n", input.c_str(), image.wii ? "Wii" : "GameCube",
         image.entry, (unsigned)image.segments.size());

  {
    /* The exception vectors are entry points no flow reaches: the hardware
     * jumps to them. A program that installs handlers there has code the
     * analyzer would otherwise never see. */
    static const uint32_t VECTORS[] = {0x80000100u, 0x80000200u, 0x80000300u, 0x80000400u,
                                       0x80000500u, 0x80000600u, 0x80000700u, 0x80000800u,
                                       0x80000900u, 0x80000c00u, 0x80000d00u, 0x80000f00u,
                                       0x80001300u, 0x80001400u, 0x80001700u};
    std::vector<uint32_t> roots(VECTORS, VECTORS + sizeof(VECTORS) / sizeof(VECTORS[0]));
    std::vector<uint32_t> traced_roots;
    if (pointer_roots) {
      const std::vector<uint32_t> found = gcrecomp::pointer_roots(image);
      roots.insert(roots.end(), found.begin(), found.end());
      printf("gcrecomp: %u entry points from addresses the program stores or builds\n", (unsigned)found.size());
    }
    size_t traced = 0, inside = 0;
    for (const std::string &trace : traces) {
      if (!read_trace(trace, &traced_roots)) {
        fprintf(stderr, "gcrecomp: cannot read trace %s\n", trace.c_str());
        return 1;
      }
    }
    traced = traced_roots.size();
    if (!traced_roots.empty()) {
      /* An address inside a block static analysis finds on its own - not
       * its first instruction - is reached by running through the block's
       * start, so a run entered the interpreter there only by coming back
       * from an interrupt (or, rarely, by an indirect jump into the
       * middle). As a root it would cut that block in two and send every
       * pass through it back to the dispatcher; Shrek 2's cover trace from
       * before resumed entries were left out of traces had 475 of them.
       * Left out, a resume there is interpreted to the block's end. */
      const gcrecomp::Analysis untraced = gcrecomp::analyze(image, roots);
      for (uint32_t address : traced_roots) {
        const auto after = std::upper_bound(
            untraced.blocks.begin(), untraced.blocks.end(), address,
            [](uint32_t value, const gcrecomp::Block &block) { return value < block.start; });
        if (after != untraced.blocks.begin()) {
          const gcrecomp::Block &block = *(after - 1);
          if (address > block.start && address < block.end) {
            inside++;
            continue;
          }
        }
        roots.push_back(address);
      }
    }
    const gcrecomp::Analysis analysis = gcrecomp::analyze(image, roots);

    printf("gcrecomp: %u blocks, %u instructions\n", (unsigned)analysis.blocks.size(),
           analysis.instructions);
    if (!traces.empty()) {
      printf("gcrecomp: %u traced entry points from %u trace%s, %u of them inside a block and left out\n",
             (unsigned)traced, (unsigned)traces.size(), traces.size() == 1 ? "" : "s", (unsigned)inside);
    }
    if (!analysis.unimplemented.empty()) {
      /* Named rather than counted, because the useful question is always
       * *which* opcode, and a short list is the answer to it. */
      printf("gcrecomp: stopped at opcodes with no handler:");
      for (const auto &entry : analysis.unimplemented) {
        printf(" %s(%u)", entry.first.c_str(), entry.second);
      }
      printf("\n");
    }
    if (analysis.blocks.empty()) {
      fprintf(stderr, "gcrecomp: nothing was reachable from the entry point\n");
      return 1;
    }

    if (!gcrecomp::emit_project(image, analysis, options, &error)) {
      fprintf(stderr, "gcrecomp: %s\n", error.c_str());
      return 1;
    }
  }
  printf("gcrecomp: wrote %s\n", options.out_dir.c_str());
  return 0;
}
