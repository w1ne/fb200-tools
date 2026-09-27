// The open firmware's USB CDC console over Web Serial: a port of
// src/fb200/console.py (Console.write/drain/run/expect/command and update()).
// Names follow the Python code so the two can be read side by side.
//
// The console text is kept as a "binary string" (one char per byte), so the
// needles match bytes exactly as in Python.

import { crc32 } from "./crc32.js";
import { CommunicationError, DeviceNotFoundError } from "./errors.js";

export const VID = 0x34db;                 // the open firmware and its recovery
export const LOADER_OFF = 0x400;
export const LOADER_END = 0x784;           // vendor stub + loader code in block 0
export const FLASH_BLOCK0 = 0x60010000;
// CRC-32 of the stock V1.0.1 loader bytes [LOADER_OFF, LOADER_END).
export const STOCK_LOADER_CRC = 0x80cbbac5;
export const COMMANDS = { app: "fwbegin", block0: "fwbegin", recovery: "fwrec", stock: "fwstock" };
export const BAUD_RATE = 115200;           // the CDC ignores it

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => performance.now();
const toBinary = (bytes) => {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return s;
};
const fromText = (text) => Uint8Array.from(text, (c) => c.charCodeAt(0) & 0xff);
const pyHex = (n) => "0x" + (n >>> 0).toString(16);    // Python f"{n:#x}"

export class Console {
  /** `port` is an open SerialPort (or anything with readable/writable streams). */
  constructor(port, { echo = null } = {}) {
    this.port = port;
    this.echo = echo;                       // callable(str) per line, or null
    this.buf = "";
    this.closed = false;
    this._chunks = [];
    this._waiter = null;
    this._reader = port.readable.getReader();
    this._writer = port.writable.getWriter();
    this._pump = this._readLoop();
  }

  async _readLoop() {
    try {
      for (;;) {
        const { value, done } = await this._reader.read();
        if (done) break;
        if (value && value.length) {
          this._chunks.push(toBinary(value));
          this._wake();
        }
      }
    } catch {
      /* the device went away (reset, unplug) */
    } finally {
      this.closed = true;
      this._wake();
    }
  }

  _wake() {
    const w = this._waiter;
    this._waiter = null;
    if (w) w();
  }

  /** Wait up to `ms` for data; returns "" when nothing arrived. */
  async _readSome(ms) {
    if (!this._chunks.length && !this.closed && ms > 0) {
      await new Promise((resolve) => {
        const timer = setTimeout(() => {
          this._waiter = null;
          resolve();
        }, ms);
        this._waiter = () => {
          clearTimeout(timer);
          resolve();
        };
      });
    }
    const out = this._chunks.join("");
    this._chunks = [];
    return out;
  }

  async close() {
    try {
      await this._reader.cancel();
    } catch { /* gone */ }
    await this._pump;
    try { this._reader.releaseLock(); } catch { /* gone */ }
    try { this._writer.releaseLock(); } catch { /* gone */ }
    try {
      if (this.port.close) await this.port.close();
    } catch { /* gone */ }
  }

  async write(data, timeoutMs = 10000) {
    const bytes = typeof data === "string" ? fromText(data) : data;
    let timer;
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(
        () => reject(new CommunicationError("pedal stopped reading USB (firmware hung?)")),
        timeoutMs,
      );
    });
    try {
      await Promise.race([this._writer.write(bytes), timeout]);
    } catch (err) {
      if (err instanceof CommunicationError) throw err;
      throw new CommunicationError(`USB write failed: ${err.message}`);
    } finally {
      clearTimeout(timer);
    }
  }

  /** Read until the device has been quiet for quietMs. */
  async drain(quietMs = 300, maxMs = 3000) {
    let out = "";
    const end = now() + maxMs;
    let last = now();
    while (now() < end && now() - last < quietMs) {
      const chunk = await this._readSome(Math.min(10, end - now()));
      if (chunk) {
        out += chunk;
        last = now();
      } else if (this.closed) {
        break;
      }
    }
    return out;
  }

  /** Send one command; return its output without the echoed command line. */
  async run(cmd, quietMs = 300, maxMs = 5000) {
    this.buf += await this.drain(50, 200);
    this.buf = "";
    await this.write(cmd + "\r");
    const text = await this.drain(quietMs, maxMs);
    let lines = text.replaceAll("\r", "").split("\n");
    if (lines.length && lines[0].trim() === cmd.trim()) lines = lines.slice(1);
    return lines.join("\n").replace(/^\n+|\n+$/g, "");
  }

  /** Read until a line contains one of `needles`; return that line. */
  async expect(needles, timeoutMs) {
    const deadline = now() + timeoutMs;
    while (now() < deadline) {
      let nl;
      while ((nl = this.buf.indexOf("\n")) >= 0) {
        const text = this.buf.slice(0, nl).trim();
        this.buf = this.buf.slice(nl + 1);
        if (text && this.echo) this.echo(text);
        if (needles.some((n) => text.includes(n))) return text;
      }
      if (this.closed && !this._chunks.length) {
        throw new CommunicationError("the pedal disconnected");
      }
      const chunk = await this._readSome(deadline - now());
      if (chunk) this.buf += chunk;
    }
    throw new CommunicationError(`timeout waiting for ${JSON.stringify(needles)}`);
  }

  async command(cmd, needles, timeoutMs = 5000) {
    await this.write(cmd + "\r");
    return this.expect(needles, timeoutMs);
  }
}

