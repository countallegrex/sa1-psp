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
# 0x8C037350 (model lighting, hle/ninja/ta_xform.c) is wrapped only when the
# emitted code renamed its body (recomp/sa1_us_1005/hle_wraps.tsv): a
# recomp/out emitted before that entry keeps the recompiled function.
if(EXISTS "${SA1_RECOMP_OUT}/emitter/game.h")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/emitter/game.h")
  file(STRINGS "${SA1_RECOMP_OUT}/emitter/game.h" _sa1_wrap350 REGEX "fn_8c037350_orig" LIMIT_COUNT 1)
  if(_sa1_wrap350)
    target_compile_definitions(sa1_hle_psp PRIVATE SA1_WRAP_8C037350)
  endif()
  # 0x8C0376D0 (the mesh-set loop, hle/ninja/ta_mesh.c) the same way.
  file(STRINGS "${SA1_RECOMP_OUT}/emitter/game.h" _sa1_wrap6d0 REGEX "fn_8c0376d0_orig" LIMIT_COUNT 1)
  if(_sa1_wrap6d0)
    target_compile_definitions(sa1_hle_psp PRIVATE SA1_WRAP_8C0376D0)
  endif()
endif()
target_include_directories(sa1_hle_psp PUBLIC
  ${CMAKE_SOURCE_DIR}/hle
  ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime
  ${CMAKE_SOURCE_DIR}/backend)
target_compile_options(sa1_hle_psp PRIVATE -O2 -fno-strict-aliasing -ffp-contract=off -Wall -g)

# The PSP half of the platform interface, the sceGu renderer, and the tape
# code shared with the desktop (plain stdio).
add_library(sa1_backend_psp OBJECT
  platform_psp.c coro_psp.c draw_gu.c crash_psp.c main_psp.c savestate_psp.c
  ${CMAKE_SOURCE_DIR}/backend/desktop/input_tape.c
  ${CMAKE_SOURCE_DIR}/backend/desktop/sha256.c)
target_link_libraries(sa1_backend_psp PUBLIC sa1_hle_psp)
target_compile_options(sa1_backend_psp PRIVATE -O2 -fno-strict-aliasing -Wall -g)

# Hot-code placement.  The PSP-3001's I-cache is 16 KB, 2-way, 64-byte
# lines: where a function lands decides which functions it evicts, and code
# added anywhere moved Station Square by 1-5 ms a frame (2026-10-21: one
# 9.5 KB function slowed strip_emit, draw_strip, sceGuShadeModel ... with
# the same work).  ON compiles every function into its own section and
# hands the linker tools/psp_hot_order.txt (tools/psp_hot_order.py, from a
# console profile) as a --section-ordering-file: the listed functions go
# first in .text, in that order (callers next to their callees), at the
# image's start, 64-byte aligned (the PRX at its 16 KB aligned load
# address, the EBOOT one line later at 0x08804040: the same conflicts),
# and their cache sets no longer depend on the rest of the code.  pspsdk
# and newlib members are placed whole (one function per member).  An entry
# the build no longer has matches nothing; the link never fails on it.
# Regenerate the list after a new profile (tools/psp_hot_order.py --help).
# The D-cache is 16 KB, 2-way, 64-byte lines too, and .rodata, .data,
# .sdata, .sbss and .bss follow .text: 380 bytes of cold code moved every
# hot table and pool by 380 bytes and Station Square by 2.8 ms (cea4798).
# SA1_PSP_DATA_ALIGN starts each of them on an 8 KB boundary (through the
# same ordering file, so the stock PRX and EBOOT scripts stay as they are):
# their cache sets then depend only on the data before them in the same
# section (the game chunks' and the libraries' data first, strings last,
# below).  Cost: under 8 KB of padding each (in the file for .rodata,
# .data and .sdata).
option(SA1_PSP_HOT_ORDER "Place the profile's hot functions first in .text (tools/psp_hot_order.txt)" ON)
option(SA1_PSP_DATA_ALIGN "Start .rodata, .data, .sdata, .sbss and .bss on 8 KB boundaries" ON)
set(SA1_PSP_HOT_LIST "${CMAKE_SOURCE_DIR}/tools/psp_hot_order.txt")
set(SA1_PSP_HOT_LD "")
set(order_ld "")
if(SA1_PSP_HOT_ORDER AND EXISTS "${SA1_PSP_HOT_LIST}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SA1_PSP_HOT_LIST}")
  file(STRINGS "${SA1_PSP_HOT_LIST}" hot_lines)
  set(hot_body "")
  foreach(l IN LISTS hot_lines)
    string(REGEX REPLACE "#.*" "" l "${l}")
    string(STRIP "${l}" l)
    if(l STREQUAL "")
      continue()
    elseif(l MATCHES "^([^:]+\\.a):([^:]+)$")      # libX.a:member.o, whole
      string(APPEND hot_body "    *${CMAKE_MATCH_1}:${CMAKE_MATCH_2}(.text .text.*)\n")
    elseif(l MATCHES "^([^:]+):([^:]+)$")          # file.c:static_fn
      string(APPEND hot_body "    */${CMAKE_MATCH_1}.o*(.text.${CMAKE_MATCH_2})\n")
    else()
      string(APPEND hot_body "    *(.text.${l})\n")
    endif()
  endforeach()
  # After the hot list, the rest of the libraries (the other sceGu* calls
  # draw_end_frame makes per item) and of the game chunks: code that never
  # changes with ours goes before the HLE's and the backend's, so an edit
  # there moves only the HLE and backend code after it.
  string(APPEND order_ld ".text : {\n    . = ALIGN(64);\n${hot_body}    . = ALIGN(64);\n"
    "    *.a:*(.text .text.*)\n    *sa1_game_psp.dir/*(.text .text.*)\n}\n")
  target_compile_options(sa1_hle_psp PRIVATE -ffunction-sections)
  target_compile_options(sa1_backend_psp PRIVATE -ffunction-sections)
