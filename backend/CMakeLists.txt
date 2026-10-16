# Filled in at M-1: hello-world for desktop and psp, then the real backends.
if("${TARGET}" STREQUAL "psp")
  add_subdirectory(psp)
else()
  add_subdirectory(desktop)
endif()
