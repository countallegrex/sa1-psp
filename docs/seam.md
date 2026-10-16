# The SDK seam in `1ST_READ.BIN` (US v1.005)

Where the Katana SDK lives inside the executable, and which of its entry
points the game calls. Everything below was measured on the dump identified
in `recomp/sa1_us_1005/README.md` (SHA-256 `45bc5d3c…0367a`), on
2026-10-16, with string scans, literal-pool cross-references and
Dream-Recomp's discovery. No SDK library, header or documentation was used;
names are behavioural guesses with a confidence level attached.

Addresses are in P1 spelling (`0x8C…`). Dream-Recomp's output uses the P0
alias (`0x0C…`) for the same bytes.

## 1. Block layout

The file is one flat image loaded at `0x8C010000`. Code sits in two blocks
separated by 5.2 MB of game data.

| Block | Range | Size | What | Confidence |
|---|---|---|---|---|
| A | `0x8C010000`–`0x8C111200` | 1,053,184 B | Game code. The first 0x100 bytes are the Katana loader stub (bracketed by the library-handle start/end marker strings), then game code. | high |
| A-tail | `0x8C10BE78`–`0x8C111192` | ~21 KB | Katana pieces linked among the game objects: real-time clock access (`0xA0710000/4/8`), BIOS syscall shims (vectors `0x8C0000B0`, `0xB8`, `0xBC`), G1/G2 DMA (`0xA05F7800`, `0xA05F78BC`, `0xA05F688C`). The `syStart Ver 1.07` string follows at `0x8C1111BD`. | medium (start address is the first hardware-touching function; may begin a little earlier) |
| data | `0x8C112B84`–`0x8C5FA000` | 5.2 MB | Game data. Every address in SA Tools' `1st_read_data.ini` (543 entries, `0x102B84`–`0x5EB890` relative) falls here. Version strings of five libraries sit at `0x8C160075`–`0x8C16025D` (ntUtl, syRtc, ADXF, syCbl, syG2), so those libraries' constant data was linked with the game's. | high |
| B | `0x8C5FA000`–`0x8C66C000` | 466,944 B | Katana SDK code. First real function around `0x8C5FBB0C`; `0x8C5FA000`–`0x8C5FA67C` is a pointer table. | high |
| tail | `0x8C66C000`–`0x8C67C800` | 67 KB | SDK constant data: 26 library version strings, error messages, the NEC copyright block of the ADX decoder. | high |

Evidence for the split: per-8 KB density of SH-4 prologue/epilogue opcodes
(`sts.l pr,@-r15`, `lds.l @r15+,pr`, `rts`, `mov.l Rn,@-r15`) is 20–170 per
block inside A and B and 0–2 everywhere else; discovery with the pointer
pass restricted to A and B finds 9,568 functions and decodes 88.4 % of their
byte range; without the restriction it finds 42,000 more "functions" inside
model data.

## 2. Libraries present

Version strings, in file order. Their position tells which link group the
library belongs to.

| Address | String | Group |
|---|---|---|
| `0x8C1111BD` | syStart Ver 1.07 (Mar 1999) | A-tail const data |
| `0x8C160075` | ntUtl Ver 2.00 (Aug 1999) | game data |
| `0x8C160105` | syRtc Ver 1.02 | game data |
| `0x8C160135` | ADXF Ver.5.50 (CRI) | game data |
| `0x8C160165` | syCbl Ver 1.10 | game data |
| `0x8C16025D` | syG2 Ver 1.01.02 | game data |
| `0x8C66C31D` | syFbr Ver 1.11 | tail |
| `0x8C66C351` | syCache Ver 1.52 (Jul 1999) | tail |
| `0x8C66C381` | syCfg Ver 1.14 | tail |
| `0x8C66C3E0` | Shinobi Library for Dreamcast Version 1.41 | tail |
| `0x8C66C541` | gdFs Ver 1.07 | tail |
| `0x8C66C601` | pd Ver 1.46 | tail |
| `0x8C66C659` | syMalloc Ver 1.35 | tail |
| `0x8C66C689` | syHw Ver 1.06 | tail |
| `0x8C66C999` | syTmr Ver 1.03 | tail |
| `0x8C66C9C5` | pdLcd Ver 1.03 | tail |
| `0x8C66CA31` | bu Ver 1.44 (VMS file system) | tail |
| `0x8C66CAAD` | fmMng Ver 1.03 | tail |
| `0x8C66CADD` | syBt Ver 1.04 | tail |
| `0x8C66CCA9` | Ninja Ver 01000081 (Mar 1999) | tail |
| `0x8C66D001` | ADXT Ver.5.51 (CRI) | tail |
| `0x8C66D29D` | syChain Ver 1.35 | tail |
| `0x8C66D2D5` | syInt Ver 1.35 | tail |
| `0x8C66D309` | sySq Ver 1.35 | tail |
| `0x8C66D375` | kd Ver 1.45 | tail |
| `0x8C66D5BD` | syBtCfg Ver 1.02 | tail |
| `0x8C66D5ED` | pdVib Ver 1.01 | tail |
| `0x8C671B01` | KAMUI Ver 1.06 (Feb 1999) | tail |
| `0x8C671F69` | sd Ver 1.00.17 (sound driver interface) | tail |
| `0x8C6723F9` | mwRna Ver.1.18 (CRI middleware) | tail |
| `0x8C672445` | SJ Ver.5.45 (CRI stream joint) | tail |

