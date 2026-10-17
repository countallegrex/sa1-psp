# Recompiler configuration: Sonic Adventure US v1.005

What belongs here (committed): the title configuration for the recompiler,
the seam table (`seam.md` / `seam.toml`: address → name → `native` |
`recompile`), seed addresses for functions static discovery misses, and
notes on FPU/timing quirks.

What never belongs here: the executable, any bytes copied from it, or the
generated code. Generated code goes to `../out/` (git-ignored).

Addresses are facts about the binary and are fine to commit, in the same way
N64 recompilation projects commit their symbol files.

Identity of the supported dump (measured at M0, 2026-10-16):

| File | Size | SHA-256 |
|---|---|---|
| `1ST_READ.BIN` (descrambled) | 6,735,488 | `45bc5d3c25dcfe2cc2c5701311d46783f33e4daa44a951cd9896724d66f0367a` |

MD5 for cross-checking against other tools: `060cad2ceefc07c7429085f30a356046`.

## Files

| File | What |
|---|---|
| `sa1_us_1005.toml` | Title config for `dream-translate game` (Dream-Recomp). Load/link/entry addresses, seeds, excludes. |
| `sdk_entry_points.tsv` | The 289 SDK addresses that game code calls through literal pools, ranked by call sites, with behavioural names where known. Start of the seam table. |
| `trace_seeds.txt` | The 68 executed call/tail-jump targets M0 discovery missed, from the Flycast trace (`tools/trace_diff.py`, `docs/m0-trace.md`). Also in `sa1_us_1005.toml` `extra`. |
| `executed_entries.txt` | The 3,661 function entries (seeded list) executed on the M0 route: title → Station Square → Emerald Coast → Station Square. Addresses only. |
| `overlay_main_seeds.txt` | The 26 block-A entries that only the code overlays call (discovery cannot see them from the image; 3 are `SUMMARY.PRS`'s). Also in `sa1_us_1005.toml` `extra`. `docs/m1-overlays.md` §6. |
| `trace_call_targets.txt` | The 655 block-A entries the M0 route reached through a register (`jsr`/`jmp`/`bsrf`/`braf`, the trace's `D` records). The emitter keeps them as C functions whatever their discovery origin (`emit_sa1.py --keep`), since object and callback tables point at them (`docs/m1-boot-report.md` §5). |
| `overlays/<name>.toml` | One Dream-Recomp config per code overlay (`ADVERTISE`, `ADV00`, `EV0001`/`0002`/`0003`/`0006`/`002B` so far): slot address, code range, SHA-256 of the decompressed file (`tools/extract_overlays.py`, under `../private/`). |
| `overlays/<name>_seeds.txt` | Executed call targets in that overlay that discovery missed, from `tools/trace_diff.py --overlay`. |

## Code layout of the binary (see `docs/seam.md`)

| Block | Guest range (P1) | Contents |
|---|---|---|
| A | `0x8C010000`–`0x8C111200` | Game code, plus a few Katana pieces at its tail (`0x8C10BE78`–`0x8C111192`: syRtc, G1/G2 DMA, BIOS syscall shims). |
| data | `0x8C112B84`–`0x8C5FA000` | Game data (SA Tools addresses live here). |
| B | `0x8C5FA000`–`0x8C66C000` | Katana SDK code: Shinobi, Ninja, KAMUI, sound (sd/mwRna/SJ/ADXT). |
| tail | `0x8C66C000`–`0x8C67C800` | SDK constant data and version strings. |

## Running Dream-Recomp on this title

Two local changes to Dream-Recomp's translator are needed first. They are
not committed here (a diff of GPL code is GPL; this repo is MIT), so apply
them by hand in `recomp/dream-recomp/translator/src/`:

1. `analysis/discover.cpp`, `Discoverer::enqueue`: read the environment
   variable `DREAM_CODE_RANGES` (`lo:hi,lo:hi`, hex, link-space) once in the
   constructor and reject any candidate entry outside those ranges.
2. `emit/emit.cpp`, the entry-mode inference in `emit_unit` (the branch that
   marks functions "nothing in the image calls this one" as unknown): when
   `DREAM_ASSUME_ENTRY_FPSCR` is set, keep the spec default instead.

Then:

```
cd recomp/dream-recomp
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DDREAM_DEV_INTERPRETER=ON -DDREAM_RENDERER=OFF -DDREAM_WERROR=OFF -DDREAM_BUILD_TESTS=OFF
cmake --build build --target dream-translate
DREAM_CODE_RANGES=0C010000:0C111200,0C5FA000:0C66C000 DREAM_ASSUME_ENTRY_FPSCR=1 \
  ./build/translator/dream-translate.exe game --config ../sa1_us_1005/sa1_us_1005.toml --out-dir ../out/dream_seeded
```

The emitter reads `../out/dream_seeded/` (the list with the trace and
overlay seeds). Overlays: `docs/m1-overlays.md` §3.

`DREAM_CODE_RANGES` is needed: without it the pointer pass seeds 42,000
bogus functions inside model data (which decodes as valid SH-4), the sweep
then runs for over an hour, and the output is 90 % garbage.
`DREAM_ASSUME_ENTRY_FPSCR` keeps Katana's reset FPSCR for functions no
static caller reaches instead of emitting both single and double lowerings
behind a runtime test at every FP instruction (111,691 of them otherwise).

Dream-Recomp is GPLv2. It is run as a separate process for discovery and
measurement only; nothing from it is linked into this project (README §0).
