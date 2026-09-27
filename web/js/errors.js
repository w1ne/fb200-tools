// Mirrors src/fb200/errors.py (only the classes the web updater raises).

export class Fb200Error extends Error {
  constructor(message) {
    super(message);
    this.name = new.target.name;
  }
}

/** No matching USB device was found, or the user closed the picker. */
export class DeviceNotFoundError extends Fb200Error {}

/** A device transfer failed or timed out. */
export class CommunicationError extends Fb200Error {}

/** A firmware container could not be parsed, validated or written. */
export class FirmwareError extends Fb200Error {}
