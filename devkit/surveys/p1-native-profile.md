# p1: native-default profile, ec and ss (sa1-psp-h bce9380b gen 4, 2026-10-25)

Binary: 294f468 + p1 class instrumentation (census-gated, default path unchanged; diff saved, not landed: scratchpad 05e73d2b-.../scratchpad/p1-kamui-class.diff). Console PSP-3001, frame skip off, --frames 460, mean of f300/f450, ABBA x3 medians (spreads <= 0.13 ms). Raw: scratchpad .../p1/ (bench_*, prof_*, cls_*).

## 1. Fresh baseline and split (same binary; noland = every land model culled at decide, native and TA-way land alike)

| ms/drawn frame | ec native | ec noland | ec land | ss native | ss noland | ss land |
|---|---|---|---|---|---|---|
| frame | **69.23** | 52.45 | **16.78** | **35.55** | 26.08 | **9.47** |
| kamui parse+record (render_lists) | 16.92 | 12.10 | 4.82 | 5.74 | 2.79 | 2.95 |
| list replay | 7.07 | 1.85 | 5.22 | 2.21 | 0.70 | 1.51 |
| GE wait | 0.01 | 0.01 | 0 | 0.01 | 0.01 | 0 |
| rest (game thread) | 45.24 | 38.50 | 6.74 | 27.60 | 22.58 | 5.01 |

The 68.81 baseline is replaced by 69.23 (it was 69.53 in f7e). The plan's 16.7 ms land residue came from comparing two binaries. On this binary, measured on the same build, it is 16.78 ms, which happens to match.
RAM at exit: ec native heap 33234 KB in use, largest free block 2048 KB (textures 7670 KB in 725). ec noland: 32269 KB, 3072 KB free.

## 2. Who owns the ec land residue (16.78 ms): native minus noland, sampled f300-450 (ms = samples/75)

- **kamui +4.82**, taken from the class split (section 3, scaled):
  - TA-way translucent land (water/shore, collect + sorted draw): 2.15
  - persistent model draws (draw_model from parse, 66/frame): 0.6-1.24 (sampled floor to census scaled)
  - the rest (ta-records, untagged, sort growth, setup): 1.4
- **replay +5.22**, sampled:
  - draw_end_frame body: +2.09
  - apply_blend_depth: +0.83
  - apply_state: +0.71
  - texture binds (sceGuTexImage, TexFlush, slot_of_handle): +0.90
  - cell draws (draw_ge_geo, draw_cell_class): +0.62
  - sh_offset_pass: +0.29
  - select_proj: +0.27
  - sceGuSetMatrix: +0.19
  - The terms sum to about 5.9 against 5.22 because a few of them are partly called from parse.
- **rest +6.74**, sampled, named owners total 6.4:
  - ta_xform +1.99: the 22 TA-way land nodes/frame, 1108 corners
  - game code +1.23: land cull, collision with drawn land
  - native_land +1.09, of which native_land_decide is 0.80
  - ta_direct + clip: +0.95
  - ta_mesh: +0.59
  - sh4_rt / hw / coro: +0.5
- **Attributed by named owner: ~16 of 16.78. GO bar (>= 14) met.** The +8 "rest" in the plan, the part with no known owner, is 6.7 on this binary. It is mainly the TA-way land nodes: ta_xform plus ta_direct/ta_mesh come to 3.5. The rest is the decide step (1.1) and game-side land work (1.2).

## 3. kamui ~16.9 ms ec by strip class (the --strip-census run)

The class split reads the clock once per class change (1225 changes/frame), which inflates kamui from 16.93 to 22.10 (noland: 12.10 to 16.05). The table therefore gives the measured ms with census on, and a scaled column (x0.766) that sums to 16.93. Both ss runs have the same overhead shape. Caveat: the overhead follows class changes, not class time, so the scaling overstates classes that are finely interleaved and understates long homogeneous runs. Overstated: the sorted translucent draw, the alternation between opaque dyn and dyn-env, and single model markers (about two clock reads per model). Read the three-class totals as +-1 ms.

