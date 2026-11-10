// Reclamation Day's launcher page. The launcher (Code/immersive_launcher/ui) serves it at https://launcher.rd/ and answers
// /api/...: the state (the game install, the mod staging folder, the look, a task in progress), the patch notes, and the
// choices made here. Opened anywhere else (a browser, for working on it) it runs on made-up data instead.
'use strict';

const LOGO = 'images/rd-logo-pipboy-512.png';
const STYLES = [
    { id: 'pipboy', name: 'Pip-Boy', detail: 'Green phosphor, the setup rail at the side' },
    { id: 'brotherhood', name: 'Brotherhood of Steel', detail: 'Gunmetal and blood red, the Prydwen over Boston' },
    { id: 'bulletin', name: 'Vault-Tec Bulletin', detail: 'Navy, cream and yellow, a poster with tabs' },
    { id: 'minutemen', name: 'Minutemen', detail: 'Continental blue and buff, at a moment\u2019s notice' },
];

const inLauncher = location.hostname === 'launcher.rd';
const api = inLauncher ? {
    get: (path) => fetch('/api/' + path).then((r) => r.json()),
    post: (path, body) => fetch('/api/' + path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) }).then((r) => r.json()),
} : mockApi();

const S = {
    data: null,
    dataText: '',
    releases: [],
    notesSource: '',
    view: 'home',  // home, game, staging, notes, style
    wizard: false, // first start: game, then staging, then home
    launching: false,
    waitingForCheck: false, // Play was pressed before the update check had answered
    skipUpdate: false,      // the update failed and the player chose to play without it
    lastError: '',
    toast: null,
    rendered: '',
};
const root = document.getElementById('app');

// --- Helpers ---

