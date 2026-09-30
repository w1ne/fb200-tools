// Copyright (C) 2026 Andrii Shylenko
//
// This software is released under the MIT License.
// See the LICENSE file in the project root for full license information.

// The updater page (flash.html): a step-by-step wizard over
//   vendor-hid.js     the pedal's own update mode (A+D, WebHID)
//   serial-console.js the open firmware's USB console (Web Serial)
//   py-images.js      images.py + stockdata.py in Pyodide (first install)

import { crc32, hex8 } from "./crc32.js";
import { CommunicationError, DeviceNotFoundError, FirmwareError } from "./errors.js";
import { FirmwareUpdater, MrFile, WebHidTransport, buildFlashPlan, requestUpdateDevice } from "./vendor-hid.js";
import {
  LOADER_END, LOADER_OFF, STOCK_LOADER_CRC, openConsole, requestConsolePort, stockStatus, update,
  waitForConsole,
} from "./serial-console.js";

const $ = (id) => document.getElementById(id);

// ---- log ------------------------------------------------------------------

const logLines = [];
function log(line) {
  const t = new Date().toLocaleTimeString([], { hour12: false });
  logLines.push(`${t}  ${line}`);
  if (logLines.length > 3000) logLines.splice(0, logLines.length - 3000);
  const pre = $("log-text");
  const atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 4;
  pre.textContent = logLines.join("\n");
  if (atEnd) pre.scrollTop = pre.scrollHeight;
}
const pedalLog = (line) => log(`pedal: ${line}`);

// ---- status and errors ----------------------------------------------------

function setStatus(id, kind, text) {
  const el = $(id);
  el.className = `status${kind ? " " + kind : ""}`;
  el.textContent = text;
}

function showError(id, err, next) {
  log(`error: ${err.message}`);
  const el = $(id);
  el.className = "status err";
  el.replaceChildren();
  const what = document.createElement("strong");
  what.textContent = err.message;
  el.append(what, document.createTextNode(next));
}

const NEXT = {
  hidPick: "Unplug the pedal. Hold A and D, plug it in, then press Connect. If the list is empty, the pedal is not in update mode.",
  hid: "The pedal did not answer. Unplug it, hold A and D, plug it in again, and press Connect. It is safe to write again.",
  serialPick: "Press Connect and choose the FB200 port in the list. If the list is empty, unplug the pedal, plug it in again and wait 5 seconds.",
  serial: "The pedal did not answer. Unplug it, plug it in again, wait 5 seconds, and press Connect. If a write was cut off, the recovery stage keeps USB working: write again.",
  file: "Choose the official FB200 firmware file from FLAMMA (FB200_V1.0.1.mr).",
};

function nextStep(err, kind) {
  if (err instanceof DeviceNotFoundError) return NEXT[`${kind}Pick`];
  if (err instanceof FirmwareError) return NEXT.file;
  if (err.name === "SecurityError") return "The browser blocked USB access. Allow it in the site settings and try again.";
  if (err.name === "InvalidStateError" || /already open|Failed to open/.test(err.message)) {
    return "Another program uses the pedal. Close it (a serial terminal, the fb200 tool, FLAMMA's app), then press Connect.";
  }
  return NEXT[kind];
}

// ---- one write at a time, wake lock, do-not-unplug ------------------------

let busy = false;
let wakeLock = null;
const flashButtons = () => document.querySelectorAll("button[data-flash]");

function onBeforeUnload(e) {
  e.preventDefault();
  e.returnValue = "";
}

async function writing(on) {
  $("dont-unplug").hidden = !on;
  if (on) {
    window.addEventListener("beforeunload", onBeforeUnload);
    try {
      wakeLock = (await navigator.wakeLock?.request("screen")) ?? null;
    } catch {
      wakeLock = null;
    }
  } else {
    window.removeEventListener("beforeunload", onBeforeUnload);
    try { await wakeLock?.release(); } catch { /* released */ }
    wakeLock = null;
  }
}

/** Run one pedal task. A second click while one runs does nothing. */
async function exclusive(fn) {
  if (busy) return;
  busy = true;
  for (const b of flashButtons()) b.disabled = true;
  try {
    await fn();
  } finally {
    await writing(false);
    busy = false;
    for (const b of flashButtons()) b.disabled = b.dataset.locked === "1";
  }
}

