// Builds GameFiles/Fallout4/RD_GrapplingGun.esp, Reclamation Day's grappling gun: a light plugin (ESL flag) with
// Fallout4.esm as its only master. The client does the grappling (Code/client/Services/Generic/GrappleService.cpp,
// docs/GRAPPLE.md); these are the item, its ammo and the recipes.
//   0x800 WEAP RD_GrapplingGun "Grappling Gun": the flare gun's record (a standalone gun, no mod slots) with our model
//         (meshes/ReclamationDay/GrapplingGun/GrapplingGun.nif, both views) and the 10mm pistol's first person
//         animations (AnimsFP10mm, WeaponTypePistol); one spike at a time.
//   0x801 AMMO RD_GrappleSpike "Grapple Spike": reaches 45 m (3150 units)
//   0x802 AMMO RD_GrappleSpikeHV "High Velocity Grapple Spike": reaches 60 m (4200 units)
//   0x803 / 0x804 PROJ: the spikes in flight, the railway rifle's spike projectile (an arrow: straight, no gravity)
//         with our model, no explosion, the reach above as range
//   0x805 GLOB RD_GrappleBlueprint: 1 once the player has the blueprint (the 2287 quest's reward, docs/GRAPPLE.md)
//   0x806-0x808 COBJ at the chemistry station (Utility): the gun and five spikes need the blueprint; five high
//         velocity spikes also need Gunsmith rank 2 (Fallout4.esm's Gun Nut 02, which Be Exceptional renames)
//   0x809 / 0x80A STAT: what the client places for a shot (the game's own projectile flies only to deal a hit on someone
//         the spike bites): the spike as it sits on the gun (GrappleSpikeVisual.nif: the projectile's mesh without its
//         collision, which the ledge rays at the anchor would find) and the steel cable from the gun to it
//         (GrappleCable.nif, a unit tube the client stretches)
//   0x80B KYWD ap_RD_GrappleChamber: the gun's "Spike Chamber" mod slot (an attach point, on the gun's APPR)
//   0x80C KYWD ma_RD_GrapplingGun: the gun's mod association (on the gun, the chamber mods' MNAM)
//   0x80D / 0x80E OMOD: the chambers, Standard (the gun's own spikes) and High Velocity (fires the High Velocity spikes:
//         the ammo property set, as the pipe guns' ammo conversion receivers do)
//   0x80F / 0x810 COBJ at the weapons workbench: the two chambers (the blueprint; High Velocity also Gunsmith rank 2)
//   0x811 / 0x812 MISC: the chambers as loose mods (what one taken off at the bench becomes)
//   0x813-0x86F: the 2287 quest that gives the blueprint (the Beatsville wanderer, the wreck of the USS Germination at
//         Long Wharf, Dr. Yarrow's holotape): Tools/GrappleQuest.js
// The game-side numbers (record layouts) were read from Fallout4.esm 1.10.163: the flare gun (0x1025AC), the 10mm
// (0x4822), the railway spike ammo and projectile (0xFE269, 0xFE26A) and the chemistry station's recipes.
// usage: node Tools/BuildGrapplePlugin.js [out.esp] [Fallout4.esm] (the quest copies a few records out of the master)
const fs = require('fs');
const path = require('path');
const GrappleQuest = require('./GrappleQuest.js');

const out = process.argv[2] || path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'RD_GrapplingGun.esp');
const esmPath = process.argv[3] || process.env.FALLOUT4_ESM || 'F:/SteamLibrary/steamapps/common/Fallout 4/Data/Fallout4.esm';
const AUTHOR = 'Reclamation Day';
const DESCRIPTION = "Reclamation Day's grappling gun.";
const FORM_VERSION = 131;
const LIGHT = 0x200;
const OWN = 0x01000000;

const ID = {
  gun: 0x800, spike: 0x801, spikeHv: 0x802, projectile: 0x803, projectileHv: 0x804, blueprint: 0x805,
  recipeGun: 0x806, recipeSpike: 0x807, recipeSpikeHv: 0x808, spikeVisual: 0x809, cableVisual: 0x80a,
  apChamber: 0x80b, maGun: 0x80c, modChamberStandard: 0x80d, modChamberHv: 0x80e, recipeChamberStandard: 0x80f,
  recipeChamberHv: 0x810, looseChamberStandard: 0x811, looseChamberHv: 0x812,
};

