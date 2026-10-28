# (1b) Model cost concentration: Emerald Coast (desktop census, sa1-psp-h gen 12, 2026-10-27)

**Question (the maintainer).** Could a native path for a few models capture most of the dynamic-model gain?

**Short answer.** By type, the cost is concentrated: the top 3 object types hold 43-52 % of the dynamic-model corners in Emerald Coast, and the top 5 hold 62-75 %. Over 50 % at the top 5, as asked. Two things cut that down:
- The biggest single type is Sonic. He is posed (only half his points are unchanged between frames), so he is the hardest one to cache.
- The rest are 5-15 different models. Only rings and a few EC stage objects are one model drawn many times.

A "select few" native path for the rigid EC stage objects plus rings is about 65 % of the running corners (~11.7 ms of the ~18 ms) if it handles env-mapped sets, or ~42 % (~7.5 ms) without env. Both are gross figures, before the new path's own costs (m0.5: ~10-22 us per item on the PSP).

**Method.**
- Reused the m0 census (`private\m0\census.diff`) and added a caller signature per model node. On the desktop, each node takes a host backtrace; the recompiled function names on it are the guest call stack.
- The object type is the first game or overlay function above the generic Ninja walkers (0x8C037098/03700C/036BC0/040784/041A2E...).
- Per type: draws, corners split by texture/env/list, distinct models, and rigidity (points unchanged since the last frame).
- ms is the corner share x 18 ms. Per-node cost and the env path's higher cost per corner are not modelled.
- Windows:
  - idle: `--stage ec`, f200-460
  - running: `--stage ec` with the stick held forward, f260-1100
  - Chaos 0: `ec_scripted.tape` f5460-5900. **Correction:** that window is the Chaos 0 fight (b_chaos0/ev002b overlays), not "running" as m0's census.md said.
- Patch, script and TSVs are in `private\s2c\`. No code landed; the tree was restored.

## Running (EC act 1, 421 frames, 6965 node corners/frame)

| # | type (display fn), my guess | draws/f | corners/f | share | cum | ~ms | models (draws per model/f) | tex/env % | op/tr % | rigid % |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | fn_8c0a94b2 = **Sonic** (0x0C1Cxxxx, welded body 0x0C1D9B60) | 25.4 | 1304 | 18.7 | 18.7 | 3.4 | 30 (0.8) | 100/0 | 56/43 | **50** |
| 2 | ovl_stg01_8c907ee0 (EC object, mostly translucent) | 9.5 | 933 | 13.4 | 32.1 | 2.4 | 3 (3.2) | 100/0 | 30/69 | 99 |
| 3 | ovl_stg01_8c902d08 (EC object, half env) | 6.1 | 776 | 11.1 | 43.3 | 2.0 | 5 (1.2) | 43/56 | 100/0 | 99 |
| 4 | fn_8c0f3eaa (13-part hierarchy, common code) | 15.0 | 668 | 9.6 | 52.9 | 1.7 | 13 (1.2) | 95/0 | 95/4 | 82 |
| 5 | fn_8c0fdce0 (15-part, common code) | 15.9 | 641 | 9.2 | 62.1 | 1.7 | 15 (1.1) | 94/0 | 99/0 | 99 |
| 6 | ovl_stg01_8c90b20a (one model, env) | 4.1 | 551 | 7.9 | 70.0 | 1.4 | 1 (4.1) | 14/85 | 85/14 | 99 |
| 7 | ovl_stg01_8c908336 | 6.5 | 479 | 6.9 | 76.9 | 1.2 | 3 (2.2) | 100/0 | 40/59 | 99 |
| 8 | ovl_stg01_8c908114 | 6.3 | 461 | 6.6 | 83.5 | 1.2 | 3 (2.1) | 100/0 | 26/73 | 99 |
| 9 | fn_8c087b80 -> fn_8c086d00 = **rings** in a group (0x0C1A21DC, env) | 3.8 | 392 | 5.6 | 89.1 | 1.0 | 1 (3.8) | 0/100 | 100/0 | 100 |
| 10 | fn_8c086d00 = **rings**, single | 2.9 | 297 | 4.3 | 93.4 | 0.8 | 1 (2.9) | 0/100 | 100/0 | 99 |

The other 8 types make up the last 6.6 %.

## Idle at the start (131 frames, 8710 corners/frame): top 5 = 75 %

- ovl_stg01_8c902d08: 21.6 %
- Sonic: 16.2 % (73 % rigid while idle)
- ovl_stg01_8c907ee0: 14.5 %
- fn_8c0fdce0: 13.8 %
- ovl_stg01_8c90b20a: 9.2 %
- rings: 7.0 %, single model drawn 6x/frame

## Chaos 0 fight (221 frames, 5419 corners/frame): top 3 = 85 %

- fn_8c04bd9e: 40.3 %. 28 models x 1.7 draws, 98 % rigid.
- ovl_ev002b_8cb8366a: 31.6 %. Event overlay, 100 % translucent, 36 % posed.
- ovl_b_chaos0_8c904c40: 13.1 %. Chaos 0 himself, 53 % env.

## Reading

- **Concentrated by type, not by mesh.** Most types are a hierarchy of 3-30 distinct models drawn about once each. The true instancing candidates are:
  - rings: one env-mapped model, 6-7 draws/frame, ~10 % of the corners while running
  - ovl_stg01_8c90b20a: one model, 4-6 draws
  - ovl_stg01_8c907ee0, 8c908114, 8c908336: 3 models each, ~2-3 draws per model
- **Rigid textured stage objects** (types 2, 3, 5, 6, 7, 8 above) are ~99 % rigid. They are the natural "select few":
  - about 55 % of the running corners and 66 % of the idle ones
  - but env sets are a large part of them: 8c902d08 56 %, 8c90b20a 85 %, and rings are 100 % env
  - m0's design keeps env on the TA path, so a select-few native path must handle env (sphere-mapped UVs) or it drops to ~42 % (running).
- **Sonic** is the biggest single type: 19 % running, ~3.4 ms. He is half posed, so he needs per-frame points (the welded parts) and fits the cached design only partly.
- **Per draw, not per corner:** the m0.5 spike put ~10-22 us on each extra replay item on the PSP. About 60 draws/frame for the select few would be ~1 ms of new cost against ~7.5-11.7 ms gross.
- **Names** are my guesses from model addresses only (Sonic from his 0x0C1C/0x0C1D models; rings from one env model drawn in groups). A screenshot per type would confirm them. The EC overlay types are probably palm trees, water and flowers, but that is unverified.
