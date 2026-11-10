// Samples a New Vegas .kf (Gamebryo 20.2.0.7) at a fixed rate, B-spline compressed curves included (PyNifly doesn't
// read NiBSplineCompTransformInterpolator, which most of Realtime Pip-Boy Map's Check.kf is). Layouts from niftools'
// nif.xml; the B-spline is NifSkope's (open uniform knots, degree 3).
// usage: node decode_kf.js <in.kf> <out.json> [fps=30]
// out: { fps, start, stop, frames, markers: [[time, text]], bones: { name: [[tx, ty, tz, qw, qx, qy, qz], ...] } }
// (each bone's transform relative to its parent node, as the file has it; frames at start + i / fps)
const fs = require('fs');
const [input, output, fpsArg] = process.argv.slice(2);
const fps = Number(fpsArg || 30);
const b = fs.readFileSync(input);
let p = 0;
const u8 = () => b[p++];
const u16 = () => { const v = b.readUInt16LE(p); p += 2; return v; };
const u32 = () => { const v = b.readUInt32LE(p); p += 4; return v; };
const i32 = () => { const v = b.readInt32LE(p); p += 4; return v; };
const f32 = () => { const v = b.readFloatLE(p); p += 4; return v; };
const sized = () => { const n = u32(); const s = b.toString('latin1', p, p + n); p += n; return s; };
const short = () => { const n = u8(); const s = b.toString('latin1', p, p + n).replace(/\0$/, ''); p += n; return s; };

// header
const line = b.toString('latin1', 0, b.indexOf(0x0a));
p = line.length + 1;
const version = u32();
if (version !== 0x14020007) throw new Error('not a 20.2.0.7 file: ' + line);
u8(); // endian
const userVersion = u32();
const numBlocks = u32();
const bsVersion = u32();
short(); short(); short(); // author, process script, export script
const types = Array.from({ length: u16() }, sized);
const typeOf = Array.from({ length: numBlocks }, () => types[u16()]);
const sizes = Array.from({ length: numBlocks }, u32);
const numStrings = u32();
u32(); // longest string
const strings = Array.from({ length: numStrings }, sized);
const groups = u32();
p += groups * 4;
const blockStart = [];
for (let i = 0; i < numBlocks; i++) { blockStart.push(p); p += sizes[i]; }
const str = (i) => (i < 0 ? null : strings[i]);
const at = (i) => { p = blockStart[i]; };

const vec3 = () => [f32(), f32(), f32()];
const quat = () => [f32(), f32(), f32(), f32()]; // w x y z

// blocks we read
function readSequence(i) {
  at(i);
  const name = str(i32());
  const n = u32();
  u32(); // array grow by
  const blocks = [];
  for (let k = 0; k < n; k++) {
    const interpolator = i32();
    i32(); // controller
    u8(); // priority (Bethesda)
    const node = str(i32());
    i32(); i32(); i32(); i32(); // property type, controller type, controller id, interpolator id
    blocks.push({ node, interpolator });
  }
  f32(); // weight
  const textKeys = i32();
  u32(); // cycle type
  const frequency = f32();
  const start = f32();
  const stop = f32();
  return { name, blocks, textKeys, frequency, start, stop };
}

function readTextKeys(i) {
  at(i);
  i32(); // name
  return Array.from({ length: u32() }, () => [f32(), str(i32())]);
}

// a key group: [time, value, ...] keys of an interpolation type
function keyGroup(size) {
  const n = u32();
  if (!n) return { type: 0, keys: [] };
  const type = u32();
  const keys = [];
  for (let k = 0; k < n; k++) {
    const t = f32();
    const v = Array.from({ length: size }, f32);
    if (type === 2) p += size * 8; // forward and backward tangents
    if (type === 3) p += 12; // tension, bias, continuity
    keys.push([t, v]);
  }
  return { type, keys };
}

function readTransformData(i) {
  at(i);
  const nRot = u32();
  let rotation = { type: 0, keys: [] };
  if (nRot) {
    const type = u32();
    if (type === 4) throw new Error('XYZ rotation keys not handled (block ' + i + ')');
    const keys = [];
    for (let k = 0; k < nRot; k++) {
      const t = f32();
      keys.push([t, quat()]);
      if (type === 3) p += 12;
    }
    rotation = { type, keys };
  }
  const translation = keyGroup(3);
  const scale = keyGroup(1);
  return { rotation, translation, scale };
}

// value of linear keys at a time (quaternions: normalised lerp after a sign fix)
function sampleKeys(keys, t, isQuat) {
  if (!keys.length) return null;
  if (t <= keys[0][0]) return keys[0][1];
  for (let k = 1; k < keys.length; k++) {
    if (t <= keys[k][0]) {
      const [t0, a] = keys[k - 1];
      const [t1, c] = keys[k];
      const s = (t - t0) / (t1 - t0 || 1);
      let d = c;
      if (isQuat && a.reduce((acc, v, j) => acc + v * c[j], 0) < 0) d = c.map((v) => -v);
      const v = a.map((x, j) => x + (d[j] - x) * s);
      return isQuat ? normalise(v) : v;
    }
  }
  return keys[keys.length - 1][1];
}

