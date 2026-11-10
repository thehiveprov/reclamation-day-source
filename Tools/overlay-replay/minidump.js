// Reads a Windows minidump: the exception, the faulting thread's registers, and return addresses found on its stack.
// Usage: node minidump.js <file.dmp>
const fs = require('fs');
const b = fs.readFileSync(process.argv[2]);
const u32 = o => b.readUInt32LE(o);
const u64 = o => b.readBigUInt64LE(o);
const hex = v => '0x' + v.toString(16);

if (u32(0) !== 0x504d444d) throw new Error('not a minidump');
const streams = u32(8);
const dir = u32(12);
const stream = {};
for (let i = 0; i < streams; i++) {
  const o = dir + i * 12;
  stream[u32(o)] = { size: u32(o + 4), rva: u32(o + 8) };
}

function readString(rva) {
  const len = u32(rva);
  return b.toString('utf16le', rva + 4, rva + 4 + len);
}

const modules = [];
if (stream[4]) {
  const count = u32(stream[4].rva);
  for (let i = 0; i < count; i++) {
    const o = stream[4].rva + 4 + i * 108;
    modules.push({ base: u64(o), size: BigInt(u32(o + 8)), name: readString(u32(o + 20)).split('\\').pop() });
  }
}
const where = address => {
  const m = modules.find(m => address >= m.base && address < m.base + m.size);
  return m ? `${m.name}+${hex(address - m.base)}` : null;
};

// Memory
const ranges = [];
if (stream[5]) {
  const count = u32(stream[5].rva);
  for (let i = 0; i < count; i++) {
    const o = stream[5].rva + 4 + i * 16;
    ranges.push({ start: u64(o), size: BigInt(u32(o + 8)), rva: BigInt(u32(o + 12)) });
  }
}
if (stream[9]) {
  const count = Number(u64(stream[9].rva));
  let rva = u64(stream[9].rva + 8);
  for (let i = 0; i < count; i++) {
    const o = stream[9].rva + 16 + i * 16;
    const size = u64(o + 8);
    ranges.push({ start: u64(o), size, rva });
    rva += size;
  }
}
function read(address, length) {
  for (const r of ranges) {
    if (address >= r.start && address + BigInt(length) <= r.start + r.size) {
      const at = Number(r.rva + (address - r.start));
      return b.subarray(at, at + length);
    }
  }
  return null;
}

const ex = stream[6].rva;
const threadId = u32(ex);
const code = u32(ex + 8);
const address = u64(ex + 8 + 16);
const parameters = u32(ex + 8 + 24);
console.log('thread', threadId, 'exception', hex(code), 'at', hex(address), where(address) || '');
for (let i = 0; i < parameters; i++) console.log('  parameter', i, hex(u64(ex + 8 + 32 + i * 8)));
const contextRva = u32(ex + 8 + 152 + 4);

const names = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'rip'];
const regs = {};
names.forEach((name, i) => (regs[name] = u64(contextRva + 0x78 + i * 8)));
for (const name of names) console.log(' ', name.padEnd(4), hex(regs[name]).padEnd(20), where(regs[name]) || '');

console.log('\nreturn addresses on the stack (first 60 into a module):');
let shown = 0;
for (let at = regs.rsp; shown < 60; at += 8n) {
  const value = read(at, 8);
  if (!value) break;
  const v = value.readBigUInt64LE(0);
  const w = where(v);
  if (w) {
    console.log(' ', hex(at), w);
    shown++;
  }
}

console.log('\nmodules:', modules.length);
for (const m of modules.slice(0, 6)) console.log(' ', m.name, hex(m.base), hex(m.size));

const code2 = read(regs.rip - 24n, 56);
if (code2) console.log('\ncode at rip-24:', code2.toString('hex'));

// Every thread, briefly
if (stream[3]) {
  const count = u32(stream[3].rva);
  console.log('\nthreads:', count);
}

// Strings near what the registers point to, for a clue to what was being processed
for (const name of ['rcx', 'rdx', 'rbx', 'rsi', 'rdi', 'r8', 'r12', 'r13', 'r14', 'r15']) {
  const m = read(regs[name], 64);
  if (m) console.log(name.padEnd(4), m.toString('hex').slice(0, 64), JSON.stringify(m.toString('latin1').replace(/[^\x20-\x7e]/g, '.').slice(0, 48)));
}