Debug strings in the tail name three source files of the `sd` sound driver
(driver info, host-command flush, multi-unit download) and carry the
numbered ADX/mwRna error messages, which confirms the sound stack is Katana
`sd` + CRI ADXT/mwRna/SJ. A block of KAMUI configuration key names (region
array, object list, ISP/TSP parameter and vertex buffer sizes, TA status
polling, render and vsync time-outs) shows KAMUI is the PowerVR layer under
Ninja. No version string is referenced from any discovered function, so
they cannot be used to locate code directly.

## 3. Where hardware is touched

Literal-pool words loaded by `mov.l @(disp,PC)` inside discovered functions,
classified by address. This is the strongest "this is SDK" signal.

| Hardware | Functions in A (game) | Functions in B (SDK) |
|---|---|---|
| PowerVR registers `0xA05F8xxx` | **0** | 13, pages `0x8C604000`, `0x8C646000`, `0x8C650000`–`0x8C653000` |
| VRAM `0xA4/0xA5xxxxxx` | 0 (one false positive, a float constant) | 8, pages `0x8C605000`, `0x8C651000`–`0x8C653000`, `0x8C665000` |
| Holly system regs `0xA05F6xxx/7xxx` (Maple, GD-ROM, DMA) | 4, all in A-tail | 23 |
| TA FIFO `0x10000000` | **8** (see §5) | 18 |
| AICA `0xA07xxxxx` | 5: four RTC reads in A-tail, one `0xA0720009` at `0x8C053B60` (G2 bus/AICA access from game code, to check in M1) | 13, pages `0x8C604000`, `0x8C65A000`, `0x8C666000`–`0x8C66A000` |
| BIOS syscall vectors `0x8C0000xx` | 16, all in A-tail (`0x8C10C0D0`–`0x8C10DA78`) | 6 |

Reading of the B pages: `0x8C604000`–`0x8C606000` is the hardware
initialisation core (PVR, AICA, Holly and BIOS together; Shinobi `syHw`),
`0x8C641000`–`0x8C653000` is the PowerVR/KAMUI layer (register writes, VRAM,
TA), `0x8C654000`–`0x8C661000` builds TA display lists (Ninja draw path),
`0x8C65A000` and `0x8C666000`–`0x8C66A000` talk to the AICA (sound driver
`sd`). Confidence: medium; it is consistent with the version-string order but
no boundary has been pinned to a function.

## 4. Entry points the game calls

Every pool word in block A whose value is an even address inside block B is
a call target (SHC emits `mov.l @(disp,PC),Rn; jsr @Rn` for every
out-of-range call). Result: **289 distinct targets, 268 of them exact
function entries found by discovery, 7,738 call sites.** The full list with
site counts is `recomp/sa1_us_1005/sdk_entry_points.tsv`.

Distribution by SDK page (sites):

| Page | Targets | Sites | Reading |
|---|---|---|---|
| `0x8C638000`–`0x8C63A000` | 49 | 5,688 | Ninja matrix and vector math (73 % of all SDK calls) |
| `0x8C63C000`–`0x8C645000` | 83 | 780 | Ninja model/texture/draw API |
| `0x8C605000`, `0x8C608000` | 28 | 652 | Shinobi core (two hot entries, 249 and 209 sites) |
| `0x8C600000`–`0x8C604000` | 45 | 167 | Shinobi system/pad/timer |
| `0x8C5FB000`–`0x8C5FF000` | 31 | 133 | Shinobi startup/task layer |
| `0x8C60D000`–`0x8C611000`, `0x8C621000` | 16 | 220 | mixed (one 128-site entry at `0x8C60EDF0` copies a 4-float vector to a global) |
| `0x8C65A000`–`0x8C65E000` | 13 | 59 | sound (`sd`) |

