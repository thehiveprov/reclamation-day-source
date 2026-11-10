# The first person version of an emote: one of the game's third person clips (Fallout4 - Animations.ba2) turned into a
# clip for Fallout 4's first person skeleton with the hands brought into the view, written as a Fallout 4 hkx (94
# tracks in skeleton order, like vanilla first person clips). Both skeletons have the same bones and rest pose (but
# CamTarget, third person only), so the body's motion is kept as it is; what changes is where it is against the view.
#
# A first person clip is drawn against its Camera bone, which stays at rest (0, 0, 120.5) under Root: the game puts
# that bone where the player's view is. The third person clips leave their Camera at rest too, but the body moves under
# it (a sit takes the head down 60 units), and their gestures happen where a body's arms are: a clap 22 units under the
# eyes and 16 in front of them, 54 degrees under the line of sight (docs/EMOTES.md). So, per frame:
#   1. The body is seen from its own eyes: moved so its head is where the camera is (its rest offset kept), turned as
#      ever (a nod of the head doesn't swing the arms through the view).
#   2. And moved so its shoulders are where the game's first person arms have theirs (the unarmed ready pose,
#      _1stPerson\Animations\H2H\Equip.hkx, last frame: 12.3 under the eyes and 1.5 behind them), the body's
#      shoulder motion kept.
#   3. A hand out in front of the body is lifted into the view: its angle under the line of sight kept down to
#      PHI_KEEP, past that only K of it, its distance kept and at least FWD_MIN in front; a hand at the side (less
#      than F0 in front) isn't moved, between F0 and F1 partly; its offset to the side is cut to XK of it. The hand
#      turns up by what it was lifted.
#   4. The arm reaches it: two-bone IK on the upper arm and forearm, the elbow bending the way the body's elbow does;
#      the hand keeps its (lifted) turn, the fingers their motion.
# Usage: first_person.py <third person clip.hkx> <third person skeleton.hkx> <first person skeleton.hkx> <out.hkx>
import os, sys
import numpy as np
import anim_fo4, skymap
from retarget import Fo4Skeleton, mat, rot_between, rot_x

FWD_MIN = 16.0  # a lifted hand is at least this far in front of the eyes
F0, F1 = 2.0, 12.0  # how far in front of the eyes a hand starts being lifted, and is lifted fully
PHI_KEEP = np.radians(10.0)  # down to this angle under the line of sight a hand stays where it is
K = 0.3  # past PHI_KEEP, how much of the angle is kept
XK = 0.75  # how much of a lifted hand's offset to the side is kept (a salute's hand at the brow is at the view's edge)
REF_SHOULDERS = np.array([0.0, -12.3, 1.5])  # camera space (x right, y up, -z forward), H2H\Equip.hkx's last frame
N_TRACKS = 94


def worlds(skel, local):
    out = []
    for i in range(len(skel.names)):
        p = skel.parent[i]
        out.append(local[i] if p < 0 else out[p] @ local[i])
    return out


def smoothstep(x):
    x = min(1.0, max(0.0, x))
    return x * x * (3 - 2 * x)


