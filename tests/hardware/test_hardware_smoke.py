import pytest

from fb200.pedal import FB200Device
from fb200.transport import HidapiTransport

pytestmark = pytest.mark.hardware


@pytest.fixture
def device():
    transport = HidapiTransport().open()
    yield FB200Device(transport)
    transport.close()


def test_info_reads_versions(device):
    info = device.info()
    assert info.product == "FB200"
    assert info.firmware_version.startswith("V")


def test_ir_slots_are_nine(device):
    assert len(device.ir_list()) == 9
