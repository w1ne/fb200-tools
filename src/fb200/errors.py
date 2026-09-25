class Fb200Error(Exception):
    """Base class for all fb200-tools errors."""


class DeviceNotFoundError(Fb200Error):
    """No matching USB device was found."""


class CommunicationError(Fb200Error):
    """A device transfer failed or timed out."""


class ProtocolError(Fb200Error):
    """A reply frame was malformed or unexpected."""


class InvalidArgumentError(Fb200Error):
    """A caller-supplied value is outside the accepted range."""


class WavError(Fb200Error, ValueError):
    """A WAV file could not be parsed, validated or converted."""


class FirmwareError(Fb200Error, ValueError):
    """A firmware container could not be parsed, validated or written."""
