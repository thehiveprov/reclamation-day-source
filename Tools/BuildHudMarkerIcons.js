// The compass's marker icons for the overlay's HUD, remade from Fallout 4's own (HUDMenu.swf, read from the player's
// game, nothing of it kept here) in Reclamation Day's look: on a 24 unit grid (y down), filled shapes drawn in scan
// lines as the HUD markers are (Tools/HudMarkerIcons/make_icon.ps1: each line's spans at its top and bottom edges as
// quads, the middle's when they differ), the undiscovered locations' outlines kept as thin solid lines. Writes
// Code/skyrim_ui/src/app/components/hud/hud-marker-icons.ts.
//   node Tools/BuildHudMarkerIcons.js [HUDMenu.swf]   (default: the vanilla movie in Fallout4 - Interface.ba2)
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const { execFileSync } = require('child_process');

const kGame = String.raw`F:\SteamLibrary\steamapps\common\Fallout 4`;
const kOut = path.join(__dirname, '..', 'Code', 'skyrim_ui', 'src', 'app', 'components', 'hud', 'hud-marker-icons.ts');
// The grid, the share of it a location's shape and a marker kind's shape take, scan lines over each shape's height
const kGrid = 24;
const kLocationSize = 13;
const kKindSize = 20;
const kLocationLines = 7;
const kKindLines = 9;
// The red kinds are tiny next to a quest marker at the game's own sizes: larger, so they read on the compass
const kKindBoost = { enemy: 2.6, recon: 1.6, enemyTargeted: 1.6 };
const kLineFill = 0.7;
// Narrower spans than this (grid units) are left out: a line's sliver at a point reads as a speck
const kMinSpan = 0.5;
// Upright strokes narrower than this (grid units) are drawn through the gaps between lines
const kThin = 1.6;
const kCurveSteps = 8;

// The game's map marker icons in its own order (+0x30 of a compass record: id 511543's labels in the exe), each with
// the movie's class for it ("KiddieKingdome" is the exe's spelling, the movie's class is KiddieKingdomMarker)
const kIcons = ['Cave', 'City', 'DiamondCity', 'Encampment', 'Factory', 'Monument', 'Metro', 'MilitaryBase', 'Landmark', 'Office', 'TownRuins', 'UrbanRuins',
  'SancHills', 'Settlement', 'Sewer', 'Vault', 'Airfield', 'BunkerHill', 'Camper', 'Car', 'Church', 'CountryClub', 'CustomHouse', 'DriveIn', 'ElevatedHighway',
  'FaneuilHall', 'Farm', 'FillingStation', 'Forested', 'Goodneighbor', 'Graveyard', 'Hospital', 'IndustrialDome', 'IndustrialStacks', 'Institute', 'IrishPride',
  'Junkyard', 'Observatory', 'Pier', 'PondLake', 'Quarry', 'RadioactiveArea', 'RadioTower', 'Salem', 'School', 'Shipwreck', 'Submarine', 'SwanPond', 'SynthHead',
  'Town', 'BoS', 'Brownstone', 'Bunker', 'Castle', 'Skyscraper', 'Libertalia', 'LowRise', 'Minutemen', 'PoliceStation', 'Prydwen', 'RailroadFaction', 'Railroad',
  'Satellite', 'Sentinel', 'USSConstitution', 'Mechanist', 'RaiderSettlement', 'VassalSettlement', 'PotentialVassalSettlement', 'BottlingPlant', 'Galactic', 'Hub',
  'KiddieKingdom', 'Monorail', 'Rides', 'Safari', 'WildWest', 'POI', 'Disciples', 'Operators', 'Pack', 'Door', 'Quest', 'Door', 'Quest', 'PlayerSet', 'PlayerLoc',
  'PowerArmorLoc', 'Dogmeat', 'Teammate'];
