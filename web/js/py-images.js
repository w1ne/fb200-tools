// Copyright (C) 2026 Andrii Shylenko
//
// This software is released under the MIT License.
// See the LICENSE file in the project root for full license information.

// Runs src/fb200/images.py and stockdata.py in Pyodide: the same code as the
// command-line tool builds the first-install .mr and the stock sound-data
// blob from the user's own stock .mr. Loaded only when needed.
// The deploy copies the modules to py/fb200/ (see .github/workflows/release.yml).

import { FirmwareError } from "./errors.js";

export const PYODIDE_VERSION = "0.28.3";
const PYODIDE_URL = `https://cdn.jsdelivr.net/pyodide/v${PYODIDE_VERSION}/full/`;
const PY_FILES = ["__init__.py", "errors.py", "firmware.py", "images.py", "stockdata.py"];
const PY_ROOT = "/fb200lib";

let pyodide = null;

export function loadPython(log = () => {}) {
  pyodide ??= (async () => {
    log(`loading Python (Pyodide ${PYODIDE_VERSION})`);
    const { loadPyodide } = await import(`${PYODIDE_URL}pyodide.mjs`);
    const py = await loadPyodide({ indexURL: PYODIDE_URL });
    py.FS.mkdirTree(`${PY_ROOT}/fb200`);
    for (const name of PY_FILES) {
      const res = await fetch(new URL(`../py/fb200/${name}`, import.meta.url), { cache: "no-cache" });
      if (!res.ok) throw new Error(`cannot load py/fb200/${name} (HTTP ${res.status})`);
      py.FS.writeFile(`${PY_ROOT}/fb200/${name}`, await res.text());
    }
    py.runPython(`import sys; sys.path.insert(0, "${PY_ROOT}")\nimport fb200.images, fb200.stockdata`);
    log("Python ready");
    return py;
  })();
  pyodide.catch(() => { pyodide = null; });   // allow a retry after a network error
  return pyodide;
}

/** Turn a Python exception into a FirmwareError with the last line of the traceback. */
function pyError(err) {
  const lines = String(err.message || err).trim().split("\n");
  const last = lines[lines.length - 1].replace(/^fb200\.errors\.|^\w+Error: /, "");
  return new FirmwareError(last.replace(/^FirmwareError: /, ""));
}

function call(py, code, args) {
  const ns = py.toPy({});
  try {
    for (const [k, v] of Object.entries(args)) ns.set(k, v);
    const result = py.runPython(code, { globals: ns });
    const buf = result.getBuffer("u8");
    const bytes = buf.data.slice();
    buf.release();
    result.destroy();
    return bytes;
  } catch (err) {
    throw pyError(err);
  } finally {
    ns.destroy();
  }
}

/**
 * images.twostage_mr() and stockdata.build() for the first install.
 * Returns { mr, blob } as Uint8Arrays.
 */
export async function buildFirstInstall(stockMr, recovery, slot, log = () => {}) {
  const py = await loadPython(log);
  const mr = call(py, `
from fb200 import images
images.check_slot(slot.to_bytes())
images.twostage_mr(stock.to_bytes(), recovery.to_bytes(), slot.to_bytes())
`, { stock: stockMr, recovery, slot });
  log(`first-install image: ${mr.length} bytes`);
  const blob = await buildStockBlob(stockMr, log);
  return { mr, blob };
}

/** stockdata.build() + stockdata.verify(): the sound data for `fwstock`. */
export async function buildStockBlob(stockMr, log = () => {}) {
  const py = await loadPython(log);
  const blob = call(py, `
from fb200 import stockdata
blob = stockdata.build(stock.to_bytes())
stockdata.verify(blob)
blob
`, { stock: stockMr });
  log(`stock sound data: ${blob.length} bytes`);
  return blob;
}
