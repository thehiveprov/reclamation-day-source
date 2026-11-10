// The 2287 way to the grappling gun's blueprint, in RD_GrapplingGun.esp (BuildGrapplePlugin.js calls this; docs/
// GRAPPLE.md "Getting it"). The design (2026-10-24): a wanderer once of Beatsville, at the burning barrel by
// Diamond City's gate, sends the player to the wreck of the USS Germination off Long Wharf's south face; on a crate on
// its deck lie the grappling gun, Dr. Yarrow's last log (a holotape) and a white lab coat. Back at the wanderer, they
// explain the gun and hand over its plans (RD_GrappleBlueprint = 1); the player keeps the gun. The wanderer's lines are
// silent voice files with subtitles (Tools/BuildGrappleVoice.js) until someone records them. In Winter of Atom the
// quest stops at once (ReclamationDay:GrappleQuest) and everything placed here stays disabled.
//   0x813 NPC_  RD_BeatsvilleWanderer: Fallout4.esm's settler face preset EncWorkshopNPCMaleFarmer02 (0x113343) for its
//               traits only (face, race, voice MaleEvenToned), as Winter of Atom's Mirage trader is made
//   0x814 QUST  RD_GrappleQuest "The Wreck of the Germination": start game enabled, script ReclamationDay:GrappleQuest;
//               aliases Wanderer, Crate, Gun (forced references); stages 10 job taken, 20 gun taken, 30 plans given
//   0x815 NOTE  RD_YarrowHolotape and 0x816 TERM, its text: a terminal holotape as Fallout4.esm's
//               DN084_HolotapeJackRockford (0x5A7CF) and its terminal (0x5A7CE)
//   0x817 MESG  RD_GrappleBlueprintMessage: the notification when the plans are given
//   0x818 SCEN  RD_GrappleWandererScene: the wanderer's offer, the game's four-choice dialogue
//   0x820-0x82F DIAL, 0x840-0x85F INFO: the wanderer's greeting lines and the scene's topics and lines
//   0x860-0x86F placed in the Commonwealth's persistent cell (a worldspace override, as RD_Companions places its dogs):
//               an enable marker (disabled; the quest enables it in 2287) and, hanging from it, the wanderer, the wreck
//               (Fallout4.esm's ShipWrecked01, its beams and bulkhead, as the game builds its beached freighter), the
//               crate, the gun, five spikes, the lab coat and the holotape
// Layouts read from Fallout4.esm 1.10.163 (record by record in docs/GRAPPLE.md); the worldspace and its persistent
// cell, the face preset and the holotape templates are copied from the master at build time.
const fs = require('fs');
const zlib = require('zlib');

const ID = {
  wanderer: 0x813, quest: 0x814, holotape: 0x815, holotapeTerminal: 0x816, blueprintMessage: 0x817, scene: 0x818,
  topicGreeting: 0x820, topicHello: 0x821, topicAccept: 0x822, topicDecline: 0x823, topicWho: 0x824, topicWhat: 0x825,
  topicAcceptReply: 0x826, topicDeclineReply: 0x827, topicWhoReply: 0x828, topicWhatReply: 0x829,
  infoOffer: 0x840, infoReturn: 0x841, infoWaiting: 0x842, infoAfter: 0x843, infoHello: 0x844, infoAccept: 0x845,
  infoDecline: 0x846, infoWho: 0x847, infoWhat: 0x848, infoAcceptReply: 0x849, infoDeclineReply: 0x84a,
  infoWhoReply: 0x84b, infoWhatReply: 0x84c,
  refMarker: 0x860, refWanderer: 0x861, refHull: 0x862, refBeams: 0x863, refBulkhead: 0x864, refCrate: 0x865,
  refGun: 0x866, refSpikes: 0x867, refCoat: 0x868, refHolotape: 0x869,
};

// Fallout4.esm forms
const FO4 = {
  Commonwealth: 0x00003c, CommonwealthPersistentCell: 0x018aa2,
  WandererPreset: 0x113343,             // EncWorkshopNPCMaleFarmer02 (voice MaleEvenToned 0x13AD2)
  StayAtEditorLocationTightRadius: 0x217b6c, Wastelander02withHood: 0x02186e, Caps: 0x00000f,
  HolotapeTemplate: 0x05a7cf, HolotapeTerminalTemplate: 0x05a7ce,
  XMarker: 0x00003b, ShipWrecked01: 0x145162, ShipWreckedBeams01: 0x1e59e3, ShipWreckedBulkHead01: 0x1e59e4,
  WoodCrate01: 0x0211f5, LabCoat: 0x178b68,   // ClothesScientist_VariantWasteland "Lab Coat"
};

