# Retargets the Mantle for Fallout: New Vegas climbs by STRADAT (first-person arm animations, NV Nexus mod 99083) onto
# Fallout 4's first-person skeleton. Needs Blender 4.4+ with PyNifly (github.com/BadDogSkyrim/PyNifly) enabled:
#   blender -b --python retarget.py -- <out.json> <fo4 skeleton.hkx> <name>=<kf> [<name>=<kf> ...]
# <fo4 skeleton.hkx>: Meshes\Actors\Character\_1stPerson\CharacterAssets\skeleton.hkx from "Fallout4 - Animations.ba2".
# The climbs: meshes\characters\_male\idleanims\Mantle\{Base,Sprint,Low Obstacle}\SpecialIdle_ClimbUp.kf of the mod.
# Then: node make_header.js <out.json> Code/client/Games/Fallout4/MantleGrabAnimations.h
# Both armatures are imported without PyNifly's cosmetic bone rotation, so a bone's matrix is the game node's transform
# (armature space = the game's model space). Output: per frame, each Fallout 4 bone's local transform (relative to its
# parent), as the game's NiTransform wants it.
import bpy, sys, os, json, math
from mathutils import Matrix, Vector, Quaternion

args = sys.argv[sys.argv.index('--') + 1:]
OUT = args[0]
FO4_SKELETON = args[1]
CLIPS = [a.split('=', 1) for a in args[2:]]
FPS = 30

ADDON = os.path.join(os.environ['APPDATA'], 'Blender Foundation', 'Blender', '4.4', 'scripts', 'addons', 'io_scene_nifly')
NV_SKELETON = os.path.join(ADDON, 'skeletons', 'FONV', 'skeleton.nif')

# New Vegas bone -> Fallout 4 bone, per side; the child that gives each bone its direction (FO4 name, NV name)
def side_map(nv, fo):
    m = [
        (f'Bip01 {nv} Clavicle', f'{fo}Arm_Collarbone', f'{fo}Arm_UpperArm', f'Bip01 {nv} UpperArm'),
        (f'Bip01 {nv} UpperArm', f'{fo}Arm_UpperArm', f'{fo}Arm_ForeArm1', f'Bip01 {nv} Forearm'),
        (f'Bip01 {nv} Forearm', f'{fo}Arm_ForeArm1', f'{fo}Arm_Hand', f'Bip01 {nv} Hand'),
        (f'Bip01 {nv} Hand', f'{fo}Arm_Hand', f'{fo}Arm_Finger31', f'Bip01 {nv} Finger2'),
    ]
    # Fingers: NV Thumb/Finger1-4 (3 joints each) -> FO4 Finger1x (thumb) .. Finger5x (little)
    nv_digits = ['Thumb1', 'Finger1', 'Finger2', 'Finger3', 'Finger4']  # joints: X, X1, X2
    for i, d in enumerate(nv_digits):
        joints = [f'Bip01 {nv} {d}', f'Bip01 {nv} {d}1', f'Bip01 {nv} {d}2']
        fo_joints = [f'{fo}Arm_Finger{i + 1}1', f'{fo}Arm_Finger{i + 1}2', f'{fo}Arm_Finger{i + 1}3']
        for j in range(3):
            child_fo = fo_joints[j + 1] if j < 2 else None
            child_nv = joints[j + 1] if j < 2 else None
            m.append((joints[j], fo_joints[j], child_fo, child_nv))
    return m

MAPS = {'L': side_map('L', 'L'), 'R': side_map('R', 'R')}
# The forearm's twist bones follow the forearm (FO4 spreads the twist over ForeArm1-3)
FOREARM_CHAIN = ['ForeArm1', 'ForeArm2', 'ForeArm3']


def import_armature(op, **kw):
    before = {o for o in bpy.data.objects if o.type == 'ARMATURE'}
    op(**kw)
    return next(o for o in bpy.data.objects if o.type == 'ARMATURE' and o not in before)


def basis(direction, up):
    d = direction.normalized()
    u = (up - d * up.dot(d)).normalized()
    return Matrix((d, u, d.cross(u))).transposed()  # columns: d, u, d x u


def reference_axis(*directions):
    # The world axis least aligned with the bone (in both skeletons), for the twist
    best, best_score = None, 2
    for axis in (Vector((0, 0, 1)), Vector((0, 1, 0)), Vector((1, 0, 0))):
        score = max(abs(axis.dot(d.normalized())) for d in directions)
        if score < best_score:
            best, best_score = axis, score
    return best


bpy.ops.wm.read_homefile(use_empty=True)
bpy.context.scene.render.fps = FPS
nv = import_armature(bpy.ops.import_scene.pynifly, filepath=NV_SKELETON, blender_xf=False, rename_bones=False, rotate_bones_pretty=False)
fo = import_armature(bpy.ops.import_scene.pynifly_hkx, filepath=FO4_SKELETON, blender_xf=False, rename_bones=False, rotate_bones_pretty=False)
assert nv.matrix_world == Matrix.Identity(4) and fo.matrix_world == Matrix.Identity(4), 'armatures must sit at the origin'

