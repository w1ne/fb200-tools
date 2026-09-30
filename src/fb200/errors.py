# Copyright (C) 2026 Andrii Shylenko
#
# This software is released under the MIT License.
# See the LICENSE file in the project root for full license information.

class Fb200Error(Exception):
    """Base class for all fb200-tools errors."""


class DeviceNotFoundError(Fb200Error):
    """No matching USB device was found."""


class CommunicationError(Fb200Error):
    """A device transfer failed or timed out."""


class NoReplyError(CommunicationError):
    """The device did not answer a request in time (it may be busy or resetting)."""


class ProtocolError(Fb200Error):
    """A reply frame was malformed or unexpected."""


class InvalidArgumentError(Fb200Error):
    """A caller-supplied value is outside the accepted range."""


class WavError(Fb200Error, ValueError):
    """A WAV file could not be parsed, validated or converted."""


class FirmwareError(Fb200Error, ValueError):
    """A firmware container could not be parsed, validated or written."""
