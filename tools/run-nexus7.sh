#!/usr/bin/env bash
# Single build/package/run entry point; fastboot stage/continue to RAM.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export NEXUS7_UART_PHYS=0x70006000
export NEXUS7_OBJROOT=${NEXUS7_OBJROOT:-$root/build/obj-nexus7-uarta}
out="$root/build/nexus7-run"
port="${NEXUS7_PORT:-/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0}"
attempts=${NEXUS7_ATTEMPTS:-1}
mkdir -p "$out"
rm -f "$out/results.json"
"$root/tools/build-nexus7.sh" > "$out/build.log" 2>&1
SHIM_PLATFORM=nexus7 SHIM_UART_PHYS=$NEXUS7_UART_PHYS SHIM_OUT="$out/shim" \
    "$root/tools/build-xnu-shim.sh"
python3 "$root/tools/package-xnu-boot.py" \
    --kernel "$NEXUS7_OBJROOT/DEVELOPMENT_ARM_TEGRA30_NEXUS7/mach_kernel" \
    --shim "$out/shim/xnu-shim.bin" --kernel-uart UARTA \
    --bootargs 'debug=0x8 keepsyms=1 serial=1' --out "$out/bundle"
: > "$out/serial.log"
python3 "$root/tools/uboot-xnu.py" debug --bundle "$out/bundle" \
    --port "$port" --transport fastboot --force-upload --attempts "$attempts" \
    --watchdog-seconds "${NEXUS7_WATCHDOG_SECONDS:-30}" \
    --log "$out/serial.log" 2>&1 | tee "$out/hardware.log"
python3 - "$out" "$attempts" <<'PY'
from pathlib import Path
import json
import sys
out, attempts = Path(sys.argv[1]), int(sys.argv[2])
log = (out / 'serial.log').read_text(errors='replace')
checks = {
    'emmc_online': log.count('[tegra-emmc] online sectors=') == attempts,
    'platform_started': log.count('AppleARMPE::start: Welcome') == attempts,
    'cpu_initialized': log.count('we are here to serve!') == attempts,
    'arm_io_started': log.count('AppleARMIO::start: Publishing device tree') == attempts,
    'bsd_started': log.count('bsd_autoconf: calling kminit') == attempts,
    'no_emmc_failure': '[tegra-emmc] init failed' not in log and '[tegra-emmc] status error' not in log,
    'no_devfs_name_collision': 'name slot allocation failed' not in log,
    'no_panic': 'panic(cpu' not in log,
    'watchdog_recovered': log.count('Reset cause: WATCHDOG') == attempts,
    'fastboot_uploaded': 'through fastboot to RAM only' in (out / 'hardware.log').read_text(),
    'no_storage_test_client': '[tegra-emmc-test]' not in log,
}
result = {'passed': all(checks.values()), 'checks': checks,
          'transport': 'fastboot stage/continue', 'attempts': attempts,
          'files': json.loads((out / 'bundle/manifest.json').read_text())['files']}
(out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
if not result['passed']:
    sys.exit('Nexus7 run verification failed; see serial.log')
PY