Named entries (from disassembly of the function body; "high" means the
semantics are unambiguous, "medium" means the family is certain but the
member is not):

| Address | Sites | Name | Confidence | Why |
|---|---|---|---|---|
| `0x8C639C70` | 566 | `njPushMatrix` | high | Saves the 16 floats of XMTRX to a stack through paired `fmov` under `fschg`, increments a depth counter, checks against a limit, uses `movca.l` to allocate the cache lines. |
| `0x8C639B98` | 621 | `njPopMatrix` | high | Subtracts `n` from the depth counter, rewinds the stack pointer by `n*64`, reloads XMTRX. |
| `0x8C639F5C` | 655 | `njRotate{X,Y,Z}` | medium | `fsca` on the angle, builds two axis vectors with ±sin/cos, `ftrv` both through XMTRX, writes them back. `r4 == NULL` means "current matrix". Three functions share this shape (`0x8C639F5C`, `0x8C639EC8`, `0x8C63A1CC`); which is X, Y, Z follows from the lane assignment and is left for M1. |
| `0x8C639EC8` | 440 | `njRotate{X,Y,Z}` | medium | as above |
| `0x8C63A1CC` | 421 | `njRotate{X,Y,Z}` | medium | as above |
| `0x8C63A75C` | 288 | `njScalor` | high | Loads a 3-vector, `fipr` with itself, `fsqrt`. |
| `0x8C63A94C` | 238 | `njUnitVector` | high | `fipr`, `fsrra`, scales the three components in place. |
| `0x8C63A804` | 281 | `njCalcPoint` | medium | Matrix (or XMTRX) times a 3-vector with w = 1 (`fldi1`). |
| `0x8C638F28` | 263 | `njCalcVector` | medium | Same with w = 0 (`fldi0`). |
| `0x8C63A970` | 224 | `njSin` | medium | `extu.w r4; lds r4,fpul; fsca fpul,dr0; rts` — returns sin in fr0. |
| `0x8C63A9B8` | 237 | `njSin`/`njCos` family | low | Same body with `fsca` in the `rts` delay slot. |
| `0x8C605628` | 249 | unnamed | low | 12-way `braf` switch on `r5`; a Shinobi "get/set by id" entry. |
| `0x8C608CCC` | 209 | unnamed | low | Returns −1 on `NULL`, otherwise continues; string or list helper. |

The remaining 276 targets are unnamed. The practical seam is therefore: the
289 addresses in the TSV become `native` in the recompiler config, the rest
of block B is dropped, and block A is recompiled. Names are a convenience
for the HLE author, not a requirement.

58 pool words in B point into A: callbacks the SDK stores (VBlank handler,
pad callbacks, task entry points). They are the places where the native
layer must call back into recompiled code.

## 5. Leaks: game code that bypasses the SDK

Eight functions in block A load the TA FIFO address `0x10000000` directly:

| Function | Cluster |
|---|---|
| `0x8C0522C0`, `0x8C052518`, `0x8C0528AE` | one object at `0x8C0522xx`–`0x8C0529xx` (uses `jsr` into B first, then writes vertices itself) |
| `0x8C07E0AE`, `0x8C07E480`, `0x8C07E642`, `0x8C07E9E6` | one object at `0x8C07Exxx` |
| `0x8C080FA2` | single |

These are the "TA-vertex-to-GE translator" cases README §4 rule 3 allows
for. They are bounded: 8 of 7,206 game functions. One more game function
(`0x8C053B60`) loads `0xA0720009`, which is a G2-bus address in the AICA
range; what it does is an M1 question.

No game-block function touches PowerVR registers or VRAM. Ninja and KAMUI
own the display entirely, which is the precondition for replacing them.

## 6. What this does not yet give

- **Library boundaries inside B** are page-level guesses. Byte-pattern
  signatures from Katana static libraries would pin them in an hour; we have
  none and did not look for any (README §0).
- **Executed-function list.** Flycast is not installed (not on winget), so
  the title → Station Square → Emerald Coast trace that README §8 asks for
  has not been captured. Static discovery found 7,206 game functions; the
  dynamic comparison is the open half of the M0 exit test.
- **Which game functions are SHC runtime** (`__divls`, `memcpy`, …). They
  are in block A and get recompiled like everything else; naming them is
  optional.