document.addEventListener("visibilitychange", async () => {
  if (busy && !wakeLock && document.visibilityState === "visible") {
    try { wakeLock = await navigator.wakeLock?.request("screen"); } catch { /* optional */ }
  }
});

function progress(prefix) {
  $(`${prefix}-progress`).hidden = false;
  return (done, total) => {
    $(`${prefix}-bar`).value = done / total;
    $(`${prefix}-pct`).textContent = `${Math.floor((100 * done) / total)}%`;
  };
}

function setProgressLabel(prefix, text) {
  $(`${prefix}-label`).textContent = text;
  $(`${prefix}-bar`).value = 0;
  $(`${prefix}-pct`).textContent = "0%";
}

// ---- steps ------------------------------------------------------------------

function show(id) {
  $(id).hidden = false;
}

function activate(id) {
  for (const s of document.querySelectorAll(".step")) s.classList.toggle("active", s.id === id);
}

function done(id) {
  $(id).classList.add("done");
  $(id).classList.remove("active");
}

function finish() {
  show("s-done");
  activate("s-done");
  $("s-done").scrollIntoView({ behavior: "smooth", block: "start" });
}

// ---- pedal illustration -----------------------------------------------------

/** The pedal seen from above: display, 16 knobs, footswitches A-D, USB cable. */
function pedalSvg(mode) {
  const hold = mode === "hold";
  const display = mode === "done" ? "P0C" : "";
  const sw = ["A", "B", "C", "D"].map((name, i) => {
    const x = 80 + i * 93.3;
    const held = hold && (name === "A" || name === "D");
    return `<g>${held ? `<circle class="pd-ring" cx="${x}" cy="188" r="25"/>` +
      `<path class="pd-arrow" d="M${x} 147 v12 m-7 -7 l7 7 l7 -7"/>` : ""}
      <circle class="pd-sw${held ? " hold" : ""}" cx="${x}" cy="188" r="16"/>
      <text class="pd-label" x="${x}" y="226">${name}</text></g>`;
  }).join("");
  const knobs = Array.from({ length: 16 }, (_, i) => {
    const x = 70 + (i % 8) * 43;
    const y = i < 8 ? 110 : 135;
    return `<circle class="pd-knob" cx="${x}" cy="${y}" r="8"/>`;
  }).join("");
  const plugY = hold ? 2 : 18;
  const cable = `<path class="pd-cable" d="M390 ${plugY} C 390 ${plugY - 30}, 430 ${plugY - 10}, 436 ${plugY - 40}"/>
    <rect class="pd-plug" x="381" y="${plugY}" width="18" height="${hold ? 16 : 22}" rx="3"/>
    ${hold ? '<path class="pd-arrow" d="M372 8 v20 m-6 -6 l6 6 l6 -6"/>' : ""}`;
  const caption = {
    hold: "Hold A + D, then plug in USB",
    usb: "Plugged in: no buttons needed",
    done: "The open firmware runs",
  }[mode];
  const label = {
    hold: "The pedal with footswitches A and D highlighted: hold them while you plug in the USB cable.",
    usb: "The pedal with the USB cable plugged in. No footswitch is held.",
    done: "The pedal display shows P0C, the current preset.",
  }[mode];
  return `<svg viewBox="0 -44 440 316" role="img" aria-label="${label}" xmlns="http://www.w3.org/2000/svg">
    <rect class="pd-body" x="20" y="40" width="400" height="200" rx="14"/>
    <rect class="pd-lcd" x="175" y="56" width="90" height="36" rx="4"/>
    <text class="pd-lcd-text" x="220" y="83">${display}</text>
    ${knobs}${sw}${cable}
    <text class="pd-caption" x="220" y="262">${caption}</text>
  </svg>`;
}

// ---- 0. browser -------------------------------------------------------------

function isLinux() {
  const platform = navigator.userAgentData?.platform ?? navigator.platform ?? "";
  return /Linux/i.test(platform) && !/Android|CrOS/i.test(navigator.userAgent);
}

