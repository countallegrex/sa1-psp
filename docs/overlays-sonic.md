# Code overlays for Sonic's story

Follow-up to `docs/m1-overlays.md`. M1 recompiled the eight overlay files
on the way to Station Square. This step covers every code file Sonic's story
loads, so later milestones never trap on a call into an unrecompiled slot:
the action stages, Sonic's bosses, Sky Chase, the other adventure fields
and Sonic's events. The pipeline is now one command per file (§2).

Status, 2026-10-17:

- **57 overlay configs, all emitted, compiled and linked** (desktop clang
  `-O1`; psp-gcc `-Os` in WSL). Every overlay's calls into the main image
  resolve: the main image gained 57 seeds and 9 SDK entries (§5).
- **The scripted desktop run no longer traps.** It loads `B_CHAOS0` and
  `EV002B` (frame 3,119, or 5,299 with ground collision restored) and runs
  the Chaos 0 fight for as long as it is given (30,000 frames tested).
  **The fight is not won on the scripted pad.** See §6: the M1 build has no
  ground collision at all, because three collision functions were wrongly
  stubbed as TA writers. With them restored (tested in an isolated
  worktree) Sonic stands, jumps, homing-attacks and gets hit, but blind
  periodic presses do not beat Chaos 0. Winning needs an input script that
  aims, which is a follow-up.
- **MIPS size:** 7.90 MB for all 57 files together, but only one file per
  slot is resident: 0.77 MB at most (§4).

## 1. Which files

| Group | Files | How chosen |
|---|---|---|
| Action stages | `STG01`–`STG10`, `STG12` | The brief's list. `STG12` (Hot Shelter) is not on Sonic's route (Amy, Gamma, Big) and is included anyway; `STG00` (Hedgehog Hammer) and the sub-games `SBOARD` and `MINICART` are left out. |
| Bosses | `B_CHAOS0`, `B_CHAOS4`, `B_EGM1` (Egg Hornet), `B_CHAOS6`, `B_EGM3` (Egg Viper) | Sonic's bosses. `B_CHAOS2`, `B_EGM2`, `B_E101`, `B_E101_R` and `B_ROBO` belong to the other characters. `B_CHAOS7` (Perfect Chaos) belongs to the last story. Sonic's fights with Knuckles and Gamma have no `B_` file. |
| Sky Chase | `SHOOTING` | Acts 1 and 2 are on Sonic's route. |
| Adventure fields | `ADV00` (M1), `ADV01OBJ` + `ADV0100` / `ADV0130` (Egg Carrier), `ADV02` (Mystic Ruins), `ADV03` (the Past) | There is no `ADV01.PRS`. The Egg Carrier field is `ADV01OBJ.PRS` at `0x8C900000` plus one of two files at `0x8C920000`: `ADV0100` (flying) or `ADV0130` (sunk). Both hold code linked at `0x8C920000`, so they need their own runtime slot (§3). |
| Menus | `ADVERTISE`, `SUMMARY` (M1) | `ALERTWND.PRS` (the menu's VMU alert window) is still not recompiled. |
| Events | `EV0001`–`EV002B`: the 32 event files with IDs below 0x30 | Block A has a table at `0x8C19C0F4` of 32-bit event IDs grouped by character: Sonic's run 0x01–0x2A, then Tails from 0x30, Amy from 0x50, Knuckles from 0x60, Gamma from 0x80, Big from 0xB0, and 0xD0+. The opening run used 0x01 and 0x2B, and the oracle run added 0x02, 0x03 and 0x06. The files from `EV0100` up may be shared events or the last story; they are not attributed to Sonic and not included. |

`MOVIE.BIN` (`0x8CEB0000`) stays native (M1: the player entry returns
"skipped").

## 2. Pipeline: adding a config is the only step

```
python -I tools/overlays.py new STG01            # extract, estimate the code range, write the config
python -I tools/overlays.py new ADV0100 --base 8C920000
python -I tools/overlays.py build [NAME ...]     # discover, trace-diff, seed, emit, registry
python -I tools/overlay_report.py --reached ...  # the table in section 4
```

- `new` decompresses `SONICADV/<NAME>.PRS` into
  `../private/overlays/sa1_us_1005/`, runs `find_code_end`, and writes
  `recomp/sa1_us_1005/overlays/<name>.toml`. The template is the same as
  M1's configs. The slot comes from the name (`EVnnnn` → event slot,
  otherwise area) or from `--base`. The seeds are the header entry pointers
  that fall inside the code. An existing config is never overwritten.
