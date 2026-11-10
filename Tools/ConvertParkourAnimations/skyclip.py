# Reads a Skyrim LE (hk_2010, 32-bit) animation: spline compressed through PyNifly's anim_skyrim, interleaved
# uncompressed (which PyNifly doesn't read) here. Returns PyNifly's AnimationData (tracks[i].translations/rotations
# per frame, rotations x y z w) plus annotations.
import os, struct, sys

ADDON = os.path.join(os.environ['APPDATA'], 'Blender Foundation', 'Blender', '4.4', 'scripts', 'addons', 'io_scene_nifly')
sys.path.insert(0, ADDON)
sys.path.insert(0, os.path.join(ADDON, 'hkx'))
import anim_skyrim as sk
from anim_fo4 import AnimationData, Annotation, TrackData


def _interleaved(data):
    ptr = data[0x10]
    sections = sk._parse_hkx_sections(data)
    cn, ds = sections['__classnames__'], sections['__data__']
    fixups = sk._parse_local_fixups(data, ds)
    objects = sk._parse_virtual_fixups(data, ds, cn['offset'])
    rel = next((r for r, c in objects if c == 'hkaInterleavedUncompressedAnimation'), None)
    if rel is None:
        return None
    base = ds['offset']
    arr = ptr + 8
    o = 2 * ptr
    anim = AnimationData()
    anim.duration = sk._f32(data, base + rel + o + 4)
    anim.num_tracks = sk._u32(data, base + rel + o + 8)
    o_ann = o + 16 + ptr
    # annotations: same layout as in the spline reader
    cnt = sk._u32(data, base + rel + o_ann + ptr)
    content = fixups.get(rel + o_ann)
    if content is not None:
        for i in range(cnt):
            tr = content + i * (ptr + arr)
            name_rel = fixups.get(tr)
            anim.bone_names.append(sk._read_null_string(data, base + name_rel) if name_rel is not None else '')
            ev_cnt = sk._u32(data, base + tr + ptr + ptr)
            ev = fixups.get(tr + ptr)
            if ev is not None:
                for j in range(ev_cnt):
                    e = ev + j * 2 * ptr
                    srel = fixups.get(e + ptr)
                    anim.annotations.append(Annotation(time=sk._f32(data, base + e), text=sk._read_null_string(data, base + srel) if srel is not None else ''))
    o_tr = o_ann + arr
    n = sk._u32(data, base + rel + o_tr + ptr)
    t_rel = fixups[rel + o_tr]
    frames = n // anim.num_tracks
    anim.num_frames = frames
    anim.frame_duration = anim.duration / max(1, frames - 1)
    anim.tracks = [TrackData() for _ in range(anim.num_tracks)]
    for f in range(frames):
        for t in range(anim.num_tracks):
            q = base + t_rel + (f * anim.num_tracks + t) * 48
            v = struct.unpack_from('<12f', data, q)
            anim.tracks[t].translations.append(list(v[0:3]))
            anim.tracks[t].rotations.append(list(v[4:8]))
            anim.tracks[t].scales.append(list(v[8:11]))
    return anim


def load(path):
    data = open(path, 'rb').read()
    a = _interleaved(data)
    if a is None:
        a = sk.load_skyrim_animation(path)
    if not a.frame_duration:
        a.frame_duration = a.duration / max(1, a.num_frames - 1)
    return a


def root_motion(a):
    """animmotion annotations -> [(time, x, y, z)] (Skyrim's extracted root motion, model space of the start)."""
    out = []
    for e in a.annotations:
        p = e.text.split()
        if p and p[0] == 'animmotion':
            out.append((e.time, float(p[1]), float(p[2]), float(p[3])))
    return sorted(out)


if __name__ == '__main__':
    for p in sys.argv[1:]:
        a = load(p)
        rm = root_motion(a)
        ev = sorted({e.text for e in a.annotations if not e.text.startswith('animmotion')})
        print(f'{os.path.basename(p):34s} {a.duration:.3f}s {a.num_frames} frames {a.num_tracks} tracks, motion {len(rm)} keys end {rm[-1] if rm else None}, events {ev}')
