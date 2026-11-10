# Better Interact's hand motions (docs/BETTER_INTERACT.md): the left arm's reach for picking up an item, pressing a
# button and pushing a door, written as keys in the first person view, and the hand shapes they use. Writes the table the
# client compiles (Code/client/Games/Fallout4/BetterInteractTable.inl) and draws what the player sees, frame by frame.
#
# Nothing here is read from B42 Loot's files and none of its data ships: its clip was measured as a reference for timing
# and path (as a video would be), and these keys are written to that.
#
# The first person motions' frame is the view (the first person skeleton's Camera bone): x right, y up, z back. The third
# person ones' is the same three directions at the chest bone, the character facing -z. The hand's place is LArm_Hand's; its
# turn is given by where the fingers point (wrist to the middle knuckle) and where the palm faces. Both skeletons have the
# same hand and the same arm's length (main() checks).
#
# Usage: make_hands.py <first person skeleton.hkx> <third person skeleton.hkx> <H2H Equip.hkx, the arm's pose in the drawing>
#        <out .inl> <out .png> [<reference path .json>]
# Blender 4.4's Python with PyNifly's hkx module and Tools/ConvertEmoteAnimations + ConvertParkourAnimations on the path
# (as Tools/ConvertEmoteAnimations/export_all.sh sets it up).
import json
import sys
import numpy as np
import anim_fo4, skymap
from retarget import Fo4Skeleton, mat
from first_person import worlds, smoothstep
from hand_pose import HandShape, palm_normal, reach, unit
from render import Img, RED, BLUE, GREY, GREEN

# Each finger's three bends toward the palm (degrees, hand_pose.HandShape.bend), index to little, and where the thumb's
# tip goes: "rest" or a place on the index or middle finger (its joint, out of the palm by)
SHAPES = {
    # wide open, reaching
    "Open": {"bends": [(6, 8, 4), (8, 8, 4), (10, 10, 5), (12, 12, 6)], "thumb": "rest"},
    # closed around something small: the fingers wrap, the thumb comes across
    "Grip": {"bends": [(26, 32, 22), (30, 36, 24), (34, 40, 26), (38, 44, 28)], "thumb": ("Finger22", 2.4)},
    # the index finger out, the rest curled in, the thumb on the middle finger
    "Point": {"bends": [(4, 7, 4), (62, 84, 54), (70, 90, 58), (76, 94, 60)], "thumb": ("Finger32", 1.2)},
    # flat, a push
    "Flat": {"bends": [(2, 4, 2), (3, 4, 2), (4, 5, 3), (6, 6, 3)], "thumb": "rest"},
}
SHAPE_NAMES = list(SHAPES)

