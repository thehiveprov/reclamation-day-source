// Builds GameFiles/Fallout4/RD_Camps.esp, Reclamation Day's C.A.M.P.s (like Fallout 76's): a light plugin (ESL flag)
// with Fallout4.esm as its only master. No scripts: the client does the rest (Code/client/Services/Generic/
// WorkshopCamps.cpp).
//   0x800 RD_CampModule "Makeshift CAMP device": the workbench a player's C.A.M.P. is built around, placed where they
//         use the device. A workshop as Build Anywhere's container is (its keywords and workshop ref type, the vanilla
//         WorkshopWorkbench's without its WorkshopScript), so build mode works at it anywhere. Its model is the old
//         Vault-Tec C.A.M.P. device standing on a garage cart (GameFiles/Fallout4/meshes/ReclamationDay/Camp/
//         CampModule.nif, docs/CAMPS.md).
//   0x801 RD_CampDevice "Makeshift CAMP device": an Aid item every player carries, the device alone (CampDevice.nif).
//         Using it from the Pip-Boy places, moves or picks up their C.A.M.P.; the client stops the game from using it
//         up, so it has no effects.
// usage: node Tools/BuildCampsPlugin.js [out.esp]
const fs = require('fs');
const path = require('path');

const out = process.argv[2] || path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'RD_Camps.esp');
const AUTHOR = 'Reclamation Day';
const DESCRIPTION = "Reclamation Day's C.A.M.P.s.";
const FORM_VERSION = 131;
const LIGHT = 0x200;

// Fallout4.esm: WorkshopKeyword and the workbench types build mode needs, as on WorkshopWorkbench (0xC1AEB)
const WORKSHOP_KEYWORDS = [0x4af68, 0x54ba7, 0x5a0c8, 0x5a0c9, 0x5a0ca, 0x5b5e3, 0x8280b, 0x91fd4, 0x12e2c8];
const WORKSHOP_REF_TYPE = 0x234e9;

const MODULE = {
  id: 0x800,
  edid: 'RD_CampModule',
  name: 'Makeshift CAMP device',
  model: 'ReclamationDay\\Camp\\CampModule.nif',
  bounds: [-45, -30, 0, 45, 30, 122], // the cart with the device on it
};
const DEVICE = {
  id: 0x801,
  edid: 'RD_CampDevice',
  name: 'Makeshift CAMP device',
  description: "Found an old Vault-Tec C.A.M.P device. Let's see what we can do with this.",
  model: 'ReclamationDay\\Camp\\CampDevice.nif',
  bounds: [-38, -29, 0, 38, 29, 58], // the device's
};

const zstr = s => Buffer.from(s + '\0', 'latin1');
const sub = (type, data) => {
  const head = Buffer.alloc(6);
  head.write(type, 0, 'latin1');
  head.writeUInt16LE(data.length, 4);
  return Buffer.concat([head, data]);
};
const u32 = value => {
  const b = Buffer.alloc(4);
  b.writeUInt32LE(value >>> 0, 0);
  return b;
};
const f32 = value => {
  const b = Buffer.alloc(4);
  b.writeFloatLE(value, 0);
  return b;
};
const obnd = bounds => {
  const b = Buffer.alloc(12);
  bounds.forEach((value, i) => b.writeInt16LE(value, i * 2));
  return b;
};
// record header: type, data size, flags, form ID, version control, form version, unknown
const record = (type, flags, formId, data) => {
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

// The module: a container (no items, weight 0) that is a workshop
const campModule = m =>
  record('CONT', 0, 0x01000000 | m.id,
         Buffer.concat([sub('EDID', zstr(m.edid)), sub('OBND', obnd(m.bounds)), sub('FULL', zstr(m.name)), sub('MODL', zstr(m.model)),
                        sub('DATA', Buffer.alloc(5)), sub('KSIZ', u32(WORKSHOP_KEYWORDS.length)),
                        sub('KWDA', Buffer.concat(WORKSHOP_KEYWORDS.map(u32))), sub('FTYP', u32(WORKSHOP_REF_TYPE))]));

// The device: an Aid item as the Stimpak's record is laid out (weight 0; ENIT: value, flags, addiction, addiction
// chance, consume sound; no effects)
const device = d =>
  record('ALCH', 0, 0x01000000 | d.id,
         Buffer.concat([sub('EDID', zstr(d.edid)), sub('OBND', obnd(d.bounds)), sub('FULL', zstr(d.name)), sub('MODL', zstr(d.model)),
                        sub('DESC', zstr(d.description)), sub('DATA', f32(0)), sub('ENIT', Buffer.alloc(20)), sub('DNAM', u32(0))]));

const groups = [group('CONT', [campModule(MODULE)]), group('ALCH', [device(DEVICE)])];
const recordCount = 2 + groups.length;

const hedr = Buffer.alloc(12);
hedr.writeFloatLE(0.95, 0);
hedr.writeUInt32LE(recordCount, 4);
hedr.writeUInt32LE(Math.max(MODULE.id, DEVICE.id) + 1, 8);
const header = record('TES4', LIGHT, 0,
                      Buffer.concat([sub('HEDR', hedr), sub('CNAM', zstr(AUTHOR)), sub('SNAM', zstr(DESCRIPTION)),
                                     sub('MAST', zstr('Fallout4.esm')), sub('DATA', Buffer.alloc(8))]));

fs.writeFileSync(out, Buffer.concat([header, ...groups]));
console.log(`${out}: C.A.M.P. module and device, ${fs.statSync(out).size} bytes`);
