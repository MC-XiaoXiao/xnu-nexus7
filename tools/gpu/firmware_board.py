"""The tested iPad2,5 userland profile over Tegra30 hardware matching."""
import re
from firmware_identity import board_properties


def native_board_profile(board, usb=True):
    # Native UIKit chooses its resource suffix from MobileGestalt DeviceClass,
    # which derives from hw.machine. Use the installed firmware's product type
    # in this isolated boot profile; hardware matching remains Tegra30.
    if not usb:
        board = re.sub(r'\n        usb-device \{[^{}]*\n        \};', '', board)
    assert board.count('model = "Nexus 7 (2012)";') == 1
    board = board.replace('model = "Nexus 7 (2012)";', 'model = "iPad2,5";', 1)
    # HWModelStr comes from the first root compatible string. Stock firmware
    # parses the Apple board ID; retain the SoC string for hardware matching.
    assert board.count('compatible = "nvidia,tegra30";') == 1
    # Our IOAudio2 provider exposes a single Codec, as on K93. P105's native
    # routing database requires a separate Speaker device for active speaker
    # protection; selecting it deadlocks mediaserverd initialization and makes
    # SpringBoard's InitializeSystemSoundPorts RPC time out and kill itself.
    # ProductType remains iPad2,5 for the installed UIKit resources.
    board = board.replace('compatible = "nvidia,tegra30";',
                          'compatible = "K93AP", "nvidia,tegra30";', 1)
    return board + board_properties(usb)
