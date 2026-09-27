import test from "node:test";
import assert from "node:assert/strict";
import { crc32, hex8 } from "../js/crc32.js";
import { fixture, fromHex, skipParity } from "./fixture.mjs";

const ascii = (s) => new TextEncoder().encode(s);

test("crc32 known vectors", () => {
  assert.equal(crc32(new Uint8Array(0)), 0);
  assert.equal(crc32(ascii("123456789")), 0xcbf43926);
  assert.equal(crc32(ascii("The quick brown fox jumps over the lazy dog")), 0x414fa339);
  assert.equal(hex8(0x0000abcd), "0000abcd");
});

test("crc32 continues across chunks", () => {
  const data = ascii("123456789");
  assert.equal(crc32(data.subarray(4), crc32(data.subarray(0, 4))), 0xcbf43926);
});

test("crc32 matches zlib.crc32", { skip: skipParity }, () => {
  for (const { data, crc } of fixture.crc32) assert.equal(crc32(fromHex(data)), crc);
});
