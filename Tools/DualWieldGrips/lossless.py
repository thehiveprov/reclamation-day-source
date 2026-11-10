# Decodes Fallout 4's hkaLosslessCompressedAnimation (hk_2014 packfiles, 64-bit), worked out from the files
# (2026-10-24): the object (0xE0 bytes) is hkaAnimation (+0x14 duration, +0x18 transform tracks, +0x28 annotation tracks,
# their names the bones) then hkArrays {ptr, size, capacity}: +0x38 dynamic translations (float), +0x48 static
# translations (float), +0x58 translation codes (4 x uint16 a track: x, y, z, pad), +0x68 dynamic rotations
# (quaternion x y z w), +0x78 static rotations, +0x88 rotation codes (uint16 a track), +0x98 / +0xA8 / +0xB8 the same
# for scales, +0xC8 float tracks, +0xD8 the frame count. A code's low 2 bits: 0 nothing (0 / identity / 1), 1 static,
# 2 dynamic; the rest an index (a static array's, or a dynamic one's per frame: LAYOUT below).
import sys, os, struct
sys.path.insert(0, sys.argv[1])
import anim_fo4 as A

LAYOUT = os.environ.get('LAYOUT', 'frame')  # 'frame': dyn[frame * count + index]; 'track': dyn[index * frames + frame]

def decode(path):
    data = open(path, 'rb').read()
    secs = A._parse_hkx_sections(data)
    ds, cn = secs['__data__'], secs['__classnames__']
    fix = A._parse_local_fixups(data, ds)
    objs = A._parse_virtual_fixups(data, ds, cn['offset'])
    base = ds['offset']
    rel = next(r for r, c in objs if c == 'hkaLosslessCompressedAnimation')
    u32 = lambda o: struct.unpack_from('<I', data, base + o)[0]
    f32 = lambda o: struct.unpack_from('<f', data, base + o)[0]
    u16 = lambda o: struct.unpack_from('<H', data, base + o)[0]
    def arr(off):
        return fix.get(rel + off), u32(rel + off + 8)
    duration = f32(rel + 0x14)
    tracks = u32(rel + 0x18)
    frames = u32(rel + 0xD8)
    names = []
    ann_ptr, ann_n = arr(0x28)
    for i in range(ann_n):
        p = fix.get(ann_ptr + i * 24)
        names.append(A._read_null_string(data, base + p) if p is not None else '')
    dynT, nDynT = arr(0x38); statT, _ = arr(0x48); codeT, _ = arr(0x58)
    dynR, nDynR = arr(0x68); statR, _ = arr(0x78); codeR, _ = arr(0x88)
    dynS, nDynS = arr(0x98); statS, _ = arr(0xA8); codeS, _ = arr(0xB8)
    perT = nDynT // frames if frames else 0
    perR = nDynR // frames if frames else 0
    perS = nDynS // frames if frames else 0
    def dyn_index(i, f, per):
        return f * per + i if LAYOUT == 'frame' else i * frames + f
    out = []
    for t in range(tracks):
        tr = {'name': names[t] if t < len(names) else str(t), 't': [], 'q': [], 'kinds': ''}
        tcodes = [u16(codeT + t * 8 + k * 2) for k in range(3)]
        rcode = u16(codeR + t * 2)
        tr['kinds'] = ''.join('-sd?'[c & 3] for c in tcodes) + '/' + '-sd?'[rcode & 3]
        for f in range(frames):
            v = []
            for c in tcodes:
                kind, idx = c & 3, c >> 2
                v.append(0.0 if kind == 0 else f32(statT + idx * 4) if kind == 1 else f32(dynT + dyn_index(idx, f, perT) * 4))
            tr['t'].append(v)
            kind, idx = rcode & 3, rcode >> 2
            if kind == 0:
                q = [0.0, 0.0, 0.0, 1.0]
            elif kind == 1:
                q = list(struct.unpack_from('<4f', data, base + statR + idx * 16))
            else:
                q = list(struct.unpack_from('<4f', data, base + dynR + dyn_index(idx, f, perR) * 16))
            tr['q'].append(q)
        out.append(tr)
    return {'duration': duration, 'frames': frames, 'tracks': out, 'dyn': (perT, perR, perS)}

if __name__ == '__main__':
    import json
    skel = A.load_fo4_skeleton(sys.argv[2])
    rest = {n: skel.reference_pose[i] for i, n in enumerate(skel.bones)}
    for path in sys.argv[3:]:
        a = decode(path)
        print(os.path.basename(os.path.dirname(path)), 'frames', a['frames'], 'dynamic per frame (t, r, s)', a['dyn'])
        # each dynamic track's frame-to-frame jump (small for the right layout) and the clip against the rest pose
        jump = 0.0
        for tr in a['tracks']:
            for f in range(1, a['frames']):
                jump = max(jump, max(abs(x - y) for x, y in zip(tr['q'][f], tr['q'][f - 1])))
        print('  largest rotation jump between frames', round(jump, 4))
        for tr in a['tracks']:
            if tr['name'] in ('Root', 'COM', 'RArm_Hand', 'Weapon', 'RArm_Finger21', 'RArm_Finger22', 'LArm_Hand', 'WeaponLeft', 'Camera'):
                r = rest.get(tr['name'])
                print(f"  {tr['name']:14} {tr['kinds']:6} t0 {[round(x, 3) for x in tr['t'][0]]} q0 {[round(x, 3) for x in tr['q'][0]]}   rest t {[round(x, 3) for x in r.translation] if r else '-'} q {[round(x, 3) for x in r.rotation] if r else '-'}")
