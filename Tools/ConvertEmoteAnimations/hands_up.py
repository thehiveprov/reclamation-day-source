# "Hands Up!" (2026-10-27, in place of Yes on the wheel, with a clip of its own; its third person clip matches its
# first person one near 1-1, like the other emotes): standing, both hands raised in front of the head,
# open, palms out, held with a small bob and lowered again. The game has no standing hands-up clip (none in its
# animation archives; its kneeling hostage holds the hands out beside the head, where a first person view can't see
# them), so it's made here on the third person skeleton from the body standing still (the first frame of
# MT\Neutral\PoseA_PointForward1.hkx, arms down): over RAISE seconds each hand goes up to HAND (mirrored for the left;
# the body's frame: x right, y forward, z up, from Root), its fingers up and its palm forward; held until LOWER seconds
# before the end, then back down. Each arm gets there by two-bone IK (the elbow down and out; hand_pose.py). HAND is
# where the first person view frames the top of the screen with them (about 30 degrees to each side, at the eyes'
# height), so the first person clip, made from this one by first_person.py, shows the same pose.
# Usage: hands_up.py <PoseA_PointForward1.hkx> <third person skeleton.hkx> <out.hkx>
import os, sys
import numpy as np
import anim_fo4, skymap
from retarget import Fo4Skeleton, mat, slerp
from first_person import worlds
from hand_pose import HandShape, hand_turn, reach, smoothstep, add_frame, write_clip

DURATION = 3.5
RAISE = 0.5
LOWER = 0.6
FPS = 30.0
HAND = np.array([18.0, 26.0, 118.0])  # the right hand's place, held (head at 121, right shoulder at 12.7, -3.8, 105)
BOB, BOB_PERIOD = 0.8, 1.4  # the hands' bob while held: up and down this far, this often (seconds)
OPEN = (4.0, 6.0, 4.0)  # each finger's three bends toward the palm (degrees; hand_pose.HandShape.bend): nearly straight


def open_hand(sk, side):
    shape = HandShape(sk, side)
    for finger in (2, 3, 4, 5):
        shape.bend(finger, OPEN)
    return shape.turns()


def make(stand_path, skel_path, out_path):
    sk = Fo4Skeleton(skel_path)
    clip = anim_fo4.load_fo4_animation(stand_path)
    stand = [l.copy() for l in sk.rest_local]
    for t, b in enumerate(clip.track_to_bone_indices):
        tr = clip.tracks[t]
        stand[b] = mat(tr.rotations[0], tr.translations[0])
    sides = {}
    for side, sign in (('R', 1.0), ('L', -1.0)):
        fingers = open_hand(sk, side)
        turn = hand_turn(sk, side, fingers, np.array([0.0, 0.0, 1.0]), np.array([0.0, 1.0, 0.0]))
        sides[side] = (sign, fingers, turn)

    frames = int(round(DURATION * FPS)) + 1
    out_rot = [[] for _ in clip.track_to_bone_indices]
    for fr in range(frames):
        time = fr / FPS
        w = smoothstep(time / RAISE) * smoothstep((DURATION - time) / LOWER)
        bob = BOB * np.sin(2 * np.pi * time / BOB_PERIOD)
        L = [l.copy() for l in stand]
        for side, (sign, fingers, turn) in sides.items():
            for i, m in fingers.items():
                L[i][:3, :3] = skymap.m_from_q(slerp(skymap.q_from_m(stand[i][:3, :3]), skymap.q_from_m(m), w))
            ih = sk.idx[f'{side}Arm_Hand']
            W = worlds(sk, L)
            H = W[ih][:3, 3]
            goal = HAND * np.array([sign, 1.0, 1.0]) + np.array([0.0, 0.0, bob])
            hand_world = skymap.m_from_q(slerp(skymap.q_from_m(W[ih][:3, :3]), skymap.q_from_m(turn), w))
            reach(sk, L, side, H + w * (goal - H), hand_world, np.array([sign, 0.0, -1.0]))
        add_frame(out_rot, clip, L)
    write_clip(clip, out_rot, stand, frames, FPS, out_path)
    return clip


if __name__ == '__main__':
    c = make(*sys.argv[1:4])
    print(os.path.basename(sys.argv[3]), c.num_frames, 'frames', round(c.duration, 3), 's')
