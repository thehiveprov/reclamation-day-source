# Contact sheets of an exported clip (read back from disk) against its Skyrim source, every Nth frame.
#   third person: side view (forward to the right) and front view, the travel curve applied, the ledge height drawn
#   first person: what the player sees - perspective from the camera (0, 0, 120.5) looking forward, 80 degree
#                 horizontal field of view, a 16:9 screen frame; arms only (the first person model is arms)
# Fallout 4 red, Skyrim blue. Pure numpy + zlib PNG.
# Usage: render.py <exported hkx> <json> <skyrim hkx> <map json> <fo4 skeleton> <out png> [--first-person]
#        [--ledge z] [--trim s] [--every n] [--pitch deg]
import argparse, json, zlib, struct
import numpy as np
import anim_fo4
import skyclip
from retarget import Fo4Skeleton, Resampled, SourcePose
from finalize import fo4_worlds

RED, BLUE, GREY, GREEN = (200, 40, 40), (60, 60, 220), (200, 200, 200), (100, 190, 100)
CAMERA = np.array([0.0, 0.0, 120.5])
FOV = np.radians(80)
NEAR = 5.0  # the first person near plane, game units
SKIP = ('Weapon', 'Camera', 'AnimObject', 'Pipboy', 'CamTarget', 'NPC Root', 'Eye', 'Root')
ARMS = ('Arm', 'Clavicle', 'UpperArm', 'Forearm', 'Hand', 'Finger')


class Img:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.a = np.full((h, w, 3), 255, np.uint8)

    def line(self, x0, y0, x1, y1, c, th=2, clip=None):
        """clip: (x0, y0, x1, y1) - only pixels inside are drawn (default: the whole image)"""
        cx0, cy0, cx1, cy1 = clip or (0, 0, self.w, self.h)
        n = min(int(max(abs(x1 - x0), abs(y1 - y0))) + 1, 4 * (self.w + self.h))
        for t in np.linspace(0, 1, n):
            x, y = int(x0 + (x1 - x0) * t), int(y0 + (y1 - y0) * t)
            if cx0 <= x < cx1 and cy0 <= y < cy1:
                self.a[max(cy0, y - th // 2):min(cy1, y + th - th // 2), max(cx0, x - th // 2):min(cx1, x + th - th // 2)] = c

    def rect(self, x0, y0, x1, y1, c):
        for a, b, cc, d in ((x0, y0, x1, y0), (x1, y0, x1, y1), (x1, y1, x0, y1), (x0, y1, x0, y0)):
            self.line(a, b, cc, d, c, 1)

    def save(self, p):
        raw = b''.join(b'\x00' + self.a[y].tobytes() for y in range(self.h))

        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0)) +
                            chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def edges(points, parents, arms_only):
    for n, p in parents.items():
        if n not in points or p not in points or any(k in n or k in p for k in SKIP):
            continue
        if arms_only and not (any(k in n for k in ARMS) and any(k in p for k in ARMS)):
            continue
        yield points[n], points[p]


def sheet(hkx, info_json, src_path, map_json, skel, out_png, first_person=False, ledge=None, trim=0.0, every=4, pitch=0.0):
    fo = Fo4Skeleton(skel)
    anim = anim_fo4.load_fo4_animation(hkx)
    info = json.load(open(info_json))
    src = SourcePose(Resampled(skyclip.load(src_path), trim), map_json, first_person)
    fo_parents = {n: fo.names[fo.parent[i]] for i, n in enumerate(fo.names) if fo.parent[i] >= 0}
    sky_parents = {src.tmap[i]: src.tmap[p] for i, p in src.parent.items() if p >= 0 and i in src.tmap and p in src.tmap}
    frames = list(range(0, anim.num_frames, every)) + [anim.num_frames - 1]

    if first_person:
        cols = 6
        rows = (len(frames) + cols - 1) // cols
        pw, ph = 300, 169
        img = Img(cols * pw, rows * ph)
        f_len = (pw / 2) / np.tan(FOV / 2)
        for k, fr in enumerate(frames):
            ox, oy = (k % cols) * pw, (k // cols) * ph
            img.rect(ox + 2, oy + 2, ox + pw - 3, oy + ph - 3, GREY)
            cx, cy = ox + pw / 2, oy + ph / 2

            def proj(v):
                d = v - CAMERA
                # look up by `pitch`: rotate the point down about the camera's x axis
                cp, sp = np.cos(np.radians(pitch)), np.sin(np.radians(pitch))
                d = np.array([d[0], d[1] * cp + d[2] * sp, -d[1] * sp + d[2] * cp])
                if d[1] < NEAR:
                    return None
                return cx + d[0] / d[1] * f_len, cy - d[2] / d[1] * f_len

            w = fo4_worlds(fo, anim, fr)
            fpts = {n: w[i][:3, 3] for i, n in enumerate(fo.names)}
            spts = {n: m[:3, 3] for n, m in src.worlds(fr).items()}
            for pts, par, col in ((spts, sky_parents, BLUE), (fpts, fo_parents, RED)):
                for a, b in edges(pts, par, True):
                    pa, pb = proj(a), proj(b)
                    if pa and pb:
                        img.line(*pa, *pb, col, 2, clip=(ox + 3, oy + 3, ox + pw - 3, oy + ph - 3))
        img.save(out_png)
        return

    W, H = 1600, 900
    img = Img(W, H)
    cw = W / len(frames)
    sc = min(cw / 110, 1.4)
    for row, view in enumerate(('side', 'front')):
        base = H * (0.47 if row == 0 else 0.95)
        for k, fr in enumerate(frames):
            mo = np.array(info['curve'][fr])
            ox = cw * k + cw * 0.3
            img.line(int(cw * k), int(base), int(cw * (k + 1)) - 4, int(base), GREY, 1)
            if ledge is not None:
                img.line(int(cw * k + cw * 0.5), int(base - ledge * sc), int(cw * (k + 1)) - 4, int(base - ledge * sc), GREEN, 2)
            w = fo4_worlds(fo, anim, fr)
            fpts = {n: w[i][:3, 3] + mo for i, n in enumerate(fo.names)}
            spts = {n: m[:3, 3] + mo for n, m in src.worlds(fr).items()}
            for pts, par, col, dx in ((fpts, fo_parents, RED, 0), (spts, sky_parents, BLUE, 25)):
                for a, b in edges(pts, par, False):
                    ax, bx = (a[1], b[1]) if view == 'side' else (a[0], b[0])
                    shift = -dx * sc if view == 'side' else (cw * 0.35 if dx else 0)
                    img.line(ox + ax * sc + shift, base - a[2] * sc, ox + bx * sc + shift, base - b[2] * sc, col, 2)
    img.save(out_png)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    for n in ('hkx', 'json', 'src', 'map', 'skeleton', 'png'):
        ap.add_argument(n)
    ap.add_argument('--first-person', action='store_true')
    ap.add_argument('--ledge', type=float)
    ap.add_argument('--trim', type=float, default=0.0)
    ap.add_argument('--every', type=int, default=4)
    ap.add_argument('--pitch', type=float, default=0.0, help='first person: degrees the camera looks up')
    a = ap.parse_args()
    sheet(a.hkx, a.json, a.src, a.map, a.skeleton, a.png, a.first_person, a.ledge, a.trim, a.every, a.pitch)
