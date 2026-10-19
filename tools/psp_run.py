#!/usr/bin/env python3
"""Run the EBOOT on the PSP over USB with PSPLink: no card swap, live log.

    psp_run.py [--timeout S] [--quiet S] [--wait S] [--no-reset] [--build DIR]
               [--tools DIR] [--out FILE] [--linger S] [-- GAME_OPTION ...]
    psp_run.py --stop

The PSP runs PSPLink (ms0:/PSP/GAME/PSPLINK, started from the XMB) and is
on the USB cable; usbhostfs_pc on this machine serves DIR (default
build-psp/sa1psp) to it as host0:.  One run:

  1. stages DIR/psplink-run/: sa1psp.prx (the game linked as a PRX: PSPLink
     refuses the EBOOT's static ELF on ARK-4, 0x80020148) and args.txt (the GAME_OPTIONs after "--", plus
     --stdout --no-crash-screen --data ms0:/PSP/GAME/SA1PSP/data unless
     given);
  2. starts usbhostfs_pc rooted at DIR if nothing listens on its port
     (it keeps running for the next run; --stop ends it);
  3. waits for PSPLink to answer, resets it ('reset': a clean reboot into
     PSPLink, so the run starts from the same state every time), waits for
     it to come back and checks host0: is DIR;
  4. starts host0:/psplink-run/sa1psp.prx ('ldstart'): argv[0] puts the
     EBOOT's folder (args.txt, sa1psp.log, stderr.txt) on the PC, and the
     1 GB of game data comes from the card (--data ms0:/...);
  5. streams the PSP's stdout and stderr and PSPLink's shell output to the
     console and to FILE (default DIR/psplink-run/psp_run.log).

It returns when the game logs its exit line ("] exit: "), when PSPLink
reports an exception (the registers, then EPC and RA named against
build-psp/sa1psp/sa1psp_prx.elf, less the PRX's load offset, with
tools/psp_addr2line.sh through WSL), or at
--timeout.  --quiet S: after S seconds without a byte from the PSP it asks
PSPLink for its version; no answer means the console is hung below PSPLink
(interrupts off, a kernel or bus fault) and only holding POWER recovers it.

Exit status: 0 the exit line was seen, 2 an exception, 3 timeout or no
response, 4 setup failure (no tools, no PSP, a load error).

The game's own crash screen (sa1exc.prx) stays off (--no-crash-screen): it
would take the default exception handler from PSPLink, and it stops the
console in exception context, which takes the USB link down with it.  With
PSPLink's handler the faulting thread sleeps, the rest of the console runs,
and the next psp_run.py resets it.  The PSP is left as the run ended: after
an exception pspsh can still look around (thctx, exprint, memory).

This script holds PSPLink's shell connection while it runs; a pspsh opened
meanwhile takes it over (usbhostfs_pc keeps one client per channel) and the
run fails.  Close pspsh first, open it again afterwards.

One-time setup: PSPLink v3.2.1 (pspdev/psplinkusb, built in tools-psplink/
next to the repository) in ms0:/PSP/GAME/PSPLINK; the Windows usbhostfs_pc
(Cygwin build from the same commit) in tools-psplink/pc-windows/; and the
WinUSB driver bound to the PSP in PSPLink mode (USB 054C:01C9, "PSP Type B")
with Zadig, once, as administrator.  USB mode (054C:01C8) is not touched.

UNTESTED ON HARDWARE (written without a PSP attached): the shell protocol is
read from psplinkusb's pspsh and usbhostfs_pc sources (commit 4b767c0) and
checked against a stand-in server; usbhostfs_pc itself was run on Windows
without a PSP.  The reset, ldstart of the static ELF, PSPLink's exception
text and the no-answer probe have not been seen on a console.

Part of sa1-psp (MIT).
"""
from __future__ import annotations

import argparse
import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"
BASE_PORT = 10000                       # usbhostfs_pc -b: shell, gdb, stdout, stderr = +0..+3
STAGE = "psplink-run"                   # under host0:, the EBOOT's folder for the run
CARD_DATA = "ms0:/PSP/GAME/SA1PSP/data"
DEFAULT_TOOLS = os.path.join(os.path.dirname(REPO), "tools-psplink", "pc-windows", "psplinkusb-windows")

