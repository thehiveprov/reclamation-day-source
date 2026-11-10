#include <Games/Fallout4/PipboyGlancePose.h>

#include <Games/Fallout4/PipboyGlanceAnimation.h>
#include <Games/Fallout4/DualWieldPose.h>
#include <PlayerCharacter.h>
#include <PerfProbe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>

namespace
{
using Clock = std::chrono::steady_clock;

// The original's blend in and out (ActivateAnim / DeactivateAnim with 0.4)
constexpr float kBlendIn = 0.4f;
constexpr float kBlendOut = 0.4f;

// The clip's bones that are posed: both arms, from the collarbone to the hand - the left raises the Pip-Boy, the right
// (the weapon hand, its weapon hidden meanwhile) hangs down out of view, as in the original. The fingers keep the game's
// own animation, carried by the hand: New Vegas's finger joints don't line up with Fallout 4's, and posed from the clip
// some bent sideways at the knuckles (seen in game).
bool IsPosedBone(std::string_view aName) noexcept
{
    return aName.find("_Finger") == std::string_view::npos;
}

// PlayerCharacter::firstPerson3D (CommonLibF4, 1.10.163)
constexpr uintptr_t kFirstPerson3DOffset = 0xB78;
// NiAVObject (CommonLibF4, 1.10.163): parent at 0x28, the local transform at 0x30 and the world one at 0x70 (each a
// rotation of 3 rows of 4 floats, the translation at +0x30, the scale at +0x3C). NiNode's children at 0x120: the
// array at 0x128, how many slots are in use at 0x132 (some may be empty). IsNode is vtable slot 4.
constexpr uintptr_t kParentOffset = 0x28;
constexpr uintptr_t kLocalOffset = 0x30;
constexpr uintptr_t kWorldOffset = 0x70;
constexpr uintptr_t kChildrenOffset = 0x128;
constexpr uintptr_t kChildCountOffset = 0x132;

using Quaternion = std::array<float, 4>;

// A node's transform, as NiTransform keeps it: the rotation stored transposed, composed local first (world = local *
// parent for the rotation as stored), a translation moved by the parent as a row vector (docs/ANIMATION_PIPELINE.md,
// 6.3; the same as Mantle's pose had it)
struct Transform
{
    float rotation[3][3]{};
    float translation[3]{};
    float scale{1.f};
};

Transform ReadTransform(const uint8_t* apNode, uintptr_t aOffset) noexcept
{
    const auto* pFloats = reinterpret_cast<const float*>(apNode + aOffset);
    Transform transform{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            transform.rotation[row][column] = pFloats[row * 4 + column];
    for (int axis = 0; axis < 3; ++axis)
        transform.translation[axis] = pFloats[12 + axis];
    transform.scale = pFloats[15];
    return transform;
}

void WriteTransform(uint8_t* apNode, uintptr_t aOffset, const Transform& acTransform) noexcept
{
    auto* pFloats = reinterpret_cast<float*>(apNode + aOffset);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            pFloats[row * 4 + column] = acTransform.rotation[row][column];
    for (int axis = 0; axis < 3; ++axis)
        pFloats[12 + axis] = acTransform.translation[axis];
    pFloats[15] = acTransform.scale;
}

// The world transform of a node with this local transform under this parent, as the game works it out
Transform Combine(const Transform& acParent, const Transform& acLocal) noexcept
{
    Transform result{};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            float sum = 0.f;
            for (int k = 0; k < 3; ++k)
                sum += acLocal.rotation[row][k] * acParent.rotation[k][column];
            result.rotation[row][column] = sum;
        }
    }
    for (int column = 0; column < 3; ++column)
    {
        float moved = 0.f;
        for (int k = 0; k < 3; ++k)
            moved += acLocal.translation[k] * acParent.rotation[k][column];
        result.translation[column] = acParent.translation[column] + acParent.scale * moved;
    }
    result.scale = acParent.scale * acLocal.scale;
    return result;
}

