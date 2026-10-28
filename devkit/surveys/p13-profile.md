# p13: fresh profile on 9fa53c9, ec and ss (sa1-psp-h bce9380b gen 11, 2026-10-26)

Binary: b_p12 = 9fa53c9, byte-identical to the card build's PRX. PSP-3001, frame skip off. Raw files are in `private\p13\` (prof_ec/ss.*, bench-*.txt).
- Profiles: frames 200-700, ~1 kHz sampling (ec 15034 samples, ss 7633).
- Baseline: --frames 460, mean of f300/f450, same binary run 4x.
- ms = sample share x frame time. No code changes.

## Baseline (same binary)

| ms/drawn frame | ec | ss |
|---|---|---|
| frame | **60.09** (60.05-60.14) | **30.83** (30.79-30.87) |
| kamui parse+record (TA row) | 13.72 | 4.75 |
| list replay | 3.55 | 1.62 |
| GE wait | 0.01 | 0.01 |
| rest (game thread) | ~42.8 | ~24.5 |

- **ss is under the target**, 30.8 against 33.3. The gap is ec: 26.8 ms.
- Noise warning: two of the four ec runs (variant "b", same binary) read 60.55 and 61.50. Same-binary runs can be bimodal by ~1 ms in ec. Keep ABBA x3 and check the ranges.

## Top 10 buckets (ms/frame)

| # | bucket | ec | ss | main functions (ec) |
|---|---|---|---|---|
| 1 | game code (recompiled) | 13.2 | 9.8 | spread thin: fn_8c091380 0.9, fn_8c03718c 0.6, fn_8c057bc0 0.4, fn_8c056738 0.4, fn_8c040784 0.4 |
| 2 | renderer draw_gu.c | 11.8 | 4.3 | strip_emit_k 4.0, draw_strip 2.4, apply_state 1.2, draw_set_state 1.0, draw_end_frame 0.8, apply_blend_depth 0.7, draw_model 0.55 |
| 3 | ta_xform (dynamic models into TA records) | 5.8 | 1.4 | ta_xform_8c037294 3.3, _8c037350 2.5 |
| 4 | kamui parse | 5.6 | 1.5 | parse_region 1.9, direct_strip 1.2, sdk_8c642440 0.8, radix_keys 0.4 |
| 5 | Ninja math (hle/ninja/math.c) | 5.0 | 3.9 | sdk_8c639c70 0.65, sdk_8c639b98 0.5, m_rot_x/y/z 1.0, m_invert 0.4, m_xform 0.36 |
| 6 | ta_direct + clip | 4.9 | 1.2 | ta_direct_8c037990 4.0, _8c037ccc 0.8 |
| 7 | sound (snd_core + adx) | 2.7 | 1.4 | snd_core_render 1.9, tone_sample 0.4, adx 0.4 |
| 8 | other (kernel/pspsdk modules, sa1_off) | 2.5 | 1.5 | outside the PRX 0.86, sa1_off 0.46 |
| 9 | ta_mesh | 2.3 | 0.8 | ta_mesh_8c0376d0 |
| 10 | native_land | 1.6 | 1.1 | native_land_decide 0.8, find_model 0.3 |

Smaller buckets:
- sh4_rt dispatch (sa1_call, fn_lookup, seam, mmio): 1.2 ec / 0.76 ss
- sphere_sweep: 0.5 ec / 0.95 ss
- draw.h cells: 0.6 ec / 0.8 ss

The TA-way dynamic-model pipeline (ta_xform + ta_direct + ta_mesh + kamui + the strip half of draw_gu) is still about 25 ms of ec.

## Next 5 levers, ranked

Estimates are the raw figure x0.5.

| # | lever | ec | ss | hours | risk | exact? |
|---|---|---|---|---|---|---|
| 1 | **Per-strip state dedupe**: kamui calls draw_set_state per strip (~2250/frame in ec); how often the header repeats is unmeasured (count it first, 15 min); compare the header words and skip draw_set_state/apply_state/apply_blend_depth when unchanged (raw ~1.5 of 2.9 ec) | -0.75 | -0.2 | 2-3 | low | yes (byte-identical pictures) |
| 2 | **Ninja math on the VFPU**: the matrix stack routines (rot x/y/z, invert, xform, sdk_8c639c70/b98/a878) as VFPU code (raw ~50 % of 5.0 ec / 3.9 ss) | -1.25 | -1.0 | 4-6 | medium | likely not: VFPU rotation/inverse probably differ in the last ulp, so tolerance gate (check mode + segments, v40/v43); mul/add-only routines may be exact, try first |
| 3 | **Opaque bypass strips classified at write time**: ta_direct already has each vertex in registers; it stores guard/ocolor/w-range in the marker, so strip_class and its first-touch misses go (~330 samples) and direct_strip's dispatch thins (raw ~1.5 ec) | -0.75 | -0.3 | 4-6 | low-medium (ta_direct is hot and fragile) | yes (identical by construction) |
| 4 | **Sound mixer on the Media Engine**, done together with a1 (PSP audio output): the 2.7 ms ec / 1.4 ss leave the main CPU (raw ~2.5 ec) | -1.25 | -0.7 | 8-12 | high: ME code, hardware-only tests, cache coherency | yes (same mixer code) |
| 5 | **Recompiler quality** on the 13.2 / 9.8 ms of game code: cross-block flags, dead stores, cheaper sa1_call/fn_lookup dispatch (raw ~10 %) | -0.65 | -0.5 | 8-15 | medium | yes (tapes --exact) |

Runner-ups:
- native_land_decide (f5a target < 0.2 ms): ~-0.3 both, 1-2 h, exact.
- radix_keys / sort setup: ~-0.2.

## Reading

- The five together come to ~-4.6 ms ec. They cannot close a 26.8 ms gap: incremental levers end around 55 ms ec.
- Reaching 33.3 in ec needs a structural change to the dynamic-model path. That path costs ~25 ms (ta_xform 5.8 + ta_direct 4.9 + ta_mesh 2.3 + kamui 5.6 + strips in draw_gu ~7). It should write GE vertices directly instead of going TA record -> parse -> draw_vertex -> gu_vtx.
- f7 slice 1 was the first try at that and missed its bar (-0.87 ec). It took only one strip class and kept the parse.
- I recommend the orchestrator and Fable re-plan that collapse with these numbers before more single levers. Levers 1 and 3 overlap with it: keep them small, or fold them into the collapse.
