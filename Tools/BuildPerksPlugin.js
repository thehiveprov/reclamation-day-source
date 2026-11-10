// Builds GameFiles/Fallout4/RD_Perks.esp, Reclamation Day's perks: a light plugin (ESL flag) with Fallout4.esm as its
// only master. Its perks have no entry points of their own: the client gives them and acts on them
// (Code/client/Games/Fallout4/BetterInteract.cpp).
//   0x800 RD_AnalyticalEye "Analytical Eye": given at Intelligence 6 (permanent value); while the player has it,
//         what Better Interact picks glows the way VATS shows its targets, instead of the outline.
// usage: node Tools/BuildPerksPlugin.js [out.esp]
const fs = require('fs');
const path = require('path');

const out = process.argv[2] || path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'RD_Perks.esp');
const AUTHOR = 'Reclamation Day';
const DESCRIPTION = "Reclamation Day's perks.";
const FORM_VERSION = 131;
const LIGHT = 0x200;

const PERKS = [
  {
    id: 0x800,
    edid: 'RD_AnalyticalEye',
    name: 'Analytical Eye',
    description: 'Your trained mind picks out what matters. Whatever you would use next glows, the way V.A.T.S. shows its targets.',
    icon: 'Components\\VaultBoys\\Perks\\PerkClip_Default.swf',
  },
];

const zstr = s => Buffer.from(s + '\0', 'latin1');
const sub = (type, data) => {
  const head = Buffer.alloc(6);
  head.write(type, 0, 'latin1');
  head.writeUInt16LE(data.length, 4);
  return Buffer.concat([head, data]);
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

// A perk: one rank, playable (shown in the Pip-Boy's perk list), not a trait, no level of its own
const perk = p => {
  const data = Buffer.from([0, 0, 1, 1, 0]); // trait, level, ranks, playable, hidden
  return record('PERK', 0, 0x01000000 | p.id,
                Buffer.concat([sub('EDID', zstr(p.edid)), sub('FULL', zstr(p.name)), sub('DESC', zstr(p.description)),
                               sub('DATA', data), sub('FNAM', zstr(p.icon))]));
};

const groups = [group('PERK', PERKS.map(perk))];
const recordCount = PERKS.length + groups.length;

const hedr = Buffer.alloc(12);
hedr.writeFloatLE(0.95, 0);
hedr.writeUInt32LE(recordCount, 4);
hedr.writeUInt32LE(Math.max(...PERKS.map(p => p.id)) + 1, 8);
const header = record('TES4', LIGHT, 0,
                      Buffer.concat([sub('HEDR', hedr), sub('CNAM', zstr(AUTHOR)), sub('SNAM', zstr(DESCRIPTION)),
                                     sub('MAST', zstr('Fallout4.esm')), sub('DATA', Buffer.alloc(8))]));

fs.writeFileSync(out, Buffer.concat([header, ...groups]));
console.log(`${out}: ${PERKS.length} perk(s), ${fs.statSync(out).size} bytes`);
