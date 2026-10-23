# Sound: files, the calls the game makes, and the command stream

What Sonic Adventure (US v1.005) does with sound, as far as the port needs
it. Everything here comes from our own observation: the disc's files read
byte by byte, the game's code disassembled at its call sites, and the
calls logged on the maintainer's full route (`tests/replay/sonic_ec1.tape`,
`SA1_SND_LOG=1`). The ADX layout follows the public description of the
format. No SDK document or header was used; the names are behavioural.

The implementation is `hle/sound/` (`docs/m3-audio-report.md` has the
design and the numbers).

## 1. Files

| File | Count | What | Who reads it |
|---|---|---|---|
| `SONICADV/*.ADX` | 109 | Music, stereo 44.1 kHz ADX; most loop | the music player, by name |
| `SONICADV/EVENT_ADX.AFS`, `EVENT_ADX_US.AFS` | 2 × 2,044 entries | Voice lines, Japanese and English: mono ADX, 22,050 Hz (1,962 and 2,041 of them), the rest 44.1 kHz | the voice player, by index |
| `SONICADV/*.MLT` | 123 | Multi-unit sound files: program banks (tones), sequence banks, DSP programs (§3) | the sound driver, into sound RAM |
| `SONICADV/MASARU.DRV` (33,408 B), `MANATEE.DRV` (root) | 2 | The ARM7 sound driver (`SDRV` header) | loaded to guest RAM and handed to the driver download entry; never run here |
| `STREAM.MLT` (root, 288 B) | 1 | Eight `SPSR` units of 16 KB at sound-RAM 0x018000-0x038000 (stream ring buffers) | not loaded on the route |