// Icons without the two location frames: one look (the people's at their level, the others as they are)
const kSingleFrame = { Door: '', Quest: '', PlayerSet: '', PowerArmorLoc: 'LevelWithPlayer', Dogmeat: 'LevelWithPlayer', Teammate: 'LevelWithPlayer' };
// The compass's own marker kinds (the movie's CompassMarkerWidget frames and what they hold)
const kKinds = {
  quest: ['HUDMenu_fla.QuestCompassMarkers_136', 'LevelWithPlayer'],
  questDoor: ['HUDMenu_fla.QuestDoorCompassMarkers_140', 'LevelWithPlayer'],
  playerSet: ['HUDMenu_fla.PlayerSetCompassMarker_143', 'LevelWithPlayer'],
  recon: ['HUDMenu_fla.ReconScopeMarker_146', 'LevelWithPlayer'],
  enemy: ['HUDMenu_fla.CompassMarker_Enemy_148', 'LevelWithPlayer'],
  enemyTargeted: ['CompassMarkerWidget', 'EnemyTargeted'],
};

// The movie: given, or out of the game's interface archive
const readFromArchive = (archive, want) => {
  const fd = fs.openSync(archive, 'r');
  const head = Buffer.alloc(24); fs.readSync(fd, head, 0, 24, 0);
  const count = head.readUInt32LE(12), names = Number(head.readBigUInt64LE(16));
  const recs = Buffer.alloc(count * 36); fs.readSync(fd, recs, 0, recs.length, 24);
  const nt = Buffer.alloc(fs.fstatSync(fd).size - names); fs.readSync(fd, nt, 0, nt.length, names);
  let p = 0;
  for (let i = 0; i < count; i++) {
    const len = nt.readUInt16LE(p); const name = nt.toString('latin1', p + 2, p + 2 + len); p += 2 + len;
    if (name.toLowerCase() !== want) continue;
    const r = i * 36, off = Number(recs.readBigUInt64LE(r + 16)), packed = recs.readUInt32LE(r + 24), unpacked = recs.readUInt32LE(r + 28);
    const data = Buffer.alloc(packed || unpacked); fs.readSync(fd, data, 0, data.length, off);
    return packed ? zlib.inflateSync(data) : data;
  }
  throw new Error(`${want} not in ${archive}`);
};
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'hudicons-'));
let swf = process.argv[2];
if (!swf) {
  swf = path.join(work, 'HUDMenu.swf');
  fs.writeFileSync(swf, readFromArchive(path.join(kGame, 'Data', 'Fallout4 - Interface.ba2'), 'interface\\hudmenu.swf'));
}

// Every shape wanted, flattened by Tools/SwfVectors.js
const spec = [];
kIcons.forEach((name, i) => {
  if (name in kSingleFrame) spec.push({ key: `map${i}`, symbol: `${name}Marker`, frame: kSingleFrame[name] });
  else spec.push({ key: `map${i}`, symbol: `${name}Marker`, frame: 'Discovered' }, { key: `map${i}u`, symbol: `${name}Marker`, frame: 'Undiscovered' });
});
for (const [name, [symbol, frame]] of Object.entries(kKinds)) spec.push({ key: name, symbol, frame });
fs.writeFileSync(path.join(work, 'spec.json'), JSON.stringify(spec));
execFileSync('node', [path.join(__dirname, 'SwfVectors.js'), swf, 'json', path.join(work, 'spec.json'), path.join(work, 'shapes.json')], { stdio: 'inherit' });
const shapes = JSON.parse(fs.readFileSync(path.join(work, 'shapes.json'), 'utf8'));
fs.rmSync(work, { recursive: true, force: true });

