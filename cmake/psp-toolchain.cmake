# PSP cross-compilation toolchain for the WoWee PSP port.
#
# Thin wrapper over pspdev's own toolchain file, so the port follows whatever
# the installed pspdev release sets (compilers, sysroot, -DPSP/-D__PSP__,
# create_pbp_file). Used only by the separate PSP project in psp/; the desktop
# build never reads this file.
#
#   cmake -S psp -B build-psp -DCMAKE_TOOLCHAIN_FILE=cmake/psp-toolchain.cmake
#
# Lives directly in cmake/ (not cmake/toolchains/) because upstream's
# .gitignore only un-ignores cmake/*.cmake.

if(NOT DEFINED ENV{PSPDEV})
    message(FATAL_ERROR "PSPDEV is not set. Export PSPDEV=<pspdev root> (e.g. $HOME/pspdev) before configuring the PSP build.")
endif()

if(NOT EXISTS "$ENV{PSPDEV}/psp/share/pspdev.cmake")
    message(FATAL_ERROR "pspdev toolchain file not found at $ENV{PSPDEV}/psp/share/pspdev.cmake")
endif()

include("$ENV{PSPDEV}/psp/share/pspdev.cmake")

set(WOWEE_PLATFORM "PSP" CACHE STRING "Target platform of this build tree" FORCE)