/**
 * Write the app slot (`fwbegin`), recovery (`fwrec`), the stock data
 * (`fwstock`) or all of block 0 (`fwbegin`). Mirrors console.update().
 * `progress(done, total)` is called while the image streams.
 */
export async function update(con, target, data, {
  reset = true, force = false, loaderCrc = STOCK_LOADER_CRC, progress = null,
} = {}) {
  if (!(target in COMMANDS)) throw new RangeError(target);
  const crc = crc32(data);
  const command = COMMANDS[target];
  await con.command("hb off", ["heartbeat"]);
  await con.command("fwinfo", ["fw:"]);

  // Mapping check: flash at 0x60010400 must hold the stock vendor loader.
  if ((target === "recovery" || target === "block0") &&
      crc32(data.subarray(LOADER_OFF, LOADER_END)) !== loaderCrc) {
    throw new CommunicationError("image does not carry the stock vendor loader; refusing");
  }
  const line = await con.command(`crc ${pyHex(FLASH_BLOCK0 + LOADER_OFF)} ${LOADER_END - LOADER_OFF}`, [" = "]);
  const have = parseInt(line.slice(line.lastIndexOf("=") + 1).trim(), 16) >>> 0;
  if (have !== loaderCrc && !force) {
    throw new CommunicationError(
      `mapping check failed: flash ${have.toString(16).padStart(8, "0")} != stock ${loaderCrc.toString(16).padStart(8, "0")}`,
    );
  }

  if (!(await con.command("fwtest", ["fw test"])).includes("ok")) {
    throw new CommunicationError("flash write-enable probe failed; nothing erased");
  }
  const ready = await con.command(`${command} ${data.length} ${pyHex(crc)}`,
    ["fw ready", "FAILED", "bad length", "no FCB", "unsupported", "write-protected", "unknown command"],
    60000);
  if (!ready.includes("fw ready")) {
    throw new CommunicationError(`${command} refused (${ready}); nothing was streamed`);
  }
  const t0 = now();
  for (let off = 0; off < data.length; off += 4096) {
    await con.write(data.subarray(off, off + 4096));
    if (progress) progress(Math.min(off + 4096, data.length), data.length);
  }
  const result = await con.expect(["fw done", "FAILED", "aborted"], 60000);
  const seconds = (now() - t0) / 1000;
  if (!result.endsWith("ok")) {
    if (target === "app") {
      throw new CommunicationError("flash FAILED: app slot invalid; recovery keeps the console, retry");
    }
    if (target === "stock") {
      throw new CommunicationError("flash FAILED: stock data invalid (the sound passes through); retry");
    }
    throw new CommunicationError("flash FAILED: recovery/block 0 invalid; recover with A+D");
  }
  if (reset) await con.write("reset\r");
  return { target, size: data.length, crc, seconds };
}

/**
 * The `stock` console command (app only): "stock data: flash ok, in use".
 * Returns { flashOk, line }. Recovery does not know the command.
 */
export async function stockStatus(con) {
  // "stock data: flash" only: the app also prints "stock data: ok" at boot.
  const line = await con.command("stock", ["stock data: flash", "unknown command"]);
  return { flashOk: line.includes("stock data: flash ok"), line };
}

// ---- Web Serial helpers ---------------------------------------------------

export function isConsolePort(port) {
  return port.getInfo().usbVendorId === VID;
}

/** Show the browser's port picker, filtered to the pedal (USB VID 0x34DB). */
export async function requestConsolePort() {
  try {
    return await navigator.serial.requestPort({ filters: [{ usbVendorId: VID }] });
  } catch (err) {
    throw new DeviceNotFoundError(err.name === "NotFoundError" ? "No pedal was chosen." : err.message);
  }
}

export async function openConsole(port, options = {}) {
  try {
    await port.open({ baudRate: BAUD_RATE });
  } catch (err) {
    throw new CommunicationError(`cannot open the pedal's USB console: ${err.message}`);
  }
  return new Console(port, options);
}

/**
 * After a `reset` the pedal leaves USB and comes back as a new port. Chrome
 * keeps the permission, so wait for it in getPorts() and open it. Returns
 * null on timeout: then ask the user to press Connect.
 */
export async function waitForConsole(oldPort, { timeoutMs = 20000, options = {} } = {}) {
  const deadline = now() + timeoutMs;
  await sleep(1500);                        // let the old device leave the bus
  while (now() < deadline) {
    const ports = (await navigator.serial.getPorts()).filter(isConsolePort);
    for (const port of ports) {
      if (port === oldPort && port.readable) continue;
      try {
        return await openConsole(port, options);
      } catch {
        /* still enumerating */
      }
    }
    await sleep(500);
  }
  return null;
}
