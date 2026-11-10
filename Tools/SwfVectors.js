// SWF vector symbols to SVG or flat geometry (reads a movie on disk; nothing of it is kept here).
//   node Tools/SwfVectors.js <file.swf> classes [regex]            exported classes (SymbolClass) and their character ids
//   node Tools/SwfVectors.js <file.swf> labels [regex]             sprites with frame labels matching, their labels by frame
//   node Tools/SwfVectors.js <file.swf> tree <id|class> [label]    what a sprite places at a frame (ids, names, matrices)
//   node Tools/SwfVectors.js <file.swf> svg <id|class> [label] [out.svg]   one symbol at a frame (default the first) as SVG
//   node Tools/SwfVectors.js <file.swf> json <spec.json> [out.json]  [{key, symbol, frame}] -> flat geometry in pixels
// Shapes: DefineShape 1-4, fills assembled per fill style (fill1 edges as drawn, fill0 edges reversed, linked into
// closed loops), strokes as paths; sprites: their display list at the frame, matrices and alpha applied. Twips / 20.
const fs = require('fs');
const zlib = require('zlib');

const [file, cmd, arg, label, out] = process.argv.slice(2);
let buf = fs.readFileSync(file);
const sig = buf.toString('latin1', 0, 3);
if (sig === 'CWS') buf = Buffer.concat([buf.subarray(0, 8), zlib.inflateSync(buf.subarray(8))]);

class Bits {
  constructor(b, pos) { this.b = b; this.pos = pos; this.bit = 0; }
  align() { if (this.bit) { this.pos++; this.bit = 0; } }
  ub(n) { let v = 0; for (let i = 0; i < n; i++) { v = (v << 1) | ((this.b[this.pos] >> (7 - this.bit)) & 1); if (++this.bit === 8) { this.bit = 0; this.pos++; } } return v >>> 0; }
  sb(n) { if (!n) return 0; const v = this.ub(n); return v & (1 << (n - 1)) ? v - 2 ** n : v; }
  fb(n) { return this.sb(n) / 65536; }
  u8() { this.align(); return this.b[this.pos++]; }
  u16() { this.align(); const v = this.b.readUInt16LE(this.pos); this.pos += 2; return v; }
  s16() { this.align(); const v = this.b.readInt16LE(this.pos); this.pos += 2; return v; }
  str() { this.align(); let e = this.pos; while (this.b[e]) e++; const s = this.b.toString('utf8', this.pos, e); this.pos = e + 1; return s; }
  rect() { this.align(); const n = this.ub(5); const r = { x0: this.sb(n), x1: this.sb(n), y0: this.sb(n), y1: this.sb(n) }; this.align(); return r; }
  matrix() {
    this.align();
    const m = { a: 1, b: 0, c: 0, d: 1, tx: 0, ty: 0 };
    if (this.ub(1)) { const n = this.ub(5); m.a = this.fb(n); m.d = this.fb(n); }
    if (this.ub(1)) { const n = this.ub(5); m.b = this.fb(n); m.c = this.fb(n); }
    const n = this.ub(5); m.tx = this.sb(n); m.ty = this.sb(n);
    this.align(); return m;
  }
  cxform(alpha) {
    this.align();
    const add = this.ub(1), mult = this.ub(1), n = this.ub(4);
    const cx = { r: 1, g: 1, b: 1, a: 1, ra: 0, ga: 0, ba: 0, aa: 0 };
    if (mult) { cx.r = this.sb(n) / 256; cx.g = this.sb(n) / 256; cx.b = this.sb(n) / 256; if (alpha) cx.a = this.sb(n) / 256; }
    if (add) { cx.ra = this.sb(n); cx.ga = this.sb(n); cx.ba = this.sb(n); if (alpha) cx.aa = this.sb(n); }
    this.align(); return cx;
  }
  rgb(alpha) { this.align(); const c = { r: this.b[this.pos], g: this.b[this.pos + 1], b: this.b[this.pos + 2], a: 255 }; this.pos += 3; if (alpha) c.a = this.b[this.pos++]; return c; }
}

