// The parity fixture that tests/test_web_parity.py builds with the Python
// protocol code. Without it (plain `node --test web/test/`) the parity
// tests are skipped; the Python test fails on any skip.
import { readFileSync } from "node:fs";

const path = process.env.FB200_PARITY_FIXTURE;
export const fixture = path ? JSON.parse(readFileSync(path, "utf8")) : null;
export const skipParity = fixture ? false : "no FB200_PARITY_FIXTURE (run tests/test_web_parity.py)";

export const fromHex = (h) => Uint8Array.from(h.match(/../g) ?? [], (x) => parseInt(x, 16));
export const toHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
