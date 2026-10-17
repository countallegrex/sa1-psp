# Desktop backend.  M-1's hello-world stays as the CI smoke test; the real
# target, sa1psp, needs the recompiled game code in ${SA1_RECOMP_OUT}
# (generated from the user's own dump, never committed), so it is only
# configured when that output exists.
add_executable(sa1psp_hello hello.c)

file(GLOB SA1_GAME_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/emitter/game_[0-9]*.c")
file(GLOB SA1_OVERLAY_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/overlays/emitter/*/ovl_*.c")
if(SA1_GAME_SRCS)
  file(GLOB SA1_HLE_SRCS CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/hle/*.c"
       "${CMAKE_SOURCE_DIR}/hle/bios/*.c"
       "${CMAKE_SOURCE_DIR}/hle/shinobi/*.c"
       "${CMAKE_SOURCE_DIR}/hle/game/*.c"
       "${CMAKE_SOURCE_DIR}/hle/ninja/*.c"
       "${CMAKE_SOURCE_DIR}/hle/sound/*.c"
       "${CMAKE_SOURCE_DIR}/hle/overlay/*.c")
  add_executable(sa1psp
    main.c platform_desktop.c backtrace_win.c
    ${SA1_HLE_SRCS}
    ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime/sh4_rt.c
    ${SA1_GAME_SRCS}
    ${SA1_RECOMP_OUT}/emitter/game_table.c
    ${SA1_OVERLAY_SRCS})
  target_include_directories(sa1psp PRIVATE
    ${CMAKE_SOURCE_DIR}/hle
    ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime
    ${SA1_RECOMP_OUT}/emitter)
  # The generated code needs these (recomp/emitter/README.md, "Requirements").
  set(SA1_GAME_OPT "-O1" CACHE STRING "Optimisation level for the recompiled game code")
  if(WIN32)
    target_link_libraries(sa1psp PRIVATE dbghelp)
  endif()
  # Symbols for the trap backtrace (recompiled functions are named by guest address).
  target_compile_options(sa1psp PRIVATE -g)
  target_link_options(sa1psp PRIVATE -g)
  target_compile_definitions(sa1psp PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(sa1psp PRIVATE
    -fno-strict-aliasing -ffp-contract=off -Wall
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses)
  option(SA1_TRACE_FUNCS "Instrument the recompiled code for --trace-funcs (debug)" OFF)
  set(SA1_GAME_FLAGS "${SA1_GAME_OPT}")
  if(SA1_TRACE_FUNCS)
    list(APPEND SA1_GAME_FLAGS -finstrument-functions)
  endif()
  set_source_files_properties(${SA1_GAME_SRCS} ${SA1_OVERLAY_SRCS} PROPERTIES COMPILE_OPTIONS "${SA1_GAME_FLAGS}")
  list(LENGTH SA1_GAME_SRCS n_game)
  list(LENGTH SA1_OVERLAY_SRCS n_ovl)
  message(STATUS "sa1psp: ${n_game} game chunks, ${n_ovl} overlay chunks from ${SA1_RECOMP_OUT}")
else()
  message(STATUS "sa1psp: no recompiled code in ${SA1_RECOMP_OUT}/emitter, target skipped")
endif()
