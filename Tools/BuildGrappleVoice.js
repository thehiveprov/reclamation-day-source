// Silent voice files for the grapple quest's wanderer (Tools/GrappleQuest.js), until someone records the lines. The game
// skips a line with no voice file at once, so its subtitle flashes by; a silent file as long as the line takes to read
// keeps it on screen. For each of the wanderer's lines, each of its responses: a silent 16-bit mono WAV, xWMA by the
// game's own encoder (Fallout 4\Tools\Audio\xwmaencode.exe), wrapped as a .fuz with no lip data (FUZE, version 1, lip
// size 0, then the xWMA), as Winter of Atom's trader has them:
//   GameFiles\Fallout4\sound\Voice\RD_GrapplingGun.esp\MaleEvenToned\<line's form ID, plugin index 00>_<response>.fuz
// A recorded line replaces its file under the same name. The player's lines have no files (the game skips them).
// usage: node Tools/BuildGrappleVoice.js [Fallout 4 dir]
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const { ID, LINES } = require('./GrappleQuest.js');

const gameDir = process.argv[2] || 'F:/SteamLibrary/steamapps/common/Fallout 4';
const encoder = path.join(gameDir, 'Tools', 'Audio', 'xwmaencode.exe');
if (!fs.existsSync(encoder)) throw new Error(`no ${encoder}: pass the Fallout 4 folder`);
// the wanderer's traits come from EncWorkshopNPCMaleFarmer02 (0x113343), whose voice type is MaleEvenToned (0x13AD2)
const outDir = path.join(__dirname, '..', 'GameFiles', 'Fallout4', 'sound', 'Voice', 'RD_GrapplingGun.esp', 'MaleEvenToned');
const lines = [
  [ID.infoOffer, LINES.offer], [ID.infoReturn, LINES.returned], [ID.infoWaiting, LINES.waiting], [ID.infoAfter, LINES.after],
  [ID.infoHello, LINES.offer], [ID.infoAcceptReply, LINES.acceptReply], [ID.infoDeclineReply, LINES.declineReply],
  [ID.infoWhoReply, LINES.whoReply], [ID.infoWhatReply, LINES.whatReply],
];

// long enough to read: a second and a half, plus about 2.8 words a second
const seconds = text => 1.5 + text.split(/\s+/).length / 2.8;

function silentWav(secs, rate = 44100) {
  const samples = Math.round(secs * rate), data = samples * 2;
  const b = Buffer.alloc(44 + data);
  b.write('RIFF', 0); b.writeUInt32LE(36 + data, 4); b.write('WAVE', 8);
  b.write('fmt ', 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36); b.writeUInt32LE(data, 40);
  return b;
}

fs.mkdirSync(outDir, { recursive: true });
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'rd-grapple-voice-'));
let written = 0;
for (const [localId, responses] of lines) {
  responses.forEach((text, i) => {
    const name = `${localId.toString(16).toUpperCase().padStart(8, '0')}_${i + 1}`;
    const wav = path.join(work, name + '.wav'), xwm = path.join(work, name + '.xwm');
    fs.writeFileSync(wav, silentWav(seconds(text)));
    execFileSync(encoder, [wav, xwm], { stdio: 'pipe' });
    const audio = fs.readFileSync(xwm);
    const header = Buffer.alloc(12);
    header.write('FUZE', 0); header.writeUInt32LE(1, 4); header.writeUInt32LE(0, 8);
    fs.writeFileSync(path.join(outDir, name + '.fuz'), Buffer.concat([header, audio]));
    written++;
    console.log(`${name}.fuz ${seconds(text).toFixed(1)} s: ${text.slice(0, 60)}`);
  });
}
fs.rmSync(work, { recursive: true, force: true });
console.log(`${written} silent voice files in ${outDir}`);
