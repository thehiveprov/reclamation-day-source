# Finds which Skyrim bone each track of a clip animates. The clips carry no track names and the Havok skeleton they
# were made for isn't available, so: every bone's local translation is a fixed offset from its parent (it barely
# moves in an animation), and rotations of mirrored bones (L vs R) differ. Each track is matched to the bone of
# PyNifly's Skyrim skeleton.nif (XPMSE) with the closest local translation, ties broken by the closest rest rotation.
# Parents: the nif hierarchy, skipping nodes that no track animates (XPMSE's CME helpers etc.), transforms composed.
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import skyclip
from pyn.pynifly import NifFile

NIF = os.path.join(skyclip.ADDON, 'skeletons', 'Skyrim', 'skeleton.nif')


def q_from_m(m):
    m = np.asarray(m, float)
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        return np.array([(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s])
    i = int(np.argmax([m[0, 0], m[1, 1], m[2, 2]]))
    if i == 0:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        return np.array([0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s])
    if i == 1:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        return np.array([(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s])
    s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
    return np.array([(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s])


def m_from_q(q):
    x, y, z, w = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def qangle(a, b):
    return 2 * np.arccos(min(1.0, abs(float(np.dot(a / np.linalg.norm(a), b / np.linalg.norm(b))))))


class SkyrimSkeleton:
    """The nif's nodes: name -> (parent name, local 4x4)."""

    def __init__(self):
        nif = NifFile(NIF)
        self.local, self.parent = {}, {}
        for name, node in nif.nodes.items():
            t = node.transform
            m = np.eye(4)
            m[:3, :3] = np.array([list(r) for r in t.rotation])
            m[:3, 3] = list(t.translation)
            m[:3, :3] *= t.scale
            self.local[name] = m
            self.parent[name] = node.parent.name if node.parent else None

    def chain(self, name):
        out = []
        while name:
            out.append(name)
            name = self.parent.get(name)
        return out

    def relative(self, name, ancestor):
        """name's transform relative to ancestor (composing the nodes between)."""
        m = np.eye(4)
        n = name
        while n and n != ancestor:
            m = self.local[n] @ m
            n = self.parent[n]
        return m


# Bones worth matching: the body. Weapons/skirt/magic/HDT nodes can't be mistaken for these and aren't retargeted.
def is_body(name):
    if name.startswith(('NPC ', 'x_NPC ')) or name in ('Camera3rd [Cam3]', 'NPCEyeBone', 'AnimObjectA', 'AnimObjectB'):
        return not any(w in name for w in ('MagicNode', 'Pauldron', 'Butt', 'Belly', 'Genitals', 'Breast', 'PreFront', 'Schlong', 'Thigh Front', 'RearThigh', 'FrontThigh', 'Tail'))
    return False


def map_tracks(clip, skel=None):
    """Havok skeletons list parents before children, so tracks are matched in order: a track's bone must have its
    nearest body ancestor already matched to an earlier track. Among those, closest translation, then rotation."""
    skel = skel or SkyrimSkeleton()
    names = [n for n in skel.local if is_body(n)]
    anc = {n: next((a for a in skel.chain(n)[1:] if is_body(a)), None) for n in names}
    rel = {n: skel.relative(n, anc[n]) for n in names}
    T = [np.mean(np.array(tr.translations), axis=0) for tr in clip.tracks]
    R = [np.array(tr.rotations) for tr in clip.tracks]
    track_bone, used = {0: 'NPC Root [Root]'}, {'NPC Root [Root]'}
    for i in range(1, clip.num_tracks):
        best = None
        for n in names:
            if n in used or anc[n] not in used:
                continue
            et = np.linalg.norm(rel[n][:3, 3] - T[i])
            if et > (15.0 if n == "NPC COM [COM ]" else 1.0):
                continue
            er = float(np.mean([qangle(q, q_from_m(rel[n][:3, :3])) for q in R[i]]))
            cost = et + 2.0 * er
            if best is None or cost < best[0]:
                best = (cost, n)
        if best:
            track_bone[i] = best[1]
            used.add(best[1])
    bone_track = {n: i for i, n in track_bone.items()}
    # Havok parent = nearest ancestor that has a track
    parent = {}
    for i, n in track_bone.items():
        parent[i] = next((bone_track[a] for a in skel.chain(n)[1:] if a in bone_track), -1)
    return track_bone, parent, skel


if __name__ == '__main__':
    clip = skyclip.load(sys.argv[1])
    tb, par, skel = map_tracks(clip)
    for i in sorted(tb):
        print(i, tb[i], '<-', tb.get(par[i], '-'))
    print(len(tb), 'of', clip.num_tracks, 'tracks mapped')
