# How vanilla moves the rib helpers: rib rotation (about its y, the axis it moves on) and translation along x against
# the upper arm's raise (angle between the upper arm and straight down, in chest space) - median and spread per 10
# degree bin, over every vanilla clip given, and which clips have rib values far outside the usual. Writes
# rib_table.json: per side, [raise deg (bin centre), rib y rad, translation x] from the medians, bins with >= 20
# samples, made monotonic (a higher arm never bends the rib less), for retarget3p.
import os, sys, glob, json, collections
import numpy as np
import anim_fo4
import skymap as sm
from retarget import Fo4Skeleton, mat

fo = Fo4Skeleton(sys.argv[1])
I = fo.idx
bins = {s: collections.defaultdict(list) for s in 'LR'}
odd = collections.Counter()
partial = set()
for p in glob.glob(os.path.join(sys.argv[2], '**', '*.hkx'), recursive=True):
    a = anim_fo4.load_fo4_animation(p)
    if a.num_tracks < 94:
        continue
    for f in range(0, a.num_frames, 2):
        w = []
        for i in range(94):
            L = mat(a.tracks[i].rotations[f], a.tracks[i].translations[f])
            w.append(L if fo.parent[i] < 0 else w[fo.parent[i]] @ L)
        chest = w[I['Chest']]
        for s in 'LR':
            d = chest[:3, :3].T @ (w[I[f'{s}Arm_ForeArm1']][:3, 3] - w[I[f'{s}Arm_UpperArm']][:3, 3])
            if np.linalg.norm(d) < 5:  # additive / partial clips: the arm isn't a full pose
                partial.add(os.path.relpath(p, sys.argv[2]))
                continue
            d /= np.linalg.norm(d)
            # chest space: which axis is "down the body"? the rest pose arm is horizontal, spine runs along chest x
            raise_deg = np.degrees(np.arccos(np.clip(np.dot(d, [-1, 0, 0]), -1, 1)))
            rib = I[f'{s}_RibHelper']
            q = sm.q_from_m(fo.rest_local[rib][:3, :3].T @ sm.m_from_q(np.array(a.tracks[rib].rotations[f])))
            if q[3] < 0:
                q = -q
            ry = np.degrees(2 * np.arctan2(q[1], q[3]))
            tx = a.tracks[rib].translations[f][0] - fo.rest_local[rib][0, 3]
            bins[s][int(raise_deg // 10) * 10].append((ry, tx))
            if abs(ry) > 35:
                odd[os.path.relpath(p, sys.argv[2])] += 1
for s in 'LR':
    print(s, 'raise -> rib y deg median [p10 p90] | tx median, n')
    for b in sorted(bins[s]):
        v = np.array(bins[s][b])
        print(f'  {b:4d}  {np.median(v[:, 0]):6.1f} [{np.percentile(v[:, 0], 10):6.1f} {np.percentile(v[:, 0], 90):6.1f}]  tx {np.median(v[:, 1]):5.2f}  n {len(v)}')
print('clips with |rib| > 35 deg:', odd.most_common(12))
print(len(partial), 'partial clips skipped, e.g.', sorted(partial)[:6])

table = {}
for s in 'LR':
    rows, best = [], None
    for b in sorted(bins[s]):
        v = np.array(bins[s][b])
        if len(v) < 20:
            continue
        ry, tx = float(np.median(v[:, 0])), float(np.median(v[:, 1]))
        if best is not None and abs(ry) < abs(best[0]):
            ry, tx = best
        best = (ry, tx)
        rows.append([b + 5, round(np.radians(ry), 5), round(tx, 4)])
    table[s] = rows
json.dump(table, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rib_table.json'), 'w'), indent=1)
print('rib_table.json', {k: len(v) for k, v in table.items()})
