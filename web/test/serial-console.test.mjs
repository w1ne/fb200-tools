import test from "node:test";
import assert from "node:assert/strict";
import { crc32 } from "../js/crc32.js";
import { Console, LOADER_END, LOADER_OFF, stockStatus, update } from "../js/serial-console.js";
import { fixture, fromHex, skipParity } from "./fixture.mjs";

const STOCK = Uint8Array.from({ length: 2048 }, (_, i) => i & 0xff);   // stands in for block 0
const LOADER_CRC = crc32(STOCK.subarray(LOADER_OFF, LOADER_END));

/**
 * A fake pedal behind a Web Serial port: answers like
 * firmware/audio/src/debug/console.c (and FakePedal in tests/test_console.py).
 * The replies arrive in small pieces, to test line reassembly.
 */
class FakePedal {
  constructor({ corrupt = false, known = ["fwbegin", "fwrec", "fwstock"], loaderCrc = LOADER_CRC,
    stock = "ok", hang = false } = {}) {
    Object.assign(this, { corrupt, known, loaderCrc, stock, hang });
    this.log = [];
    this.written = new Uint8Array(0);
    this._line = "";
    this._fw = null;
    this.readable = new ReadableStream({ start: (c) => { this._out = c; } });
    this.writable = new WritableStream({ write: (chunk) => this._rx(chunk) });
  }

  say(text) {
    const bytes = new TextEncoder().encode(text.replaceAll("\n", "\r\n"));
    for (let i = 0; i < bytes.length; i += 5) this._out.enqueue(bytes.slice(i, i + 5));
  }

  async _rx(chunk) {
    if (this.hang) return new Promise(() => {});
    for (let i = 0; i < chunk.length; i++) {
      if (this._fw) {
        const n = Math.min(this._fw.len - this._fw.data.length, chunk.length - i);
        this._fw.data.push(...chunk.subarray(i, i + n));
        i += n - 1;
        if (this._fw.data.length === this._fw.len) this._finish();
        continue;
      }
      const c = String.fromCharCode(chunk[i]);
      if (c === "\r" || c === "\n") {
        this._dispatch(this._line.trim());
        this._line = "";
      } else {
        this._line += c;
      }
    }
  }

  _finish() {
    const data = Uint8Array.from(this._fw.data);
    this.written = data;
    let got = crc32(data);
    if (this.corrupt) got ^= 1;
    this.say(`fw done crc=${got.toString(16).padStart(8, "0")} ${got === this._fw.crc ? "ok" : "BAD"}\n`);
    this._fw = null;
  }

  _dispatch(cmd) {
    this.log.push(cmd);
    this.say(cmd + "\n");                       // echo, as the firmware does
    const argv = cmd.split(/\s+/).filter(Boolean);
    if (!argv.length) return;
    const [name] = argv;
    if (name === "hb") this.say("heartbeat off\n");
    else if (name === "fwinfo") this.say("fw: fcb=ok cmd-pads=0 addr=24 status=ok sr=00\n");
    else if (name === "crc") this.say(`crc ${argv[1]} ${argv[2]} = ${this.loaderCrc.toString(16).padStart(8, "0")}\n`);
    else if (name === "fwtest") this.say("fw test ok (sr=02)\n");
    else if (name === "stock") this.say(`stock data: flash ${this.stock}, not loaded (amp/cab/tone pass through, drums silent)\n`);
    else if (name === "reset") this.say("rebooting\n");
    else if (["fwbegin", "fwrec", "fwstock"].includes(name)) {
      if (!this.known.includes(name)) return this.say("unknown command (try help)\n");
      this.say("fw: erasing 0x60020000..0x60030000\nfw ready\n");
      this._fw = { len: Number(argv[1]), crc: parseInt(argv[2], 16) >>> 0, data: [] };
    } else this.say("unknown command (try help)\n");
  }
}

const random = (n) => Uint8Array.from({ length: n }, () => (Math.random() * 256) | 0);
const settle = () => new Promise((r) => setTimeout(r, 20));

test("run strips the echo", async () => {
  const pedal = new FakePedal();
  const con = new Console(pedal);
  assert.equal(await con.run("hb off", 50, 1000), "heartbeat off");
});

