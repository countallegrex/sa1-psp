# sa1exc.prx: the kernel-mode crash screen (sa1exc.c).  Built by
# backend/psp/CMakeLists.txt with: make -C backend/psp/exc OUT=<dir>
TARGET = sa1exc
OBJS = sa1exc.o
BUILD_PRX = 1
USE_KERNEL_LIBS = 1
CFLAGS = -O2 -Wall -G0
LIBS = -lpspdebug -lpspge_driver -lgcc

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