// --- tags
const readTags = (start, end) => {
  const tags = [];
  let p = start;
  while (p < end) {
    const head = buf.readUInt16LE(p);
    let len = head & 0x3f; let at = p + 2;
    if (len === 0x3f) { len = buf.readUInt32LE(at); at += 4; }
    tags.push({ code: head >> 6, at, len });
    p = at + len;
    if (head >> 6 === 0) break;
  }
  return tags;
};
const hb = new Bits(buf, 8); hb.rect(); hb.pos += 4;
const top = readTags(hb.pos, buf.length);

const chars = new Map(); // id -> {kind, tag}
const classes = new Map(); // name -> id
for (const t of top) {
  if ([2, 22, 32, 83].includes(t.code)) chars.set(buf.readUInt16LE(t.at), { kind: 'shape', tag: t });
  else if (t.code === 39) chars.set(buf.readUInt16LE(t.at), { kind: 'sprite', tag: t });
  else if (t.code === 37) chars.set(buf.readUInt16LE(t.at), { kind: 'text', tag: t });
  else if (t.code === 76) {
    const b = new Bits(buf, t.at); const n = b.u16();
    for (let i = 0; i < n; i++) { const id = b.u16(); classes.set(b.str(), id); }
  }
}

// --- shapes
const fillStyle = (b, ver) => {
  const out = [];
  {
    const type = b.u8();
    if (type === 0) out.push({ type: 'solid', color: b.rgb(ver >= 3) });
    else if (type === 0x10 || type === 0x12 || type === 0x13) {
      b.matrix(); b.align(); b.ub(2); b.ub(2); const g = b.ub(4); const stops = [];
      for (let j = 0; j < g; j++) { const ratio = b.u8(); stops.push({ ratio, color: b.rgb(ver >= 3) }); }
      if (type === 0x13) b.s16();
      out.push({ type: 'gradient', color: stops[stops.length - 1]?.color ?? { r: 255, g: 255, b: 255, a: 255 }, stops });
    } else if (type >= 0x40 && type <= 0x43) { b.u16(); b.matrix(); out.push({ type: 'bitmap', color: { r: 255, g: 255, b: 255, a: 128 } }); }
    else throw new Error('fill type ' + type);
  }
  return out[0];
};
const fillStyles = (b, ver) => {
  let n = b.u8(); if (n === 0xff && ver >= 2) n = b.u16();
  const out = [];
  for (let i = 0; i < n; i++) out.push(fillStyle(b, ver));
  return out;
};
const lineStyles = (b, ver) => {
  let n = b.u8(); if (n === 0xff && ver >= 2) n = b.u16();
  const out = [];
  for (let i = 0; i < n; i++) {
    const width = b.u16();
    if (ver === 4) {
      b.ub(2); const join = b.ub(2); const hasFill = b.ub(1); b.ub(1); b.ub(1); b.ub(1); b.ub(5); b.ub(1); b.ub(2);
      if (join === 2) b.u16();
      out.push({ width, color: hasFill ? fillStyle(b, 4).color : b.rgb(true) });
    } else out.push({ width, color: b.rgb(ver >= 3) });
  }
  return out;
};

