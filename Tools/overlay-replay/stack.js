// The faulting thread's stack as text: the handler's local copy of the event's name is a short string kept on the stack.
const fs = require('fs');
const b = fs.readFileSync(process.argv[2]);
const u32 = o => b.readUInt32LE(o);
const u64 = o => b.readBigUInt64LE(o);
const dir = u32(12);
const stream = {};
for (let i = 0; i < u32(8); i++) stream[u32(dir + i * 12)] = { size: u32(dir + i * 12 + 4), rva: u32(dir + i * 12 + 8) };
const ex = stream[6].rva;
const context = u32(ex + 8 + 152 + 4);
const rsp = u64(context + 0x78 + 4 * 8);
const threadId = u32(ex);
let stack = null;
const count = u32(stream[3].rva);
for (let i = 0; i < count; i++) {
  const o = stream[3].rva + 4 + i * 48;
  if (u32(o) === threadId) stack = { start: u64(o + 24), size: u32(o + 32), rva: u32(o + 36) };
}
const offset = Number(rsp - stack.start);
const data = b.subarray(stack.rva + offset, stack.rva + stack.size);
console.log('rsp', rsp.toString(16), 'stack left', data.length);
// Runs of printable text on the first part of the stack
const text = /[\x20-\x7e]{4,}/g;
const head = data.subarray(0, 0x4000).toString('latin1');
let match;
while ((match = text.exec(head))) console.log('+0x' + match.index.toString(16).padStart(4, '0'), JSON.stringify(match[0]));
console.log('\nfirst 0x130 bytes, 8 to a line:');
for (let i = 0; i < 0x130; i += 8) {
  const q = data.readBigUInt64LE(i);
  const ascii = data.subarray(i, i + 8).toString('latin1').replace(/[^\x20-\x7e]/g, '.');
  console.log('+0x' + i.toString(16).padStart(3, '0'), q.toString(16).padStart(16, '0'), ascii);
}