// Fallout4.esm forms
const FO4 = {
  // keywords: the flare gun's, with the 10mm's first person animations and pistol type in place of the flare gun's
  WeaponTypeBallistic: 0x092a86, AnimsGripPistol: 0x01f948, ObjectTypeWeapon: 0x0f4aea, AnimsFP10mm: 0x04c7ea,
  QuickkeyGun: 0x10c485, s_IronSightsPistol: 0x03adb3, s_40_PistolFlareGun: 0x1163b6, WeaponTypePistol: 0x04a0a0,
  WeaponTypePossiblyVerySmallClipSize: 0x249fd5,
  BothHandsLeftOptional: 0x04334d,   // the 10mm's equip type
  WorkbenchChemlab: 0x102158, RecipeUtility: 0x06980c,
  c_Steel: 0x01fabd, c_Springs: 0x01fabc, c_Screws: 0x03d294, c_Gears: 0x01fab0, c_Copper: 0x01fa9c,
  c_Aluminum: 0x01fa91, c_Oil: 0x01fab4, c_Adhesive: 0x01faa5,
  GunNut02: 0x04a0db, PlayerRef: 0x000014,
  ap_Legendary: 0x1e32c8,   // the flare gun's one mod slot (APPR)
};

const GUN = {
  edid: 'RD_GrapplingGun', name: 'Grappling Gun',
  model: 'ReclamationDay\\GrapplingGun\\GrapplingGun.nif',
  bounds: [-4, -5, -11, 4, 31, 7],   // GrapplingGun.nif's, as exported
  damage: 15, weight: 3.5, value: 180, apCost: 35, reloadSpeed: 1.0,
};
const SPIKES = [
  { id: ID.spike, projectile: ID.projectile, edid: 'RD_GrappleSpike', name: 'Grapple Spike', shortName: 'Spike', value: 3,
    speed: 5000, range: 3150 },
  { id: ID.spikeHv, projectile: ID.projectileHv, edid: 'RD_GrappleSpikeHV', name: 'High Velocity Grapple Spike', shortName: 'HV Spike',
    value: 6, speed: 7000, range: 4200 },
];
const SPIKE_MODEL = 'ReclamationDay\\GrapplingGun\\GrappleSpikeProjectile.nif';
const SPIKE_BOUNDS = [-4, 0, -4, 4, 9, 4];   // the gun's own spike, barbs deployed (GrappleSpikeProjectile.nif)

// Vanilla subrecords copied as they are, then the fields named below changed
const HEX = {
  // FlareGun
  gunPtrn: 'dd1d0700', gunBids: 'ff830100', gunYnam: '23490300', gunZnam: 'c3f50100', gunAppr: 'c8321e00',
  gunDnam: 'ae2510000000803f3333b33f0000803f00a0024500f043459a99193e000000000000003f000000000000000000000000000100000100090000c04000000040320000000a0004000000000000000000000000000000cdda01000000000000000000ac2615000000000000abaa2a3f000000002042000000000000000001000000ffff7f7f',
  gunFnam: 'acc527379a99993e9a99993ecdcc4c3e6b5585405855953f9a99193e01000000000000000000000000',
  gunCrdt: '000000400000803f00000000', gunInam: '374b0000', gunWamd: '51e11900',
  // AmmoRRSpike
  ammoPtrn: '3dca2300', ammoYnam: 'df470300', ammoZnam: 'e0470300', ammoKwda: 'e84a0f00',
  // RailwayRifleProjectile
  projDnam: '08014000000000000000fa4500606a4600000000000000000000000000000000085e210065d501000ad7a33d0000003f00002041000000000000000000000000000000000000803f0000003f0000803e00000000000000000000000000',
  projNam1: '576561706f6e735c42726f616473696465725c5261696c5269666c654d757a7a6c65466c6173682e6e696600',
  projNam2: '040000000000000001000000000000000000000096000000',
};
const hex = s => Buffer.from(s, 'hex');

