// Builds GameFiles/Fallout4/RD_Combat.esp, Reclamation Day's combat AI plugin: o14's Adaptive Combat Tactics (Nexus mod
// 107069, used with the author's permission), its Tested profile with Increased Player Detection Range merged in, and
// these changes for co-op:
//   fDetectionMaxDistanceVisiblePlayer          13824 -> 12288: enemies see players from 3 cells at most, the edge of
//                                                the server's 4-a-second movement tier (CharacterService.cpp)
//   fCombatTargetSelectorScoreTargetSharedPenalty -60 -> -80: enemies spread over the players a little more
// The 151 game settings and 58 combat styles are otherwise the source's. A light plugin (ESL flag) like the source.
// Then our own enemy tactics (docs/ENEMY_TACTICS.md): one combat style per profile, variant and mood (TACTICS below),
// made from a vanilla style by the formulas in tacticsStyle(), from local id 0xA00 up, and the client's table of them,
// Code/client/Games/Fallout4/TacticsTable.h.
// usage: node Tools/BuildCombatPlugin.js "<Adaptive Combat Tactics.esp (Tested)>" [out.esp]
const fs = require('fs');
const path = require('path');

const [source, outArg] = process.argv.slice(2);
const out = outArg || path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'RD_Combat.esp');
const TABLE_OUT = path.join(__dirname, '..', 'Code', 'client', 'Games', 'Fallout4', 'TacticsTable.h');

// Profiles: aggression, discipline, cunning, fearlessness (people only), accuracy and range (null: the base style's).
// Super mutants are the exception: their vanilla style made smarter (mutantStyle), the numbers only steer their moods.
const PERSON = { variants: [['Ranged', 0x03183b], ['Melee', 0x047165], ['Heavy', 0x09a6b8]], moods: ['Steady', 'Press', 'Shaken', 'Flank'] };
const TACTICS = [
  ['Generic', 'kPerson', PERSON, 0.5, 0.55, 0.35, 0.3, 0.65, 0.5],
  ['Raiders', 'kPerson', PERSON, 0.65, 0.3, 0.25, 0.2, 0.55, 0.45],
  ['Gunners', 'kPerson', PERSON, 0.55, 0.85, 0.7, 0.6, 0.85, 0.6],
  ['Operators', 'kPerson', PERSON, 0.6, 0.8, 0.75, 0.55, 0.85, 0.6],
  ['Disciples', 'kPerson', PERSON, 0.75, 0.4, 0.8, 0.4, 0.65, 0.4],
  ['Pack', 'kPerson', PERSON, 0.9, 0.3, 0.3, 0.6, 0.55, 0.35],
  ['Triggermen', 'kPerson', PERSON, 0.7, 0.45, 0.3, 0.35, 0.5, 0.3],
  ['Bos', 'kPerson', PERSON, 0.65, 0.9, 0.55, 0.8, 0.85, 0.65],
  ['Atom', 'kPerson', PERSON, 0.85, 0.35, 0.2, 1, 0.65, 0.45],
  ['Trappers', 'kPerson', PERSON, 0.45, 0.7, 0.9, 0.45, 0.85, 0.7],
  ['RustDevils', 'kPerson', PERSON, 0.6, 0.65, 0.6, 0.45, 0.7, 0.55],
  ['Forged', 'kPerson', PERSON, 0.85, 0.3, 0.2, 0.5, 0.5, 0.25],
  ['Minutemen', 'kPerson', PERSON, 0.4, 0.75, 0.4, 0.5, 0.7, 0.6],
  ['Railroad', 'kPerson', PERSON, 0.5, 0.7, 0.75, 0.5, 0.75, 0.55],
  ['Courser', 'kSynth', { variants: [['Ranged', 0x04a9c0], ['Melee', 0x06563b]], moods: ['Steady', 'Flank'] }, 0.75, 0.9, 0.9, 1, 0.95, 0.55],
  ['Mutant', 'kMutant', { variants: [['Ranged', 0x0a881f], ['Melee', 0x0a881f]], moods: ['Steady', 'Rage'] }, 0.75, 0.45, 0.45, 1, null, null],
  ['Synth', 'kSynth', { variants: [['Ranged', 0x1603f0], ['Melee', 0x06563b]], moods: ['Steady', 'Flank'] }, 0.55, 0.95, 0.9, 1, 0.8, 0.6],
  ...[
    ['Assaultron', 0x0e1aaf, 0.85, 0.7, 0.95],
    ['Protectron', 0x0d840c, 0.5, 0.9, 0.4],
    ['SentryBot', 0x0b9d66, 0.8, 0.8, 0.5],
    ['Gutsy', 0x149c9d, 0.65, 0.9, 0.85],
    ['Handy', 0x0ef218, 0.6, 0.7, 0.6],
    ['Eyebot', 0x0a7da3, 0.4, 0.8, 0.7],
    ['Robobrain', 0x149c9d, 0.55, 0.95, 0.9],
    ['Bot', 0x149c9d, 0.65, 0.85, 0.8],
  ].map(([name, base, a, d, c]) => [name, 'kRobot', { variants: [['', base]], moods: ['Steady', 'Flank'] }, a, d, c, 1, null, null]),
];
const FIRST_TACTICS_ID = 0xa00;
const CHANGES = { fDetectionMaxDistanceVisiblePlayer: 12288, fCombatTargetSelectorScoreTargetSharedPenalty: -80 };
const AUTHOR = 'Reclamation Day';
const DESCRIPTION = "Reclamation Day's combat AI: based on Adaptive Combat Tactics by o14 (Nexus mod 107069), with Increased Player Detection Range merged in and tuned for co-op.";

