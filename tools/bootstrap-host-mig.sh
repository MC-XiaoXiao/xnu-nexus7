#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
reference=${NEXUS7_MIG_REFERENCE:-/home/xxiao/Projects/Project_iLEmu}
src=${NEXUS7_MIG_SOURCE:-$reference/sources/bootstrap_cmds-93/migcom.tproj}
shim=${NEXUS7_MIG_SHIM:-$reference/tools/abi_reference/migcom/shim}
output=${NEXUS7_MIG:-$root/build/host-mig}
[[ -f "$src/parser.y" && -f "$shim/mach/ilemu_guest.h" ]] || {
    echo "MIG 93 source and ILP32 host headers are required; see README.md" >&2
    exit 1
}
mkdir -p "$output/bin" "$output/libexec"
bison -d -y -o "$output/y.tab.c" "$src/parser.y"
flex -o "$output/lexxer.c" "$src/lexxer.l"
sources=()
for file in "$src"/*.c; do
    [[ $(basename "$file") == handler.c ]] || sources+=("$file")
done
clang -std=gnu99 -Ulinux -D_DEFAULT_SOURCE -D__private_extern__= \
    -DMIG_VERSION='"mig-93"' -Wno-implicit-function-declaration \
    -Wno-incompatible-pointer-types -Wno-int-conversion \
    -I"$src" -I"$output" -I"$shim" \
    "${sources[@]}" "$output/y.tab.c" "$output/lexxer.c" -o "$output/libexec/migcom"
sed -e 's|arch=`/usr/sbin/arch`|arch=armv7|' \
    -e 's|arch=`/usr/bin/arch`|arch=armv7|' "$src/mig.sh" > "$output/bin/mig"
chmod +x "$output/bin/mig"
"$output/bin/mig" -version
