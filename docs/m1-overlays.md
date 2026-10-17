# M1 Code overlays: discovery, coverage, emission

Follow-up to `docs/m0-trace.md` §5. M1's exit test (title screen, then
Station Square) runs code that is not in `1ST_READ.BIN`: the game
decompresses it from disc into fixed slots at run time. This step recompiles
the seven overlay files the M0 route executed on the way to Station Square:

| File | Slot | Role |
|---|---|---|
| `SONICADV/ADVERTISE.PRS` | area, `0x8C900000` | title screen, menus |
| `SONICADV/ADV00.PRS` | area, `0x8C900000` | Station Square |
| `SONICADV/EV0001.PRS`, `EV0002`, `EV0003`, `EV0006`, `EV002B` | event, `0x8CB80000` | opening events |

Not covered yet: `B_CHAOS0`, `STG01`, `SUMMARY` (same slot, later on the
route), the movie player `MOVIE.BIN` (`0x8CEB0000`), and the other ~290
overlay files on the disc. The same steps apply to each.

## 1. Extraction

```
python -I tools/extract_overlays.py ../private/dump/sa1_us_1005/data ../private/overlays/sa1_us_1005
```

Decompresses each configured file (SEGA PRS, the decoder from
`tools/trace_overlays.py`) to `../private/overlays/sa1_us_1005/<NAME>.bin`.
It refuses an output directory inside the repository or inside the dump;
the dump is only read. Under a second for all seven.

| File | PRS bytes | Decompressed | SHA-256 (decompressed) |
|---|---|---|---|
| `ADVERTISE` | 176,538 | 937,008 | `49e2690ab09d2802a4e0a284526a8ace18306722973fbb2ded663ad95fff4c39` |
| `ADV00` | 1,064,209 | 2,328,347 | `bac5bcd53a8e8b95e6a9e536d1ae5d7cd3ed63056c027366390c645d2d74c94d` |
| `EV0001` | 41,069 | 81,596 | `8386b36046abecae70bf31de35eda553e28a671ee857a7348fbeec1a0d8cdcaf` |
| `EV0002` | 70,828 | 157,824 | `eb20858ae2ad5d28ec772ad5e9e33062d82555625390474d99664f9206276e10` |
| `EV0003` | 49,216 | 87,496 | `9c53aa10e7922502b23aced8516e7b94efac5fd8d31a3330f771dca409c9b500` |
| `EV0006` | 14,984 | 31,640 | `9bdf1b01e15bff52b0a7ad3087982b9146541859f921df71d27945892ee49e34` |
| `EV002B` | 150,391 | 325,560 | `24958a1055e9aa83580f6777ba9a4e3675c44e290842f31b86375755853a985e` |

## 2. Code and data

Each file is one SHC link laid out the same way:

- an optional **32-byte header** of entry pointers. `ADVERTISE` has three
  (`0x8C9000F4`, `0x8C9001C0`, `0x8C9001DA`); each `EVnnnn` has two, the
  event's code entry and a pointer into its data, padded with `0xFF`;
  `ADV00` has none and starts with code;
- **code**, padded with `nop` (`0x0009`) to a 32-byte boundary;
- **data**: models, motions, tables, strings, which is most of the file.

`find_code_end` in `tools/extract_overlays.py` estimates where the code
ends. It runs recursive descent from the first code byte and from every SHC
prologue (`sts.l pr,@-r15`) that has a matching epilogue (`lds.l @r15+,pr`,
`rts`). It takes the furthest instruction or literal-pool word reached and
rounds up to 32 bytes. Then it extends one 32-byte chunk at a time while a
linear decode from the chunk reaches an `rts` with no invalid word on the
way; this catches leaf functions that have no prologue and are reached only
through pointers (`EV0003` +0x16C0, `EV0002` +0x2DE0). I checked each
boundary by hand in a hex dump: the data side starts with a pointer table,
floats or strings in every file.

| File | Code (offsets) | Code bytes | Share of file | Executed span in the trace (lower bound) |
|---|---|---|---|---|
| `ADVERTISE` | `0x20`–`0xEB00` | 60,128 | 6.4 % | `+0x20`–`+0xEAA6` |
| `ADV00` | `0x0`–`0x13220` | 78,368 | 3.4 % | `+0x0`–`+0x12014` |
| `EV0001` | `0x20`–`0x3220` | 12,800 | 15.7 % | `+0x20`–`+0x31C2` |
| `EV0002` | `0x20`–`0x2EE0` | 11,968 | 7.6 % | `+0x3A`–`+0x2AAE` |
| `EV0003` | `0x20`–`0x16E0` | 5,824 | 6.7 % | `+0x9E`–`+0x166A` |
| `EV0006` | `0x20`–`0x1660` | 5,696 | 18.0 % | `+0xBC`–`+0x1630` |
| `EV002B` | `0x20`–`0xA260` | 41,536 | 12.8 % | `+0x40`–`+0x8FFA` |

