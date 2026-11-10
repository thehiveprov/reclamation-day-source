// The client's VersionDb::LoadFallout4 pairs each id in version-1-10-163-0.bin with the PREVIOUS entry's address
// ("CommonLibF4 ids + 1"). usage: node clientid.js rva <hex>...   -> the id to use in POINTER_FALLOUT4
//                                 node clientid.js id <dec>...    -> the RVA a POINTER_FALLOUT4 id resolves to
const fs = require('fs');
const lib = fs.readFileSync(String.raw`F:\SteamLibrary\steamapps\common\Fallout 4\Data\F4SE\Plugins\version-1-10-163-0.bin`);
const n = Math.floor((lib.length - 8) / 16);
const clientIdByRva = new Map(), rvaByClientId = new Map();
// The client reads (address, id) pairs starting at offset 0
for (let i = 0; i < n; i++) {
  const addr = Number(lib.readBigUInt64LE(i * 16));
  const id = Number(lib.readBigUInt64LE(i * 16 + 8));
  rvaByClientId.set(id, addr);
  if (!clientIdByRva.has(addr)) clientIdByRva.set(addr, id);
}
const [mode, ...args] = process.argv.slice(2);
for (const a of args) {
  if (mode === 'rva') { const r = parseInt(a, 16); console.log(a, clientIdByRva.has(r) ? 'client id ' + clientIdByRva.get(r) : 'no id'); }
  else { const v = rvaByClientId.get(+a); console.log(a, v === undefined ? 'no rva' : '0x' + v.toString(16)); }
}
