"""Persistent development identity consumed by the native iOS6 board profile."""
import hashlib
import os
from pathlib import Path
import plistlib
import re
import struct

ROOT = Path(__file__).resolve().parents[2]
DEVICE_PATH = '/private/var/db/nexus7-identity.plist'


def identity_path():
    hardware = os.environ.get('NEXUS7_SERIAL', '015d483b881bee16').lower()
    if not re.fullmatch('[0-9a-f]{16}', hardware):
        raise ValueError('NEXUS7_SERIAL must be the 16-digit Nexus7 hardware ID')
    return ROOT / 'build/device-identity' / (hardware + '.plist')


def udid(value):
    # Original 10B329 MobileGestalt, udid-version=1: SHA1 of these four
    # strings, with decimal ECID and lowercase colon-separated MACs.
    mac = lambda data: ':'.join('%02x' % b for b in data)
    text = (value['SerialNumber'] + str(value['UniqueChipID']) +
            mac(value['WifiAddress']) + mac(value['BluetoothAddress']))
    return hashlib.sha1(text.encode('ascii')).hexdigest()


def validate(value):
    hardware = identity_path().stem
    if value.get('Version') != 1 or value.get('HardwareSerial') != hardware:
        raise ValueError('Identity belongs to a different board or schema')
    if not re.fullmatch('[0-9A-HJ-NP-Z]{12}', value.get('SerialNumber', '')):
        raise ValueError('Expected a synthetic 12-character Apple-format serial')
    if value.get('UniqueChipID') != int(hardware, 16):
        raise ValueError('Identity ECID must retain this board hardware ID')
    for key in ('WifiAddress', 'BluetoothAddress'):
        data = value.get(key)
        if not isinstance(data, bytes) or len(data) != 6 or data[0] & 3 != 2:
            raise ValueError('Development MAC must be locally administered unicast')
    if value.get('USBSerialNumber') != udid(value):
        raise ValueError('USB serial does not match native MobileGestalt UDID inputs')
    return value


def identity():
    path = identity_path()
    if not path.exists():
        # Deterministic recovery also survives loss of the host build directory.
        # Persist the inputs on the device with usb-identity-install. This is
        # an Apple-format development serial, never an Apple-issued identity.
        # https://github.com/acidanthera/OpenCorePkg/blob/master/Utilities/macserial/FORMAT.md
        digest = hashlib.sha256(('nexus7-ios6-identity-v1:' + path.stem).encode()).digest()
        alphabet = '0123456789ABCDEFGHJKLMNPQRSTUVWXYZ'
        week = '123456789CDFGHJKLMNPQRTVWX'[digest[0] % 26]
        serial = 'DMPJ' + week + ''.join(alphabet[b % 34] for b in digest[1:4]) + 'F196'
        value = dict(Version=1, HardwareSerial=path.stem, Synthetic=True,
            SerialNumber=serial, UniqueChipID=int(path.stem, 16),
            WifiAddress=bytes([2]) + digest[4:9], BluetoothAddress=bytes([2]) + digest[9:14])
        value['USBSerialNumber'] = udid(value)
        validate(value)
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as output:
            os.chmod(path, 0o600)
            output.write(plistlib.dumps(value))
    return validate(plistlib.loads(path.read_bytes()))


def board_properties(usb=True):
    value = identity()
    data = lambda raw: '[' + raw.hex(' ') + ']'
    # Root IOPlatformExpertDevice properties are available before launchd.
    # IONameMatching finds the two address nubs without requiring a network
    # driver; no interface or wireless capabilities are advertised by them.
    return '''
/ {
    IOPlatformSerialNumber = "%s";
    product { udid-version = <1>; };
    chosen { unique-chip-id = %s; };
    wlan { local-mac-address = %s; };
    bluetooth { local-mac-address = %s; };
    %s
};
''' % (value['SerialNumber'], data(struct.pack('<Q', value['UniqueChipID'])),
       data(value['WifiAddress']), data(value['BluetoothAddress']),
       ('arm-io { usb-device { USBSerialNumber = "%s"; }; };' % value['USBSerialNumber']) if usb else '')