# pspsh/psplink shell framing (psplink/shellcmd.h): a frame is 0xFF ... 0xFE;
# its first byte < 0x80 is text, else a command code.
CMD_BEGIN, CMD_END, CMD_SUCCESS, CMD_ERROR, CMD_CWD, CMD_DISASM = 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xF8

EXIT_OK, EXIT_EXCEPTION, EXIT_TIMEOUT, EXIT_SETUP = 0, 2, 3, 4


class SetupError(Exception):
    pass


# ---- staging --------------------------------------------------------------

def stage_prx(prx: str, dst: str) -> None:
    """The game as a PRX (build-psp/sa1psp/sa1psp.prx, the CMake target
    sa1psp_prx): PSPLink's ldstart refuses the EBOOT's static ELF on ARK-4
    (0x80020148).  Copied only when it changed."""
    st = os.stat(prx)
    try:
        if os.stat(dst).st_mtime >= st.st_mtime and os.stat(dst).st_size == st.st_size:
            return
    except FileNotFoundError:
        pass
    tmp = dst + ".tmp"
    shutil.copyfile(prx, tmp)
    os.replace(tmp, dst)


def write_args(path: str, game_args: list[str]) -> list[str]:
    args = list(game_args)
    for a in args:
        if not a or any(c.isspace() for c in a):
            raise SetupError(f"game option {a!r}: args.txt splits on white space")
    if "--data" not in args:
        args += ["--data", CARD_DATA]
    for flag in ("--no-crash-screen", "--stdout"):
        if flag not in args:
            args.insert(0, flag)
    with open(path, "w", newline="\n") as f:
        f.write("# written by tools/psp_run.py for one psplink run\n")
        f.write(" ".join(args) + "\n")
    return args


# ---- usbhostfs_pc ---------------------------------------------------------

def find_tool(name: str, tools: str | None) -> str | None:
    exe = name + (".exe" if os.name == "nt" else "")
    dirs = [tools, os.environ.get("SA1_PSP_PSPLINK_TOOLS"), DEFAULT_TOOLS]
    if os.environ.get("PSPDEV"):
        dirs.append(os.path.join(os.environ["PSPDEV"], "bin"))
    for d in dirs:
        if d and os.path.isfile(os.path.join(d, exe)):
            return os.path.join(d, exe)
    return shutil.which(name)


def port_open(port: int) -> bool:
    try:
        with socket.create_connection((HOST, port), timeout=0.5):
            return True
    except OSError:
        return False


def start_hostfs(exe: str, root: str, log_path: str) -> None:
    """usbhostfs_pc with root as its cwd: host0..7 default to the cwd (a root
    argument not starting with '/' is taken relative to the cwd, so a Windows
    path cannot be passed to the Cygwin build)."""
    log = open(log_path, "w")
    flags = 0
    if os.name == "nt":
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    subprocess.Popen([exe], cwd=root, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                     creationflags=flags, close_fds=True)
    log.close()
    for _ in range(50):
        if port_open(BASE_PORT):
            return
        time.sleep(0.1)
    raise SetupError(f"usbhostfs_pc did not open port {BASE_PORT} (see {log_path})")


def stop_hostfs() -> int:
    if os.name == "nt":
        r = subprocess.run(["taskkill", "/IM", "usbhostfs_pc.exe", "/F"])
    else:
        r = subprocess.run(["pkill", "-x", "usbhostfs_pc"])
    return 0 if r.returncode == 0 else 1


# ---- the session: three sockets, one event queue --------------------------