const zstr = s => Buffer.from(s + '\0', 'latin1');
const sub = (type, data) => {
  const head = Buffer.alloc(6);
  head.write(type, 0, 'latin1');
  head.writeUInt16LE(data.length, 4);
  return Buffer.concat([head, data]);
};
const u16 = value => {
  const b = Buffer.alloc(2);
  b.writeUInt16LE(value, 0);
  return b;
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

// WEAP DNAM (132 bytes, form version 131): ammo 0, speed 4, reload speed 8, reach 12, min/max range 16/20, attack
// delay 24, ?, out of range damage 32, on hit 36, skill 40, resist 44, flags 48, capacity 52 (u16), animation type 54
// (u8), secondary damage 55, weight 59, value 63 (u32), base damage 67 (u16), sound level 69, eight sounds 73-104,
// accuracy bonus 105 (u8), attack animation seconds 106, ? (u16) 110, AP cost 112, full power seconds 116, min power
// per shot 120, stagger 124, ? 128
function gunDnam() {
  const d = hex(HEX.gunDnam);
  d.writeUInt32LE(OWN | ID.spike, 0);
  d.writeFloatLE(GUN.reloadSpeed, 8);
  d.writeUInt16LE(1, 52);
  d.writeFloatLE(GUN.weight, 59);
  d.writeUInt32LE(GUN.value, 63);
  d.writeUInt16LE(GUN.damage, 67);
  d.writeFloatLE(GUN.apCost, 112);
  return d;
}

const keywords = ['WeaponTypeBallistic', 'AnimsGripPistol', 'ObjectTypeWeapon', 'AnimsFP10mm', 'QuickkeyGun', 's_IronSightsPistol',
  's_40_PistolFlareGun', 'WeaponTypePistol', 'WeaponTypePossiblyVerySmallClipSize'].map(k => FO4[k]).concat([OWN | ID.maGun]);

const gun = record('WEAP', 0, OWN | ID.gun, [
  sub('EDID', zstr(GUN.edid)), sub('OBND', obnd(GUN.bounds)), sub('PTRN', hex(HEX.gunPtrn)), sub('FULL', zstr(GUN.name)),
  sub('MODL', zstr(GUN.model)), sub('ETYP', u32(FO4.BothHandsLeftOptional)), sub('BIDS', hex(HEX.gunBids)),
  sub('YNAM', hex(HEX.gunYnam)), sub('ZNAM', hex(HEX.gunZnam)), sub('KSIZ', u32(keywords.length)),
  sub('KWDA', Buffer.concat(keywords.map(u32))), sub('DESC', zstr('')), sub('APPR', Buffer.concat([u32(FO4.ap_Legendary), u32(OWN | ID.apChamber)])),
  sub('MOD4', zstr(GUN.model)), sub('DNAM', gunDnam()), sub('FNAM', hex(HEX.gunFnam)), sub('CRDT', hex(HEX.gunCrdt)),
  sub('INAM', hex(HEX.gunInam)), sub('WAMD', hex(HEX.gunWamd)),
]);

// AMMO: DATA = value, weight; DNAM = projectile, flags, damage (0: the gun's), health
const ammo = s => record('AMMO', 0, OWN | s.id, [
  sub('EDID', zstr(s.edid)), sub('OBND', obnd(SPIKE_BOUNDS)), sub('PTRN', hex(HEX.ammoPtrn)), sub('FULL', zstr(s.name)),
  sub('MODL', zstr(SPIKE_MODEL)), sub('YNAM', hex(HEX.ammoYnam)), sub('ZNAM', hex(HEX.ammoZnam)), sub('DESC', zstr('')),
  sub('KSIZ', u32(1)), sub('KWDA', hex(HEX.ammoKwda)), sub('DATA', Buffer.concat([u32(s.value), f32(0.25)])),
  sub('DNAM', Buffer.concat([u32(OWN | s.projectile), Buffer.alloc(12)])), sub('ONAM', zstr(s.shortName)),
]);

// PROJ DNAM (93 bytes): flags 0 (u16), type 2 (u16: 0x40 arrow, 0x01 missile), gravity 4, speed 8, range 12, light
// 16, muzzle flash light 20, proximity 24, timer 28, explosion 32, sound 36, muzzle flash duration 40, fade 44, impact
// force 48, ... The grapple's code casts its own ray along the shot to the same range, so the spike lands where the code
// looks: no gravity, no explosion. The projectile itself flies only for the spike's hit on someone it bites
// (GrappleService::HitHooked, from just short of them): a missile, which doesn't stick in them as the railway rifle's
// arrow does (the spike seen is the line's own), without the railway spike's flags (0x100 pins limbs, 0x8 a muzzle
// flash at the gun, which the shot right by them would set off)
const PROJ_MISSILE = 0x01;
const projectile = s => {
  const d = hex(HEX.projDnam);
  d.writeUInt16LE(0, 0);
  d.writeUInt16LE(PROJ_MISSILE, 2);
  d.writeFloatLE(0, 4);
  d.writeFloatLE(s.speed, 8);
  d.writeFloatLE(s.range, 12);
  d.writeUInt32LE(0, 32);
  return record('PROJ', 0, OWN | s.projectile, [
    sub('EDID', zstr(s.edid + 'Projectile')), sub('OBND', obnd(SPIKE_BOUNDS)), sub('FULL', zstr(s.name)),
    sub('MODL', zstr(SPIKE_MODEL)), sub('DATA', Buffer.alloc(0)), sub('DNAM', d), sub('NAM1', hex(HEX.projNam1)),
    sub('NAM2', hex(HEX.projNam2)), sub('VNAM', u32(1)),
  ]);
};

const blueprint = record('GLOB', 0, OWN | ID.blueprint, [sub('EDID', zstr('RD_GrappleBlueprint')), sub('FNAM', Buffer.from('s', 'latin1')),
  sub('FLTV', f32(0))]);

// CTDA (32 bytes): type (operator << 5), value, function (u16) + padding, two parameters, run on, reference, a third
// parameter (-1). Operators: 0 ==, 3 >=. GetGlobalValue = 74, HasPerk = 448 (on PlayerRef: run on 2, a reference)
const condition = (op, value, fn, param, runOn = 0, reference = 0) => {
  const b = Buffer.alloc(32);
  b.writeUInt8(op << 5, 0);
  b.writeFloatLE(value, 4);
  b.writeUInt16LE(fn, 8);
  b.writeUInt32LE(param >>> 0, 12);
  b.writeUInt32LE(runOn, 20);
  b.writeUInt32LE(reference >>> 0, 24);
  b.writeInt32LE(-1, 28);
  return sub('CTDA', b);
};
const hasBlueprint = condition(0, 1, 74, OWN | ID.blueprint);
const recipe = (id, edid, created, count, components, conditions) => record('COBJ', 0, OWN | id, [
  sub('EDID', zstr(edid)),
  sub('FVPA', Buffer.concat(components.flatMap(([form, n]) => [u32(FO4[form]), u32(n)]))),
  ...conditions, sub('CNAM', u32(OWN | created)), sub('BNAM', u32(FO4.WorkbenchChemlab)), sub('FNAM', u32(FO4.RecipeUtility)),
  sub('INTV', Buffer.concat([u16(count), u16(0)])),
]);
const recipes = [
  recipe(ID.recipeGun, 'RD_co_GrapplingGun', ID.gun, 1,
         [['c_Steel', 10], ['c_Springs', 3], ['c_Screws', 4], ['c_Gears', 3], ['c_Copper', 4], ['c_Adhesive', 2]], [hasBlueprint]),
  recipe(ID.recipeSpike, 'RD_co_GrappleSpike', ID.spike, 5, [['c_Steel', 2], ['c_Screws', 1]], [hasBlueprint]),
  recipe(ID.recipeSpikeHv, 'RD_co_GrappleSpikeHV', ID.spikeHv, 5, [['c_Steel', 2], ['c_Aluminum', 1], ['c_Oil', 1]],
         [hasBlueprint, condition(3, 1, 448, FO4.GunNut02, 2, FO4.PlayerRef)]),
];

// STAT DNAM: max angle 90, no directional material, leaf amplitude and frequency 1 (as vanilla cable statics)
const visual = (id, edid, model, bounds) => record('STAT', 0, OWN | id, [
  sub('EDID', zstr(edid)), sub('OBND', obnd(bounds)), sub('MODL', zstr(model)),
  sub('DNAM', Buffer.concat([f32(90), u32(0), f32(1), f32(1)])),
]);
const visuals = [
  visual(ID.spikeVisual, 'RD_GrappleSpikeVisual', 'ReclamationDay\\GrapplingGun\\GrappleSpikeVisual.nif', SPIKE_BOUNDS),
  visual(ID.cableVisual, 'RD_GrappleCableVisual', 'ReclamationDay\\GrapplingGun\\GrappleCable.nif', [-1, 0, -1, 1, 1, 1]),
];

// KYWD: CNAM a colour, TNAM its type (2 an attach point, with NNAM the slot's name; 5 a mod association), as
// Fallout4.esm's ap_gun_receiver and ma_PipeRevolver
const keyword = (id, edid, type, slotName) => record('KYWD', 0, OWN | id, [
  sub('EDID', zstr(edid)), sub('CNAM', hex('ffffff00')), sub('TNAM', u32(type)), ...(slotName ? [sub('NNAM', zstr(slotName))] : []),
]);
const keywordRecords = [keyword(ID.apChamber, 'ap_RD_GrappleChamber', 2, 'Spike Chamber'), keyword(ID.maGun, 'ma_RD_GrapplingGun', 5)];

// OMOD DATA (read from Fallout4.esm's mod_PipeRevolver_Receiver_Standard and _AmmoConversion): include count, property
// count, two flags, the form type it mods ('WEAP'), max rank, level tier offset, the attach point keyword, the attach
// parent slots it adds (a count, keywords), items (a count), the includes, then the properties, 24 bytes each: value
// type (u8: 1 float, 4 a form id and an int), function (u8: 0 set, 1 multiply and add, 2 add), the property (u16: 61
// the ammo), value 1 at 12, value 2 at 16 (0x1B8 in every vanilla form id property, kept), a step at 20
const PROPERTY_AMMO = 61;
const setForm = (property, form) => {
  const b = Buffer.alloc(24);
  b.writeUInt8(4, 0);
  b.writeUInt8(0, 4);
  b.writeUInt16LE(property, 8);
  b.writeUInt32LE(form >>> 0, 12);
  b.writeUInt32LE(0x1b8, 16);
  return b;
};
const omodData = (attachPoint, properties) => Buffer.concat([
  u32(0), u32(properties.length), Buffer.from([0, 0]), Buffer.from('WEAP', 'latin1'), Buffer.from([0, 0]), u32(attachPoint), u32(0), u32(0),
  ...properties,
]);
// the loose mod a chamber taken off at the bench goes into the inventory as (OMOD LNAM): a MISC as Fallout4.esm's
// miscmod_ records, the mod box model, its keyword (0x135C17), value and weight
const LOOSE_MOD = { bounds: [-8, -6, 0, 8, 7, 7], ptrn: 'f9882400', model: 'Props\\ModsPartbox\\ModBox.nif', keyword: 0x135c17 };
const looseMod = (id, edid, name, value) => record('MISC', 0, OWN | id, [
  sub('EDID', zstr(edid)), sub('OBND', obnd(LOOSE_MOD.bounds)), sub('PTRN', hex(LOOSE_MOD.ptrn)), sub('FULL', zstr(name)),
  sub('MODL', zstr(LOOSE_MOD.model)), sub('KSIZ', u32(1)), sub('KWDA', u32(LOOSE_MOD.keyword)),
  sub('DATA', Buffer.concat([u32(value), f32(0.5)])),
]);
const chamber = (id, loose, edid, name, description, properties) => record('OMOD', 0, OWN | id, [
  sub('EDID', zstr(edid)), sub('FULL', zstr(name)), sub('DESC', zstr(description)), sub('DATA', omodData(OWN | ID.apChamber, properties)),
  sub('MNAM', u32(OWN | ID.maGun)), sub('LNAM', u32(OWN | loose)),
]);
const looseMods = [
  looseMod(ID.looseChamberStandard, 'RD_miscmod_GrappleChamber_Standard', 'Standard Spike Chamber', 10),
  looseMod(ID.looseChamberHv, 'RD_miscmod_GrappleChamber_HighVelocity', 'High Velocity Spike Chamber', 45),
];
const chambers = [
  chamber(ID.modChamberStandard, ID.looseChamberStandard, 'RD_mod_GrappleChamber_Standard', 'Standard Spike Chamber',
          'Fires Grapple Spikes: 45 m of cable.', []),
  chamber(ID.modChamberHv, ID.looseChamberHv, 'RD_mod_GrappleChamber_HighVelocity', 'High Velocity Spike Chamber',
          'Fires High Velocity Grapple Spikes: 60 m of cable.', [setForm(PROPERTY_AMMO, OWN | ID.spikeHv)]),
];
// the chambers' recipes, at the weapons workbench as any weapon mod's (no BNAM: Fallout4.esm's co_mod_ recipes have none)
const chamberRecipe = (id, edid, created, components, conditions) => record('COBJ', 0, OWN | id, [
  sub('EDID', zstr(edid)), sub('FVPA', Buffer.concat(components.flatMap(([form, n]) => [u32(FO4[form]), u32(n)]))),
  ...conditions, sub('CNAM', u32(OWN | created)), sub('INTV', Buffer.concat([u16(1), u16(0)])),
]);
const chamberRecipes = [
  chamberRecipe(ID.recipeChamberStandard, 'RD_co_mod_GrappleChamber_Standard', ID.modChamberStandard, [['c_Steel', 1]], [hasBlueprint]),
  chamberRecipe(ID.recipeChamberHv, 'RD_co_mod_GrappleChamber_HighVelocity', ID.modChamberHv,
                [['c_Steel', 3], ['c_Springs', 2], ['c_Screws', 2], ['c_Oil', 1]], [hasBlueprint, condition(3, 1, 448, FO4.GunNut02, 2, FO4.PlayerRef)]),
];

const quest = GrappleQuest.build({ sub, u8: v => Buffer.from([v]), u16, u32, i32: v => { const b = Buffer.alloc(4); b.writeInt32LE(v, 0); return b; }, f32, zstr, OWN },
                                  esmPath);

// top groups in the order the game's own files have them
const GROUP_ORDER = ['KYWD', 'GLOB', 'MISC', 'STAT', 'WEAP', 'AMMO', 'NPC_', 'NOTE', 'PROJ', 'TERM', 'WRLD', 'QUST', 'MESG', 'COBJ', 'OMOD'];
const groupRecords = {
  KYWD: keywordRecords, GLOB: [blueprint], MISC: looseMods, STAT: visuals, WEAP: [gun], AMMO: SPIKES.map(ammo),
  PROJ: SPIKES.map(projectile), COBJ: recipes.concat(chamberRecipes), OMOD: chambers, ...quest.groups,
};
const groups = GROUP_ORDER.filter(label => groupRecords[label]).map(label => group(label, groupRecords[label]));
// HEDR counts every record and group in the file, nested ones too
function countRecords(buffer) {
  let count = 0;
  for (let p = 0; p < buffer.length;) {
    const size = buffer.readUInt32LE(p + 4);
    ++count;
    if (buffer.toString('latin1', p, p + 4) === 'GRUP') {
      count += countRecords(buffer.subarray(p + 24, p + size));
      p += size;
    } else {
      p += 24 + size;
    }
  }
  return count;
}
const recordCount = countRecords(Buffer.concat(groups));

const hedr = Buffer.alloc(12);
hedr.writeFloatLE(0.95, 0);
hedr.writeUInt32LE(recordCount, 4);
hedr.writeUInt32LE(Math.max(...Object.values(ID), ...Object.values(quest.ids)) + 1, 8);
const header = record('TES4', LIGHT, 0, [sub('HEDR', hedr), sub('CNAM', zstr(AUTHOR)), sub('SNAM', zstr(DESCRIPTION)),
  sub('MAST', zstr('Fallout4.esm')), sub('DATA', Buffer.alloc(8))]);

fs.writeFileSync(out, Buffer.concat([header, ...groups]));
console.log(`${out}: grappling gun, ${SPIKES.length} spikes, ${chambers.length} chambers, ${recipes.length + chamberRecipes.length} recipes, ` +
            `${quest.report}; ${recordCount} records and groups, ${fs.statSync(out).size} bytes`);
