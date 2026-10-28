# Profile coverage, 2026-10-28 (orchestrator 7, Opus subagent; self samples)

Sources: private\q0b\prof_{sd3,wv2,chaos0,casino2}_all.txt (L only, b_q0 6e7242e, --frame-skip 60, f300-700); private\p13\prof_ec|ss.bin re-listed with tools/psp_prof.py --top 3000 (full frame, b_p12 9fa53c9, f200-700). Script: orchestrator scratchpad cov.py.

| place | samples | fns | 50 % | 60 % | 70 % | 80 % |
|---|---|---|---|---|---|---|
| Sky Deck 3 (L) | 8791 | 562 | 35 | 54 | 83 | 124 |
| Windy Valley 2 (L) | 8342 | 432 | 28 | 40 | 57 | 83 |
| Chaos 0 (L) | 6786 | 345 | 18 | 26 | 38 | 52 |
| Casino 2 (L) | 6791 | 443 | 15 | 25 | 42 | 74 |
| EC full | 15034 | 534 | 17 | 28 | 47 | 81 |
| SS full | 7633 | 479 | 28 | 42 | 64 | 100 |

First ~65 % by group (% / fn count):

| group | SD3 (68) | WV2 (48) | Chaos 0 (31) | Casino 2 (32) | EC (36) | SS (52) |
|---|---|---|---|---|---|---|
| game fn_8c0 (not walker) | 20.0/25 | 24.7/18 | 5.7/3 | 14.4/9 | 4.4/5 | 13.6/15 |
| walker 8c03f-8c0409 | 3.1/4 | 1.9/2 | 12.9/6 | 0.8/1 | 0.6/1 | 0.7/1 |
| Ninja math | 12.1/14 | 20.4/15 | 17.2/10 | 7.6/7 | 3.9/5 | 8.7/9 |
| sound | 8.8/2 | 3.6/1 | 14.0/3 | 23.6/3 | 4.4/3 | 4.3/2 |
| kamui/TA/model | 5.2/9 | 4.0/4 | 8.4/4 | 8.0/5 | 29.7/10 | 16.3/9 |
| renderer/GE (draw_gu) | 1.0/2 | - | - | 0.6/1 | 18.3/8 | 13.9/10 |
| dispatch | 5.9/4 | 5.3/4 | 2.5/2 | 2.5/2 | 1.4/2 | 2.7/4 |
| collision (sph_native) | 2.8/1 | 2.1/1 | 0.9/1 | 4.0/1 | 0.8/1 | 3.0/1 |
| kernel/libc | 4.4/5 | 2.4/2 | 1.8/1 | 3.1/2 | 1.4/1 | 2.3/1 |
| overlays | 1.9/2 | 0.6/1 | 1.5/1 | 1.0/1 | - | - |

Common core in all four L places (13 fns, 24-36 %): snd_core_render, sdk_8c639c70, sph_native, fn_8c02d2e0, fn_8c040784, kernel 0x88020000, fn_8c057bc0, nj_copy, fn_8c03718c, m_xform, m_invert, sa1_seam_hit_at, sdk_8c639b98. With EC: 10 remain (20-31 % of L, 10.9 % of EC).

Caveats: self time only; EC/SS from the older 9fa53c9 build; Allegrex load stalls land on the consuming instruction.