# Per motion: its keys (seconds, the hand's place, fingers' way, palm's way), which key is the contact (moved toward what
# the player reaches for), how far the fingertips are past the wrist there, the shoulder going with the arm (its peak, when
# it's back, by how much), the shapes in turn (shape, from when, on by when), how it fades, and the item's times
GRAB = {
    "keys": [
        (0.000, (-16.5, -18.5, -31.0), (0.45, 0.68, -0.58), (0.64, -0.70, -0.33)),
        (0.067, (-12.5, -12.5, -37.5), (0.30, 0.79, -0.53), (0.67, -0.57, -0.47)),
        (0.133, (-9.2, -6.2, -41.2), (0.38, 0.86, -0.33), (0.67, -0.51, -0.55)),
        (0.167, (-8.4, -5.0, -42.0), (0.42, 0.86, -0.30), (0.66, -0.51, -0.54)),
        (0.233, (-9.4, -10.0, -37.0), (0.39, 0.78, -0.49), (0.71, -0.59, -0.39)),
        (0.300, (-10.0, -16.0, -30.5), (0.42, 0.60, -0.67), (0.74, -0.66, -0.12)),
        (0.367, (-11.2, -22.0, -23.0), (0.46, 0.40, -0.79), (0.78, -0.61, 0.14)),
        (0.433, (-15.0, -29.0, -12.0), (0.35, -0.05, -0.93), (0.77, -0.55, 0.32)),
    ],
    "contact": 3,
    "tip": 5.0,
    "shoulder": (0.167, 0.40, (-0.3, -2.2, -8.2)),
    "shapes": [("Open", 0.0, 0.07), ("Grip", 0.12, 0.23)],
    "blend_in": 0.08,
    "blend_out": 0.17,
    "take": 0.16,
    "release": 0.40,
}
PRESS = {
    "keys": [
        (0.000, (-15.0, -21.0, -21.0), (0.30, 0.55, -0.78), (0.75, -0.60, -0.10)),
        (0.100, (-10.0, -13.0, -29.0), (0.22, 0.45, -0.87), (0.70, -0.70, 0.10)),
        (0.200, (-6.5, -7.5, -33.0), (0.18, 0.28, -0.94), (0.60, -0.78, 0.15)),
        (0.260, (-6.2, -7.2, -35.0), (0.18, 0.26, -0.95), (0.60, -0.78, 0.15)),
        (0.330, (-7.0, -8.5, -31.5), (0.20, 0.30, -0.93), (0.62, -0.76, 0.12)),
        (0.430, (-11.0, -17.0, -25.0), (0.28, 0.40, -0.87), (0.70, -0.70, 0.05)),
        (0.540, (-15.0, -27.0, -14.0), (0.32, 0.20, -0.92), (0.75, -0.62, 0.20)),
    ],
    "contact": 3,
    "tip": 17.0,
    "shoulder": (0.26, 0.48, (0.5, -1.0, -6.0)),
    "shapes": [("Point", 0.0, 0.12), ("Open", 0.38, 0.50)],
    "blend_in": 0.09,
    "blend_out": 0.17,
    "take": -1.0,
    "release": -1.0,
}
PUSH = {
    "keys": [
        (0.000, (-15.0, -21.0, -20.0), (0.35, 0.60, -0.72), (0.60, -0.62, -0.50)),
        (0.120, (-11.0, -12.0, -28.0), (0.25, 0.85, -0.45), (0.30, -0.35, -0.88)),
        (0.220, (-8.0, -6.0, -36.0), (0.20, 0.95, -0.22), (0.12, -0.18, -0.97)),
        (0.320, (-7.5, -5.5, -40.0), (0.18, 0.96, -0.20), (0.10, -0.16, -0.98)),
        (0.420, (-9.0, -9.0, -36.0), (0.25, 0.85, -0.45), (0.25, -0.40, -0.88)),
        (0.520, (-12.0, -18.0, -27.0), (0.35, 0.55, -0.75), (0.55, -0.60, -0.55)),
        (0.620, (-15.0, -28.0, -15.0), (0.35, 0.15, -0.92), (0.72, -0.60, 0.05)),
    ],
    "contact": 2,
    "tip": 7.0,
    "shoulder": (0.32, 0.56, (0.5, -0.5, -8.0)),
    "shapes": [("Flat", 0.0, 0.14), ("Open", 0.44, 0.58)],
    "blend_in": 0.10,
    "blend_out": 0.18,
    "take": -1.0,
    "release": -1.0,
}
MOTIONS = [("Grab", GRAB), ("Press", PRESS), ("Push", PUSH)]
# Third person: the same timing, the hand out from the chest (the shoulder is at about (-12.7, 13.9, 4.3), the arm 36.4 long),
# the body leaning into it by "lean" radians at its peak (at the shoulder's times)
GRAB_3P = {
    "keys": [
        (0.000, (-18.0, -6.0, -14.0), (0.20, -0.25, -0.95), (0.55, -0.80, 0.25)),
        (0.090, (-15.0, -6.0, -22.0), (0.15, -0.40, -0.90), (0.40, -0.80, 0.45)),
        (0.167, (-12.5, -4.0, -25.0), (0.10, -0.55, -0.83), (0.25, -0.80, 0.55)),
        (0.233, (-13.0, -2.0, -21.0), (0.20, -0.35, -0.92), (0.35, -0.85, 0.40)),
        (0.300, (-14.0, -1.0, -16.0), (0.30, -0.10, -0.95), (0.55, -0.75, 0.35)),
        (0.367, (-15.0, -3.0, -12.0), (0.35, 0.10, -0.93), (0.70, -0.60, 0.40)),
        (0.433, (-17.0, -8.0, -8.0), (0.30, 0.00, -0.95), (0.80, -0.50, 0.35)),
    ],
    "contact": 2,
    "tip": 5.0,
    "shoulder": (0.167, 0.40, (0.3, -1.5, -4.0)),
    "lean": 0.30,
    "shapes": GRAB["shapes"],
    "blend_in": 0.08,
    "blend_out": 0.17,
    "take": 0.16,
    "release": 0.40,
}
PRESS_3P = {
    "keys": [
        (0.000, (-15.0, 0.0, -16.0), (0.25, 0.20, -0.95), (0.70, -0.65, 0.15)),
        (0.100, (-12.0, 4.0, -24.0), (0.15, 0.15, -0.98), (0.70, -0.70, 0.05)),
        (0.200, (-9.0, 7.0, -28.0), (0.12, 0.10, -0.99), (0.70, -0.70, 0.00)),
        (0.260, (-8.5, 7.0, -30.0), (0.12, 0.10, -0.99), (0.70, -0.70, 0.00)),
        (0.330, (-9.5, 6.0, -27.0), (0.14, 0.10, -0.98), (0.70, -0.70, 0.02)),
        (0.430, (-13.0, 1.0, -20.0), (0.22, 0.05, -0.97), (0.72, -0.65, 0.10)),
        (0.540, (-16.0, -6.0, -12.0), (0.30, -0.15, -0.94), (0.75, -0.60, 0.25)),
    ],
    "contact": 3,
    "tip": 17.0,
    "shoulder": (0.26, 0.48, (0.5, -0.5, -3.0)),
    "lean": 0.06,
    "shapes": PRESS["shapes"],
    "blend_in": 0.09,
    "blend_out": 0.17,
    "take": -1.0,
    "release": -1.0,
}
PUSH_3P = {
    "keys": [
        (0.000, (-15.0, 0.0, -14.0), (0.30, 0.55, -0.78), (0.50, -0.60, -0.60)),
        (0.120, (-12.5, 5.0, -22.0), (0.20, 0.85, -0.45), (0.25, -0.40, -0.88)),
        (0.220, (-11.0, 7.0, -29.0), (0.12, 0.96, -0.22), (0.10, -0.20, -0.97)),
        (0.320, (-10.5, 7.5, -31.0), (0.10, 0.97, -0.20), (0.08, -0.18, -0.98)),
        (0.420, (-12.0, 6.0, -27.0), (0.20, 0.85, -0.45), (0.20, -0.40, -0.88)),
        (0.520, (-14.0, 1.0, -20.0), (0.30, 0.55, -0.75), (0.50, -0.60, -0.60)),
        (0.620, (-16.0, -6.0, -12.0), (0.35, 0.15, -0.92), (0.72, -0.60, 0.05)),
    ],
    "contact": 2,
    "tip": 7.0,
    "shoulder": (0.32, 0.56, (0.5, -0.3, -4.0)),
    "lean": 0.10,
    "shapes": PUSH["shapes"],
    "blend_in": 0.10,
    "blend_out": 0.18,
    "take": -1.0,
    "release": -1.0,
}
MOTIONS_3P = [("Grab3P", GRAB_3P), ("Press3P", PRESS_3P), ("Push3P", PUSH_3P)]
# The elbow's way in the motions' frame: down, out and a little back
POLE = (-0.7, -0.7, 0.1)
# how much earlier each finger moves than the index, per finger toward the little one (the hand closes in a wave)
FINGER_WAVE = 0.012


