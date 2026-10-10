#!/usr/bin/env python3
"""Verify a fully linked Nexus7 ARMv7 Mach-O kernel and write build evidence."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('kernel', type=Path)
p.add_argument('--nm', required=True)
p.add_argument('--filelist', type=Path, required=True)
p.add_argument('--log', type=Path)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
b = a.kernel.read_bytes()
magic, cpu, subtype, kind, ncmd, cmdsize, flags = struct.unpack_from('<7I', b)
assert (magic, cpu, subtype & 0xffffff, kind) == (0xfeedface, 12, 9, 2), 'not ARMv7 MH_EXECUTE'
assert flags & 1, 'MH_NOUNDEFS absent'
assert 28 + cmdsize <= len(b)
if a.log:
    assert not re.search(r'^make(?:\[\d+\])?: \*\*\*', a.log.read_text(), re.M), 'submake failed'
undefined = subprocess.check_output([a.nm, '-u', str(a.kernel)], text=True).strip()
assert not undefined, f'unresolved symbols: {undefined}'
symbols = subprocess.check_output([a.nm, '-g', str(a.kernel)], text=True)
for name in ('__start', '_arm_init', '_PE_init_SocSupport_stub', '_tegra30_early_putc',
             '_tegra30_uart_init', '_rtc_configure', '_rtclock_intr', '_vm_pageout'):
    assert re.search(r'^[0-9a-f]+ [A-Z] ' + re.escape(name) + '$', symbols, re.M), f'missing {name}'
objects = [Path(s) for s in a.filelist.read_text().splitlines() if s]
assert objects and all(o.is_file() for o in objects)
assert any(o.name == 'pe_tegra30.o' for o in objects)
assert any(o.name == 'locore.o' for o in objects)
segments, entry, pos = [], None, 28
for _ in range(ncmd):
    command, size = struct.unpack_from('<2I', b, pos)
    assert size >= 8 and pos + size <= 28 + cmdsize
    if command == 1:
        name, va, vsize, off, fsize, maxprot, prot, nsect, segflags = struct.unpack_from('<16s8I', b, pos + 8)
        assert off + fsize <= len(b)
        segments.append(dict(name=name.rstrip(b'\0').decode(), vmaddr=hex(va), vmsize=vsize,
                             fileoff=off, filesize=fsize, initprot=prot))
    elif command == 5:
        flavor, count = struct.unpack_from('<2I', b, pos + 8)
        assert (flavor, count) == (1, 17), 'unexpected ARM thread state'
        entry = struct.unpack_from('<I', b, pos + 16 + 15 * 4)[0]
    pos += size
assert entry is not None, 'LC_UNIXTHREAD entry missing'
assert any(s['initprot'] & 4 and int(s['vmaddr'], 16) <= entry < int(s['vmaddr'], 16) + s['vmsize'] for s in segments)
for obj in objects:
    omagic, ocpu, osubtype, okind = struct.unpack_from('<4I', obj.read_bytes())
    assert (omagic, ocpu, osubtype & 0xffffff, okind) == (0xfeedface, 12, 9, 1), str(obj)
report = dict(kernel=str(a.kernel.resolve()), sha256=hashlib.sha256(b).hexdigest(),
              bytes=len(b), architecture='armv7', format='Mach-O MH_EXECUTE',
              entry=hex(entry), object_count=len(objects), unresolved_symbols=0,
              platform='TEGRA30_NEXUS7', segments=segments,
              hardware_boot_tested=False)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2) + '\n')
print(f'Verified ARMv7 kernel: {len(objects)} objects, entry {entry:#x}, no unresolved symbols')
