# PSP backend.  Configure with
#   -DTARGET=psp -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake
#
# M-1's hello-world stays as the CI smoke test (build-psp/EBOOT.PBP).  The
# HLE and the backend compile into object libraries on every build, CI
# included, so they keep compiling for the PSP without game data; the game
# itself, build-psp/sa1psp/EBOOT.PBP, needs the recompiled code in
# ${SA1_RECOMP_OUT} (generated from the user's own dump, never committed).
add_executable(sa1psp_hello hello.c)
target_link_libraries(sa1psp_hello PRIVATE pspdebug pspdisplay pspge)

# MEMSIZE=1 in PARAM.SFO unlocks the PSP-2000+ extra RAM; pspsdk defaults to 2.
create_pbp_file(
  TARGET sa1psp_hello
  TITLE "SA1 PSP"
  VERSION 00.01
  MEMSIZE 1
  OUTPUT_DIR ${CMAKE_BINARY_DIR}
)

# GCC for MIPS predefines `mips` (gnu11), which the HLE uses as a name, and
# newlib's uint32_t is unsigned long here, so every %u/%x of one warns.
add_compile_options(-Umips -Wno-format)

# The platform-neutral HLE, as on the desktop (backend/desktop/CMakeLists.txt).
file(GLOB SA1_HLE_SRCS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/hle/*.c"
     "${CMAKE_SOURCE_DIR}/hle/bios/*.c"
     "${CMAKE_SOURCE_DIR}/hle/shinobi/*.c"
     "${CMAKE_SOURCE_DIR}/hle/game/*.c"
     "${CMAKE_SOURCE_DIR}/hle/ninja/*.c"
     "${CMAKE_SOURCE_DIR}/hle/sound/*.c"
     "${CMAKE_SOURCE_DIR}/hle/overlay/*.c")
add_library(sa1_hle_psp OBJECT ${SA1_HLE_SRCS} ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime/sh4_rt.c)
target_include_directories(sa1_hle_psp PUBLIC
  ${CMAKE_SOURCE_DIR}/hle
  ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime
  ${CMAKE_SOURCE_DIR}/backend)
target_compile_options(sa1_hle_psp PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wall -g)

# The PSP half of the platform interface, the sceGu renderer, and the tape
# code shared with the desktop (plain stdio).
add_library(sa1_backend_psp OBJECT
  platform_psp.c coro_psp.c draw_gu.c crash_psp.c main_psp.c
  ${CMAKE_SOURCE_DIR}/backend/desktop/input_tape.c
  ${CMAKE_SOURCE_DIR}/backend/desktop/sha256.c)
target_link_libraries(sa1_backend_psp PUBLIC sa1_hle_psp)
target_compile_options(sa1_backend_psp PRIVATE -O2 -fno-strict-aliasing -Wall -g)

file(GLOB SA1_GAME_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/emitter/game_[0-9]*.c")
file(GLOB SA1_OVERLAY_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/overlays/emitter/*/ovl_*.c")
if(SA1_GAME_SRCS)
  # Object libraries, not static ones: the weak stubs in hle/seam_stubs.c
  # must see every strong HLE body at link time (as on the desktop).
  add_executable(sa1psp
    $<TARGET_OBJECTS:sa1_hle_psp> $<TARGET_OBJECTS:sa1_backend_psp>
    ${SA1_GAME_SRCS}
    ${SA1_RECOMP_OUT}/emitter/game_table.c
    ${SA1_OVERLAY_SRCS})
  # The runtime header first: the emitter leaves a copy of sh4_rt.h next to
  # its output, which must not shadow the one in the tree.
  target_include_directories(sa1psp BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime)
  target_include_directories(sa1psp PRIVATE ${SA1_RECOMP_OUT}/emitter
    ${CMAKE_SOURCE_DIR}/hle ${CMAKE_SOURCE_DIR}/backend)
  target_link_libraries(sa1psp PRIVATE pspgu pspge pspdisplay pspctrl psppower psprtc pspdebug)
  # -Os for the game code: README section 7 budgets its size (3.66 MB at M1).
  set(SA1_PSP_GAME_OPT "-Os" CACHE STRING "Optimisation level for the recompiled game code on the PSP")
  target_compile_options(sa1psp PRIVATE -fno-strict-aliasing -ffp-contract=off -Wall
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses)
  set_source_files_properties(${SA1_GAME_SRCS} ${SA1_OVERLAY_SRCS} ${SA1_RECOMP_OUT}/emitter/game_table.c
    PROPERTIES COMPILE_OPTIONS "${SA1_PSP_GAME_OPT}")
  # Keep the linked ELF with its symbols for tools/psp_addr2line.sh, and put
  # a stripped copy in the EBOOT (these run before create_pbp_file's own
  # post-build steps, which pack the target file).
  add_custom_command(TARGET sa1psp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sa1psp
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sa1psp> ${CMAKE_BINARY_DIR}/sa1psp/sa1psp.elf
    COMMAND $ENV{PSPDEV}/bin/psp-strip $<TARGET_FILE:sa1psp>
    COMMENT "Keeping sa1psp.elf with symbols, stripping the EBOOT's copy")
  create_pbp_file(
    TARGET sa1psp
    TITLE "SA1 PSP - Sonic Adventure"
    VERSION 00.04
    MEMSIZE 1
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/sa1psp
  )
  list(LENGTH SA1_GAME_SRCS n_game)
  list(LENGTH SA1_OVERLAY_SRCS n_ovl)
  message(STATUS "sa1psp (PSP): ${n_game} game chunks, ${n_ovl} overlay chunks from ${SA1_RECOMP_OUT}")
else()
  message(STATUS "sa1psp (PSP): no recompiled code in ${SA1_RECOMP_OUT}/emitter, game EBOOT skipped")
endif()