def finger_turns(sk, spec):
    shape = HandShape(sk, "L")
    for finger, bends in zip((2, 3, 4, 5), spec["bends"]):
        shape.bend(finger, bends)
    if spec["thumb"] != "rest":
        joint, out = spec["thumb"]
        shape.thumb_to(shape.pos(joint) + shape.palm * out)
    return shape.local


def hand_frame(sk):
    """the hand's own frame in its bone's axes: where its fingers point, its palm faces, their cross; where an item is held"""
    shape = HandShape(sk, "L")
    across = shape.pos("Finger21") - shape.pos("Finger51")
    fingers = unit(shape.pos("Finger31"))
    palm = palm_normal(across, fingers, "L")
    palm = unit(palm - np.dot(palm, fingers) * fingers)
    side = np.cross(fingers, palm)
    grip = shape.pos("Finger31") * 0.75 + palm * 2.4
    return fingers, palm, side, grip


def v3(v):
    return "{%.5ff, %.5ff, %.5ff}" % tuple(v)


def write_table(sk, path):
    fingers, palm, side, grip = hand_frame(sk)
    out = ["// Generated by Tools/BetterInteractHands/make_hands.py. Do not edit by hand.", ""]
    out.append("// the left hand in its bone's axes: where the fingers point, the palm faces, their cross; where an item is held")
    out.append(f"inline constexpr float kHandFingers[3] = {v3(fingers)};")
    out.append(f"inline constexpr float kHandPalm[3] = {v3(palm)};")
    out.append(f"inline constexpr float kHandSide[3] = {v3(side)};")
    out.append(f"inline constexpr float kHandGrip[3] = {v3(grip)};")
    out.append(f"inline constexpr float kFingerWave = {FINGER_WAVE:.3f}f;")
    out.append(f"inline constexpr float kPole[3] = {v3(POLE)};")
    out.append("// the forearm's twist bones and the hand on their parents at rest, w x y z: a hand's turn about the forearm is shared a")
    out.append("// third each by them, as the game's own clips share it")
    for name, bone in (("kRestFore2", "LArm_ForeArm2"), ("kRestFore3", "LArm_ForeArm3"), ("kRestHand", "LArm_Hand")):
        q = skymap.q_from_m(sk.rest_local[sk.idx[bone]][:3, :3])
        out.append("inline constexpr float %s[4] = {%.5ff, %.5ff, %.5ff, %.5ff};" % (name, q[3], q[0], q[1], q[2]))
    out.append("")
    out.append("// each hand shape: the 15 left finger bones (thumb, index .. little; three joints each) on their parents, w x y z")
    out.append(f"inline constexpr int kShapeCount = {len(SHAPES)};")
    for i, name in enumerate(SHAPE_NAMES):
        out.append(f"inline constexpr int kShape{name} = {i};")
    out.append("inline constexpr float kShapes[kShapeCount][15][4] = {")
    for name in SHAPE_NAMES:
        local = finger_turns(sk, SHAPES[name])
        out.append(f"    {{ // {name}")
        for k in range(1, 6):
            for j in (1, 2, 3):
                q = skymap.q_from_m(local[sk.idx[f"LArm_Finger{k}{j}"]][:3, :3])  # x y z w
                out.append("        {%.5ff, %.5ff, %.5ff, %.5ff}," % (q[3], q[0], q[1], q[2]))
        out.append("    },")
    out.append("};")
    out.append("")
    for name, m in MOTIONS + MOTIONS_3P:
        out.append(f"inline constexpr Key k{name}Keys[] = {{")
        for t, p, f, pl in m["keys"]:
            out.append("    {%.3ff, %s, %s, %s}," % (t, v3(p), v3(unit(np.array(f))), v3(unit(np.array(pl)))))
        out.append("};")
        shapes = ", ".join("{kShape%s, %.3ff, %.3ff}" % s for s in m["shapes"])
        out.append(f"inline constexpr ShapeStep k{name}Shapes[] = {{{shapes}}};")
        s = m["shoulder"]
        out.append(
            f"inline constexpr Motion k{name} = {{k{name}Keys, static_cast<int>(std::size(k{name}Keys)), {m['contact']}, {m['tip']:.1f}f, {s[0]:.3f}f, {s[1]:.3f}f, {v3(s[2])}, "
            f"k{name}Shapes, static_cast<int>(std::size(k{name}Shapes)), {m['blend_in']:.3f}f, {m['blend_out']:.3f}f, {m['take']:.3f}f, {m['release']:.3f}f, {m.get('lean', 0.0):.3f}f}};"
        )
        out.append("")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out))