// Where things are (Commonwealth, game units; rotations in degrees, x then y then z, z the heading clockwise from +y).
// The burning barrel by Diamond City's gate is Fallout4.esm's MetalBarrel01FireGrating_Static 001436BF at
// (-15087, -29795, 1351), 13 m from the gate's intercom: the wanderer stands at its south west side, facing it.
const WANDERER_AT = { pos: [-15142, -29850, 1352], rot: [0, 0, 45] };
const MARKER_AT = { pos: [-15087, -29795, 1460], rot: [0, 0, 0] };
// The wreck: in the open water south of Long Wharf (the harbor's water at z 450; cells 9..11,-4, empty in vanilla), its
// bow (local -x) toward the shore, sunk to its deck and settled stern up (y 3.65 deg), listing a little. ShipWrecked01's
// deck plates are at local z 440: the one at local x -200..1000, y -800..-400 rises out of the water at its west end to
// 70 units above it at its east end, so a swimmer can walk up onto it; the crate stands on it at local (800, -600).
// Checked offline with renders of the placed meshes against the water line (docs/GRAPPLE.md).
const WRECK = { pos: [42300, -14600, 20], rot: [1.5, 3.65, 3] };
const DECK_SPOT = [800, -600, 439.8];
// On the crate (WoodCrate01: 50 x 55, its top 51 up), in the crate's frame: the gun on its side, the spikes, the coat,
// the holotape; each a little over the top (the game's physics settles them)
const ON_CRATE = {
  gun: { at: [-6, 8, 56], rot: [0, 90, 35] },
  spikes: { at: [13, -12, 53], rot: [0, 0, 70] },
  coat: { at: [-8, -12, 52], rot: [0, 0, 10] },
  holotape: { at: [12, 14, 52], rot: [0, 0, 200] },
};
const SPIKE_COUNT = 5;

// --- the words (the wanderer's lines need a silent voice file each, Tools/BuildGrappleVoice.js, which reads LINES)
const QUEST_NAME = 'The Wreck of the Germination';
const WANDERER_NAME = 'Beatsville Wanderer';
const LOG = {
  10: 'A wanderer at Diamond City\'s gate, whose father made winches in Beatsville, wants a grappling gun off the wreck of the USS Germination, which lies off the south face of Long Wharf. In return they\'ll give me their father\'s plans for it.',
  20: 'I found the grappling gun on a crate on the Germination\'s deck, with Dr. Yarrow\'s last log and a lab coat. The wanderer from Beatsville will want to see it.',
  30: 'The wanderer from Beatsville gave me their father\'s plans. I can make grappling guns and grapple spikes at a chemistry station, and spike chambers at a weapons workbench.',
};
const OBJECTIVES = {
  10: 'Search the wreck of the USS Germination off Long Wharf',
  20: 'Bring the grappling gun to the Beatsville Wanderer',
};
const BLUEPRINT_MESSAGE = 'Grappling gun plans learned: the gun and its spikes at a chemistry station, spike chambers at a weapons workbench.';
const LINES = {
  offer: ['You\'ve got the look of somebody who doesn\'t mind getting wet.',
    'There\'s a wreck down at Long Wharf with something on it I want, and these knees won\'t swim it anymore.'],
  accept: ['I\'ll take a look.'],
  decline: ['Not interested.'],
  who: ['Who are you?'],
  what: ['What\'s on the wreck?'],
  acceptReply: ['Good. She\'s off the south face of Long Wharf, settled on the bottom with her deck awash. The USS Germination.',
    'There\'s a crate up on her deck, high and dry, with a grappling gun on it and a dead man\'s things. Bring me that gun.'],
  declineReply: ['Suit yourself. She\'s been sitting there five years. She\'ll keep.'],
  whoReply: ['Nobody much. My folks were from Beatsville, back when there was a Beatsville.',
    'My father made winches. Cranes, hoists, anything with a spool. The winter of \'82, he drew up a gun that could throw a hook and reel a man up a wall.'],
  whatReply: ['One of his guns, if the stories are true. The fanatics who stormed that ship carried them, and one of them never came back off her.',
    'Bring it to me and you can have his plans. Every part, every turn of the spool. You\'ll get more use out of them than I ever will.'],
  waiting: ['Long Wharf, south face. Mind the holes in her deck: she\'s full of water.'],
  returned: ['You found it. You actually swam out there.',
    'That\'s his winch, all right. Hold the trigger and you\'ll see where the spike will bite. Let go and it flies. Catch a ledge and it\'ll pull you right up onto it.',
    'Here. His plans, like I promised. You\'ll need steel for the spikes. Keep the gun: it\'s waited long enough for somebody to use it.'],
  after: ['Keep your line tight out there.'],
};
// Dr. Yarrow's last log: the holotape's terminal, a header and three entries (Winter of Atom's A New Eden, the canon end:
// the Germination destroyed and sunk)
const HOLOTAPE = {
  name: 'Dr. Yarrow\'s Last Log',
  header: 'USS GERMINATION\nPERSONAL LOG: DR. YARROW',
  entries: [
    ['The boarders',
      'They came over the rails at dawn on hooks and cables, singing to their glowing god. Crude guns: a spike on a steel line and a winch you crank by hand. And yet they climbed my hull faster than my crew could reload.\n\nWhoever designed that winch understood torque better than anyone aboard this ship. Myself included.'],
    ['The fire',
      'The Minutemen held the deck long enough for the fanatics\' bear to open the hold below. Then someone found the fuel stores.\n\nThe orchard burned. Years of compost and grafting, gone in an hour. The ice is breaking up under us, and the hull is opening along the old welds. She will not float through the thaw.'],
    ['Last entry',
      'She is settling onto the bottom against Long Wharf, where the storm first left her. I have carried what I could save to the one plate of deck still above the water: my coat, this log, and the grappling gun a dead boarder dropped at my feet.\n\nIf you are reading this, the Germination is a reef now. Whatever grows here grows without me.'],
  ],
};

