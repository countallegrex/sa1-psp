# Launcher (M7)

One executable. The user picks a dump; the launcher produces a Memory Stick
folder. Every step is a separate, resumable, logged stage so a failure in step
5 does not redo steps 1–4.

| # | Stage | Input | Output | Notes |
|---|---|---|---|---|
| 1 | Verify dump | `.gdi` + tracks | hash report | Must match US v1.005. Refuse anything else with a clear message. |
| 2 | Extract | tracks | `work/data/` incl. descrambled `1ST_READ.BIN` | Our own GDI/ISO9660 reader (Python or C), no third-party binary. |
| 3 | Fetch tools | internet, first run only | `toolchain/` | pspdev toolchain release, recompiler release, pinned by version + hash. Cached. |
| 4 | Recompile | `1ST_READ.BIN` + `recomp/sa1_us_1005/` | `recomp/out/*.c` | Seam table decides native vs recompiled. |
| 5 | Compile | `recomp/out/` + `hle/` + `backend/psp/` | `EBOOT.PBP` | CMake + psp-gcc, `-Os`, parallel. Expect minutes. |
| 6 | Convert assets | `work/data/` | `out/PSP/GAME/SA1PSP/data/` | Textures to PSP formats, ADX kept, SFX clips, PMF for FMV. |
| 7 | Package | 5 + 6 | `out/PSP/GAME/SA1PSP/` | ICON0/PIC1 from our own art. "Copy this folder to your Memory Stick." |

Rules:

- The launcher ships **no game-derived bytes**. Stages 4–6 run on the user's machine.
- GPL tools are downloaded and executed as separate processes, never linked.
- Everything the launcher does is also runnable from the command line
  (`launcher/cli.py <dump>`), so CI and headless users get the same pipeline.
- Until M7, the stages exist as scripts under `tools/` and are run by hand.
