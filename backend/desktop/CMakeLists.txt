# Desktop backend.  M-1's hello-world stays as the CI smoke test; the real
# target, sa1psp, needs the recompiled game code in ${SA1_RECOMP_OUT}
# (generated from the user's own dump, never committed), so it is only
# configured when that output exists.  The HLE and the backend are compiled
# into static libraries on every build (CI included) so that they keep
# compiling without game data.
add_executable(sa1psp_hello hello.c)

# SDL2 gives the window, the GL context and the controller (M2).  A system
# package is used when CMake can find one; on Windows the pinned development
# package (zlib licence) is fetched otherwise.  -DSA1_NO_SDL=ON builds the
# headless backend only.
option(SA1_NO_SDL "Build the desktop backend without SDL2 (headless only)" OFF)
set(SA1_HAVE_SDL OFF)
if(NOT SA1_NO_SDL)
  find_package(SDL2 CONFIG QUIET)
  if(NOT SDL2_FOUND AND WIN32)
    include(FetchContent)
    FetchContent_Declare(sdl2_devel
      URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-VC.zip
      URL_HASH SHA256=6e524b5f0e3ecd9fe3b439336b5e7c3fe597e8823e8e62c8e3074d335e9ca7d9)
    FetchContent_MakeAvailable(sdl2_devel)
    set(SDL2_DIR "${sdl2_devel_SOURCE_DIR}/cmake")
    find_package(SDL2 CONFIG QUIET)
  endif()
  if(SDL2_FOUND)
    set(SA1_HAVE_SDL ON)
  else()
    message(STATUS "sa1psp: SDL2 not found, desktop backend is headless only")
  endif()
endif()

