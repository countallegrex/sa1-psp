cmake_minimum_required(VERSION 3.25)
project(sa1psp LANGUAGES C CXX)

# -DTARGET=desktop (default) | psp
set(TARGET "desktop" CACHE STRING "Build target: desktop or psp")
set_property(CACHE TARGET PROPERTY STRINGS desktop psp)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated game code lives outside version control.
set(SA1_RECOMP_OUT "${CMAKE_SOURCE_DIR}/recomp/out" CACHE PATH
    "Directory holding recompiled game code (generated, never committed)")

if(TARGET STREQUAL "psp")
  if(NOT DEFINED ENV{PSPDEV})
    message(FATAL_ERROR "PSPDEV is not set; install pspsdk (M-1).")
  endif()
endif()

# Milestone M-1 exit test: a hello-world for each backend.
# Real targets are added as hle/, backend/ and recomp/ fill in.
add_subdirectory(backend)
