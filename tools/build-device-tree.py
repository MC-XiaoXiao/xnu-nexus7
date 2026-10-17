#!/usr/bin/env python3
"""Compile DTS/DTSI with the existing Apple Device Tree dtc fork."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def build(board, uart=None):
    if sys.byteorder != 'little':
        raise RuntimeError('the Apple dtc fork requires a little-endian host')
    compiler=os.environ.get('NEXUS7_APPLE_DTC')
    if not compiler:
        compiler=subprocess.check_output(['bash',str(ROOT/'tools/build-apple-dtc.sh')],text=True).strip()
    with tempfile.TemporaryDirectory(prefix='xnu-dts-') as tmp:
        source=Path(tmp)/'tree.dts'; output=Path(tmp)/'tree.adt'
        flags=[os.environ.get('CPP','clang'),'-E','-P','-nostdinc','-undef','-x','assembler-with-cpp',
               '-I'+str(ROOT/'boot/device-tree')]
        if uart is not None: flags.append(f'-DXNU_DT_UART=0x{uart:x}')
        subprocess.run([*flags,str(Path(board).resolve()),'-o',str(source)],check=True)
        subprocess.run([compiler,'-I','dts','-O','dtb','-o',str(output),str(source)],check=True)
        blob=output.read_bytes()
    if not 8 <= len(blob) <= 8192 or blob[:4]==b'\xd0\x0d\xfe\xed':
        raise ValueError('expected Apple DT, <=8192 bytes; use the modified Apple dtc')
    return blob

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--board',type=Path,default=ROOT/'boot/device-tree/nexus7.dts')
    p.add_argument('--uart',type=lambda s:int(s,0))
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    try: blob=build(a.board,a.uart)
    except (ValueError,RuntimeError,subprocess.CalledProcessError) as e: p.error(str(e))
    a.out.parent.mkdir(parents=True,exist_ok=True); a.out.write_bytes(blob)
    print(f'Apple DT: {a.out} ({len(blob)} bytes)')
if __name__=='__main__': main()