function checkBrowser() {
  if (isLinux()) $("linux-help").hidden = false;
  if (!window.isSecureContext) {
    setStatus("browser-status", "err", "Open this page over https (or from localhost). USB access needs a secure page.");
    return false;
  }
  const hid = "hid" in navigator;
  const serial = "serial" in navigator;
  if (!hid || !serial) {
    setStatus("browser-status", "err",
      "Use Chrome or Edge on a computer (Windows, macOS, Linux or ChromeOS). " +
      `This browser has no ${[!hid && "WebHID", !serial && "Web Serial"].filter(Boolean).join(" and ")}. ` +
      "Phones, tablets, Firefox and Safari cannot talk to the pedal.");
    return false;
  }
  setStatus("browser-status", "ok", "This browser can talk to the pedal (WebHID and Web Serial).");
  done("s-browser");
  return true;
}

// ---- 1. firmware ------------------------------------------------------------

const fw = { version: null, app: null, recovery: null, stockDataVersion: null };

async function fetchChecked(entry, base) {
  const url = new URL(entry.file, base);
  const res = await fetch(url, { cache: "no-cache" });
  if (!res.ok) throw new Error(`cannot download ${entry.file} (HTTP ${res.status})`);
  const data = new Uint8Array(await res.arrayBuffer());
  if (data.length !== entry.size) {
    throw new Error(`${entry.file}: size ${data.length}, expected ${entry.size}`);
  }
  const crc = hex8(crc32(data));
  if (crc !== String(entry.crc32).toLowerCase()) {
    throw new Error(`${entry.file}: CRC-32 ${crc}, expected ${entry.crc32}`);
  }
  log(`${entry.file}: ${data.length} bytes, CRC-32 ${crc} ok`);
  return data;
}

/** The header of an app slot (images.build_slot): magic "FBAP", version 2. */
function checkSlotHeader(slot) {
  const v = new DataView(slot.buffer, slot.byteOffset, slot.byteLength);
  if (slot.length < 0x500 || v.getUint32(0, true) !== 0x50414246 || v.getUint32(12, true) !== 2) {
    throw new Error("the published app image is not an FB200 app slot");
  }
}

async function loadFirmware() {
  show("s-firmware");
  activate("s-firmware");
  $("firmware-retry-row").hidden = true;
  setStatus("firmware-status", "", "Loading…");
  try {
    const base = new URL("firmware/", document.baseURI);
    const res = await fetch(new URL("manifest.json", base), { cache: "no-cache" });
    if (!res.ok) throw new Error(`cannot download firmware/manifest.json (HTTP ${res.status})`);
    const manifest = await res.json();
    fw.app = await fetchChecked(manifest.app, base);
    fw.recovery = await fetchChecked(manifest.recovery, base);
    checkSlotHeader(fw.app);
    fw.version = manifest.version;
    fw.stockDataVersion = manifest.stock_data_version;
    setStatus("firmware-status", "ok",
      `Firmware ${fw.version} is ready (size and CRC-32 checked).`);
    done("s-firmware");
    show("s-path");
    activate("s-path");
  } catch (err) {
    log(`error: ${err.message}`);
    setStatus("firmware-status", "err", `${err.message}. Check your connection and try again.`);
    $("firmware-retry-row").hidden = false;
  }
}

// ---- 2. path ----------------------------------------------------------------

const PATHS = { install: "s-install", update: "s-update", stock: "s-stock" };

function choosePath(path) {
  if (busy) return;
  for (const [name, step] of Object.entries(PATHS)) {
    $(`path-${name}`).setAttribute("aria-pressed", String(name === path));
    $(step).hidden = name !== path;
  }
  $("s-done").hidden = true;
  activate(PATHS[path]);
  $(PATHS[path]).scrollIntoView({ behavior: "smooth", block: "start" });
}

// ---- stock .mr (user's file) ------------------------------------------------

