// Writes GameFiles/Fallout4/ReclamationDay/BundledMods.txt: the files of the mods bundled into Reclamation Day (each in its
// own folder in Data layout), which the launcher keeps in the game's Data folder at every start and whose plugins it
// switches on (Updater::InstallOurPlugins). Run it whenever a bundled mod's files change; the packager and the update
// publisher copy the same folders into Game Files. After 1.6.7-285 that's only the nature overhaul: the mods by other
// authors Reclamation Day needs are downloads, which the launcher's window and the main menu list.
// usage: node Tools/ListBundledMods.js [bundled folder, F:\modfiles\bundled]
const fs = require('fs');
const path = require('path');

const root = process.argv[2] || 'F:\\modfiles\\bundled';
const out = path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'ReclamationDay', 'BundledMods.txt');
// Folder, and what it is (for the list's readers)
const MODS = [
  ['RDNature', 'Boston Natural Surroundings by Silverfox600 (Nexus 30673), 3.0, remade as RD_Landscape.esp and RD_Trees.esp ' +
               '(F:\\modfiles\\RDNature): the 2287 Commonwealth\'s grass, ground and trees, off in Winter of Atom'],
];

const list = folder => {
  const files = [];
  const walk = dir => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory())
        walk(full);
      else
        files.push(path.relative(folder, full).split(path.sep).join('\\'));
    }
  };
  walk(folder);
  return files.sort((a, b) => a.toLowerCase().localeCompare(b.toLowerCase()));
};

const lines = ['# The bundled mods\' files, relative to Game Files (written by Tools/ListBundledMods.js). The launcher keeps them in',
               '# the game\'s Data folder and switches on the plugins among them; "[folder]" starts a mod.'];
const seen = new Map();
for (const [folder, what] of MODS) {
  const files = list(path.join(root, folder));
  lines.push('', `# ${what}`, `[${folder}]`);
  for (const file of files) {
    const key = file.toLowerCase();
    if (seen.has(key))
      throw new Error(`${file} is in both ${seen.get(key)} and ${folder}`);
    seen.set(key, folder);
    lines.push(file);
  }
}
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, lines.join('\r\n') + '\r\n');
console.log(`${out}: ${seen.size} files from ${MODS.length} mods`);