test("update app streams the image and resets", async () => {
  const pedal = new FakePedal();
  const lines = [];
  const con = new Console(pedal, { echo: (l) => lines.push(l) });
  const data = random(47000);
  const seen = [];
  const res = await update(con, "app", data, { loaderCrc: LOADER_CRC, progress: (d, t) => seen.push([d, t]) });
  await settle();
  assert.deepEqual(pedal.written, data);
  assert.equal(res.crc, crc32(data));
  assert.deepEqual(pedal.log, ["hb off", "fwinfo", "crc 0x60010400 900", "fwtest",
    `fwbegin ${data.length} 0x${res.crc.toString(16)}`, "reset"]);
  assert.deepEqual(seen.at(-1), [data.length, data.length]);
  assert.ok(lines.some((l) => l.startsWith("fw done")));
});

test("update stock uses fwstock", async () => {
  const pedal = new FakePedal();
  const data = random(59276);
  await update(new Console(pedal), "stock", data, { reset: false, loaderCrc: LOADER_CRC });
  assert.ok(pedal.log.some((c) => c.startsWith("fwstock ")));
  assert.ok(!pedal.log.includes("reset"));
  assert.deepEqual(pedal.written, data);
});

test("update recovery refuses a foreign loader", async () => {
  const pedal = new FakePedal();
  await assert.rejects(update(new Console(pedal), "recovery", random(4096), { loaderCrc: LOADER_CRC }),
    /vendor loader/);
  assert.ok(!pedal.log.some((c) => c.startsWith("fwrec")));
});

test("mapping check failure streams nothing", async () => {
  const pedal = new FakePedal({ loaderCrc: 0x12345678 });
  await assert.rejects(update(new Console(pedal), "app", random(100), { loaderCrc: LOADER_CRC }),
    /mapping check failed: flash 12345678 != stock/);
  assert.equal(pedal.written.length, 0);
});

test("update fails fast on an unknown command", async () => {
  const pedal = new FakePedal({ known: ["fwrec"] });
  await assert.rejects(update(new Console(pedal), "app", random(100), { loaderCrc: LOADER_CRC }), /refused/);
  assert.equal(pedal.written.length, 0);
});

test("update reports a CRC mismatch", async () => {
  const pedal = new FakePedal({ corrupt: true });
  await assert.rejects(update(new Console(pedal), "app", random(1000), { loaderCrc: LOADER_CRC }),
    /app slot invalid/);
});

test("a hung pedal times out the write", async () => {
  const con = new Console(new FakePedal({ hang: true }));
  await assert.rejects(con.write("hb off\r", 100), /stopped reading USB/);
});

test("a silent pedal times out expect", async () => {
  const con = new Console(new FakePedal());
  await assert.rejects(con.expect(["never"], 100), /timeout waiting/);
});

test("stockStatus reads the stock command", async () => {
  assert.equal((await stockStatus(new Console(new FakePedal()))).flashOk, true);
  assert.equal((await stockStatus(new Console(new FakePedal({ stock: "missing" })))).flashOk, false);
});

test("first install: stock data, then the app, one reset", async () => {
  const pedal = new FakePedal();
  const con = new Console(pedal);
  const blob = random(700);
  const app = random(5000);
  await update(con, "stock", blob, { reset: false, loaderCrc: LOADER_CRC });
  assert.deepEqual(pedal.written, blob);
  await update(con, "app", app, { loaderCrc: LOADER_CRC });
  await settle();
  assert.deepEqual(pedal.written, app);
  assert.deepEqual(pedal.log.filter((c) => /^(fwstock|fwbegin|reset)/.test(c)).map((c) => c.split(" ")[0]),
    ["fwstock", "fwbegin", "reset"]);
});

test("the dialogue matches console.update()", { skip: skipParity }, async () => {
  for (const c of fixture.console) {
    const pedal = new FakePedal({ loaderCrc: c.loader_crc });
    const con = new Console(pedal);
    for (const step of c.steps) {
      const data = fromHex(step.data);
      const res = await update(con, step.target, data, { reset: step.reset, loaderCrc: c.loader_crc });
      assert.equal(res.crc, step.crc);
      assert.deepEqual(pedal.written, data);
    }
    await settle();
    assert.deepEqual(pedal.log, c.log, c.steps.map((s) => s.target).join("+"));
  }
});

test("stockStatus ignores the app's boot line", async () => {
  const pedal = new FakePedal();
  pedal.say("stock data: missing\n");            // main.c prints this at boot
  assert.equal((await stockStatus(new Console(pedal))).flashOk, true);
});
