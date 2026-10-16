# M-1: PSP hello-world. Configure with
#   -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake
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