- `build` runs Dream-Recomp discovery with `DREAM_CODE_RANGES` taken from
  the config's `[sa1_overlay]` code range. When the Flycast trace is
  present, it runs `trace_diff.py --overlay`, writes any new seeds into the
  config's `extra` and `<name>_seeds.txt`, and rediscovers. Then it runs
  `emit_overlay.py` and regenerates `hle/overlay/overlay_registry.c`.
  Per-file numbers go to `recomp/out/overlays/stats/<name>.json`, and the
  run prints any main-image targets the overlay calls that have no function
  (§5). All 57 files take about 100 s.
- **Registry**: the tables are referenced **weakly**
  (`HLE_OVERLAY_WEAK`). A config whose code has not been emitted still
  links; its `ov` is `NULL`, and the loader logs "its code was not emitted
  into this build" and leaves the slot empty.
- **CMake**: `backend/desktop/CMakeLists.txt` already globs
  `recomp/out/overlays/emitter/*/ovl_*.c`, so new output is built without
  edits.
- **CI**: the `check-clean` job runs `gen_overlay_registry.py --check`
  (registry matches the configs). The `emitter-tests` job builds and runs
  `tests/overlay/loader_test.c` on Ubuntu (gcc) and Windows (clang). It
  covers the slot rules with a fake registry, including the Egg Carrier
  pair, a file that is not emitted, and the texture loader's scratch use of
  the area.

**Code ranges.** `find_code_end` (M1 §2) gives each file's end. Every file
was then checked for pointers that land past the end on an SHC prologue
(`sts.l pr,@-r15`, or pushes of r8–r14 followed by one). The check found
one miss. In `ADV0100` a function follows a literal pool and padding, and
the padding is `0x0900` there, not `nop` (`0x0009`). `find_code_end` now
continues past such padding when a prologue follows within 64 bytes. That
moved `ADV0100`'s end from `0xF0C0` to `0xF160` and changed no other file's
end. The two pointers past the end that remain (`ADV0100` +0x1EF660,
`ADV03` +0x4E81C) point deep into model data. For each `EVnnnn`, the
header's second word (a data pointer) lies at or beyond the code end.

## 3. Runtime: a third slot for the Egg Carrier

`SA1_SLOT_AREA2` (`recomp/emitter/runtime/sh4_rt.h`) has its base at
`0x8C920000`. `hle/overlay/overlay_loader.c` empties a slot before a load
when the destination is the slot's base, lies inside the installed code, or
lies below that code by less than the incoming file's decompressed size.
The size comes from the registry; a file the registry does not know counts
as filling the area up to the event slot. So `ADV01OBJ` (6,764 B) at
`0x8C900000` keeps `ADV0100` installed, and a stage or any data file loaded
at `0x8C900000` empties both area slots. `sa1_fn_lookup` already searches
all slots by code range.

## 4. Per-file numbers

Discovery is restricted to each file's code range. "Bytes decoded" is
Dream-Recomp's own coverage line (decoded bytes over the span its functions
cover). The undecoded part is literal pools, jump tables and padding inside
that span, plus code nothing reaches. "Seeds" are executed call targets
from the Flycast trace that plain discovery missed. "Unresolved" counts
`jsr @rn` / `jmp @rn` sites that go through `sa1_call`. "MIPS `.text`" is
psp-gcc `-Os -G0` (pspdev v20261001, WSL, as root), summed over the file's
chunks and table. "Ratio" is MIPS bytes over SH-4 bytes (2 × emitted
instructions). "Flycast trace" is the M0 oracle run (title → Station
Square → Chaos 0 → Emerald Coast → Station Square). "Scripted pad" means
the desktop acceptance route reaches the file (§6).