const src = fs.readFileSync(source);
const sub = (type, data) => {
  const head = Buffer.alloc(6);
  head.write(type, 0, 'latin1');
  head.writeUInt16LE(data.length, 4);
  return Buffer.concat([head, data]);
};
const zstr = s => Buffer.from(s + '\0', 'latin1');

// TES4 header: keep every subrecord but the author (CNAM) and description (SNAM), which become ours
const headSize = src.readUInt32LE(4);
const headerSubs = [];
for (let o = 24; o < 24 + headSize;) {
  const type = src.toString('latin1', o, o + 4);
  const size = src.readUInt16LE(o + 4);
  headerSubs.push([type, src.subarray(o + 6, o + 6 + size)]);
  o += 6 + size;
}
const subs = [];
for (const [type, data] of headerSubs) {
  if (type === 'CNAM' || type === 'SNAM') continue;
  subs.push(sub(type, data));
  if (type === 'HEDR') subs.push(sub('CNAM', zstr(AUTHOR)), sub('SNAM', zstr(DESCRIPTION)));
}
const headerData = Buffer.concat(subs);
const header = Buffer.from(src.subarray(0, 24));
header.writeUInt32LE(headerData.length, 4);

// The rest unchanged but the two game settings' values (GMST: EDID, then a 4-byte DATA), patched in place
const body = Buffer.from(src.subarray(24 + headSize));
const done = new Set();
function walk(start, end) {
  for (let o = start; o < end;) {
    const type = body.toString('latin1', o, o + 4);
    const size = body.readUInt32LE(o + 4);
    if (type === 'GRUP') {
      walk(o + 24, o + size);
      o += size;
      continue;
    }
    if (type === 'GMST') {
      if (body.readUInt32LE(o + 8) & 0x40000) throw new Error('compressed GMST: not handled');
      let edid = null;
      for (let s = o + 24; s < o + 24 + size;) {
        const subType = body.toString('latin1', s, s + 4);
        const subSize = body.readUInt16LE(s + 4);
        if (subType === 'EDID') edid = body.toString('latin1', s + 6, s + 6 + subSize - 1);
        if (subType === 'DATA' && edid in CHANGES) {
          const before = edid[0] === 'f' ? body.readFloatLE(s + 6) : body.readInt32LE(s + 6);
          edid[0] === 'f' ? body.writeFloatLE(CHANGES[edid], s + 6) : body.writeInt32LE(CHANGES[edid], s + 6);
          console.log(`${edid}: ${before} -> ${CHANGES[edid]}`);
          done.add(edid);
        }
        s += 6 + subSize;
      }
    }
    o += 24 + size;
  }
}
walk(0, body.length);
for (const name of Object.keys(CHANGES))
  if (!done.has(name)) throw new Error(`${name} not found in ${source}`);