/** Quick checks in JS before Python runs: FB200, two blocks, the V1.0.1 loader. */
async function readStockMr(file) {
  const data = new Uint8Array(await file.arrayBuffer());
  log(`${file.name}: ${data.length} bytes (stays on this computer)`);
  const mr = MrFile.fromBytes(data);
  if (mr.header.productTag !== "FB200" || mr.blocks.length < 2) {
    throw new FirmwareError(`this is not the FB200 firmware (product "${mr.header.productTag}")`);
  }
  const loader = crc32(mr.blocks[0].data.subarray(LOADER_OFF, LOADER_END));
  if (loader !== STOCK_LOADER_CRC) {
    throw new FirmwareError(`this is not the FB200 V1.0.1 firmware (loader CRC ${hex8(loader)})`);
  }
  return data;
}

function checkBlobVersion(blob) {
  const version = new DataView(blob.buffer, blob.byteOffset).getUint32(4, true);
  if (fw.stockDataVersion != null && version !== fw.stockDataVersion) {
    throw new FirmwareError(`sound data version ${version}, the firmware expects ${fw.stockDataVersion}`);
  }
}

// ---- 3. first install -------------------------------------------------------

const install = { mr: null, blob: null };

async function installPickMr() {
  const file = $("mr-file").files[0];
  if (!file || busy) return;
  install.mr = install.blob = null;
  $("fi-hid").hidden = true;
  busy = true;
  try {
    setStatus("mr-status", "", "Checking the file…");
    const stock = await readStockMr(file);
    setStatus("mr-status", "", "Building the install image. The first time this loads Python into the page (about 10 MB)…");
    const { buildFirstInstall } = await import("./py-images.js");
    const { mr, blob } = await buildFirstInstall(stock, fw.recovery, fw.app, log);
    checkBlobVersion(blob);
    const parsed = MrFile.fromBytes(mr);
    const plan = buildFlashPlan(parsed);
    install.mr = parsed;
    install.blob = blob;
    log(`install image: ${plan.writeCount} pages, ${plan.totalBytes} bytes`);
    setStatus("mr-status", "ok", `Ready: firmware ${fw.version} with the sound data from ${file.name}.`);
    show("fi-hid");
    $("fi-hid").scrollIntoView({ behavior: "smooth", block: "start" });
  } catch (err) {
    showError("mr-status", err, err instanceof FirmwareError ? NEXT.file :
      "Could not load Python from cdn.jsdelivr.net. Check your connection and choose the file again.");
  } finally {
    busy = false;
  }
}

async function installHid() {
  await exclusive(async () => {
    let transport = null;
    try {
      setStatus("hid-status", "", "Choose the pedal in the browser's list…");
      const device = await requestUpdateDevice();
      log(`update device: ${device.productName || "(no name)"} ${hex4(device.vendorId)}:${hex4(device.productId)}`);
      transport = await new WebHidTransport(device).open();
      await writing(true);
      setStatus("hid-status", "", "Erasing. This takes about 15 seconds…");
      const onProgress = progress("hid");
      const result = await new FirmwareUpdater(transport).flash(install.mr, {
        log,
        progress: (d, t) => {
          if (d === 1) setStatus("hid-status", "", "Writing…");
          onProgress(d, t);
        },
      });
      log(`wrote ${result.bytesWritten} bytes in ${result.writeFrames} pages`);
      setStatus("hid-status", "ok",
        "Written. The pedal restarts into the recovery stage. It does not come back as the stock device: that is expected.");
      $("hid-connect").dataset.locked = "1";
      show("fi-app");
      $("fi-app").scrollIntoView({ behavior: "smooth", block: "start" });
    } catch (err) {
      showError("hid-status", err, nextStep(err, "hid"));
    } finally {
      await transport?.close();
    }
  });
}

/**
 * Recovery keeps the console but has no app yet. Write the sound data first
 * (fwstock, no reset), then the app (fwbegin), then reset: one connection.
 */
async function installSerial() {
  await exclusive(async () => {
    let con = null;
    try {
      const port = await requestConsolePort();
      con = await openConsole(port, { echo: pedalLog });
      log("USB console open");
      await writing(true);
      setProgressLabel("app", "Writing the sound data (1 of 2)");
      setStatus("app-status", "", "Writing the sound data…");
      let res = await update(con, "stock", install.blob, { reset: false, progress: progress("app") });
      log(`stock: ${res.size} bytes, CRC ${hex8(res.crc)} ok`);
      setProgressLabel("app", "Writing the firmware (2 of 2)");
      setStatus("app-status", "", "Writing the firmware…");
      res = await update(con, "app", fw.app, { progress: progress("app") });
      log(`app: ${res.size} bytes, CRC ${hex8(res.crc)} ok, ${res.seconds.toFixed(1)} s`);
      setStatus("app-status", "ok", "Sound data and firmware written. The pedal restarts.");
      $("app-connect").dataset.locked = "1";
      done("s-install");
      finish();
    } catch (err) {
      showError("app-status", err, nextStep(err, "serial"));
    } finally {
      await con?.close();
    }
  });
}

