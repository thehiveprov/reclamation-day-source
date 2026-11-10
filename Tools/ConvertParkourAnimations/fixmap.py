# One mapping for all third-person clips (they share the vanilla 99-bone order), derived from ClimbHigh; checks
# each clip's mapped tracks keep the bone offsets (translation within 1.5, COM excluded).
import os, sys, glob, json
import numpy as np
import skyclip, skymap
d = sys.argv[1]
skel = skymap.SkyrimSkeleton()
ref = skyclip.load(os.path.join(d, 'SkyParkour_ClimbHigh.hkx'))
tb, par, _ = skymap.map_tracks(ref, skel)
json.dump({'track_bone': tb, 'parent': par}, open(sys.argv[2], 'w'), indent=1)
refT = {i: np.mean(ref.tracks[i].translations, axis=0) for i in tb}
for f in sorted(glob.glob(os.path.join(d, '*.hkx'))):
    a = skyclip.load(f)
    bad = [(tb[i], round(float(np.linalg.norm(np.mean(a.tracks[i].translations, axis=0) - refT[i])), 1)) for i in tb
           if tb[i] not in ('NPC COM [COM ]', 'NPC Root [Root]') and np.linalg.norm(np.mean(a.tracks[i].translations, axis=0) - refT[i]) > 1.5]
    print(os.path.basename(f).ljust(30), a.num_tracks, 'tracks, offsets differ:', bad or 'none')
