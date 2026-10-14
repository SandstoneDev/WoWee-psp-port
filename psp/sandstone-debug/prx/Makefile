# sdbg.prx -- the kernel module that catches a hard exception.
#
# THE BUILD SETUP IS COPIED FROM A WORKING MODULE, not written from memory:
# a kernel module of a sibling PSP port. The same set of flags runs there on
# this same hardware, and repeating it is cheaper than finding out again which
# of them is mandatory.
#
# WHAT EACH LINE DOES, SO THAT NOBODY REMOVES ONE AT RANDOM:
#   BUILD_PRX=1        -- build a .prx, not a standalone EBOOT;
#   PRX_EXPORTS        -- the export list (exports.exp next to this file);
#   USE_KERNEL_LIBC/LIBS -- the kernel variants of the libraries. Without them
#                        the reference to ExceptionManagerForKernel does not
#                        resolve;
#   psp-build-exports --build-stubs -- produces the STUB FOR THE GAME
#                        (sdbg.S). That is what gets compiled into the EBOOT
#                        and lets a user module call a kernel function AFTER
#                        the module has been loaded.
TARGET = sdbg
OBJS   = sdbg_prx.o trap.o exmrel.o   # exmrel.S -- import of sceKernelReleaseExceptionHandler, which the SDK lacks

CFLAGS  = -Os -G0 -Wall -fno-pic
ASFLAGS = $(CFLAGS)

BUILD_PRX       = 1
PRX_EXPORTS     = exports.exp
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

# The same firmware version as the module it was copied from: below it the
# kernel exports differ.
PSP_FW_VERSION = 500

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

EXTRA_TARGETS = sdbg.S

sdbg.S: $(PRX_EXPORTS)
	psp-build-exports --build-stubs $(PRX_EXPORTS)

all: $(FINAL_TARGET) sdbg.S
