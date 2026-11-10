# Retargets Realtime Pip-Boy Map's Check.kf (New Vegas, by Stentorious, xConfused and Hitman; used with permission) onto
# Fallout 4's first-person arms, relative to the first-person camera. Needs Blender 4.4+ with PyNifly enabled:
#   blender -b --python retarget_glance.py -- <out.json> <fo4 skeleton.hkx> <clip.json> [sides=L,R]
# Both arms: the left raises the Pip-Boy, the right hangs down out of view (the weapon hand, its weapon hidden meanwhile).
# <fo4 skeleton.hkx>: Meshes\Actors\Character\_1stPerson\CharacterAssets\skeleton.hkx from "Fallout4 - Animations.ba2".
# <clip.json>: the clip sampled by decode_kf.js (PyNifly can't read its B-spline compressed curves).
# Then: node make_header.js <out.json> Code/client/Games/Fallout4/PipboyGlanceAnimation.h
#
# Same bone map and per-bone maths as Tools/ConvertMantleAnimations/retarget.py (docs/ANIMATION_PIPELINE.md, 5.3-5.4):
# each Fallout 4 bone points the way its New Vegas bone points, with the same twist. Unlike the climbs, the arm is placed
# relative to the camera, not the chest: Fallout 4's first-person chest is posed differently from New Vegas's, and what
# matters here is where the Pip-Boy is in view. The collarbone sits where New Vegas's is relative to New Vegas's
# Camera1st, the arm hangs from it with Fallout 4's bone lengths. Output per frame and driven bone: its transform
# relative to the Fallout 4 first-person Camera bone (and, for comparison, relative to its parent).
import bpy, sys, os, json
from mathutils import Matrix, Vector, Quaternion

args = sys.argv[sys.argv.index('--') + 1:]
OUT, FO4_SKELETON, CLIP = args[0], args[1], args[2]
SIDES = args[3].split('=', 1)[1].split(',') if len(args) > 3 else ['L', 'R']

ADDON = os.path.join(os.environ['APPDATA'], 'Blender Foundation', 'Blender', '4.4', 'scripts', 'addons', 'io_scene_nifly')
NV_SKELETON = os.path.join(ADDON, 'skeletons', 'FONV', 'skeleton.nif')
NV_CAMERA = 'Camera1st'
FO_CAMERA = 'Camera'


def side_map(nv, fo):
    m = [
        (f'Bip01 {nv} Clavicle', f'{fo}Arm_Collarbone', f'{fo}Arm_UpperArm', f'Bip01 {nv} UpperArm'),
        (f'Bip01 {nv} UpperArm', f'{fo}Arm_UpperArm', f'{fo}Arm_ForeArm1', f'Bip01 {nv} Forearm'),
        (f'Bip01 {nv} Forearm', f'{fo}Arm_ForeArm1', f'{fo}Arm_Hand', f'Bip01 {nv} Hand'),
        (f'Bip01 {nv} Hand', f'{fo}Arm_Hand', f'{fo}Arm_Finger31', f'Bip01 {nv} Finger2'),
    ]
    for i, d in enumerate(['Thumb1', 'Finger1', 'Finger2', 'Finger3', 'Finger4']):
        joints = [f'Bip01 {nv} {d}', f'Bip01 {nv} {d}1', f'Bip01 {nv} {d}2']
        fo_joints = [f'{fo}Arm_Finger{i + 1}1', f'{fo}Arm_Finger{i + 1}2', f'{fo}Arm_Finger{i + 1}3']
        for j in range(3):
            m.append((joints[j], fo_joints[j], fo_joints[j + 1] if j < 2 else None, joints[j + 1] if j < 2 else None))
    return m


MAPS = {s: side_map(s, s) for s in ('L', 'R')}
FOREARM_CHAIN = ['ForeArm1', 'ForeArm2', 'ForeArm3']

# Each skeleton's names for the joints the roll is read from
NV_NAMES = {'upper': 'Bip01 {s} UpperArm', 'fore': 'Bip01 {s} Forearm', 'hand': 'Bip01 {s} Hand', 'index': 'Bip01 {s} Finger1', 'little': 'Bip01 {s} Finger4'}
FO_NAMES = {'upper': '{s}Arm_UpperArm', 'fore': '{s}Arm_ForeArm1', 'hand': '{s}Arm_Hand', 'index': '{s}Arm_Finger21', 'little': '{s}Arm_Finger51'}


def joint(transforms, names, key, side):
    return transforms[names[key].format(s=side)].translation