The MLT names follow the game's areas: `STATIONSQUARE`, `EMERALDCOAST`,
`CHAOS0`, ... (level banks), `P_SONICTAILS` (the player's effects),
`V_SONICTAILS_E` and `V_SONICTAILS` (character voice clips, English and
Japanese), `E_0001`... (one per event), `MOBILE1`, `EGGCARRIER` (the title
and menus).

## 2. ADX music and voice lines

### 2.1 The stream format (public description)

Big-endian header:

| Offset | Size | Field |
|---|---|---|
| 0x00 | 2 | 0x8000 |
| 0x02 | 2 | offset of the the CRI copyright string ("(c)" then "CRI") string; audio starts 4 bytes after it |
| 0x04 | 1 | encoding: 3 (the only one on the disc) |
| 0x05 | 1 | block size: 18 |
| 0x06 | 1 | bits per sample: 4 |
| 0x07 | 1 | channels: 1 or 2 |
| 0x08 | 4 | sample rate |
| 0x0C | 4 | samples per channel |
| 0x10 | 2 | high-pass cutoff (500 on every file seen) |
| 0x12 | 1 | version: 3 |
| 0x13 | 1 | flags: 0 (no encryption on this disc) |
| 0x14 | 0x18 | loop record (version 3): u16 alignment samples, u16, u32 loop enabled, u32 loop start sample, u32 loop start byte, u32 loop end sample (exclusive), u32 loop end byte |

Audio is interleaved per channel in 18-byte blocks: a 16-bit big-endian
scale, then 32 signed 4-bit samples, high nibble first. Each output
sample is `nibble × scale + ((c1 × h1 + c2 × h2) >> 12)`, clamped to 16
bits, where h1 and h2 are the two previous outputs of that channel and the
Q12 coefficients come from the cutoff f and the rate r:
a = √2 − cos(2πf/r), b = √2 − 1, c = (a − √((a+b)(a−b)))/b,
c1 = ⌊8192 c⌋, c2 = ⌊−4096 c²⌋. Example: `S_SQUARE.ADX` is stereo,
44,100 Hz, 4,208,184 samples, looping from sample 208,288 to 4,208,182.

A loop continues the predictor from the history it had when the decoder
first passed the start of the loop's first block; `hle/sound/snd_core.c`
saves it there and restores it at the loop end.

### 2.2 How the game plays them

The game's ADX manager (a task, `0x8C107864`) runs at every area load. It
creates two players and keeps their handles in `0x8C5FBA70` (music) and
`0x8C5FBA74` (voice):

| Call (entry) | Arguments seen | Meaning |
|---|---|---|
| `0x8C644D84`, `0x8C64401C`, `0x8C644022` | at boot | library set-up |
| `0x8C64489C` | (`0x8C107860`, 0) | error callback |
| `0x8C10C1C8` (CRI code linked into the game, recompiled) | (0, "event_adx_us.afs", ..., 0x864) | ADXF_LoadPartitionNw: partition 0 = the voice archive. Which archive: the word `0x8C754C00` (0 → `event_adx.afs`, else `event_adx_us.afs`) |
| `0x8C64413C` ADXT_Create | (2, work, 0x2D0C4) and (1, work, 0xD884) | the stereo music player and the mono voice player |
| `0x8C6446D2` | (player, 2) and (player, 1) | stores a byte at +109 |
| `0x8C6445EA` ADXT_SetOutVol | (music, −10), (voice, 0); (both, −960) to silence | level in 0.1 dB, kept at +64 |
| `0x8C644602` | (player, 20) | stores a word at +56 |
| `0x8C644968` ADXT_StartFname | (music, name) | name from the table at `0x8C5FB8A8` (110 entries: `ADVAMY.ADX` … `WNDYVLY3.ADX`, index = the game's music id) |
| `0x8C644904` ADXT_StartAfs | (voice, 0, line) | line = AFS entry index |
| `0x8C644468` ADXT_Stop | (player) | |
| `0x8C644500` ADXT_GetStat | (player) | the byte at +1: 0 stopped, 3 playing, 5 ended (also 1, 2, 4 while starting and ending) |
| `0x8C644504` ADXT_GetTime | (player, &count, &rate) | time = count / rate |
| `0x8C6446E2` ADXT_ExecServer | every frame | |
| `0x8C6442F2` ADXT_Destroy | (player) | when the manager task ends |

The game side, in its own words (game functions in block A):

| Function | Sites | What |
|---|---|---|
| `0x8C107AC6` (id) | 67 | play music `id`: writes the request word `0x8C5FBAA8` (and `0x8C5FBAB0`, the id a later "resume" restarts) |
| `0x8C107ADA` | 11 | stop music |
| `0x8C107B26` | 4 | resume the last music |
| `0x8C107BA0` (line) | 55 | play voice line: writes `0x8C5FBAA0` |
| `0x8C107BB0` | 5 | stop the voice |
| `0x8C107BC2` | 3 | voice status (`0x8C5FBAA4`): the event code loops until it is 0 or 5 |
| `0x8C107AF6` | 2 | music status (`0x8C5FBAAC`) |
| `0x8C10792A` | each frame | the manager's frame: starts a pending request, stores both statuses |

So a cutscene line holds the event until the line has been heard: with
no player (before M3) the status was always 0 and every line ended at
once.

The status around a start, as the console shows it (Flycast, the voice
lines and music starts of the first Station Square cutscenes): 1 for 6
frames, 2 for 2 frames, then 3 while playing, 4 over the last 22 frames,
then 5; from 3 to 5 is the stream's length. `hle/sound/snd_host.c` keeps
the same phases and starts the stream in the core when the status turns
3; without the 8 frames each cutscene line ended early.

### 2.3 The AFS archives

`"AFS\0"`, u32 entry count (2,044 in both), then per entry u32 offset and
u32 size (little-endian), then a zero pair. Entries start at 0x80000 and
are 2 KB aligned. Lines on the route: 4 (character select), 400-416
(Station Square night, the Chaos 0 arrival, Station Square day), 1837,
1843, 1847 (short exclamations).

## 3. Multi-unit files (S-MLT) and sound RAM

Little-endian. Header: the tag S-MLT (the four letters S, M, L, T), u32 0x101, u32 unit count (21 in every
level file), 20 bytes of 0xFF. Then one 32-byte entry per unit:

| Offset | Field |
|---|---|
| 0 | tag: `SMPB` program bank, `SMSB` sequence bank, `SFPB` DSP program, `SFOB` DSP output, `SFPW` DSP work, `SPSR` stream buffer |
| 4 | bank number |
| 8 | sound-RAM address |
| 12 | size reserved there |
| 16 | file offset of the unit's data, 0xFFFFFFFF = none in this file |
| 20 | data size, 0xFFFFFFFF = none |
| 24 | 0, 0 |

The table is the same in every file, so sound RAM has a fixed map, and a
file fills only the banks it carries; a multi-unit download leaves the
others as an earlier file left them:

| Bank | Sound RAM | Program bank size | Filled by |
|---|---|---|---|
| 0 | 0x018000 (256 KB) | 0x3FD00 in every level file | the level file: common effects (rings, jumps, …) |
| 1 | 0x059000 (320 KB) | varies | the level file: the area's own effects |
| 2 | 0x0AA000 (320 KB) | 0x4F8E0 in every level file | the level file: a second common set |
| 3 | 0x0FB000 (192 KB) | | `P_*`: the player character's effects |
| 4 | 0x12C000 (192 KB) | | the level file (enemies, objects) |
| 5 | 0x15D000 (256 KB) | | the level file |
| 6 | 0x19E000 (220 KB) | | `V_*` voice clips, or an `E_*` event file |
| 7, 8 | 0x1E6CE0, 0x1E6EE0 | | empty on this disc |

Each bank number has its `SMSB` (4 KB) right after its `SMPB`; the DSP
units sit at 0x1D6000-0x1E6CE0.

On a level load the game downloads, in this order: the level file, the
voice file, the player file (`STATIONSQUARE.MLT`, `V_SONICTAILS_E.MLT`,
`P_SONICTAILS.MLT`), and an `E_*` file when an event starts.

## 4. Program banks (SMPB)

Offsets relative to the start of the bank, little-endian.

Header: `"SMPB"`, u32 1, u32 size, u32 0, then three (offset, count)
pairs and more: +0x10 program table offset, +0x14 program count; +0x18
velocity curve (128 bytes, 0 → 0, 127 → 127, concave), +0x1C 1; +0x20 a
12-byte table, +0x24 1; +0x28 a 4-byte table, +0x2C 1. The tables at
+0x20 and +0x28 are not used here.

Program table: one u32 offset per program (0 = none). A program is 24
bytes: four u32 layer offsets (0 = none; two layers seen), then 8 bytes of
zero.

Layer, 16 bytes: u32 split count, u32 split table offset, u8 delay (0 in
7,066 of 7,105 layers; 2, 25, 50, 75, 225 elsewhere; taken as
milliseconds), three bytes of 0, u8 bend range up and u8 bend range down
in semitones (2, or 12 in seven layers), two bytes of 0.

Split, 48 bytes. The first 34 bytes are AICA channel registers as the
driver writes them, with the sample address relative to the bank:

| Offset | Bits | Field |
|---|---|---|
| 0 | 9 | loop on |
| | 8-7 | format: 0 16-bit PCM, 1 8-bit PCM, 2 4-bit ADPCM, 3 ADPCM (long) |
| | 6-0 | sample address bits 22-16 |
| 2 | 15-0 | sample address bits 15-0 |
| 4 | | loop start, samples |
| 6 | | loop end (one-shot: the length), samples |
| 8 | 15-11, 10-6, 4-0 | envelope decay 2 rate, decay 1 rate, attack rate (31 = instant) |
| 10 | 13-10, 9-5, 4-0 | key rate scaling (15 = off), decay level, release rate |
| 12 | 14-11, 9-0 | octave, frequency step (pitch offset; 0 on the route) |
| 14 | | LFO (0 on the route) |
| 16 | 7-4, 3-0 | DSP send level, DSP input (reverb; 0xA0 often) |
| 18 | 11-8, 4-0 | direct send level (15 = 0 dB, 3 dB steps), pan |
| 20 | 15-8, 4-0 | total level (0.375 dB steps), filter resonance |
| 22 | | five filter levels (0x1FF8: open) |
| 32 | | filter envelope rates (0x19 each) |
| 36 | | key range low, key range high |
| 38 | | base key: the key that plays the sample at 44.1 kHz |
| 39 | | fine tune, signed (0, −2, −12 seen), taken as cents |
| 40 | | 0xFF, 0xFF |
| 42 | | 0, 1 or 4 |
| 44 | | 0x7F |

Most tones are recorded at 22,050 Hz with base key 72, so the game's key
60 plays them at their own rate. A layer can hold several splits on the
same range (two in `HOTSHELTER.MLT` bank 5 program 6, an octave and a
half apart); all of them sound.

The ADPCM is the Yamaha 4-bit scheme: low nibble first; step starts at
127; per nibble n (sign bit 3, magnitude m = n & 7):
d = ((1 + 2m) × step) >> 3, sample ± d, step × {230, 230, 230, 230, 307,
409, 512, 614}[m] >> 8, clamped to 127-24,576.

## 5. The command stream: sd entries and MIDI

The sound driver interface (`0x8C643400`-`0x8C643FFF`), as the game uses
it:

| Entry | Arguments seen | Calls on the route | Meaning |
|---|---|---|---|
| `0x8C643F6C` | (0, 0, 0) | 1 | driver system init |
| `0x8C6436D0` | (1) | 1 | mode |
| `0x8C6434E0` | (&h) | 29 | memory block create |
| `0x8C643624` | (h, addr, size, −1 or 0, 0) | 29 | memory block = a guest RAM range (size 0 for a whole S-MLT) |
| `0x8C6435BC` | (h) | 29 | memory block destroy |
| `0x8C643E6C` | (h, ...) | 2 | driver download (`MASARU.DRV` at 0x8CD00000) |
| `0x8C643970` | (h) | 28 | multi-unit download (§3) |
| `0x8C643904` | (h, 'SMPB' / 'SMSB' / 'SOSB', bank) | 0 | one bank (`0x8C093D2C`… wrappers) |
| `0x8C64387C` | ('STRO' or 'MONO') | 1 | output mode from the console setting |
| `0x8C643AC0` | (&port) | 8 | port open: eight ports into `0x8C79E3EC` |
| `0x8C643C18` | (port, msg, status, d1, d2) | 11,243 | variadic, the bytes on the stack: builds a MIDI message in the game's buffer `0x8C79E3E0` |
| `0x8C643B9C` | (port, msg, 0) | 11,243 | sends it |
| `0x8C643B08` | (port, a, b, c) | 2 (all zero) | sequence play on ports 2-7 |
| `0x8C643E08` | (port) | 86 | sequence stop |
| `0x8C643D88` | (port, level, 0, fade) | 56 | sequence volume |
| `0x8C643754` | (group, 0, level) | 28 | volume: groups 0 and 1 get −48, 2 and 3 get 0, at every bank load |
| `0x8C6437F0` | (0, 0) | 7 | before the volumes |
| `0x8C6436E8` | (a, b, b, 4) | 141 | a query after each load; 0 accepted |
| `0x8C643F98` | () | every frame | driver server |

The game's own sound-effect layer (`0x8C092FE8`-`0x8C094E40`) drives 32
"channels" on the two MIDI ports (channel c < 16 on port 0, c − 16 on
port 1) and six more (32-37) on the sequence ports 2-7, which the route
never starts. An effect is a program of a bank played as a note:

```
B9 20 06    bank select (CC 32) = MLT bank number 6
C9 09       program change: program 9 of that bank
B9 0A 40    pan centre
E9 00 40    pitch bend centre
B9 01 7F    modulation 127 (before every note)
99 3C 7F    note on, key 60, velocity 127
B9 07 nn    volume ramps, one message per frame (CC 7)
89 3C 00    note off
```

On the route: 932 note-ons (all key 60 or near it), 932 program changes
and bank selects, 3,949 volume messages, 1,045 pan, 944 pitch bends, 224
reset-all-controllers (CC 121, at every load on all 32 channels), and
note-offs on all channels at each load. Banks selected: 0 (583), 1
(142), 2 (82), 3 (37), 4 (10), 5 (21), 6 (57).

## 6. What is not known

- The driver's volume law for CC 7, its pan law, and the unit of
  `0x8C643754` (taken as 1/8 dB) are guesses checked only by ear and
  against the console capture's levels (`docs/m3-audio-report.md`).
- The envelope rate table is approximated (full 96 dB decay in 118 s at
  effective rate 2, halving every four steps); attack is taken as eight
  times faster. Splits on the route nearly all use instant attack and
  release.
- DSP effects (`SFPB`/`SFOB`, the send levels): not rendered.
- The sequence banks (`SMSB`: 39 sequences in bank 0) and the sequence
  ports: not played; the route starts none.
- Split bytes 40-47 and the header tables at +0x20 and +0x28.