function normalise(q) {
  const l = Math.hypot(...q) || 1;
  return q.map((v) => v / l);
}

// open uniform cubic B-spline over n control points (NifSkope's bspline: order t = 4, knots 0..n-t+2)
function bspline(control, u) {
  const t = 4;
  const n = control.length - 1;
  const knots = [];
  for (let j = 0; j <= n + t; j++) knots.push(j < t ? 0 : j <= n ? j - t + 1 : n - t + 2);
  const blend = (k, order, v) => {
    if (order === 1) {
      if (knots[k] <= v && v < knots[k + 1]) return 1;
      return 0;
    }
    let value = 0;
    if (knots[k + order - 1] !== knots[k]) value += ((v - knots[k]) / (knots[k + order - 1] - knots[k])) * blend(k, order - 1, v);
    if (knots[k + order] !== knots[k + 1]) value += ((knots[k + order] - v) / (knots[k + order] - knots[k + 1])) * blend(k + 1, order - 1, v);
    return value;
  };
  const end = n - t + 2;
  if (u >= end) return control[n];
  const out = control[0].map(() => 0);
  for (let k = 0; k <= n; k++) {
    const w = blend(k, t, u);
    if (w) control[k].forEach((v, j) => { out[j] += v * w; });
  }
  return out;
}

function readBSpline(i) {
  at(i);
  const start = f32();
  const stop = f32();
  const splineData = i32();
  const basisData = i32();
  const translation = vec3();
  const rotation = quat();
  const scale = f32();
  const handles = [u32(), u32(), u32()];
  const ranges = [[f32(), f32()], [f32(), f32()], [f32(), f32()]]; // offset, half range: translation, rotation, scale
  at(splineData);
  const floatPoints = u32();
  p += floatPoints * 4; // float control points (unused by the compact interpolator)
  const compact = Array.from({ length: u32() }, () => { const v = b.readInt16LE(p); p += 2; return v; });
  at(basisData);
  const nControl = u32();
  const curve = (handle, size, [offset, half]) => {
    if (handle === 0xffff || handle === 0xffffffff) return null;
    return Array.from({ length: nControl }, (_, c) => Array.from({ length: size }, (_, j) => (compact[handle + c * size + j] / 32767) * half + offset));
  };
  return { start, stop, nControl, translation, rotation, scale, t: curve(handles[0], 3, ranges[0]), r: curve(handles[1], 4, ranges[1]) };
}

function sampleBSpline(s, time) {
  const u = ((Math.min(Math.max(time, s.start), s.stop) - s.start) / (s.stop - s.start || 1)) * (s.nControl - 3);
  const t = s.t ? bspline(s.t, u) : s.translation;
  const r = s.r ? normalise(bspline(s.r, u)) : s.rotation;
  return [...t, ...r];
}

// the sequence
const seqIndex = typeOf.indexOf('NiControllerSequence');
if (seqIndex < 0) throw new Error('no NiControllerSequence');
const seq = readSequence(seqIndex);
const markers = seq.textKeys >= 0 ? readTextKeys(seq.textKeys) : [];
const frames = Math.round((seq.stop - seq.start) * fps) + 1;
const bones = {};
const kinds = {};
for (const { node, interpolator } of seq.blocks) {
  if (!node || interpolator < 0) continue;
  const type = typeOf[interpolator];
  kinds[type] = (kinds[type] || 0) + 1;
  let sample;
  if (type === 'NiBSplineCompTransformInterpolator') {
    const s = readBSpline(interpolator);
    sample = (time) => sampleBSpline(s, time);
  } else if (type === 'NiTransformInterpolator') {
    at(interpolator);
    const translation = vec3();
    const rotation = quat();
    f32(); // scale
    const data = i32();
    const d = data >= 0 ? readTransformData(data) : null;
    sample = (time) => [...((d && sampleKeys(d.translation.keys, time, false)) || translation), ...((d && sampleKeys(d.rotation.keys, time, true)) || rotation)];
  } else {
    continue;
  }
  bones[node] = Array.from({ length: frames }, (_, f) => sample(seq.start + f / fps).map((v) => Math.round(v * 1e6) / 1e6));
}

fs.writeFileSync(output, JSON.stringify({ source: input.split(/[\\/]/).pop(), sequence: seq.name, fps, start: seq.start, stop: seq.stop, frequency: seq.frequency, frames, markers, bones }, null, 1));
console.log(`${seq.name}: ${seq.start}-${seq.stop} s, ${frames} frames, ${Object.keys(bones).length} bones (${JSON.stringify(kinds)}), user ${userVersion} bs ${bsVersion}, markers ${JSON.stringify(markers)}`);
// the bones that move: largest change of any value over the clip
const moving = Object.entries(bones).map(([name, fr]) => [name, Math.max(...fr[0].map((_, j) => Math.max(...fr.map((x) => x[j])) - Math.min(...fr.map((x) => x[j]))))]).filter(([, c]) => c > 0.01).sort((x, y) => y[1] - x[1]);
console.log('moving:', moving.map(([n, c]) => `${n} ${c.toFixed(3)}`).join(', '));