# --- the drawing: the same sum the client does (BetterInteractClip.cpp), on the game's unarmed first person pose


def catmull(ps, ts, t):
    t = float(np.clip(t, ts[0], ts[-1]))
    i = min(int(np.searchsorted(ts, t, side="right")) - 1, len(ts) - 2)
    p0, p1, p2, p3 = ps[max(i - 1, 0)], ps[i], ps[i + 1], ps[min(i + 2, len(ps) - 1)]
    t0, t1, t2, t3 = ts[max(i - 1, 0)], ts[i], ts[i + 1], ts[min(i + 2, len(ts) - 1)]
    u = (t - t1) / (t2 - t1)
    m1 = (p2 - p0) / max(t2 - t0, 1e-3) * (t2 - t1)
    m2 = (p3 - p1) / max(t3 - t1, 1e-3) * (t2 - t1)
    return (2 * u**3 - 3 * u**2 + 1) * p1 + (u**3 - 2 * u**2 + u) * m1 + (-2 * u**3 + 3 * u**2) * p2 + (u**3 - u**2) * m2


def at(m, t):
    ts = np.array([k[0] for k in m["keys"]])
    end = ts[-1]
    pos = catmull(np.array([k[1] for k in m["keys"]], dtype=float), ts, t)
    f = unit(catmull(np.array([unit(np.array(k[2])) for k in m["keys"]]), ts, t))
    p = catmull(np.array([unit(np.array(k[3])) for k in m["keys"]]), ts, t)
    p = unit(p - np.dot(p, f) * f)
    w = smoothstep(t / m["blend_in"]) * (1 - smoothstep((t - end) / m["blend_out"]))
    peak, fall, push = m["shoulder"]
    bump = smoothstep(t / peak) if t < peak else 1 - smoothstep((t - peak) / (fall - peak))
    return pos, f, p, w, bump * np.array(push)