elseif(SA1_PSP_HOT_ORDER)
  message(WARNING "SA1_PSP_HOT_ORDER: no ${SA1_PSP_HOT_LIST}, default code placement")
endif()
if(SA1_PSP_DATA_ALIGN)
  # The ordering file's statements go before the stock script's own input
  # sections: the padding first, then the game chunks' data (sa1_fn_table,
  # the overlay tables: they change only with the recompiler's output),
  # then the libraries' (pspgu's context), then the HLE's and the
  # backend's.  Strings and literal pools (.rodata.str*, .rodata.cst*) are
  # merged where their first input lands, so they stay out of these lists
  # and go last (the stock script's pattern): a log string added anywhere
  # moves nothing.
  set(ro ".rodata .rodata.[!sc]* .rodata.s[!t]* .rodata.st[!r]* .rodata.c[!s]* .rodata.cs[!t]*")
  foreach(sec ".rodata|${ro}" ".data|.data .data.*" ".sdata|.sdata .sdata.*"
              ".sbss|.sbss .sbss.* .scommon" ".bss|.bss .bss.* COMMON")
    string(REGEX REPLACE "\\|.*" "" out "${sec}")
    string(REGEX REPLACE ".*\\|" "" in "${sec}")
    string(APPEND order_ld "${out} : {\n    . = ALIGN(0x2000);\n"
      "    *sa1_game_psp.dir/*(${in})\n    *.a:*(${in})\n    *(${in})\n}\n")
  endforeach()
endif()
if(order_ld)
  # Written only when it changes (configure_file), so a re-configure does
  # not relink.
  file(WRITE "${CMAKE_BINARY_DIR}/sa1_hot_order.ld.in"
    "/* Generated by backend/psp/CMakeLists.txt (tools/psp_hot_order.txt, SA1_PSP_DATA_ALIGN). */\n${order_ld}")
  configure_file("${CMAKE_BINARY_DIR}/sa1_hot_order.ld.in" "${CMAKE_BINARY_DIR}/sa1_hot_order.ld" COPYONLY)
  set(SA1_PSP_HOT_LD "${CMAKE_BINARY_DIR}/sa1_hot_order.ld")
endif()

