# Stage survey: Sonic's story on the PSP-3001 (orders v62; sa1-psp-h gen 13, 2026-10-27)

Build: main 0a07c96 (= 0d20494 + a p50/p95 line in the draw stats window; local CI PASS). Every place was
reached by `--stage LEVEL:ACT:MODE` (stage select at frame 12, Sonic, no story state). Frame skip was off and
`--no-pace` was used.

Each place had three runs:
- **idle:** Sonic stands at the start point, 760 frames. Mean and p95 are over the windows ending f450/f600/f750 (150 frames each). Max is the slowest single frame. Free is the largest free block at exit.
- **prof:** sampled f300-f700, used for the top 3 groups.
- **run:** stick held up from f300 (mean and p95 only).

The ss/ec bench figures for comparison: ss3 = 30.3 here vs ~31 in the bench; ec1 = 59.2 vs ~60. Records are in
`private\s3` (survey.py, fskip.py, out\*.json, logs and profiles).

## Measured places, worst first (idle mean, ms per drawn frame)

| place | stage | idle mean | p95 | max | run mean | run p95 | free KB | top 3 groups (% of samples) |
|---|---|---|---|---|---|---|---|---|
| Sky Deck 3 | 6:2:4 | **97.7** | 129.2 | 297 | 94.8 | 119.2 | 2048 | renderer 22, game code 21, kamui 9 |
| Sky Chase 2 | 37:0:4 | **85.4** | 92.8 | 126 | 85.1 | 92.2 | 3264 | renderer 29, ta_direct 13, game code 12 |
| Sky Deck 1 | 6:0:4 | **82.7** | 86.2 | 172 | 82.8 | 85.8 | 2304 | renderer 25, game code 19, ta_xform 10 |
| Mystic Ruins (station) | 33:0:5 | 70.1 | 72.8 | 163 | 56.0 | 73.2 | **1472** | renderer 24, game code 18, ta_xform 10 |
| Sky Deck 2 | 6:1:4 | 64.3 | 64.8 | 147 | 64.2 | 70.8 | 2944 | game code 21, renderer 21, kamui 10 |
| Chaos 0 | 15:0:4 | 64.1 | 87.8 | 144 | 64.1 | 87.8 | 6528 | game code 22, renderer 18, kamui 14 |
| Chaos 4 | 17:0:4 | 63.7 | 71.8 | 137 | 60.9 | 74.8 | 5888 | game code 26, renderer 18, math 11 |
| Emerald Coast 1 | 1:0:4 | 59.2 | 62.2 | 138 | 57.5 | 87.2 | 3008 | game code 23, renderer 19, ta_xform 10 |
| Casinopolis 2 | 9:1:4 | 57.3 | 60.8 | 139 | 60.3 | 70.2 | 3776 | game code 21, renderer 17, **sound 14** |
| Final Egg 1 | 10:0:4 | 54.3 | 57.2 | 141 | 55.4 | 60.2 | 2944 | renderer 22, game code 18, **sound 13** |
| Sky Chase 1 | 36:0:4 | 52.7 | 57.2 | 106 | 52.3 | 57.2 | 3200 | renderer 33, kamui 12, ta_xform 11 |
| Mystic Ruins (jungle) | 33:2:5 | 52.0 | 52.2 | 134 | 57.5 | 65.8 | 1920 | renderer 23, game code 18, kamui 11 |
| Egg Carrier inside 1 | 32:1:5 | 48.3 | 51.2 | 142 | 39.4 | 52.8 | 6144 | game code 24, renderer 20, ta_direct_clip 12 |
| Casinopolis 1 | 9:0:4 | 42.4 | 45.2 | 121 | 40.3 | 43.8 | 4288 | game code 31, renderer 12, math 10 |
| Chaos 6 | 18:0:4 | 42.0 | 59.2 | 134 | 42.1 | 59.2 | 2944 | renderer 20, game code 19, math 13 |
| Egg Carrier inside 0 | 32:0:5 | 41.7 | 44.8 | 122 | 42.1 | 45.2 | 6144 | renderer 26, game code 17, ta_xform 10 |
| Mystic Ruins (angel island) | 33:1:5 | 40.5 | 43.8 | 120 | 52.3 | 60.8 | 1920 | renderer 22, game code 19, ta_xform 11 |
| Station Square 4 | 26:4:5 | 33.7 | 36.8 | 111 | 35.2 | 38.2 | 2816 | game code 23, renderer 19, math 9 |
| Station Square 3 (bench ss) | 26:3:5 | 30.3 | 33.2 | 120 | 41.4 | 100.8 | 2752 | game code 32, renderer 14, math 12 |
| Emerald Coast 2 | 1:1:4 | 29.5 | 32.2 | 99 | 42.7 | 56.2 | 2496 | game code 28, renderer 14, math 10 |
| Station Square 1 | 26:1:5 | 27.4 | 30.2 | 102 | 25.6 | 36.2 | **1664** | game code 31, renderer 14, math 10 |
| Emerald Coast 3 | 1:2:4 | 27.0 | 31.8 | 117 | 27.3 | 34.2 | 2752 | game code 26, renderer 12, math 10 |
| Egg Viper | 22:0:4 | 17.8 | 21.2 | 97 | 17.8 | 20.8 | 4864 | game code 24, renderer 16, no source 10 |