// The local transform that puts a node with this world transform under this parent (the inverse of Combine)
Transform Relative(const Transform& acParent, const Transform& acWorld) noexcept
{
    const float parentScale = acParent.scale != 0.f ? acParent.scale : 1.f;
    Transform result{};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            float sum = 0.f;
            for (int k = 0; k < 3; ++k)
                sum += acWorld.rotation[row][k] * acParent.rotation[column][k];
            result.rotation[row][column] = sum;
        }
    }
    for (int column = 0; column < 3; ++column)
    {
        float moved = 0.f;
        for (int k = 0; k < 3; ++k)
            moved += (acWorld.translation[k] - acParent.translation[k]) * acParent.rotation[column][k];
        result.translation[column] = moved / parentScale;
    }
    result.scale = acWorld.scale / parentScale;
    return result;
}

void Transpose(float (&aRows)[3][3]) noexcept
{
    std::swap(aRows[0][1], aRows[1][0]);
    std::swap(aRows[0][2], aRows[2][0]);
    std::swap(aRows[1][2], aRows[2][1]);
}

// A rotation's quaternion (w, x, y, z) from its rows (column vectors), and back
Quaternion FromRows(const float (&acRows)[3][3]) noexcept
{
    const float trace = acRows[0][0] + acRows[1][1] + acRows[2][2];
    Quaternion q{};
    if (trace > 0.f)
    {
        const float s = std::sqrt(trace + 1.f) * 2.f;
        q = {0.25f * s, (acRows[2][1] - acRows[1][2]) / s, (acRows[0][2] - acRows[2][0]) / s, (acRows[1][0] - acRows[0][1]) / s};
    }
    else if (acRows[0][0] > acRows[1][1] && acRows[0][0] > acRows[2][2])
    {
        const float s = std::sqrt(1.f + acRows[0][0] - acRows[1][1] - acRows[2][2]) * 2.f;
        q = {(acRows[2][1] - acRows[1][2]) / s, 0.25f * s, (acRows[0][1] + acRows[1][0]) / s, (acRows[0][2] + acRows[2][0]) / s};
    }
    else if (acRows[1][1] > acRows[2][2])
    {
        const float s = std::sqrt(1.f + acRows[1][1] - acRows[0][0] - acRows[2][2]) * 2.f;
        q = {(acRows[0][2] - acRows[2][0]) / s, (acRows[0][1] + acRows[1][0]) / s, 0.25f * s, (acRows[1][2] + acRows[2][1]) / s};
    }
    else
    {
        const float s = std::sqrt(1.f + acRows[2][2] - acRows[0][0] - acRows[1][1]) * 2.f;
        q = {(acRows[1][0] - acRows[0][1]) / s, (acRows[0][2] + acRows[2][0]) / s, (acRows[1][2] + acRows[2][1]) / s, 0.25f * s};
    }
    return q;
}

void ToRows(const Quaternion& acQ, float (&aRows)[3][3]) noexcept
{
    const float w = acQ[0], x = acQ[1], y = acQ[2], z = acQ[3];
    aRows[0][0] = 1.f - 2.f * (y * y + z * z);
    aRows[0][1] = 2.f * (x * y - w * z);
    aRows[0][2] = 2.f * (x * z + w * y);
    aRows[1][0] = 2.f * (x * y + w * z);
    aRows[1][1] = 1.f - 2.f * (x * x + z * z);
    aRows[1][2] = 2.f * (y * z - w * x);
    aRows[2][0] = 2.f * (x * z - w * y);
    aRows[2][1] = 2.f * (y * z + w * x);
    aRows[2][2] = 1.f - 2.f * (x * x + y * y);
}

// The shorter way from a to b, a fraction t along
Quaternion Slerp(const Quaternion& acA, const Quaternion& acB, float aT) noexcept
{
    Quaternion b = acB;
    float dot = acA[0] * b[0] + acA[1] * b[1] + acA[2] * b[2] + acA[3] * b[3];
    if (dot < 0.f)
    {
        dot = -dot;
        for (float& c : b)
            c = -c;
    }
    float wa = 1.f - aT;
    float wb = aT;
    if (dot < 0.9995f)
    {
        const float angle = std::acos(std::min(dot, 1.f));
        const float sine = std::sin(angle);
        wa = std::sin((1.f - aT) * angle) / sine;
        wb = std::sin(aT * angle) / sine;
    }
    Quaternion q{};
    float length = 0.f;
    for (int i = 0; i < 4; ++i)
    {
        q[i] = wa * acA[i] + wb * b[i];
        length += q[i] * q[i];
    }
    length = std::sqrt(length);
    if (length > 0.f)
        for (float& c : q)
            c /= length;
    return q;
}