// The source's combat styles group and its Fallout4.esm styles (the tactics' bases), subrecords by type
let cstyGroup = null;
for (let o = 0; o < body.length;) {
  const size = body.readUInt32LE(o + 4);
  if (body.toString('latin1', o, o + 4) === 'GRUP' && body.toString('latin1', o + 8, o + 12) === 'CSTY') cstyGroup = { start: o, end: o + size };
  o += size;
}
if (!cstyGroup) throw new Error('no combat styles group in the source');
const bases = new Map();
let recordHead = null;
for (let o = cstyGroup.start + 24; o < cstyGroup.end;) {
  const size = body.readUInt32LE(o + 4);
  if (body.readUInt32LE(o + 8) & 0x40000) throw new Error('compressed CSTY: not handled');
  const id = body.readUInt32LE(o + 12);
  recordHead ||= body.subarray(o + 16, o + 24);
  if (id >>> 24 === 0) {
    const fields = {};
    for (let s = o + 24; s < o + 24 + size;) {
      const subSize = body.readUInt16LE(s + 4);
      fields[body.toString('latin1', s, s + 4)] = body.subarray(s + 6, s + 6 + subSize);
      s += 6 + subSize;
    }
    bases.set(id, fields);
  }
  o += 24 + size;
}

const floats = (data, count, pad) => Array.from({ length: count }, (_, i) => (data && data.length >= (i + 1) * 4 ? data.readFloatLE(i * 4) : pad));
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const kFlanking = 2, kCharging = 8;

const f32 = (values) => { const b = Buffer.alloc(values.length * 4); values.forEach((v, i) => b.writeFloatLE(v, i * 4)); return b; };

// A super mutant's style: its vanilla one (csSupermutantRanged) a little smarter, fighting as a pack, flanking and using
// cover between pushes; rage is a push that still takes cover (docs/ENEMY_TACTICS.md, Super mutants)
function mutantStyle(base, variant, mood) {
  const rage = mood === 'Rage', melee = variant === 'Melee';
  const general = floats(base.CSGD, 12, 1);
  general[0] += rage ? 0.2 : 0;
  general[1] = (general[1] + 0.1) * (rage ? 0.7 : 1);
  general[2] = Math.max(general[2], 0.9);
  if (melee) { general[3] = 10; general[5] = 0.01; }
  for (const i of [9, 10, 11]) general[i] *= rage ? 1 : 1.5;

  const close = floats(base.CSCR, 11, 0);
  const throwMax = base.CSCR && base.CSCR.length >= 40 ? base.CSCR.readUInt32LE(36) : 0;
  close[0] += 0.1;
  close[1] *= rage ? 0.5 : 1;
  close[2] = rage ? 0.3 : 0.45;
  if (melee) { close[4] = 0.95; close[6] = 0.94; }

  const long = floats(base.CSLR, 5, 0.5);
  long[0] += rage ? 0.25 : 0.2;
  if (rage) { long[2] *= 0.5; long[3] *= 0.4; long[4] = Math.max(0.3, long[4] - 0.1); }
  const cover = (base.CSCV ? base.CSCV.readFloatLE(0) : 0.5) + (rage ? -0.15 : 0.2);

  const flags = (base.DATA ? base.DATA.readUInt32LE(0) : 1) | kFlanking | (rage || melee ? kCharging : 0);
  const closeData = f32(close);
  closeData.writeUInt32LE(throwMax, 36);
  const flagData = Buffer.alloc(4);
  flagData.writeUInt32LE(flags);
  return [
    ['CSGD', f32(general)],
    ...(base.CSMD ? [['CSMD', base.CSMD]] : []),
    ['CSME', f32(floats(base.CSME, 10, 1))],
    ['CSRA', f32([(base.CSRA ? base.CSRA.readFloatLE(0) : 0.55) + 0.05])],
    ['CSCR', closeData],
    ['CSLR', f32(long)],
    ['CSCV', f32([cover])],
    ...(base.CSFL ? [['CSFL', base.CSFL]] : []),
    ['DATA', flagData],
  ];
}