class Session:
    def __init__(self, log_file, hostfs_log: str | None):
        self.q: queue.Queue = queue.Queue()
        self.log = log_file
        self.socks: dict[str, socket.socket] = {}
        self.partial = {"out": "", "err": "", "shell": ""}
        self.frame: bytearray | None = None
        self.reply = None                    # ("ok"|"error", value) of the last command
        self.cmd_lines: list[str] = []       # shell text of the last command
        # PSPLink answers commands in order, with no tag: a command that timed
        # out (a 'ver' while the PSP is still connecting) is answered later,
        # and that answer must not be taken for the next command's.  Replies
        # are counted against commands sent; the last command's reply is the
        # one that brings the count to zero.  A reset drops the old ones.
        self.pending = 0
        self.last_data = time.monotonic()
        self.exit_line: str | None = None
        self.exit_at = 0.0
        self.resetting = False
        self.booted = False                  # an unsolicited prompt after a reset
        self.ex_lines: list[str] | None = None
        self.ex_at = 0.0
        self.ex_done = False
        self.lost: str | None = None
        self.hostfs_log = hostfs_log
        self.main_at: int | None = None      # the game's "image: main at 0x..." line (the PRX's load address)
        self.hostfs_pos = 0

    def say(self, text: str) -> None:
        line = "psp_run: " + text
        print(line, flush=True)
        self.log.write(line + "\n")
        self.log.flush()

    def emit(self, prefix: str, line: str) -> None:
        print(prefix + line, flush=True)
        self.log.write(prefix + line + "\n")

    def connect(self) -> None:
        for name, off in (("shell", 0), ("out", 2), ("err", 3)):
            try:
                s = socket.create_connection((HOST, BASE_PORT + off), timeout=3)
            except OSError as e:
                raise SetupError(f"cannot connect to usbhostfs_pc port {BASE_PORT + off}: {e}")
            s.settimeout(None)
            self.socks[name] = s
            threading.Thread(target=self._reader, args=(name, s), daemon=True).start()

    def close(self) -> None:
        for s in self.socks.values():
            try:
                s.close()
            except OSError:
                pass

    def _reader(self, name: str, s: socket.socket) -> None:
        while True:
            try:
                data = s.recv(65536)
            except OSError:
                data = b""
            self.q.put((name, data))
            if not data:
                return

    def send(self, *args: str) -> None:
        """A shell command as pspsh sends it: each argument NUL-terminated, then 0x01."""
        self.reply = None
        if self.pending == 0:
            self.cmd_lines = []
        self.pending += 1
        payload = b"".join(a.encode("latin-1") + b"\0" for a in args) + b"\x01"
        self.socks["shell"].sendall(payload)

    def command(self, *args: str, timeout: float = 5.0):
        self.send(*args)
        end = time.monotonic() + timeout
        while self.reply is None and time.monotonic() < end and not self.lost:
            self.pump(min(0.2, max(0.0, end - time.monotonic())))
        return self.reply

    # -- event handling --
    def pump(self, timeout: float) -> None:
        self._tail_hostfs()
        try:
            name, data = self.q.get(timeout=timeout)
        except queue.Empty:
            return
        if not data:
            self.lost = f"usbhostfs_pc closed the {name} channel (exited, or another client such as pspsh took it)"
            return
        self.last_data = time.monotonic()
        if name == "shell":
            self._shell_bytes(data)
        else:
            self._text(name, data.decode("utf-8", "replace"))

    def _lines(self, key: str, text: str) -> list[str]:
        buf = (self.partial[key] + text).replace("\r", "")
        *lines, self.partial[key] = buf.split("\n")
        return lines

    def _text(self, name: str, text: str) -> None:
        for line in self._lines(name, text):
            self.emit("" if name == "out" else "stderr| ", line)
            m = re.search(r"image: main at (0x[0-9a-fA-F]+)", line) if name == "out" else None
            if m:
                self.main_at = int(m.group(1), 16)
            if name == "out" and "] exit: " in line and self.exit_line is None:
                self.exit_line = line
                self.exit_at = time.monotonic()
        self.log.flush()

    def _shell_bytes(self, data: bytes) -> None:
        for b in data:
            if b == CMD_BEGIN:
                self.frame = bytearray()
            elif b == CMD_END:
                if self.frame:
                    self._frame(bytes(self.frame))
                self.frame = None
            elif self.frame is not None:
                self.frame.append(b)
        self.log.flush()

    def _frame(self, fr: bytes) -> None:
        code, body = fr[0], fr[1:].decode("latin-1")
        if code < 0x80:
            for line in self._lines("shell", fr.decode("latin-1")):
                self._shell_line(line)
        elif code in (CMD_SUCCESS, CMD_ERROR):
            self.pending = max(0, self.pending - 1)
            if self.pending == 0:
                self.reply = ("ok" if code == CMD_SUCCESS else "error", body)
            else:
                self.cmd_lines = []          # that text answered an older command
        elif code == CMD_CWD:
            if self.resetting:
                self.booted = True
                self.pending = 0             # the new instance: nothing older is answered
        elif code == CMD_DISASM:
            addr, _, insn = body.partition(":")
            self.emit("psplink| ", f"instruction at {addr}: {insn}")

    def _shell_line(self, line: str) -> None:
        self.emit("psplink| ", line)
        self.cmd_lines.append(line)
        if "Resetting psplink" in line:
            self.resetting = True
            self.booted = False
            self.pending = 0                 # the old instance answers nothing more
        if line.startswith("Exception - ") and self.ex_lines is None:
            self.ex_lines, self.ex_at = [], time.monotonic()
        if self.ex_lines is not None and not self.ex_done:
            self.ex_lines.append(line)
            if re.search(r"\bra:0x[0-9A-Fa-f]{8}", line):
                self.ex_done = True

    def _tail_hostfs(self) -> None:
        """usbhostfs_pc's stderr lines (connects, disconnects, USB errors), when this script started it."""
        if not self.hostfs_log:
            return
        try:
            with open(self.hostfs_log, "r", errors="replace") as f:
                f.seek(self.hostfs_pos)
                chunk = f.read()
                self.hostfs_pos = f.tell()
        except OSError:
            return
        for line in chunk.splitlines():
            if line.strip():
                self.emit("usbhostfs| ", line)


