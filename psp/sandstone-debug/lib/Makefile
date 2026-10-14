# libsdbg -- the user half of SandstoneDebug.
#
# IT IS BUILT SEPARATELY FROM THE GAME AND WITHOUT THE GAME'S HEADERS. That is
# the portability check: as soon as something from the game is needed here,
# the boundary is drawn in the wrong place and part of the code must go back
# into the game.
#
# Using it from a project:
#   CFLAGS  += -I<path>/sandstone-debug/lib
#   LDFLAGS += -L<path>/sandstone-debug/lib -lsdbg
# and put sdbg.prx next to the EBOOT (the path is passed in sdbg_cfg.prx_path).

TARGET_LIB = libsdbg.a
OBJS       = sdbg.o sdbg_psplink.o

CFLAGS  = -O2 -G0 -Wall -Werror=implicit-function-declaration
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