def nlerp(a, b, u):
    if np.dot(a, b) < 0:
        b = -b
    q = a + (b - a) * u
    return q / np.linalg.norm(q)


def fingers_at(sk, shapes, m, t, base):
    """the finger bones' turns at a time: the arm's own, then each step of the motion coming on"""
    out = {}
    for k in range(1, 6):
        for j in (1, 2, 3):
            i = sk.idx[f"LArm_Finger{k}{j}"]
            q = skymap.q_from_m(base[i][:3, :3])
            for name, t0, t1 in m["shapes"]:
                u = smoothstep((t - t0 + FINGER_WAVE * (k - 1)) / max(t1 - t0, 1e-3))
                q = nlerp(q, skymap.q_from_m(shapes[name][i][:3, :3]), u)
            out[i] = skymap.m_from_q(q)
    return out


def draw(sk, base, out_path, measured=None):
    fingers_l, palm_l, side_l, grip_l = hand_frame(sk)
    shapes = {n: finger_turns(sk, SHAPES[n]) for n in SHAPE_NAMES}
    cam = sk.rest_world[sk.idx["Camera"]]
    inv_cam = np.linalg.inv(cam)
    W, H = 300, 169
    f_px = (W / 2) / np.tan(np.radians(80) / 2)
    STEP = 1 / 15
    rows = [(name, m, int(np.ceil((m["keys"][-1][0] + m["blend_out"]) / STEP)) + 1) for name, m in MOTIONS]
    cols = max(r[2] for r in rows)
    img = Img(cols * (W + 6) + 6, len(rows) * (H + 6) + 6)
    ih = sk.idx["LArm_Hand"]
    collar = sk.idx["LArm_Collarbone"]
    clip = None
    for r, (name, m, n) in enumerate(rows):
        for c in range(n):
            t = c * STEP
            ox, oy = 6 + c * (W + 6), 6 + r * (H + 6)
            clip = (ox, oy, ox + W, oy + H)
            img.rect(ox, oy, ox + W - 1, oy + H - 1, GREY)
            pos, f, p, w, push = at(m, t)
            L = [l.copy() for l in base]
            for i, turn in fingers_at(sk, shapes, m, t, base).items():
                L[i][:3, :3] = turn
            WW = worlds(sk, L)
            L[collar][:3, 3] += (WW[sk.parent[collar]][:3, :3].T @ cam[:3, :3] @ push) * w
            WW = worlds(sk, L)
            wrist = WW[ih][:3, 3]
            target = cam[:3, :3] @ pos + cam[:3, 3]
            fw, pw = cam[:3, :3] @ f, cam[:3, :3] @ p
            want = np.column_stack([fw, pw, np.cross(fw, pw)])
            have = np.column_stack([fingers_l, palm_l, side_l])
            hand_world = want @ have.T
            q = nlerp(skymap.q_from_m(WW[ih][:3, :3]), skymap.q_from_m(hand_world), w)
            reach(sk, L, "L", wrist + (target - wrist) * w, skymap.m_from_q(q), cam[:3, :3] @ np.array([-0.7, -0.7, 0.1]))
            WW = worlds(sk, L)

            def proj(v3d):
                v = (inv_cam @ np.append(v3d, 1))[:3]
                if -v[2] < 5:
                    return None
                return ox + W / 2 + f_px * v[0] / -v[2], oy + H / 2 - f_px * v[1] / -v[2]

            img.line(ox + W / 2 - 4, oy + H / 2, ox + W / 2 + 4, oy + H / 2, GREY, 1, clip=clip)
            img.line(ox + W / 2, oy + H / 2 - 4, ox + W / 2, oy + H / 2 + 4, GREY, 1, clip=clip)
            for i, nm in enumerate(sk.names):
                pa = sk.parent[i]
                if pa < 0 or not nm.startswith("LArm_") or not sk.names[pa].startswith("LArm_"):
                    continue
                a, b = proj(WW[pa][:3, 3]), proj(WW[i][:3, 3])
                if a and b:
                    img.line(a[0], a[1], b[0], b[1], RED, 2, clip=clip)
            if 0 <= m["take"] <= t <= m["release"]:
                g = proj((WW[ih] @ np.append(grip_l, 1))[:3])
                if g:
                    img.rect(int(g[0]) - 3, int(g[1]) - 3, int(g[0]) + 3, int(g[1]) + 3, GREEN)
            if measured is not None and name == "Grab":
                k = int(round(t * 30))
                if k < len(measured):
                    g = proj(cam[:3, :3] @ np.array(measured[k]) + cam[:3, 3])
                    if g:
                        img.rect(int(g[0]) - 2, int(g[1]) - 2, int(g[0]) + 2, int(g[1]) + 2, BLUE)
    img.save(out_path)


