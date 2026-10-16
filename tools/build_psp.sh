#!/bin/sh
# Configure and build the PSP target. Used by the Dockerfile and by CI.
# Run from the repo root inside a pspdev environment (PSPDEV set).
# Usage: sh tools/build_psp.sh [build-dir]   (default: build-ci-psp)
set -eu

BUILD_DIR="${1:-build-ci-psp}"
: "${PSPDEV:?PSPDEV is not set; run inside the pspdev image or install pspsdk}"

cmake -S . -B "$BUILD_DIR" -DTARGET=psp \
  -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake"
cmake --build "$BUILD_DIR"
test -f "$BUILD_DIR/EBOOT.PBP"
echo "built $BUILD_DIR/EBOOT.PBP"