# sa1exc.prx: the kernel-mode crash screen the EBOOT loads through kubridge
# (exc/sa1exc.c).  Kernel modules link without libc, so it is built by
# pspsdk's own makefile, in a copy under the build tree.
set(SA1_EXC_DIR ${CMAKE_BINARY_DIR}/sa1exc)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/sa1psp/sa1exc.prx
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/exc ${SA1_EXC_DIR}
  COMMAND make -C ${SA1_EXC_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sa1psp
  COMMAND ${CMAKE_COMMAND} -E copy ${SA1_EXC_DIR}/sa1exc.prx ${CMAKE_BINARY_DIR}/sa1psp/sa1exc.prx
  DEPENDS exc/sa1exc.c exc/Makefile
  COMMENT "Building sa1exc.prx (kernel-mode crash screen)")
add_custom_target(sa1exc ALL DEPENDS ${CMAKE_BINARY_DIR}/sa1psp/sa1exc.prx)

# sa1prof.prx: the kernel-mode PC sampler for --prof (prof/sa1prof.c), built
# the same way.
set(SA1_PROF_DIR ${CMAKE_BINARY_DIR}/sa1prof)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/sa1psp/sa1prof.prx
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/prof ${SA1_PROF_DIR}
  COMMAND make -C ${SA1_PROF_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sa1psp
  COMMAND ${CMAKE_COMMAND} -E copy ${SA1_PROF_DIR}/sa1prof.prx ${CMAKE_BINARY_DIR}/sa1psp/sa1prof.prx
  DEPENDS prof/sa1prof.c prof/Makefile
  COMMENT "Building sa1prof.prx (kernel-mode PC sampler)")
add_custom_target(sa1prof ALL DEPENDS ${CMAKE_BINARY_DIR}/sa1psp/sa1prof.prx)

