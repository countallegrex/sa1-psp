# f4: Emerald Coast profile (sa1-psp-g 75498336, 2026-10-24, PRX of 87d4bea)

Console PSP-3001, ABBA x3, frame skip off, --frames 460, mean of f300/f450, medians (ms per drawn frame; each drawn frame = 2 game frames).
Raw: scratchpad c852cff8-.../scratchpad/f4/ (bench_*.tsv/txt, prof_ec*.bin/txt, gamecode_classes.md).

## Split
| row | ec | ec noland | ss | ss noland |
|---|---|---|---|---|
| frame | 80.21 | 52.21 | 42.16 | 26.27 |
| kamui parse+record | 24.87 | 11.36 | 10.41 | 2.99 |
| list replay | 6.94 | 2.92 | 2.36 | 0.71 |
| GE wait | 0.01 | 0.01 | 0.01 | 0.01 |
| rest (game thread: game code, ninja xform/TA writers, sound) | 48.4 | 37.9 | 29.4 | 22.6 |
| GE busy (no pipeline, stall withheld) | 5.20 | - | 3.99 | - |
Land cost: ec 28.0 ms, ss 15.9 ms. GE is far from the limit in both.

## Sampled profile, ec f300-450 (6155 samples at ~1 ms over 75 drawn frames; samples/75 = ms)
By group, ec (noland): renderer draw_gu.c 22.0 (9.1) | game code 14.1 (13.6) | kamui.c 9.0 (6.0) | ta_direct 7.4 (3.6) | ta_xform 6.6 (3.6) | ninja math 5.8 (5.6) | sound snd_core+adx 2.9 (3.0) | pspsdk 2.5 (1.1) | ta_direct_clip 2.5 (0.5) | ta_mesh 2.1 (1.4) | other/kernel 2.7 (2.3) | sh4_rt 1.1 (1.0) | native_land 1.1 (0.6).
Shares (ec): renderer+TA path (draw_gu, kamui, ta_*, pspsdk) ~52 ms = 63 %; game code + ninja math 20 ms = 24 %; sound 3.5 %; HLE runtime/other ~5 %. libm (game's own SHC acos/asin/atan) 1.2 ms inside game code.
Land's 28 ms by group: draw_gu 12.9 (strip_emit, draw_strip, clip_plane: 22050 guard-band-clipped strips per 150 f vs 4875 noland), ta_direct+clip 5.8, kamui 3.0, ta_xform 3.0, pspsdk 1.4, ta_mesh 0.75, native_land 0.5, game 0.5.

Top 25 (ec ms): strip_emit 9.52, ta_direct_8c037990 7.44, draw_strip 3.92, ta_xform_8c037294 3.48, ta_xform_8c037350 3.16, direct_strip 2.53, ta_direct_8c037ccc 2.39, parse_region 2.36, ta_mesh_8c0376d0 2.13, snd_core_render 2.04, draw_set_state 1.55, apply_state 1.48, draw_end_frame 1.43, clip_plane 1.31, apply_blend_depth 1.27, kernel/pspsdk 1.12, kmRender 0.97, njPushMatrix 0.87, fn_8c091380 (SET range-in) 0.73, native_land_decide 0.67, fn_8c03718c (model cull) 0.60, njPopMatrix 0.60, fn_8c029b00 (sphere sweep) 0.57, strip_split 0.52, flush_strip 0.52.

Game code + math by subsystem (ec / noland, ms; +math share low confidence): model drawing 4.55/4.12 (+2.0) | collision land 2.56/2.44 (+1.6) | collision obj-obj 1.08/0.96 | libm 1.17/1.35 | object logic 2.27/2.45 (+0.4) | player 1.19/1.16 (+1.2) | camera 0.60/0.59 | land draw game side 0.44/0.25 | task loop 0.37 | water/sea 0.19/0.08 | sound driving 0.07 | other 0.41. Water costs nothing in game code; EC's sea is renderer cost. ~20 ms of logic is fixed, independent of the land.

## RAM high-water (heap in use / sbrk / largest free, KB)
ec 32699 / 32742 / 2944 (textures 7670 KB in 725) | ec noland 32251 / 32294 / 3392 | ss 32595 / 32726 / 2944 (textures 8509 in 924). Stack root 9 of 256 KB.

## Reading
EC is renderer-bound on the CPU: the TA/GE list path (kamui parse + ta_direct writers + draw_gu emit/clip/state) is ~52 of 82 ms, land and non-land alike (non-land EC 52 ms vs ss 26: more objects/translucent strips, not game logic). Biggest single levers in order: (1) the strip path itself (strip_emit + draw_strip + state + clip ~19 ms, ta_direct ~10 ms, kamui parse ~5 ms): fewer passes/copies per vertex (TA writer -> kamui parse -> GU emit is three touches); (2) CPU guard-band clipping of land (clip_plane, ta_direct_clip, 22k clipped strips); (3) game-side candidates: SET range-in scan ~1.4 ms, libm native ~1.2 ms, f5(a) cull decision ~0.7 ms.

## f5 prepared on the desktop (uncommitted, hle/ninja/native_land.c only)
(a) Cheap decision prototype: land address-range reject, packed key array + next-model hint (89 % hits), view_out_fast (9 mul centre + 1 dot per plane, no game_matrix); SA1_LAND_CHECK=1 compares old vs new: 0 mismatches on sonic_ec1 (673,786), sonic_to_ec1_start (430,730), and with translucent culled 886,335 ec / 567,262 ss. Console time not yet measured (estimate < 0.2 ms). World-space spheres not feasible as briefed (land objects carry their own matrix).
(b) Root cause (hypothesis, PSP only): not the sort (radix_keys stable, qsort ties by index; desktop 15/15 frames identical with translucent culled). draw_gu.c merges same-state strips into one GE draw and replays the run's offset (specular) draw after the whole run; an out-of-view translucent strip ended the run, without it neighbouring strips merge and S2's base blends before S1's offset. Fix: flush the batch before a translucent strip when offset vertices are pending (strip_emit, one line), then drop the !md->alpha exemption (~3 ms ss). Needs a console picture check.