const shapeCache = new Map();
const parseShape = (id) => {
  if (shapeCache.has(id)) return shapeCache.get(id);
  const { tag } = chars.get(id);
  const ver = { 2: 1, 22: 2, 32: 3, 83: 4 }[tag.code];
  const b = new Bits(buf, tag.at + 2);
  b.rect();
  if (ver === 4) { b.rect(); b.u8(); }
  let fills = fillStyles(b, ver), lines = lineStyles(b, ver);
  b.align(); let nf = b.ub(4), nl = b.ub(4);
  const groups = []; // {fills, lines, edges:[{f0,f1,l,pts}]}
  let group = { fills, lines, edges: [] }; groups.push(group);
  let x = 0, y = 0, f0 = 0, f1 = 0, l = 0;
  for (;;) {
    if (b.ub(1) === 0) {
      const newStyles = b.ub(1), sl = b.ub(1), sf1 = b.ub(1), sf0 = b.ub(1), move = b.ub(1);
      if (!newStyles && !sl && !sf1 && !sf0 && !move) break;
      if (move) { const n = b.ub(5); x = b.sb(n); y = b.sb(n); }
      if (sf0) f0 = b.ub(nf);
      if (sf1) f1 = b.ub(nf);
      if (sl) l = b.ub(nl);
      if (newStyles) {
        fills = fillStyles(b, ver); lines = lineStyles(b, ver); b.align(); nf = b.ub(4); nl = b.ub(4);
        group = { fills, lines, edges: [] }; groups.push(group); f0 = f1 = l = 0;
      }
    } else if (b.ub(1)) {
      const n = b.ub(4) + 2; let dx = 0, dy = 0;
      if (b.ub(1)) { dx = b.sb(n); dy = b.sb(n); } else if (b.ub(1)) dy = b.sb(n); else dx = b.sb(n);
      group.edges.push({ f0, f1, l, from: [x, y], to: [x + dx, y + dy] }); x += dx; y += dy;
    } else {
      const n = b.ub(4) + 2;
      const cx = x + b.sb(n), cy = y + b.sb(n); const ax = cx + b.sb(n), ay = cy + b.sb(n);
      group.edges.push({ f0, f1, l, from: [x, y], c: [cx, cy], to: [ax, ay] }); x = ax; y = ay;
    }
  }
  shapeCache.set(id, groups);
  return groups;
};

const key = (p) => p[0] + ',' + p[1];
const reverse = (e) => ({ from: e.to, c: e.c, to: e.from });
const pathOf = (edges) => {
  // link into closed loops by matching end to start
  const pool = edges.slice(); let d = '';
  while (pool.length) {
    let e = pool.shift(); const start = key(e.from);
    d += `M${e.from[0] / 20} ${e.from[1] / 20}`;
    for (;;) {
      d += e.c ? `Q${e.c[0] / 20} ${e.c[1] / 20} ${e.to[0] / 20} ${e.to[1] / 20}` : `L${e.to[0] / 20} ${e.to[1] / 20}`;
      if (key(e.to) === start) { d += 'Z'; break; }
      const i = pool.findIndex((n) => key(n.from) === key(e.to));
      if (i < 0) break;
      e = pool.splice(i, 1)[0];
    }
  }
  return d;
};
const rgba = (c, cx) => {
  const ch = (v, m, a) => Math.max(0, Math.min(255, Math.round(v * m + a)));
  const col = cx ? { r: ch(c.r, cx.r, cx.ra), g: ch(c.g, cx.g, cx.ga), b: ch(c.b, cx.b, cx.ba), a: ch(c.a, cx.a, cx.aa) } : c;
  return { fill: `rgb(${col.r},${col.g},${col.b})`, opacity: +(col.a / 255).toFixed(3) };
};
const shapeSvg = (id, cx) => {
  let s = '';
  for (const g of parseShape(id)) {
    for (let i = 1; i <= g.fills.length; i++) {
      const edges = [];
      for (const e of g.edges) { if (e.f1 === i) edges.push(e); if (e.f0 === i) edges.push(reverse(e)); }
      if (!edges.length) continue;
      const c = rgba(g.fills[i - 1].color, cx);
      s += `<path d="${pathOf(edges)}" fill="${c.fill}" fill-opacity="${c.opacity}"/>`;
    }
    for (let i = 1; i <= g.lines.length; i++) {
      const edges = g.edges.filter((e) => e.l === i);
      if (!edges.length) continue;
      const c = rgba(g.lines[i - 1].color, cx);
      const d = edges.map((e) => `M${e.from[0] / 20} ${e.from[1] / 20}` + (e.c ? `Q${e.c[0] / 20} ${e.c[1] / 20} ${e.to[0] / 20} ${e.to[1] / 20}` : `L${e.to[0] / 20} ${e.to[1] / 20}`)).join('');
      s += `<path d="${d}" fill="none" stroke="${c.fill}" stroke-opacity="${c.opacity}" stroke-width="${Math.max(g.lines[i - 1].width / 20, 0.5)}"/>`;
    }
  }
  return s;
};