# ---- PSPLink steps --------------------------------------------------------

def wait_psplink(sess: Session, seconds: float) -> bool:
    """One 'ver' at a time: a resend while the first is still queued in
    usbhostfs_pc (the PSP connecting, or rebooting) would be answered too,
    later, and flood the shell; so a 'ver' is resent only every 15 s."""
    end = time.monotonic() + seconds
    resend = 0.0
    while time.monotonic() < end and not sess.lost:
        if time.monotonic() >= resend:
            if resend:
                sess.say("still waiting for PSPLink (running on the PSP? cable in? driver bound with Zadig?)")
            sess.pending = 0
            sess.send("ver")
            resend = time.monotonic() + 15
        sess.pump(0.2)
        if sess.reply and sess.reply[0] == "ok" and any("PSPLink" in l for l in sess.cmd_lines):
            # let any answer to an older 'ver' arrive before the next command
            quiet_until = time.monotonic() + 1.0
            while time.monotonic() < quiet_until and not sess.lost:
                sess.pump(0.2)
            sess.pending = 0
            return True
    return False


def reset_psplink(sess: Session, seconds: float) -> bool:
    sess.say("resetting the PSP (psplink 'reset')")
    sess.resetting = sess.booted = False
    sess.send("reset")
    t0 = time.monotonic()
    while not sess.resetting and time.monotonic() - t0 < 5 and not sess.lost:
        sess.pump(0.2)
    # The reboot takes USB down within a moment of "Resetting psplink";
    # a 'ver' answered before that would be the old instance.
    t1 = time.monotonic()
    while time.monotonic() - t1 < 3 and not sess.booted and not sess.lost:
        sess.pump(0.2)
    while not sess.booted and time.monotonic() - t0 < seconds and not sess.lost:
        sess.pump(0.2)
    return wait_psplink(sess, max(10.0, seconds - (time.monotonic() - t0)))


def link_address(symbol: str, elf: str, distro: str) -> int | None:
    """symbol's address in the ELF (psp-nm, through WSL on Windows)."""
    if os.name == "nt":
        drive, rest = os.path.splitdrive(os.path.abspath(elf))
        path = "/mnt/" + drive[0].lower() + rest.replace("\\", "/")
        cmd = ["wsl.exe", "-d", distro, "--exec", os.environ.get("SA1_PSP_WSL_PSPDEV", "/opt/pspdev") + "/bin/psp-nm", path]
    else:
        cmd = ["psp-nm", elf]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == symbol:
            return int(f[0], 16)
    return None