| File | Role | Slot | Code (file offsets) | Code bytes | Functions found | Bytes decoded | Seeds | C functions | Unresolved `jsr`/`jmp` | MIPS `.text` | Ratio | Flycast trace (found/executed call targets, executed bytes) | Scripted pad |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `ADV00` | Station Square | area | `0x0`–`0x13220` | 78,368 | 540 | 59,840 / 67,286 (88.9 %) | 4 | 496 | 5 | 343,207 | 5.31x | 123/123 calls, 12,028 B | **yes** |
| `ADV0100` | Egg Carrier, flying | area 2 | `0x0`–`0xF160` | 61,792 | 285 | 36,184 / 40,600 (89.1 %) | 0 | 252 | 0 | 187,352 | 5.12x | no | no |
| `ADV0130` | Egg Carrier, sunk | area 2 | `0x0`–`0xB620` | 46,624 | 220 | 32,014 / 36,512 (87.7 %) | 0 | 177 | 0 | 157,884 | 4.88x | no | no |
| `ADV01OBJ` | Egg Carrier objects | area | `0x0`–`0x1460` | 5,216 | 33 | 4,000 / 4,798 (83.4 %) | 0 | 33 | 0 | 18,979 | 5.13x | no | no |
| `ADV02` | Mystic Ruins | area | `0x0`–`0x15E60` | 89,696 | 404 | 61,002 / 91,332 (66.8 %) | 0 | 377 | 2 | 305,191 | 4.98x | no (28 B match by bytes only) | no |
| `ADV03` | the Past | area | `0x0`–`0x4F20` | 20,256 | 164 | 14,902 / 16,122 (92.4 %) | 0 | 135 | 2 | 81,411 | 5.39x | no (16 B match by bytes only) | no |
| `ADVERTISE` | title screen and menus | area | `0x20`–`0xEB00` | 60,128 | 379 | 45,648 / 55,888 (81.7 %) | 4 | 329 | 36 | 240,384 | 5.13x | 184/184 calls, 18,336 B | **yes** |
| `B_CHAOS0` | boss Chaos 0 | area | `0x0`–`0x5280` | 21,120 | 129 | 15,912 / 18,954 (84.0 %) | 7 | 70 | 36 | 89,066 | 4.95x | 56/56 calls, 12,684 B | **yes** |
| `B_CHAOS4` | boss Chaos 4 | area | `0x0`–`0x7160` | 29,024 | 131 | 23,202 / 25,416 (91.3 %) | 0 | 122 | 0 | 113,261 | 4.88x | no | no |
| `B_CHAOS6` | boss Chaos 6 | area | `0x0`–`0x74C0` | 29,888 | 101 | 22,472 / 27,162 (82.7 %) | 0 | 92 | 0 | 113,930 | 5.07x | no (4 B match by bytes only) | no |
| `B_EGM1` | boss Egg Hornet | area | `0x0`–`0x4B60` | 19,296 | 137 | 15,198 / 16,054 (94.7 %) | 0 | 127 | 1 | 77,212 | 5.05x | no | no |
| `B_EGM3` | boss Egg Viper | area | `0x0`–`0xACE0` | 44,256 | 195 | 35,320 / 42,432 (83.2 %) | 0 | 179 | 3 | 174,613 | 4.94x | no | no |
| `EV0001` | event 0001 | event | `0x20`–`0x3220` | 12,800 | 59 | 10,026 / 11,740 (85.4 %) | 1 | 51 | 14 | 51,272 | 5.03x | 33/33 calls, 5,462 B | **yes** |
| `EV0002` | event 0002 | event | `0x20`–`0x2EE0` | 11,968 | 81 | 9,122 / 10,272 (88.8 %) | 2 | 50 | 4 | 50,296 | 4.80x | 20/20 calls, 3,028 B | no |
| `EV0003` | event 0003 | event | `0x20`–`0x16E0` | 5,824 | 27 | 4,400 / 5,238 (84.0 %) | 0 | 25 | 0 | 21,143 | 4.81x | 13/13 calls, 3,312 B | no |
| `EV0006` | event 0006 | event | `0x20`–`0x1660` | 5,696 | 21 | 4,370 / 5,290 (82.6 %) | 0 | 21 | 0 | 21,923 | 5.02x | 4/4 calls, 2,626 B | no |
| `EV0007` | event 0007 | event | `0x20`–`0x7F00` | 32,480 | 142 | 26,852 / 29,632 (90.6 %) | 0 | 121 | 2 | 127,777 | 4.76x | no (4 B match by bytes only) | no |
| `EV0008` | event 0008 | event | `0x20`–`0x9C60` | 40,000 | 168 | 32,304 / 36,774 (87.8 %) | 0 | 135 | 2 | 150,445 | 4.66x | no (38 B match by bytes only) | no |
| `EV0009` | event 0009 | event | `0x20`–`0x5100` | 20,704 | 76 | 17,294 / 19,528 (88.6 %) | 0 | 73 | 1 | 83,252 | 4.81x | no (16 B match by bytes only) | no |
| `EV000B` | event 000B | event | `0x20`–`0x78E0` | 30,912 | 125 | 25,234 / 29,048 (86.9 %) | 0 | 95 | 2 | 117,776 | 4.67x | no (20 B match by bytes only) | no |
| `EV000C` | event 000C | event | `0x20`–`0x4BC0` | 19,360 | 65 | 16,410 / 18,226 (90.0 %) | 0 | 58 | 4 | 78,212 | 4.77x | no (4 B match by bytes only) | no |
| `EV000D` | event 000D | event | `0x20`–`0x2420` | 9,216 | 37 | 7,568 / 8,408 (90.0 %) | 0 | 36 | 0 | 38,768 | 5.12x | no | no |
| `EV0011` | event 0011 | event | `0x20`–`0xFE0` | 4,032 | 20 | 3,038 / 3,620 (83.9 %) | 0 | 18 | 0 | 14,947 | 4.92x | no (4 B match by bytes only) | no |
| `EV0012` | event 0012 | event | `0x20`–`0x24A0` | 9,344 | 26 | 7,734 / 8,950 (86.4 %) | 0 | 25 | 0 | 40,228 | 5.13x | no (16 B match by bytes only) | no |
| `EV0013` | event 0013 | event | `0x20`–`0x2A00` | 10,720 | 46 | 8,738 / 10,010 (87.3 %) | 0 | 44 | 0 | 46,072 | 5.21x | no (8 B match by bytes only) | no |
| `EV0014` | event 0014 | event | `0x20`–`0x4C0` | 1,184 | 1 | 786 / 1,122 (70.1 %) | 0 | 1 | 0 | 3,931 | 5.00x | no (4 B match by bytes only) | no |
| `EV0015` | event 0015 | event | `0x20`–`0x1CC0` | 7,328 | 23 | 6,176 / 6,714 (92.0 %) | 0 | 23 | 0 | 31,512 | 5.10x | no | no |
| `EV0016` | event 0016 | event | `0x20`–`0x31E0` | 12,736 | 66 | 10,266 / 11,534 (89.0 %) | 0 | 61 | 0 | 51,988 | 5.06x | no | no |
| `EV0017` | event 0017 | event | `0x20`–`0x2980` | 10,592 | 53 | 8,554 / 9,764 (87.6 %) | 0 | 46 | 0 | 43,280 | 5.06x | no | no |
| `EV001A` | event 001A | event | `0x20`–`0x6400` | 25,568 | 91 | 20,804 / 23,948 (86.9 %) | 0 | 86 | 4 | 99,532 | 4.76x | no (8 B match by bytes only) | no |
| `EV001B` | event 001B | event | `0x20`–`0x2380` | 9,056 | 16 | 7,158 / 8,610 (83.1 %) | 0 | 16 | 3 | 33,831 | 4.73x | no (20 B match by bytes only) | no |
| `EV001C` | event 001C | event | `0x20`–`0x4040` | 16,416 | 56 | 13,468 / 15,262 (88.2 %) | 0 | 54 | 0 | 63,235 | 4.70x | no (4 B match by bytes only) | no |
| `EV001D` | event 001D | event | `0x20`–`0x3420` | 13,312 | 38 | 10,986 / 12,668 (86.7 %) | 0 | 36 | 0 | 50,023 | 4.55x | no (4 B match by bytes only) | no |
| `EV001E` | event 001E | event | `0x20`–`0x6540` | 25,888 | 89 | 22,226 / 38,692 (57.4 %) | 0 | 86 | 2 | 102,405 | 4.61x | no | no |
| `EV0020` | event 0020 | event | `0x20`–`0x26E0` | 9,920 | 54 | 8,134 / 9,156 (88.8 %) | 0 | 52 | 0 | 42,868 | 5.27x | no | no |
| `EV0021` | event 0021 | event | `0x20`–`0xAC0` | 2,720 | 21 | 2,004 / 2,316 (86.5 %) | 0 | 21 | 0 | 11,331 | 5.65x | no | no |
| `EV0022` | event 0022 | event | `0x20`–`0x1460` | 5,184 | 32 | 3,814 / 4,496 (84.8 %) | 0 | 26 | 0 | 18,687 | 4.90x | no | no |
| `EV0023` | event 0023 | event | `0x20`–`0x3680` | 13,920 | 54 | 11,386 / 12,796 (89.0 %) | 0 | 50 | 0 | 53,847 | 4.73x | no | no |
| `EV0024` | event 0024 | event | `0x20`–`0x21C0` | 8,608 | 34 | 7,022 / 7,890 (89.0 %) | 0 | 33 | 8 | 33,907 | 4.83x | no (16 B match by bytes only) | no |
| `EV0026` | event 0026 | event | `0x20`–`0x9920` | 39,168 | 123 | 32,894 / 52,218 (63.0 %) | 0 | 117 | 5 | 150,913 | 4.59x | no (24 B match by bytes only) | no |
| `EV0028` | event 0028 | event | `0x20`–`0x1E40` | 7,712 | 17 | 6,556 / 7,442 (88.1 %) | 0 | 17 | 6 | 33,424 | 5.10x | no | no |
| `EV0029` | event 0029 | event | `0x20`–`0x5620` | 22,016 | 80 | 18,596 / 34,908 (53.3 %) | 0 | 76 | 2 | 83,620 | 4.50x | no | no |
| `EV002A` | event 002A | event | `0x20`–`0xA00` | 2,528 | 7 | 1,846 / 2,350 (78.6 %) | 0 | 7 | 0 | 8,763 | 4.75x | no | no |
| `EV002B` | event 002B | event | `0x20`–`0xA260` | 41,536 | 119 | 34,304 / 53,934 (63.6 %) | 1 | 97 | 219 | 166,158 | 4.39x | 29/29 calls, 11,746 B | **yes** |
| `SHOOTING` | Sky Chase | area | `0x0`–`0x8CE0` | 36,064 | 228 | 27,644 / 31,164 (88.7 %) | 0 | 219 | 1 | 139,179 | 5.03x | no | no |
| `STG01` | Emerald Coast | area | `0x0`–`0xF3E0` | 62,432 | 359 | 50,160 / 56,760 (88.4 %) | 4 | 274 | 0 | 255,967 | 5.02x | 174/174 calls, 31,456 B | no |
| `STG02` | Windy Valley | area | `0x0`–`0xCDC0` | 52,672 | 286 | 41,650 / 47,704 (87.3 %) | 0 | 199 | 0 | 215,086 | 5.15x | no (4 B match by bytes only) | no |
| `STG03` | Twinkle Park | area | `0x0`–`0x16860` | 92,256 | 340 | 74,754 / 107,138 (69.8 %) | 0 | 297 | 12 | 365,933 | 4.66x | no (16 B match by bytes only) | no |
| `STG04` | Speed Highway | area | `0x0`–`0x11640` | 71,232 | 338 | 56,274 / 64,278 (87.5 %) | 0 | 310 | 2 | 287,254 | 4.80x | no | no |
| `STG05` | Red Mountain | area | `0x0`–`0x13780` | 79,744 | 409 | 63,342 / 71,320 (88.8 %) | 0 | 344 | 3 | 327,731 | 5.02x | no (4 B match by bytes only) | no |
| `STG06` | Sky Deck | area | `0x0`–`0x1BB00` | 113,408 | 456 | 90,710 / 101,054 (89.8 %) | 0 | 428 | 7 | 539,235 | 4.82x | no | no |
| `STG07` | Lost World | area | `0x0`–`0xBFE0` | 49,120 | 311 | 37,616 / 42,536 (88.4 %) | 0 | 254 | 3 | 195,546 | 4.98x | no | no |
| `STG08` | Ice Cap | area | `0x0`–`0x14060` | 82,016 | 382 | 63,344 / 70,604 (89.7 %) | 0 | 350 | 0 | 335,260 | 4.90x | no | no |
| `STG09` | Casinopolis | area | `0x0`–`0x23DA0` | 146,848 | 779 | 115,478 / 128,318 (90.0 %) | 0 | 740 | 0 | 602,067 | 5.00x | no | no |
| `STG10` | Final Egg | area | `0x0`–`0x16B60` | 93,024 | 477 | 73,768 / 84,464 (87.3 %) | 0 | 417 | 3 | 359,179 | 4.83x | no | no |
| `STG12` | Hot Shelter | area | `0x0`–`0x18020` | 98,336 | 575 | 76,228 / 87,552 (87.1 %) | 0 | 502 | 2 | 397,613 | 5.17x | no (8 B match by bytes only) | no |
| `SUMMARY` | file select and result screens | area | `0x20`–`0x3460` | 13,376 | 73 | 10,332 / 17,976 (57.5 %) | 0 | 73 | 2 | 55,201 | 5.34x | 2/2 calls, 270 B | **yes** |

