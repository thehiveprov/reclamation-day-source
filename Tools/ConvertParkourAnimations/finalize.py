# The data the client needs next to an exported clip, as JSON:
#   duration  - seconds (the hkx's)
#   curve     - per 30 fps frame of the hkx: travel x/y/z from the clip start (Fallout 4 units; x right, y forward,
#               z up in the character's frame) - the Skyrim clip's animmotion curve after speed and trim
#   landing   - seconds: first frame after the feet's highest point where the lower foot is back within 2 units of
#               where it ends (where a vault can hand back to running)
#   settled   - seconds: first frame from which the travel stays within 0.5 units of where it ends
#   events    - [seconds, text] the Skyrim clip's annotations (SoundPlay.*, SkyParkour_Recovery/Stop) after speed/trim,
#               within the clip
#   eye       - first person, per frame: where the clip's camera is against where it rests (x right, y forward, z up)
#               and how it's turned (pitch down, yaw right; radians) - the view motion the game gives the player
# Usage: finalize.py <exported hkx> <skyrim hkx> <fo4 skeleton.hkx> <out json> [--trim s] [--first-person]
import argparse, json
import numpy as np
import anim_fo4, skyclip
from retarget import Fo4Skeleton, Resampled, mat, SPEED, FPS, FP_CAMERA_REST, FP_CAMERA_TRACK


def motion_at(keys, t):
    """animmotion keys [(t, x, y, z)] at time t (linear; before the first key, from 0)"""
    if not keys:
        return np.zeros(3)
    if t <= keys[0][0]:
        return np.array(keys[0][1:]) * (t / keys[0][0] if keys[0][0] > 0 else 1)
    for a, b in zip(keys, keys[1:]):
        if a[0] <= t <= b[0]:
            u = (t - a[0]) / max(1e-6, b[0] - a[0])
            return np.array(a[1:]) * (1 - u) + np.array(b[1:]) * u
    return np.array(keys[-1][1:])


def fo4_worlds(fo, anim, frame):
    world = []
    for i in range(len(fo.names)):
        L = mat(anim.tracks[i].rotations[frame], anim.tracks[i].translations[frame]) if i < anim.num_tracks else fo.rest_local[i]
        world.append(L if fo.parent[i] < 0 else world[fo.parent[i]] @ L)
    return world


def finalize(hkx, src_clip_path, skel_path, out_json, trim=0.0, first_person=False):
    fo = Fo4Skeleton(skel_path)
    a = anim_fo4.load_fo4_animation(hkx)
    src = skyclip.load(src_clip_path)
    keys = [[t / SPEED, x, y, z] for t, x, y, z in skyclip.root_motion(src)]
    m0 = motion_at(keys, trim)
    # Skyrim and Fallout 4 humans are the same height (COM 68.9 both), the curve keeps its units
    curve = [(motion_at(keys, trim + f / FPS) - m0).round(3).tolist() for f in range(a.num_frames)]
    # first person: the clip's camera per frame; the exported body is as that camera sees it (retarget.py), so the
    # feet are put back where the body really is
    resampled = Resampled(src, trim) if first_person else None
    cameras = [resampled.local(FP_CAMERA_TRACK, f) for f in range(a.num_frames)] if first_person else None
    back = [c @ np.linalg.inv(FP_CAMERA_REST) for c in cameras] if first_person else None
    feet = []
    for f in range(a.num_frames):
        w = fo4_worlds(fo, a, f)
        if first_person:
            w = [back[f] @ m for m in w]
        feet.append(min(w[fo.idx['LLeg_Foot']][2, 3], w[fo.idx['RLeg_Foot']][2, 3]) + curve[f][2])
    top = int(np.argmax(feet))
    landing = next((f for f in range(top, a.num_frames) if feet[f] <= feet[-1] + 2), a.num_frames - 1)
    end = np.array(curve[-1])
    settled = next(f for f in range(a.num_frames) if all(np.abs(np.array(c) - end).max() <= 0.5 for c in curve[f:]))
    events = sorted([round(e.time / SPEED - trim, 4), e.text] for e in src.annotations
                    if not e.text.startswith('animmotion') and trim <= e.time / SPEED <= trim + a.duration + 1e-3)
    info = {'duration': round(a.duration, 4), 'curve': curve, 'landing': round(landing / FPS, 4),
            'settled': round(settled / FPS, 4), 'events': events}
    if first_person:
        eye = []
        for c in cameras:
            ahead = c[:3, :3] @ np.array([0.0, 1.0, 0.0])
            offset = c[:3, 3] - FP_CAMERA_REST[:3, 3]
            pitch = np.arctan2(-ahead[2], np.hypot(ahead[0], ahead[1]))
            yaw = np.arctan2(ahead[0], ahead[1])
            eye.append([round(float(v), 3) for v in offset] + [round(float(pitch), 4), round(float(yaw), 4)])
        info['eye'] = eye
    json.dump(info, open(out_json, 'w'), indent=1)
    return info


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('hkx')
    ap.add_argument('src')
    ap.add_argument('skeleton')
    ap.add_argument('out_json')
    ap.add_argument('--trim', type=float, default=0.0)
    ap.add_argument('--first-person', action='store_true')
    a = ap.parse_args()
    i = finalize(a.hkx, a.src, a.skeleton, a.out_json, a.trim, a.first_person)
    print(a.hkx.replace('\\', '/').split('/')[-1], 'frames', len(i['curve']), 'landing', i['landing'], 'of', i['duration'], 'end', i['curve'][-1])