// One style: the base's, with the profile's fighting put in (docs/ENEMY_TACTICS.md, The formulas)
function tacticsStyle(base, kind, variant, mood, [a, d, c], accuracy, range) {
  if (kind === 'kMutant') return mutantStyle(base, variant, mood);
  if (mood === 'Press') a = Math.min(1, a + 0.3);
  if (mood === 'Shaken') { a = Math.max(0, a - 0.4); d = Math.min(1, d + 0.15); }
  if (mood === 'Rage') { a = 1; d *= 0.4; }
  if (mood === 'Flank') { c = 1; a = Math.min(1, a + 0.1); }
  const melee = variant === 'Melee', heavy = variant === 'Heavy', robot = kind === 'kRobot';

  const general = floats(base.CSGD, 12, 1);
  general[0] = melee ? 0.6 + 0.7 * a : 0.25 + 0.75 * a + (heavy ? 0.15 : 0);
  general[1] = clamp(0.95 - 0.75 * a + 0.2 * d, 0.02, 1.2);
  general[2] = 0.2 + 0.8 * d;
  general[9] = 0.03 + 0.6 * d * (1 - 0.6 * a);
  general[10] = general[9] * 0.2;
  general[11] = general[9] * 0.35;

  const ownAccuracy = base.CSRA ? base.CSRA.readFloatLE(0) : 0.75;
  const aim = robot ? Math.max(ownAccuracy, ownAccuracy * (0.9 + 0.2 * d)) : accuracy;

  const close = floats(base.CSCR, 11, 0);
  const throwMax = base.CSCR && base.CSCR.length >= 40 ? base.CSCR.readUInt32LE(36) : 0;
  close[0] = 0.15 + 0.4 * c;
  close[1] = 0.05 + 0.4 * d * (1 - a) + (mood === 'Shaken' ? 0.35 : 0);
  close[2] = mood === 'Flank' ? 1.2 : 0.15 + 0.85 * c;
  close[3] = 0.05 + 0.6 * c * (1 - 0.5 * a);
  close[4] = melee ? Math.max(close[4], 0.5 + 0.5 * a) : 0.1 + 0.4 * a;
  close[6] = melee ? Math.max(close[6], 0.4 + 0.6 * a) : 0.05 + 0.5 * a;
  if (melee) close[8] = 0.02 + 0.2 * d * (1 - a);
  close[10] = 0.4 + 0.55 * c;

  const long = floats(base.CSLR, 5, 0.5);
  long[0] = Math.min(1.3, 0.4 + 0.6 * c + 0.3 * (1 - d));
  long[1] = 0.5 + 0.6 * d;
  long[2] = 0.1 + 0.8 * d * (1 - a);
  long[3] = 0.05 + 0.85 * d * (1 - a) + (mood === 'Shaken' ? 0.3 : 0);
  if (range !== null) long[4] = range;
  let cover = 0.4 + 1.6 * d * (1 - 0.6 * a);
  if (mood === 'Shaken') cover *= 1.4;
  if (mood === 'Rage') cover = 0.05;
  if (heavy) { cover *= 0.3; long[2] *= 0.2; long[3] *= 0.5; }

  let flags = base.DATA ? base.DATA.readUInt32LE(0) & ~(kFlanking | kCharging) : 1;
  if (c >= (heavy ? 0.8 : 0.5)) flags |= kFlanking;
  if (melee || a >= 0.8) flags |= kCharging;

  const closeData = f32(close);
  closeData.writeUInt32LE(throwMax, 36);
  const flagData = Buffer.alloc(4);
  flagData.writeUInt32LE(flags);
  return [
    ['CSGD', f32(general)],
    ...(base.CSMD ? [['CSMD', base.CSMD]] : []),
    ['CSME', f32(floats(base.CSME, 10, 1))],
    ['CSRA', f32([aim])],
    ['CSCR', closeData],
    ['CSLR', f32(long)],
    ['CSCV', f32([cover])],
    ...(base.CSFL ? [['CSFL', base.CSFL]] : []),
    ['DATA', flagData],
  ];
}

