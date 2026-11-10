# What our own emotes (the_bird.py, hands_up.py) share: a hand's fingers bent in their own planes, a hand turned to
# point its fingers and palm somewhere, and an arm reaching a place by two-bone IK. On the third person skeleton; the
# body's frame from Root: x right, y forward, z up. Bones lie along their local x axis.
import os
import numpy as np
import anim_fo4, skymap
from retarget import rot_between, rot_x, twist_x, slerp
from first_person import worlds, smoothstep

FINGER_TIP = 1.6  # past the last finger bone (it has no child), where the fingertip is


def unit(v):
    return v / np.linalg.norm(v)


def in_hand(sk, local, i, hand):
    """bone i's transform in the hand's space (local: each bone's on its parent)"""
    m = np.eye(4)
    while i != hand:
        m = local[i] @ m
        i = sk.parent[i]
    return m


def palm_normal(thumb_side, fingers, side):
    """out of the palm, from the line across the knuckles (little to index) and the fingers' line"""
    return unit(np.cross(thumb_side, fingers)) * (1.0 if side == 'R' else -1.0)


class HandShape:
    """a hand's finger bones set from the rest pose, in the hand's space"""

    def __init__(self, sk, side):
        self.sk, self.side = sk, side
        self.hand = sk.idx[f'{side}Arm_Hand']
        self.local = [l.copy() for l in sk.rest_local]
        self.palm = palm_normal(self.pos('Finger21') - self.pos('Finger51'), self.pos('Finger31'), side)

    def at(self, i):
        return in_hand(self.sk, self.local, i, self.hand)

    def bone(self, name):
        return self.sk.idx[f'{self.side}Arm_{name}']

    def pos(self, name):
        return self.at(self.bone(name))[:3, 3]

    def aim(self, i, want):
        """bone i turned (the least turn) so it points along want"""
        m = self.at(i)
        self.local[i][:3, :3] = self.at(self.sk.parent[i])[:3, :3].T @ rot_between(m[:3, 0], want) @ m[:3, :3]

    def bend(self, finger, bends):
        """a finger (2 index .. 5 little) bent toward the palm by its three bends (degrees, each on the bone before it,
        the first on the line from the wrist to its knuckle), in one plane: its knuckle's line and the palm's normal, so
        it doesn't splay or twist"""
        knuckle = unit(self.pos(f'Finger{finger}1'))
        toward = unit(self.palm - np.dot(self.palm, knuckle) * knuckle)
        total = 0.0
        for j, b in enumerate(bends, 1):
            total += np.radians(b)
            self.aim(self.bone(f'Finger{finger}{j}'), np.cos(total) * knuckle + np.sin(total) * toward)

    def thumb_to(self, target, shares=(0.5, 0.5, 0.1)):
        """the thumb's tip to target: each thumb bone turned in turn toward it (cyclic coordinate descent), each by its
        share of the turn a time (tip, middle, base) so no one joint takes it all; the base's share small, so it stays
        along the palm instead of swinging out from it"""
        thumb = [self.bone(f'Finger1{j}') for j in (3, 2, 1)]
        for _ in range(60):
            for i, share in zip(thumb, shares):
                m = self.at(i)
                end = self.at(thumb[0])
                tip = end[:3, 3] + end[:3, 0] * FINGER_TIP
                want = rot_between(tip - m[:3, 3], target - m[:3, 3])
                part = skymap.m_from_q(slerp(np.array([0.0, 0.0, 0.0, 1.0]), skymap.q_from_m(want), share))
                self.local[i][:3, :3] = self.at(self.sk.parent[i])[:3, :3].T @ part @ m[:3, :3]

    def turns(self):
        """the finger bones' turns on their parents (bone -> 3x3)"""
        return {self.bone(f'Finger{k}{j}'): self.local[self.bone(f'Finger{k}{j}')][:3, :3] for k in range(1, 6) for j in (1, 2, 3)}