def name_addresses(addrs: list[str], elf: str, distro: str, delta: int = 0) -> list[str]:
    """tools/psp_addr2line.sh on the addresses (less the PRX's load offset):
    through WSL on Windows."""
    addrs = ["0x%08x" % ((int(a, 16) - delta) & 0xFFFFFFFF) for a in addrs if 0x08800000 <= int(a, 16) < 0x0C000000]
    if not addrs:
        return []
    if os.name == "nt":
        rel = os.path.relpath(elf, REPO).replace("\\", "/")
        if rel.startswith(".."):
            drive, rest = os.path.splitdrive(os.path.abspath(elf))
            rel = "/mnt/" + drive[0].lower() + rest.replace("\\", "/")
        cmd = ["wsl.exe", "-d", distro, "--cd", REPO, "--exec", "env",
               "PSPDEV=" + os.environ.get("SA1_PSP_WSL_PSPDEV", "/opt/pspdev"), "SA1_PSP_ELF=" + rel,
               "sh", "tools/psp_addr2line.sh", *addrs]
    else:
        env_elf = os.path.relpath(elf, REPO)
        cmd = ["env", "SA1_PSP_ELF=" + env_elf, "sh", "tools/psp_addr2line.sh", *addrs]
    try:
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return [f"(addr2line failed: {e})"]
    out = (r.stdout + r.stderr).strip().splitlines()
    return out or [f"(addr2line printed nothing, status {r.returncode})"]


def report_exception(sess: Session, elf: str, distro: str) -> None:
    text = "\n".join(sess.ex_lines or [])
    epc = re.search(r"EPC\s+-\s+(0x[0-9A-Fa-f]{8})", text)
    ra = re.search(r"\bra:(0x[0-9A-Fa-f]{8})", text)
    names = [("EPC", epc.group(1) if epc else None), ("RA", ra.group(1) if ra else None)]
    sess.say("exception: " + ", ".join(f"{n} {v}" for n, v in names if v))
    delta = 0
    if sess.main_at is not None:
        link = link_address("main", elf, distro)
        if link is not None:
            delta = sess.main_at - link
            sess.say(f"the PRX loaded 0x{delta & 0xFFFFFFFF:08x} above its link address (main at 0x{sess.main_at:08x})")
    for (n, v) in names:
        if not v:
            continue
        for line in name_addresses([v], elf, distro, delta):
            sess.say(f"  {n} {v}: {line}")
    sess.say("the PSP is halted in PSPLink (the faulting thread sleeps): pspsh can look further "
             "(thctx, exprint, memory); the next psp_run.py resets it")


# ---- main -----------------------------------------------------------------