function esc(text) {
    return String(text == null ? '' : text).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function versionNumber(version) {
    const match = /-(\d+)$/.exec(version || '');
    return match ? parseInt(match[1], 10) : -1;
}

function formatDate(iso) {
    if (!iso) return '';
    const d = new Date(iso);
    return isNaN(d) ? iso : d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

// A release's notes: the lines before the list, then each "- " item (a line without one carries the item before on)
function parseBody(body) {
    const intro = [];
    const items = [];
    for (const raw of String(body || '').split(/\r?\n/)) {
        const line = raw.trim();
        if (!line) continue;
        if (line.startsWith('- ') || line.startsWith('* ')) items.push(line.slice(2).trim());
        else if (items.length) items[items.length - 1] += ' ' + line;
        else intro.push(line);
    }
    return { intro, items };
}

// "Title: the rest" when the item starts with a short name, otherwise all of it as the text
function splitItem(item) {
    const colon = item.indexOf(': ');
    if (colon > 0 && colon < 48) return { t: item.slice(0, colon), d: item.slice(colon + 2) };
    return { t: '', d: item };
}

function clip(text, length) {
    return text.length > length ? text.slice(0, length).replace(/\s+\S*$/, '') + '…' : text;
}

// An item's leading name in bold, the rest as written
function itemHtml(item) {
    const colon = item.indexOf(': ');
    return colon > 0 && colon < 48 ? '<b>' + esc(item.slice(0, colon)) + ':</b> ' + esc(item.slice(colon + 2)) : esc(item);
}

const icon = {
    min: '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M5 12h14"/></svg>',
    close: '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>',
    folder: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="M3 7h6l2 2h10v10H3z"/></svg>',
    check: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" aria-hidden="true"><path d="M4 12l5 5L20 6"/></svg>',
    cross: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>',
    play: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="M7 5l12 7-12 7z"/></svg>',
    notes: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="M6 3h9l4 4v14H6zM14 3v5h5M9 12h7M9 16h7"/></svg>',
    download: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="M12 3v12M7 10l5 5 5-5M4 19h16"/></svg>',
    setup: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="M4 7l8-4 8 4-8 4zM4 12l8 4 8-4M4 17l8 4 8-4"/></svg>',
    style: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 000 18c1.5 0 2-1 2-2s-1-1.5-1-2.5S14 15 15 15h2a4 4 0 004-4c0-4.5-4-8-9-8z"/><circle cx="7.5" cy="11" r="1.2"/><circle cx="10" cy="7" r="1.2"/><circle cx="15" cy="7.5" r="1.2"/></svg>',
};

function winButtons() {
    return '<div class="winbtns"><button type="button" class="winbtn" data-act="minimize" aria-label="Minimize">' + icon.min + '</button>' +
        '<button type="button" class="winbtn close" data-act="close" aria-label="Close">' + icon.close + '</button></div>';
}

// --- What the screens show ---

function context() {
    const d = S.data;
    const current = S.releases.find((r) => r.version === d.version) || null;
    const latest = S.releases[0] || null;
    const shown = current || latest;
    const highlights = shown ? parseBody(shown.body).items.slice(0, 3).map(splitItem).map((h) => ({ t: h.t, d: clip(h.d, 150) })) : [];
    const updateVersion = latest && versionNumber(latest.version) > versionNumber(d.version) ? latest.version : '';
    const game = d.game;
    const staging = d.staging;
    const gameOk = !!(game && game.supported);
    const stagingOk = !!staging;
    const reqs = d.requirements || [];
    const missing = reqs.filter((r) => r.status === 'missing').length;
    const staged = reqs.filter((r) => r.status === 'staged').length;
    return {
        d, view: S.view, wizard: S.wizard, current, latest, shown, highlights, updateVersion, game, staging, gameOk, stagingOk,
        reqs, missing, staged, modsOk: missing === 0,
        modsLine: missing ? missing + ' of ' + reqs.length + ' missing' : staged ? 'All here, ' + staged + ' still in your mod manager' : 'All ' + reqs.length + ' installed',
        ready: gameOk && stagingOk,
        gameName: game ? 'Fallout 4 ' + (game.version || '') : 'Fallout 4 not found',
        gamePath: game ? game.folder : 'Pick the game\u2019s install',
        stagingName: staging ? staging.name : 'Not chosen',
        stagingPath: staging ? staging.path : 'Pick where the mods go',
        stagingNote: staging ? staging.note : '',
        versionLine: d.version + (updateVersion ? ' \u00b7 ' + updateVersion + ' available' : ''),
    };
}

function checks(c) {
    const ok = (yes, text) => '<li>' + (yes ? icon.check : icon.cross) + '<span>' + esc(text) + '</span></li>';
    return '<ul class="checks">' +
        ok(c.gameOk, c.gameOk ? 'Fallout 4 ' + c.d.requiredVersion + ', the version Reclamation Day runs on' : 'Fallout 4 ' + c.d.requiredVersion + ' needed') +
        ok(c.stagingOk, c.stagingOk ? c.stagingNote || 'Game Files: ' + c.stagingName : 'Pick where Game Files\u2019 mods go') +
        ok(c.modsOk, 'Required mods: ' + c.modsLine) +
        ok(!c.updateVersion, c.updateVersion ? c.updateVersion + ' is out: the main menu offers it' : 'Reclamation Day ' + c.d.version) +
        '</ul>';
}

function setupCards(c, compact) {
    const card = (label, view, dotClass, name, path, button) =>
        '<div class="card"><div class="head"><span class="label">' + label + '</span>' +
        '<button type="button" class="btn ' + (compact ? 'link' : 'small') + '" data-act="view" data-arg="' + view + '">' + (button || 'Change') + '</button></div>' +
        '<span class="value"><span class="state-dot ' + dotClass + '"></span>' + esc(name) + '</span>' +
        '<span class="path" title="' + esc(path) + '">' + esc(path) + '</span></div>';
    return card('Game', 'game', c.gameOk ? '' : 'bad', c.gameName, c.gamePath) +
        card('Mod staging', 'staging', c.stagingOk ? '' : 'bad', c.stagingName, c.stagingPath) +
        card('Required mods', 'mods', c.modsOk ? '' : 'bad', c.modsLine, 'Downloads through your mod manager', c.modsOk ? 'Show' : 'Get them');
}

function highlightsHtml(c) {
    if (!c.highlights.length) return '<p class="hint">The patch notes load in a moment.</p>';
    return '<div class="highlights">' + c.highlights.map((h) =>
        '<div class="highlight">' + (h.t ? '<span class="t">' + esc(h.t) + '</span>' : '') + (h.d ? '<span class="d">' + esc(h.d) + '</span>' : '') + '</div>').join('') + '</div>';
}

// Back to the main screen, or on through the first start's steps
function stepFoot(c, step) {
    if (!c.wizard) return '<div class="foot"><span></span><div class="right"><button type="button" class="btn primary" data-act="view" data-arg="home">Done</button></div></div>';
    if (step === 'game') return '<div class="foot"><span class="hint">Step 1 of 3</span><div class="right"><button type="button" class="btn primary" data-act="next" ' + (c.gameOk ? '' : 'disabled') + '>Continue</button></div></div>';
    if (step === 'staging') return '<div class="foot"><button type="button" class="btn" data-act="view" data-arg="game">Back</button><div class="right"><span class="hint">Step 2 of 3</span><button type="button" class="btn primary" data-act="view" data-arg="mods" ' + (c.stagingOk ? '' : 'disabled') + '>Continue</button></div></div>';
    return '<div class="foot"><button type="button" class="btn" data-act="view" data-arg="staging">Back</button><div class="right"><span class="hint">Step 3 of 3</span><button type="button" class="btn primary" data-act="finish">' + (c.modsOk ? 'Continue' : 'Skip for now') + '</button></div></div>';
}

function viewMods(c, title) {
    const statusBadge = (r) => r.status === 'ok' ? '<span class="badge">Installed</span>'
        : r.status === 'staged' ? '<span class="badge soft">In your mod manager</span>' : '<span class="badge bad">Missing</span>';
    const rows = c.reqs.map((r, i) =>
        '<div class="req"><span class="grow"><span class="name">' + esc(r.name) + '</span><span class="detail">' + esc(r.exact ? r.which : 'Take ' + r.which) + '</span></span>' + statusBadge(r) +
        (r.status === 'ok' ? '' : '<button type="button" class="btn small" data-act="getMod" data-arg="' + i + '">Download</button>') + '</div>').join('');
    return '<section class="view enter">' + (title || '') +
        '<p class="lede">Reclamation Day builds on these mods by other authors. Download opens each on Nexus Mods as a mod manager download: log in there, press Download (Slow download on a free account), and Vortex or Mod Organizer 2 installs it. This list ticks them off as they land.</p>' +
        (c.staged ? '<p class="hint">“In your mod manager”: Vortex has it but hasn’t deployed it yet, or Mod Organizer 2 has it (make sure it’s enabled there).</p>' : '') +
        '<div class="scroll" data-scroll="mods" style="flex:1"><div class="reqs">' + rows + '</div></div>' +
        stepFoot(c, 'mods') + '</section>';
}

function viewGame(c, title) {
    const items = c.d.installs.map((it, i) => {
        const on = c.game && it.exe.toLowerCase() === c.game.exe.toLowerCase();
        const badge = it.supported ? '<span class="badge">' + esc(it.version) + ' \u00b7 supported</span>'
            : '<span class="badge bad">' + esc(it.version || 'unknown') + ' \u00b7 needs ' + esc(c.d.requiredVersion) + '</span>';
        return '<button type="button" role="radio" aria-checked="' + on + '" class="choice' + (on ? ' on' : '') + '" data-act="pickGame" data-arg="' + i + '" ' + (it.supported ? '' : 'disabled') + '>' +
            '<span class="dot"></span><span class="grow"><span class="name">' + esc(it.source) + '</span><span class="path" title="' + esc(it.folder) + '">' + esc(it.folder) + '</span></span>' + badge + '</button>';
    }).join('');
    const unsupported = c.d.installs.some((it) => !it.supported);
    return '<section class="view enter">' + (title || '') +
        '<p class="lede">Reclamation Day runs on Fallout 4 ' + esc(c.d.requiredVersion) + '. ' + (c.d.installs.length ? 'These are the installs it found on this PC.' : 'It found none on this PC: browse for Fallout4.exe.') + '</p>' +
        '<div class="choice-list" role="radiogroup" aria-label="Game install">' + items + '</div>' +
        (unsupported ? '<p class="warn">An install on another version (the next-gen update is 1.10.980 and up) needs downgrading to ' + esc(c.d.requiredVersion) + ' first, or pick a downgraded copy.</p>' : '') +
        '<div><button type="button" class="btn dashed" data-act="browseGame">' + icon.folder + 'Browse for Fallout4.exe</button></div>' +
        stepFoot(c, 'game') + '</section>';
}

function viewStaging(c, title) {
    const items = c.d.stagings.map((it, i) => {
        const on = c.staging && c.staging.kind === it.kind && (c.staging.path || '').toLowerCase() === (it.path || '').toLowerCase();
        const kind = it.detected ? 'Found' : it.kind === 'data' ? 'No mod manager' : it.kind === 'manual' ? 'Yourself' : 'Chosen';
        return '<button type="button" role="radio" aria-checked="' + on + '" class="choice tall' + (on ? ' on' : '') + '" data-act="pickStaging" data-arg="' + i + '">' +
            '<span class="top"><span class="label">' + esc(kind) + '</span><span class="dot"></span></span>' +
            '<span class="name">' + esc(it.name) + '</span><span class="detail">' + esc(it.detail) + '</span>' +
            (it.path ? '<span class="bottom path" title="' + esc(it.path) + '">' + esc(it.path) + '</span>' : '') + '</button>';
    }).join('');
    return '<section class="view enter">' + (title || '') +
        '<p class="lede">\u201cGame Files\u201d has the parts of Reclamation Day your mod manager should know about: the motorcycle\u2019s F4SE plugin and models, and the dogs\u2019 looks. The launcher keeps them in your mod manager as one mod, \u201cReclamation Day - Game Files\u201d, or puts them straight into Data. Everything else of ours goes straight into Data. The mods by other authors it needs are separate downloads, under Required mods.</p>' +
        '<div class="scroll" data-scroll="staging"><div class="choice-grid" role="radiogroup" aria-label="Mod staging">' + items + '</div></div>' +
        (c.stagingNote ? '<p class="hint">' + esc(c.stagingNote) + '</p>' : '') +
        '<div><button type="button" class="btn dashed" data-act="browseStaging">' + icon.folder + 'Choose another folder</button></div>' +
        stepFoot(c, 'staging') + '</section>';
}

function notesList(c) {
    if (!S.releases.length) return '<p class="hint">Loading the patch notes\u2026</p>';
    return '<div class="notes">' + S.releases.map((r) => {
        const body = parseBody(r.body);
        const isThis = r.version === c.d.version;
        const isNew = versionNumber(r.version) > versionNumber(c.d.version);
        return '<article class="release"><div class="rhead"><span class="ver">' + esc(r.version) + '</span><span class="date">' + esc(formatDate(r.date)) + '</span>' +
            (isThis ? '<span class="tag fill">This build</span>' : '') + (isNew ? '<span class="tag">New \u00b7 from the main menu</span>' : '') + '</div>' +
            body.intro.map((line) => '<p class="intro">' + esc(line) + '</p>').join('') +
            (body.items.length ? '<ul>' + body.items.map((item) => '<li>' + itemHtml(item) + '</li>').join('') + '</ul>' : '') + '</article>';
    }).join('') + '</div>';
}

function viewNotes(c, title) {
    return '<section class="view enter">' + (title || '') +
        '<div class="scroll" data-scroll="notes" style="flex:1">' + notesList(c) + '</div>' +
        '<div class="foot" style="padding-top:0"><span class="notes-source">' + (S.notesSource === 'live' ? 'Up to date from the releases' : S.notesSource ? 'The notes that came with this build' : '') + '</span>' +
        '<div class="right"><button type="button" class="btn primary small" data-act="view" data-arg="home">Done</button></div></div></section>';
}

function viewStyle(c, title) {
    return '<section class="view enter">' + (title || '') +
        '<p class="lede">The launcher\u2019s look. It changes nothing in the game.</p>' +
        '<div class="scroll" data-scroll="styles"><div class="styles" role="radiogroup" aria-label="Launcher style">' + STYLES.map((s) =>
            '<button type="button" role="radio" aria-checked="' + (c.d.style === s.id) + '" class="style-card' + (c.d.style === s.id ? ' on' : '') + '" data-act="style" data-arg="' + s.id + '">' +
            '<span class="sw sw-' + s.id + '"></span><span class="name">' + esc(s.name) + '</span><span class="detail">' + esc(s.detail) + '</span></button>').join('') + '</div></div>' +
        '<div class="foot"><span></span><div class="right"><button type="button" class="btn primary" data-act="view" data-arg="home">Done</button></div></div></section>';
}

function subview(c, heading) {
    if (c.view === 'game') return viewGame(c, heading('Where is Fallout 4?', c.wizard ? 'Step 01' : 'Game'));
    if (c.view === 'staging') return viewStaging(c, heading('Where should the mods go?', c.wizard ? 'Step 02' : 'Mod staging'));
    if (c.view === 'mods') return viewMods(c, heading('Required mods', c.wizard ? 'Step 03' : c.modsLine));
    if (c.view === 'notes') return viewNotes(c, heading('Patch notes', 'Every release'));
    if (c.view === 'style') return viewStyle(c, heading('Style', 'Launcher'));
    return '';
}

// --- The four looks ---

const layouts = {};

layouts.pipboy = (c) => {
    const heading = (title, kicker) => '<div><span class="step">' + esc(kicker) + '</span><h1 class="h1" style="margin-top:8px">' + esc(title) + '</h1></div>';
    const rail = c.wizard
        ? [['game', '1', 'Game install'], ['staging', '2', 'Mod staging'], ['mods', '3', 'Required mods'], ['home', '4', 'Launch']].map(([v, n, label], i) => {
            const order = ['game', 'staging', 'mods', 'home'];
            const at = order.indexOf(c.view);
            const done = i < at;
            return '<button type="button" class="rail-item' + (c.view === v ? ' on' : '') + (done ? ' done' : '') + '" ' + (i <= at ? 'data-act="view" data-arg="' + v + '"' : 'disabled') + '>' +
                '<span class="mark">' + (done ? '\u2713' : n) + '</span><span class="lbl">' + label + '</span></button>';
        }).join('')
        : [['home', 'Play'], ['game', 'Game'], ['staging', 'Mod staging'], ['mods', 'Required mods'], ['notes', 'Patch notes'], ['style', 'Style']].map(([v, label], i) =>
            '<button type="button" class="rail-item' + (c.view === v ? ' on' : '') + '" data-act="view" data-arg="' + v + '"><span class="mark">' + (i + 1) + '</span><span class="lbl">' + label + '</span></button>').join('');

    let main;
    if (c.view === 'home') {
        main = '<section class="view enter">' + heading(c.ready ? 'Ready for the Commonwealth' : 'Almost ready', 'Status') +
            '<div class="home-grid">' + setupCards(c, false) + '</div>' + checks(c) +
            '<div class="scroll" data-scroll="home" style="flex:1"><span class="label">In this build</span>' + highlightsHtml(c) + '</div>' +
            '<div class="foot"><button type="button" class="btn" data-act="view" data-arg="notes">Patch notes</button>' +
            '<div class="right"><button type="button" class="btn primary launch" data-act="play" ' + (c.ready ? '' : 'disabled') + '>Launch</button></div></div></section>';
    } else main = subview(c, heading);

    return '<div class="app T-pipboy L-pipboy"><div class="glow"></div><div class="scan"></div>' +
        '<header class="titlebar" data-drag><img src="' + LOGO + '" alt=""><span class="name">Reclamation Day</span>' + winButtons() + '</header>' +
        '<div class="body"><nav class="rail" aria-label="Launcher"><img class="rail-logo" src="' + LOGO + '" alt="Reclamation Day">' +
        '<div class="rail-title">Reclamation<br>Day</div><div class="rail-sub">Fallout 4 co-op</div><div class="rail-nav">' + rail + '</div>' +
        '<div class="rail-foot"><span class="v">' + esc(c.versionLine) + '</span></div></nav>' +
        '<main class="main">' + main + '</main></div></div>';
};

// The Prydwen over Boston, drawn here (the same every time)
function prydwenSky() {
    let seed = 7;
    const rand = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
    const row = (base, minH, maxH, minW, maxW, color, gapChance) => {
        let x = -10, out = '';
        while (x < 780) {
            const w = minW + rand() * (maxW - minW);
            if (rand() > gapChance) {
                const h = minH + rand() * (maxH - minH);
                out += '<rect x="' + x.toFixed(1) + '" y="' + (base - h).toFixed(1) + '" width="' + w.toFixed(1) + '" height="' + (h + 200).toFixed(1) + '" fill="' + color + '"/>';
                if (rand() > 0.72) out += '<rect x="' + (x + w * 0.4).toFixed(1) + '" y="' + (base - h - 18).toFixed(1) + '" width="2" height="18" fill="' + color + '"/>';
            }
            x += w + rand() * 4;
        }
        return out;
    };
    const hull = 'M230 95C262 58 400 40 540 40C610 40 660 60 682 90C660 122 610 142 540 144C400 146 262 134 230 95Z';
    let ribs = '';
    for (let x = 260; x < 680; x += 30) ribs += 'M' + x + ' 30V150';
    let lights = '';
    for (let x = 432; x < 580; x += 16) lights += '<rect x="' + x + '" y="154" width="9" height="3"/>';
    const vertibird = (x, y, scale) => '<g transform="translate(' + x + ' ' + y + ') scale(' + scale + ')" fill="#1b2125"><path d="M-22 0L-6-5L14-5L24-2L40-7L41-3L24 3L-6 5Z"/><rect x="-26" y="-11" width="24" height="1.6"/><rect x="2" y="-11" width="24" height="1.6"/><rect x="-15" y="-11" width="2" height="7"/><rect x="13" y="-11" width="2" height="7"/></g>';
    return '<svg class="sky" viewBox="0 0 760 520" preserveAspectRatio="xMidYMax slice" aria-hidden="true">' +
        '<defs><linearGradient id="bsky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#141a1f"/><stop offset="0.38" stop-color="#34424b"/><stop offset="0.62" stop-color="#65747d"/><stop offset="0.78" stop-color="#a48d76"/><stop offset="1" stop-color="#b57f5c"/></linearGradient>' +
        '<linearGradient id="bhull" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#65737c"/><stop offset="0.45" stop-color="#3a454c"/><stop offset="1" stop-color="#191f23"/></linearGradient>' +
        '<linearGradient id="bbeam" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffeec8" stop-opacity="0.2"/><stop offset="0.75" stop-color="#ffeec8" stop-opacity="0"/></linearGradient>' +
        '<linearGradient id="bhaze" x1="0" y1="0.55" x2="0" y2="1"><stop offset="0" stop-color="#14171a" stop-opacity="0"/><stop offset="1" stop-color="#14171a" stop-opacity="0.82"/></linearGradient>' +
        '<clipPath id="bclip"><path d="' + hull + '"/></clipPath></defs>' +
        '<rect width="760" height="520" fill="url(#bsky)"/>' +
        '<g fill="#0a0e11" opacity="0.28"><ellipse cx="170" cy="60" rx="240" ry="20"/><ellipse cx="620" cy="24" rx="220" ry="16"/><ellipse cx="420" cy="215" rx="300" ry="11"/></g>' +
        '<g opacity="0.7">' + row(420, 20, 70, 14, 34, '#55626a', 0.15) + '</g>' +
        '<path d="M522 184L450 470L640 470L556 184Z" fill="url(#bbeam)"/>' +
        '<g>' + row(440, 30, 130, 18, 46, '#20282d', 0.2) +
        // a broken tower and the old radio mast
        '<path d="M150 440L150 250L162 236L170 252L178 240L178 440Z" fill="#20282d"/><path d="M610 440L622 190L626 190L638 440ZM612 300L636 300M615 250L633 250" stroke="#20282d" stroke-width="3" fill="#20282d"/></g>' +
        // the fins behind the hull, then the hull, its ribs and red stripe, then the command deck under it
        '<g fill="#39444b"><path d="M628 56L704 20L694 72L650 72Z"/><path d="M628 130L704 168L694 114L650 114Z"/><path d="M656 88L708 84L708 98L656 98Z"/></g>' +
        '<path d="' + hull + '" fill="url(#bhull)"/>' +
        '<g clip-path="url(#bclip)"><path d="' + ribs + '" stroke="#12171a" stroke-width="1.4" opacity="0.55"/><rect x="220" y="116" width="480" height="6" fill="#a4231e"/><rect x="220" y="64" width="480" height="1.5" fill="#0d1114" opacity="0.5"/></g>' +
        '<path d="M400 140L600 137L584 170L520 186L440 184L416 168Z" fill="#2c353b"/><path d="M416 168L584 170L520 186L440 184Z" fill="#1e252a"/><g fill="#f0cc84">' + lights + '</g>' +
        vertibird(640, 236, 1) + vertibird(590, 266, 0.62) +
        '<g>' + row(470, 10, 60, 24, 60, '#161b1f', 0.3) + '</g>' +
        '<path d="M0 470Q120 455 220 478T460 470T760 476L760 520L0 520Z" fill="#111417"/><rect width="760" height="520" fill="url(#bhaze)"/></svg>';
}

// The Brotherhood's sword, gear and wings
function brotherhoodEmblem() {
    return '<svg class="emblem-bos" width="42" height="26" viewBox="0 0 120 72" aria-hidden="true"><g fill="currentColor">' +
        '<path d="M46 34L6 10L16 24L2 23L16 33L6 36L24 40L18 46L44 44Z"/><path d="M74 34L114 10L104 24L118 23L104 33L114 36L96 40L102 46L76 44Z"/></g>' +
        '<circle cx="60" cy="36" r="17" fill="none" stroke="currentColor" stroke-width="6" stroke-dasharray="5 3.9"/><circle cx="60" cy="36" r="13" fill="none" stroke="currentColor" stroke-width="2"/>' +
        '<path fill="#c4302b" d="M60 2L64 12L64 56L70 56L70 60L64 60L62 70L58 70L56 60L50 60L50 56L56 56L56 12Z"/></svg>';
}

layouts.brotherhood = (c) => {
    const heading = (title, kicker) => '<div><h2>' + esc(kicker) + '</h2><h1 class="h1" style="margin-top:8px">' + esc(title) + '</h1></div>';
    let aside;
    if (c.view === 'home') {
        aside = '<div style="display:flex;flex-direction:column;gap:12px;min-height:0"><h2>In this build' + (c.shown ? ' · ' + esc(c.shown.version) : '') + '</h2>' + highlightsHtml(c) +
            '<div><button type="button" class="btn link" data-act="view" data-arg="notes">All patch notes</button></div></div>' +
            '<div class="setup2">' + setupCards(c, true) + '</div>';
    } else aside = subview(c, heading);
    return '<div class="app T-brotherhood L-brotherhood"><header class="titlebar" data-drag>' + brotherhoodEmblem() + '<span class="name">Reclamation Day</span>' + winButtons() + '</header>' +
        '<div class="body"><section class="stage"><div class="art" role="img" aria-label="The Prydwen over Boston">' + prydwenSky() +
        '<div class="mark"><img class="emblem" src="' + LOGO + '" alt=""><div class="word">Reclamation<br>Day</div><div class="sub">Fallout 4 co-op</div><div class="motto">Ad Victoriam</div></div></div>' +
        '<div class="band"><button type="button" class="btn primary play" data-act="play" ' + (c.ready ? '' : 'disabled') + '>Play</button>' +
        '<button type="button" class="btn" data-act="view" data-arg="notes">Patch notes</button><button type="button" class="btn" data-act="view" data-arg="game">Setup</button>' +
        '<button type="button" class="btn" data-act="view" data-arg="style">Style</button>' +
        '<span class="ready">' + esc(c.versionLine) + '<br>' + (c.ready ? 'Ready' : 'Setup needed') + '</span></div></section>' +
        '<aside class="aside scroll" data-scroll="aside">' + aside + '</aside></div></div>';
};

layouts.bulletin = (c) => {
    const heading = (title, kicker) => '<div><span class="kicker">' + esc(kicker) + '</span><h1 class="h1" style="margin-top:6px">' + esc(title) + '</h1></div>';
    const tab = (v, label, also) => {
        const on = c.view === v || (also && also.includes(c.view));
        return '<button type="button" class="tab' + (on ? ' on' : '') + '" data-act="view" data-arg="' + v + '"' + (on ? ' aria-current="page"' : '') + '>' + label + '</button>';
    };
    const tick = (ok) => '<span class="tick' + (ok ? '' : ' bad') + '">' + (ok ? '\u2713' : '!') + '</span>';
    const side = '<div class="side"><div class="box"><span class="ttl">Readiness check</span>' +
        '<div class="check">' + tick(c.gameOk) + '<span class="grow"><b>' + esc(c.gameName) + '</b><span class="path">' + esc(c.gamePath) + '</span></span><button type="button" class="btn small" data-act="view" data-arg="game">Change</button></div>' +
        '<div class="check">' + tick(c.stagingOk) + '<span class="grow"><b>' + esc(c.stagingOk ? 'Mods: ' + c.stagingName : 'Mods not set up') + '</b><span class="path">' + esc(c.stagingPath) + '</span></span><button type="button" class="btn small" data-act="view" data-arg="staging">Change</button></div>' +
        '<div class="check">' + tick(c.modsOk) + '<span class="grow"><b>' + esc('Required mods: ' + c.modsLine) + '</b><span class="path">Downloads through your mod manager</span></span><button type="button" class="btn small" data-act="view" data-arg="mods">' + (c.modsOk ? 'Show' : 'Get') + '</button></div>' +
        '<div class="check">' + tick(!c.updateVersion) + '<span class="grow"><b>' + esc(c.updateVersion ? c.updateVersion + ' is out' : 'Up to date') + '</b><span class="path">' + esc(c.d.version) + '</span></span></div></div>' +
        '<div class="box fill"><span class="ttl">Latest notes' + (c.latest ? ' \u00b7 ' + esc(c.latest.version) : '') + '</span>' +
        '<div class="scroll" data-scroll="side">' + (c.latest ? '<ul style="margin:0;padding-left:18px;display:flex;flex-direction:column;gap:8px;font-size:14px;line-height:1.4">' +
            parseBody(c.latest.body).items.slice(0, 4).map((i) => '<li>' + esc(clip(i, 120)) + '</li>').join('') + '</ul>' : '<p class="hint">Loading\u2026</p>') + '</div>' +
        '<button type="button" class="btn small" style="align-self:flex-start" data-act="view" data-arg="notes">All patch notes</button></div></div>';

    let body;
    if (c.view === 'home') {
        body = '<div class="body"><section class="paper enter"><div class="hero"><div><span class="kicker">' + (c.ready ? 'Ready when you are' : 'Setup needed') + '</span>' +
            '<h1 class="h1" style="margin-top:6px">Rebuild it<br>together</h1></div><img class="mascot" src="' + LOGO + '" alt="Reclamation Day"></div>' +
            '<p class="lede" style="color:var(--paper-muted)">Fallout 4 co-op. Host or join from the game\u2019s MULTIPLAYER menu; your worlds, friends and campaigns are all in there.</p>' +
            '<div style="margin-top:auto;display:flex;gap:12px;align-items:center"><button type="button" class="btn primary play" data-act="play" ' + (c.ready ? '' : 'disabled') + '>Play</button>' +
            '<button type="button" class="btn" style="min-height:72px" data-act="view" data-arg="notes">Patch notes</button></div></section>' + side +
            '<footer class="footer">' + c.highlights.map((h) => '<div class="col"><span class="label">In this build</span><span class="t">' + esc(h.t) + '</span><span class="d">' + esc(clip(h.d, 110)) + '</span></div>').join('') + '</footer></div>';
    } else {
        body = '<div class="body full"><section class="paper">' + subview(c, heading) + '</section>' + side + '</div>';
    }
    return '<div class="app T-bulletin L-bulletin"><header class="titlebar" data-drag><div class="brand"><img src="' + LOGO + '" alt=""><div><div class="word">Reclamation Day</div><div class="sub">Fallout 4 co-op</div></div></div>' +
        '<nav class="tabs" aria-label="Launcher">' + tab('home', 'Play') + tab('notes', 'Patch notes') + tab('game', 'Setup', ['staging']) + tab('mods', 'Mods') + tab('style', 'Style') + '</nav>' + winButtons() + '</header>' + body + '</div>';
};

// The Minutemen's ring of thirteen stars round a pair of crossed muskets, drawn here
function minutemenFlag() {
    const star = (cx, cy, r) => {
        let d = '';
        for (let i = 0; i < 10; i++) {
            const angle = -Math.PI / 2 + i * Math.PI / 5;
            const reach = i % 2 ? r * 0.4 : r;
            d += (i ? 'L' : 'M') + (cx + reach * Math.cos(angle)).toFixed(1) + ' ' + (cy + reach * Math.sin(angle)).toFixed(1);
        }
        return d + 'Z';
    };
    let stars = '';
    for (let i = 0; i < 13; i++) {
        const angle = -Math.PI / 2 + i * 2 * Math.PI / 13;
        stars += star(60 + 49 * Math.cos(angle), 60 + 49 * Math.sin(angle), 8);
    }
    const musket = (turn) => '<g transform="rotate(' + turn + ' 60 60)"><rect x="58.6" y="20" width="2.8" height="62"/><path d="M57.5 76L62.5 76L65 96L58 96Z"/></g>';
    return '<svg class="flag" width="92" height="92" viewBox="0 0 120 120" aria-hidden="true"><g fill="currentColor"><path d="' + stars + '"/>' + musket(38) + musket(-38) + '</g></svg>';
}

layouts.minutemen = (c) => {
    const heading = (title, kicker) => '<div><h2>' + esc(kicker) + '</h2><h3 class="h2big" style="margin-top:8px">' + esc(title) + '</h3></div>';
    const nav = (v, label, svg, also, extra) => {
        const on = c.view === v || (also && also.includes(c.view));
        return '<button type="button" class="navbtn' + (on ? ' on' : '') + (extra || '') + '" data-act="view" data-arg="' + v + '"' + (on ? ' aria-current="page"' : '') + '>' + svg + label + '</button>';
    };
    const status = 'RECLAMATION DAY · ' + c.versionLine.toUpperCase() + (c.updateVersion ? '' : ' · UP TO DATE');
    const top = '<section class="top"><div><h1 class="h1">Reclamation Day</h1><div class="subline">FALLOUT 4 CO-OP · ' + (c.ready ? 'READY' : 'SETUP NEEDED') + '</div><div class="motto">At a moment’s notice</div></div>' +
        minutemenFlag() +
        '<div style="display:flex;gap:10px"><button type="button" class="btn" data-act="view" data-arg="notes">Notes</button><button type="button" class="btn primary play" data-act="play" ' + (c.ready ? '' : 'disabled') + '>Play ▸</button></div></section>';
    let panes;
    if (c.view === 'home') {
        const items = c.shown ? parseBody(c.shown.body).items.slice(0, 6) : [];
        panes = '<section class="pane"><h2>Changelog · ' + esc(c.shown ? c.shown.version : 'this build') + '</h2><div class="scroll" data-scroll="log"><ul class="log">' +
            (items.length ? items.map((i) => '<li><span class="plus">★</span> ' + esc(clip(i, 200)) + '</li>').join('') : '<li>loading…</li>') + '</ul></div>' +
            '<button type="button" class="btn small" style="align-self:flex-start" data-act="view" data-arg="notes">All notes</button></section>' +
            '<aside class="pane scroll" data-scroll="setup"><h2>Setup</h2>' + setupCards(c, true) + '<h2 style="margin-top:6px">Checks</h2>' + checks(c) + '</aside>';
    } else {
        panes = '<section class="pane wide">' + subview(c, heading) + '</section>';
    }
    return '<div class="app T-minutemen L-minutemen"><nav class="rail" aria-label="Launcher"><img src="' + LOGO + '" alt="Reclamation Day">' +
        nav('home', 'Play', icon.play) + nav('notes', 'Notes', icon.notes) + nav('game', 'Setup', icon.setup, ['staging']) + nav('mods', 'Mods', icon.download) +
        nav('style', 'Style', icon.style, null, ' bottom') + '</nav>' +
        '<div class="col"><header class="titlebar" data-drag><span>' + esc(status) + '</span>' + winButtons() + '</header>' +
        '<div class="grid">' + top + panes + '</div></div></div>';
};

// --- The update ---

// The update, when it has the window: a newer release to install, one installing, or one that failed. Nothing else can
// be done until then, the first start's setup included, so a player only ever sets up and plays the latest version.
function updateHolding(d) {
    const update = d && d.update;
    if (!update || S.launching) return null;
    if (update.state === 'available' || update.state === 'updating' || update.state === 'ready') return update;
    return update.state === 'failed' && !S.skipUpdate ? update : null;
}

// The updater's progress messages are in capitals, for the game's main menu
function sentence(text) {
    const lower = String(text || '').toLowerCase();
    return lower.charAt(0).toUpperCase() + lower.slice(1);
}

// What the update brings: the notes of every release since this build, once the live list is in
function updateNotes(c) {
    const fresh = S.releases.filter((r) => versionNumber(r.version) > versionNumber(c.d.version));
    const items = fresh.flatMap((r) => parseBody(r.body).items);
    if (!items.length) return '';
    return '<div class="update-notes scroll" data-scroll="update"><span class="label">What\u2019s new</span><ul>' + items.map((item) => '<li>' + itemHtml(item) + '</li>').join('') + '</ul></div>';
}

function updateOverlay(c, update) {
    const close = '<button type="button" class="btn" data-act="close">Close</button>';
    const frame = (label, content, canClose) => '<div class="overlay update" data-drag><div class="winbtns"><button type="button" class="winbtn" data-act="minimize" aria-label="Minimize">' + icon.min + '</button>' +
        (canClose ? '<button type="button" class="winbtn close" data-act="close" aria-label="Close">' + icon.close + '</button>' : '') + '</div>' +
        '<div class="box"><span class="label">' + label + '</span>' + content + '</div></div>';

    if (update.state === 'available') {
        return frame('Update available', '<span class="msg">' + esc(update.version) + ' is out. You have ' + esc(c.d.version) + '.</span>' +
            '<p class="hint">Players and servers must be on the same version, so it goes in before you play. This installs the launcher and the mod together, stops a server running from this install, and opens the launcher again.</p>' +
            updateNotes(c) +
            '<div class="update-actions"><button type="button" class="btn primary" data-act="update">Install update</button><span class="right">' + close + '</span></div>', true);
    }
    if (update.state === 'failed') {
        const why = String(update.error || 'Something went wrong');
        return frame('Update failed', '<span class="msg">' + esc(update.version) + ' didn\u2019t install.</span>' +
            '<p class="warn">' + esc(why.charAt(0).toUpperCase() + why.slice(1)) + (/[.?!]$/.test(why) ? '' : '.') + ' Nothing was changed.</p>' +
            '<div class="update-actions"><button type="button" class="btn primary" data-act="update">Try again</button>' +
            '<button type="button" class="btn" data-act="skipUpdate">Play without it</button><span class="right">' + close + '</span></div>', true);
    }
    const known = update.percent >= 0;
    return frame(update.state === 'ready' ? 'Update installed' : 'Updating to ' + esc(update.version), '<span class="msg">' + esc(sentence(update.message)) + '</span>' +
        '<div class="bar' + (known ? '' : ' indeterminate') + '"><i style="width:' + (known ? update.percent : 0) + '%"></i></div>' +
        '<p class="hint">The launcher opens again when it\u2019s done.</p>', false);
}

// --- Drawing ---

function render() {
    if (!S.data) return;
    const c = context();
    const style = layouts[c.d.style] ? c.d.style : 'pipboy';
    const holding = updateHolding(S.data);
    // the look's tokens here too: the overlay and the toast are drawn next to the look's frame, not inside it
    root.className = 'app T-' + style + (holding ? ' update-held' : '');
    let html = layouts[style](c);
    if (holding) {
        html += updateOverlay(c, holding);
    } else if (S.data.busy || S.launching) {
        const busy = S.data.busy;
        const pct = busy && busy.total ? Math.round(100 * busy.done / busy.total) : 0;
        const waiting = S.waitingForCheck && !busy;
        html += '<div class="overlay"><div class="box"><span class="label">' + (S.launching ? 'Launching' : 'Working') + '</span>' +
            '<span class="msg">' + esc(busy ? busy.message : waiting ? 'Checking for updates\u2026' : 'Starting Fallout 4\u2026') + '</span>' +
            '<div class="bar' + (busy && busy.total ? '' : ' indeterminate') + '"><i style="width:' + pct + '%"></i></div>' +
            (busy && busy.total ? '<span class="hint">' + busy.done + ' of ' + busy.total + ' files</span>' : '') + '</div></div>';
    }
    if (S.toast) html += '<div class="toast' + (S.toast.ok ? ' ok' : '') + '" role="status">' + esc(S.toast.text) + '</div>';
    if (html === S.rendered) return;

    const scrolls = {};
    root.querySelectorAll('[data-scroll]').forEach((el) => { scrolls[el.dataset.scroll] = el.scrollTop; });
    const focusedAct = document.activeElement && document.activeElement.dataset ? document.activeElement.dataset.act + '|' + (document.activeElement.dataset.arg || '') : '';
    const sameView = root.dataset.view === S.view + '|' + style;
    const overlayUp = !!root.querySelector('.overlay');
    root.innerHTML = html;
    if (overlayUp) root.querySelectorAll('.overlay').forEach((el) => el.classList.add('still'));
    root.dataset.view = S.view + '|' + style;
    S.rendered = html;
    // a redraw of the same screen (new data) keeps its place and doesn't play the entrance again
    if (sameView) {
        root.querySelectorAll('.enter').forEach((el) => el.classList.remove('enter'));
        root.querySelectorAll('[data-scroll]').forEach((el) => { if (scrolls[el.dataset.scroll]) el.scrollTop = scrolls[el.dataset.scroll]; });
        if (focusedAct) {
            const [act, arg] = focusedAct.split('|');
            const el = root.querySelector('[data-act="' + act + '"]' + (arg ? '[data-arg="' + arg + '"]' : ''));
            if (el) el.focus();
        }
    }
}

let toastTimer = 0;
function toast(text, ok) {
    S.toast = { text, ok: !!ok };
    render();
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { S.toast = null; render(); }, 5000);
}

async function refresh() {
    try {
        const data = await api.get('state');
        const text = JSON.stringify(data);
        if (text !== S.dataText) {
            S.dataText = text;
            S.data = data;
            // a copy that failed, whether Play or a new staging choice started it
            if (data.error && !data.busy && data.error !== S.lastError) {
                S.launching = false;
                toast(data.error);
            }
            S.lastError = data.error;
            // Play pressed during the update check: on now that it has answered, unless there's an update to install
            if (S.waitingForCheck && !(data.update && data.update.state === 'checking')) {
                S.waitingForCheck = false;
                if (data.update && data.update.state === 'available') S.launching = false;
                else play();
            }
            render();
        }
    } catch (e) {
        console.error('state', e);
    }
}

// GitHub's releases or our own list: version, date, notes; drafts out, and a release with the same notes as the one
// after it (one published again under a new version) once
function setReleases(list, source) {
    const seen = new Set();
    S.releases = list.map((r) => ({ version: r.tag_name || r.version, date: r.published_at || r.date, body: r.body || '', draft: !!r.draft }))
        .filter((r) => r.version && !r.draft)
        .sort((a, b) => versionNumber(b.version) - versionNumber(a.version))
        .filter((r) => { const key = r.body.trim(); if (key && seen.has(key)) return false; seen.add(key); return true; });
    S.notesSource = source;
    render();
}

// The notes that came with this build at once, then the releases' own once the launcher has them
let notesTries = 0;
async function loadNotes() {
    try {
        if (!S.releases.length) setReleases(await fetch('patchnotes.json').then((r) => r.json()), 'built-in');
        const answer = await api.get('notes');
        if (answer.releases) setReleases(answer.releases, 'live');
        else if (answer.pending && ++notesTries < 15) setTimeout(loadNotes, 1000);
    } catch (e) {
        console.error('notes', e);
    }
}

// --- Choices ---

async function play() {
    const answer = await api.post('play');
    S.waitingForCheck = !!answer.checking;
    if (!answer.ok && !answer.checking) {
        S.launching = false;
        toast(answer.error || 'Fallout 4 can\u2019t start yet.');
    }
    render();
}

async function act(name, arg) {
    const d = S.data;
    switch (name) {
    case 'view':
        S.view = arg;
        if (arg === 'home') S.wizard = S.wizard && !d.setupDone;
        render();
        break;
    case 'next':
        S.view = 'staging';
        render();
        break;
    case 'finish':
        S.wizard = false;
        S.view = 'home';
        render();
        break;
    case 'pickGame': {
        const it = d.installs[+arg];
        const answer = await api.post('game', { exe: it.exe });
        if (!answer.ok) toast(answer.error || 'That install can\u2019t be used.');
        await refresh();
        break;
    }
    case 'browseGame': {
        const answer = await api.post('browseGame');
        if (!answer.ok && !answer.cancelled) toast(answer.error || 'That isn\u2019t Fallout4.exe.');
        await refresh();
        break;
    }
    case 'pickStaging': {
        const it = d.stagings[+arg];
        const answer = await api.post('staging', { kind: it.kind, path: it.path || '' });
        if (!answer.ok) toast(answer.error || 'That folder can\u2019t be used.');
        await refresh();
        break;
    }
    case 'browseStaging': {
        const answer = await api.post('browseStaging');
        if (!answer.ok && !answer.cancelled) toast(answer.error || 'That folder can\u2019t be used.');
        await refresh();
        break;
    }
    case 'style':
        d.style = arg;
        render();
        await api.post('style', { style: arg });
        await refresh();
        break;
    case 'play': {
        if (S.launching) break;
        S.launching = true;
        render();
        await play();
        break;
    }
    case 'update': {
        const answer = await api.post('update');
        if (!answer.ok) toast(answer.error || 'The update couldn\u2019t start.');
        await refresh();
        break;
    }
    case 'skipUpdate':
        S.skipUpdate = true;
        render();
        break;
    case 'getMod': {
        const answer = await api.post('getMod', { index: String(arg) });
        if (!answer.ok) toast(answer.error || 'Couldn’t open Nexus Mods.');
        else toast('Opened on Nexus Mods: press Download there, and your mod manager takes it.', true);
        break;
    }
    case 'minimize':
    case 'close':
        api.post('window', { op: name });
        break;
    }
}

root.addEventListener('click', (e) => {
    const el = e.target.closest('[data-act]');
    if (!el || el.disabled) return;
    // a screen under the update's overlay can still be reached with the keyboard
    if (updateHolding(S.data) && !el.closest('.overlay')) return;
    act(el.dataset.act, el.dataset.arg);
});

// Dragging the window by its title bar (not by its buttons)
root.addEventListener('mousedown', (e) => {
    if (e.button !== 0 || !e.target.closest('[data-drag]') || e.target.closest('button')) return;
    e.preventDefault();
    api.post('window', { op: 'drag' });
});

document.addEventListener('keydown', (e) => {
    if (!S.data || S.data.busy || S.launching || updateHolding(S.data)) return;
    if (e.key === 'Escape' && S.view !== 'home' && !S.wizard) act('view', 'home');
    else if (e.key === 'Enter' && S.view === 'home' && e.target === document.body) act('play');
});
document.addEventListener('contextmenu', (e) => e.preventDefault());

(async () => {
    await refresh();
    if (S.data && !S.data.setupDone) {
        S.wizard = true;
        S.view = S.data.game && S.data.game.supported ? 'staging' : 'game';
        render();
    }
    loadNotes();
    // quickly while something is moving: a copy, the launch, the update's check or its progress
    const pause = () => {
        const update = S.data && S.data.update;
        return S.data && (S.data.busy || S.launching || (update && update.state !== 'available' && update.state !== 'failed')) ? 400 : 3000;
    };
    const tick = async () => {
        await refresh();
        setTimeout(tick, pause());
    };
    setTimeout(tick, pause());
})();

// --- Made-up data, outside the launcher ---

function mockApi() {
    const params = new URLSearchParams(location.search);
    const state = {
        version: 'v1.6.7-280',
        requiredVersion: '1.10.163',
        style: params.get('style') || 'pipboy',
        setupDone: params.get('first') !== '1',
        game: { exe: 'F:\\SteamLibrary\\steamapps\\common\\Fallout 4\\Fallout4.exe', folder: 'F:\\SteamLibrary\\steamapps\\common\\Fallout 4', version: '1.10.163', supported: true, source: 'Steam library' },
        installs: [
            { exe: 'F:\\SteamLibrary\\steamapps\\common\\Fallout 4\\Fallout4.exe', folder: 'F:\\SteamLibrary\\steamapps\\common\\Fallout 4', version: '1.10.163', supported: true, source: 'Steam library' },
            { exe: 'C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4\\Fallout4.exe', folder: 'C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout 4', version: '1.10.984', supported: false, source: 'Steam, default library' },
        ],
        staging: params.get('first') === '1' ? null : { kind: 'vortex', path: 'F:\\Vortex Mods\\fallout4', name: 'Vortex', note: 'In Vortex as \u201cReclamation Day - Game Files\u201d: enable it there and deploy.' },
        stagings: [
            { kind: 'vortex', name: 'Vortex', detail: 'Its Fallout 4 staging folder. Vortex lists the mod; enable it and deploy.', path: 'F:\\Vortex Mods\\fallout4', detected: true },
            { kind: 'mo2', name: 'Mod Organizer 2 \u00b7 Fallout 4', detail: 'The instance\u2019s mods folder. Enable the mod in MO2\u2019s left pane.', path: 'C:\\Modding\\MO2\\mods', detected: true },
            { kind: 'data', name: 'No mod manager', detail: 'The launcher puts them straight into the game\u2019s Data folder.', path: 'F:\\SteamLibrary\\steamapps\\common\\Fallout 4\\Data', detected: false },
            { kind: 'manual', name: 'I install them myself', detail: 'Nothing is copied: install Game Files with your own tools, as the README says.', path: '', detected: false },
        ],
        requirements: [
            { name: 'F4SE (Fallout 4 Script Extender)', which: '0.6.23, the one for 1.10.163', exact: true, status: 'ok' },
            { name: 'Address Library for F4SE Plugins', which: 'the file for 1.10.163', exact: true, status: 'ok' },
            { name: 'Full Body First Person', which: 'the main file', exact: false, status: 'ok' },
            { name: 'Unofficial Fallout 4 Patch', which: '2.2.2a', exact: true, status: 'staged' },
            { name: 'Start Me Up Redux', which: 'Normal - XDI Dialogue, 9.2', exact: true, status: 'missing' },
            { name: 'KARMA', which: '3.4.1', exact: true, status: 'missing' },
            { name: 'PipboyTabs', which: 'the file for 1.10.163', exact: true, status: 'missing' },
            { name: 'Upscaling', which: 'the Pre-NG (version 1.10.163) file under Update files', exact: false, status: 'missing' },
        ],
        busy: null,
        error: '',
        update: null,
    };
    if (params.get('first') === '1') state.game = null;
    // ?update=available, updating, failed or checking
    if (params.get('update')) {
        state.update = { state: params.get('update'), version: 'v1.6.7-285', message: 'DOWNLOADING THE UPDATE (1 OF 2)... 42%', percent: 42, error: 'the file server answered 503' };
    }
    return {
        get: async (path) => {
            if (path === 'state') return JSON.parse(JSON.stringify(state));
            if (path === 'notes') return { source: 'built-in' };
            return {};
        },
        post: async (path, body) => {
            if (path === 'style') state.style = body.style;
            if (path === 'game') { state.game = state.installs.find((it) => it.exe === body.exe) || state.game; }
            if (path === 'staging') {
                const it = state.stagings.find((s) => s.kind === body.kind);
                state.staging = { kind: it.kind, path: it.path, name: it.name, note: it.kind === 'manual' ? 'Nothing is copied.' : 'Staged as \u201cReclamation Day - Game Files\u201d.' };
            }
            if (path === 'update') state.update = { state: 'updating', version: state.update.version, message: 'DOWNLOADING THE UPDATE (1 OF 2)... 42%', percent: 42, error: '' };
            if (path === 'play') {
                let done = 0;
                state.busy = { message: 'Putting Game Files in Vortex', done, total: 412 };
                const timer = setInterval(() => {
                    done = Math.min(412, done + 37);
                    state.busy = done < 412 ? { message: 'Putting Game Files in Vortex', done, total: 412 } : { message: 'Starting Fallout 4\u2026', done: 0, total: 0 };
                    if (done >= 412) clearInterval(timer);
                }, 300);
            }
            if (state.game && state.staging) state.setupDone = true;
            return { ok: true };
        },
    };
}
