#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
platform=${SHIM_PLATFORM:-nexus7}
case "$platform" in
    nexus7) address=0x88000000 ;;
    qemu-virt) address=0x48000000 ;;
    *) echo "Unsupported SHIM_PLATFORM: $platform" >&2; exit 1 ;;
esac
out=${SHIM_OUT:-$root/build/shim-$platform}
cc=${SHIM_CC:-clang}
objcopy=${SHIM_OBJCOPY:-llvm-objcopy}
readelf=${SHIM_READELF:-llvm-readelf}
mkdir -p "$out"
src="$root/boot/shim"
flags=(--target=armv7a-none-eabi -mcpu=cortex-a9 -marm -mfloat-abi=soft -mno-unaligned-access
    -std=c11 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic
    -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -Werror
    -I"$src/include" -I"$root/xnu/pexpert")
if [[ -n ${SHIM_UART_PHYS:-} ]]; then
    [[ $SHIM_UART_PHYS == 0x70006000 || $SHIM_UART_PHYS == 0x70006300 ]] || {
        echo 'SHIM_UART_PHYS must be UARTA 0x70006000 or UARTD 0x70006300' >&2; exit 1; }
    flags+=("-DSHIM_UART_PHYS=$SHIM_UART_PHYS")
fi
python3 "$root/tools/build-device-tree.py" --board "$root/boot/device-tree/$platform.dts" \
    --uart "${SHIM_UART_PHYS:-$([[ $platform == nexus7 ]] && echo 0x70006000 || echo 0x09000000)}" --out "$out/xnu-device-tree.adt"
"$cc" -E -P -x c -DSHIM_LINK_ADDRESS="$address" "$src/linker.ld.S" -o "$out/linker.ld"
objects=()
for file in "$src"/core/*.c "$src"/arch/armv7/*.c "$src"/arch/armv7/*.S "$src/platform/$platform/platform.c"; do
    obj="$out/$(basename "${file%.*}").o"
    "$cc" "${flags[@]}" -c "$file" -o "$obj"
    objects+=("$obj")
done
"$cc" "${flags[@]}" -nostdlib -fuse-ld=lld \
    -Wl,--build-id=none,--no-undefined,-T,"$out/linker.ld",-Map,"$out/shim.map" \
    "${objects[@]}" -o "$out/xnu-shim.elf"
"$objcopy" -O binary "$out/xnu-shim.elf" "$out/xnu-shim.bin"
"$readelf" -h -l "$out/xnu-shim.elf" > "$out/elf-layout.txt"
(cd "$out" && sha256sum xnu-shim.elf xnu-shim.bin > SHA256SUMS)
printf '{"platform":"%s","uart_phys":"%s"}\n' "$platform" "${SHIM_UART_PHYS:-0x70006000}" > "$out/build-config.json"
printf 'Built %s shim at %s: %s\n' "$platform" "$address" "$out"
