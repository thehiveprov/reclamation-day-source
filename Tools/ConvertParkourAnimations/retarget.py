# Retargets a parkour clip (Skyrim LE hkx) onto Fallout 4's skeleton - third person or first person (both Fallout 4
# skeletons have the same bones and rest pose) - and writes a Fallout 4 hkx: 94 tracks in skeleton order, like
# vanilla clips. Clips play in place; the travel is the clip's animmotion curve (finalize.py writes it per frame).
#
# Rotations: each mapped bone's world rotation change from the Skyrim rest pose is applied to the Fallout 4 rest
# pose, after that has been turned so the bone points where the Skyrim rest bone points (bone -> child direction).
# Skin helpers follow vanilla Fallout 4 (measured over vanilla clips): ForeArm1 only bends, the forearm's whole twist
# is shared a third each by ForeArm2, ForeArm3 and the hand; UpperTwist1 undoes the upper arm's twist and UpperTwist2
# gives half of it back; the rib helpers bend with the arm's raise (rib_table.json, from rib_bins.py).
#
# First person: Skyrim's first person body hangs from a static chain (tracks 1 -> 2 -> 3 above COM), and the view is
# track 97, a camera bone the clips animate (it dips and looks down at the hands as they reach the ledge, up at a
# high one). Fallout 4's first person skeleton stays under its own camera bone, which no clip moves (the game draws
# the arms against it). So the Skyrim body is taken as seen from its moving camera and put under Fallout 4's still
# one: the arms are where the Skyrim player sees them. The camera's own motion is the view motion, written out for
# the game to turn the view by (finalize.py). After retargeting each hand is put exactly where the Skyrim hand is
# (two-bone IK on the upper arm and forearm, the elbow bending the way the Skyrim elbow does), the hand keeps its
# retargeted rotation.
import os, sys, json
import numpy as np
import skyclip, skymap
import anim_fo4

HERE = os.path.dirname(os.path.abspath(__file__))
FPS = 30.0
SPEED = 1.15  # the playback speed the original mod runs these clips at
N_TRACKS = 94  # vanilla clips animate every bone but CamTarget
REACH_MARGIN = 1.0  # first person: arms are never asked to lock fully straight (IK snaps there)
POLE_BLEND = 2.0  # first person: length of the retargeted arm's own bend added to the Skyrim elbow's (units)
SLIDE_WINDOW = 3  # first person: frames either side whose reach slide a frame also covers
SLIDE_SIGMA = 2  # first person: gaussian smoothing of the reach slide, frames

# (Skyrim bone, Fallout 4 bone, Fallout 4 child, Skyrim child) - children give the bone's direction
SPINE = [('NPC COM [COM ]', 'COM', None, None), ('NPC Pelvis [Pelv]', 'Pelvis', None, None),
         ('NPC Spine [Spn0]', 'Spine1', 'Spine2', 'NPC Spine1 [Spn1]'),
         ('NPC Spine1 [Spn1]', 'Spine2', 'Chest', 'NPC Spine2 [Spn2]'),
         ('NPC Spine2 [Spn2]', 'Chest', 'Neck', 'NPC Neck [Neck]'),
         ('NPC Neck [Neck]', 'Neck', 'Head', 'NPC Head [Head]'), ('NPC Head [Head]', 'Head', None, None)]


