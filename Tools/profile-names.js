// Names the game's functions in the sampler's "Profile" log lines (RD_PROFILE, Code/client/PerfSampler.cpp).
// usage: node profile-names.js <tp_client.log or a run's probe log> [--game "<Fallout 4 folder>"] [--commonlib <CommonLibF4 folder>]
// Each game+<offset> becomes game+<offset>[<id> <name>]: <id> is the address library id as CommonLibF4 counts it (ours is
// one more, VersionDb::LoadFallout4), the name from CommonLibF4's headers or our own code where one of them uses the id.
// An offset marked @ is an address inside a function with no unwind entry (a leaf): it gets the nearest id below it, +off.
// A function no header names but a vtable holds gets <Class>::vf<slot> (+n other classes holding it), read from the
// decrypted exe (--exe; the Steam one is encrypted) at CommonLibF4's VTABLE ids.
// Writes <log>.named.txt next to the log and prints where.
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : fallback;
};
const logFile = args.find((a, i) => !a.startsWith('--') && (i === 0 || !args[i - 1].startsWith('--')));
if (!logFile) {
  console.error('usage: node profile-names.js <log> [--game <Fallout 4 folder>] [--commonlib <CommonLibF4 folder>]');
  process.exit(1);
}
const gameDir = option('--game', 'F:/SteamLibrary/steamapps/common/Fallout 4');
const commonLibDir = option('--commonlib', 'F:/modfiles/RDVehicles/extern/CommonLibF4OG/CommonLibF4');
const exeFile = option('--exe', 'F:/ReclamationDay Beta/test-rig/rig-src/re/Fallout4.decrypted.exe');
const ourCode = path.join(__dirname, '..', 'Code');

// The address library: a count, then (id, offset) pairs sorted by id
const bin = fs.readFileSync(path.join(gameDir, 'Data', 'F4SE', 'Plugins', 'version-1-10-163-0.bin'));
const count = Number(bin.readBigUInt64LE(0));
const byOffset = [];
for (let i = 0; i < count && 8 + i * 16 + 16 <= bin.length; ++i) {
  const id = Number(bin.readBigUInt64LE(8 + i * 16));
  const offset = Number(bin.readBigUInt64LE(16 + i * 16));
  byOffset.push([offset, id]);
}
byOffset.sort((a, b) => a[0] - b[0]);
const exact = new Map(byOffset);

function nearestBelow(offset) {
  let low = 0;
  let high = byOffset.length - 1;
  let found = -1;
  while (low <= high) {
    const mid = (low + high) >> 1;
    if (byOffset[mid][0] <= offset) {
      found = mid;
      low = mid + 1;
    } else high = mid - 1;
  }
  return found >= 0 ? byOffset[found] : null;
}

// Names by CommonLibF4 id
const names = new Map();
function* headers(dir) {
  if (!fs.existsSync(dir)) return;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) yield* headers(full);
    else if (/\.(h|hpp|cpp|inl)$/.test(entry.name)) yield full;
  }
}
const skipWords = new Set(['if', 'for', 'while', 'switch', 'return', 'func', 'REL', 'ID', 'Relocation', 'static_cast', 'reinterpret_cast', 'sizeof', 'decltype', 'func_t', 'using']);
function functionBefore(lines, at) {
  for (let i = at; i >= Math.max(0, at - 10); --i) {
    const matches = [...lines[i].matchAll(/([A-Za-z_~][A-Za-z0-9_]*)\s*\(/g)].map((m) => m[1]).filter((w) => !skipWords.has(w));
    if (matches.length) return matches[0];
  }
  return null;
}
for (const file of headers(commonLibDir)) {
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/);
  let owner = null;
  lines.forEach((line, i) => {
    const type = line.match(/^\s*(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*|__declspec\([^)]*\)\s*|alignas\([^)]*\)\s*)*([A-Za-z_][A-Za-z0-9_]*)\b(?!\s*;)/);
    if (type) owner = type[1];
    for (const m of line.matchAll(/REL::ID\(\s*(\d+)\s*\)/g)) {
      const id = Number(m[1]);
      if (names.has(id)) continue;
      const variable = line.match(/Relocation<[^>]*>\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{/);
      const fn = functionBefore(lines, i);
      const name = variable && variable[1] !== 'func' ? `${owner ? owner + '::' : ''}${variable[1]}` : fn ? `${owner ? owner + '::' : ''}${fn}` : owner;
      if (name) names.set(id, name);
    }
  });
}
// Our own code's ids (one more than CommonLibF4's) where it names them
for (const file of headers(ourCode)) {
  const text = fs.readFileSync(file, 'utf8');
  for (const m of text.matchAll(/POINTER_FALLOUT4\(\s*[^,]+,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*\)/g)) {
    const id = Number(m[2]) - 1;
    if (!names.has(id)) names.set(id, `rd:${m[1]}`);
  }
  for (const m of text.matchAll(/constexpr\s+uint32_t\s+k([A-Za-z0-9_]+)\s*=\s*(\d{4,8})\s*;/g)) {
    const id = Number(m[2]) - 1;
    if (!names.has(id)) names.set(id, `rd:${m[1]}`);
  }
}