// ---- 4. update --------------------------------------------------------------

const upd = { blob: null };

async function updateApp() {
  await exclusive(async () => {
    let con = null;
    try {
      const port = await requestConsolePort();
      con = await openConsole(port, { echo: pedalLog });
      log("USB console open");
      await writing(true);
      setStatus("up-status", "", "Writing the firmware…");
      setProgressLabel("up", "Writing the firmware");
      const res = await update(con, "app", fw.app, { progress: progress("up") });
      log(`app: ${res.size} bytes, CRC ${hex8(res.crc)} ok, ${res.seconds.toFixed(1)} s`);
      await con.close();
      con = null;
      await writing(false);
      setStatus("up-status", "", "Firmware written. The pedal restarts. Checking the sound data…");
      con = await waitForConsole(port, { options: { echo: pedalLog } });
      if (!con) {
        setStatus("up-status", "ok",
          `Firmware ${fw.version} written. The pedal restarted. If it has no amp and cab sound, check the sound data below.`);
        show("up-stock");
        return;
      }
      await checkStock(con);
    } catch (err) {
      showError("up-status", err, nextStep(err, "serial"));
    } finally {
      await con?.close();
    }
  });
}

/** Ask the running app for its sound data. Offer the .mr picker if it is not ok. */
async function checkStock(con) {
  const { flashOk, line } = await stockStatus(con);
  log(line);
  if (flashOk) {
    setStatus("up-status", "ok", `Firmware ${fw.version} written. The sound data is ok.`);
    $("up-stock").hidden = true;
    done("s-update");
    finish();
  } else {
    setStatus("up-status", "err",
      /unknown command/.test(line)
        ? "The pedal is in the recovery stage. Write the firmware again (press Connect)."
        : "Firmware written, but the pedal has no sound data. Choose the official .mr file below to add it.");
    show("up-stock");
  }
}

async function updateCheckStock() {
  await exclusive(async () => {
    let con = null;
    try {
      con = await openConsole(await requestConsolePort(), { echo: pedalLog });
      await checkStock(con);
      setStatus("up-stock-status", "", "");
    } catch (err) {
      showError("up-stock-status", err, nextStep(err, "serial"));
    } finally {
      await con?.close();
    }
  });
}

async function updatePickMr() {
  const file = $("up-mr-file").files[0];
  if (!file || busy) return;
  upd.blob = null;
  $("up-stock-write").disabled = true;
  $("up-stock-write").dataset.locked = "1";
  busy = true;
  try {
    setStatus("up-stock-status", "", "Reading the sound data. The first time this loads Python into the page (about 10 MB)…");
    const stock = await readStockMr(file);
    const { buildStockBlob } = await import("./py-images.js");
    upd.blob = await buildStockBlob(stock, log);
    checkBlobVersion(upd.blob);
    setStatus("up-stock-status", "ok", `Sound data ready (${upd.blob.length} bytes). Press "Write the sound data".`);
    $("up-stock-write").dataset.locked = "0";
    $("up-stock-write").disabled = false;
  } catch (err) {
    showError("up-stock-status", err, err instanceof FirmwareError ? NEXT.file :
      "Could not load Python from cdn.jsdelivr.net. Check your connection and choose the file again.");
  } finally {
    busy = false;
  }
}

async function updateWriteStock() {
  await exclusive(async () => {
    let con = null;
    try {
      con = await openConsole(await requestConsolePort(), { echo: pedalLog });
      await writing(true);
      setProgressLabel("up", "Writing the sound data");
      const res = await update(con, "stock", upd.blob, { progress: progress("up") });
      log(`stock: ${res.size} bytes, CRC ${hex8(res.crc)} ok`);
      setStatus("up-stock-status", "ok", "Sound data written. The pedal restarts.");
      $("up-stock-write").dataset.locked = "1";
      done("s-update");
      finish();
    } catch (err) {
      showError("up-stock-status", err, nextStep(err, "serial"));
    } finally {
      await con?.close();
    }
  });
}

