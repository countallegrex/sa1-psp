# tests

Tests that need game data take the dump location from the environment
variable `SA1_DUMP` (e.g. `..\private\dump\sa1_us_1005`) and skip cleanly when
it is unset. No fixture in this directory may be derived from the game.

- `emitter/`: the SH-4 → C emitter on hand-written snippets (no game data).
- `overlay/`: overlay loader slot rules (no game data).
- `ninja/`: the PVR texture decoder (`hle/ninja/pvr_decode.c`) on synthetic
  twiddled, mipmapped, VQ and rectangle textures (no game data).
- `replay/`: input tapes and state checks for the desktop build
  (`tools/replay_test.py`, `replay/README.md`). A tape holds controller
  input and the SHA-256 of the title it was recorded on; a check file holds
  guest addresses and the values a good run had there (mode numbers, ring
  counts, positions). Neither contains any of the game's code or data.
- `savestate/`: the save-state machinery (`backend/desktop/savestate.c`,
  the fixed-address coroutines) on a synthetic run: saved by one process,
  resumed by another (no game data). `docs/savestate.md`.
