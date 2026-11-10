# First person smoothness: largest frame-to-frame acceleration (second difference, units/frame^2) of the visible arm
# joints in the exported clip against the same joints of the Skyrim source, counted only where the joint is in front
# of the camera (more than VISIBLE units ahead - behind the camera it can't be seen). Ours should not exceed the source's.
# Usage: jitter.py <out 1p folder> <skyrim 1p folder> <fo4 1p skeleton> <map json>
import os, sys, glob, json
import numpy as np
import anim_fo4, skyclip
from retarget import Fo4Skeleton, Resampled, SourcePose
from finalize import fo4_worlds

out_dir, src_dir, skel, map_json = sys.argv[1:5]
fo = Fo4Skeleton(skel)
CAMERA = np.array([0.0, 0.0, 120.5])
VISIBLE = 5.0
JOINTS = {'UpperArm': 'UpperArm [{s}Uar]', 'ForeArm1': 'Forearm [{s}Lar]', 'Hand': 'Hand [{s}Hnd]'}
for hkx in sorted(glob.glob(os.path.join(out_dir, '*.hkx'))):
    name = os.path.basename(hkx)[:-4]
    info = json.load(open(hkx[:-4] + '.json'))
    trim = 0.0667 if name.startswith(('Vault', 'Step')) else 0.0
    a = anim_fo4.load_fo4_animation(hkx)
    src = SourcePose(Resampled(skyclip.load(os.path.join(src_dir, f'SkyParkour_{name}_1.hkx')), trim), map_json, True)
    ours, theirs = {}, {}
    for f in range(a.num_frames):
        w = fo4_worlds(fo, a, f)
        sw = src.worlds(f)
        for s in 'LR':
            for fj, sj in JOINTS.items():
                ours.setdefault(f'{s}{fj}', []).append(w[fo.idx[f'{s}Arm_{fj}']][:3, 3])
                theirs.setdefault(f'{s}{fj}', []).append(sw[f'NPC {s} ' + sj.format(s=s)][:3, 3])
    row = []
    worst = 0.0
    for j in ours:
        def acc(p):
            p = np.array(p)
            a = np.linalg.norm(np.diff(p, 2, axis=0), axis=1)
            seen = (p[1:-1, 1] - CAMERA[1]) > VISIBLE
            return float(a[seen].max()) if seen.any() else 0.0
        o, t = acc(ours[j]), acc(theirs[j])
        worst = max(worst, o - t)
        row.append(f'{j} {o:4.1f}/{t:4.1f}')
    print(name.ljust(14), 'worst excess', round(worst, 1), '|', ' '.join(row))
