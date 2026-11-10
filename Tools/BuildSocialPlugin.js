// Builds GameFiles/Fallout4/RD_Social.esp, the Pip-Boy's DATA > SOCIAL tab (docs/STEAM_SOCIAL.md): a light plugin (ESL
// flag) with Fallout4.esm as its only master. PipboyTabs shows it (GameFiles/Fallout4/PBT/RD_SocialTab.ini, a note tab):
// its entries are the messages of a form list, each shown while its global says so. The client names the entries and
// gives their text (Code/client/Games/Fallout4/SocialPipboy.cpp): the hosted world, friends asking to join, the
// players in this world and the party, the Steam friends; one script says which entry was pressed.
//   0x800-0x83F RD_SocialPage00-63  MESG: the entries (title = FULL, text = DESC, both set in game)
//   0x840-0x87F RD_SocialShown00-63 GLOB: 0 hidden, 1 shown, 2 shown with a dot
//   0x880 RD_SocialPages FLST: the entries (PipboyTabs' FirstList)
//   0x881 RD_SocialShown FLST: the globals, in the same order (its FilterList)
//   0x882 RD_SocialPressed GLOB: the pressed entry's number + 1 (the client acts on it, sets it to 0)
//   0x883 RD_SocialTab QUST: start game enabled, script ReclamationDay:SocialTab (GameFiles/Fallout4/Scripts, its source
//         under Scripts/Source/User): PipboyTabs' PBT_ItemPressed on one of the entries sets 0x882
// Also the factions other players' characters carry their standing with (docs/FACTIONS.md, the client's FactionService):
//   0x884-0x88A RD_FoughtBy<faction>  FACT: the faction fights who's in it (Minutemen, Brotherhood, Railroad,
//               Institute, Disciples, Operators, Pack)
//   0x88B-0x891 RD_AlliedWith<faction> FACT: the faction is their ally
//   Their relations toward Fallout4.esm's factions are here (XNAM); the faction's own toward them, and the Nuka-World
//   gangs' (DLCNukaWorld.esm isn't a master), are set in game (Faction.SetEnemy / SetAlly).
// usage: node Tools/BuildSocialPlugin.js [out.esp]
const fs = require('fs');
const path = require('path');

const out = process.argv[2] || path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'RD_Social.esp');
const AUTHOR = 'Reclamation Day';
const DESCRIPTION = "Reclamation Day's Steam friends, the Pip-Boy's SOCIAL tab, and the factions other players' standing is carried with.";
const FORM_VERSION = 131;
const LIGHT = 0x200;
// Code/client/Games/Fallout4/SocialPipboy.h kPages
const PAGES = 64;
const FIRST_PAGE = 0x800;
const FIRST_SHOWN = 0x840;
const PAGE_LIST = 0x880;
const SHOWN_LIST = 0x881;
const PRESSED = 0x882;
const TAB_QUEST = 0x883;
// Code/client/Services/Generic/FactionService.cpp kStandingFactions: fought by, then allied with, each of these
const FIRST_STANDING = 0x884;
const STANDING_FACTIONS = [['Minutemen', 0x068043], ['Brotherhood', 0x05DE41], ['Railroad', 0x0994F6], ['Institute', 0x05E558],
                           ['Disciples', null], ['Operators', null], ['Pack', null]];
const LAST_FORM = FIRST_STANDING + STANDING_FACTIONS.length * 2 - 1;
// Fallout4.esm is master 00: our own forms are 01xxxxxx in the file
const OWN = 0x01000000;

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
const u16 = value => {
  const b = Buffer.alloc(2);
  b.writeUInt16LE(value & 0xFFFF, 0);
  return b;
};
const u8 = value => Buffer.from([value & 0xFF]);
// A VMAD string: its length, then the text
const wstr = s => Buffer.concat([u16(s.length), Buffer.from(s, 'latin1')]);
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
const two = i => String(i).padStart(2, '0');

// An entry: a message as KARMA's tab pages are laid out (INAM no icon, DNAM 1 a message box)
const page = i =>
  record('MESG', 0, OWN | (FIRST_PAGE + i),
         Buffer.concat([sub('EDID', zstr(`RD_SocialPage${two(i)}`)), sub('DESC', zstr('...')), sub('FULL', zstr('...')), sub('INAM', u32(0)),
                        sub('DNAM', u32(1))]));
