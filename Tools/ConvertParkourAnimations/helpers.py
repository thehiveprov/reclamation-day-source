# Twist of the skin helpers and the hands in exported clips (max degrees per clip) - compare with vanilla
import os, sys, glob
import numpy as np
import anim_fo4, skymap as sm
from retarget import Fo4Skeleton, twist_x

fo = Fo4Skeleton(sys.argv[2])
I = fo.idx


def rel(n, q):
    return fo.rest_local[I[n]][:3, :3].T @ sm.m_from_q(np.array(q))


for p in sorted(glob.glob(os.path.join(sys.argv[1], '*.hkx'))):
    a = anim_fo4.load_fo4_animation(p)
    r = {}
    for n in ['LArm_ForeArm2', 'RArm_ForeArm2', 'LArm_Hand', 'RArm_Hand', 'LArm_UpperTwist1', 'RArm_UpperTwist1']:
        r[n.replace('Arm_', '')] = round(max(abs(np.degrees(twist_x(rel(n, q)))) for q in a.tracks[I[n]].rotations))
    for s in 'LR':
        n = s + '_RibHelper'
        r[n] = round(max(np.degrees(2 * np.arccos(min(1, abs(sm.q_from_m(rel(n, q))[3])))) for q in a.tracks[I[n]].rotations))
    print(os.path.basename(p).ljust(18), r)
