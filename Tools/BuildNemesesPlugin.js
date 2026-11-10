// Builds GameFiles/Fallout4/RD_Nemeses.esp, the Pip-Boy's DATA > NEMESIS tab: a light plugin (ESL flag) with
// Fallout4.esm as its only master. PipboyTabs shows it (GameFiles/Fallout4/PBT/RD_NemesesTab.ini, a note tab): its
// entries are the messages of a form list, each shown while its global says so. The client names the entries and
// gives their text (Code/client/Games/Fallout4/NemesisPipboy.cpp), from the server's pages; the first is always OPEN
// FULL SCREEN, which one script listens for.
//   0x800-0x827 RD_NemesisPage00-39  MESG: the pages (title = FULL, text = DESC, both set in game)
//   0x840-0x867 RD_NemesisShown00-39 GLOB: 0 hidden, 1 shown, 2 shown with a dot
//   0x880 RD_NemesisPages FLST: the pages (PipboyTabs' FirstList)
//   0x881 RD_NemesisShown FLST: the globals, in the same order (its FilterList)
//   0x882 RD_NemesisOpenScreen GLOB: 1 when OPEN FULL SCREEN was pressed (the client opens the screen, sets it to 0)
//   0x883 RD_NemesesTab QUST: start game enabled, script ReclamationDay:NemesesTab (GameFiles/Fallout4/Scripts, its
//         source under Scripts/Source/User): the tab's entry, every player's Interrogate perk, and a man's hands up
//         and down (the client calls HandsUp, MarkCaptive, LetGo, HandsDown)
//   0x884 RD_NemesisCaptive KYWD: on a man who gave up (or was held up with the game's Intimidation perk)
//   0x885 RD_NemesisInterrogate PERK: an activate choice "Interrogate" on a captive, as Fallout4.esm's Intimidation01
//         adds "Intimidate" (entry point Activate, Add Activate Choice, replacing the default; its fragment script
//         ReclamationDay:InterrogatePerk sets 0x886)
//   0x886 RD_NemesisInterrogateNow GLOB: 1 when Interrogate was chosen (the client opens his card, sets it to 0)
// usage: node Tools/BuildNemesesPlugin.js [out.esp]
const fs = require('fs');
const path = require('path');

const out = process.argv[2] || path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'RD_Nemeses.esp');
const AUTHOR = 'Reclamation Day';
const DESCRIPTION = "Reclamation Day's nemeses, the Pip-Boy's NEMESIS tab.";
const FORM_VERSION = 131;
const LIGHT = 0x200;
// Code/server/Nemesis/NemesisTypes.h kPipboyPages
const PAGES = 40;
const FIRST_PAGE = 0x800;
const FIRST_SHOWN = 0x840;
const PAGE_LIST = 0x880;
const SHOWN_LIST = 0x881;
const OPEN_SCREEN = 0x882;
const TAB_QUEST = 0x883;
const CAPTIVE = 0x884;
const INTERROGATE_PERK = 0x885;
const INTERROGATE_NOW = 0x886;
// Fallout4.esm is master 00: our own forms are 01xxxxxx in the file
const OWN = 0x01000000;
// Fallout4.esm's: the player's reference, the keyword that raises a man's hands (holdupEffectScript's
// AnimFlavorHandsup), the Assistance actor value
const PLAYER_REF = 0x14;
const ANIM_FLAVOR_HANDS_UP = 0xB1CF5;
const ASSISTANCE = 0x2C1;

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

// A page: a message as KARMA's tab pages are laid out (INAM no icon, DNAM 1 a message box)
const page = i =>
  record('MESG', 0, OWN | (FIRST_PAGE + i),
         Buffer.concat([sub('EDID', zstr(`RD_NemesisPage${two(i)}`)), sub('DESC', zstr('...')), sub('FULL', zstr('...')), sub('INAM', u32(0)),
                        sub('DNAM', u32(1))]));
// Hidden until the client fills the page
const shown = i => record('GLOB', 0, OWN | (FIRST_SHOWN + i), Buffer.concat([sub('EDID', zstr(`RD_NemesisShown${two(i)}`)), sub('FLTV', f32(0))]));
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
const openScreen = record('GLOB', 0, OWN | OPEN_SCREEN, Buffer.concat([sub('EDID', zstr('RD_NemesisOpenScreen')), sub('FNAM', Buffer.from('s', 'latin1')),
                                                                      sub('FLTV', f32(0))]));

const interrogateNow = record('GLOB', 0, OWN | INTERROGATE_NOW,
                              Buffer.concat([sub('EDID', zstr('RD_NemesisInterrogateNow')), sub('FNAM', Buffer.from('s', 'latin1')), sub('FLTV', f32(0))]));
// A keyword as Fallout4.esm's: CNAM its colour, TNAM its type (0 none)
const captive = record('KYWD', 0, OWN | CAPTIVE, Buffer.concat([sub('EDID', zstr('RD_NemesisCaptive')), sub('CNAM', u32(0x00FFFFFF)), sub('TNAM', u32(0))]));

// The tab's quest: one script with object properties. VMAD version 6, object format 2 (an object is: unused u16,
// alias s16 -1, form id); a script-only quest's VMAD ends after its scripts. DNAM as the start game enabled quests of
// the masters: 0x10 (started with a new game, or when the plugin is first loaded into a save) and 0x01 (running).
const objectProperty = (name, formId) => Buffer.concat([wstr(name), u8(1), u8(1), u16(0), u16(0xFFFF), u32(formId)]);
const questProperties = [objectProperty('OpenScreen', OWN | OPEN_SCREEN), objectProperty('FirstPage', OWN | FIRST_PAGE),
                         objectProperty('Captive', OWN | CAPTIVE), objectProperty('InterrogatePerk', OWN | INTERROGATE_PERK),
                         objectProperty('AnimFlavorHandsUp', ANIM_FLAVOR_HANDS_UP), objectProperty('Assistance', ASSISTANCE)];
