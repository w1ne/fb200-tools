// Copyright (C) 2026 Andrii Shylenko
//
// This software is released under the MIT License.
// See the LICENSE file in the project root for full license information.

// The vendor (Mooer) updater over WebHID: a port of
//   src/fb200/protocol.py  (framing, CRC16, report chunking, FrameReader)
//   src/fb200/firmware.py  (MrFile.from_bytes, parse only)
//   src/fb200/updater.py   (build_flash_plan, FirmwareUpdater.flash)
// The pedal is in this mode (USB 0483:5703) when A+D are held at power-on.
// Names follow the Python code so the two can be read side by side.

import { CommunicationError, DeviceNotFoundError, FirmwareError } from "./errors.js";

// ---- protocol.py ----------------------------------------------------------

export const UPDATE_VID = 0x0483;
export const UPDATE_PID = 0x5703;
export const REPORT_SIZE = 64;
export const CHUNK_SIZE = 63;
export const CMD_EXIT_BOOTLOADER = 0xff;

// CCITT polynomial 0x1021, MSB first: the same table as protocol.CRC_TABLE.
export const CRC_TABLE = (() => {
  const t = new Uint16Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n << 8;
    for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff;
    t[n] = c;
  }
  return t;
})();

/** Table-driven CCITT-style CRC16 with XOR-0xFFFF final; returns 2 bytes, high first. */
export function crc16(data) {
  let crc = 0;
  for (const b of data) crc = (CRC_TABLE[((crc >> 8) ^ b) & 0xff] ^ ((crc << 8) & 0xffff)) & 0xffff;
  crc ^= 0xffff;
  return Uint8Array.of((crc >> 8) & 0xff, crc & 0xff);
}

function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

/** Pack a command frame: AA 55 | len(u16 LE) | fn | data | CRC16. */
export function packFrame(fn, data = new Uint8Array(0)) {
  const body = concat(Uint8Array.of(fn), data);
  const framed = concat(Uint8Array.of(body.length & 0xff, (body.length >> 8) & 0xff), body);
  return concat(Uint8Array.of(0xaa, 0x55), framed, crc16(framed));
}

/** Split a frame into HID reports: [length][payload][padding]. */
export function* iterReports(frame, reportSize = REPORT_SIZE) {
  for (let off = 0; off < Math.max(frame.length, 1); off += CHUNK_SIZE) {
    const chunk = frame.subarray(off, off + CHUNK_SIZE);
    const report = new Uint8Array(reportSize);
    report[0] = chunk.length;
    report.set(chunk, 1);
    yield report;
  }
}

export async function writeFrame(transport, frame) {
  for (const report of iterReports(frame)) await transport.writeReport(report);
}

/** Incremental reassembly of AA 55 frames from HID reports. */
export class FrameReader {
  constructor() {
    this._buf = [];
    this._packets = [];
  }

  feedReport(report) {
    const valid = report[0];
    for (const b of report.subarray(1, 1 + valid)) this._buf.push(b);
    this._parse();
  }

  _find() {
    for (let i = 0; i + 1 < this._buf.length; i++) {
      if (this._buf[i] === 0xaa && this._buf[i + 1] === 0x55) return i;
    }
    return -1;
  }

  _parse() {
    for (;;) {
      const start = this._find();
      if (start < 0) {
        if (this._buf.length > 1) this._buf.splice(0, this._buf.length - 1);
        return;
      }
      if (start > 0) this._buf.splice(0, start);
      if (this._buf.length < 4) return;
      const length = this._buf[2] | (this._buf[3] << 8);
      const total = 6 + length;
      if (this._buf.length < total) return;
      const framed = Uint8Array.from(this._buf.slice(2, 4 + length));
      const want = crc16(framed);
      if (want[0] !== this._buf[4 + length] || want[1] !== this._buf[5 + length]) {
        this._buf.splice(0, 2);
        continue;
      }
      this._packets.push(Uint8Array.from(this._buf.slice(4, 4 + length)));
      this._buf.splice(0, total);
    }
  }

  nextPacket() {
    return this._packets.length ? this._packets.shift() : null;
  }

  async readPacket(transport, timeoutMs = 1500) {
    const deadline = performance.now() + timeoutMs;
    for (;;) {
      const packet = this.nextPacket();
      if (packet !== null) return packet;
      const remaining = deadline - performance.now();
      if (remaining <= 0) return null;
      const report = await transport.readReport(Math.ceil(remaining));
      if (report) this.feedReport(report);
    }
  }
}

// ---- firmware.py (parse only) ---------------------------------------------