Totals (one file per slot is resident at a time):

| Slot | Files | SH-4 code bytes | MIPS `.text`, sum | Largest file |
|---|---|---|---|---|
| area | 23 | 1,387,776 | 5,632,505 | `STG09`, 602,067 |
| area 2 | 2 | 108,416 | 345,236 | `ADV0100`, 187,352 |
| event | 32 | 488,448 | 1,925,366 | `EV002B`, 166,158 |
| **all** | **57** | **1,984,640** | **7,903,107** | |

**Against README §7.** The main image re-measured now is 3,891,679 B of
MIPS `.text`: 4,313 C functions, including the wrapped originals and the
table. It was 3,655,809 B at M1, before the call-sync fixes and the new
seeds. The overlays change what the EBOOT has to hold:

- **Sum of all 57 overlays: 7,903,107 B (7.9 MB).** Linked into one image
  next to the main code, that is 11.8 MB, far past the 4.7 MB target. The
  overlays cannot all be resident.
- **Largest resident set: 768,225 B (0.77 MB).** The game keeps one file
  per slot. The worst cases are `STG09` (602,067 B, the largest area file)
  plus `EV002B` (166,158 B, the largest event), and the Egg Carrier at
  18,979 + 187,352 + 166,158 = 372,489 B. Main image plus the worst
  resident set is **4,659,904 B (4.66 MB), just under §7's 4.7 MB target**,
  with no margin.