bool IsSameTransform(const Transform& acA, const Transform& acB) noexcept
{
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            if (std::abs(acA.rotation[row][column] - acB.rotation[row][column]) > 1e-5f)
                return false;
    for (int axis = 0; axis < 3; ++axis)
        if (std::abs(acA.translation[axis] - acB.translation[axis]) > 1e-4f)
            return false;
    return true;
}

uint8_t* GetParent(const uint8_t* apNode) noexcept
{
    return *reinterpret_cast<uint8_t* const*>(apNode + kParentOffset);
}

// The node's children when it's a node (NiObject::IsNode, slot 4), else none
template <class TVisit> void ForEachChild(uint8_t* apObject, const TVisit& acVisit) noexcept
{
    using TIsNode = uint8_t* (*)(uint8_t*);
    uint8_t* pNode = (*reinterpret_cast<TIsNode* const*>(apObject))[4](apObject);
    if (!pNode)
        return;
    auto* const* pChildren = *reinterpret_cast<uint8_t* const* const*>(pNode + kChildrenOffset);
    const uint16_t count = *reinterpret_cast<const uint16_t*>(pNode + kChildCountOffset);
    if (!pChildren)
        return;
    for (uint16_t i = 0; i < count; ++i)
        if (pChildren[i])
            acVisit(pChildren[i]);
}

uint8_t* GetObjectByName(uint8_t* apNode, const BSFixedString& acName) noexcept
{
    using TGetObjectByName = uint8_t* (*)(uint8_t*, const BSFixedString&);
    return (*reinterpret_cast<TGetObjectByName* const*>(apNode))[0x2E](apNode, acName);
}

// What the hook works with, set up on the main thread (Start)
struct Pose
{
    std::mutex lock;
    std::atomic<bool> posing{false};
    std::atomic<bool> wanted{false};
    std::atomic<float> blend{0.f};
    std::atomic<float> time{0.f};
    uint8_t* pRoot{nullptr};
    uint8_t* pCamera{nullptr};
    std::array<uint8_t*, PipboyGlanceAnimation::kBoneCount> bones{};
    // per bone, the game's local before the pose and the local the pose left (a second update in the same frame
    // starts from the game's again)
    std::array<Transform, PipboyGlanceAnimation::kBoneCount> baseLocals{};
    std::array<Transform, PipboyGlanceAnimation::kBoneCount> writtenLocals{};
    std::array<bool, PipboyGlanceAnimation::kBoneCount> hasWritten{};
    Clock::time_point lastUpdate{};
};
Pose s_pose;

int BoneIndex(const uint8_t* apNode) noexcept
{
    for (int i = 0; i < PipboyGlanceAnimation::kBoneCount; ++i)
        if (s_pose.bones[i] == apNode)
            return i;
    return -1;
}

// The clip's transform for a bone at a time, relative to the camera, as NiTransform keeps it (the last frame holds)
Transform ClipKey(int aBone, float aTime) noexcept
{
    using namespace PipboyGlanceAnimation;
    const float frame = std::min(aTime * kFps, static_cast<float>(kFrameCount - 1));
    const int frame0 = static_cast<int>(frame);
    const int frame1 = std::min(frame0 + 1, kFrameCount - 1);
    const float t = frame - static_cast<float>(frame0);
    const Key& key0 = kKeys[frame0 * kBoneCount + aBone];
    const Key& key1 = kKeys[frame1 * kBoneCount + aBone];
    Quaternion q0{key0.rotation[0], key0.rotation[1], key0.rotation[2], key0.rotation[3]};
    Quaternion q1{key1.rotation[0], key1.rotation[1], key1.rotation[2], key1.rotation[3]};
    Transform key{};
    ToRows(Slerp(q0, q1, t), key.rotation);
    Transpose(key.rotation);
    for (int axis = 0; axis < 3; ++axis)
        key.translation[axis] = key0.translation[axis] + (key1.translation[axis] - key0.translation[axis]) * t;
    key.scale = 1.f;
    return key;
}

