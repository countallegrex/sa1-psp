# sa1autolink: start PSPLink from the XMB automatically

`sa1autolink.prx` is an XMB plugin for ARK-4 on the development PSP. Every
time the XMB comes up (power-on, or coming back from a game or from PSPLink),
it waits about 8 seconds and then launches PSPLink:

1. `ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP` (the patched PSPLink), or, if that one
   fails validation,
2. `ms0:/PSP/GAME/PSPLINK/EBOOT.PBP` (stock PSPLink).

So the PC side (`usbhostfs_pc`, `tools/psp_run.py`) can get PSPLink back
without anyone touching the PSP: if PSPLink exits to the XMB, it is running
again some 10 seconds later.

It is built to run with nobody around to rescue it:

- **Once per XMB visit.** The plugin's thread makes at most one launch
  attempt and then exits. If the launch fails, the XMB carries on as normal.
- **Crash-loop guard.** `ms0:/SEPLUGINS/sa1autolink.st` (24 bytes, rewritten
  once per launch) holds the last three launch times and a back-off level.
  If the last three launches fall within 3 minutes, with no healthy gap in
  between, it backs off: the next launch waits 10 minutes, then 30, then
  60, and 60 from then on, counted from the previous launch. The XMB stays
  usable the whole time. A return to the XMB 10 minutes or more after the
  last launch means PSPLink was fine, and the history is cleared. If the
  state file can't be written, the plugin doesn't launch at all, since the
  guard would then be blind. If the file is unreadable, it starts fresh.
- **Validation.** An EBOOT counts only if it is more than 100 KB (PSPLink's is
  about 131 KB), starts with the PBP magic, and has the `psplink.prx` that
  its bootstrap loads from the same folder. If PSPLINK2 fails, PSPLINK is
  tried. If both fail, nothing is launched.
- **Holds.** While USB mass storage is active, or the battery is below 5%
  (when a battery is present), it waits. Once the condition clears, it waits
  another quiet 8 seconds. Nothing touches the card while USB is active.
- **Never blocks the XMB.** `module_start` only creates one low-priority
  thread (0x30, 8 KB stack, static buffers). Every import is a user-mode
  library the VSH itself uses: ThreadManForUser, IoFileMgrForUser,
  UtilsForUser (`sceKernelLibcTime`), sceCtrl, sceUsb, scePower, plus ARK's
  SystemCtrlForUser. Any failed call means giving up quietly. An unresolved
  import returns an error code, and that is never taken as permission. The
  PRX links no libc.

## Log

Each decision is logged with an RTC timestamp to `ms0:/SEPLUGINS/sa1autolink.log`.
The lines are buffered and written once per XMB visit, before the launch.
Once the file would pass 16 KB it is truncated, starting with a marker line.
Read it from the PC over `host0:` or USB. Typical lines:

```
2026-10-21 04:10:02 launching ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP
2026-10-21 04:10:41 3 launches within 3 min: crash loop, backing off
2026-10-21 04:10:41 back-off level 1
2026-10-21 04:10:41 waiting until 9 min from now before launching
2026-10-21 04:13:05 L/R held: not launching this time
2026-10-21 04:20:30 skipping ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP: missing or invalid
```

## Install

Copy `sa1autolink.prx` to `ms0:/SEPLUGINS/` and add this line to
`ms0:/SEPLUGINS/PLUGINS.TXT`:

```
xmb, ms0:/SEPLUGINS/sa1autolink.prx, on
```

Then reboot the PSP, or restart the VSH.

## Turning it off

Any one of these works:

- **Once:** hold **L** or **R** while the XMB comes up, until about 8 seconds
  after it appears, or at any time during a back-off wait. The triggers are
  checked every 0.1 s, so a press anywhere in that window cancels the launch
  for this XMB visit.
- **Until the file is deleted:** create `ms0:/SEPLUGINS/sa1autolink.off`
  (any content, even empty). It is checked every 5 s during the waits and
  again just before the launch.
- **For good:** change the line in `PLUGINS.TXT` to
  `xmb, ms0:/SEPLUGINS/sa1autolink.prx, off`, or delete the line. ARK's
  plugin manager in the XMB (or in the recovery menu) can toggle it too.

To reach USB Connection without PSPLink launching first, hold R as the XMB
comes up, then pick USB Connection. Once USB mode is on, the plugin waits.
To clear a back-off by hand, delete `sa1autolink.st`.

## Recovery

If the PSP ever gets stuck in a launch loop that L/R can't break:

- Hold **R** while powering on to get ARK-4's recovery menu. Recovery mode
  loads no plugins. From there, turn the plugin off (Plugins), or turn on USB
  and edit `PLUGINS.TXT` from the PC.
- Or hold **START** while ARK boots: ARK then skips every plugin for that boot
  (`core/systemctrl/src/exitgame.c`, `checkControllerInput`).

## Build

Needs pspsdk (`psp-config` on PATH; on this machine, WSL's `/opt/pspdev`):

```
wsl -d Ubuntu -u root -- bash -l "<project-dir>/sa1-psp/tools/psp_autolink/build.sh"
```

Output: `build-autolink/sa1autolink.prx` (git-ignored, like every `.prx`).

`sim/run.sh` (host gcc, e.g. in WSL) compiles the same `sa1autolink.c`
against fake PSP headers and a fake clock and Memory Stick. It checks the
back-off sequence (13 s, 13 s, then 10, 30, 60, 60 min for a PSPLink that
dies at once), the reset after a healthy gap, the fallback and validation,
the USB and battery holds, L/R, the off file, an unwritable card, a dead
clock, the log cap and a corrupt state file, all under ASan and UBSan.

## How it launches

It makes the same calls that ARK-4's own VshCtrl makes when you start a
memory-stick homebrew from the XMB (`core/vshctrl/xmbiso.c`,
`homebrewloadexec`):

```
sctrlSESetBootConfFileIndex(0);   /* MODE_UMD: no ISO driver */
sctrlSESetUmdFile("");
sctrlKernelLoadExecVSHMs2(path, &param);   /* apitype 0x141 */
```

In the code, `param` is a zeroed `SceKernelLoadExecVSHParam` with `key = "game"`,
`argp` = the EBOOT path and `args` = its length + 1. ARK's `xmbctrl` and
VSH menu launch homebrew the same way.

The three imports come from ARK's SystemControl (`SystemCtrlForUser`), which
exports them by name hash. `stubs.S` declares them itself instead of
linking ARK's or pspsdk's systemctrl stubs. The NIDs were checked against
ARK-4 (`libs/SystemCtrlForUser/SystemCtrlForUser.S`,
`core/systemctrl/exports.exp`, commit `7c03c37`) and pspsdk's
`libpspsystemctrl_user.a`.

## Licence note

ARK-4 (github.com/PSP-Archive/ARK-4), like PRO CFW before it, is GPL-3.0 or
later. No ARK code or headers are copied here. This plugin uses only the
function names and NIDs needed to call ARK's exported API, plus pspsdk's
BSD-licensed headers. A reference checkout of ARK-4 is kept outside the
repo, in `tools-psplink/ark4-src/`.
