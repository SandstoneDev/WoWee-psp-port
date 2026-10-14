#!/bin/bash
# Reproducible PSP build for the WoWee PSP port.
#
#   bash tools/psp/build_psp.sh            # bump psp/BUILD_NUMBER, clean build
#   bash tools/psp/build_psp.sh --no-bump  # rebuild the same build number
#
# Runs from Git Bash (re-executes itself inside WSL) or directly inside Linux.
# Why it looks the way it does (each rule cost a day on the sibling ports):
#  - builds in a COPY under $HOME, never on /mnt/e: cross-volume mtime skew
#    makes make report "nothing to do" over a stale binary;
#  - always a clean build; proof of compilation = the "Building CXX" lines;
#  - "stubs out of order" from psp-fixup-imports is a build FAILURE: the second
#    stub chunk stays `jr ra` and those imports are dead on hardware;
#  - artifacts land in psp/bin/ (ignored by upstream's .gitignore "bin/").
set -euo pipefail

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # /<drive>/foo -> /mnt/<drive>/foo
        wsl_path="$(echo "$SCRIPT_PATH" | sed -E 's#^/([a-zA-Z])/#/mnt/\L\1/#')"
        exec env MSYS_NO_PATHCONV=1 wsl.exe -d "${WOWEE_WSL_DISTRO:-Debian}" -- bash "$wsl_path" "$@"
        ;;
esac

BUMP=1
for arg in "$@"; do
    case "$arg" in
        --no-bump) BUMP=0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PSPDEV/bin:/usr/local/bin:/usr/bin:/bin"

REPO="$(cd "$(dirname "$SCRIPT_PATH")/../.." && pwd)"
WORK="${WOWEE_PSP_WORK:-$HOME/wowee_psp_build}"
OUT="$REPO/psp/bin"

command -v psp-g++ >/dev/null || { echo "psp-g++ not found; PSPDEV=$PSPDEV" >&2; exit 1; }
command -v cmake   >/dev/null || { echo "cmake not found" >&2; exit 1; }
[ -f "$PSPDEV/psp/share/pspdev.cmake" ] || { echo "pspdev.cmake missing under $PSPDEV" >&2; exit 1; }

BUILD_FILE="$REPO/psp/BUILD_NUMBER"
BUILD_NUM="$(tr -dc '0-9' < "$BUILD_FILE" 2>/dev/null || echo 0)"
[ -n "$BUILD_NUM" ] || BUILD_NUM=0
if [ "$BUILD_NUM" -eq 0 ] || [ "$BUMP" -eq 1 ]; then
    BUILD_NUM=$((BUILD_NUM + 1))
    printf '%s\n' "$BUILD_NUM" > "$BUILD_FILE"
fi

echo "== wowee_psp build $BUILD_NUM"
echo "   repo : $REPO"
echo "   work : $WORK"
echo "   gcc  : $(psp-gcc --version | head -1)"

# Fresh copy of exactly what the PSP project reads.
rm -rf "$WORK"
mkdir -p "$WORK/src"
tar -C "$REPO" \
    --exclude='./psp/bin' \
    -cf - ./psp ./cmake ./src ./include ./Data ./extern/lua-5.1.5/src \
    | tar -C "$WORK/src" -xf -

cmake -S "$WORK/src/psp" -B "$WORK/build" \
    -DCMAKE_TOOLCHAIN_FILE="$WORK/src/cmake/psp-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DWOWEE_PSP_BUILD="$BUILD_NUM" \
    > "$WORK/configure.log" 2>&1 || { cat "$WORK/configure.log"; exit 1; }

set +e
# WOWEE_PSP_JOBS: fewer compile jobs when several builds share the machine (default: all cores).
cmake --build "$WORK/build" --parallel "${WOWEE_PSP_JOBS:-$(nproc)}" 2>&1 | tee "$WORK/build.log"
rc=${PIPESTATUS[0]}
set -e
[ "$rc" -eq 0 ] || { echo "BUILD FAILED (rc=$rc)" >&2; exit "$rc"; }

compiled=$(grep -c 'Building CXX object\|Building C object' "$WORK/build.log" || true)
echo "   compiled translation units: $compiled"
[ "$compiled" -gt 0 ] || { echo "no compile lines - refusing a stale binary" >&2; exit 1; }

if grep -qiE 'stubs out of order|could not fixup imports' "$WORK/build.log"; then
    echo "psp-fixup-imports reported split stubs - imports would be dead on hardware" >&2
    grep -iE 'stubs out of order|could not fixup imports' "$WORK/build.log" >&2
    exit 1
fi

mkdir -p "$OUT"
cp "$WORK/build/EBOOT.PBP" "$OUT/EBOOT.PBP"
cp "$WORK/build/wowee_psp.debug.elf" "$OUT/wowee_psp.debug.elf"
cp "$WORK/build/wowee_psp.map" "$OUT/wowee_psp.map"
# The crash catcher's kernel module (psp/sandstone-debug/prx), next to the EBOOT.
make -C "$WORK/src/psp/sandstone-debug/prx" > "$WORK/sdbg_prx.log" 2>&1 || { cat "$WORK/sdbg_prx.log"; exit 1; }
cp "$WORK/src/psp/sandstone-debug/prx/sdbg.prx" "$OUT/sdbg.prx"
cmp -s "$WORK/build/EBOOT.PBP" "$OUT/EBOOT.PBP" || { echo "EBOOT copy mismatch" >&2; exit 1; }

printf '%s\n' "$BUILD_NUM" > "$OUT/BUILD"
echo "== OK build $BUILD_NUM"
echo "   EBOOT.PBP $(stat -c %s "$OUT/EBOOT.PBP") bytes  md5 $(md5sum "$OUT/EBOOT.PBP" | cut -c1-32)"
echo "   text/data/bss: $(psp-size "$WORK/build/wowee_psp.debug.elf" | tail -1 | awk '{print $1"/"$2"/"$3}')"