// Once the first person bone nodes have this frame's world transforms, before the bone tree takes them: each arm bone,
// parents first, goes where the clip has it relative to the camera, by the blend; everything under a posed bone (the
// Pip-Boy, fingers) keeps where it sits on its bone
void PoseArm(uint8_t* apRoot) noexcept
{
    std::unique_lock lock(s_pose.lock, std::try_to_lock);
    if (!lock.owns_lock() || apRoot != s_pose.pRoot || !s_pose.pCamera)
        return;

    const auto now = Clock::now();
    const float delta = s_pose.lastUpdate == Clock::time_point{} ? 0.f : std::min(std::chrono::duration<float>(now - s_pose.lastUpdate).count(), 0.1f);
    s_pose.lastUpdate = now;
    s_pose.time = s_pose.time + delta;
    float blend = s_pose.blend;
    blend = s_pose.wanted ? std::min(1.f, blend + delta / kBlendIn) : std::max(0.f, blend - delta / kBlendOut);
    s_pose.blend = blend;
    if (blend <= 0.f)
    {
        if (!s_pose.wanted)
            s_pose.posing = false;
        return;
    }
    // ease in and out
    const float weight = blend * blend * (3.f - 2.f * blend);

    const Transform camera = ReadTransform(s_pose.pCamera, kWorldOffset);

    struct Moved
    {
        Transform OldWorld;
        Transform NewWorld;
    };
    std::unordered_map<uint8_t*, Moved> moved;
    moved.reserve(128);
    std::vector<uint8_t*> pending;
    for (uint8_t* pBone : s_pose.bones)
    {
        if (!pBone)
            continue;
        bool posedAbove = false;
        for (uint8_t* pAbove = GetParent(pBone); pAbove && !posedAbove; pAbove = GetParent(pAbove))
            posedAbove = BoneIndex(pAbove) >= 0;
        if (!posedAbove)
            pending.push_back(pBone);
    }

    while (!pending.empty())
    {
        uint8_t* pNode = pending.back();
        pending.pop_back();
        uint8_t* pParent = GetParent(pNode);
        if (!pParent || moved.contains(pNode))
            continue;

        Transform parentOld{};
        Transform parentNew{};
        if (const auto it = moved.find(pParent); it != moved.end())
        {
            parentOld = it->second.OldWorld;
            parentNew = it->second.NewWorld;
        }
        else
        {
            parentOld = parentNew = ReadTransform(pParent, kWorldOffset);
        }

        const Transform oldWorld = ReadTransform(pNode, kWorldOffset);
        Transform local = Relative(parentOld, oldWorld);

        const int index = BoneIndex(pNode);
        if (index >= 0)
        {
            if (s_pose.hasWritten[index] && IsSameTransform(local, s_pose.writtenLocals[index]))
                local = s_pose.baseLocals[index];
            else
                s_pose.baseLocals[index] = local;

            // where the clip has the bone: the camera's world with the key on it, then as a local under the parent
            Transform target = Combine(camera, ClipKey(index, s_pose.time));
            target.scale = oldWorld.scale;
            const Transform targetLocal = Relative(parentNew, target);

            Transform gameRows = local;
            Transform targetRows = targetLocal;
            Transpose(gameRows.rotation);
            Transpose(targetRows.rotation);
            ToRows(Slerp(FromRows(gameRows.rotation), FromRows(targetRows.rotation), weight), local.rotation);
            Transpose(local.rotation);
            for (int axis = 0; axis < 3; ++axis)
                local.translation[axis] += (targetLocal.translation[axis] - local.translation[axis]) * weight;

            WriteTransform(pNode, kLocalOffset, local);
            s_pose.writtenLocals[index] = local;
            s_pose.hasWritten[index] = true;
        }

        const Transform newWorld = Combine(parentNew, local);
        WriteTransform(pNode, kWorldOffset, newWorld);
        moved.emplace(pNode, Moved{oldWorld, newWorld});
        ForEachChild(pNode, [&](uint8_t* apChild) { pending.push_back(apChild); });
    }
}

