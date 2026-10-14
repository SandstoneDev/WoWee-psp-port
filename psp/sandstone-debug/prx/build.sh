#!/bin/sh
# Builds the kernel catcher from WSL:
#   wsl bash /mnt/<drive>/<path-to>/sandstone-debug/prx/build.sh
# A separate file rather than a command line for wsl.exe: wsl.exe passes its
# arguments through a shell, and any $PATH inside is expanded BEFORE bash -c
# (with the parentheses of Windows paths -- a syntax error out of nowhere).
set -e
cd "$(dirname "$0")"
export PATH="$HOME/pspdev/bin:$PATH"
make 2>&1 | tail -6
# THE FILE NAME HERE USED TO LAG BEHIND THE Makefile OUTPUT: the script printed
# "psrccrash.prx: No such file" and returned an ERROR on a successful build --
# that is, it lied in exactly the direction in which lying is not allowed.
# There is one name, and it comes from the Makefile.
ls -la sdbg.prx
