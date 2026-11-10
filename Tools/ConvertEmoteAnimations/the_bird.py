# "The Bird" (2026-10-27, in place of No on the wheel, made from a reference picture: the hand up in
# front, low to the right of the view, upright, the middle finger straight up and the rest curled). The game has no
# middle finger gesture (none in its animation archives), so it's made here on the third person skeleton, from the body
# standing still (the first frame of MT\Neutral\PoseA_PointForward1.hkx, arms down): over RAISE seconds the right hand
# goes up to HAND (in front of the chest, under the chin, a little to the right; the body's frame: x right, y forward,
# z up, from Root), turned so its middle finger points up, its palm to the body and its back to the one it's shown to
# (turned that way after the first version had it backwards); held with a small sway (SWAY)
# until LOWER seconds before the end, then back down. The arm gets there by two-bone IK (hand_pose.py), the elbow under
# the hand so the forearm stands up and the wrist bends little (it bent too far at the wrist at first, 2026-10-27;
# HAND was 30 in front, the arm near straight, the wrist bent back 72 degrees, now about 17).
# The fingers (a fist but for the middle finger, which was too straight and stiff at first): the middle up with a slight natural curve (MIDDLE, as the game's own point holds
# its finger), the index, ring and little curled into the palm (FIST), the thumb laid across the curled index's middle
# bone. The first person clip is made from this one by first_person.py.
# Usage: the_bird.py <PoseA_PointForward1.hkx> <third person skeleton.hkx> <out.hkx>
import os, sys
import numpy as np
import anim_fo4, skymap
from retarget import Fo4Skeleton, mat, slerp
from first_person import worlds
from hand_pose import HandShape, hand_turn, reach, smoothstep, add_frame, write_clip

DURATION = 3.2
RAISE = 0.4
LOWER = 0.6
FPS = 30.0
HAND = np.array([13.0, 20.0, 110.0])  # the right hand's place, held (head at 121, right shoulder at 12.7, -3.8, 105)
ELBOW_POLE = np.array([0.5, 0.0, -1.0])  # where the elbow bends toward: down and a little out
# the held hand's sway: forward and back, up and down (units), and how long each takes (seconds)
SWAY = np.array([0.0, 0.8, 0.6])
SWAY_PERIOD = np.array([1.0, 2.2, 1.6])
# each finger's three bends toward the palm (degrees; hand_pose.HandShape.bend): the curled ones, the middle
FIST = (85.0, 100.0, 65.0)
MIDDLE = (18.0, 14.0, 6.0)
THUMB_OUT = 1.2  # the thumb's tip this far outside the curled index's middle bone


def fist(sk):
    """the right hand's finger bones' turns on their parents for the bird"""
    shape = HandShape(sk, 'R')
    for finger in (2, 4, 5):
        shape.bend(finger, FIST)
    shape.bend(3, MIDDLE)
    shape.thumb_to((shape.pos('Finger22') + shape.pos('Finger23')) / 2 + shape.palm * THUMB_OUT)
    return shape.turns()


def make(stand_path, skel_path, out_path):
    sk = Fo4Skeleton(skel_path)
    clip = anim_fo4.load_fo4_animation(stand_path)
    stand = [l.copy() for l in sk.rest_local]
    for t, b in enumerate(clip.track_to_bone_indices):
        tr = clip.tracks[t]
        stand[b] = mat(tr.rotations[0], tr.translations[0])
    fingers = fist(sk)
    want_hand = hand_turn(sk, 'R', fingers, np.array([0.0, 0.0, 1.0]), np.array([0.0, -1.0, 0.0]))
    ih = sk.idx['RArm_Hand']

    frames = int(round(DURATION * FPS)) + 1
    out_rot = [[] for _ in clip.track_to_bone_indices]
    for fr in range(frames):
        time = fr / FPS
        w = smoothstep(time / RAISE) * smoothstep((DURATION - time) / LOWER)
        L = [l.copy() for l in stand]
        for i, m in fingers.items():
            L[i][:3, :3] = skymap.m_from_q(slerp(skymap.q_from_m(stand[i][:3, :3]), skymap.q_from_m(m), w))
        W = worlds(sk, L)
        H = W[ih][:3, 3]
        hand_world = skymap.m_from_q(slerp(skymap.q_from_m(W[ih][:3, :3]), skymap.q_from_m(want_hand), w))
        sway = SWAY * np.sin(2 * np.pi * time / SWAY_PERIOD)
        reach(sk, L, 'R', H + w * (HAND + sway - H), hand_world, ELBOW_POLE)
        add_frame(out_rot, clip, L)
    write_clip(clip, out_rot, stand, frames, FPS, out_path)
    return clip


if __name__ == '__main__':
    c = make(*sys.argv[1:4])
    print(os.path.basename(sys.argv[3]), c.num_frames, 'frames', round(c.duration, 3), 's')