const MAGIC = "Mooer_TAG";
const HEADER_SIZE = 128;
const TAG_SIZE = 512;

function ascii(bytes) {
  return String.fromCharCode(...bytes);
}

function parseHeader(raw) {
  const v = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  const product = raw.subarray(9, 41);
  const nul = product.indexOf(0);
  return {
    tag: ascii(raw.subarray(0, 9)),
    productTag: new TextDecoder().decode(nul < 0 ? product : product.subarray(0, nul)),
    sendCmd: raw[41],
    recCmd: raw[42],
    timeout: v.getUint32(43, true),
    updateBlock: raw[47],
    updateAddr: raw.slice(48, 52),
    version: raw[52],
  };
}

function parseTag(raw) {
  const v = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  return {
    tag: ascii(raw.subarray(0, 9)),
    startAddr: v.getUint32(9, true),
    stopAddr: v.getUint32(13, true),
    blockSize: v.getUint32(17, true),
    sendCmd: raw[21],
    recCmd: raw[22],
    timeout: v.getUint32(23, true),
    startPage: v.getUint32(27, true),
    romId: raw[31],
  };
}

/** Mooer `.mr` container (MrFile.from_bytes). */
export class MrFile {
  constructor(header, blocks) {
    this.header = header;
    this.blocks = blocks;
  }

  get totalDataSize() {
    return this.blocks.reduce((n, b) => n + b.data.length, 0);
  }

  static fromBytes(data) {
    if (data.length < HEADER_SIZE) throw new FirmwareError("file too small for .mr header");
    const header = parseHeader(data.subarray(0, HEADER_SIZE));
    if (header.tag !== MAGIC) throw new FirmwareError("not a Mooer .mr file (bad header tag)");
    const blocks = [];
    let offset = HEADER_SIZE;
    for (let i = 0; i < header.updateBlock; i++) {
      if (offset + TAG_SIZE > data.length) throw new FirmwareError("truncated block tag");
      const tag = parseTag(data.subarray(offset, offset + TAG_SIZE));
      if (tag.tag !== MAGIC) throw new FirmwareError("not a Mooer .mr file (bad block tag)");
      offset += TAG_SIZE;
      if (offset + tag.blockSize > data.length) throw new FirmwareError("truncated block data");
      blocks.push({ tag, data: data.subarray(offset, offset + tag.blockSize) });
      offset += tag.blockSize;
    }
    if (offset !== data.length) {
      throw new FirmwareError(`${data.length - offset} trailing bytes after blocks`);
    }
    return new MrFile(header, blocks);
  }
}

// ---- updater.py -----------------------------------------------------------

export const PAGE_SIZE = 512;
export const NEW_PORT_PAGE_SIZE = 1024;

function u32le(n) {
  return Uint8Array.of(n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff);
}

export function buildFlashPlan(mr) {
  const headerCmd = mr.header.sendCmd;
  if (!(headerCmd >= 0 && headerCmd <= 0xff)) {
    throw new FirmwareError(`invalid erase command 0x${headerCmd.toString(16)}`);
  }
  let eraseData;
  if (mr.header.version === 0) {
    eraseData = Uint8Array.from(mr.header.updateAddr);
  } else {
    eraseData = concat(
      ...mr.blocks.map((b) => concat(Uint8Array.of(b.tag.romId), u32le(b.tag.startPage), u32le(b.data.length))),
    );
  }
  const writes = [];
  let total = 0;
  for (const block of mr.blocks) {
    if (!block.data.length) throw new FirmwareError("empty firmware block");
    if (!(block.tag.sendCmd >= 0 && block.tag.sendCmd <= 0xfe)) {
      throw new FirmwareError(`invalid block send command 0x${block.tag.sendCmd.toString(16)}`);
    }
    const chunkSize = block.tag.sendCmd >> 7 ? NEW_PORT_PAGE_SIZE : PAGE_SIZE;
    const pageCount = Math.ceil(block.data.length / chunkSize);
    if (block.tag.startPage + pageCount > 0x10000) {
      throw new FirmwareError("page range exceeds 16-bit page address space");
    }
    for (let page = 0, offset = 0; offset < block.data.length; page++, offset += chunkSize) {
      const chunk = block.data.subarray(offset, offset + chunkSize);
      const pageNumber = (block.tag.startPage + page) & 0xffff;
      writes.push([
        block.tag.sendCmd + 1,
        packFrame(block.tag.sendCmd, concat(Uint8Array.of(pageNumber >> 8, pageNumber & 0xff), chunk)),
      ]);
      total += chunk.length;
    }
  }
  return {
    eraseFrame: packFrame(headerCmd, eraseData),
    eraseReply: mr.header.recCmd,
    writes,
    exitFrame: packFrame(CMD_EXIT_BOOTLOADER),
    totalBytes: total,
    get writeCount() {
      return this.writes.length;
    },
  };
}

