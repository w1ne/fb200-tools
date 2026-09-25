from fb200.pedal import FB200Device
from fb200.protocol import pack_frame


def test_info_parses_version_reply(version_transport):
    info = FB200Device(version_transport).info()
    assert info.product == "FB200"
    assert info.app_version == "V1.0.0"
    assert info.firmware_version == "V1.0.1"
    assert info.bluetooth_version == "V1.0.0"
    assert info.hardware_rev == "A"


def test_info_sends_get_version_frame(version_transport):
    FB200Device(version_transport).info()
    expected = pack_frame(0x00)
    assert version_transport.written[0] == bytes([len(expected)]) + expected + bytes(64 - 1 - len(expected))