This confirms M1's note: each overlay has to be loaded with its file, as a
PRX or a code blob paged in by the loader hook, rather than linked into the
EBOOT. The M1 overlays grew 3 % (`ADV00` 332,721 → 343,207 B) because the
main image's call-sync fixes add stores at every call into it.

**Trace coverage.** 10 of the 57 files ran in the oracle. For each of them
every executed call target is now a function start; plain discovery missed
7 in `B_CHAOS0` (49 of 56) and 4 in `STG01` (170 of 174). Those 11 are now
seeds. Several of the overlay seeds are return addresses that the trace
records as dynamic jump targets (code right after a `jsr`); they are
harmless as entries. The other 47 files were reached by neither the trace
nor the scripted pad. They rely on static discovery alone, with the numbers
above.

## 5. What the overlays changed in the main image

The new files call 57 block-A addresses and 9 SDK addresses that had no
function. Most block-A ones start right where discovery had ended the
previous function, so nothing in block A calls them. All are handled the way
M1 did:

- the 57 block-A addresses are in
  `recomp/sa1_us_1005/overlay_main_seeds.txt` and in the main config's
  `extra`; the main image is rediscovered (9,592 functions, was 9,558) and
  re-emitted (4,313 C functions, 446,518 instructions);
- the 9 SDK entries (`0x8C5FC94C`, `0x8C5FCDF6`, `0x8C5FCEDA`,
  `0x8C5FD044`, `0x8C5FD358`, `0x8C5FF24C`, `0x8C5FF2A4`, `0x8C60E14C`,
  `0x8C63A9DC`) are in `sdk_entry_points.tsv` with 0 block-A sites, and
  `hle/seam.h` and `hle/seam_stubs.c` are regenerated (300 entries). They
  are logging stubs like every unexamined entry.

