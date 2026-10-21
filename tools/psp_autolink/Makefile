# sa1autolink.prx: user-mode XMB plugin for ARK-4 (sa1autolink.c).
# Built by build.sh in a copy under build-autolink/, as backend/psp/exc is.
TARGET = sa1autolink
OBJS = sa1autolink.o stubs.o
BUILD_PRX = 1
USE_PSPSDK_LIBC = 1
CFLAGS = -Os -G0 -Wall -Wextra -std=gnu99 -fno-builtin -fno-tree-loop-distribute-patterns
ASFLAGS = $(CFLAGS)
LDFLAGS = -nostartfiles
LIBS = -lpspusb -lpsppower

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
