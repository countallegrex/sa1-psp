# sa1me.prx: the Media Engine loader (sa1me.c).  Built by
# backend/psp/CMakeLists.txt in a copy under the build tree.
TARGET = sa1me
OBJS = sa1me.o
BUILD_PRX = 1
USE_KERNEL_LIBS = 1
CFLAGS = -O2 -Wall -G0
LIBS = -lpspthreadman_kernel_660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