# Guest-address check in the flat-RAM macros (sh4_rt.h, SA1_RAM_CHECK): on in
# Debug builds, or forced; a release build compiles the bare mask.
option(SA1_RAM_CHECK "Check every guest access against the RAM window (debug aid; always on in Debug)" OFF)
if(SA1_RAM_CHECK OR CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(SA1_RAM_CHECK=1)
  message(STATUS "sa1psp: guest RAM-window check on")
endif()

# The platform-neutral HLE (hle/) as a library: compiles without game code.
file(GLOB SA1_HLE_SRCS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/hle/*.c"
     "${CMAKE_SOURCE_DIR}/hle/bios/*.c"
     "${CMAKE_SOURCE_DIR}/hle/shinobi/*.c"
     "${CMAKE_SOURCE_DIR}/hle/game/*.c"
     "${CMAKE_SOURCE_DIR}/hle/ninja/*.c"
     "${CMAKE_SOURCE_DIR}/hle/sound/*.c"
     "${CMAKE_SOURCE_DIR}/hle/overlay/*.c")
add_library(sa1_hle OBJECT ${SA1_HLE_SRCS} ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime/sh4_rt.c)
# 0x8C037350 (model lighting, hle/ninja/ta_xform.c) is wrapped only when the
# emitted code renamed its body (recomp/sa1_us_1005/hle_wraps.tsv): a
# recomp/out emitted before that entry keeps the recompiled function.
if(EXISTS "${SA1_RECOMP_OUT}/emitter/game.h")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/emitter/game.h")
  file(STRINGS "${SA1_RECOMP_OUT}/emitter/game.h" _sa1_wrap350 REGEX "fn_8c037350_orig" LIMIT_COUNT 1)
  if(_sa1_wrap350)
    target_compile_definitions(sa1_hle PRIVATE SA1_WRAP_8C037350)
  endif()
endif()
target_include_directories(sa1_hle PUBLIC
  ${CMAKE_SOURCE_DIR}/hle
  ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime
  ${CMAKE_SOURCE_DIR}/backend)
target_compile_definitions(sa1_hle PUBLIC _CRT_SECURE_NO_WARNINGS)
target_compile_options(sa1_hle PRIVATE -fno-strict-aliasing -ffp-contract=off -Wall -g)

# The desktop half of the platform interface plus the GL renderer.
add_library(sa1_backend_desktop OBJECT platform_desktop.c coro_switch.c draw_gl.c audio_sdl.c backtrace_win.c)
target_link_libraries(sa1_backend_desktop PUBLIC sa1_hle)
target_compile_options(sa1_backend_desktop PRIVATE -Wall -g)
if(SA1_HAVE_SDL)
  target_link_libraries(sa1_backend_desktop PUBLIC SDL2::SDL2)
  find_package(OpenGL REQUIRED)
  target_link_libraries(sa1_backend_desktop PUBLIC OpenGL::GL)
else()
  target_compile_definitions(sa1_backend_desktop PRIVATE SA1_NO_SDL)
endif()
if(WIN32)
  target_link_libraries(sa1_backend_desktop PUBLIC dbghelp)
endif()

file(GLOB SA1_GAME_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/emitter/game_[0-9]*.c")
file(GLOB SA1_OVERLAY_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/overlays/emitter/*/ovl_*.c")
if(SA1_GAME_SRCS)
  # Object libraries, not static ones: the weak stubs in hle/seam_stubs.c
  # must see every strong HLE body at link time, as in M1.
  add_executable(sa1psp
    main.c input_tape.c savestate.c sha256.c
    $<TARGET_OBJECTS:sa1_hle> $<TARGET_OBJECTS:sa1_backend_desktop>
    ${SA1_GAME_SRCS}
    ${SA1_RECOMP_OUT}/emitter/game_table.c
    ${SA1_OVERLAY_SRCS})
  # The runtime header first: the emitter leaves a copy of sh4_rt.h next to
  # its output, which must not shadow the one in the tree.
  target_include_directories(sa1psp BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime)
  # A quoted #include searches the including file's folder first, so the copy
  # won anyway (as on the PSP until 2e0f494): the tree's header is included
  # first and its guard turns the copy's #include into nothing.
  target_compile_options(sa1psp BEFORE PRIVATE "SHELL:-include \"${CMAKE_SOURCE_DIR}/recomp/emitter/runtime/sh4_rt.h\"")
  target_include_directories(sa1psp PRIVATE ${SA1_RECOMP_OUT}/emitter)
  target_link_libraries(sa1psp PRIVATE sa1_backend_desktop sa1_hle)
  target_include_directories(sa1psp PRIVATE ${CMAKE_SOURCE_DIR}/hle ${CMAKE_SOURCE_DIR}/backend)
  # The generated code needs these (recomp/emitter/README.md, "Requirements").
  set(SA1_GAME_OPT "-O1" CACHE STRING "Optimisation level for the recompiled game code")
  # Symbols for the trap backtrace (recompiled functions are named by guest address).
  target_compile_options(sa1psp PRIVATE -g)
  target_link_options(sa1psp PRIVATE -g)
  # No ASLR: a save state holds native stacks, which only resume if this
  # build's code and statics load at the same addresses every run
  # (docs/savestate.md section 1).
  if(WIN32)
    target_link_options(sa1psp PRIVATE -Xlinker /DYNAMICBASE:NO)
  else()
    target_link_options(sa1psp PRIVATE -no-pie)
  endif()
  target_compile_options(sa1psp PRIVATE
    -fno-strict-aliasing -ffp-contract=off -Wall
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses)
  # fmac fused like the console oracle (sh4_rt.h FMAC_F)
  target_compile_definitions(sa1psp PRIVATE SH4_FMAC_FUSED=1)
  option(SA1_TRACE_FUNCS "Instrument the recompiled code for --trace-funcs (debug)" OFF)
  set(SA1_GAME_FLAGS "${SA1_GAME_OPT}")
  if(SA1_TRACE_FUNCS)
    list(APPEND SA1_GAME_FLAGS -finstrument-functions)
  endif()
  set_source_files_properties(${SA1_GAME_SRCS} ${SA1_OVERLAY_SRCS} PROPERTIES COMPILE_OPTIONS "${SA1_GAME_FLAGS}")
  if(SA1_HAVE_SDL AND WIN32)
    add_custom_command(TARGET sa1psp POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:sa1psp>)
  endif()
  list(LENGTH SA1_GAME_SRCS n_game)
  list(LENGTH SA1_OVERLAY_SRCS n_ovl)
  message(STATUS "sa1psp: ${n_game} game chunks, ${n_ovl} overlay chunks from ${SA1_RECOMP_OUT}")
else()
  message(STATUS "sa1psp: no recompiled code in ${SA1_RECOMP_OUT}/emitter, target skipped")
endif()
