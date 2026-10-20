# PSPLink patches: a console that recovers from a dead host

The console runs under PSPLink v3.2.1 (pspdev/psplinkusb, BSD, commit 4b767c0). Stock
PSPLink wedges when usbhostfs_pc dies while the PSP is connected. The PSP then needs a
power-cycle and a relaunch. These two patches let it recover by itself. Only the patches
and the build script live here; the PSPLink source is fetched at build time.

## Why it wedged

From the 2026-10-20 audit; line numbers are from 4b767c0.

1. **Nothing on the PSP side times out.** The PSP waits on its bulk transfers with no
   timeout (usbhostfs/main.c 366, 406, 434, 470; marked "TODO: Add a timeout"). A thread
   writing stdout to a dead host therefore blocks forever, holding the bus semaphore, and
   every other host0:, stdout or shell call queues behind it. The game never reaches
   ExitGame, so PSPLink never resets.
2. **The PSP reads the host's hello only on USB attach** (main.c 1030). A host killed
   with the cable still in causes no detach. On Windows (libusb with WinUSB), the next
   host's "reset" only resets the pipes, so attach never fires again. Every new host's
   hello on ep 2 times out, forever ("waiting for device / Connected" once a second).
3. **usbhostfs_pc dies of SIGPIPE.** A client closes its socket while the PSP is still
   streaming (psp_run.py did this at the game's exit line), and the next write kills the
   host with no message (Cygwin, no SIG_IGN). The writes also race with the async thread
   closing the sockets, and the async thread uses `usbhdr` while the main thread closes it.

## The patches

**0001, usbhostfs.prx (PSP side)**
- Every bulk transfer has a timeout: 5 s for bulk in (a live host reads it all the time),
  20 s for bulk out (the reply to a file command). On a timeout the request is cancelled
  (`sceUsbbdReqCancelAll`) and the link is dropped (`link_lost`). Callers then fail at
  once instead of queueing. The bus semaphore waits are bounded too.
- After an attach or a dropped link, the USB thread polls ep 2 for a hello every 0.5 s
  (instead of one read that blocks forever), so it keeps serving detach and async events.
- **Re-hello:** a 4-byte HOSTFS magic arriving on the async endpoint (ep 3, which a
  connected PSP always reads) means a new host has arrived. The old session is dropped
  and the PSP waits for the hello on ep 2. A stock PSP ignores those 4 bytes.
- The async request is cancelled and queued again for each new session. The `stop_func`
  bug (it reset `g_mainevent` where it meant `g_transevent`) is fixed.

**0002, usbhostfs_pc (host side)**
- `SIGPIPE` is ignored, and the client sockets are written with `send(MSG_NOSIGNAL)`
  under a lock that `close()` also takes. A client that goes away can no longer kill the
  host or hit a reused fd.
- `usbhdr` is opened and closed under a lock that the async thread's ep 3 writes also
  take (their timeout drops from 10 s to 2 s). `euid_usb_bulk_read` uses its `dev` argument.
- stdout is line-buffered, so a log redirected to a file is complete up to a crash.
- **Handshake fallback:** if the hello on ep 2 times out, the host announces itself on
  ep 3 (re-hello on a patched PSP) and retries ep 2 three times. If those also time out,
  it **resumes** the PSP's previous session without a hello, for a stock PSP. Draining
  ep 0x81 frees a thread stuck in stdout. The PSP's next commands work as they did with
  the old host, because the PSP still thinks it is connected.

## What recovers (host killed or restarted with the cable in)

| PSP state when the host died      | stock + stock | stock PSP + new host  | new PSP + new host       | new PSP + stock host           |
|-----------------------------------|---------------|-----------------------|--------------------------|--------------------------------|
| idle, connected                   | wedge         | resumes the session   | re-hello (about 1-2 s)   | only at the PSP's next output (+5 s) |
| game writing stdout               | wedge         | resumes, drains it    | 5 s timeout, then hello  | 5 s timeout, then hello        |
| waiting for a host0: reply        | wedge         | wedge                 | 20 s timeout, then hello | 20 s timeout, then hello       |
| client closes mid-stream (SIGPIPE)| host dies     | host survives         | host survives            | host dies, then row 2          |

Only the patched pair recovers from every row.

## Build

```
tools/psplink/build.sh ../tools-psplink/patched ../tools-psplink/m5c ../tools-psplink/cygwin
```

- The PSP side builds in WSL with /opt/pspdev (`make -f Makefile.psp release`,
  PSP_FW_VERSION=271). The host side builds with a Cygwin install that needs no admin
  rights: `setup-x86_64.exe --no-admin --quiet-mode --root DIR --packages
  gcc-core,make,pkg-config,libusb1.0-devel`.
- Output: `PSPLINK2/` (the stock PSPLink files plus the patched usbhostfs.prx; only
  usbhostfs.prx differs from the card's PSPLINK) and `psplinkusb-windows/`
  (usbhostfs_pc.exe plus cygwin1.dll, cygusb-1.0.dll, cyggcc_s-seh-1.dll).
- usbhostfs.prx builds byte-identical from the patches: sha256 4bb2c586c6bd5a64...
- psp_run.py picks the host with `--tools DIR` (or SA1_PSP_PSPLINK_TOOLS).

## Deploy and test (not done yet)

1. **Card.** Copy `PSPLINK2/` to `E:\PSP\GAME\PSPLINK2\` and keep `PSPLINK\` as the
   fallback. From the PSPLink shell this works over host0:, with no USB mass storage.
   The maintainer launches PSPLINK2 once from the XMB.
2. **Host.** Never swap a *connected* stock host while the PSP runs the stock
   usbhostfs.prx: if the resume path fails, the console wedges. Swap only right after a
   PSPLink (re)launch with no host running, or once the stock host is already dead (then
   the new host is the recovery attempt and there is nothing to lose).
3. **Recovery test** (PSPLINK2 plus the new host): during a game run, kill
   usbhostfs_pc 10 times at random points (idle, mid-stdout, mid-host0: read) and start
   it again. The PSP must reconnect each time with no power-cycle, and the run's next
   commands must work. Also close the psp_run sockets mid-stream 10 times; the host must
   survive. Log every attempt.