// The last step of every BSFlattenedBoneTree update (id 1421730): the bone nodes have this frame's world transforms,
// which the tree is about to copy for the skinned meshes, so a pose put on them here is what gets drawn (found for
// Mantle's first-person climbs, docs/ANIMATION_PIPELINE.md, 8). Only the first person skeleton's tree is posed here;
// the dual wield arms (DualWieldPose) go on any character's first, so the Pip-Boy's arm is the glance's.
struct BSFlattenedBoneTree;
TP_THIS_FUNCTION(TFinishBoneTreeUpdate, void, BSFlattenedBoneTree);
TFinishBoneTreeUpdate* RealFinishBoneTreeUpdate = nullptr;

void TP_MAKE_THISCALL(HookFinishBoneTreeUpdate, BSFlattenedBoneTree)
{
    static PerfProbe::HookStat s_perf("BoneTreeUpdate");
    PerfProbe::Hook _perf(s_perf);
    DualWieldPose::OnBoneTreeUpdated(reinterpret_cast<uint8_t*>(apThis));
    if (s_pose.posing.load(std::memory_order_relaxed))
    {
        auto* pTree = reinterpret_cast<uint8_t*>(apThis);
        if (uint8_t* pRoot = s_pose.pRoot; pRoot && GetParent(pTree) == pRoot)
            PoseArm(pRoot);
    }
    PerfProbe::GameCall _game;
    TiltedPhoques::ThisCall(RealFinishBoneTreeUpdate, apThis);
}

TiltedPhoques::Initializer s_glanceHooks(
    []()
    {
        POINTER_FALLOUT4(TFinishBoneTreeUpdate, s_finishBoneTreeUpdate, 1421731);
        RealFinishBoneTreeUpdate = s_finishBoneTreeUpdate.Get();
        TP_HOOK(&RealFinishBoneTreeUpdate, HookFinishBoneTreeUpdate);
    });
} // namespace

namespace PipboyGlancePose
{
bool Start(PlayerCharacter* apPlayer) noexcept
{
    uint8_t* pRoot = apPlayer ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(apPlayer) + kFirstPerson3DOffset) : nullptr;
    if (!pRoot)
        return false;

    std::array<uint8_t*, PipboyGlanceAnimation::kBoneCount> bones{};
    int found = 0;
    int posed = 0;
    for (int i = 0; i < PipboyGlanceAnimation::kBoneCount; ++i)
    {
        if (!IsPosedBone(PipboyGlanceAnimation::kBones[i]))
            continue;
        ++posed;
        bones[i] = GetObjectByName(pRoot, BSFixedString(PipboyGlanceAnimation::kBones[i]));
        found += bones[i] != nullptr;
    }
    uint8_t* pCamera = GetObjectByName(pRoot, BSFixedString(PipboyGlanceAnimation::kCameraBone));
    spdlog::info("Realtime Pip-Boy Map: arm pose on {} of {} bones, camera bone {}", found, posed, pCamera ? "found" : "missing");
    if (!pCamera || !found)
        return false;

    std::scoped_lock lock(s_pose.lock);
    const bool restart = s_pose.pRoot != pRoot || !s_pose.posing;
    s_pose.pRoot = pRoot;
    s_pose.pCamera = pCamera;
    s_pose.bones = bones;
    s_pose.hasWritten.fill(false);
    if (restart)
    {
        s_pose.time = 0.f;
        s_pose.blend = 0.f;
        s_pose.lastUpdate = Clock::time_point{};
    }
    s_pose.wanted = true;
    s_pose.posing = true;
    return true;
}

void Stop() noexcept
{
    s_pose.wanted = false;
}

void StopNow() noexcept
{
    s_pose.wanted = false;
    s_pose.blend = 0.f;
}

bool IsUp() noexcept
{
    return s_pose.posing && s_pose.blend >= 1.f;
}

bool IsActive() noexcept
{
    return s_pose.posing;
}
} // namespace PipboyGlancePose
