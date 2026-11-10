// Lists / extracts files from a BA2 (GNRL) archive. Usage: node ba2.js archive.ba2 [regex] [outdir]
const fs = require('fs'), zlib = require('zlib'), path = require('path');
const [file, pattern, outDir] = process.argv.slice(2);
const d = fs.readFileSync(file);
if (d.toString('latin1', 0, 4) !== 'BTDX' || d.toString('latin1', 8, 12) !== 'GNRL') throw new Error('not a GNRL BA2');
const count = d.readUInt32LE(12), nameOff = Number(d.readBigUInt64LE(16));
const names = [];
let o = nameOff;
for (let i = 0; i < count; i++) { const l = d.readUInt16LE(o); names.push(d.toString('latin1', o + 2, o + 2 + l)); o += 2 + l; }
const re = pattern ? new RegExp(pattern, 'i') : null;
const backslash = String.fromCharCode(92);
for (let i = 0; i < count; i++) {
  const r = 24 + i * 36;
  const off = Number(d.readBigUInt64LE(r + 16)), packed = d.readUInt32LE(r + 24), size = d.readUInt32LE(r + 28);
  if (re && !re.test(names[i])) continue;
  if (!outDir) { console.log(names[i], size); continue; }
  let data = d.slice(off, off + (packed || size));
  if (packed) data = zlib.inflateSync(data);
  const out = path.join(outDir, names[i].split(backslash).join('/'));
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, data);
}
