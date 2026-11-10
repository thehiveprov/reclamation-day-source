# Contact sheet of an emote's first person clip: what the player sees, every Nth frame - perspective from the Camera
# bone at rest looking forward, 80 degree horizontal field of view, a 16:9 screen frame (grey), the arms only (the
# first person model is arms). Red: the converted clip, read back from disk. Blue: the third person clip as it is
# (what copying the body's arms gave, 2026-10-26: under the view).
# Usage: sheet.py <first person clip.hkx> <third person clip.hkx> <third person skeleton> <first person skeleton>
#        <out.png> [--every n]
import argparse
import numpy as np
import anim_fo4
from retarget import Fo4Skeleton, mat
from render import Img, RED, BLUE, GREY
from first_person import worlds

FOV = np.radians(80)
NEAR = 5.0
CELL_W, CELL_H = 320, 180


def poses(clip, skel):
    for fr in range(clip.num_frames):
        local = [l.copy() for l in skel.rest_local]
        for t, b in enumerate(clip.track_to_bone_indices):
            tr = clip.tracks[t]
            local[b] = mat(tr.rotations[fr], tr.translations[fr])
        yield worlds(skel, local)


def draw(img, ox, oy, skel, world, inv_cam, colour):
    f = (CELL_W / 2) / np.tan(FOV / 2)

    def proj(p):
        c = (inv_cam @ np.append(p, 1))[:3]
        fwd = -c[2]
        if fwd < NEAR:
            return None
        return ox + CELL_W / 2 + f * c[0] / fwd, oy + CELL_H / 2 - f * c[1] / fwd

    for i, name in enumerate(skel.names):
        p = skel.parent[i]
        if p < 0 or 'Arm_' not in name or 'Arm_' not in skel.names[p]:
            continue
        a, b = proj(world[p][:3, 3]), proj(world[i][:3, 3])
        if a and b:
            img.line(a[0], a[1], b[0], b[1], colour, 2, clip=(ox, oy, ox + CELL_W, oy + CELL_H))


def sheet(first_hkx, third_hkx, third_skel, first_skel, out_png, every=6):
    s3, s1 = Fo4Skeleton(third_skel), Fo4Skeleton(first_skel)
    inv_cam = np.linalg.inv(s1.rest_world[s1.idx['Camera']])
    first = list(poses(anim_fo4.load_fo4_animation(first_hkx), s1))
    third = list(poses(anim_fo4.load_fo4_animation(third_hkx), s3))
    frames = list(range(0, len(first), every))
    cols = 5
    rows = (len(frames) + cols - 1) // cols
    img = Img(cols * (CELL_W + 8) + 8, rows * (CELL_H + 8) + 8)
    for k, fr in enumerate(frames):
        ox, oy = 8 + (k % cols) * (CELL_W + 8), 8 + (k // cols) * (CELL_H + 8)
        img.rect(ox, oy, ox + CELL_W - 1, oy + CELL_H - 1, GREY)
        draw(img, ox, oy, s3, third[min(fr, len(third) - 1)], inv_cam, BLUE)
        draw(img, ox, oy, s1, first[fr], inv_cam, RED)
    img.save(out_png)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    for n in ('first_hkx', 'third_hkx', 'third_skel', 'first_skel', 'out_png'):
        ap.add_argument(n)
    ap.add_argument('--every', type=int, default=6)
    a = ap.parse_args()
    sheet(a.first_hkx, a.third_hkx, a.third_skel, a.first_skel, a.out_png, a.every)
