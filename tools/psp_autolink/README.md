# sa1autolink: start PSPLink from the XMB automatically

`sa1autolink.prx` is an XMB plugin for ARK-4 on the development PSP. Every
time the XMB comes up (power-on, or coming back from a game or from PSPLink),
it waits about 8 seconds and then launches PSPLink:

1. `ms0:/PSP/GAME/PSPLINK2/EBOOT.PBP` (the patched PSPLink), or, if that is
   missing,
2. `ms0:/PSP/GAME/PSPLINK/EBOOT.PBP` (stock PSPLink).

So the PC side (`usbhostfs_pc`, `tools/psp_run.py`) can get PSPLink back
without anyone touching the PSP: if PSPLink exits or resets to the XMB, it is
running again some 10 seconds later.

It launches at most once per XMB visit. The plugin's thread makes one
attempt and then exits, and if neither EBOOT exists, or the launch fails, the
XMB carries on as normal.

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
  after it appears. The plugin checks the triggers every 0.1 s during the
  wait, so a press at any point in that window cancels the launch for that
  XMB visit.
- **Until the file is deleted:** create `ms0:/SEPLUGINS/sa1autolink.off`
  (any content, even empty). The plugin then does nothing.
- **For good:** change the line in `PLUGINS.TXT` to
  `xmb, ms0:/SEPLUGINS/sa1autolink.prx, off`, or delete the line. ARK's
  plugin manager in the XMB (or in the recovery menu) can toggle it too.

It also stays put while USB mode (mass storage) is active, so it never pulls
the memory stick away from the PC. You still have to enter USB mode within
the 8 seconds, though, or hold L/R first. The easy way: hold R, then pick
USB Connection.

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

## How it launches

It makes the same calls that ARK-4's own VshCtrl makes when you start a
memory-stick homebrew from the XMB (`core/vshctrl/xmbiso.c`,
`homebrewloadexec`):

```
sctrlSESetBootConfFileIndex(0);   /* MODE_UMD: no ISO driver */
sctrlSESetUmdFile("");
sctrlKernelLoadExecVSHMs2(path, &param);   /* apitype 0x141 */
```

Here `param` is a zeroed `SceKernelLoadExecVSHParam` with `key = "game"`,
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
