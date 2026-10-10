#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
# Existing local cross tools; override to relocate the toolchain.
deps=${NEXUS7_DEPS:-/home/xxiao/Projects/Project_iOS2ARM/build}
tools=${NEXUS7_CCTOOLS:-$deps/toolchain/bin}
mig=${NEXUS7_MIG:-$root/build/host-mig}
helpers="$root/tools/linux"
obj=${NEXUS7_OBJROOT:-$root/build/obj-nexus7}
mkdir -p "$root/build"
export NEXUS7_CCTOOLS="$tools"
jobs=${JOBS:-8}
uart=${NEXUS7_UART_PHYS:-0x70006300}
[[ $uart == 0x70006000 || $uart == 0x70006300 ]] || {
    echo 'NEXUS7_UART_PHYS must be 0x70006000 (UARTA) or 0x70006300 (UARTD)' >&2; exit 1;
}
[[ -x "$mig/libexec/migcom" ]] || "$root/tools/bootstrap-host-mig.sh"
for path in "$tools/arm-apple-darwin11-ld" "$tools/arm-apple-darwin11-nm" \
    "$deps/deps/unifdef/usr/bin/unifdef" "$deps/deps/tcsh/usr/bin/tcsh"; do
    [[ -x "$path" ]] || { echo "Missing build prerequisite: $path" >&2; exit 1; }
done
if [[ $# == 0 ]]; then set -- all; fi
log="$root/build/build.log"
target="$obj/DEVELOPMENT_ARM_TEGRA30_NEXUS7"

# Make does not track command-line flag changes. Refuse to reuse unknown or
# differently configured objects; selecting a fresh OBJROOT is non-destructive.
mkdir -p "$obj"
signature="$obj/nexus7-uart.txt"
compiler_signature="$obj/nexus7-compiler.sha256"
compiler_hash=$(sha256sum "$helpers/clang-armv7" | cut -d ' ' -f 1)
if [[ -d "$target" ]] && { [[ ! -f "$compiler_signature" ]] || [[ $(cat "$compiler_signature") != "$compiler_hash" ]]; }; then
    echo 'Compiler wrapper changed or is unknown: choose a fresh NEXUS7_OBJROOT to rebuild all objects' >&2; exit 1
fi
if [[ -f "$signature" ]]; then
    [[ $(cat "$signature") == "$uart" ]] || {
        echo 'UART changed: choose a fresh NEXUS7_OBJROOT to rebuild all objects' >&2; exit 1;
    }
elif [[ -d "$target" ]]; then
    echo 'Existing OBJROOT has no UART signature: choose a fresh NEXUS7_OBJROOT' >&2; exit 1
fi
printf '%s\n' "$uart" > "$signature"
printf '%s\n' "$compiler_hash" > "$compiler_signature"

export PATH="$deps/deps/tcsh/usr/bin:$helpers:$PATH"
# doconf is an upstream tcsh script with an absolute /bin/tcsh shebang.
# Use a build-local copy so the source tree stays portable.
mkdir -p "$root/build/setup"
sed '1c#!/usr/bin/env tcsh' "$root/xnu/SETUP/config/doconf" > "$root/build/setup/doconf"
chmod +x "$root/build/setup/doconf"
sed 's|arch=`/usr/sbin/arch`|arch=armv7|' "$mig/bin/mig" > "$root/build/setup/mig"
chmod +x "$root/build/setup/mig"
make -C "$root/xnu" -j"${JOBS:-8}" \
 MACHINE_FLAGS_TEGRA30_NEXUS7="-DCONFIG_EMBEDDED=1 -DNO_KEXTD=1 -DNEXUS7_UART_PHYS=$uart" \
 TARGET_CONFIGS="DEVELOPMENT ARM TEGRA30_NEXUS7" ARCH_CONFIGS=ARM MACHINE_CONFIG=TEGRA30_NEXUS7 MAKEJOBS="-j$jobs" \
 XCRUN="$helpers/xcrun" SDKROOT=/ SDKROOT_RESOLVED=/ HOST_SDKROOT=/ \
 CC="$helpers/clang-armv7" CXX="$helpers/clang-armv7" MIGCC="$helpers/clang-armv7" \
 LD="$helpers/ld-armv7" AS="$tools/arm-apple-darwin11-as" \
 MIG="$root/build/setup/mig" MIGCOM="$mig/libexec/migcom" \
 STRIP="$tools/arm-apple-darwin11-strip" LIPO="$tools/arm-apple-darwin11-lipo" \
 LIBTOOL="$tools/arm-apple-darwin11-libtool" NM="$tools/arm-apple-darwin11-nm" \
 NMEDIT="$tools/arm-apple-darwin11-nmedit" \
 UNIFDEF="$deps/deps/unifdef/usr/bin/unifdef" \
 HOST_CC="$helpers/host-cc" HOST_FLEX=/usr/bin/flex HOST_BISON=/usr/bin/bison \
 HOST_CODESIGN=/usr/bin/true NEWVERS="$root/xnu/config/newvers.pl" \
 SEG_HACK="$helpers/setsegname" KEXT_CREATE_SYMBOL_SET=/usr/bin/false DOCONF="$root/build/setup/doconf" \
 OBJROOT="$obj" DSTROOT="$root/build/dst" SYMROOT="$root/build/sym" \
 BUILD_LTO=0 NO_DTRACE_SYMS=YES BUILD_WERROR=0 LD_KERNEL_LIBS= CONFIG_SUBDIRS_ARM= \
 "$@" 2>&1 | tee "$log"
python3 "$root/tools/verify-kernel.py" "$target/mach_kernel.sys" \
    --nm "$tools/arm-apple-darwin11-nm" --filelist "$target/link.filelist" \
    --log "$log" --output "$root/build/verification.json"
mkdir -p "$root/build/output"
cp "$target/mach_kernel" "$root/build/output/mach_kernel"
cp "$target/mach_kernel.sys" "$root/build/output/mach_kernel.sys"
cp "$root/build/verification.json" "$root/build/output/verification.json"
sha256sum "$root/build/output/mach_kernel" "$root/build/output/mach_kernel.sys" > "$root/build/output/SHA256SUMS"
printf '{"uart_phys":"%s","platform":"TEGRA30_NEXUS7"}\n' "$uart" > "$root/build/output/build-config.json"
