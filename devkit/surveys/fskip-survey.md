# Frame-skip survey after the skipper fix: Sonic's story on the PSP-3001 (orders v64; sa1-psp-h gen 14, 2026-10-27)

Build: main 1a6ef3c (o1 overlay coverage 237135e + o2 skipper fix; local CI PASS). PRX = private\o2\b_fix, the
same code as the card EBOOT 411a310f35b5. Every place reached by `--stage LEVEL:ACT:MODE` (stage select at frame 12,
Sonic, no story state), idle (no input). A turn is 2 vblanks (33.37 ms; the game's frame interval at 0x8C754EBC).

Three runs per place, same binary:
- **paced:** `--frame-skip auto`, 1050 frames. Game speed, true pictures/s and % of turns drawn over f300-f1050
  (750 vblanks = 375 turns), from the `--stats 150` windows and their timestamps.
- **draw:** `--no-pace --frame-skip off`, 750 frames. **D** = wall ms per turn over f300-f750, every turn drawn.
- **logic:** `--no-pace --frame-skip 60`, 750 frames: one picture in 30 turns. **L** = (30 T - D) / 29 = the logic-only
  turn (game code, the skipped model pipeline's early return, sound, kmFlip bookkeeping).

Check of the split: the drawn share the skipper can sustain, (33.37 - L) / (D - L) clipped to [25 %, 100 %]
(MAX_RUN 3), matches the measured paced share within ~3 points in every place that holds full speed. Records:
`private\fs4` (fsurvey.py, out\*.json, logs).

## Summary
- Before the fix every place showed ~7.5 pictures/s (1 turn in 4) whatever its speed.
- **27 of 44 places hold full game speed** with frame skip auto. 11 of them draw (nearly) every turn,
  ~30 pictures/s; 21 of them show >= 14 pictures/s.
- **17 places run in slow motion (speed < 99 %)** even with the skipper at its 1-in-4 cap: there
  (D + 3 L) / 4 > 33.37 ms. 2 of them (Sky Deck 3, Windy Valley 2) have L > 33.37: game logic alone
  is slower than real time, so no amount of skipping can bring them to full speed.
- The picture (D - L) costs 9-70 ms; logic-only turns 9-41 ms. Where speed is lost, L is 20-40 ms: game logic
  (incl. the TA writers that still run on skipped turns) is as much a limit as drawing.
- Option, not done (policy: authentic first, the orchestrator decides): raising MAX_RUN would buy speed in places
  with L < 33.37 (Sky Deck 1/2, Ice Cap, Twinkle Park, Chaos 0/4, Windy Valley 1/3, Casino 2, Red Mountain 1,
  Sky Chase 2) at fewer than 7.5 pictures/s.

## Full game speed, fewest pictures first

| place | stage | game speed % | pictures/s | turns drawn % | D drawn turn ms | L logic-only turn ms | picture ms (D-L) | D for 14/s at this L | |
|---|---|---|---|---|---|---|---|---|---|
| Mystic Ruins 0 (station) | 33:0:5 | 100.6 | **7.6** | 25 | 71.7 | 20.2 | 51.5 | 48 |  |
| Emerald Coast 1 | 1:0:4 | 100.1 | **8.9** | 30 | 59.9 | 22.4 | 37.5 | 46 |  |
| Final Egg 1 | 10:0:4 | 100.4 | **9.5** | 31 | 56.0 | 23.3 | 32.7 | 45 |  |
| Speed Highway 3 | 4:2:4 | 100.2 | **9.7** | 32 | 61.8 | 20.1 | 41.7 | 48 |  |
| Casinopolis 1 | 9:0:4 | 100.2 | **12.0** | 40 | 43.4 | 26.6 | 16.7 | 41 |  |
| Egg Carrier inside 1 | 32:1:5 | 100.1 | **13.9** | 46 | 49.2 | 19.7 | 29.5 | 49 |  |
| Mystic Ruins 2 | 33:2:5 | 100.2 | **14.3** | 47 | 52.4 | 16.6 | 35.8 | 53 |  |
| Sky Chase 1 | 36:0:4 | 100.1 | **16.0** | 53 | 53.9 | 11.5 | 42.4 | 58 |  |
| Egg Hornet | 20:0:4 | 100.2 | **19.2** | 64 | 43.0 | 15.5 | 27.5 | 54 |  |
| Egg Carrier inside 0 | 32:0:5 | 100.2 | **21.1** | 70 | 42.8 | 11.0 | 31.8 | 59 |  |
| Chaos 6 | 18:0:4 | 100.6 | **21.4** | 71 | 43.3 | 16.4 | 26.8 | 53 |  |
| Mystic Ruins 1 | 33:1:5 | 100.3 | **22.2** | 74 | 40.4 | 13.5 | 27.0 | 56 |  |
| Red Mountain 2 | 5:1:4 | 100.0 | **25.7** | 86 | 35.3 | 16.4 | 18.9 | 53 |  |
| Egg Carrier outside 1 | 29:1:5 | 100.2 | **26.6** | 89 | 35.4 | 17.7 | 17.7 | 51 |  |
| Lost World 1 | 7:0:4 | 100.2 | **27.7** | 92 | 33.2 | 19.8 | 13.4 | 49 |  |
| Egg Carrier outside 0 | 29:0:5 | 100.1 | **28.1** | 94 | 34.6 | 16.2 | 18.3 | 53 |  |
| Station Square 4 | 26:4:5 | 100.2 | **28.7** | 95 | 34.2 | 15.6 | 18.6 | 54 |  |
| Egg Viper | 22:0:4 | 99.7 | **29.2** | 98 | 18.0 | 8.7 | 9.3 | 62 |  |
| Emerald Coast 3 | 1:2:4 | 100.6 | **29.8** | 99 | 27.2 | 15.8 | 11.5 | 53 |  |
| Final Egg 2 | 10:1:4 | 100.4 | **29.8** | 99 | 33.5 | 14.3 | 19.2 | 55 |  |
| Station Square 3 | 26:3:5 | 100.2 | **29.9** | 99 | 30.7 | 16.4 | 14.3 | 53 |  |
| Lost World 2 | 7:1:4 | 100.2 | **29.9** | 99 | 25.4 | 13.3 | 12.2 | 56 |  |
| Speed Highway 1 | 4:0:4 | 100.2 | **29.9** | 99 | 31.0 | 16.2 | 14.8 | 53 |  |
| Emerald Coast 2 | 1:1:4 | 100.1 | **29.9** | 100 | 29.8 | 14.0 | 15.7 | 55 |  |
| Station Square 0 | 26:0:5 | 100.1 | **30.0** | 100 | 26.9 | 14.6 | 12.3 | 55 |  |
| Speed Highway 2 | 4:1:4 | 100.1 | **30.0** | 100 | 23.1 | 12.0 | 11.1 | 58 |  |
| Station Square 1 | 26:1:5 | 100.2 | **30.0** | 100 | 27.7 | 14.5 | 13.2 | 55 |  |

## Slow motion even at the 1-in-4 cap, slowest first

| place | stage | game speed % | pictures/s | turns drawn % | D drawn turn ms | L logic-only turn ms | picture ms (D-L) | D for 14/s at this L | speed predicted at the 1-in-4 cap % |
|---|---|---|---|---|---|---|---|---|---|
| Sky Deck 3 | 6:2:4 | 54.7 | **5.4** | 33 | 99.1 | 40.5 | 58.5 | 25 | **60** |
| Windy Valley 2 | 2:1:4 | 68.7 | **6.6** | 32 | 68.2 | 39.4 | 28.8 | 26 | **72** |
| Sky Deck 1 | 6:0:4 | 71.1 | **6.6** | 31 | 83.9 | 29.5 | 54.4 | 38 | **77** |
| Chaos 0 | 15:0:4 | 75.1 | **6.8** | 30 | 65.6 | 32.0 | 33.5 | 35 | **83** |
| Red Mountain 1 | 5:0:4 | 77.4 | **6.7** | 29 | 83.2 | 21.9 | 61.4 | 46 | **90** |
| Ice Cap 1 | 8:0:4 | 81.6 | **7.2** | 30 | 68.1 | 29.7 | 38.4 | 38 | **85** |
| Windy Valley 3 | 2:2:4 | 81.8 | **7.1** | 29 | 69.8 | 28.8 | 41.0 | 39 | **85** |
| Casinopolis 2 | 9:1:4 | 82.4 | **7.4** | 30 | 59.1 | 32.1 | 26.9 | 35 | **86** |
| Sky Deck 2 | 6:1:4 | 82.5 | **7.5** | 30 | 65.5 | 29.7 | 35.8 | 38 | **86** |
| Twinkle Park 1 | 3:0:4 | 82.7 | **7.2** | 29 | 69.8 | 28.4 | 41.4 | 39 | **86** |
| Chaos 4 | 17:0:4 | 84.1 | **7.9** | 31 | 65.2 | 31.7 | 33.5 | 35 | **83** |
| Twinkle Park 2 | 3:1:4 | 87.8 | **8.4** | 32 | 66.3 | 28.3 | 38.0 | 39 | **88** |
| Windy Valley 1 | 2:0:4 | 89.2 | **7.3** | 27 | 67.9 | 25.9 | 42.0 | 42 | **92** |
| Sky Chase 2 | 37:0:4 | 90.2 | **7.3** | 27 | 89.6 | 19.1 | 70.5 | 50 | **91** |
| Ice Cap 3 | 8:2:4 | 91.4 | **9.3** | 34 | 46.9 | 27.3 | 19.6 | 40 | **104** |
| Ice Cap 2 | 8:1:4 | 95.4 | **7.5** | 26 | 64.9 | 24.4 | 40.4 | 44 | **97** |
| Final Egg 3 | 10:2:4 | 98.9 | **11.2** | 38 | 46.1 | 26.5 | 19.6 | 41 | **106** |

"D for 14/s" = the drawn-turn time that would give 14 pictures/s at full speed with the measured L
((33.37 - 8/15 L) / (7/15)). Emerald Coast 1: D 59.9 -> ~45 ms needed (L 22.4).