module.exports = { ID, LINES, build };

function build(K, esmPath) {
  const { sub, u8, u16, u32, i32, f32, zstr, OWN } = K;
  const own = id => (OWN | id) >>> 0;
  const esm = readMaster(esmPath, [FO4.Commonwealth, FO4.CommonwealthPersistentCell, FO4.WandererPreset, FO4.HolotapeTemplate,
    FO4.HolotapeTerminalTemplate]);
  // record header: type, data size, flags, form ID, version control, form version 131, unknown
  const record = (type, flags, formId, subs) => {
    const data = Buffer.concat(subs);
    const head = Buffer.alloc(24);
    head.write(type, 0, 'latin1');
    head.writeUInt32LE(data.length, 4);
    head.writeUInt32LE(flags, 8);
    head.writeUInt32LE(formId >>> 0, 12);
    head.writeUInt16LE(131, 20);
    return Buffer.concat([head, data]);
  };
  // a group under a record: its label a form ID, its type (1 world children, 6 cell children, 7 topic children, 8 cell
  // persistent children, 10 quest children)
  const childGroup = (label, type, records) => {
    const body = Buffer.concat(records);
    const head = Buffer.alloc(24);
    head.write('GRUP', 0, 'latin1');
    head.writeUInt32LE(24 + body.length, 4);
    head.writeUInt32LE(label >>> 0, 8);
    head.writeUInt32LE(type, 12);
    head.writeUInt16LE(131, 20);
    return Buffer.concat([head, body]);
  };
  const quest = own(ID.quest);

  // ------------------------------------------------------------ the wanderer
  // Traits only from the preset (template flag 0x0001, TPLT and the first TPTA slot the preset), as MQ203MemoryH_Kellogg;
  // our own: the name, no factions, essential and unique, a few caps, standing at the barrel, a hooded wastelander's
  // clothes. The record is written uncompressed.
  const npcSubs = [];
  for (const [t, d] of subsOf(esm.get(FO4.WandererPreset), 'NPC_')) {
    if (t === 'EDID') npcSubs.push(sub('EDID', zstr('RD_BeatsvilleWanderer')));
    else if (t === 'ACBS') {
      const a = Buffer.from(d);
      a.writeUInt32LE((a.readUInt32LE(0) & 0x1) | 0x2 | 0x10 | 0x20, 0);   // the preset's sex, essential, auto-calc, unique
      a.writeUInt16LE(0x0001, 14);                                        // template: use traits
      npcSubs.push(sub('ACBS', a));
    } else if (t === 'SNAM') continue;
    else if (t === 'TPLT') npcSubs.push(sub('TPLT', u32(FO4.WandererPreset)));
    else if (t === 'TPTA') { const a = Buffer.alloc(d.length); a.writeUInt32LE(FO4.WandererPreset, 0); npcSubs.push(sub('TPTA', a)); }
    else if (t === 'COCT') npcSubs.push(sub('COCT', u32(1)), sub('CNTO', Buffer.concat([u32(FO4.Caps), i32(23)])));
    else if (t === 'CNTO') continue;
    else if (t === 'PKID') npcSubs.push(sub('PKID', u32(FO4.StayAtEditorLocationTightRadius)));
    else if (t === 'CNAM') npcSubs.push(sub('CNAM', d), sub('FULL', zstr(WANDERER_NAME)));
    else if (t === 'DOFT') npcSubs.push(sub('DOFT', u32(FO4.Wastelander02withHood)));
    else npcSubs.push(sub(t, d));
  }
  const wanderer = record('NPC_', 0, own(ID.wanderer), npcSubs);

  // ------------------------------------------------------------ the holotape
  // NOTE: its type (DNAM) 3 a terminal, SNAM the terminal; the rest as the template (model, sounds, value 1)
  const noteSubs = [];
  for (const [t, d] of subsOf(esm.get(FO4.HolotapeTemplate), 'NOTE')) {
    if (t === 'EDID') noteSubs.push(sub('EDID', zstr('RD_YarrowHolotape')));
    else if (t === 'FULL') noteSubs.push(sub('FULL', zstr(HOLOTAPE.name)));
    else if (t === 'DNAM') noteSubs.push(sub('DNAM', u8(3)));
    else if (t === 'SNAM') noteSubs.push(sub('SNAM', u32(own(ID.holotapeTerminal))));
    else noteSubs.push(sub(t, d));
  }
  const holotape = record('NOTE', 0, own(ID.holotape), noteSubs);
  // TERM: the template's fields up to its body text, then ours: the header (BSIZ/BTXT), the entries (ISIZ; per entry
  // ITXT its label, ANAM 8 shows text, ITID its number, UNAM the text)
  const termSubs = [];
  for (const [t, d] of subsOf(esm.get(FO4.HolotapeTerminalTemplate), 'TERM')) {
    if (t === 'BSIZ') break;
    if (t === 'EDID') termSubs.push(sub('EDID', zstr('RD_YarrowHolotapeTerminal')));
    else if (t === 'FULL') termSubs.push(sub('FULL', zstr(HOLOTAPE.name)));
    else termSubs.push(sub(t, d));
  }
  termSubs.push(sub('BSIZ', u32(1)), sub('BTXT', zstr(HOLOTAPE.header)), sub('ISIZ', u32(HOLOTAPE.entries.length)));
  HOLOTAPE.entries.forEach(([label, text], i) => termSubs.push(sub('ITXT', zstr(label)), sub('ANAM', u8(8)), sub('ITID', u16(i + 1)),
    sub('UNAM', zstr(text))));
  const holotapeTerminal = record('TERM', 0, own(ID.holotapeTerminal), termSubs);

  // ------------------------------------------------------------ the notification (DNAM 0: not a message box; TNAM seconds)
  const blueprintMessage = record('MESG', 0, own(ID.blueprintMessage), [sub('EDID', zstr('RD_GrappleBlueprintMessage')),
    sub('DESC', zstr(BLUEPRINT_MESSAGE)), sub('FULL', zstr(BLUEPRINT_MESSAGE)), sub('INAM', u32(0)), sub('DNAM', u32(0)), sub('TNAM', u32(6))]);

  // ------------------------------------------------------------ the quest
  // DNAM: flags 0x0111 (starts enabled 0x10 + running 0x01, the pair the new game's start step checks, and run once),
  // priority 60, type 7 side quest, as MS07c. Stages: INDX index + flags, then a log entry (QSDT 0x01 completes the
  // quest, CNAM the text). Objectives: QOBJ, FNAM, NNAM, QSTA (the target alias). Aliases: forced references (ALFR).
  const stage = (index, log, logFlags = 0) => [sub('INDX', Buffer.concat([u16(index), u8(0), u8(0)])), sub('QSDT', u8(logFlags)), sub('CNAM', zstr(log))];
  const objective = (index, text, alias) => [sub('QOBJ', u16(index)), sub('FNAM', u32(0)), sub('NNAM', zstr(text)),
    sub('QSTA', Buffer.concat([i32(alias), u32(0), u32(0)]))];
  const forcedAlias = (index, name, ref) => [sub('ALST', u32(index)), sub('ALID', zstr(name)), sub('FNAM', u32(0)), sub('ALFR', u32(own(ref))),
    sub('VTCK', u32(0)), sub('ALED', Buffer.alloc(0))];
  const script = Buffer.from('ReclamationDay:GrappleQuest', 'latin1');
  const questRecord = record('QUST', 0, quest, [
    sub('EDID', zstr('RD_GrappleQuest')),
    sub('VMAD', Buffer.concat([u16(6), u16(2), u16(1), u16(script.length), script, u8(0), u16(0)])),   // one script, no properties
    sub('FULL', zstr(QUEST_NAME)),
    sub('DNAM', Buffer.concat([u16(0x0111), u8(60), u8(0), u32(0), u32(7)])),
    sub('FLTR', zstr('Reclamation Day\\')), sub('NEXT', Buffer.alloc(0)),
    ...stage(10, LOG[10]), ...stage(20, LOG[20]), ...stage(30, LOG[30], 0x01),
    ...objective(10, OBJECTIVES[10], 1), ...objective(20, OBJECTIVES[20], 0),
    sub('ANAM', u32(3)),
    ...forcedAlias(0, 'Wanderer', ID.refWanderer), ...forcedAlias(1, 'Crate', ID.refCrate), ...forcedAlias(2, 'Gun', ID.refGun),
  ]);

  // ------------------------------------------------------------ the wanderer's dialogue
  // As Winter of Atom's trader (from Fallout4.esm: DuffGreetScene 0002A04D and Duff's greeting
  // 0021A19A, 1419 player dialogue scenes surveyed), in the game's own menu. Talking to the wanderer says a line of the
  // quest's greeting topic (category 7 subtype 0x74, SNAM GREE) picked by its conditions: the offer starts the scene at
  // its choice phase (TSCE scene, INTV phase, NAM0 its name); the return sets stage 30 when it ends (TIQS). The scene's
  // topics: category 2 subtype 0x0E, SNAM SCEN. A line: ENAM, per response TRDA + NAM1 text + NAM2 notes + NAM3 + NAM4,
  // CTDA, TSCE/INTV, TIQS (begin stage, end stage, -1 none), RNAM the player's prompt, NAM0, INAM.
  const condition = (fn, value, p1 = 0, p2 = 0) => {
    const c = Buffer.alloc(32);
    c.writeFloatLE(value, 4); c.writeUInt16LE(fn, 8); c.writeUInt32LE(p1 >>> 0, 12); c.writeUInt32LE(p2 >>> 0, 16); c.writeInt32LE(-1, 28);
    return sub('CTDA', c);
  };
  const isWanderer = condition(72, 1, own(ID.wanderer));            // GetIsID, on the speaker
  const stageDone = (index, done) => condition(59, done ? 1 : 0, quest, index);   // GetStageDone
  const topic = (localId, edid, infoCount, kind = 'SCEN') => record('DIAL', 0, own(localId), [sub('EDID', zstr(edid)),
    sub('PNAM', f32(50)), sub('QNAM', u32(quest)),
    sub('DATA', Buffer.from(kind === 'GREE' ? [0, 7, 0x74, 0] : [0, 2, 0x0e, 0])), sub('SNAM', Buffer.from(kind, 'latin1')),
    sub('TIFC', u32(infoCount))]);
  const line = (localId, edid, responses, { notes = '', conditions = [], startsPhase = null, endStage = -1, prompt = null } = {}) => {
    const subs = [sub('EDID', zstr(edid)), sub('ENAM', u32(0))];
    responses.forEach((text, i) => subs.push(
      sub('TRDA', Buffer.concat([i32(-1), u32(i + 1), Buffer.from([0, 1, 0, 0]), i32(-1), i32(-1)])),
      sub('NAM1', zstr(text)), sub('NAM2', zstr(i === 0 ? notes : '')), sub('NAM3', zstr('')), sub('NAM4', zstr(''))));
    subs.push(...conditions);
    if (startsPhase) subs.push(sub('TSCE', u32(own(ID.scene))), sub('INTV', u32(startsPhase.index)));
    if (endStage >= 0) subs.push(sub('TIQS', Buffer.concat([u16(0xffff), u16(endStage)])));
    if (prompt !== null) subs.push(sub('RNAM', zstr(prompt)));
    subs.push(sub('NAM0', zstr(startsPhase ? startsPhase.name : '')), sub('INAM', u32(1)));
    return record('INFO', 0, own(localId), subs);
  };
  const CHOICE = { index: 1, name: 'Choice' };
  const L = LINES;
  const dialogue = [
    topic(ID.topicGreeting, 'RD_GrappleWandererGreetingTopic', 4, 'GREE'),
    // before the job (or the gun): the offer, into the choice
    line(ID.infoOffer, 'RD_GrappleWandererOffer', L.offer, { notes: 'warming their hands at the barrel, sizing the player up',
      conditions: [isWanderer, stageDone(10, false), stageDone(20, false)], startsPhase: CHOICE }),
    // the gun found: the plans
    line(ID.infoReturn, 'RD_GrappleWandererReturn', L.returned, { notes: 'turning the gun over in their hands, then handing it back with a roll of plans',
      conditions: [isWanderer, stageDone(20, true), stageDone(30, false)], endStage: 30 }),
    // the job taken, no gun yet
    line(ID.infoWaiting, 'RD_GrappleWandererWaiting', L.waiting, { conditions: [isWanderer, stageDone(10, true), stageDone(20, false)] }),
    line(ID.infoAfter, 'RD_GrappleWandererAfter', L.after, { conditions: [isWanderer, stageDone(30, true)] }),
    topic(ID.topicHello, 'RD_GrappleWandererHelloTopic', 1),
    line(ID.infoHello, 'RD_GrappleWandererHello', L.offer, { notes: 'warming their hands at the barrel, sizing the player up' }),
    topic(ID.topicAccept, 'RD_GrappleWandererAcceptTopic', 1),
    line(ID.infoAccept, 'RD_GrappleWandererAccept', L.accept, { prompt: L.accept[0] }),
    topic(ID.topicDecline, 'RD_GrappleWandererDeclineTopic', 1),
    line(ID.infoDecline, 'RD_GrappleWandererDecline', L.decline, { prompt: L.decline[0] }),
    topic(ID.topicWho, 'RD_GrappleWandererWhoTopic', 1),
    line(ID.infoWho, 'RD_GrappleWandererWho', L.who, { prompt: L.who[0] }),
    topic(ID.topicWhat, 'RD_GrappleWandererWhatTopic', 1),
    line(ID.infoWhat, 'RD_GrappleWandererWhat', L.what, { prompt: L.what[0] }),
    topic(ID.topicAcceptReply, 'RD_GrappleWandererAcceptReplyTopic', 1),
    line(ID.infoAcceptReply, 'RD_GrappleWandererAcceptReply', L.acceptReply, { notes: 'pleased, pointing toward the harbor', endStage: 10 }),
    topic(ID.topicDeclineReply, 'RD_GrappleWandererDeclineReplyTopic', 1),
    line(ID.infoDeclineReply, 'RD_GrappleWandererDeclineReply', L.declineReply, { notes: 'shrugging, back to the fire' }),
    topic(ID.topicWhoReply, 'RD_GrappleWandererWhoReplyTopic', 1),
    line(ID.infoWhoReply, 'RD_GrappleWandererWhoReply', L.whoReply, { notes: 'wistful, proud of their father', startsPhase: CHOICE }),
    topic(ID.topicWhatReply, 'RD_GrappleWandererWhatReplyTopic', 1),
    line(ID.infoWhatReply, 'RD_GrappleWandererWhatReply', L.whatReply, { notes: 'lowering their voice', startsPhase: CHOICE }),
  ];
  // the scene: phase 0 the hello (only when started on its own), phase 1 the choice; the wanderer is alias 0. Phases
  // HNAM, NAM0, NEXT, NEXT, WNAM, HNAM; the actor ALID, LNAM, DNAM; actions ANAM type (0 dialogue, 3 player dialogue),
  // NAM0, ALID, INAM index, FNAM, SNAM/ENAM phases, its data, ANAM; then PNAM the quest, INAM the next action index,
  // VNAM, XNAM.
  const WANDERER_ALIAS = 0;
  const phase = name => [sub('HNAM', Buffer.alloc(0)), sub('NAM0', zstr(name)), sub('NEXT', Buffer.alloc(0)),
    sub('NEXT', Buffer.alloc(0)), sub('WNAM', u32(0x1aa)), sub('HNAM', Buffer.alloc(0))];
  const action = (type, index, startPhase, endPhase, rest) => [sub('ANAM', u16(type)), sub('NAM0', zstr('')),
    sub('ALID', u32(WANDERER_ALIAS)), sub('INAM', u32(index)), sub('FNAM', u32(0x28000)), sub('SNAM', u32(startPhase)),
    sub('ENAM', u32(endPhase)), ...rest, sub('ANAM', Buffer.alloc(0))];
  const scene = record('SCEN', 0, own(ID.scene), [
    sub('EDID', zstr('RD_GrappleWandererScene')), sub('FNAM', u32(0x1024)),
    ...phase('Hello'), ...phase(CHOICE.name),
    sub('ALID', u32(WANDERER_ALIAS)), sub('LNAM', u32(0)), sub('DNAM', u32(0x0a)),
    ...action(0, 1, 0, 0, [sub('DATA', u32(own(ID.topicHello))), sub('DMAX', f32(10)), sub('DMIN', f32(1))]),
    ...action(3, 2, 1, 1, [sub('PTOP', u32(own(ID.topicAccept))), sub('NTOP', u32(own(ID.topicDecline))),
      sub('NETO', u32(own(ID.topicWho))), sub('QTOP', u32(own(ID.topicWhat))),
      sub('NPOT', u32(own(ID.topicAcceptReply))), sub('NNGT', u32(own(ID.topicDeclineReply))),
      sub('NNUT', u32(own(ID.topicWhoReply))), sub('NQUT', u32(own(ID.topicWhatReply))), sub('DTGT', u32(WANDERER_ALIAS))]),
    sub('PNAM', u32(quest)), sub('INAM', u32(3)), sub('VNAM', Buffer.concat([u32(3), u32(3), u32(3), u32(3)])), sub('XNAM', u32(0)),
  ]);
  // a quest's dialogue is a group (type 10) after it: each topic, then a group (type 7) of its lines; the scenes
  const questChildren = [];
  for (let i = 0; i < dialogue.length; i++) {
    questChildren.push(dialogue[i]);
    const infos = [];
    while (i + 1 < dialogue.length && dialogue[i + 1].toString('latin1', 0, 4) === 'INFO') infos.push(dialogue[++i]);
    questChildren.push(childGroup(dialogue[i - infos.length].readUInt32LE(12), 7, infos));
  }
  questChildren.push(scene);

  // ------------------------------------------------------------ placed in the world
  const deg = Math.PI / 180;
  // a point of a frame placed at (pos, rot) into the world, with the game's clockwise angles (as docs/GRAPPLE.md)
  const place = (frame, local) => {
    const [rx, ry, rz] = frame.rot.map(a => a * deg);
    const [cx, sx, cy, sy, cz, sz] = [Math.cos(rx), Math.sin(rx), Math.cos(ry), Math.sin(ry), Math.cos(rz), Math.sin(rz)];
    let [x, y, z] = local;
    [y, z] = [cx * y + sx * z, -sx * y + cx * z];
    [x, z] = [cy * x - sy * z, sy * x + cy * z];
    [x, y] = [cz * x + sz * y, -sz * x + cz * y];
    return [x + frame.pos[0], y + frame.pos[1], z + frame.pos[2]];
  };
  const crate = { pos: place(WRECK, DECK_SPOT), rot: WRECK.rot };
  const onCrate = o => ({ pos: place(crate, o.at), rot: [crate.rot[0] + o.rot[0], crate.rot[1] + o.rot[1], crate.rot[2] + o.rot[2]] });
  const data = at => Buffer.concat([...at.pos.map(f32), ...at.rot.map(a => f32(a * deg))]);
  // XESP: the enable parent, flags 0 (the same state as it)
  const parent = sub('XESP', Buffer.concat([u32(own(ID.refMarker)), u32(0)]));
  const PERSISTENT = 0x400, DISABLED = 0x800;
  const ref = (type, localId, base, at, extra = []) => record(type, PERSISTENT, own(localId), [sub('NAME', u32(base)), ...extra, sub('DATA', data(at)), parent]);
  const placed = [
    record('REFR', PERSISTENT | DISABLED, own(ID.refMarker), [sub('NAME', u32(FO4.XMarker)), sub('DATA', data(MARKER_AT))]),
    ref('ACHR', ID.refWanderer, own(ID.wanderer), WANDERER_AT),
    ref('REFR', ID.refHull, FO4.ShipWrecked01, WRECK),
    ref('REFR', ID.refBeams, FO4.ShipWreckedBeams01, WRECK),
    ref('REFR', ID.refBulkhead, FO4.ShipWreckedBulkHead01, WRECK),
    ref('REFR', ID.refCrate, FO4.WoodCrate01, crate),
    ref('REFR', ID.refGun, own(0x800), onCrate(ON_CRATE.gun)),
    ref('REFR', ID.refSpikes, own(0x801), onCrate(ON_CRATE.spikes), [sub('XCNT', i32(SPIKE_COUNT))]),
    ref('REFR', ID.refCoat, FO4.LabCoat, onCrate(ON_CRATE.coat)),
    ref('REFR', ID.refHolotape, own(ID.holotape), onCrate(ON_CRATE.holotape)),
  ];
  // the worldspace as an override, as RD_Companions writes it: without its large references (RNAM) and the master's
  // cell offsets (OFST, CLSZ: valid only in Fallout4.esm), its name written out (the master's FULL is a string ID);
  // then its persistent cell as it is, holding our references
  const worldSubs = [];
  const worldRec = esm.get(FO4.Commonwealth);
  for (const [t, d] of subsOf(worldRec, 'WRLD')) {
    if (t === 'RNAM' || t === 'OFST' || t === 'CLSZ') continue;
    worldSubs.push(sub(t, t === 'FULL' ? zstr('Commonwealth') : d));
  }
  const world = record('WRLD', worldRec.flags & ~0x40000, FO4.Commonwealth, worldSubs);
  const persistentCell = esm.get(FO4.CommonwealthPersistentCell).whole;
  const worldGroup = [world, childGroup(FO4.Commonwealth, 1, [persistentCell,
    childGroup(FO4.CommonwealthPersistentCell, 6, [childGroup(FO4.CommonwealthPersistentCell, 8, placed)])])];

  return {
    groups: { NPC_: [wanderer], NOTE: [holotape], TERM: [holotapeTerminal], WRLD: worldGroup, QUST: [questRecord, childGroup(quest, 10, questChildren)],
      MESG: [blueprintMessage] },
    ids: ID,
    report: `quest "${QUEST_NAME}": ${dialogue.length} topics and lines, scene, wanderer at ${WANDERER_AT.pos.join(', ')}, crate at ` +
      crate.pos.map(v => v.toFixed(1)).join(', '),
  };
}

