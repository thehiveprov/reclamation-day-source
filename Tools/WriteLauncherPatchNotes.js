// Writes Code/launcher_ui/patchnotes.json, the patch notes the launcher shows when it can't reach the releases: every
// published release of the updates repo (its version, date and notes), newest first.
//
//   node Tools/WriteLauncherPatchNotes.js [--token-file <file>] [--add <version> <notes file>]
//
// --add puts the release being made in too (its notes as given to PublishUpdate.ps1 -Notes), dated now. The key is the
// launchers' read-only one (Tools/release-token.txt by default).
'use strict';
const fs = require('fs');
const path = require('path');

const repoRoot = path.join(__dirname, '..');
const args = process.argv.slice(2);
let tokenFile = path.join(__dirname, 'release-token.txt');
let added = null;
for (let i = 0; i < args.length; i++) {
    if (args[i] === '--token-file') tokenFile = args[++i];
    else if (args[i] === '--add') added = { version: args[++i], file: args[++i] };
    else throw new Error('unknown argument ' + args[i]);
}

const repo = 'thehiveprov/reclamation-day-updates';
const token = fs.readFileSync(tokenFile, 'utf8').trim();

async function main() {
    const releases = [];
    for (let page = 1; ; page++) {
        const response = await fetch(`https://api.github.com/repos/${repo}/releases?per_page=100&page=${page}`, {
            headers: { Authorization: 'Bearer ' + token, Accept: 'application/vnd.github+json' },
        });
        if (!response.ok) throw new Error(`GitHub answered ${response.status} for page ${page}`);
        const list = await response.json();
        releases.push(...list);
        if (list.length < 100) break;
    }

    const out = releases.filter((r) => !r.draft && r.published_at)
        .map((r) => ({ version: r.tag_name, date: r.published_at, body: (r.body || '').replace(/\r\n/g, '\n').trim() }));
    if (added) {
        const body = fs.readFileSync(added.file, 'utf8').replace(/\r\n/g, '\n').trim();
        const existing = out.findIndex((r) => r.version === added.version);
        const entry = { version: added.version, date: new Date().toISOString(), body };
        if (existing >= 0) out[existing] = entry;
        else out.push(entry);
    }
    const number = (v) => parseInt((/-(\d+)$/.exec(v) || [0, -1])[1], 10);
    out.sort((a, b) => number(b.version) - number(a.version));

    const file = path.join(repoRoot, 'Code', 'launcher_ui', 'patchnotes.json');
    fs.writeFileSync(file, JSON.stringify(out, null, 1) + '\n');
    console.log(`${file}: ${out.length} releases, newest ${out[0] && out[0].version}`);
}

main().catch((e) => {
    console.error(e.message);
    process.exit(1);
});