The trace executed no block past any of these ends ("blocks executed in the
data region" is 0 for every file below).

## 3. Discovery

One Dream-Recomp config per overlay in `recomp/sa1_us_1005/overlays/`
(`advertise.toml`, `adv00.toml`, `ev0001.toml`, ...). Each config:

- points `[binary]` at the decompressed file and loads it at its slot
  (link address `0x0C900000` or `0x0CB80000`);
- seeds discovery with the header entry pointers plus the trace seeds;
- has an `[sa1_overlay]` table with the slot, the code range and the
  SHA-256, which `emit_overlay.py` reads and Dream-Recomp ignores.

The `DREAM_CODE_RANGES` value for each overlay is in its config's header
comment, and limits entries to the code part:

```
cd recomp/dream-recomp
DREAM_CODE_RANGES=0C900000:0C913220 DREAM_ASSUME_ENTRY_FPSCR=1 \
  ./build/translator/dream-translate.exe game \
  --config ../sa1_us_1005/overlays/adv00.toml --out-dir ../out/overlays/dream/adv00
```

0.1–0.3 s each. Every overlay calls into `1ST_READ.BIN` constantly. Those
targets lie outside the code range, so discovery leaves them alone, and the
emitter turns them into calls into the main image (§5).

## 4. Coverage against the M0 trace

`tools/trace_diff.py` has an overlay mode:

```
python -I tools/trace_diff.py ../flycast/trace-2.7/sa1trace.txt \
    recomp/out/overlays/dream/adv00/sa1_ovl_adv00.functions.json \
    --overlay ../private/overlays/sa1_us_1005/ADV00.bin --base 8C900000 --code-end 8C913220 \
    --seeds recomp/sa1_us_1005/overlays/adv00_seeds.txt
```

Several files share each slot over a run, so a trace block inside the slot
counts only if its `M` record (the first 32 bytes it executed) matches the
given file at that offset. Blocks belonging to the other files are dropped.
The table below is the M0 route (`docs/m0-trace.md` §3).

| File | Executed call targets | Found by plain discovery | **Coverage** | With seeds | Seeds | Executed bytes | inside discovered fns | Functions (executed) |
|---|---|---|---|---|---|---|---|---|
| `ADVERTISE` | 184 | 180 | 97.83 % | **100.00 %** | 4 | 18,336 | 100.00 % | 379 (234) |
| `ADV00` | 123 | 119 | 96.75 % | **100.00 %** | 4 | 12,028 | 100.00 % | 540 (164) |
| `EV0001` | 33 | 32 | 96.97 % | **100.00 %** | 1 | 5,462 | 100.00 % | 59 (43) |
| `EV0002` | 20 | 18 | 90.00 % | **100.00 %** | 2 | 3,028 | 100.00 % | 81 (39) |
| `EV0003` | 13 | 13 | 100.00 % | **100.00 %** | 0 | 3,312 | 100.00 % | 27 (21) |
| `EV0006` | 4 | 4 | 100.00 % | **100.00 %** | 0 | 2,626 | 100.00 % | 21 (8) |
| `EV002B` | 29 | 28 | 96.55 % | **100.00 %** | 1 | 11,746 | 100.00 % | 119 (49) |
| **All seven** | **406** | **394** | **97.04 %** | **100.00 %** | **12** | **56,538** | **100.00 %** | 1,226 (558) |

All 12 seeds are executed call targets that fall inside a discovered
function rather than at its start: discovery split the code at the wrong
place, and no executed code was missed. They are in `<name>_seeds.txt` and
in the `extra` list of each config. "Executed bytes" counts merged block
ranges (overlapping blocks once). `docs/m0-trace.md` §5 summed block sizes,
so its byte column is a little higher.

## 5. Emission

```
python recomp/emitter/emit_overlay.py recomp/sa1_us_1005/overlays/adv00.toml
```

writes `recomp/out/overlays/emitter/adv00/` (git-ignored): `ovl_adv00_NNN.c`,
`ovl_adv00.h`, `ovl_adv00_table.c` and `report.txt`. It uses the same
emitter as the main image (`recomp/emitter/sh4emit/`), with three changes:

- **Names carry the file.** Files that share a slot share addresses, so the
  functions are `ovl_adv00_8c9xxxxx`, `ovl_advertise_8c9xxxxx`, and so on.
- **Calls into the main image are direct.** `--main-table` (default
  `recomp/out/emitter/game_table.c`) gives the main image's emitted
  functions. A call or tail call to one of them becomes `fn_8c0xxxxx(c)`,
  with a full register sync on both sides, because no summary crosses units.
  Calls to the seam become `sdk_xxxx(c)` as in the main image.
- **One table per overlay.** `ovl_<name>_table.c` defines a static
  `(addr, fn)` table and a `const sa1_overlay sa1_ovl_<name>` record (name,
  code range, table). `runtime/sh4_rt.[ch]` gained
  `sa1_overlay_set(slot, ov)` / `sa1_overlay_get(slot)` with two slots,
  `SA1_SLOT_AREA` and `SA1_SLOT_EVENT`. `sa1_fn_lookup` (behind
  `sa1_call`) checks the installed overlays by code range before the main
  table, so main-image code that calls a pointer into the slot reaches
  whichever file is loaded. The loader hook that calls `sa1_overlay_set`
  when the game decompresses a file belongs to the HLE file layer and is
  not written yet.

| File | C functions | SH-4 instructions | Calls into main | Seam calls | Unresolved `jsr`/`jmp` | C source | psp-gcc `-Os` `.text` | Ratio | clang `-Os` x86-64 `.text` |
|---|---|---|---|---|---|---|---|---|---|
| `ADVERTISE` | 330 | 22,492 | 683 | 165 | 38 | 1.51 MB | 219,148 | 4.87x | 201,038 |
| `ADV00` | 496 | 32,313 | 1,524 | 677 | 5 | 2.13 MB | 332,721 | 5.15x | 296,268 |
| `EV0001` | 51 | 5,097 | 353 | 31 | 14 | 0.34 MB | 50,337 | 4.94x | 46,160 |
| `EV0002` | 50 | 5,237 | 324 | 130 | 4 | 0.34 MB | 48,449 | 4.63x | 42,123 |
| `EV0003` | 25 | 2,200 | 229 | 15 | 0 | 0.15 MB | 20,532 | 4.67x | 18,578 |
| `EV0006` | 21 | 2,185 | 383 | 3 | 0 | 0.15 MB | 21,596 | 4.94x | 19,291 |
| `EV002B` | 97 | 18,937 | 831 | 288 | 219 | 1.21 MB | 160,482 | 4.24x | 148,223 |
| **All seven** | **1,070** | **88,461** | 4,327 | 1,309 | 280 | 5.84 MB | **853,265** | **4.82x** | 771,681 |

"Ratio" is MIPS `.text` over SH-4 code bytes (2 × instructions). It is
higher than the main image's 3.88x because every call into the main image
syncs the whole register file. Passing the main image's call summaries
across units would remove most of that. No direct calls go between the area
and event slots. Each overlay emits in 0.1–2.2 s.

**Build check.** All 33 overlay C files compile with clang 23 (`-Os
-std=c11 -Wall`, 0 warnings) and with psp-gcc (`-Os -G0`, inside WSL, no
errors). The clang objects also link together with the whole main image,
`sh4_rt.c` and the seam stubs into one test program. The test installs each
overlay in turn, checks that `sa1_fn_lookup` returns every overlay
function's own C function for its address (0 mismatches over the 1,070),
then empties the slots and checks that `0x8C900000` no longer resolves.
No guest code runs. The scripts are `recomp/out/overlays/compile_clang.sh`,
`compile_psp.sh` and `link_test.c` (git-ignored, next to the output).

## 6. What the overlays changed in the main image

The overlays call 62 distinct addresses outside their own code that the
main build had no function for. All of them are now handled:

- **37 were M0 trace seeds** that the main emitter did not yet use, because
  it still read the unseeded list `recomp/out/dream/`. The main image is
  now emitted from `recomp/out/dream_seeded/`.
- **23 entries in block A that only overlays call.** 19 were never
  discovered, because no static path inside `1ST_READ.BIN` leads to them. 4 were discovered as
  branch targets, which the main emitter merges into their parent as local
  code when nothing in the image calls them. All 23 are in
  `recomp/sa1_us_1005/overlay_main_seeds.txt` and in the main config's
  `extra`. Four of them (`0x0C019360`, `0x0C04A480`, `0x0C0A6696`,
  `0x0C107AF6`) are the four "executed runs outside discovered functions"
  that `docs/m0-trace.md` §4 could not explain. They are called from the
  overlays, so main-image byte coverage with seeds is now **100.00 %**
  (was 99.99 %).
- **2 SDK entry points** that only `ADVERTISE` calls (`0x8C5FF2E8`,
  `0x8C603384`) were added to `sdk_entry_points.tsv` with 0 block-A sites.
  `hle/seam.h` and `hle/seam_stubs.c` were regenerated (291 entries).

Main image after this: 9,558 functions discovered (was 9,542). The emitter
produces 5,343 C functions and 441,670 instructions (was 5,295 / 440,850
from the unseeded list). Trace coverage is still 100.00 % of executed call
targets.

**Rule for every further overlay:** run `emit_overlay.py`, then add any
"external target without a function" it prints to the main config. These are
main-image entries that only overlays reach.

## 7. Open items

- The **loader hook**: find the routine in block A that decompresses a PRS
  into `0x8C900000` / `0x8CB80000`, and have the HLE call
  `sa1_overlay_set` with the matching record. The disc file name or the
  decompressed SHA-256 can select the record.
- **280 unresolved indirect sites** in the overlays (219 in `EV002B`, mostly
  `jsr @rn` through event-script tables) go through `sa1_call`. That works
  for targets with a function, and traps on any other.
- **Code size at full scale**: on the PSP the seven files cost 0.85 MB of
  `.text` together, whereas the game keeps only one area file and one event
  file in RAM at a time. Roughly 300 files will not fit in the EBOOT
  alongside the main image's 3.66 MB. Each overlay will probably have to
  become a PRX loaded with its file.
- The remaining route files (`B_CHAOS0`, `STG01`, `SUMMARY`, `MOVIE.BIN`) and
  a trace covering more events.