// A shape as closed polygons (curves in steps); strokes as thin rectangles, wound as the fills are
const polygons = (shape) => {
  const out = [];
  const point = (s, t) => {
    if (!s.c) return [s.from[0] + (s.to[0] - s.from[0]) * t, s.from[1] + (s.to[1] - s.from[1]) * t];
    const u = 1 - t;
    return [u * u * s.from[0] + 2 * u * t * s.c[0] + t * t * s.to[0], u * u * s.from[1] + 2 * u * t * s.c[1] + t * t * s.to[1]];
  };
  for (const fill of shape.fills) {
    for (const loop of fill.loops) {
      const pts = [loop[0].from];
      for (const s of loop) for (let k = 1; k <= (s.c ? kCurveSteps : 1); k++) pts.push(point(s, k / (s.c ? kCurveSteps : 1)));
      out.push(pts);
    }
  }
  for (const stroke of shape.strokes) {
    for (const s of stroke.segs) {
      const steps = s.c ? kCurveSteps : 1;
      for (let k = 0; k < steps; k++) {
        const a = point(s, k / steps), b = point(s, (k + 1) / steps);
        const dx = b[0] - a[0], dy = b[1] - a[1], len = Math.hypot(dx, dy) || 1, h = stroke.width / 2;
        const nx = (-dy / len) * h, ny = (dx / len) * h;
        out.push([[a[0] + nx, a[1] + ny], [b[0] + nx, b[1] + ny], [b[0] - nx, b[1] - ny], [a[0] - nx, a[1] - ny]]);
      }
    }
  }
  return out;
};
const bounds = (polys) => {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const p of polys) for (const [x, y] of p) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
  return { x0, y0, x1, y1 };
};
// Onto the grid: the larger side `size` units, centred on the grid's middle (both frames of a location the same way)
const fitter = (b, size) => {
  const scale = size / Math.max(b.x1 - b.x0, b.y1 - b.y0);
  const cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
  return (polys) => polys.map((p) => p.map(([x, y]) => [kGrid / 2 + (x - cx) * scale, kGrid / 2 + (y - cy) * scale]));
};
const n = (v) => (Math.round(v * 10) / 10).toString();
// A shape's fills with their curves, through the same fit (one point at a time)
const curvedPath = (shape, fit) => {
  const at = (p) => fit([[p]])[0][0];
  let d = '';
  for (const fill of shape.fills) {
    for (const loop of fill.loops) {
      const [x0, y0] = at(loop[0].from); d += `M${n(x0)} ${n(y0)}`;
      for (const sg of loop) {
        const [x, y] = at(sg.to);
        if (sg.c) { const [cx, cy] = at(sg.c); d += `Q${n(cx)} ${n(cy)} ${n(x)} ${n(y)}`; } else d += `L${n(x)} ${n(y)}`;
      }
      d += 'Z';
    }
  }
  return d;
};

// Where a row crosses the shape: spans with a non-zero winding
const spans = (polys, y) => {
  const hits = [];
  for (const p of polys) {
    for (let i = 0; i < p.length; i++) {
      const [ax, ay] = p[i], [bx, by] = p[(i + 1) % p.length];
      if (ay === by || y < Math.min(ay, by) || y >= Math.max(ay, by)) continue;
      hits.push({ x: ax + ((y - ay) / (by - ay)) * (bx - ax), w: by > ay ? 1 : -1 });
    }
  }
  hits.sort((a, b) => a.x - b.x);
  const out = []; let wind = 0, start = 0;
  for (const h of hits) {
    const before = wind; wind += h.w;
    if (before === 0 && wind !== 0) start = h.x;
    else if (before !== 0 && wind === 0 && h.x - start >= kMinSpan) out.push([start, h.x]);
  }
  return out;
};
// Spans along several rows merged: what a band of them covers anywhere (a thin horizontal edge inside it included)
const unionSpans = (polys, ys) => {
  const all = ys.flatMap((y) => spans(polys, y)).sort((a, b) => a[0] - b[0]);
  const out = [];
  for (const s of all) {
    const last = out[out.length - 1];
    if (last && s[0] <= last[1]) last[1] = Math.max(last[1], s[1]);
    else out.push([...s]);
  }
  return out;
};
// The scan lines: over the shape's height, each line kLineFill of its pitch. A line's spans at its top and bottom as
// quads that follow the shape's slopes when both edges agree, else what the line covers anywhere, straight. Thin
// upright strokes (narrower than kThin) are carried through the gaps, so a stem or an outline's side stays whole
const scanPath = (polys, lines) => {
  const b = bounds(polys);
  const pitch = (b.y1 - b.y0) / lines;
  const rect = (x0, x1, y0, y1) => `M${n(x0)} ${n(y0)}L${n(x1)} ${n(y0)}L${n(x1)} ${n(y1)}L${n(x0)} ${n(y1)}Z`;
  let d = '';
  for (let i = 0; i < lines; i++) {
    const top = b.y0 + i * pitch + 0.01, bottom = b.y0 + i * pitch + pitch * kLineFill - 0.01;
    const st = spans(polys, top), sb = spans(polys, bottom);
    const rows = [0, 0.25, 0.5, 0.75, 1].map((t) => top + (bottom - top) * t);
    const covered = unionSpans(polys, rows);
    const agree = st.length && st.length === sb.length && st.length === covered.length;
    if (agree) for (let s = 0; s < st.length; s++) d += `M${n(st[s][0])} ${n(top)}L${n(st[s][1])} ${n(top)}L${n(sb[s][1])} ${n(bottom)}L${n(sb[s][0])} ${n(bottom)}Z`;
    else for (const [x0, x1] of covered) d += rect(x0, x1, top, bottom);
    // the gap under the line
    const gapTop = bottom, gapBottom = Math.min(b.y1, b.y0 + (i + 1) * pitch + 0.01);
    if (i < lines - 1 && gapBottom > gapTop) {
      for (const [x0, x1] of unionSpans(polys, [gapTop, (gapTop + gapBottom) / 2, gapBottom])) if (x1 - x0 < kThin) d += rect(x0, x1, gapTop, gapBottom);
    }
  }
  return d;
};