def main() -> int:
    argv = sys.argv[1:]
    game_args: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, game_args = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--timeout", type=float, default=600, help="seconds from the launch to give up (exit 3)")
    ap.add_argument("--quiet", type=float, default=30, help="seconds without output before asking PSPLink whether the PSP is alive")
    ap.add_argument("--wait", type=float, default=90, help="seconds to wait for PSPLink to answer (start, and after the reset)")
    ap.add_argument("--no-reset", action="store_true", help="launch into the running PSPLink without resetting it first")
    ap.add_argument("--build", default=os.path.join(REPO, "build-psp", "sa1psp"), help="the folder with EBOOT.PBP and sa1psp.elf; host0:")
    ap.add_argument("--tools", help="folder with usbhostfs_pc (default tools-psplink/pc-windows/psplinkusb-windows next to the repo)")
    ap.add_argument("--out", help="the run's console log (default BUILD/psplink-run/psp_run.log)")
    ap.add_argument("--linger", type=float, default=3, help="seconds to keep reading after the exit line (the exit reports)")
    ap.add_argument("--wsl-distro", default="Ubuntu", help="WSL distribution with pspdev, for addr2line")
    ap.add_argument("--stop", action="store_true", help="stop usbhostfs_pc and exit")
    a = ap.parse_args(argv)
    if sys.stdout and hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    if a.stop:
        return stop_hostfs()

    build = os.path.abspath(a.build)
    pbp = os.path.join(build, "EBOOT.PBP")
    elf = os.path.join(build, "sa1psp_prx.elf")
    prx = os.path.join(build, "sa1psp.prx")
    stage = os.path.join(build, STAGE)
    try:
        if not os.path.isfile(pbp):
            raise SetupError(f"no {pbp} (tools/build_psp.sh)")
        os.makedirs(stage, exist_ok=True)
        if not os.path.isfile(prx):
            raise SetupError(f"no {prx} (build the sa1psp_prx target)")
        stage_prx(prx, os.path.join(stage, "sa1psp.prx"))
        args = write_args(os.path.join(stage, "args.txt"), game_args)
    except (SetupError, OSError) as e:
        print(f"psp_run: {e}", file=sys.stderr)
        return EXIT_SETUP

    out_path = a.out or os.path.join(stage, "psp_run.log")
    with open(out_path, "w", encoding="utf-8", newline="\n") as log:
        hostfs_log = None
        sess = None
        try:
            if port_open(BASE_PORT):
                print("psp_run: usbhostfs_pc is already running; checking that its host0: is", build, flush=True)
            else:
                exe = find_tool("usbhostfs_pc", a.tools)
                if not exe:
                    raise SetupError("usbhostfs_pc not found (--tools, SA1_PSP_PSPLINK_TOOLS, or tools-psplink/ next to the repo)")
                hostfs_log = os.path.join(stage, "usbhostfs_pc.log")
                print(f"psp_run: starting {exe} with host0: = {build}", flush=True)
                start_hostfs(exe, build, hostfs_log)
            sess = Session(log, hostfs_log)
            sess.connect()
            sess.say("args.txt: " + " ".join(args))
            sess.say("waiting for PSPLink")
            if not wait_psplink(sess, a.wait):
                raise SetupError(sess.lost or "PSPLink did not answer")
            if not a.no_reset and not reset_psplink(sess, a.wait):
                raise SetupError(sess.lost or "PSPLink did not come back after the reset")
            r = sess.command("ls", f"host0:/{STAGE}/", timeout=10)
            if not r or not any(l.endswith(" sa1psp.prx") or l == "sa1psp.prx" for l in sess.cmd_lines):
                raise SetupError(f"host0:/{STAGE}/sa1psp.prx not visible on the PSP: the running usbhostfs_pc "
                                 f"serves another folder (psp_run.py --stop, then run again)")
            sess.say(f"starting host0:/{STAGE}/sa1psp.prx")
            r = sess.command("ldstart", f"host0:/{STAGE}/sa1psp.prx", timeout=120)
            if not r or any("Failed to Load/Start" in l or "Error invalid file" in l for l in sess.cmd_lines):
                raise SetupError("ldstart failed" + ("" if r else " (no answer in 120 s)"))
            t0 = time.monotonic()
            probe_due = time.monotonic() + a.quiet
            while True:
                sess.pump(0.2)
                now = time.monotonic()
                if sess.lost:
                    sess.say(sess.lost)
                    return EXIT_SETUP
                if sess.ex_lines is not None and (sess.ex_done or now - sess.ex_at > 2):
                    report_exception(sess, elf, a.wsl_distro)
                    return EXIT_EXCEPTION
                if sess.exit_line and (sess.resetting or now - sess.last_data > a.linger
                                       or now - sess.exit_at > 10 * a.linger):
                    sess.say("exit line seen: " + sess.exit_line.split("] exit: ", 1)[1])
                    return EXIT_OK
                if now - t0 > a.timeout:
                    sess.say(f"timeout after {a.timeout:g} s; the game is left running (the next run resets it)")
                    return EXIT_TIMEOUT
                if now - sess.last_data > a.quiet and now >= probe_due:
                    sess.say(f"no output for {now - sess.last_data:.0f} s: asking PSPLink whether the PSP is alive")
                    if sess.command("ver", timeout=10) is None and not sess.lost:
                        sess.say("no answer: the PSP is hung below PSPLink (a kernel or interrupt-level fault, "
                                 "a bus lockup, or interrupts left off). Hold POWER until it turns off, "
                                 "start PSPLink again from the XMB, then rerun.")
                        return EXIT_TIMEOUT
                    sess.say("PSPLink answers: the console is alive; the game is quiet (loading, or stuck: "
                             "look for a 'watchdog:' line)")
                    probe_due = time.monotonic() + a.quiet
        except SetupError as e:
            if sess:
                sess._tail_hostfs()
                sess.say(str(e))
            else:
                print(f"psp_run: {e}", file=sys.stderr)
            return EXIT_SETUP
        except KeyboardInterrupt:
            print("psp_run: interrupted", file=sys.stderr)
            return 130
        finally:
            if sess:
                sess.close()


if __name__ == "__main__":
    sys.exit(main())
