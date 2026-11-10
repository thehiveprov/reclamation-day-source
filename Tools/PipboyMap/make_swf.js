// Writes the Realtime Pip-Boy Map's movie: an empty ActionScript 3 movie (no code, no shapes) the client draws the map
// into at run time. usage: node make_swf.js <out.swf> [width=1024] [height=768]
const fs = require('fs');
const [out, width = 1024, height = 768] = process.argv.slice(2);

// the stage rectangle, in twips, as a SWF RECT (bit packed: 5 bit field size, then x min, x max, y min, y max)
const bits = [];
const put = (value, count) => { for (let i = count - 1; i >= 0; i--) bits.push((value >> i) & 1); };
const size = 16;
put(size, 5);
put(0, size); put(width * 20, size); put(0, size); put(height * 20, size);
while (bits.length % 8) bits.push(0);
const rect = Buffer.alloc(bits.length / 8);
bits.forEach((bit, i) => { rect[i >> 3] |= bit << (7 - (i & 7)); });

const tag = (code, data = Buffer.alloc(0)) => {
  const header = Buffer.alloc(2);
  header.writeUInt16LE((code << 6) | data.length);
  return Buffer.concat([header, data]);
};
const body = Buffer.concat([
  rect,
  Buffer.from([0x00, 0x3c]),          // 60 frames a second (8.8 fixed)
  Buffer.from([0x01, 0x00]),          // one frame
  tag(69, Buffer.from([0x08, 0, 0, 0])), // FileAttributes: ActionScript 3
  tag(1),                             // ShowFrame
  tag(0),                             // End
]);
const header = Buffer.alloc(8);
header.write('FWS', 0, 'latin1');
header[3] = 10;
header.writeUInt32LE(8 + body.length, 4);
fs.writeFileSync(out, Buffer.concat([header, body]));
console.log(out, 8 + body.length, 'bytes');
