#!/usr/bin/env bash
# Build the existing Apple dtc fork in a private staging directory.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
source_tree=${NEXUS7_APPLE_DTC_SOURCE:-$root/../../Project_iOS2ARM/device_trees/dtc-AppleDeviceTree}
out=${NEXUS7_APPLE_DTC_OUT:-$root/build/apple-dtc}
if [[ -x "$out/dtc" && -f "$out/.apple-layout-fixed" ]]; then
    printf '%s\n' "$out/dtc"
    exit 0
fi
[[ -f "$source_tree/AppleDeviceTree.c" ]] || {
    echo 'Set NEXUS7_APPLE_DTC_SOURCE to the dtc-AppleDeviceTree source directory' >&2; exit 1;
}
mkdir -p "$out"
# Do not alter the compiler checkout shared with Project_iOS2ARM.
tar -C "$source_tree" --exclude=.git --exclude='*.o' --exclude='*.d' --exclude=dtc -cf - . | tar -C "$out" -xf -
patch --directory="$out" --forward --batch < "$root/tools/apple-dtc64.patch" >&2
make -C "$out" clean >&2
make -C "$out" -j"${DTC_JOBS:-4}" CC="${DTC_CC:-cc}" \
    CFLAGS='-g -Os -fPIC -Wall -m64' LDFLAGS=-m64 dtc >&2
printf '%s\n' 'uint32_t Apple DT layout; little-endian 64-bit host' > "$out/.apple-layout-fixed"
printf '%s\n' "$out/dtc"
