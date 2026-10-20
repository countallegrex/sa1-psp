# sa1prof.prx: the kernel-mode PC sampler (sa1prof.c).  Built by
# backend/psp/CMakeLists.txt in a copy under the build tree.
TARGET = sa1prof
OBJS = sa1prof.o
BUILD_PRX = 1
USE_KERNEL_LIBS = 1
CFLAGS = -O2 -Wall -G0
LIBS = -lpspthreadman_kernel_660

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
