# Data-mod packages

Native Console data mods are ROM-free, declarative packages. Version 1 defines
identity, exact-game compatibility, deterministic composition, content
integrity, provenance, and an immutable runtime overlay artifact.

A port supplies two JSON documents that make the generic contract concrete: a
package schema describing manifest shape, and an independently versioned
target policy that names the exact ROM, port, semantic contract, allowed
content targets, extensions, and size limits.

## Package contract

Every manifest declares:

- a reverse-domain-like package ID and semantic version;
- data-mod runtime ABI `gbrecomp.data-mod` v1;
- exact port, ROM, semantic-package, and semantic-schema identities;
- a numeric load order, exact-version dependencies, and package conflicts;
- SPDX-style license, authors, HTTPS source, and content origin; and
- one or more confined content files with stable IDs, known targets, and
  lowercase SHA-256 hashes.

A target policy permits only bounded declarative content. It must not permit
executable code, arbitrary guest addresses, ROM replacement data, or unknown
target types.

## Validation

Validate the complete installed set in one invocation:

```bash
python3 tools/validate_data_mods.py \
  --policy /path/to/port/mods/target-policy.json \
  --package-schema /path/to/port/mods/package-schema.json \
  --semantic-package /path/to/port/semantic/package.json \
  --semantic-schema /path/to/port/semantic/package-schema.json \
  --manifest /path/to/base/package.json \
  --manifest /path/to/addon/package.json \
  --output /path/to/resolved-mods.json
```

The validator reads and hashes every content file, rejects symlinks and path
escape, checks the installed semantic files against the policy, resolves exact
dependencies and conflicts, and emits a deterministic `(load order, package
ID)` sequence. Any error exits nonzero and produces no trusted resolution.

Package directories, user paths, and content are not logged by the generated
runtime in v1. Portable replay manifests record the ordered stable identities
and hashes without exposing local paths.

## Runtime overlay artifact

A port-specific compiler turns a resolved package set plus the user's local
ROM into a compact `.gbdm` overlay. The artifact is a private derived file
bound to the exact ROM hash; it contains sorted expected/replacement byte
pairs generated from semantic identities rather than addresses. The generated
executable activates it with:

```bash
./output/game/build/game --data-mod /path/to/mods.gbdm
```

The runtime validates all entries before activation and never writes them
into `GBContext.rom`. Removing the option immediately restores vanilla
behavior; neither mode requires regenerating C. The runtime side of this
contract lives in `runtime/src/gbrt_data_mod.c` and is covered by the
`data_mod_overlay_preserves_original_rom` test.

## Portable replay provenance

An input script alone is insufficient for a modded replay. Create a portable
replay envelope after compiling the resolved package set:

```bash
python3 tools/create_data_mod_replay.py \
  --seed /path/to/replay-seed.json \
  --rom /path/to/game.gbc \
  --executable output/game/build/game \
  --generation-receipt output/game/game-generation.json \
  --resolution /path/to/resolved-mods.json \
  --compile-report /path/to/mods-report.json \
  --artifact /path/to/mods.gbdm \
  --output /path/to/replay.json
```

The single JSON envelope embeds the small private overlay artifact and exact
cycle-anchored input files. It records:

- exact ROM, executable, generation receipt, and generated-source inventory;
- ordered package IDs, versions, manifest hashes, content IDs/targets/hashes,
  and their canonical package-set hash;
- target-policy, package-schema, semantic-manifest, and semantic-schema hashes;
- a canonical runtime-configuration hash;
- per-segment input, frame, and selected-state expectations; and
- a canonical portable-seed hash over the game, mods, configuration, and
  ordered segments.

A port's replay driver is expected to validate the entire envelope, the
user's ROM, the local executable and receipt, the embedded artifact, package
order, all content/schema hashes, configuration, and segment order before it
creates an output directory or starts a guest process. Paths and incidental
runtime-log bytes are excluded from the reproduction hash, so independent
output directories produce the same value.