def draw_3p(sk, out_path):
    """the third person motions from the character's left, frame by frame (the arm red, the body blue, a mark where an item goes)"""
    fingers_l, palm_l, side_l, grip_l = hand_frame(sk)
    shapes = {n: finger_turns(sk, SHAPES[n]) for n in SHAPE_NAMES}
    W, H = 220, 240
    STEP = 1 / 15
    rows = [(name, m, int(np.ceil((m["keys"][-1][0] + m["blend_out"]) / STEP)) + 1) for name, m in MOTIONS_3P]
    cols = max(r[2] for r in rows)
    img = Img(cols * (W + 6) + 6, len(rows) * (H + 6) + 6)
    ih, chest = sk.idx["LArm_Hand"], sk.idx["Chest"]
    # the frame's directions in the skeleton's (it faces +y, z up): x right, y up, z back, as columns
    frame = np.array([[1.0, 0, 0], [0, 0, 1.0], [0, -1.0, 0]]).T
    for r, (name, m, n) in enumerate(rows):
        for c in range(n):
            t = c * STEP
            ox, oy = 6 + c * (W + 6), 6 + r * (H + 6)
            clip = (ox, oy, ox + W, oy + H)
            img.rect(ox, oy, ox + W - 1, oy + H - 1, GREY)
            pos, f, p, w, push = at(m, t)
            L = [l.copy() for l in sk.rest_local]
            WW = worlds(sk, L)
            origin = WW[chest][:3, 3]
            # the arm down at the side first (the rest pose holds it out): the game's pose under the motion
            reach(sk, L, "L", origin + frame @ np.array([-16.0, -18.0, -6.0]), WW[ih][:3, :3], frame @ np.array(POLE))
            base = [l.copy() for l in L]
            for i, turn in fingers_at(sk, shapes, m, t, base).items():
                L[i][:3, :3] = turn
            WW = worlds(sk, L)
            collar = sk.idx["LArm_Collarbone"]
            L[collar][:3, 3] += (WW[sk.parent[collar]][:3, :3].T @ frame @ push) * w
            WW = worlds(sk, L)
            wrist = WW[ih][:3, 3]
            target = frame @ pos + origin
            fw, pw = frame @ f, frame @ p
            want = np.column_stack([fw, pw, np.cross(fw, pw)])
            have = np.column_stack([fingers_l, palm_l, side_l])
            q = nlerp(skymap.q_from_m(WW[ih][:3, :3]), skymap.q_from_m(want @ have.T), w)
            reach(sk, L, "L", wrist + (target - wrist) * w, skymap.m_from_q(q), frame @ np.array(POLE))
            WW = worlds(sk, L)

            def proj(v3d):
                # seen from its left: ahead is left on the panel
                return ox + W * 0.62 - (v3d[1] - origin[1]) * 2.2, oy + H * 0.45 - (v3d[2] - origin[2]) * 2.2

            for i, nm in enumerate(sk.names):
                pa = sk.parent[i]
                if pa < 0:
                    continue
                arm = nm.startswith("LArm_") and sk.names[pa].startswith("LArm_")
                body = nm in ("Spine1", "Spine2", "Chest", "Neck", "Head", "LArm_Collarbone", "LLeg_Thigh")
                if not arm and not body:
                    continue
                a, b = proj(WW[pa][:3, 3]), proj(WW[i][:3, 3])
                img.line(a[0], a[1], b[0], b[1], RED if arm else BLUE, 2, clip=clip)
            if 0 <= m["take"] <= t <= m["release"]:
                g = proj((WW[ih] @ np.append(grip_l, 1))[:3])
                img.rect(int(g[0]) - 3, int(g[1]) - 3, int(g[0]) + 3, int(g[1]) + 3, GREEN)
    img.save(out_path)