// ---- 5. back to the stock firmware -------------------------------------------

const back = { mr: null };

/** The official .mr, flashed unchanged (fb200 fw flash FB200.mr --no-jump). */
async function stockPickMr() {
  const file = $("st-file").files[0];
  back.mr = null;
  $("st-hid").hidden = true;
  if (!file || busy) return;
  try {
    const data = new Uint8Array(await file.arrayBuffer());
    const mr = MrFile.fromBytes(data);
    if (mr.header.productTag !== "FB200") {
      throw new FirmwareError(`refusing to flash an image for "${mr.header.productTag}" (expected FB200)`);
    }
    const plan = buildFlashPlan(mr);
    log(`${file.name}: ${data.length} bytes, ${mr.blocks.length} blocks, ${plan.writeCount} pages`);
    back.mr = mr;
    setStatus("st-file-status", "ok", `Ready: ${file.name} (FB200, ${plan.writeCount} pages).`);
    show("st-hid");
    $("st-hid").scrollIntoView({ behavior: "smooth", block: "start" });
  } catch (err) {
    showError("st-file-status", err, NEXT.file);
  }
}

async function stockHid() {
  await exclusive(async () => {
    let transport = null;
    try {
      setStatus("st-status", "", "Choose the pedal in the browser's list…");
      const device = await requestUpdateDevice();
      transport = await new WebHidTransport(device).open();
      await writing(true);
      setStatus("st-status", "", "Erasing. This takes about 15 seconds…");
      const onProgress = progress("st");
      const result = await new FirmwareUpdater(transport).flash(back.mr, {
        log,
        progress: (d, t) => {
          if (d === 1) setStatus("st-status", "", "Writing…");
          onProgress(d, t);
        },
      });
      log(`wrote ${result.bytesWritten} bytes in ${result.writeFrames} pages`);
      setStatus("st-status", "ok",
        "Done. The pedal restarts with the stock firmware. Your presets, settings and user IRs are still there. " +
        "To come back to the open firmware later, open this page and choose First install.");
      done("s-stock");
    } catch (err) {
      showError("st-status", err, nextStep(err, "hid"));
    } finally {
      await transport?.close();
    }
  });
}

const hex4 = (n) => n.toString(16).padStart(4, "0");

// ---- wiring -----------------------------------------------------------------

function init() {
  for (const slot of document.querySelectorAll(".svg-slot")) slot.innerHTML = pedalSvg(slot.dataset.pedal);
  for (const id of ["hid-connect", "st-connect", "app-connect", "up-connect", "up-stock-check", "up-stock-write"]) {
    $(id).dataset.flash = "";
  }
  $("up-stock-write").dataset.locked = "1";
  $("firmware-retry").addEventListener("click", loadFirmware);
  $("path-install").addEventListener("click", () => choosePath("install"));
  $("path-update").addEventListener("click", () => choosePath("update"));
  $("path-stock").addEventListener("click", () => choosePath("stock"));
  $("st-file").addEventListener("change", stockPickMr);
  $("st-connect").addEventListener("click", stockHid);
  $("mr-file").addEventListener("change", installPickMr);
  $("hid-connect").addEventListener("click", installHid);
  $("app-connect").addEventListener("click", installSerial);
  $("up-connect").addEventListener("click", updateApp);
  $("up-stock-check").addEventListener("click", updateCheckStock);
  $("up-mr-file").addEventListener("change", updatePickMr);
  $("up-stock-write").addEventListener("click", updateWriteStock);
  navigator.hid?.addEventListener("disconnect", (e) => log(`USB: ${e.device.productName || "HID device"} left`));
  navigator.serial?.addEventListener("disconnect", () => log("USB: the console port left"));
  navigator.serial?.addEventListener("connect", () => log("USB: a console port arrived"));
  log(`fb200-tools web updater; ${navigator.userAgent}`);
  if (checkBrowser()) loadFirmware();
}

init();