# The upper arm rolls with the elbow's bend (the way the forearm goes, square to the upper arm); the forearm and the
# hand with the knuckles (little finger to index finger)
def elbow_plane(side, transforms, names):
    upper, fore, hand = (joint(transforms, names, k, side) for k in ('upper', 'fore', 'hand'))
    d = (fore - upper).normalized()
    forearm = hand - fore
    return forearm - d * forearm.dot(d)


def knuckles(side, transforms, names):
    return joint(transforms, names, 'index', side) - joint(transforms, names, 'little', side)


TWIST = {'UpperArm': elbow_plane, 'ForeArm1': knuckles, 'Hand': knuckles}


def import_armature(op, **kw):
    before = {o for o in bpy.data.objects if o.type == 'ARMATURE'}
    op(**kw)
    return next(o for o in bpy.data.objects if o.type == 'ARMATURE' and o not in before)


def basis(direction, up):
    d = direction.normalized()
    u = (up - d * up.dot(d)).normalized()
    return Matrix((d, u, d.cross(u))).transposed()


def reference_axis(*directions):
    best, best_score = None, 2
    for axis in (Vector((0, 0, 1)), Vector((0, 1, 0)), Vector((1, 0, 0))):
        score = max(abs(axis.dot(d.normalized())) for d in directions)
        if score < best_score:
            best, best_score = axis, score
    return best


def walk_order(armature):
    order = []
    def walk(bone):
        order.append(bone.name)
        for child in bone.children:
            walk(child)
    for b in armature.data.bones:
        if not b.parent:
            walk(b)
    return order


bpy.ops.wm.read_homefile(use_empty=True)
nv = import_armature(bpy.ops.import_scene.pynifly, filepath=NV_SKELETON, blender_xf=False, rename_bones=False, rotate_bones_pretty=False)
fo = import_armature(bpy.ops.import_scene.pynifly_hkx, filepath=FO4_SKELETON, blender_xf=False, rename_bones=False, rotate_bones_pretty=False)
assert nv.matrix_world == Matrix.Identity(4) and fo.matrix_world == Matrix.Identity(4), 'armatures must sit at the origin'

fo_rest = {b.name: b.matrix_local.copy() for b in fo.data.bones}
nv_rest = {b.name: b.matrix_local.copy() for b in nv.data.bones}
fo_parent = {b.name: (b.parent.name if b.parent else None) for b in fo.data.bones}
nv_parent = {b.name: (b.parent.name if b.parent else None) for b in nv.data.bones}
fo_order, nv_order = walk_order(fo), walk_order(nv)

clip = json.load(open(CLIP))
# The Fallout 4 camera bone's axes against New Vegas's camera's: both look ahead (+Y) at rest
camera_axes = nv_rest[NV_CAMERA].to_3x3().inverted() @ fo_rest[FO_CAMERA].to_3x3()