const hex2 = (n) => "0x" + n.toString(16).padStart(2, "0");

export class FirmwareUpdater {
  constructor(transport) {
    this.transport = transport;
    this.reader = new FrameReader();
  }

  /**
   * Erase, write and exit the bootloader. The erase acknowledgement can take
   * well over ten seconds (measured ~11.5 s), so it has its own, longer
   * timeout. On failure the pedal stays in the bootloader; run it again.
   * After the exit frame the pedal reboots. It does not come back as the
   * vendor app (34DB:800F HID): with the open firmware that is expected.
   */
  async flash(mr, { progress = null, timeoutMs = 10000, eraseTimeoutMs = 60000, log = () => {} } = {}) {
    const plan = buildFlashPlan(mr);
    while (this.reader.nextPacket() !== null) {
      /* drop stale packets */
    }
    log(`erase: ${plan.writeCount} pages to write, ${plan.totalBytes} bytes`);
    await writeFrame(this.transport, plan.eraseFrame);
    let packet = await this.reader.readPacket(this.transport, eraseTimeoutMs);
    if (!packet) throw new CommunicationError("erase command not acknowledged (no reply)");
    if (packet[0] !== plan.eraseReply) {
      throw new CommunicationError(`erase command not acknowledged (got ${hex2(packet[0])})`);
    }
    log("erase ok");
    let sent = 0;
    for (const [expectedReply, frame] of plan.writes) {
      await writeFrame(this.transport, frame);
      packet = await this.reader.readPacket(this.transport, timeoutMs);
      if (!packet || packet[0] !== expectedReply) {
        throw new CommunicationError(`unexpected reply while writing page ${sent + 1}`);
      }
      sent += 1;
      if (progress) progress(sent, plan.writes.length);
    }
    log(`wrote ${sent} pages; exit`);
    try {
      await writeFrame(this.transport, plan.exitFrame);
    } catch (err) {
      // The pedal may reboot while the exit report is in flight.
      log(`exit frame: ${err.message} (the pedal is rebooting; expected)`);
    }
    return { writeFrames: sent, bytesWritten: plan.totalBytes };
  }
}

// ---- WebHID transport (transport.py HidapiTransport) ----------------------

/**
 * 64-byte reports over WebHID. The update device has no report IDs: byte 0
 * of every report is the payload length (protocol.py), so reports go out
 * with sendReport(0, report), as hidapi sends them.
 */
export class WebHidTransport {
  constructor(device) {
    this.device = device;
    this._reports = [];
    this._waiters = [];
    this._onReport = (event) => {
      const report = new Uint8Array(event.data.buffer, event.data.byteOffset, event.data.byteLength).slice();
      const waiter = this._waiters.shift();
      if (waiter) waiter(report);
      else this._reports.push(report);
    };
  }

  async open() {
    try {
      if (!this.device.opened) await this.device.open();
    } catch (err) {
      throw new CommunicationError(`failed to open the update device: ${err.message}`);
    }
    this.device.addEventListener("inputreport", this._onReport);
    return this;
  }

  async writeReport(report) {
    try {
      await this.device.sendReport(0, report);
    } catch (err) {
      throw new CommunicationError(`HID write failed: ${err.message}`);
    }
  }

  readReport(timeoutMs = 500) {
    if (this._reports.length) return Promise.resolve(this._reports.shift());
    return new Promise((resolve) => {
      const waiter = (report) => {
        clearTimeout(timer);
        resolve(report);
      };
      const timer = setTimeout(() => {
        const i = this._waiters.indexOf(waiter);
        if (i >= 0) this._waiters.splice(i, 1);
        resolve(null);
      }, timeoutMs);
      this._waiters.push(waiter);
    });
  }

  async close() {
    this.device.removeEventListener("inputreport", this._onReport);
    try {
      if (this.device.opened) await this.device.close();
    } catch {
      /* already gone after the reboot */
    }
  }
}

/** Show the browser's device picker, filtered to the update mode (0483:5703). */
export async function requestUpdateDevice() {
  const devices = await navigator.hid.requestDevice({
    filters: [{ vendorId: UPDATE_VID, productId: UPDATE_PID }],
  });
  if (!devices.length) {
    throw new DeviceNotFoundError("No pedal in update mode was chosen.");
  }
  return devices[0];
}
