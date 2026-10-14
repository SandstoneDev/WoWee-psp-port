#!/bin/bash
# Sets up a fresh Linux box (a cloud session, CI) to build the PSP EBOOT:
#
#   bash tools/psp/cloud_setup.sh          # installs pspdev into $PSPDEV (~/pspdev)
#   bash tools/psp/build_psp.sh --no-bump  # then build (psp/bin/EBOOT.PBP)
#
# pspdev comes from the official release tarball (toolchain + PSPSDK +
# psp-packages). The local WSL toolchain is older; the hardware builds are made
# there, so a cloud build proves "compiles, warning-free, no stubs out of
# order" - it never replaces a build + test on the PSP-3000.
set -euo pipefail

PSPDEV="${PSPDEV:-$HOME/pspdev}"
URL="${PSPDEV_URL:-https://github.com/pspdev/pspdev/releases/download/v20260901/pspdev-ubuntu-latest-x86_64.tar.gz}"

if [ -x "$PSPDEV/bin/psp-g++" ]; then
    echo "pspdev already in $PSPDEV: $("$PSPDEV/bin/psp-gcc" --version | head -1)"
else
    tmp="$(mktemp -d)"
    echo "downloading $URL"
    curl -fL --retry 3 -o "$tmp/pspdev.tar.gz" "$URL"
    mkdir -p "$(dirname "$PSPDEV")"
    tar -xzf "$tmp/pspdev.tar.gz" -C "$tmp"
    # The tarball holds a single "pspdev" directory.
    rm -rf "$PSPDEV"
    mv "$tmp/pspdev" "$PSPDEV"
    rm -rf "$tmp"
    echo "installed: $("$PSPDEV/bin/psp-gcc" --version | head -1)"
fi

if ! command -v cmake >/dev/null; then
    if command -v apt-get >/dev/null; then
        (sudo -n apt-get update && sudo -n apt-get install -y cmake make) || apt-get install -y cmake make
    else
        echo "cmake is missing and there is no apt-get; install cmake >= 3.20" >&2
        exit 1
    fi
fi
echo "ok: export PSPDEV=$PSPDEV; bash tools/psp/build_psp.sh --no-bump"