frames = []
for f in range(clip['frames']):
    # New Vegas's pose, model space: the clip's local transforms down the hierarchy, rest for bones it doesn't key
    nv_pose = {}
    for name in nv_order:
        parent = nv_parent[name]
        rest_local = (nv_rest[parent].inverted() @ nv_rest[name]) if parent else nv_rest[name]
        if name in clip['bones']:
            t = clip['bones'][name][f]
            local = Matrix.Translation(Vector(t[:3])) @ Quaternion((t[3], t[4], t[5], t[6])).to_matrix().to_4x4()
        else:
            local = rest_local
        nv_pose[name] = (nv_pose[parent] @ local) if parent else local

    target_world_rot, anchor = {}, {}
    for side in SIDES:
        for nv_b, fo_b, fo_child, nv_child in MAPS[side]:
            if nv_b not in nv_pose or fo_b not in fo_rest:
                continue
            r_nv0, r_nv = nv_rest[nv_b].to_3x3(), nv_pose[nv_b].to_3x3()
            r_fo0 = fo_rest[fo_b].to_3x3()
            twist = TWIST.get(fo_b.split('_', 1)[1]) if '_' in fo_b else None
            if fo_child and nv_child and nv_child in nv_pose and twist:
                # the roll from the arm itself, the same way on both skeletons: the back of the wrist (where both
                # games' Pip-Boys sit) faces the same way in Fallout 4 as in New Vegas
                d_nv = nv_pose[nv_child].translation - nv_pose[nv_b].translation
                d_fo0 = fo_rest[fo_child].translation - fo_rest[fo_b].translation
                up_nv = twist(side, nv_pose, NV_NAMES)
                up_fo0 = twist(side, fo_rest, FO_NAMES)
                q = basis(d_nv, up_nv) @ basis(d_fo0, up_fo0).transposed()
            elif fo_child and nv_child and nv_child in nv_pose:
                d_nv0 = nv_rest[nv_child].translation - nv_rest[nv_b].translation
                d_nv = nv_pose[nv_child].translation - nv_pose[nv_b].translation
                d_fo0 = fo_rest[fo_child].translation - fo_rest[fo_b].translation
                ref = reference_axis(d_nv0, d_fo0)
                up_nv = (r_nv @ r_nv0.inverted()) @ ref
                q = basis(d_nv, up_nv) @ basis(d_fo0, ref).transposed()
            else:
                q = r_nv @ r_nv0.inverted()
            target_world_rot[fo_b] = q @ r_fo0
            if fo_b.endswith('Arm_ForeArm1'):
                for twist in FOREARM_CHAIN[1:]:
                    name = fo_b.replace('ForeArm1', twist)
                    target_world_rot[name] = q @ fo_rest[name].to_3x3()
            if fo_b.endswith('Arm_Collarbone'):
                anchor[fo_b] = nv_pose[nv_b].translation.copy()

    # Fallout 4's arm, model space: the collarbone at New Vegas's, the rest hanging from it with Fallout 4's lengths
    world = {}
    for name in fo_order:
        parent = fo_parent[name]
        rest_local = (fo_rest[parent].inverted() @ fo_rest[name]) if parent else fo_rest[name]
        w = (world[parent] @ rest_local) if parent else rest_local
        if name in target_world_rot:
            pos = anchor.get(name, w.translation)
            w = Matrix.Translation(pos) @ target_world_rot[name].to_4x4()
        world[name] = w

    # The camera, New Vegas's pose with Fallout 4's camera bone axes
    cam = Matrix.Translation(nv_pose[NV_CAMERA].translation) @ (nv_pose[NV_CAMERA].to_3x3() @ camera_axes).to_4x4()
    cam_inv = cam.inverted()
    bones = {}
    for name in [n for n in fo_order if n in target_world_rot]:  # parents first
        rel = cam_inv @ world[name]
        local = world[fo_parent[name]].inverted() @ world[name]
        bones[name] = {
            'camera': {'rotation': [[round(rel[r][c], 6) for c in range(3)] for r in range(3)], 'translation': [round(x, 4) for x in rel.translation]},
            'local': {'rotation': [[round(local[r][c], 6) for c in range(3)] for r in range(3)], 'translation': [round(x, 4) for x in local.translation]},
        }
    frames.append(bones)
    if f == clip['frames'] // 2:
        cam_nv_mid = nv_pose[NV_CAMERA].to_quaternion()
        pos_nv_mid = nv_pose[NV_CAMERA].translation.copy()
        def dorsal(transforms, names, to_camera):
            fore = (joint(transforms, names, 'hand', 'L') - joint(transforms, names, 'fore', 'L')).normalized()
            return (to_camera @ fore.cross(knuckles('L', transforms, names)).normalized())
        cam_rot_inv = cam.to_3x3().inverted()
        dorsal_nv = dorsal(nv_pose, NV_NAMES, cam_rot_inv)
        dorsal_fo = dorsal(world, FO_NAMES, cam_rot_inv)

# how the New Vegas camera sits in the clip (frame 30) against its rest
print('CAMERA', 'tilt from rest', round(nv_rest[NV_CAMERA].to_quaternion().rotation_difference(cam_nv_mid).angle * 57.2958, 1), 'deg', 'euler', [round(a * 57.2958, 1) for a in cam_nv_mid.to_euler()], 'pos', [round(x, 2) for x in pos_nv_mid])
# the back of the wrist (forearm direction x knuckle line) in camera space, both skeletons, frame 30: +z faces the camera
print('DORSAL', 'nv', [round(x, 3) for x in dorsal_nv], 'fo4', [round(x, 3) for x in dorsal_fo])
# where the Pip-Boy ends up in view, frame 30 (camera space: x right, y up, -z ahead)
mid = frames[len(frames) // 2]
for n in ('LArm_Collarbone', 'LArm_ForeArm3', 'LArm_Hand'):
    if n in mid:
        print('VIEW', n, mid[n]['camera']['translation'])
result = {'fps': clip['fps'], 'source': clip.get('source'), 'sides': SIDES, 'markers': clip['markers'], 'frames': frames}
with open(OUT, 'w') as file:
    json.dump(result, file)
print(f'CLIP frames {len(frames)} sides {SIDES} bones {len(frames[0])} markers {clip["markers"]}')
print('WROTE', OUT)
