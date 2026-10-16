# M-1 Toolchain

Exit test (README §8): a hello-world builds for both desktop and PSP from one
CMake tree.

Status: desktop **done**; PSP **waiting on WSL** (see "Resume after reboot").

## Installed (2026-10-16)

| Tool | Version | Path | How |
|---|---|---|---|
| Git | 2.55 | `C:\Program Files\Git\cmd` | before M-1 |
| CMake | 4.4.4 | `C:\Program Files\CMake\bin` | `winget install Kitware.CMake` |
| Ninja | 1.13.2 | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe` | `winget install Ninja-build.Ninja` |
| LLVM / clang | 23.1.3 | `C:\Program Files\LLVM\bin` | `winget install LLVM.LLVM` |
| VS 2022 Build Tools | 17.14 (MSVC 14.44.35207, Windows SDK 10.0.26100.0) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` | `winget install Microsoft.VisualStudio.2022.BuildTools` with `--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended` |
| PPSSPP | 1.20.4 | `C:\Program Files\PPSSPP` | `winget install PPSSPPTeam.PPSSPP` |
| pspdev (pspsdk) | v20261001 | `~/pspdev` inside WSL Ubuntu; `PSPDEV=$HOME/pspdev` | WSL, see below |

PATH: CMake and Git add themselves to the machine PATH, Ninja to the user
PATH. `C:\Program Files\LLVM\bin` and `C:\Program Files\PPSSPP` were added to
the **user** PATH by hand. Open a new shell to pick them up.

clang finds the MSVC headers and libraries on its own (via the VS setup
API), so no Developer Command Prompt is needed for the desktop build.

### Why WSL for the PSP SDK

pspdev publishes no native Windows build; its releases are Linux and macOS
tarballs only, and the official Windows instructions
(pspdev.github.io/installation/windows) are "install WSL, then follow
Ubuntu". The PSP build therefore runs inside WSL, on the same checkout under
`/mnt/c/...`, with the same CMake tree.

## Build commands

Desktop (PowerShell, from `sa1-psp\`):

```
cmake -S . -B build-desktop -G Ninja -DTARGET=desktop -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-desktop
build-desktop\backend\desktop\sa1psp_hello.exe
```

Prints `sa1-psp: hello from the desktop backend`.

PSP (WSL Ubuntu shell, from `sa1-psp/`):

```
cmake -S . -B build-psp -DTARGET=psp -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake
cmake --build build-psp
```

Produces `build-psp/EBOOT.PBP`. Run it in PPSSPP (Windows):

```
PPSSPPWindows64.exe build-psp\EBOOT.PBP
```

## PSP hello-world notes

- `PSP_MODULE_INFO("SA1PSP", PSP_MODULE_USER, 0, 1)`, main thread
  `PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU`.
- Heap: current pspsdk no longer defines `PSP_HEAP_SIZE_MAX()`. A negative
  `PSP_HEAP_SIZE_KB` gives libcglue's `sbrk` all free user memory minus a
  small threshold, which is what the old macro did. The source uses the macro
  if the SDK has it, else `PSP_HEAP_SIZE_KB(-1)`.
- `create_pbp_file` defaults to `MEMSIZE=2` in PARAM.SFO, despite its own
  comment saying 1. We pass `MEMSIZE 1` so a PSP-2000 or later gets its
  extended user memory (the 52 MB of README §7).
- The hello-world mallocs 16 MB (the future guest RAM array) and prints
  whether it worked.

## Resume after reboot

The commands below are for the maintainer. The Claude session was not allowed to run
`wsl --install` itself, because its permission settings block that kind of
system change.

1. **Install WSL** from an *administrator* PowerShell, then reboot when asked:

   ```
   wsl --install -d Ubuntu
   ```

2. **First run.** Open "Ubuntu" from the Start menu once and create the Linux
   user when prompted.

3. **Dependencies** (Ubuntu shell):

   ```
   sudo apt-get update
   sudo apt-get install build-essential cmake pkgconf libreadline8 libusb-0.1 libgpgme11 libarchive-tools fakeroot wget
   ```

4. **pspdev v20261001** (pinned, Ubuntu shell):

   ```
   cd ~
   wget https://github.com/pspdev/pspdev/releases/download/v20261001/pspdev-ubuntu-latest-x86_64.tar.gz
   tar -xf pspdev-ubuntu-latest-x86_64.tar.gz
   echo 'export PSPDEV="$HOME/pspdev"' >> ~/.bashrc
   echo 'export PATH="$PATH:$PSPDEV/bin"' >> ~/.bashrc
   exec bash
   psp-config --pspdev-path
   ```

   The last command should print `/home/<user>/pspdev`.

5. **Build** (Ubuntu shell):

   ```
   cd "<project-dir>/sa1-psp"
   cmake -S . -B build-psp -DTARGET=psp -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake
   cmake --build build-psp
   ```

6. Resume the M-1 Claude session. It boots `build-psp\EBOOT.PBP` in PPSSPP,
   saves the screenshot as `docs/m-1-ppsspp.png` and ticks M-1.
