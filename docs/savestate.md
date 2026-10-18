# Save states (desktop build)

A save state is the whole run at one frame hook: guest RAM, the HLE's
state outside guest RAM, and the native stacks of the game's tasks. Loading
one resumes the run at that frame as if it had never stopped, so a debug
run can start at any frame instead of at the title screen. Written before
the code (2026-10-18), then kept current with it.

```
sa1psp <1ST_READ.BIN> <data root> --strict --save 3000:s3000.state --save 6000:s6000.state ...
sa1psp <1ST_READ.BIN> <data root> --strict --load s3000.state --frames 6000 --dump 6000:ram.bin ...
```

F5 / F8 in the window save / load `quick.state` (`--quick FILE` to rename).
`*.state` is game-derived (it holds guest RAM) and is git-ignored.

## 1. Why native stacks are part of the state

The recompiled game keeps guest registers in C locals and returns through
the host's call stack (`recomp/emitter/README.md`), so a suspended task's
SH-4 state is not a register file in a struct: it is the contents of its
coroutine's native stack, from the frame of `fn_8c099a50` (yield) down to
the recompiled frames that called it, plus the callee-saved host registers
pushed by the switch. The same is true of the context that runs the frame
hook: the game's main loop is a chain of recompiled frames under
`fn_8c051790`. Serialising "the task list" therefore means serialising
those stacks byte for byte, and a stack can only be restored to the same
virtual address it was saved from: it holds return addresses into the
executable, frame pointers into itself, and pointers to `sa1_ram`, to the
`sh4_ctx` in `main.c` and to the task records in `hle/game/tasks.c`.

So every address a stack can hold is fixed between runs of one build:

| What | How it is fixed | Checked on load |
|---|---|---|
| Code and static data (`fn_8c0xxxxx`, `sa1_ram`, `ctx`, `tasks[]`, string literals) | the executable is linked without ASLR: `/DYNAMICBASE:NO` (lld-link), `-no-pie` (ELF) | the address of a static (`sa1_ram`) is in the header and must match, and so must the SHA-256 of the executable: a state belongs to one build |
| Guest RAM (`SA1_RAM_ALLOC` bytes: 16 MB + the 2 MB list area + the masked alias + the null page) | `VirtualAlloc` / `mmap(MAP_FIXED_NOREPLACE)` at `SA1_FIXED_BASE` (`backend/desktop/platform_desktop.h`); the run refuses to start when the address is taken | the base is in the header |
| Coroutine stacks | one reservation at `SA1_FIXED_BASE + SA1_FIXED_STACKS_OFF`: 40 slots of 1 MB, a 64 KB no-access guard between them, all committed | the layout constants are in the header |
| Coroutine descriptors (`sa1_coro`) | a static array in `platform_desktop.c`, one per slot | (static data) |
| Heap blocks (`malloc`) | nothing: no pointer to one may live on a coroutine stack across a frame hook, and none does: the only heap users are per-frame scratch (`kamui.c` vertex arrays, texture decode buffers), the tape and check tables of `input_tape.c` (host side only) and `argv` strings, which only `main()` and the option tables hold | reviewed, below |

The main thread's own stack is not fixed (the OS places it) and is never
saved: the game does not run on it. `main()` becomes a supervisor that
creates the root coroutine on the first fixed slot, switches into it to
boot the game, and otherwise only wakes to service save and load requests
(section 4). Windows fibers and `ucontext` are replaced by a 40-line
context switch (`backend/desktop/coro_switch.c`, x86-64 Windows and SysV):
push the callee-saved registers, store `rsp` in the descriptor, load the
other descriptor's `rsp`, pop, return. On Windows it also swaps the
thread's stack bounds in the TEB (`StackBase`, `StackLimit`,
`DeallocationStack`), as fibers do, so `__chkstk`, the stack probe in
`main.c` (`GetCurrentThreadStackLimits`) and the exception dispatcher see
the right stack. A coroutine's whole state is then `rsp` plus the bytes
from `rsp` to the top of its slot, and a state file stores exactly that.

## 2. Inventory: runtime state outside guest RAM

Everything a running game depends on that is not in `sa1_ram`, module by
module, with what the state file does about it. "Saved" means written and
restored through `hle/state.h` (section 3). Debug statistics and host-only
state are listed so the inventory is complete, not because they are saved.

### Platform / runtime (`recomp/emitter/runtime/sh4_rt.c`)

| State | Kind | Treatment |
|---|---|---|
| `sa1_ram` (the pointer) | fixed | the allocation is at the fixed base; the pointer value is a header check |
| `sa1_slots[3]` installed overlay tables | guest-affecting | saved by name through `hle/overlay/overlay_loader.c` (`hle_overlay_state`): the registry maps the name back to the table of this build |
| `sa1_reg[]` registered native functions (BIOS vectors, MOVIE.BIN entry) | guest-affecting, static | not saved: `hle_init()` installs the same set before a load |
| seam hit counters, spin detectors (`hit_*`, `mmio_*`, `stub_*`), watches, bad-address records, function trace | debug / host | not saved; a loaded run's exit report counts from the load |
| log sink | host | not saved |

