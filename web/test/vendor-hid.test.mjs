import test from "node:test";
import assert from "node:assert/strict";
import {
  FirmwareUpdater, FrameReader, MrFile, buildFlashPlan, crc16, iterReports, packFrame,
} from "../js/vendor-hid.js";
import { fixture, fromHex, skipParity, toHex } from "./fixture.mjs";

/** transport.py MockTransport: records writes, answers from a queue. */
class MockTransport {
  constructor(reports = []) {
    this.written = [];
    this._reports = [...reports];
  }
  async writeReport(report) {
    this.written.push(Uint8Array.from(report));
  }
  async readReport() {
    return this._reports.length ? this._reports.shift() : null;
  }
}

const replyReport = (fn) => [...iterReports(packFrame(fn))][0];

test("frame and report layout from docs/PROTOCOL.md", () => {
  const frame = packFrame(0x00);
  assert.equal(toHex(frame), "aa55010000c8cf");
  const reports = [...iterReports(frame)];
  assert.equal(reports.length, 1);
  assert.equal(reports[0].length, 64);
  assert.equal(toHex(reports[0].subarray(0, 8)), "07aa55010000c8cf");
  assert.ok(reports[0].subarray(8).every((b) => b === 0));
  assert.equal(toHex(crc16(fromHex("010000"))), "c8cf");
});

test("long frames split into 63-byte chunks", () => {
  const frame = packFrame(0x04, new Uint8Array(514));   // 521-byte frame
  const reports = [...iterReports(frame)];
  assert.equal(reports.length, Math.ceil(frame.length / 63));
  assert.equal(reports.at(-1)[0], frame.length % 63 || 63);
  const joined = reports.flatMap((r) => [...r.subarray(1, 1 + r[0])]);
  assert.deepEqual(Uint8Array.from(joined), frame);
});

test("FrameReader resyncs past garbage and bad CRCs", () => {
  const reader = new FrameReader();
  const good = packFrame(0x05, Uint8Array.of(1, 2, 3));
  const bad = Uint8Array.from(packFrame(0x07));
  bad[bad.length - 1] ^= 0xff;
  const stream = Uint8Array.of(0x11, 0xaa, ...bad, ...good);
  for (const report of iterReports(stream)) reader.feedReport(report);
  assert.deepEqual(reader.nextPacket(), Uint8Array.of(0x05, 1, 2, 3));
  assert.equal(reader.nextPacket(), null);
});

test("flash fails on a wrong erase reply", { skip: skipParity }, async () => {
  const mr = MrFile.fromBytes(fromHex(fixture.vendor[0].mr));
  const updater = new FirmwareUpdater(new MockTransport([replyReport(0x09)]));
  await assert.rejects(updater.flash(mr), /erase command not acknowledged \(got 0x09\)/);
}).skip = undefined;

for (const [i, name] of [[0, "header v0 (UPDATE_ADDR erase)"], [1, "header v1 (per-block erase list)"]]) {
  test(`flash writes the same reports as updater.py: ${name}`, { skip: skipParity }, async () => {
    const c = fixture.vendor[i];
    const mr = MrFile.fromBytes(fromHex(c.mr));
    const transport = new MockTransport(c.replies.map(replyReport));
    const seen = [];
    const result = await new FirmwareUpdater(transport).flash(mr, {
      progress: (done, total) => seen.push([done, total]),
    });
    assert.equal(result.writeFrames, c.write_frames);
    assert.equal(result.bytesWritten, c.bytes_written);
    assert.equal(transport.written.length, c.reports.length);
    transport.written.forEach((r, j) => assert.equal(toHex(r), c.reports[j], `report ${j}`));
    assert.deepEqual(seen.at(-1), [c.write_frames, c.write_frames]);
  });
}

test("flash stops on a missing page reply", { skip: skipParity }, async () => {
  const c = fixture.vendor[0];
  const mr = MrFile.fromBytes(fromHex(c.mr));
  const transport = new MockTransport(c.replies.slice(0, 2).map(replyReport));
  await assert.rejects(new FirmwareUpdater(transport).flash(mr, { timeoutMs: 50 }),
    /unexpected reply while writing page 2/);
});

test("MrFile rejects foreign files", () => {
  assert.throws(() => MrFile.fromBytes(new Uint8Array(10)), /too small/);
  assert.throws(() => MrFile.fromBytes(new Uint8Array(128)), /bad header tag/);
});

test("page range check", { skip: skipParity }, () => {
  const mr = MrFile.fromBytes(fromHex(fixture.vendor[0].mr));
  mr.blocks[1].tag.startPage = 0xffff;
  assert.throws(() => buildFlashPlan(mr), /16-bit page/);
});