| class (ec native) | census ms | scaled | verts/frame |
|---|---|---|---|
| opaque, dynamic, not env (direct writers, fast path) | 5.97 | **4.58** | 3812 |
| opaque, dynamic, env-mapped | 2.44 | **1.87** | 2564 |
| opaque, TA records (ta_xform/ta_mesh/game writers) | 1.91 | 1.47 | 88 + headers |
| opaque, persistent land models (draw_model) | 1.62 | 1.24 | 66 models |
| opaque, untagged | 0.17 | 0.13 | 165 |
| **opaque total** | 12.13 | **9.29** | |
| translucent dynamic (collect 1.73 + draw 2.71) | 4.44 | **3.40** | 2059 |
| translucent TA-way land, not env (0.69 + 1.20) | 1.89 | 1.45 | 877 |
| translucent TA-way land, env (0.29 + 0.63) | 0.91 | 0.70 | 336 |
| translucent TA records (0.73 + 0.37) | 1.09 | 0.84 | 219 |
| translucent untagged (0.24 + 0.38) | 0.62 | 0.47 | 239 |
| **translucent total** (collect 3.67, sorted draw 5.28) | 8.95 | **6.86** | |
| other (frame setup, ta_runs, radix sort) | 1.03 | 0.79 | |
| punch-through | 0 | 0 | |

The same numbers by the three classes the order asks for:
- **Opaque non-env: 7.4 ms** (dynamic 4.6, TA records 1.5, models 1.2)
- **Translucent: 6.9 ms** (dynamic 3.4, land 2.15, of which env 0.7)
- **Env: 2.6 ms** (1.87 opaque dynamic + 0.70 translucent land); setup 0.8

The plan's 6.1 reading ("kamui's 16 ms sits on translucent and env strips") was half right. The largest single class is **opaque non-env dynamic: 4.6 ms for 3812 verts, about 1.2 us/vert on direct_strip's fast path**, almost all of it draw_strip + strip_emit. f7 slice 1 failed because its writer cost more than this saved, not because the class is small. Env is 2.6 ms in all. Translucent is 6.9 ms, and the sorted draw (5.28 census) costs more than the collection (3.67).

ss native (census 7.13 ms, bench 5.74, scale x0.805):
- persistent model draws: 1.57 ms scaled for 60 models. That figure is inflated by the per-marker clock reads. Sampled, the floor is draw_model alone at 0.65 ms (~11 us/model), rising to 1.36 ms with draw_ge_geo and draw_cell_class. **Range 0.65-1.4 ms, still the largest or second-largest ss class.** In ec, sampled draw_model is 0.60 ms, plus 0.9 for texture binds (TexImage, TexFlush, slot_of_handle).
- opaque dynamic 1.50
- translucent dynamic 1.0
- opaque TA records 0.50
- TA-way land translucent 0.58 (env 0.36)
- other 0.26

ss noland drops the models and the land classes and keeps the rest unchanged.

## 4. Reading for the ranking (the orchestrator's call)

- **ss (-2.2 needed):**
  - Persistent model draws cost about 0.6-1.2 ms in ec and 0.65-1.4 ms in ss (11-26 us per model; see the caveat in section 3), inside parse. The ec profile also shows the texture bind and LRU touch work (TexImage, TexFlush, slot_of_handle, about 0.9) and cell work (about 0.6). Prebuilding the bind into the model's list, or deferring the model draws to replay with no per-model texture work, is a candidate that is identical by construction, worth about 0.5-1 ms in ss.
  - p2 (replay remainder) also hits ss by about 1 ms. Together these cover ss.
  - ss game code + ninja math is about 17.4 ms of 36.6 (math.c alone 5.2): lever 4 is the ss reserve.
- **ec:**
  - p2 is confirmed: the replay land share alone is 5.2.
  - Lever 5 (land residue) is real at about -4: TA-way translucent land through ta_xform, kamui, sort and replay is about 2.15 + ~2 + ~1 ms, and native_land_decide is 0.8.
  - Lever 6 (f7 slices 2-3) targets about 4.3 ms of kamui (translucent dynamic 3.4 + env 1.9 minus overlap) before any writer cost. With slice 1's per-corner writer floor, a realistic net is no more than -1.5.
  - The opaque fast path (strip_emit, 1.2 us/vert) is a better target than a new writer: make the existing emit cheaper.

## 5. Files

- Instrumentation: scratchpad 05e73d2b-.../scratchpad/p1-kamui-class.diff (kamui.c, native_land.c census_source, ninja.h). It was reverted from the shared tree.
- build-psp/sa1psp is the instrumented PRX. Its default path matches 294f468, but rebuild it before the next A/B.
- Batch script: scratchpad p1batch.sh. Profile diff tool: scratchpad pdiff.py.