After this, `tools/overlays.py build` reports no external target without a
function.

## 6. The scripted desktop run and Chaos 0

Route: the M1 acceptance presses (`--press 500:8:4 --press 900:8:4
--press 1300:4:4 --press 1700:4:4 --press 2100:4:4`). With the M1 build
(`hle/ninja/ta_stubs.c` as committed):

| Frame | Event |
|---|---|
| 2458 / 2465 | `ADV00`, then the opening event `EV0001` |
| 3119 / 3122 | `B_CHAOS0` and the Chaos 0 intro event `EV002B` are installed. Before this step, the run trapped here. |
| 6485 | `EV002B`'s script task returns; the fight starts |
| 7192, 7902, 8612 | Sonic dies on every life at the same frames, with or without input; game over at 9306, back to the title at 11708 |

**Cause: no ground collision.** Sonic's object (main `0x8C0A77BC`, data
laid out like SADX's EntityData1: action at +0, status at +4, position at
+0x20, collision at +0x38) stays in action 12 with the grounded bit clear.
During the cutscene he falls through the arena to y = −2,712. The ground
query `0x8C091C20` walks a collision list built by `0x8C0522C0` (list at
`0x8C757EF4`, count `0x8C19E964`), and that count is always 0.
`0x8C0522C0`, `0x8C052518` and `0x8C0528AE` are wrapped as no-op "TA
writers" (`hle_wraps.tsv`, `hle/ninja/ta_stubs.c`). They are not TA
writers: the M0 scan (`docs/seam.md` §5) matched the constant
`0x10000000`, which here is a collision-flag bit (`tst r3,r2`). The other
five functions in that §5 list (`0x8C07E0AE`, `0x8C07E480`, `0x8C07E642`,
`0x8C07E9E6`, `0x8C080FA2`) also only compare or test `0x10000000` as a
value; none of the eight touches `0xE0000000` or `pref`. The 24
store-queue writers in the same file are genuine. `hle/ninja/` belongs to
the M2 renderer session, which has the finding. Its `ta_stubs.c` rewrite
(not committed when this was written) lets every wrapped body run, which
restores the three collision functions. The narrower fix is to drop the
eight rows from `hle_wraps.tsv` and their `TA_STUB` lines together, then
re-emit the main image.