// Hidden until the client fills the entry
const shown = i => record('GLOB', 0, OWN | (FIRST_SHOWN + i), Buffer.concat([sub('EDID', zstr(`RD_SocialShown${two(i)}`)), sub('FLTV', f32(0))]));
const list = (id, edid, first) => {
  const entries = [];
  for (let i = 0; i < PAGES; ++i)
    entries.push(sub('LNAM', u32(OWN | (first + i))));
  return record('FLST', 0, OWN | id, Buffer.concat([sub('EDID', zstr(edid)), sub('FULL', zstr(edid)), ...entries]));
};

const pages = [], shownGlobals = [];
for (let i = 0; i < PAGES; ++i) {
  pages.push(page(i));
  shownGlobals.push(shown(i));
}
const pressed = record('GLOB', 0, OWN | PRESSED, Buffer.concat([sub('EDID', zstr('RD_SocialPressed')), sub('FNAM', Buffer.from('s', 'latin1')),
                                                              sub('FLTV', f32(0))]));

// The tab's quest: one script with object properties. VMAD version 6, object format 2 (an object is: unused u16,
// alias s16 -1, form id); a script-only quest's VMAD ends after its scripts. DNAM as the start game enabled quests of
// the masters: 0x10 (started with a new game, or when the plugin is first loaded into a save) and 0x01 (running).
const objectProperty = (name, formId) => Buffer.concat([wstr(name), u8(1), u8(1), u16(0), u16(0xFFFF), u32(formId)]);
const questProperties = [objectProperty('Pressed', OWN | PRESSED), objectProperty('FirstPage', OWN | FIRST_PAGE)];
const vmad = Buffer.concat([u16(6), u16(2), u16(1), wstr('ReclamationDay:SocialTab'), u8(0), u16(questProperties.length), ...questProperties]);
const tabQuest = record('QUST', 0, OWN | TAB_QUEST,
                        Buffer.concat([sub('EDID', zstr('RD_SocialTab')), sub('VMAD', vmad), sub('FULL', zstr('Social Tab')),
                                       sub('DNAM', Buffer.concat([u16(0x0011), u8(80), u8(0), u32(0), u32(0)])),
                                       sub('FLTR', zstr('Reclamation Day\\')), sub('NEXT', Buffer.alloc(0)), sub('ANAM', u32(0))]));

// A standing faction, laid out as the game's own small ones (BoSDanseBoSEnemyFaction): DATA 1 hidden from the player,
// CRVA and VENV as theirs. XNAM: form id, modifier 0, combat reaction (1 enemy, 2 ally)
const ENEMY = 1, ALLY = 2;
const standingFaction = (id, edid, name, target, reaction) =>
  record('FACT', 0, OWN | id,
         Buffer.concat([sub('EDID', zstr(edid)), sub('FULL', zstr(name)),
                        ...(target ? [sub('XNAM', Buffer.concat([u32(target), u32(0), u32(reaction)]))] : []), sub('DATA', u32(1)),
                        sub('CRVA', Buffer.concat([Buffer.from([1, 1]), Buffer.alloc(18)])), sub('VENV', Buffer.alloc(12))]));
const standing = [];
STANDING_FACTIONS.forEach(([name, target], i) => {
  standing.push(standingFaction(FIRST_STANDING + i, `RD_FoughtBy${name}`, `Fought by ${name}`, target, ENEMY));
});
STANDING_FACTIONS.forEach(([name, target], i) => {
  standing.push(standingFaction(FIRST_STANDING + STANDING_FACTIONS.length + i, `RD_AlliedWith${name}`, `Allied with ${name}`, target, ALLY));
});

const groups = [group('GLOB', [...shownGlobals, pressed]), group('MESG', pages), group('QUST', [tabQuest]),
                group('FLST', [list(PAGE_LIST, 'RD_SocialPages', FIRST_PAGE), list(SHOWN_LIST, 'RD_SocialShown', FIRST_SHOWN)]),
                group('FACT', standing)];
// Entries and their globals, the pressed global, the quest, two lists, the standing factions
const recordCount = PAGES * 2 + 1 + 1 + 2 + standing.length + groups.length;

const hedr = Buffer.alloc(12);
hedr.writeFloatLE(0.95, 0);
hedr.writeUInt32LE(recordCount, 4);
hedr.writeUInt32LE(LAST_FORM + 1, 8);
const header = record('TES4', LIGHT, 0,
                      Buffer.concat([sub('HEDR', hedr), sub('CNAM', zstr(AUTHOR)), sub('SNAM', zstr(DESCRIPTION)),
                                     sub('MAST', zstr('Fallout4.esm')), sub('DATA', Buffer.alloc(8))]));

fs.writeFileSync(out, Buffer.concat([header, ...groups]));
console.log(`${out}: ${PAGES} entries, ${fs.statSync(out).size} bytes`);