def main():
    skel_path, skel_3p_path, equip_path, inl_path, png_path = sys.argv[1:6]
    sk = Fo4Skeleton(skel_path)
    sk_3p = Fo4Skeleton(skel_3p_path)
    # the one table does for both skeletons: the hand's own frame and the fingers, the forearm's bones at rest
    for a, b, what in zip(hand_frame(sk), hand_frame(sk_3p), ("fingers", "palm", "side", "grip")):
        assert np.allclose(a, b, atol=1e-3), what
    for name in SHAPE_NAMES:
        a, b = finger_turns(sk, SHAPES[name]), finger_turns(sk_3p, SHAPES[name])
        for k in range(1, 6):
            for j in (1, 2, 3):
                assert np.allclose(a[sk.idx[f"LArm_Finger{k}{j}"]][:3, :3], b[sk_3p.idx[f"LArm_Finger{k}{j}"]][:3, :3], atol=1e-3), (name, k, j)
    for bone in ("LArm_ForeArm2", "LArm_ForeArm3", "LArm_Hand"):
        assert np.allclose(sk.rest_local[sk.idx[bone]], sk_3p.rest_local[sk_3p.idx[bone]], atol=1e-3), bone
    write_table(sk, inl_path)
    clip = anim_fo4.load_fo4_animation(equip_path)
    base = [l.copy() for l in sk.rest_local]
    last = clip.num_frames - 1
    for t, b in enumerate(clip.track_to_bone_indices):
        tr = clip.tracks[t]
        base[b] = mat(tr.rotations[last], tr.translations[last])
    measured = None
    if len(sys.argv) > 6:
        with open(sys.argv[6]) as f:
            o = json.load(f)
        measured = [fr["LArm_Hand"]["camera"]["translation"] for fr in o["frames"]]
    draw(sk, base, png_path, measured)
    draw_3p(sk_3p, png_path.replace(".png", "_3p.png"))


if __name__ == "__main__":
    main()