def side(s):
    S = f'NPC {s} '
    m = [(S + f'Thigh [{s}Thg]', s + 'Leg_Thigh', s + 'Leg_Calf', S + f'Calf [{s}Clf]'),
         (S + f'Calf [{s}Clf]', s + 'Leg_Calf', s + 'Leg_Foot', S + f'Foot [{s}ft ]'),
         (S + f'Foot [{s}ft ]', s + 'Leg_Foot', s + 'Leg_Toe1', S + f'Toe0 [{s}Toe]'),
         (S + f'Toe0 [{s}Toe]', s + 'Leg_Toe1', None, None),
         (S + f'Clavicle [{s}Clv]', s + 'Arm_Collarbone', s + 'Arm_UpperArm', S + f'UpperArm [{s}Uar]'),
         (S + f'UpperArm [{s}Uar]', s + 'Arm_UpperArm', s + 'Arm_ForeArm1', S + f'Forearm [{s}Lar]'),
         (S + f'Forearm [{s}Lar]', s + 'Arm_ForeArm1', s + 'Arm_Hand', S + f'Hand [{s}Hnd]'),
         (S + f'Hand [{s}Hnd]', s + 'Arm_Hand', s + 'Arm_Finger31', S + f'Finger20 [{s}F20]')]
    # Skyrim Finger0x (thumb) .. Finger4x (little) -> Fallout 4 Finger1x .. Finger5x, joints 0-2 -> 1-3
    for k in range(5):
        for j in range(3):
            src, dst = S + f'Finger{k}{j} [{s}F{k}{j}]', s + f'Arm_Finger{k + 1}{j + 1}'
            nxt = (s + f'Arm_Finger{k + 1}{j + 2}', S + f'Finger{k}{j + 1} [{s}F{k}{j + 1}]') if j < 2 else (None, None)
            m.append((src, dst) + nxt)
    return m


BONES = SPINE + side('L') + side('R')

RIB_TABLE = os.path.join(HERE, 'rib_table.json')
RIB = {s: np.array(v) for s, v in json.load(open(RIB_TABLE)).items()} if os.path.exists(RIB_TABLE) else None


# ---------------------------------------------------------------------------------------------------- maths

def mat(q, t=(0, 0, 0)):
    m = np.eye(4)
    m[:3, :3] = skymap.m_from_q(np.asarray(q, float))
    m[:3, 3] = t
    return m


def rot_between(a, b):
    """smallest rotation turning direction a onto direction b"""
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if c < -0.9999:
        ax = np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0])
        ax /= np.linalg.norm(ax)
        return 2 * np.outer(ax, ax) - np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx / (1 + c)