fo_rest = {b.name: b.matrix_local.copy() for b in fo.data.bones}
nv_rest = {b.name: b.matrix_local.copy() for b in nv.data.bones}
fo_parent = {b.name: (b.parent.name if b.parent else None) for b in fo.data.bones}
fo_order = []
def walk(bone):
    fo_order.append(bone.name)
    for child in bone.children:
        walk(child)
for b in fo.data.bones:
    if not b.parent:
        walk(b)

result = {'fps': FPS, 'clips': {}}
for clip_name, kf in CLIPS:
    bpy.context.view_layer.objects.active = nv
    nv.select_set(True)
    if nv.animation_data:
        nv.animation_data.action = None
    d, f = os.path.split(kf)
    bpy.ops.import_scene.pynifly_kf(directory=d, files=[{'name': f}], blender_xf=False, rename_bones=False)
    act = nv.animation_data.action
    first, last = int(act.frame_range[0]), int(act.frame_range[1])

    # The sides this clip actually moves (a still arm is left to the game)
    moving = set()
    for fc in act.fcurves:
        vals = [k.co[1] for k in fc.keyframe_points]
        if vals and max(vals) - min(vals) > 0.01 and '"' in fc.data_path:
            bone = fc.data_path.split('"')[1]
            for side in ('L', 'R'):
                if bone.startswith(f'Bip01 {side} ') or bone == f'Bip01 {side}UpArmTwistBone':
                    moving.add(side)
    sides = sorted(moving)

    frames = []
    for frame in range(first, last + 1):
        bpy.context.scene.frame_set(frame)
        nv_pose = {pb.name: pb.matrix.copy() for pb in nv.pose.bones}
        target_world_rot = {}
        collar_offset = {}
        for side in sides:
            for nv_b, fo_b, fo_child, nv_child in MAPS[side]:
                if nv_b not in nv_pose or fo_b not in fo_rest:
                    continue
                r_nv0, r_nv = nv_rest[nv_b].to_3x3(), nv_pose[nv_b].to_3x3()
                r_fo0 = fo_rest[fo_b].to_3x3()
                if fo_child and nv_child and nv_child in nv_pose:
                    d_nv0 = nv_rest[nv_child].translation - nv_rest[nv_b].translation
                    d_nv = nv_pose[nv_child].translation - nv_pose[nv_b].translation
                    d_fo0 = fo_rest[fo_child].translation - fo_rest[fo_b].translation
                    ref = reference_axis(d_nv0, d_fo0)
                    # The NV bone's twist reference, carried by its rotation since rest
                    up_nv = (r_nv @ r_nv0.inverted()) @ ref
                    q = basis(d_nv, up_nv) @ basis(d_fo0, ref).transposed()
                else:
                    # Fingertips: the NV rotation since rest
                    q = r_nv @ r_nv0.inverted()
                target_world_rot[fo_b] = q @ r_fo0
                if fo_b.endswith('Arm_ForeArm1'):
                    for twist in FOREARM_CHAIN[1:]:
                        name = fo_b.replace('ForeArm1', twist)
                        target_world_rot[name] = q @ fo_rest[name].to_3x3()
                if fo_b.endswith('Arm_Collarbone'):
                    collar_offset[fo_b] = nv_pose[nv_b].translation - nv_rest[nv_b].translation

        # FO4 world transforms down the hierarchy (bones we don't drive keep their rest local transform), then locals
        world = {}
        for name in fo_order:  # parents first
            b = fo.data.bones[name]
            parent = fo_parent[b.name]
            rest_local = (fo_rest[parent].inverted() @ fo_rest[b.name]) if parent else fo_rest[b.name]
            parent_world = world[parent] if parent else Matrix.Identity(4)
            w = parent_world @ rest_local
            if b.name in target_world_rot:
                pos = w.translation + collar_offset.get(b.name, Vector())
                w = Matrix.Translation(pos) @ target_world_rot[b.name].to_4x4()
            world[b.name] = w
        driven = sorted(target_world_rot)
        bones = {}
        for name in driven:
            parent = fo_parent[name]
            local = world[parent].inverted() @ world[name]
            entry = {'rotation': [[round(local[r][c], 6) for c in range(3)] for r in range(3)]}
            if name in collar_offset:
                entry['translation'] = [round(x, 4) for x in local.translation]
            bones[name] = entry
        frames.append(bones)

    markers = sorted((m.frame, m.name) for m in act.pose_markers)
    result['clips'][clip_name] = {'sides': sides, 'frames': frames, 'markers': [[f - first, n] for f, n in markers]}
    print(f'CLIP {clip_name}: frames {len(frames)} sides {sides} bones {len(frames[0]) if frames else 0} markers {markers}')

with open(OUT, 'w') as file:
    json.dump(result, file)
print('WROTE', OUT)