// --- sprites: the display list at a frame (by number from 1, or by label)
const spriteInfo = (id) => {
  const { tag } = chars.get(id);
  const tags = readTags(tag.at + 4, tag.at + tag.len);
  const labels = []; let frame = 1;
  for (const t of tags) { if (t.code === 43) labels.push({ frame, name: new Bits(buf, t.at).str() }); if (t.code === 1) frame++; }
  return { tags, labels, frames: frame - 1 };
};
const displayList = (id, at) => {
  const { tags, labels } = spriteInfo(id);
  let target = 1;
  if (at) { const l = labels.find((x) => x.name === at); target = l ? l.frame : Number(at) || 1; }
  const list = new Map(); let frame = 1;
  for (const t of tags) {
    if (t.code === 26 || t.code === 70) {
      const b = new Bits(buf, t.at); const f = b.u8(); let f2 = 0; if (t.code === 70) f2 = b.u8();
      const depth = b.u16();
      if (t.code === 70 && (f2 & 0x08 || (f2 & 0x10 && f & 0x02))) b.str();
      const e = f & 0x01 ? { ...(list.get(depth) || {}) } : {};
      if (f & 0x02) e.id = b.u16();
      if (f & 0x04) e.m = b.matrix();
      if (f & 0x08) e.cx = b.cxform(true);
      if (f & 0x10) b.u16();
      if (f & 0x20) e.name = b.str();
      if (f & 0x40) e.clip = b.u16();
      list.set(depth, e);
    } else if (t.code === 28) list.delete(buf.readUInt16LE(t.at));
    else if (t.code === 1) { if (frame === target) break; frame++; }
  }
  return [...list.entries()].sort((a, b) => a[0] - b[0]).map(([depth, e]) => ({ depth, ...e }));
};
const mat = (m) => (m ? `matrix(${m.a} ${m.b} ${m.c} ${m.d} ${m.tx / 20} ${m.ty / 20})` : '');
const mulcx = (a, b) => (!a ? b : !b ? a : { r: a.r * b.r, g: a.g * b.g, b: a.b * b.b, a: a.a * b.a, ra: a.ra + b.ra, ga: a.ga + b.ga, ba: a.ba + b.ba, aa: a.aa + b.aa });
const render = (id, at, cx, depth = 0) => {
  const c = chars.get(id);
  if (!c || depth > 12) return '';
  if (c.kind === 'shape') return shapeSvg(id, cx);
  if (c.kind === 'text') return '';
  let s = '';
  for (const e of displayList(id, depth === 0 ? at : undefined)) {
    if (e.clip || e.id === undefined) continue;
    const inner = render(e.id, undefined, mulcx(cx, e.cx), depth + 1);
    if (inner) s += `<g${e.m ? ` transform="${mat(e.m)}"` : ''}${e.name ? ` data-name="${e.name}"` : ''}>${inner}</g>`;
  }
  return s;
};
const resolve = (x) => (/^\d+$/.test(x) ? Number(x) : classes.get(x));