How to read it:
- **Speed:** 5 of the 23 measured places are at or below 33.3 ms (Station Square 1/3, Emerald Coast 2/3, Egg Viper). Three are clearly worse than Emerald Coast act 1: Sky Deck 1 and 3, and Sky Chase 2 (+40-65 %).
- **Proxy:** Emerald Coast act 1 is mid-pack, not the worst case.
- **Heap:** Mystic Ruins station (1472 KB) and Station Square 1 (1664 KB) have the smallest largest-free blocks.
- **Buckets:**
  - "renderer" = draw_gu.c. It leads in the slowest places (Sky Deck, Sky Chase), with the TA path (kamui, ta_xform, ta_direct) close behind.
  - Game code leads in the fields and the faster stages.
  - Sound is a top-3 group in Casinopolis 2 and Final Egg 1 (13-14 %).

## Unreachable: 21 places trap within 30 frames of loading

The same traps occur on the desktop build (build-s2c, the same `--poke` stage select; trap at f28 at the same
addresses). So these are gaps in the recompiled code, not PSP problems.

| place | stage | trap |
|---|---|---|
| Windy Valley 1-3 | 2:0-2 | indirect call to 0x0c90011a (STG02) |
| Ice Cap 1-3 | 8:0-2 | indirect call to 0x0c90ea56 (STG08) |
| Twinkle Park 1-2 | 3:0-1 | indirect call to 0x0c90843e (STG03) |
| Speed Highway 1-3 | 4:0-2 | indirect call to 0x0c90bad2 (STG04) |
| Red Mountain 1 | 5:0 | indirect call to 0x0cb80020 (second slot) |
| Red Mountain 2 | 5:1 | indirect call to 0x8c06c0a8 (main image) |
| Lost World 1-2 | 7:0-1 | indirect call to 0x0c90b4cc (STG07) |
| Final Egg 2-3 | 10:1-2 | indirect call to 0x8c06c022 (main image) |
| Egg Hornet | 20:0 | indirect call to 0x0c904a40 |
| Egg Carrier outside 0-1 | 29:0-1 | indirect call to 0x0c927fe0 (AREA2 slot) |
| Station Square 0 | 26:0 | switch value outside the recovered table at 0x0c9108a6 |

Most traps are 0x0C… overlay addresses, the class 0a00711 fixed for stg01/stg08/adv (pointers stored in the
0x0C form, or entries never discovered). On a real story playthrough these places would very likely trap the
same way. I did not check whether stage select itself contributes.

## Frame skip ON (paced, auto): does game speed hold?

Game speed is game frames per wall second over f300-f1050, as a share of 60. Pictures per second come from the
frame-skip exit counts.

| place | game speed | pictures drawn | pictures/s |
|---|---|---|---|
| Station Square 3 | 100.0 % | 25.5 % | 15.3 |
| Emerald Coast 1 | 100.1 % | 25.3 % | 15.2 |
| Final Egg 1 | 100.1 % | 35.4 % | 21.3 |
| Mystic Ruins station | 99.9 % | 26.5 % | 15.9 |
| Sky Chase 2 | 95.8 % | 28.2 % | 16.2 |
| Chaos 4 | 90.3 % | 25.7 % | 13.9 |
| Casinopolis 2 | 87.6 % | 25.7 % | 13.5 |
| Sky Deck 2 | 87.3 % | 25.7 % | 13.5 |
| Chaos 0 | 79.7 % | 25.9 % | 12.4 |
| Sky Deck 1 | 77.1 % | 25.7 % | 11.9 |
| Sky Deck 3 | 59.1 % | 26.9 % | 9.5 |

- Game speed holds (≥ 99 %) where the drawn frame costs up to ~70 ms (Station Square, Emerald Coast 1, Final Egg 1, Mystic Ruins).
- It drops to 59-90 % in Sky Deck, the Chaos 0 and Chaos 4 fights, and Casinopolis 2. There the audio thread waited 14-30 s for the game, so the sound has gaps.
- Auto mode skips at most 3 pictures in a row (MAX_RUN) against a 60 Hz schedule. Every place except Final Egg 1 sits at that cap, drawing about 1 picture in 4: about 15 pictures/s even in Station Square, which measures 30.3 ms with frame skip off.
- So 30 pictures/s at full game speed needs one drawn frame plus one skipped frame to fit in 33.3 ms, not only the drawn frame.

## Follow-ups (not done here)

1. Unreachable stages: add the missing overlay entries (the 0a00711 method) for STG02/03/04/05/07/08, B_EGM1 (Egg Hornet), ADV01 outside, ADV00 act 0, and the two main-image addresses. It is reproducible on the desktop in 0.2 s with the 5 `--poke 12:...` lines.
2. Lever ranking: Sky Deck (renderer + TA) and Sky Chase (renderer 29-33 %) are the worst places measured. The renderer/TA path is the common bucket.