const records = [];
const tableRows = [];
let nextId = FIRST_TACTICS_ID;
for (const [name, kind, shape, a, d, c, fear, accuracy, range] of TACTICS) {
  const f = (v) => `${v.toFixed(2)}f`;
  tableRows.push(`    {"${name}", Kind::${kind}, ${f(a)}, ${f(d)}, ${f(c)}, ${f(fear)}, ${shape.variants.length}, ${shape.moods.length}, 0x${nextId.toString(16).toUpperCase()}},`);
  for (const [variant, baseId] of shape.variants) {
    const base = bases.get(baseId);
    if (!base) throw new Error(`base style ${baseId.toString(16)} isn't in the source`);
    for (const mood of shape.moods) {
      const edid = `RD_cs${name}${variant}${mood}`;
      const data = Buffer.concat([sub('EDID', zstr(edid)), ...tacticsStyle(base, kind, variant, mood, [a, d, c], accuracy, range).map(([t, v]) => sub(t, v))]);
      const head = Buffer.alloc(24);
      head.write('CSTY', 0, 'latin1');
      head.writeUInt32LE(data.length, 4);
      head.writeUInt32LE(0x01000000 | nextId, 12); // index 1: this plugin, after its one master
      recordHead.copy(head, 16);
      records.push(Buffer.concat([head, data]));
      nextId++;
    }
  }
}

// The new styles at the end of the combat styles group, and the header's count and next id after them
const tactics = Buffer.concat(records);
const grown = Buffer.concat([body.subarray(0, cstyGroup.end), tactics, body.subarray(cstyGroup.end)]);
grown.writeUInt32LE(cstyGroup.end - cstyGroup.start + tactics.length, cstyGroup.start + 4);
for (let o = 0; o < headerData.length;) {
  const size = headerData.readUInt16LE(o + 4);
  if (headerData.toString('latin1', o, o + 4) === 'HEDR') {
    headerData.writeUInt32LE(headerData.readUInt32LE(o + 10) + records.length, o + 10);
    headerData.writeUInt32LE(Math.max(headerData.readUInt32LE(o + 14), nextId), o + 14);
  }
  o += 6 + size;
}

fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, Buffer.concat([header, headerData, grown]));
console.log(`wrote ${out}, ${fs.statSync(out).size} bytes, ${records.length} tactics styles 0x${FIRST_TACTICS_ID.toString(16)}..0x${(nextId - 1).toString(16)}`);

fs.writeFileSync(TABLE_OUT, `// Written by Tools/BuildCombatPlugin.js with RD_Combat.esp: change the table there and run it again, never this file.
// The enemy tactics profiles (docs/ENEMY_TACTICS.md) and where their combat styles start in RD_Combat.esp. A profile's
// styles are its variants in order, each with its moods in order: FirstStyle + variant * MoodCount + mood.
#pragma once

#include <cstdint>

namespace Tactics
{
enum class Kind : uint8_t
{
    kPerson, // variants ranged, melee, heavy; moods steady, press, shaken, flank
    kMutant, // variants ranged, melee (both from their vanilla style); moods steady, rage
    kSynth,  // variants ranged, melee; moods steady, flank
    kRobot,  // its own race's style; moods steady, flank
};

struct Profile
{
    const char* Name;
    Kind Type;
    float Aggression;
    float Discipline;
    float Cunning;
    float Fearlessness;
    uint8_t VariantCount;
    uint8_t MoodCount;
    uint32_t FirstStyle; // local id in RD_Combat.esp
};

inline constexpr Profile kProfiles[] = {
${tableRows.join('\n')}
};
} // namespace Tactics
`);
console.log(`wrote ${TABLE_OUT}`);
