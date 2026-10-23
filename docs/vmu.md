# VMU saving (bu library HLE)

The game saves through the Katana "bu" library (bu Ver 1.44, the VMU file
system). `hle/shinobi/cfg.c` replaces it at the wrapper level with one
standard VMU in port A slot 1 (drive 0): 200 user blocks of 512 bytes, whose
files are host files behind `hle/shinobi/vmu_store.h`.

## Backends

| Backend | Store | When |
|---|---|---|
| desktop | `vmu/NAME.VMS` beside the executable, or `--vmu-dir DIR` | on by default; off with `--no-vmu` and in every `--strict` run (`--replay`, `--record`, `--check`, the oracle), so replay tapes keep the no-VMU boot they were recorded with. `--vmu-dir` turns it on even in a strict run. |
| PSP | none yet (`hle_vmu_store` stays NULL: no VMU) | TODO: an ms0: store, set before `hle_init`. |

A `.VMS` file is the exact image the game wrote: the VMS header (title,
comment, creator, icon count, CRC-16/0x1021 at 0x46, data size), the
palette, the icons, then the game's data, padded to 512 bytes. The desktop
store writes a `.tmp` and renames it over the old file, so a crash during a
save keeps the previous one.

## What the game uses

Measured on US 1.005 (1ST_READ.BIN; entry points in P1 addresses):

- `0x8C602B68` buInit(size class, drive mask, work, init_cb): the game
  passes (15, 1 << drive, ...) at boot and the advertise overlay (122, 0xFF,
  ...); init_cb sets the completion and progress callbacks.
- Mounting: the real server (`0x8C602CB8`, run every frame by the game)
  sees a new card and calls complete_cb(drive, 1, 0, user). The game marks
  the drive mounted only from that callback.
- `0x8C603560` buSaveFile(drive, name, image, blocks, time, flags) and
  `0x8C603508` buLoadFile(drive, name, buf, blocks) are asynchronous on the
  console. The game spins on buIsBusy (`0x8C6033E4`) without running the
  server, so the HLE finishes them inside the call and then calls
  complete_cb(drive, 11 or 12, result, 0).
- `0x8C602F5C` buGetDiskInfo fills 56 bytes. The game reads only the free
  blocks at +36 (the advertise overlay wants at least 10).
- `0x8C602FB8` / `0x8C602FE6` find first/next return 12-character names.
  The game compares them with strcmp, so the NUL at +12 matters.
- `0x8C603474` buGetFileSize returns blocks, and `0x8C603384` (exists)
  returns 0 or -251.
- `0x8C603998` buCalcBackupFileSize and `0x8C6037A0` buMakeBackupFileImage
  are pure computation in the SDK; the HLE does the same.
- Error codes are 16-bit, sign-extended: -1 busy, -255 no card, -252 not
  enough room, -251 no such file, -245 bad name.

The main save is `SONICADV_INT`, 10 blocks: 2 icons plus 3968 bytes of
data, copied from and to `0x8C7989A0`. It is written when a new game is
created and at the game's save points. The game also knows the upload files
(CHAO_UPLOAD, DATA_UPLOAD, CART_TIME, EVENT_RESULT) and the Lantern
`SONICADV_Pnn` / `SONICADV_Snn` files.

With no store, every entry point gives the same answer as before the VMU
existed (no card), so no-VMU runs are unchanged.

## Checked on the desktop

With a VMU the game adds its "Select a memory card" and "Select a file"
screens after the title, so the no-VMU tapes go off route. A private copy of
`sonic_ec1` with those presses added (the inputs from frame 1182 on moved
300 frames later) creates the file at the new game, then saves again when
Sonic is back in Station Square after Chaos 0. Run again from that file, the
file screen shows "From Station Square, Completed Chaos, Play Time 0:02",
and Adventure goes on at the hotel pool, not at the opening scene.

## Not done

- Delete and format: the game never calls them.
- The progress callback is called once per save, with the whole size.
- No card removal while running.
- Only drive 0. The advertise overlay can pick another port, which then
  reads as empty.
- The extra state (pending mount, find cursor) is not in save states. A
  state loaded in the middle of a find loop restarts that listing.