// --- flat geometry: every fill's closed loops and every stroke in pixels, all matrices applied
const ID = { a: 1, b: 0, c: 0, d: 1, tx: 0, ty: 0 };
const toPx = (m) => (m ? { ...m, tx: m.tx / 20, ty: m.ty / 20 } : ID);
const mmul = (P, C) => ({ a: P.a * C.a + P.c * C.b, b: P.b * C.a + P.d * C.b, c: P.a * C.c + P.c * C.d, d: P.b * C.c + P.d * C.d, tx: P.a * C.tx + P.c * C.ty + P.tx, ty: P.b * C.tx + P.d * C.ty + P.ty });
const apply = (M, p) => [M.a * (p[0] / 20) + M.c * (p[1] / 20) + M.tx, M.b * (p[0] / 20) + M.d * (p[1] / 20) + M.ty];
const loopsOf = (edges) => {
  const pool = edges.slice(); const loops = [];
  while (pool.length) {
    let e = pool.shift(); const start = key(e.from); const loop = [e];
    for (;;) {
      if (key(e.to) === start) break;
      const i = pool.findIndex((n) => key(n.from) === key(e.to));
      if (i < 0) break;
      e = pool.splice(i, 1)[0]; loop.push(e);
    }
    loops.push(loop);
  }
  return loops;
};
const seg = (M, e) => ({ from: apply(M, e.from), c: e.c ? apply(M, e.c) : undefined, to: apply(M, e.to) });
const flat = (id, at, M = ID, alpha = 1, depth = 0, out = { fills: [], strokes: [] }) => {
  const c = chars.get(id);
  if (!c || depth > 12 || c.kind === 'text') return out;
  if (c.kind === 'shape') {
    const scale = Math.sqrt(Math.abs(M.a * M.d - M.b * M.c));
    for (const g of parseShape(id)) {
      for (let i = 1; i <= g.fills.length; i++) {
        const edges = [];
        for (const e of g.edges) { if (e.f1 === i) edges.push(e); if (e.f0 === i) edges.push(reverse(e)); }
        if (edges.length) out.fills.push({ alpha: alpha * g.fills[i - 1].color.a / 255, loops: loopsOf(edges).map((l) => l.map((e) => seg(M, e))) });
      }
      for (let i = 1; i <= g.lines.length; i++) {
        const edges = g.edges.filter((e) => e.l === i);
        if (edges.length) out.strokes.push({ alpha: alpha * g.lines[i - 1].color.a / 255, width: Math.max(g.lines[i - 1].width / 20, 0.5) * scale, segs: edges.map((e) => seg(M, e)) });
      }
    }
    return out;
  }
  for (const e of displayList(id, depth === 0 ? at : undefined)) {
    if (e.clip || e.id === undefined) continue;
    flat(e.id, undefined, mmul(M, toPx(e.m)), alpha * (e.cx ? e.cx.a : 1), depth + 1, out);
  }
  return out;
};

if (cmd === 'classes') {
  const re = new RegExp(arg || '.', 'i');
  for (const [name, id] of classes) if (re.test(name)) console.log(id, chars.get(id)?.kind ?? '?', name);
} else if (cmd === 'labels') {
  const re = new RegExp(arg || '.', 'i');
  const byId = new Map([...classes].map(([n, i]) => [i, n]));
  for (const [id, c] of chars) {
    if (c.kind !== 'sprite') continue;
    const { labels, frames } = spriteInfo(id);
    if (labels.some((l) => re.test(l.name))) console.log(`sprite ${id}${byId.has(id) ? ' (' + byId.get(id) + ')' : ''}, ${frames} frames: ` + labels.map((l) => `${l.frame}:${l.name}`).join(' '));
  }
} else if (cmd === 'tree') {
  const byId = new Map([...classes].map(([n, i]) => [i, n]));
  for (const e of displayList(resolve(arg), label)) console.log(`depth ${e.depth} id ${e.id} ${chars.get(e.id)?.kind ?? '?'}${byId.has(e.id) ? ' (' + byId.get(e.id) + ')' : ''}${e.name ? ' name ' + e.name : ''}${e.m ? ' ' + mat(e.m) : ''}${e.cx ? ' alpha*' + e.cx.a.toFixed(2) : ''}${e.clip ? ' clip' : ''}`);
} else if (cmd === 'json') {
  // arg: a JSON file of [{key, symbol, frame}]; prints {key: {fills, strokes}}
  const result = {};
  for (const { key: k, symbol, frame } of JSON.parse(fs.readFileSync(arg, 'utf8'))) {
    const id = resolve(symbol);
    if (id === undefined) { console.error('no symbol', symbol); continue; }
    result[k] = flat(id, frame || undefined);
  }
  const text = JSON.stringify(result);
  if (label) fs.writeFileSync(label, text); else console.log(text);
} else if (cmd === 'svg') {
  const body = render(resolve(arg), label);
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="-40 -40 80 80" width="320" height="320" style="background:#0a1a10">${body}</svg>`;
  if (out) fs.writeFileSync(out, svg); else console.log(svg);
}