// --- reading the master: the records with these form IDs, walking only the group and record headers
function readMaster(file, ids) {
  if (!file || !fs.existsSync(file)) throw new Error(`Fallout4.esm not found at ${file}: pass its path (node Tools/BuildGrapplePlugin.js <out.esp> <Fallout4.esm>)`);
  const buf = fs.readFileSync(file);
  const want = new Set(ids), found = new Map();
  (function walk(start, end) {
    let p = start;
    while (p < end && found.size < want.size) {
      const type = buf.toString('latin1', p, p + 4), size = buf.readUInt32LE(p + 4);
      if (type === 'GRUP') { walk(p + 24, p + size); p += size; continue; }
      const id = buf.readUInt32LE(p + 12);
      if (want.has(id) && !found.has(id)) found.set(id, { type, flags: buf.readUInt32LE(p + 8), whole: buf.subarray(p, p + 24 + size) });
      p += 24 + size;
    }
  })(0, buf.length);
  for (const id of ids) if (!found.has(id)) throw new Error(`${id.toString(16)} not in ${file}`);
  return found;
}

function subsOf(rec, type) {
  if (rec.type !== type) throw new Error(`expected a ${type}, found a ${rec.type}`);
  let d = rec.whole.subarray(24);
  if (rec.flags & 0x40000) d = zlib.inflateSync(d.subarray(4));
  const out = [];
  let p = 0, big = 0;
  while (p + 6 <= d.length) {
    const t = d.toString('latin1', p, p + 4);
    let size = d.readUInt16LE(p + 4);
    if (t === 'XXXX') { big = d.readUInt32LE(p + 6); p += 10; continue; }
    if (big) { size = big; big = 0; }
    out.push([t, d.subarray(p + 6, p + 6 + size)]);
    p += 6 + size;
  }
  return out;
}