const map = {};
kIcons.forEach((name, i) => {
  // PlayerLoc is the map's own (the player on the Pip-Boy map), not in the HUD's movie
  if (!shapes[`map${i}`])
    return;
  const discovered = polygons(shapes[`map${i}`]);
  const undiscovered = shapes[`map${i}u`] ? polygons(shapes[`map${i}u`]) : null;
  const fit = fitter(bounds(undiscovered ? discovered.concat(undiscovered) : discovered), kLocationSize);
  const entry = { name, lines: scanPath(fit(discovered), kLocationLines) };
  if (undiscovered) entry.outline = curvedPath(shapes[`map${i}u`], fit);
  map[i] = entry;
});
// The kinds at one scale, the quest marker's (the game's sizes against each other kept), each centred
const kinds = {};
const questBounds = bounds(polygons(shapes.quest));
const kindScale = kKindSize / Math.max(questBounds.x1 - questBounds.x0, questBounds.y1 - questBounds.y0);
for (const name of Object.keys(kKinds)) {
  const polys = polygons(shapes[name]);
  const b = bounds(polys);
  const scale = kindScale * (kKindBoost[name] ?? 1);
  const fit = fitter(b, scale * Math.max(b.x1 - b.x0, b.y1 - b.y0));
  const fitted = fit(polys);
  const height = (b.y1 - b.y0) * scale;
  // a small shape gets fewer lines, at the quest marker's pitch
  kinds[name] = { lines: scanPath(fitted, Math.max(2, Math.round((kKindLines * height) / kKindSize))) };
}

const ts = `// Generated by Tools/BuildHudMarkerIcons.js from Fallout 4's own compass icons, remade in scan lines: do not edit.
/* eslint-disable */

/** An icon on a ${kGrid} unit grid (y down): the shape in scan lines, and a location's outline (undiscovered). */
export interface HudMarkerIcon {
  name?: string;
  lines: string;
  outline?: string;
}

/** The map marker icons in the game's own order (a compass record's icon, 0-89). */
export const HUD_MAP_ICONS: Record<number, HudMarkerIcon> = ${JSON.stringify(map)};

/** The compass's own marker kinds. */
export const HUD_KIND_ICONS: Record<'quest' | 'questDoor' | 'playerSet' | 'recon' | 'enemy' | 'enemyTargeted', HudMarkerIcon> = ${JSON.stringify(kinds)};
`;
fs.writeFileSync(kOut, ts);
console.log(`wrote ${kOut}: ${Object.keys(map).length} map icons, ${Object.keys(kinds).length} kinds, ${(ts.length / 1024).toFixed(0)} KB`);
