// Shadow settings, one at a time, while the player stands still: writes RD's MCM settings file (re-read every 3 s by
// SunShadowCache's RefreshFeatures), lets one 30 s report pass, takes the next, and a screenshot of the game's window.
// Never touches input. Usage: node cycle.js <label> [steps: all | presets | features | local]
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const ini = 'F:/SteamLibrary/steamapps/common/Fallout 4/Data/MCM/Settings/ReclamationDay.ini';
const log = 'G:/RECLAMATION DAY TEST BIKE/Reclamation Day/logs/tp_client.log';
const label = process.argv[2] || 'spot';
const which = process.argv[3] || 'all';
const out = path.join(__dirname, label);
fs.mkdirSync(out, { recursive: true });
const base = { iSunShadowCache: 2, iSmallCasterTexels: 0, bStaticDynamicShadows: 0, bOcclusionCull: 0, bGpuShadows: 0, bParallelSunGather: 0, iLocalLightCache: 0 };
const presets = [['off', { iSunShadowCache: 0 }], ['quality', { iSunShadowCache: 1 }], ['balanced', {}], ['performance', { iSunShadowCache: 3 }]];
// the riskiest last: the gathering on other threads has never run
const features = [['small-casters-2', { iSmallCasterTexels: 2 }], ['static-dynamic', { bStaticDynamicShadows: 1 }], ['occlusion', { bOcclusionCull: 1 }],
  ['gpu-shadows', { bGpuShadows: 1 }], ['parallel-gather', { bParallelSunGather: 1 }]];
const local = [['balanced', {}], ['local-light-8', { iLocalLightCache: 8 }]];
const occlusion = [['balanced', {}], ['occlusion', { bOcclusionCull: 1 }], ['balanced-again', {}]];
const steps = which === 'presets' ? presets : which === 'features' ? [['balanced', {}], ...features] : which === 'local' ? local : which === 'occlusion' ? occlusion : [...presets, ...features];

const before = fs.existsSync(ini) ? fs.readFileSync(ini, 'latin1') : null;
const kept = /\[Performance\]([^\[]*)/i.exec(before || '');
if (kept) for (const line of kept[1].split(/\r?\n/)) { const m = /^\s*(\w+)\s*=\s*(\d+)/.exec(line); if (m) base[m[1]] = Number(m[2]); }
if (kept) console.log('base from the file: ' + JSON.stringify(base));
function write(values) {
  // only [Performance] is ours; anything else in the file stays as it was
  let text = before || '';
  const body = Object.entries(values).map(([k, v]) => `${k}=${v}`).join('\r\n');
  const re = /\[Performance\][^\[]*/i;
  text = re.test(text) ? text.replace(re, `[Performance]\r\n${body}\r\n`) : `${text}${text && !text.endsWith('\n') ? '\r\n' : ''}[Performance]\r\n${body}\r\n`;
  fs.mkdirSync(path.dirname(ini), { recursive: true });
  fs.writeFileSync(ini, text, 'latin1');
}
function restore() {
  if (before === null) { if (fs.existsSync(ini)) fs.unlinkSync(ini); } else fs.writeFileSync(ini, before, 'latin1');
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const logSize = () => fs.statSync(log).size;
function linesSince(offset) {
  const fd = fs.openSync(log, 'r');
  const len = logSize() - offset;
  const buf = Buffer.alloc(Math.max(0, len));
  fs.readSync(fd, buf, 0, buf.length, offset);
  fs.closeSync(fd);
  return buf.toString('latin1').split('\n');
}
const ours = /Shadows, ms per frame|Visibility query|Small shadow casters|Static and dynamic|Occlusion culling|GPU-driven shadows|Parallel sun shadow|Local light shadow|Sun shadow|error|crash/i;
const reportCount = (lines) => lines.filter((l) => l.includes('Shadows, ms per frame')).length;
const running = () => /ReclamationDay\.exe/i.test(execFileSync('tasklist', ['/fo', 'csv', '/nh'], { encoding: 'utf8' }));

process.on('SIGINT', () => { restore(); console.log('stopped: settings put back'); process.exit(1); });
(async () => {
  const summary = [];
  try {
    for (const [name, change] of steps) {
      if (!running()) { console.log('the game is gone: stopping'); break; }
      const values = { ...base, ...change };
      const start = logSize();
      write(values);
      console.log(`${new Date().toTimeString().slice(0, 8)} ${name}: ${JSON.stringify(change)}`);
      // the change is picked up within 3 s; then the report already under way (mixed) passes, and the next is the one
      let shot = false;
      for (let t = 0; t < 120; ++t) {
        await sleep(1000);
        const lines = linesSince(start);
        const n = reportCount(lines);
        if (n >= 1 && !shot) {
          shot = true;
          try { console.log('  shot ' + execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(__dirname, 'grab.ps1'), path.join(out, `${name}.png`)], { encoding: 'utf8' }).trim()); } catch (e) { console.log('  no shot: ' + e.message.split('\n')[0]); }
        }
        if (n >= 2) break;
      }
      const lines = linesSince(start).filter((l) => ours.test(l));
      const reports = lines.filter((l) => l.includes('Shadows, ms per frame'));
      const last = reports[reports.length - 1] || '(no report)';
      for (const l of lines.filter((l) => !l.includes('Shadows, ms per frame') && !l.includes('Visibility query'))) console.log('  ' + l.trim().slice(0, 260));
      console.log('  ' + last.trim().slice(0, 330));
      summary.push(`${name}\t${last.replace(/^.*\] /, '').trim()}`);
    }
  } finally {
    restore();
    fs.writeFileSync(path.join(out, 'summary.txt'), summary.join('\n') + '\n');
    console.log(`settings put back (${before === null ? 'file removed: it was not there' : 'as they were'}); summary in ${path.join(out, 'summary.txt')}`);
  }
})();