file(GLOB SA1_GAME_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/emitter/game_[0-9]*.c")
file(GLOB SA1_OVERLAY_SRCS CONFIGURE_DEPENDS "${SA1_RECOMP_OUT}/overlays/emitter/*/ovl_*.c")
if(SA1_GAME_SRCS)
  # The recompiled code, compiled once for both images below.
  # -Os for the game code: README section 7 budgets its size (3.66 MB at M1).
  set(SA1_PSP_GAME_OPT "-Os" CACHE STRING "Optimisation level for the recompiled game code on the PSP")
  add_library(sa1_game_psp OBJECT ${SA1_GAME_SRCS} ${SA1_RECOMP_OUT}/emitter/game_table.c ${SA1_OVERLAY_SRCS})
  # The chunks that hold the console profile's hottest game functions
  # (tools/psp_prof.py --pcs) at -O2: there the guest-address helper sa1_off
  # is inlined, where -Os calls it out of line at every access (12 % of the
  # frame in Station Square, 2026-10-19).  game_005 holds the game's TA
  # writers (fn_8c037294, fn_8c037990, ...), game_003 the next ones.
  set(SA1_PSP_HOT_CHUNKS "game_001;game_002;game_003;game_004;game_005;game_007;game_008;game_015;game_030" CACHE STRING "Game chunks compiled at -O2 on the PSP")
  foreach(chunk ${SA1_PSP_HOT_CHUNKS})
    set_source_files_properties(${SA1_RECOMP_OUT}/emitter/${chunk}.c PROPERTIES COMPILE_OPTIONS "-O2")
  endforeach()
  # The runtime header first: the emitter leaves a copy of sh4_rt.h next to
  # its output, which must not shadow the one in the tree.
  target_include_directories(sa1_game_psp BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/recomp/emitter/runtime)
  # A quoted #include searches the including file's folder before any -I, so
  # the copy next to the generated sources won anyway (the game code built
  # against a stale runtime until 2026-10-21): the tree's header is included
  # first, and its guard turns the copy's #include into nothing.
  target_compile_options(sa1_game_psp BEFORE PRIVATE "SHELL:-include \"${CMAKE_SOURCE_DIR}/recomp/emitter/runtime/sh4_rt.h\"")
  # sh4_rt.h sa1_off: the game code tests the area only (-6.8 ms a frame in
  # Station Square); ON adds the RAM + EXTRA bound the HLE has.
  option(SA1_PSP_GAME_FULL_OFF "Game code checks every guest offset against RAM + EXTRA (sh4_rt.h sa1_off)" OFF)
  if(NOT SA1_PSP_GAME_FULL_OFF)
    target_compile_definitions(sa1_game_psp PRIVATE SA1_OFF_AREA_TEST_ONLY)
  endif()
  target_include_directories(sa1_game_psp PRIVATE ${SA1_RECOMP_OUT}/emitter
    ${CMAKE_SOURCE_DIR}/hle ${CMAKE_SOURCE_DIR}/backend)
  target_compile_options(sa1_game_psp PRIVATE ${SA1_PSP_GAME_OPT} -fno-strict-aliasing -ffp-contract=off -Wall
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses)
  if(SA1_PSP_HOT_ORDER AND EXISTS "${SA1_PSP_HOT_LIST}")
    # Hot game functions (and their chunk's out-of-line sa1_off) are listed
    # too; per-function sections cost nothing in the image (no padding: the
    # sections keep the functions' own 4-byte alignment).
    target_compile_options(sa1_game_psp PRIVATE -ffunction-sections)
  endif()
  # Object libraries, not static ones: the weak stubs in hle/seam_stubs.c
  # must see every strong HLE body at link time (as on the desktop).
  set(SA1_PSP_OBJS $<TARGET_OBJECTS:sa1_hle_psp> $<TARGET_OBJECTS:sa1_backend_psp> $<TARGET_OBJECTS:sa1_game_psp>)
  set(SA1_PSP_LIBS pspgu pspge pspdisplay pspctrl psppower psprtc pspdebug pspkubridge)
  if(SA1_PSP_HOT_LD)
    set(SA1_PSP_LINK_ORDER "-Wl,--section-ordering-file=${SA1_PSP_HOT_LD}")
  else()
    set(SA1_PSP_LINK_ORDER "")
  endif()

  # 1. The EBOOT the XMB runs: a static ELF at 0x08804000.
  add_executable(sa1psp ${SA1_PSP_OBJS})
  target_link_libraries(sa1psp PRIVATE ${SA1_PSP_LIBS})
  target_link_options(sa1psp PRIVATE ${SA1_PSP_LINK_ORDER})
  add_dependencies(sa1psp sa1exc)
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

  # 2. The same program as a relocatable PRX, for PSPLink (tools/psp_run.py):
  # PSPLink's ldstart refuses a static ELF (0x80020148 on ARK-4).  It loads
  # wherever the kernel puts it, so its addresses differ from sa1psp.elf's;
  # sa1psp_prx.elf keeps its symbols at link addresses (tools/psp_run.py
  # subtracts the load base before naming a crash address).
  add_executable(sa1psp_prx ${SA1_PSP_OBJS})
  target_link_libraries(sa1psp_prx PRIVATE ${SA1_PSP_LIBS})
  target_link_options(sa1psp_prx PRIVATE
    -specs=$ENV{PSPDEV}/psp/sdk/lib/prxspecs -Wl,-q,-T$ENV{PSPDEV}/psp/sdk/lib/linkfile.prx
    $ENV{PSPDEV}/psp/sdk/lib/prxexports.o ${SA1_PSP_LINK_ORDER})
  if(SA1_PSP_HOT_LD)
    set_target_properties(sa1psp sa1psp_prx PROPERTIES LINK_DEPENDS "${SA1_PSP_HOT_LD}")
  endif()
  add_custom_command(TARGET sa1psp_prx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/sa1psp
    COMMAND $ENV{PSPDEV}/bin/psp-fixup-imports $<TARGET_FILE:sa1psp_prx>
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sa1psp_prx> ${CMAKE_BINARY_DIR}/sa1psp/sa1psp_prx.elf
    COMMAND $ENV{PSPDEV}/bin/psp-prxgen $<TARGET_FILE:sa1psp_prx> ${CMAKE_BINARY_DIR}/sa1psp/sa1psp.prx
    COMMENT "Building sa1psp.prx for PSPLink")
  add_dependencies(sa1psp_prx sa1exc)

  list(LENGTH SA1_GAME_SRCS n_game)
  list(LENGTH SA1_OVERLAY_SRCS n_ovl)
  message(STATUS "sa1psp (PSP): ${n_game} game chunks, ${n_ovl} overlay chunks from ${SA1_RECOMP_OUT}")
else()
  message(STATUS "sa1psp (PSP): no recompiled code in ${SA1_RECOMP_OUT}/emitter, game EBOOT skipped")
endif()