def hand_turn(sk, side, fingers, want_finger, want_palm):
    """a hand's world turn with its middle finger (as `fingers` hold it: bone -> turn on its parent) along want_finger
    and its palm facing want_palm, from the rest pose's hand. The middle finger's own line, knuckle to tip: the rest
    pose's hand is a loose curl, so the knuckle line would leave the finger tipped toward the palm."""
    local = [l.copy() for l in sk.rest_local]
    for i, m in fingers.items():
        local[i][:3, :3] = m
    rw = worlds(sk, local)
    p = lambda n: rw[sk.idx[f'{side}Arm_{n}']][:3, 3]
    f = unit(p('Finger33') - p('Finger31'))
    palm = palm_normal(p('Finger21') - p('Finger51'), f, side)
    have = np.column_stack([f, palm, np.cross(f, palm)])
    want = np.column_stack([want_finger, want_palm, np.cross(want_finger, want_palm)])
    return want @ have.T @ rw[sk.idx[f'{side}Arm_Hand']][:3, :3]


def reach(sk, L, side, target, hand_world, pole):
    """an arm (L: the locals, changed in place) to target by two-bone IK on the upper arm and forearm, the elbow bending
    toward pole; the hand turned to hand_world, its turn about the forearm shared a third each by ForeArm2, ForeArm3 and
    the hand as vanilla clips share it (Tools/ConvertParkourAnimations/retarget.py)"""
    rest = lambda i: sk.rest_local[i][:3, :3]
    iu, ie, if2, if3, ih = (sk.idx[f'{side}Arm_{b}'] for b in ('UpperArm', 'ForeArm1', 'ForeArm2', 'ForeArm3', 'Hand'))
    W = worlds(sk, L)
    S, E, H = W[iu][:3, 3], W[ie][:3, 3], W[ih][:3, 3]
    a, b = np.linalg.norm(E - S), np.linalg.norm(H - E)
    d = np.clip(np.linalg.norm(target - S), abs(a - b) + 1e-3, a + b - 1e-3)
    u = (target - S) / np.linalg.norm(target - S)
    v = pole - np.dot(pole, u) * u
    v /= np.linalg.norm(v)
    alpha = np.arccos(np.clip((a * a + d * d - b * b) / (2 * a * d), -1, 1))
    E2 = S + a * (np.cos(alpha) * u + np.sin(alpha) * v)
    R1 = rot_between(E - S, E2 - S)
    R2 = rot_between(R1 @ (H - E), S + d * u - E2)
    want_upper, want_fore = R1 @ W[iu][:3, :3], R2 @ R1 @ W[ie][:3, :3]
    L[iu][:3, :3] = W[sk.parent[iu]][:3, :3].T @ want_upper
    W = worlds(sk, L)
    L[ie][:3, :3] = W[sk.parent[ie]][:3, :3].T @ want_fore
    W = worlds(sk, L)
    untwisted = W[ie][:3, :3] @ rest(if2) @ rest(if3) @ rest(ih)
    twist = twist_x(untwisted.T @ hand_world)
    L[if2][:3, :3] = rest(if2) @ rot_x(twist / 3)
    L[if3][:3, :3] = rest(if3) @ rot_x(twist / 3)
    W = worlds(sk, L)
    L[ih][:3, :3] = W[if3][:3, :3].T @ hand_world


def add_frame(out_rot, clip, L):
    """one frame's turns (L: the locals) onto out_rot (per track), each quaternion on the same side as the last"""
    for t, bone in enumerate(clip.track_to_bone_indices):
        q = skymap.q_from_m(L[bone][:3, :3])
        if out_rot[t] and np.dot(q, out_rot[t][-1]) < 0:
            q = -q
        out_rot[t].append([float(x) for x in q])


def write_clip(clip, out_rot, base, frames, fps, out_path):
    """the clip's tracks set to out_rot (per track, per frame), base's translations held, and written"""
    for t, bone in enumerate(clip.track_to_bone_indices):
        tr = clip.tracks[t]
        tr.rotations = out_rot[t]
        tr.translations = [[float(x) for x in base[bone][:3, 3]]] * frames
        tr.scales = [list(tr.scales[0])] * frames
    clip.num_frames = frames
    clip.duration = (frames - 1) / fps
    clip.frame_duration = 1.0 / fps
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    anim_fo4.write_fo4_animation(out_path, clip)