// Virtual functions by offset, from the vtables in the decrypted exe
const virtuals = new Map();
if (fs.existsSync(exeFile)) {
  const exe = fs.readFileSync(exeFile);
  const pe = exe.readUInt32LE(0x3c);
  const sectionCount = exe.readUInt16LE(pe + 6);
  const optionalSize = exe.readUInt16LE(pe + 20);
  const imageBase = Number(exe.readBigUInt64LE(pe + 24 + 24));
  const sections = [];
  for (let i = 0; i < sectionCount; ++i) {
    const s = pe + 24 + optionalSize + i * 40;
    sections.push({ rva: exe.readUInt32LE(s + 12), size: exe.readUInt32LE(s + 8), raw: exe.readUInt32LE(s + 20), rawSize: exe.readUInt32LE(s + 16), code: (exe.readUInt32LE(s + 36) & 0x20000000) !== 0 });
  }
  const fileOffset = (rva) => {
    const s = sections.find((x) => rva >= x.rva && rva < x.rva + Math.max(x.size, x.rawSize));
    return s && rva - s.rva < s.rawSize ? s.raw + rva - s.rva : -1;
  };
  const isCode = (rva) => sections.some((x) => x.code && rva >= x.rva && rva < x.rva + x.size);
  const byId = new Map(byOffset.map(([offset, id]) => [id, offset]));
  const vtableText = fs.readFileSync(path.join(commonLibDir, 'include', 'RE', 'VTABLE_IDs.h'), 'utf8');
  for (const m of vtableText.matchAll(/\s([A-Za-z_][A-Za-z0-9_]*)\{([^}]*)\}/g)) {
    const ids = [...m[2].matchAll(/REL::ID\((\d+)\)/g)].map((x) => Number(x[1]));
    ids.forEach((id, part) => {
      const at = byId.get(id);
      if (at === undefined) return;
      const file = fileOffset(at);
      if (file < 0) return;
      for (let slot = 0; slot < 512 && file + slot * 8 + 8 <= exe.length; ++slot) {
        const target = Number(exe.readBigUInt64LE(file + slot * 8)) - imageBase;
        if (target <= 0 || !isCode(target)) break;
        const name = `${m[1].replace(/__/g, '::')}${part ? '#' + part : ''}::vf${slot.toString(16)}`;
        const known = virtuals.get(target);
        if (!known) virtuals.set(target, { name, others: 0 });
        else known.others++;
      }
    });
  }
}

function nameAt(offset, id) {
  if (names.has(id)) return names.get(id);
  const v = virtuals.get(offset);
  return v ? `${v.name}${v.others ? ` (+${v.others})` : ''}` : '?';
}

function describe(hex, isAddress) {
  const offset = parseInt(hex, 16);
  if (!isAddress && exact.has(offset)) {
    const id = exact.get(offset);
    return `[${id} ${nameAt(offset, id)}]`;
  }
  const below = nearestBelow(offset);
  if (!below) return '';
  return `[${below[1]} ${nameAt(below[0], below[1])}+${(offset - below[0]).toString(16)}]`;
}

// --dump-names <file>: every name known, by id ("names") and virtuals by offset ("virtuals"), for other tools
const dump = option('--dump-names', null);
if (dump) {
  const byIdName = Object.fromEntries(names);
  for (const [offset, v] of virtuals) {
    const id = exact.get(offset);
    if (id !== undefined && !byIdName[id]) byIdName[id] = `${v.name}${v.others ? ` (+${v.others})` : ''}`;
  }
  fs.writeFileSync(dump, JSON.stringify(byIdName));
  console.log(`${Object.keys(byIdName).length} names: ${dump}`);
}

const out = [];
for (const line of fs.readFileSync(logFile, 'utf8').split(/\r?\n/)) {
  if (!/Profile/.test(line)) continue;
  out.push(line.replace(/game\+([0-9A-F]+)(@?)/g, (whole, hex, at) => whole + describe(hex, at === '@')));
}
const target = `${logFile}.named.txt`;
fs.writeFileSync(target, out.join('\n') + '\n');
console.log(`${out.length} lines, ${names.size} names and ${virtuals.size} virtual functions known: ${target}`);