const vmad = Buffer.concat([u16(6), u16(2), u16(1), wstr('ReclamationDay:NemesesTab'), u8(0), u16(questProperties.length), ...questProperties]);
const tabQuest = record('QUST', 0, OWN | TAB_QUEST,
                        Buffer.concat([sub('EDID', zstr('RD_NemesesTab')), sub('VMAD', vmad), sub('FULL', zstr('Nemesis Tab')),
                                       sub('DNAM', Buffer.concat([u16(0x0011), u8(80), u8(0), u32(0), u32(0)])),
                                       sub('FLTR', zstr('Reclamation Day\\')), sub('NEXT', Buffer.alloc(0)), sub('ANAM', u32(0))]));

// A condition (CTDA, 32 bytes): operator (0x00 ==, 0x80 <; 0x01 ORs it with the next), 3 unused, the value, the
// function (Fallout 4's condition function index), 2 unused, two parameters, run on (0 the subject, 2 a reference),
// the reference, a third parameter (-1)
const condition = (op, value, fn, param1, runOn = 0, reference = 0) => {
  const b = Buffer.alloc(32);
  b.writeUInt8(op, 0);
  b.writeFloatLE(value, 4);
  b.writeUInt16LE(fn, 8);
  b.writeUInt32LE(param1 >>> 0, 12);
  b.writeUInt32LE(runOn, 20);
  b.writeUInt32LE(reference, 24);
  b.writeInt32LE(-1, 28);
  return sub('CTDA', b);
};
const GET_DISTANCE = 1, GET_DEAD = 46, IS_WEAPON_OUT = 263, HAS_KEYWORD = 560;

// Every player's Interrogate perk, laid out as Fallout4.esm's Intimidation01 (0x1D02B5): VMAD with no scripts and its
// fragments (version 3, the fragment script and its properties, one fragment: index 0, 0, 1, script, function); DATA
// trait, level, ranks, playable, hidden; one entry point (PRKE type 2, rank 0, priority 0; DATA 0x0E Activate, 0x09 Add
// Activate Choice, 2 condition tabs) with its conditions on the target (PRKC 1), the choice's fragment (EPFT 4, EPFB
// 0), its label (EPF2) and flags (EPF3 2: replace the default activation), PRKF ending it. Its choice shows on the
// crosshair while a captive is aimed at (down the sights or not), the player's weapon out, within 15 m.
const FRAGMENT_SCRIPT = 'ReclamationDay:InterrogatePerk';
const perkVmad = Buffer.concat([u16(6), u16(2), u16(0), u8(3), wstr(FRAGMENT_SCRIPT), u8(0), u16(1), objectProperty('InterrogateNow', OWN | INTERROGATE_NOW),
                                u16(1), u16(0), u16(0), u8(1), wstr(FRAGMENT_SCRIPT), wstr('Fragment_Entry_00')]);
const interrogatePerk = record('PERK', 0, OWN | INTERROGATE_PERK, Buffer.concat([
  sub('EDID', zstr('RD_NemesisInterrogate')), sub('VMAD', perkVmad), sub('FULL', zstr('Interrogate')),
  sub('DESC', zstr('Question a gang member who gave up, at gunpoint.')), sub('DATA', Buffer.from([0, 0, 1, 0, 1])),
  sub('PRKE', Buffer.from([2, 0, 0])), sub('DATA', Buffer.from([0x0E, 0x09, 2])), sub('PRKC', u8(1)),
  condition(0x00, 1, HAS_KEYWORD, OWN | CAPTIVE), condition(0x00, 0, GET_DEAD, 0), condition(0x80, 1500, GET_DISTANCE, PLAYER_REF),
  condition(0x00, 1, IS_WEAPON_OUT, 0, 2, PLAYER_REF),
  sub('EPFT', u8(4)), sub('EPFB', u16(0)), sub('EPF2', zstr('Interrogate')), sub('EPF3', u16(2)), sub('PRKF', Buffer.alloc(0))]));

const groups = [group('GLOB', [...shownGlobals, openScreen, interrogateNow]), group('KYWD', [captive]), group('MESG', pages),
                group('PERK', [interrogatePerk]), group('QUST', [tabQuest]),
                group('FLST', [list(PAGE_LIST, 'RD_NemesisPages', FIRST_PAGE), list(SHOWN_LIST, 'RD_NemesisShown', FIRST_SHOWN)])];
// Pages and their globals, OPEN FULL SCREEN's global, Interrogate's, the keyword, the perk, the quest, two lists
const recordCount = PAGES * 2 + 1 + 1 + 1 + 1 + 1 + 2 + groups.length;

const hedr = Buffer.alloc(12);
hedr.writeFloatLE(0.95, 0);
hedr.writeUInt32LE(recordCount, 4);
hedr.writeUInt32LE(INTERROGATE_NOW + 1, 8);
const header = record('TES4', LIGHT, 0,
                      Buffer.concat([sub('HEDR', hedr), sub('CNAM', zstr(AUTHOR)), sub('SNAM', zstr(DESCRIPTION)),
                                     sub('MAST', zstr('Fallout4.esm')), sub('DATA', Buffer.alloc(8))]));

fs.writeFileSync(out, Buffer.concat([header, ...groups]));
console.log(`${out}: ${PAGES} pages, ${fs.statSync(out).size} bytes`);