**With the eight restored** (a scratch worktree where the eight wrappers
call the recompiled bodies; not committed): the opening runs longer
(`B_CHAOS0` at frame 5299, the fight from 8665). Sonic stands (action 1,
grounded), jumps (8), homing-attacks (14) and is hit (82). Chaos 0
(`0x8C9023CE`, data action at +0) walks through its states: 1, 2, 6, 7, 9,
11, 15, 16. Periodic A presses did not beat it. I tried 18 schedules (jump
every 30–90 frames, second press 8–20 frames later) for up to 30,000 frames
each; Sonic loses every life, and A on the game-over screen continues into
a fresh fight. The homing attacks do head for Chaos: at frames 9160–9180
Sonic flies from z = 264 to 345, past Chaos at z ≈ 322. Winning needs input
that steers toward Chaos when it is vulnerable, and a check of the boss's
health. That is a follow-up; the oracle shows the code paths it needs
(`B_CHAOS0` has 63 of the oracle's 67 executed functions running already).
The B_CHAOS0 globals seen so far: the lamp-post hop counter `0x8C95FEC0`
(set to 4, decremented per hop) and Chaos' action history
`0x8C6B6888`–`8A`. Neither is its health.

**Debug aids added** (desktop, compile-time, off by default):
`-DSA1_RAMDUMP` with `SA1_RAMDUMP="F1,F2,..."` and `SA1_RAMDUMP_DIR`
writes guest RAM at those frames (`hle/shinobi/sys.c`; game state, keep it
outside the repo). `-DSA1_PAD_PULSE` with
`SA1_PAD_PULSE="START:END:PERIOD:BUTTONS:HOLD[,...]"` adds periodic presses
on top of `--press`, whose table holds 64 entries (`hle/shinobi/pad.c`).
The game's object lists (`0x8C79F04C` + 4 × level, records with main at +16
and data at +32) were walked from the dumps with a scratch script.

## 7. Open items

- **Win Chaos 0 on the scripted pad**: needs the collision fix above, then
  an input script that aims (stick toward Chaos, homing when it is on the
  ground) or a better-placed press table.
- **47 files have no dynamic coverage.** When later milestones reach them,
  run `tools/overlays.py build NAME` with a new trace or with the desktop
  run's `--trace-funcs` output. Any call into code with no function traps
  loudly ("indirect call to an address with no function"), which is the
  signal to add a seed.
- **Unresolved indirect sites** (Unresolved column) go through `sa1_call`
  and trap only if the target has no function. `EV002B`'s 219 come from its
  event-script tables.
- **Per-slot loading on the PSP** (§4): needed before M4.
- `ALERTWND.PRS`, `EV0100`+ (attribution), and the other characters'
  stages, bosses and events: the same two commands each.
