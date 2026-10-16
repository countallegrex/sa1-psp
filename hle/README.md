# HLE: native replacements for the Katana SDK

Platform-agnostic. Each subdirectory replaces one SDK library the game calls
into. Implementations read game data straight out of guest RAM using the
layouts documented in `docs/`.

- `shinobi/` system: memory (`syMalloc`…), GD-ROM file system (`gdFs*`),
  peripherals (`pd*`), timers, vsync, interrupts.
- `ninja/` graphics: matrix stack, models, textures, lights, 2D/3D draw calls.
  Emits to the backend's draw interface (`backend/draw.h`), never to a GPU API.
- `sound/` Manatee command interface + ADX streaming.
- `prs/` PRS decompression (tiny LZ; may stay recompiled if cheap enough).

Every function here carries the SDK signature it replaces and a note on which
guest-visible side effects it must preserve (return values, memory it writes,
flags the game polls).
