// Builds GameFiles/Fallout4/RD_DualWield.esp, Reclamation Day's dual wielding: a light plugin (ESL flag) with
// Fallout4.esm as its only master. The client does the dual wielding (Code/client/Services/Generic/DualWieldService.cpp,
// docs/DUAL_WIELD.md); these globals keep the weapon in the left hand in the save (the save's global variables table
// holds every global's value, so writing the value is all it takes):
//   0x800 GLOB RD_LeftHandPlugin: the weapon's plugin, a 24-bit hash of its file name (0: the left hand is empty)
//   0x801 GLOB RD_LeftHandForm: the weapon's form id in its plugin (24 bits, 12 for a light plugin)
//   0x802 GLOB RD_LeftHandMods: the stack's object mods, a 24-bit hash of their plugins and ids (in any order)
// A float holds every integer up to 2^24 exactly. Plugin-relative, so a changed load order finds the same weapon.
// usage: node Tools/BuildDualWieldPlugin.js [out.esp]
const fs = require('fs');
const path = require('path');

const out = process.argv[2] || path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'RD_DualWield.esp');
const AUTHOR = 'Reclamation Day';
const DESCRIPTION = "Reclamation Day's dual wielding.";
const FORM_VERSION = 131;
const LIGHT = 0x200;
const OWN = 0x01000000;

const GLOBALS = [
  { id: 0x800, edid: 'RD_LeftHandPlugin' },
  { id: 0x801, edid: 'RD_LeftHandForm' },
  { id: 0x802, edid: 'RD_LeftHandMods' },
];

const zstr = s => Buffer.from(s + '\0', 'latin1');
const sub = (type, data) => {
  const head = Buffer.alloc(6);
  head.write(type, 0, 'latin1');
  head.writeUInt16LE(data.length, 4);
  return Buffer.concat([head, data]);
};
const f32 = value => {
  const b = Buffer.alloc(4);
  b.writeFloatLE(value, 0);
  return b;
};
// record header: type, data size, flags, form ID, version control, form version, unknown
const record = (type, flags, formId, subs) => {
  const data = Buffer.concat(subs);
  const head = Buffer.alloc(24);
  head.write(type, 0, 'latin1');
  head.writeUInt32LE(data.length, 4);
  head.writeUInt32LE(flags, 8);
  head.writeUInt32LE(formId, 12);
  head.writeUInt16LE(FORM_VERSION, 20);
  return Buffer.concat([head, data]);
};
// top group: size includes its 24-byte header, label = the record type
const group = (label, records) => {
  const body = Buffer.concat(records);
  const head = Buffer.alloc(24);
  head.write('GRUP', 0, 'latin1');
  head.writeUInt32LE(24 + body.length, 4);
  head.write(label, 8, 'latin1');
  head.writeUInt16LE(FORM_VERSION, 20);
  return Buffer.concat([head, body]);
};

// GLOB: a float ('f'), not constant (no record flag 0x40), 0 to start with
const globals = GLOBALS.map(g => record('GLOB', 0, OWN | g.id, [sub('EDID', zstr(g.edid)), sub('FNAM', Buffer.from('f', 'latin1')),
  sub('FLTV', f32(0))]));
const groups = [group('GLOB', globals)];
const recordCount = globals.length + groups.length;

const hedr = Buffer.alloc(12);
hedr.writeFloatLE(0.95, 0);
hedr.writeUInt32LE(recordCount, 4);
hedr.writeUInt32LE(Math.max(...GLOBALS.map(g => g.id)) + 1, 8);
const header = record('TES4', LIGHT, 0, [sub('HEDR', hedr), sub('CNAM', zstr(AUTHOR)), sub('SNAM', zstr(DESCRIPTION)),
  sub('MAST', zstr('Fallout4.esm')), sub('DATA', Buffer.alloc(8))]);

fs.writeFileSync(out, Buffer.concat([header, ...groups]));
console.log(`${out}: ${globals.length} globals, ${fs.statSync(out).size} bytes`);