def rot_x(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_y(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def twist_x(m):
    """angle of m's twist about its own x axis (swing-twist split), in (-pi, pi]"""
    q = skymap.q_from_m(m)
    if q[3] < 0:
        q = -q
    return 2 * np.arctan2(q[0], q[3])


def slerp(q0, q1, u):
    q0, q1 = np.asarray(q0, float), np.asarray(q1, float)
    if np.dot(q0, q1) < 0:
        q1 = -q1
    d = float(np.clip(np.dot(q0, q1), -1, 1))
    if d > 0.9995:
        q = q0 + u * (q1 - q0)
        return q / np.linalg.norm(q)
    th = np.arccos(d)
    return (np.sin((1 - u) * th) * q0 + np.sin(u * th) * q1) / np.sin(th)


def smooth(vectors, sigma):
    """gaussian over a sequence of vectors (ends held)"""
    n = len(vectors)
    kernel = np.exp(-0.5 * (np.arange(-3 * sigma, 3 * sigma + 1) / sigma) ** 2)
    out = []
    for k in range(n):
        idx = np.clip(np.arange(k - 3 * sigma, k + 3 * sigma + 1), 0, n - 1)
        out.append((kernel[:, None] * np.array([vectors[i] for i in idx])).sum(0) / kernel.sum())
    return out


def rib_pose(side_, raise_deg):
    t = RIB[side_]
    return np.interp(raise_deg, t[:, 0], t[:, 1]), np.interp(raise_deg, t[:, 0], t[:, 2])


# ---------------------------------------------------------------------------------------------------- skeletons

class Fo4Skeleton:
    def __init__(self, path):
        s = anim_fo4.load_fo4_skeleton(path)
        self.names = s.bones
        self.idx = {n: i for i, n in enumerate(s.bones)}
        self.parent = s.parents
        self.rest_local = [mat(p.rotation, p.translation) for p in s.reference_pose]
        self.rest_world = []
        for i in range(len(self.names)):
            p = self.parent[i]
            self.rest_world.append(self.rest_local[i] if p < 0 else self.rest_world[p] @ self.rest_local[i])


class Resampled:
    """a Skyrim clip resampled at FPS after the playback speed, the first `trim` seconds left out, cut after `end`
    seconds (of the result) when given"""

    def __init__(self, clip, trim=0.0, end=None):
        self.duration = clip.duration / SPEED - trim
        if end is not None:
            self.duration = min(self.duration, end)
        self.num_frames = int(round(self.duration * FPS)) + 1
        self.num_tracks = clip.num_tracks
        self.rotations, self.translations = [], []
        for tr in clip.tracks:
            rs, ts = [], []
            for k in range(self.num_frames):
                f = min(clip.duration, (k / FPS + trim) * SPEED) / clip.frame_duration
                a = max(0, min(clip.num_frames - 1, int(np.floor(f))))
                b = min(clip.num_frames - 1, a + 1)
                u = float(f - np.floor(f)) if a != b else 0.0
                rs.append(slerp(tr.rotations[a], tr.rotations[b], u))
                ts.append((1 - u) * np.asarray(tr.translations[a]) + u * np.asarray(tr.translations[b]))
            self.rotations.append(rs)
            self.translations.append(ts)

    def local(self, track, frame):
        return mat(self.rotations[track][frame], self.translations[track][frame])


FP_CAMERA_TRACK = 97  # first person clips: the camera bone (under Root, at rest where Fallout 4's camera is)
FP_CAMERA_REST = mat((0, 0, 0, 1), (0, 0, 120.5))


class SourcePose:
    """Skyrim world transforms of the mapped bones per frame. Third person: the body from COM up, Root and its
    helper tracks left out (the travel is the animmotion curve). First person: COM hangs from the camera chain, and
    the whole body is as the clip's camera sees it, put in front of a camera that stays at rest."""

    def __init__(self, clip, map_json, first_person):
        m = json.load(open(map_json))
        self.tmap = {int(k): v for k, v in m['track_bone'].items()}
        self.parent = {int(k): v for k, v in m['parent'].items()}
        self.clip = clip
        self.first_person = first_person

    def camera(self, frame):
        """first person: the clip's camera bone (under Root) at this frame"""
        return self.clip.local(FP_CAMERA_TRACK, frame)

    def worlds(self, frame):
        c = self.clip
        base = c.local(1, frame) @ c.local(2, frame) @ c.local(3, frame) if self.first_person else np.eye(4)
        w = {}
        for i in sorted(self.tmap):
            n = self.tmap[i]
            if n == 'NPC Root [Root]':
                w[n] = np.eye(4)
            elif n == 'NPC COM [COM ]':
                w[n] = base @ c.local(i, frame)
            else:
                w[n] = w[self.tmap[self.parent[i]]] @ c.local(i, frame)
        if self.first_person:
            seen = FP_CAMERA_REST @ np.linalg.inv(self.camera(frame))
            w = {n: seen @ m for n, m in w.items()}
        return w


# ---------------------------------------------------------------------------------------------------- retarget

class Retargeter:
    def __init__(self, fo4_skeleton, sky_skeleton):
        self.fo = fo4_skeleton
        self.sky = sky_skeleton
        fo = fo4_skeleton
        # Skyrim rest worlds (nif, Root at the origin)
        self.sky_rest = {}
        # alignment per Fallout 4 bone: turn its rest so it points along the Skyrim rest bone
        self.align = {}
        for src, dst, dchild, schild in BONES:
            A = np.eye(3)
            if dchild and schild:
                fd = fo.rest_world[fo.idx[dchild]][:3, 3] - fo.rest_world[fo.idx[dst]][:3, 3]
                sd = self.rest(schild)[:3, 3] - self.rest(src)[:3, 3]
                A = rot_between(fd, sd)
            elif dst.endswith('3') or dst.endswith('Toe1'):
                # end joints have no child: point like the joint before them
                prev = dst[:-1] + '2' if 'Finger' in dst else dst.replace('Toe1', 'Foot')
                A = self.align[prev][0]
            self.align[dst] = (A, src)
        self.scale = fo.rest_world[fo.idx['COM']][2, 3] / self.rest('NPC COM [COM ]')[2, 3]

    def rest(self, n):
        if n not in self.sky_rest:
            self.sky_rest[n] = self.sky.relative(n, 'NPC Root [Root]')
        return self.sky_rest[n]

    def desired(self, sw):
        """Fallout 4 world rotations for the mapped bones"""
        out = {}
        for dst, (A, src) in self.align.items():
            D = sw[src][:3, :3] @ self.rest(src)[:3, :3].T
            out[dst] = D @ A @ self.fo.rest_world[self.fo.idx[dst]][:3, :3]
        return out

    def solve(self, desired, com_offset):
        """local and world transforms of every Fallout 4 bone for the desired world rotations, helpers after vanilla"""
        fo = self.fo
        world, local = [None] * len(fo.names), [None] * len(fo.names)
        forearm_twist = {'L': 0.0, 'R': 0.0}
        upper_twist = {'L': 0.0, 'R': 0.0}
        for i, name in enumerate(fo.names):
            p = fo.parent[i]
            pw = np.eye(4) if p < 0 else world[p]
            L = fo.rest_local[i].copy()
            if name in desired:
                L[:3, :3] = pw[:3, :3].T @ desired[name]
            if name == 'COM':
                L[:3, 3] = fo.rest_local[i][:3, 3] + com_offset
            for s in 'LR':
                if name == f'{s}Arm_ForeArm1':
                    # ForeArm1 only bends; the forearm's whole twist (its own + the hand's, which Skyrim keeps in
                    # the hand) is shared a third each by ForeArm2, ForeArm3 and the hand. The hand still reaches
                    # its world rotation (solved from ForeArm3's world).
                    L[:3, :3] = L[:3, :3] @ rot_x(-twist_x(fo.rest_local[i][:3, :3].T @ L[:3, :3]))
                    if f'{s}Arm_Hand' in desired:
                        rl = lambda n: fo.rest_local[fo.idx[n]][:3, :3]
                        untwisted = pw[:3, :3] @ L[:3, :3] @ rl(f'{s}Arm_ForeArm2') @ rl(f'{s}Arm_ForeArm3') @ rl(f'{s}Arm_Hand')
                        forearm_twist[s] = twist_x(untwisted.T @ desired[f'{s}Arm_Hand'])
                elif name in (f'{s}Arm_ForeArm2', f'{s}Arm_ForeArm3'):
                    L[:3, :3] = fo.rest_local[i][:3, :3] @ rot_x(forearm_twist[s] / 3)
                elif name == f'{s}Arm_UpperTwist1':
                    up = fo.idx[f'{s}Arm_UpperArm']
                    upper_twist[s] = twist_x(fo.rest_local[up][:3, :3].T @ local[up][:3, :3])
                    L[:3, :3] = fo.rest_local[i][:3, :3] @ rot_x(-upper_twist[s])
                elif name == f'{s}Arm_UpperTwist2':
                    L[:3, :3] = fo.rest_local[i][:3, :3] @ rot_x(upper_twist[s] / 2)
                elif name == f'{s}_RibHelper':
                    chest = world[fo.idx['Chest']]
                    d = chest[:3, :3].T @ (world[fo.idx[f'{s}Arm_ForeArm1']][:3, 3] - world[fo.idx[f'{s}Arm_UpperArm']][:3, 3])
                    raise_deg = np.degrees(np.arccos(np.clip(-d[0] / np.linalg.norm(d), -1, 1)))
                    ry, tx = rib_pose(s, raise_deg)
                    L[:3, :3] = fo.rest_local[i][:3, :3] @ rot_y(ry)
                    L[:3, 3] = fo.rest_local[i][:3, 3] + np.array([tx, 0.0, 0.0])
            local[i] = L
            world[i] = pw @ L
        return local, world

    def shoulder_match(self, desired, sw):
        """first person: COM offset putting the shoulders where the Skyrim shoulders are, relative to the camera
        (which is at the same spot in both)"""
        _, world = self.solve(desired, np.zeros(3))
        mid_fo = np.mean([world[self.fo.idx[f'{s}Arm_UpperArm']][:3, 3] for s in 'LR'], axis=0)
        mid_sky = np.mean([sw[f'NPC {s} UpperArm [{s}Uar]'][:3, 3] for s in 'LR'], axis=0)
        return mid_sky - mid_fo

    def reach_slide(self, desired, sw, offset):
        """first person: how far to slide the body toward a hand the arm can't reach from there - Skyrim's first
        person arms stretch further than Fallout 4's (42.9 vs 36.4). The body isn't drawn in first person; the hands
        are what's seen."""
        fo = self.fo
        slid = np.zeros(3)
        for _ in range(4):
            _, world = self.solve(desired, offset + slid)
            need = []
            for s in 'LR':
                S = world[fo.idx[f'{s}Arm_UpperArm']][:3, 3]
                E = world[fo.idx[f'{s}Arm_ForeArm1']][:3, 3]
                H = world[fo.idx[f'{s}Arm_Hand']][:3, 3]
                v = sw[f'NPC {s} Hand [{s}Hnd]'][:3, 3] - S
                short = np.linalg.norm(v) - (np.linalg.norm(E - S) + np.linalg.norm(H - E) - REACH_MARGIN)
                if short > 0:
                    need.append(v / np.linalg.norm(v) * short)
            if not need:
                break
            slid += np.mean(need, axis=0)
        return slid

    def elbow_aim(self, world, sw):
        """first person, per arm: where the elbow should bend toward - the Skyrim elbow itself (relative to the
        camera, as seen), off our shoulder -> hand line; plus the retargeted arm's own bend at a small fixed length,
        which decides only where the Skyrim offset fades (arm straight: it turns any way frame to frame)"""
        fo = self.fo
        out = {}
        for s in 'LR':
            S = world[fo.idx[f'{s}Arm_UpperArm']][:3, 3]
            E = world[fo.idx[f'{s}Arm_ForeArm1']][:3, 3]
            T = sw[f'NPC {s} Hand [{s}Hnd]'][:3, 3]
            u = (T - S) / np.linalg.norm(T - S)
            aim = sw[f'NPC {s} Forearm [{s}Lar]'][:3, 3] - S
            aim = aim - np.dot(aim, u) * u
            own = (E - S) - np.dot(E - S, u) * u
            if np.linalg.norm(own) > 1e-6:
                aim = aim + own / np.linalg.norm(own) * POLE_BLEND
            out[s] = aim
        return out

    def reach(self, desired, world, sw, aims):
        """first person: each hand to the Skyrim hand's place, the elbow bending toward aims[side]"""
        fo = self.fo
        for s in 'LR':
            S = world[fo.idx[f'{s}Arm_UpperArm']][:3, 3]
            E = world[fo.idx[f'{s}Arm_ForeArm1']][:3, 3]
            H = world[fo.idx[f'{s}Arm_Hand']][:3, 3]
            T = sw[f'NPC {s} Hand [{s}Hnd]'][:3, 3]
            a, b = np.linalg.norm(E - S), np.linalg.norm(H - E)
            d = np.clip(np.linalg.norm(T - S), abs(a - b) + 1e-3, a + b - 1e-3)
            u = (T - S) / np.linalg.norm(T - S)
            pole = aims[s] - np.dot(aims[s], u) * u
            if np.linalg.norm(pole) < 1e-6:
                pole = (E - S) - np.dot(E - S, u) * u
            v = pole / np.linalg.norm(pole)
            alpha = np.arccos(np.clip((a * a + d * d - b * b) / (2 * a * d), -1, 1))
            E2 = S + a * (np.cos(alpha) * u + np.sin(alpha) * v)
            R1 = rot_between(E - S, E2 - S)
            R2 = rot_between(R1 @ (H - E), S + d * u - E2)
            desired[f'{s}Arm_UpperArm'] = R1 @ desired[f'{s}Arm_UpperArm']
            desired[f'{s}Arm_ForeArm1'] = R2 @ R1 @ desired[f'{s}Arm_ForeArm1']
        return desired


def retarget(clip_path, out_hkx, fo4_skel_path, map_json, first_person=False, trim=0.0, end=None):
    src_clip = skyclip.load(clip_path)
    clip = Resampled(src_clip, trim, end)
    source = SourcePose(clip, map_json, first_person)
    rt = Retargeter(Fo4Skeleton(fo4_skel_path), skymap.SkyrimSkeleton())
    fo = rt.fo

    out = anim_fo4.AnimationData()
    out.duration = clip.duration
    out.num_frames = clip.num_frames
    out.num_tracks = N_TRACKS
    out.frame_duration = 1.0 / FPS
    out.original_skeleton_name = 'Root'
    out.track_to_bone_indices = list(range(N_TRACKS))
    out.blend_hint = 0
    out.tracks = [anim_fo4.TrackData() for _ in range(N_TRACKS)]
    stats = {'hand_error': 0.0, 'slid': 0.0}
    frames = [(source.worlds(fr),) for fr in range(clip.num_frames)]
    frames = [(sw, rt.desired(sw)) for (sw,) in frames]
    if first_person:
        # body placement for every frame first, so the reach slide can be smoothed over time: each frame takes the
        # largest slide within SLIDE_WINDOW frames of it (enough reach for its neighbours too), then a gaussian
        match = [rt.shoulder_match(desired, sw) for sw, desired in frames]
        need = [rt.reach_slide(desired, sw, m) for (sw, desired), m in zip(frames, match)]
        widest = [max(need[max(0, k - SLIDE_WINDOW):k + SLIDE_WINDOW + 1], key=np.linalg.norm) for k in range(len(need))]
        slide = smooth(widest, SLIDE_SIGMA)
        com_offsets = [m + s for m, s in zip(match, slide)]
        stats['slid'] = max(float(np.linalg.norm(s)) for s in slide)
        # elbow aims, smoothed over time the same way
        posed = [rt.solve(desired, off)[1] for (sw, desired), off in zip(frames, com_offsets)]
        aims = [rt.elbow_aim(world, sw) for world, (sw, _) in zip(posed, frames)]
        aims = [dict(zip('LR', pair)) for pair in zip(*(smooth([a[s] for a in aims], SLIDE_SIGMA) for s in 'LR'))]
    for fr, (sw, desired) in enumerate(frames):
        if first_person:
            local, world = rt.solve(rt.reach(desired, posed[fr], sw, aims[fr]), com_offsets[fr])
            for s in 'LR':
                err = np.linalg.norm(world[fo.idx[f'{s}Arm_Hand']][:3, 3] - sw[f'NPC {s} Hand [{s}Hnd]'][:3, 3])
                stats['hand_error'] = max(stats['hand_error'], float(err))
        else:
            # third person: COM moves as the Skyrim COM moves from its rest pose
            local, world = rt.solve(desired, rt.scale * (sw['NPC COM [COM ]'][:3, 3] - rt.rest('NPC COM [COM ]')[:3, 3]))
        for i in range(N_TRACKS):
            q = skymap.q_from_m(local[i][:3, :3])
            if fr and np.dot(q, out.tracks[i].rotations[-1]) < 0:
                q = -q
            out.tracks[i].rotations.append([float(v) for v in q])
            out.tracks[i].translations.append([float(v) for v in local[i][:3, 3]])
            out.tracks[i].scales.append([1.0, 1.0, 1.0])
    anim_fo4.write_fo4_animation(out_hkx, out)
    return out, rt, stats


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('clip')
    ap.add_argument('out_hkx')
    ap.add_argument('skeleton')
    ap.add_argument('map_json')
    ap.add_argument('--first-person', action='store_true')
    ap.add_argument('--trim', type=float, default=0.0)
    ap.add_argument('--end', type=float, help='cut the clip after this many seconds')
    a = ap.parse_args()
    o, rt, st = retarget(a.clip, a.out_hkx, a.skeleton, a.map_json, a.first_person, a.trim, a.end)
    fp = f"  1st person: hands off by <= {st['hand_error']:.2f}, body slid <= {st['slid']:.1f} for reach" if a.first_person else ''
    print(os.path.basename(a.out_hkx), o.num_frames, 'frames', round(o.duration, 3), 's' + fp)
