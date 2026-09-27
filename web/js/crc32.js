// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320): the same value as
// Python's zlib.crc32(data) & 0xFFFFFFFF and the firmware's crc32_ieee().

const TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** crc32(bytes[, crc]) -> unsigned 32-bit. Pass the last result as `crc` to continue. */
export function crc32(data, crc = 0) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  let c = (crc ^ 0xffffffff) >>> 0;
  for (let i = 0; i < bytes.length; i++) c = TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** 8 lowercase hex digits, as pack_images.py writes them to manifest.json. */
export function hex8(value) {
  return (value >>> 0).toString(16).padStart(8, "0");
}
