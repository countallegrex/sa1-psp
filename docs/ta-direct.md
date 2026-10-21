# The TA bypass (M5): strips from the game's writers to sceGu without the TA bytes

Status: stage 1 is the default (2026-10-20; `--no-ta-direct` turns it off): Station Square 74.40 -> 70.17 ms/frame on the PSP-3001.

## Where the frame goes

On the PSP-3001 in Station Square (74.4 ms/frame at 6ec6ecf) the render path is
about 45 % of the frame. A vertex makes three trips through memory:

1. **The game's TA writers** (recompiled, `hle/ninja/ta_stubs.c`): the model
   renderer at 0x8C036F00-0x8C038E00 gathers already-transformed vertices by
   index and writes one 32-byte TA vertex record each (`fn_8c037990` 7.5 %,
   `fn_8c037294` 3.8 %, `fn_8c037350` 3.0 %, the clipper and the others ~4 %).
   Every store goes through the guest-address macros.
2. **kamui's parse** (`hle/ninja/kamui.c`, `parse_region` 7.5 %) reads the
   records back at kmRender, applies the headers and builds `draw_vertex`
   strips.
3. **draw_gu** (`strip_emit` and the guard-band clipper) turns them into the
   GE's vertices.

## The idea

`fn_8c037990`'s record is `[pcw, x, y, z, u, v, colour, offset]`: after the
control word, exactly `draw_vertex` (`backend/draw.h`). A native writer can
put the vertices straight into a host array and leave **one marker record per
strip** in the TA buffer instead of one record per vertex:

- marker: a 32-byte record of parameter type 6 (reserved on the TA, never
  written by the game): word 1 is the strip's first index in the host array,
  word 2 its vertex count.
- `ta_runs` already steps over unknown records 32 bytes at a time, so the list
  runs are unchanged. `parse_region` takes a marker as a whole strip under the
  current header: for the header shape the writer is made for (packed colour,
  float UV, no sprite or modifier volume) it hands the host array straight to
  `draw_strip`, zeroing the offset colour when the header has none; any other
  header goes through the TA path's per-vertex decode on the same words.
- Order is kept: the markers sit where the strips' records sat, so headers,
  list types, the translucent sort and the 2D ranges see the same sequence.
- The host array is reset with the TA buffers (kmFlip, kmSkipFrame).

The native writer mirrors the recompiled function's control flow, guest
memory side effects (the gbr counters, the stack slot) and register results,
with one difference: r14/r0 (the TA write pointer it returns) advance by 32
per strip instead of 32 per vertex. Callers only carry that pointer forward.

## What it saves, and what it does not

Saved per vertex: the 32-byte guest store, the 32-byte parse read and its
decode. Not saved: the gather itself (index and array reads), `draw_strip`.
Estimate for `fn_8c037990` alone: 3-4 ms/frame. The same pattern then applies
to the other writers that emit plain strips (`fn_8c037294`, `fn_8c037350`,
the clipper last).

## Stages

1. `fn_8c037990` native, behind `--ta-direct`. PPSSPP frames against the TA
   path must be byte-identical; title_menu.tape must pass; measured on the
   console.
2. The next writers by profile weight, each verified the same way.
3. Later: the gather into GE vertices directly (skip `draw_vertex`), once the
   writers are native; and VFPU for branch B's FTRV.

## Risks

- A save state taken between a writer and kmRender would hold markers whose
  host array is not saved. States are taken at frame boundaries (after kmFlip
  reset the buffers), so none should be in flight; to be checked with
  `replay_test.py --savestate` before the flag becomes the default.
- The game reading its own TA records back: not seen (the clipper reads the
  transformed arrays, not the TA buffer).