### Task system (`hle/game/tasks.c`)

| State | Kind | Treatment |
|---|---|---|
| `tasks[32]`: guest record address, coroutine, saved `sh4_ctx`, entry `fn`/`arg`, `started`, `dead` | guest-affecting | saved; the coroutine is stored as its slot index (`sa1_plat_coro_index`) |
| `running` | guest-affecting | saved as a task index |
| `gctx` (always `&ctx` of `main.c`) | fixed pointer | not saved: every wrapped entry sets it from its argument before it is used |
| `switches` | statistic | saved (cheap, keeps the report honest) |
| the coroutines' native stacks | guest-affecting | saved: every used slot, `rsp` and `[rsp, top)` |

### Shinobi (`hle/shinobi/`)

| File | State | Treatment |
|---|---|---|
| `sys.c` | `frame_count` (the guest frame counter, the virtual clock's integer part) | saved |
| `timer.c` | `frame_seen`, `reads_in_frame` (strict mode: the sub-frame clock) | saved; `frame_t0` (host time, non-strict) is not: it re-bases on the first read |
| `hw.c` | TMU1: `tmu_tcnt1_start`, `tmu_tcor1`, `tmu_tstr`, `tmu_tcr1`, `tmu_tcnt1_t0` (guest time) | saved |
| `vsync.c` | `vblank_callback`, `vblank_in_callback`, `vblank_in_arg` | saved |
| `pad.c` | `hle_pad_state` (the pad the backend wrote for this frame), `prev_on` | saved |
| `gdfs.c` | `files[8]` (path, size, open), `work_area`, `handles_base`, `cur_dir`, `initialised`, `err_func`, `err_arg` | saved (the read positions themselves are in the guest handle records); `last_opened` is a debug name, not saved |
| `memory.c` | `blocks[1024]`, `nblocks`, `heap_base`, `heap_size`, `heap_used`, `heap_peak` | saved |
| `chain.c` | `chain[16]`, `in_callbacks` | saved |
| `cfg.c` | `bu_complete_cb`, `bu_progress_cb` | saved |
| `hle_init.c` | `hle_strict`, `hle_rtc_epoch` | recorded in the header; a load with a different setting warns (the run would diverge from the saving one) |

### BIOS (`hle/bios/syscalls.c`)

| State | Treatment |
|---|---|
| `gd_req_id`, `gd_pending` | saved |
| `sys_props`, `sys_id`, `flash_part` | constants |

### Overlays (`hle/overlay/`)

| State | Treatment |
|---|---|
| installed tables | see `sa1_slots` above |
| `hle_overlay_files` registry | constant (generated) |

### Ninja / KAMUI (`hle/ninja/`)

| File | State | Treatment |
|---|---|---|
| `kamui.c` | `sh_addr`, `desc_addr`, `bufs[3]`, `clear_color`, fog: `fog_near`, `fog_far`, `fog_color`, `fog_density`, `fog_density_reg`, `fog_table[128]`, `fog_table_set`; 2D ranges: `ranges2d`, `nranges2d`, `mark_depth`, `mark_start`; `frame_rendered` | saved; after a load the fog is re-applied to the backend (`draw_set_fog`), since GL fog state is native |
| `kamui.c` | `verts`/`strips`/`strip_states` (heap, rebuilt every render), `screenshot_path`, mark state, `dump_frame`, `stat_*`, `buf_high`, `last_*`, `frame_rendered_count` | per-frame scratch, options or statistics: not saved |
| `texture.c` | `memlist_base`, `memlist_count`, `slots[0x800]` (handle, bytes, used, gbix, index, w, h, formats), `total_bytes`, `ntex`, `palette[1024]`, `decode_index` | saved; the handles are backend texture numbers that also sit in guest RAM (TCW words, memlist entries), so the backend restores its textures under the same numbers |
| `texture.c` | `decode_name`, `dump_dir`, `dump_min_w`, `tex_log` | debug / options: not saved |
| `light.c`, `math.c`, `draw2d.c` | none (all in guest RAM) | |
| `model.c` | `tvs`, `svs`, `cnk_*` caches | per-draw scratch: not saved |
| `hle_aspect_wide` | option that shaped guest RAM at `njInitSystem` | recorded in the header; a mismatch warns |

### Backend (`backend/desktop/`)

| File | State | Treatment |
|---|---|---|
| `main.c` | `ctx` (the live `sh4_ctx`: the current coroutine's registers at the hook) | saved |
| `main.c` | `--goto` autopilot: `goto_cur`, `goto_cal`, `goto_nudge`, `goto_theta`, `goto_best`, `goto_px0/pz0`, `goto_ux/uz`, `goto_progress_frame` | saved (it decides future pad input from past positions) |
| `main.c` | option tables (`presses`, `sticks`, `pokes`, `dumps`, `shots`, `saves`), `frame_limit`, `headless`, `scale`, `log_file`, `t0`, stack high-water marks | host / options: the loading run's own apply |
| `main.c` | the crash handler: on a coroutine stack the frame-based dispatch cannot reach the unhandled-exception filter (the bottom frame has no unwind data), so a vectored handler runs the same report for the fatal codes | host; a stack overflow past the probe's margin (160 KB) hits the no-access gap and may die without the report |
| `draw_gl.c` | `texs[]`: GL texture names, sizes, formats; from this change also a CPU copy of every texture's pixels (and CLUT) | saved from the copies (`draw_gl_texture_state`); on load the GL textures are destroyed and re-created under the saved handles; headless runs keep the copies too, so a headless save loads into a window |
| `draw_gl.c` | window, GL context, projection matrices, pacing clock, `cur` state | native / per frame: rebuilt or irrelevant |
| `input_tape.c` | replay position: the tape is indexed by frame, so the frame counter is the position; `--check` rows whose window ended before the load frame are marked done (not failed) on load | nothing saved; `--record` together with `--load` is refused (a tape must start at frame 1) |
| `platform_desktop.c` | `data_root`, coroutine descriptors | options / fixed (section 1) |

### Not in guest RAM and reviewed as host pointers

`grep` of `hle/` for pointer-to-integer casts finds none written to guest
RAM; the heap bookkeeping (`memory.c`), gdFs handles, texture handles and
TA buffer pointers are all guest addresses or small integers. The texture
handle in a TCW is the backend slot number, not a GL name.

## 3. File format (`.state`, version 1)

```
"SA1STATE" u32 version=1 u32 header_len
header: u8 exe_sha256[32], u64 static_anchor (&sa1_ram), u64 fixed_base,
        u32 frame, u32 flags (bit0 strict, bit1 wide aspect), i64 rtc_epoch,
        u64 raw_len, u64 packed_len
body (RLE-packed, raw_len bytes when unpacked): a sequence of chunks
        tag[8] u32 len bytes[len]
```

Chunks, in order: `RAM` (the whole `SA1_RAM_ALLOC` window), one chunk per
state blob (`hle/state.h`: each module writes its variables as named blobs,
and a load checks every name and size against what this build expects, so a
stale file fails with the name of the first blob that differs), `TEXTURES`
(the backend copies), `STACKS` (slot count; per used slot: index, `rsp`,
bytes from `rsp` to the top), `CURRENT` (the slot that was running the frame
hook). The RLE is byte runs: `0x00 n` = n zero bytes, `0x01 n` = n literal
bytes, `0x02 n b` = n copies of b, n a LEB128 varint; the 16 MB of guest RAM
packs to roughly what the game has loaded (about 9 MB in Emerald Coast).

## 4. Where a save happens, and how a load resumes

`--save FRAME:FILE` and F5 act in the frame hook (`on_frame`, `main.c`),
after `--poke` and `--dump` of that frame and before `input_tape_frame_end`,
so a state saved at F holds what `--dump F` dumped. The hook calls
`savestate_request`, which switches to the supervisor context; the
supervisor writes the file (every coroutine is suspended at that moment,
the requester included, so their stacks are complete) and switches back,
and the hook returns as if nothing had happened.

A load (`--load` before the first frame, or F8 in the window) is the same
in reverse: the supervisor restores guest RAM, the blobs, the textures and
the stacks, then switches into the saved current coroutine, which resumes
inside `savestate_request` and returns to the hook of the saved frame. With
`--load`, `fn_8c010000` is never entered: `hle_init` runs (it writes the
same BIOS words a restore overwrites and registers the same natives), then
the supervisor resumes the file. Everything the hook does after the save
point (checks, stats, frame limit) then runs in the loaded process with its
own options, which is how `--frames 6000 --dump 6000:x.bin` works on a
state saved at 3000.

## 5. Proof

`python tools/replay_test.py --savestate` (needs `SA1_DUMP`): records the
scripted route (the M1 presses and the Emerald Coast pokes at 5290) to a
tape in strict mode with `--save 3000` and `--save 6000` and `--dump 6000`,
then replays the tape with `--load` of the 3000 state to frame 6000 with
the same `--dump` and `--save 6000`. The RAM dumps must be byte-identical,
and the two 6000 states must agree in every chunk but the coroutine
stacks. Result (2026-10-18, Debug build): the 18 MB dumps are identical;
of the 2,594 chunks of the 6000 states one `stack.bytes` differs by 28
bytes (dead slots between frames that the two histories filled
differently; nothing reads them). The 3000 state packs 41.9 MB to 17.1 MB.
Usage: `tests/replay/README.md`, "Save states".

Without game data, `tests/savestate/run_synthetic.py` (CI job
`savestate`) drives `tests/savestate/savestate_test.c`: five coroutines
whose state lives in their locals, a guest RAM they write to and a module
blob, saved at one step by one process and loaded by another; the
checksum at the end must match a straight run's, a file whose build hash
is altered must be refused, and the RLE must round-trip.