class Converter:
    def __init__(self, third_skel, first_skel):
        self.s3, self.s1 = third_skel, first_skel
        self.cam = self.s3.rest_world[self.s3.idx['Camera']]
        self.cam_rot = self.cam[:3, :3]
        self.inv_cam = np.linalg.inv(self.cam)
        self.eye_from_head = self.cam[:3, 3] - self.s3.rest_world[self.s3.idx['Head']][:3, 3]
        rest_mid = np.mean([self.s3.rest_world[self.s3.idx[f'{s}Arm_UpperArm']][:3, 3] for s in 'LR'], axis=0)
        self.shoulder_shift = self.cam_rot @ (REF_SHOULDERS - (self.inv_cam @ np.append(rest_mid, 1))[:3])

    def to_cam(self, p):
        return (self.inv_cam @ np.append(p, 1))[:3]

    def from_cam(self, c):
        return (self.cam @ np.append(c, 1))[:3]

    def lift(self, hand):
        """the hand's place lifted into the view (world), and the angle it was lifted by"""
        c = self.to_cam(hand)
        fwd, down = -c[2], -c[1]
        w = smoothstep((fwd - F0) / (F1 - F0))
        if w <= 0.0:
            return hand, 0.0
        r = np.hypot(fwd, down)
        phi = np.arctan2(down, max(fwd, 1e-3))
        new_phi = phi if phi <= PHI_KEEP else PHI_KEEP + (phi - PHI_KEEP) * K
        n = np.array([c[0] * XK, -r * np.sin(new_phi), -r * np.cos(new_phi)])
        if -n[2] < FWD_MIN:
            n[2] = -FWD_MIN
        lifted = c + w * (n - c)
        return self.from_cam(lifted), w * (phi - new_phi)

    def frame(self, src_local):
        """the first person locals for one frame of third person locals (by bone name)"""
        s3, s1 = self.s3, self.s1
        w3 = worlds(s3, src_local)
        head = w3[s3.idx['Head']][:3, 3]
        shift = (self.cam[:3, 3] - (head + self.eye_from_head)) + self.shoulder_shift
        local = []
        for i, name in enumerate(s1.names):
            j = s3.idx[name]
            L = src_local[j].copy()
            if name == 'Root' or name == 'Camera':
                L = s1.rest_local[i].copy()
            elif name == 'COM':
                L = w3[j].copy()
                L[:3, 3] += shift
            local.append(L)
        w1 = worlds(s1, local)
        reach = {}
        for s in 'LR':
            iu, ie, ih = (s1.idx[f'{s}Arm_{b}'] for b in ('UpperArm', 'ForeArm1', 'Hand'))
            target, angle = self.lift(w1[ih][:3, 3])
            if angle == 0.0 and np.allclose(target, w1[ih][:3, 3]):
                continue
            S, E, H = w1[iu][:3, 3], w1[ie][:3, 3], w1[ih][:3, 3]
            a, b = np.linalg.norm(E - S), np.linalg.norm(H - E)
            d = np.clip(np.linalg.norm(target - S), abs(a - b) + 1e-3, a + b - 1e-3)
            u = (target - S) / np.linalg.norm(target - S)
            pole = (E - S) - np.dot(E - S, u) * u
            if np.linalg.norm(pole) < 1e-6:
                pole = self.cam_rot @ np.array([0.0, -1.0, 0.0])
                pole = pole - np.dot(pole, u) * u
            v = pole / np.linalg.norm(pole)
            alpha = np.arccos(np.clip((a * a + d * d - b * b) / (2 * a * d), -1, 1))
            E2 = S + a * (np.cos(alpha) * u + np.sin(alpha) * v)
            R1 = rot_between(E - S, E2 - S)
            R2 = rot_between(R1 @ (H - E), S + d * u - E2)
            lift = self.cam_rot @ rot_x(angle) @ self.cam_rot.T
            reach[iu] = R1 @ w1[iu][:3, :3]
            reach[ie] = R2 @ R1 @ w1[ie][:3, :3]
            reach[ih] = lift @ w1[ih][:3, :3]
        if reach:
            world = [None] * len(s1.names)
            for i in range(len(s1.names)):
                p = s1.parent[i]
                pw = np.eye(4) if p < 0 else world[p]
                if i in reach:
                    local[i][:3, :3] = pw[:3, :3].T @ reach[i]
                world[i] = pw @ local[i]
        return local


def convert(clip_path, third_skel_path, first_skel_path, out_path):
    s3, s1 = Fo4Skeleton(third_skel_path), Fo4Skeleton(first_skel_path)
    assert all(n in s3.idx for n in s1.names), 'a first person bone the third person skeleton lacks'
    conv = Converter(s3, s1)
    clip = anim_fo4.load_fo4_animation(clip_path)
    out = anim_fo4.AnimationData()
    out.duration = clip.duration
    out.num_frames = clip.num_frames
    out.num_tracks = N_TRACKS
    out.frame_duration = clip.frame_duration
    out.original_skeleton_name = 'Root'
    out.track_to_bone_indices = list(range(N_TRACKS))
    out.blend_hint = 0
    out.tracks = [anim_fo4.TrackData() for _ in range(N_TRACKS)]
    assert len(s1.names) == N_TRACKS
    for fr in range(clip.num_frames):
        src = [l.copy() for l in s3.rest_local]
        for t, b in enumerate(clip.track_to_bone_indices):
            tr = clip.tracks[t]
            src[b] = mat(tr.rotations[fr], tr.translations[fr])
        local = conv.frame(src)
        for i in range(N_TRACKS):
            q = skymap.q_from_m(local[i][:3, :3])
            if fr and np.dot(q, out.tracks[i].rotations[-1]) < 0:
                q = -q
            out.tracks[i].rotations.append([float(v) for v in q])
            out.tracks[i].translations.append([float(v) for v in local[i][:3, 3]])
            out.tracks[i].scales.append([1.0, 1.0, 1.0])
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    anim_fo4.write_fo4_animation(out_path, out)
    return out


if __name__ == '__main__':
    o = convert(*sys.argv[1:5])
    print(os.path.basename(sys.argv[4]), o.num_frames, 'frames', round(o.duration, 3), 's')
