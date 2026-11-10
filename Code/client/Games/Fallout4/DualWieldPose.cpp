#include <HivemindPCH.h>

#include <DualWieldPose.h>
#include <NodeTools.h>
#include <OffHandModel.h>
#include <WeaponGrips.h>

#include <Misc/BSFixedString.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using NodeTools::Transform;

// Where each hand holds its weapon, in the frame (x right, y ahead, z up; game units; the right hand's, the left's
// mirrored): first person from the eye, third person from the chest bone
const glm::vec3 kHoldFirstPerson{15.f, 33.f, -13.f};
const glm::vec3 kHoldThirdPerson{13.f, 36.f, 9.f};
// lowered for the reload: in toward the body, back and down, out of the view (the right hand's; the left's mirrored)
const glm::vec3 kLowerFirstPerson{-4.f, -16.f, -46.f};
const glm::vec3 kLowerThirdPerson{-4.f, -18.f, -34.f};
// ... but a third person reload (Arms::ReloadDip) only a little out, back and down, the hands staying out in front of the
// body, the guns tipped this share of the dip's turn (the third person reload's arms neither lowered that far nor
// held that close to the body, kept natural; the game's pistol ready idle holds
// the hands 3 to 8 below the collarbones, 20 to 25 ahead of them). The draw, holster and a consumable's lowering keep the
// deep one: the guns go to the hips
const glm::vec3 kReloadThirdPerson{4.f, -4.f, -15.f};
constexpr float kReloadThirdPersonTurn = 0.65f;
// The reload's dip: the share of it going down and the share coming back up (between them the guns are out of view),
// the muzzle tipped down and the gun rolled outward at the bottom (radians), the left hand this much of the reload
// behind the right, the little lift before the drop (of the drop) and the overshoot coming up (the back ease's s)
constexpr float kDipDown = 0.26f;
constexpr float kDipUp = 0.32f;
constexpr float kDipPitch = 0.95f;
constexpr float kDipRoll = 0.5f;
constexpr float kDipLag = 0.05f;
constexpr float kDipLift = 0.05f;
constexpr float kDipOvershoot = 1.3f;
// a shot's kick at its peak: the hand this far back and the muzzle this far up (radians)
constexpr float kKickBack = 5.f;
constexpr float kKickPitch = 0.32f;
// life: the holds breathe a little (units, cycles a second), the two hands a little out of step
constexpr float kSwayAmount = 0.6f;
constexpr float kSwayRate = 0.85f;
// the left hand's melee swing: from raised and back to across the front and down (first person; third person the same
// moved by the difference between the two holds)
const glm::vec3 kSwingFrom{-30.f, 12.f, 16.f};
const glm::vec3 kSwingTo{10.f, 44.f, -10.f};
// a power swing: wound further back and up, carried further across and down
const glm::vec3 kPowerSwingFrom{-36.f, 4.f, 24.f};
const glm::vec3 kPowerSwingTo{18.f, 46.f, -16.f};
// the guard (first person; third person moved as the swing is): the blade across the face, a little ahead
const glm::vec3 kGuard{-2.f, 26.f, -2.f};
// on the grapple's cable: the hand this far from the shoulder up along it when the arm's length can't be read
constexpr float kHangReach = 48.f;
// the elbows point down and a little out
constexpr float kPoleOut = 0.6f;
constexpr float kPoleBack = 0.2f;
// the left hand follows the right one's motion this fast (a beat behind: about 1/18 s)
constexpr float kLeftFollowRate = 18.f;
// Third person, the left arm mirroring the right one: moving, it mirrors where the right one was half a stride ago, as
// arms swing in turn (as the right arm pumps the left one swings back, as in real
// running). The stride is timed from the feet (their difference along the body's forward crosses its swing's
// middle upward once a stride); their swing (that difference's range, kept this many units a second) over this many
// units is moving. The delay grows to half a stride and back by at most this many s a second (the left arm's playback
// slowed or hurried a little, never two poses blended: a blend showed the run's first quick moves twice, user,
// 2026-10-25: "when starting the run, the left side anim is choppy"); the right arm's places kept this long (s)
constexpr float kFeetDecay = 25.f;
constexpr float kFeetSwing = 10.f;
constexpr float kDelayRate = 0.6f;
constexpr float kArmHistorySeconds = 2.f;
// ... and the left hand kept on its own side (the hands otherwise too close or crossed: the game's pistol holds the
// gun on the body's middle line, mirrored exactly the hands meet or cross): at least this far left of the middle and of
// the right hand
constexpr float kHandOffMiddle = 6.f;
constexpr float kHandsApart = 16.f;
// Third person, the left gun aimed (Arms::AimAt): the hand this much of the arm's length out from the shoulder toward
// the aim, the muzzle on it; as much as the right gun's barrel lies along its line to the aim (between these cosines:
// about 35 and 15 degrees off)
constexpr float kAimReach = 0.88f;
constexpr float kAimLooseCos = 0.82f;
constexpr float kAimTightCos = 0.966f;
// On the grapple's cable (hanging from the grapple, the wrist and shoulder never bend past what they can):
// the hand this much of the arm's length up the cable from the shoulder joint itself, the wrist turned this share of the
// way to the gun pointing up the cable (the rest of the way from the forearm's own line), the collarbone raised up to
// this far (radians) as the arm goes overhead
constexpr float kHangStraight = 0.92f;
constexpr float kHangWristShare = 0.45f;
constexpr float kShoulderLift = 0.35f;

// Each hand's finger bones (Fallout4 - Meshes.ba2's skeletons, first and third person alike: the same hand): five
// fingers of three, each bone after its parent
using WeaponGrips::kFingerCount;
using WeaponGrips::Grip;

struct Side
{
    uint8_t* pCollar{nullptr};
    uint8_t* pUpper{nullptr};
    uint8_t* pFore{nullptr};
    uint8_t* pHand{nullptr};
    // the node the weapon in that hand hangs from (WeaponLeft, WEAPON)
    uint8_t* pWeapon{nullptr};
    std::array<uint8_t*, kFingerCount> pFingers{};
};

// A third person right arm bone and its left partner (RArm_ to LArm_, WEAPON to WeaponLeft, AnimObjectR to
// AnimObjectL), and the pair its parent is (-1: the right collarbone's, the Chest)
struct LimbPair
{
    uint8_t* pRight{nullptr};
    uint8_t* pLeft{nullptr};
    int Parent{-1};
    // the name the clips know it by (the third person skeleton calls WEAPON "Weapon")
    std::string ClipName;
};

// The body an attack turns, from the spine to the head: the third person skeleton's nodes, and the clips' names for them
// (each one's third axis the body's side to side, as the arm pairs' are: read from Meshes.ba2's skeleton, the thighs on
// the pelvis and the collarbones on the chest either side in z, the bones turned about z alone at rest)
constexpr size_t kBodyBones = 5;
const char* const kBodyNodes[kBodyBones] = {"SPINE1", "SPINE2", "Chest", "Neck", "HEAD"};
const char* const kBodyClipBones[kBodyBones] = {"Spine1", "Spine2", "Chest", "Neck", "Head"};

// A clip's bones as the pairs' (the right ones, and their left partners) and the body's (-1: the clip doesn't move that
// one), worked out once per clip
struct ClipIndex
{
    const WeaponGrips::LimbClip* pClip{nullptr};
    std::vector<int> Bones;
    std::array<int, kBodyBones> Body{};
};

struct Slot
{
    uint8_t* pRoot{nullptr};
    bool FirstPerson{false};
    Side Left{};
    Side Right{};
    // third person: the right arm's bones with their left partners, parents first (the mirror's), and the body's
    std::vector<LimbPair> Limb;
    std::array<uint8_t*, kBodyBones> Body{};
    // (the attack and the one it blends from, the idle)
    ClipIndex LimbClipBones[3];
    // third person, the gait: the feet, their difference along the body's forward (its side of its middle last frame, its
    // range, when it was last under its middle), when it last crossed upward, the stride (s), how far behind the right
    // arm the left one mirrors it now (s)
    uint8_t* pRightFoot{nullptr};
    uint8_t* pLeftFoot{nullptr};
    float FeetSide{0.f};
    float FeetHigh{0.f};
    float FeetLow{0.f};
    float LastBelow{0.f};
    float LastCrossing{-1.f};
    float Stride{0.7f};
    float Delay{0.f};
    // the right arm's bones' places on the chest over the last kArmHistorySeconds, the newest last
    std::deque<std::pair<float, std::vector<Transform>>> RightHistory;
    uint8_t* pChest{nullptr};
    // the first person skeleton's Camera bone: the view's frame in that skeleton's own space
    uint8_t* pCamera{nullptr};
    DualWieldPose::Arms Arms{};
    // Set has put arms on it (Clear takes them off again; the slot stays while a layer is on it), the layer over them
    bool ArmsSet{false};
    DualWieldPose::Layer pLayer{nullptr};
    float Time{0.f};
    Clock::time_point Last{};
    uint32_t Posed{0};
    // the right hand's motion as the left hand has it now (following it a beat behind, held while the right gun fires)
    DualWieldPose::Motion LeftFollowed{};
    // s to the next log line of how far the right arm's animation is from its idle
    float FollowLog{0.f};
    // the next log line of how the left hand holds its weapon (third person)
    Clock::time_point NextHoldLog{};
    // the arms' bones (by name) as the last pose left them (world), and those this frame's update didn't touch since:
    // their world is still ours from the last frame (UntouchedBones)
    std::unordered_map<const uint8_t*, Transform> LeftAs;
    std::unordered_set<const uint8_t*> Untouched;
    std::unordered_set<const uint8_t*> Written;
};

std::mutex s_lock;
std::array<Slot, 16> s_slots{};
std::atomic<bool> s_any{false};
// the slots' skeletons, read without the lock (every character's bone tree update asks; only a posed one waits for it)
std::array<std::atomic<uint8_t*>, 16> s_roots{};

Side FindSide(uint8_t* apRoot, bool aLeft) noexcept
{
    static BSFixedString s_names[2][4] = {
        {BSFixedString("RArm_UpperArm"), BSFixedString("RArm_ForeArm1"), BSFixedString("RArm_Hand"), BSFixedString("WEAPON")},
        {BSFixedString("LArm_UpperArm"), BSFixedString("LArm_ForeArm1"), BSFixedString("LArm_Hand"), BSFixedString("WeaponLeft")},
    };
    static const std::vector<BSFixedString> s_fingers = []
    {
        std::vector<BSFixedString> names;
        names.reserve(2 * kFingerCount);
        for (const char side : {'R', 'L'})
        {
            for (int finger = 1; finger <= 5; ++finger)
            {
                for (int joint = 1; joint <= 3; ++joint)
                    names.emplace_back(fmt::format("{}Arm_Finger{}{}", side, finger, joint).c_str());
            }
        }
        return names;
    }();
    static BSFixedString s_collars[2] = {BSFixedString("RArm_Collarbone"), BSFixedString("LArm_Collarbone")};
    const auto& names = s_names[aLeft ? 1 : 0];
    Side side{};
    side.pCollar = NodeTools::GetObjectByName(apRoot, s_collars[aLeft ? 1 : 0]);
    side.pUpper = NodeTools::GetObjectByName(apRoot, names[0]);
    side.pFore = NodeTools::GetObjectByName(apRoot, names[1]);
    side.pHand = NodeTools::GetObjectByName(apRoot, names[2]);
    side.pWeapon = NodeTools::GetObjectByName(apRoot, names[3]);
    for (size_t i = 0; i < kFingerCount; ++i)
        side.pFingers[i] = NodeTools::GetObjectByName(apRoot, s_fingers[(aLeft ? kFingerCount : 0) + i]);
    return side;
}

// The arms' skin helpers (RArm_ForeArm3_skin, RArm_ShoulderFat_skin, ...: 9 a side) are the skeleton model's own
// (Meshes.ba2's skeleton.nif), not the animation's (Animations.ba2's skeleton.hkx: 95 bones without them, read
// 2026-10-25): nothing has updated them yet when the pose runs, their world is still the last frame's
bool IsSkinHelper(const char* acpName) noexcept
{
    const size_t length = std::strlen(acpName);
    return length > 5 && _stricmp(acpName + length - 5, "_skin") == 0;
}

// The third person right arm from its collarbone down, each bone that has a left partner with it (a bone without one,
// and what's under it, left out: the right weapon's own nodes; and the skin helpers, which ride on their own parent, so
// the left ones follow the mirrored arm by their own place on it); main thread (names are made)
std::vector<LimbPair> FindLimb(uint8_t* apRoot) noexcept
{
    std::vector<LimbPair> limb;
    static BSFixedString s_collarbone("RArm_Collarbone");
    uint8_t* pTop = NodeTools::GetObjectByName(apRoot, s_collarbone);
    if (!pTop)
        return limb;
    const auto partnerOf = [](const std::string& acName) -> std::string
    {
        if (acName.rfind("RArm_", 0) == 0)
            return "LArm_" + acName.substr(5);
        if (acName == "WEAPON")
            return "WeaponLeft";
        if (acName.rfind("AnimObjectR", 0) == 0)
            return "AnimObjectL" + acName.substr(11);
        return {};
    };
    const std::function<void(uint8_t*, int)> walk = [&](uint8_t* apNode, int aParent)
    {
        const std::string name = NodeTools::GetName(apNode);
        if (IsSkinHelper(name.c_str()))
            return;
        const std::string partner = partnerOf(name);
        uint8_t* pLeft = partner.empty() ? nullptr : NodeTools::GetObjectByName(apRoot, BSFixedString(partner.c_str()));
        if (!pLeft)
            return;
        limb.push_back({apNode, pLeft, aParent, name == "WEAPON" ? std::string("Weapon") : name});
        const int index = static_cast<int>(limb.size()) - 1;
        NodeTools::ForEachChild(apNode, [&](uint8_t* apChild) { walk(apChild, index); });
    };
    walk(pTop, -1);
    return limb;
}

bool IsComplete(const Side& acSide) noexcept
{
    return acSide.pUpper && acSide.pFore && acSide.pHand && acSide.pWeapon;
}

bool HasFingers(const Side& acSide) noexcept
{
    return std::all_of(acSide.pFingers.begin(), acSide.pFingers.end(), [](const uint8_t* apFinger) { return apFinger != nullptr; });
}

// A skeleton bone by its name (the arms', the weapon nodes, the Pip-Boy's, the held objects'; not the arms' skin
// helpers, which aren't the animation's)
bool IsBoneName(const uint8_t* apNode) noexcept
{
    const char* pName = NodeTools::GetName(apNode);
    if (IsSkinHelper(pName))
        return false;
    return std::strncmp(pName, "LArm_", 5) == 0 || std::strncmp(pName, "RArm_", 5) == 0 || std::strcmp(pName, "WEAPON") == 0 || std::strcmp(pName, "WeaponLeft") == 0 ||
           std::strcmp(pName, "PipboyBone") == 0 || std::strncmp(pName, "AnimObject", 10) == 0;
}

// The slot being posed: its bones this frame's update didn't touch (FindUntouched), and the nodes this pose writes (the
// next frame's FindUntouched looks at those)
thread_local const std::unordered_set<const uint8_t*>* t_pUntouched = nullptr;
thread_local std::unordered_set<const uint8_t*>* t_pWritten = nullptr;
// Third person, a layer's turn: everything's carried by its local, none by what its world says (RestOnLocals, and BetterInteractClip's Bases, have just made those right)
thread_local bool t_rideOnLocals = false;

void Wrote(const uint8_t* apNode) noexcept
{
    if (t_pWritten)
        t_pWritten->insert(apNode);
}

// A skeleton's bone the bone tree has just put where this frame's animation has it, in its world transform; anything
// else (a weapon's model, our copy, the arms' skin helpers, a bone the update left alone this frame) sits on its parent
// by its local one. (A world that was still the last frame's, ours, turned into a place on a parent posed since, ran off
// further each frame: seen 2026-10-25 as the right hand stretching on a left knife swing in third person; first the
// skin helpers, then the thumb's joints themselves, the log's "RArm_Finger13 is 1422 from RArm_Finger12 after the
// pose")
bool IsBone(const uint8_t* apNode) noexcept
{
    return !t_rideOnLocals && IsBoneName(apNode) && !(t_pUntouched && t_pUntouched->contains(apNode));
}

// A rotation (column vectors) taking direction a onto b the shortest way
glm::mat3 FromTo(const glm::vec3& acFrom, const glm::vec3& acTo) noexcept
{
    const float lengths = glm::length(acFrom) * glm::length(acTo);
    if (lengths < 1e-8f)
        return glm::mat3(1.f);
    const glm::vec3 a = acFrom / glm::length(acFrom);
    const glm::vec3 b = acTo / glm::length(acTo);
    const float cosine = std::clamp(glm::dot(a, b), -1.f, 1.f);
    glm::vec3 axis = glm::cross(a, b);
    const float sine = glm::length(axis);
    if (sine < 1e-6f)
    {
        if (cosine > 0.f)
            return glm::mat3(1.f);
        // opposite: half a turn about any axis across a
        axis = glm::length(glm::cross(a, glm::vec3(1.f, 0.f, 0.f))) > 1e-3f ? glm::cross(a, glm::vec3(1.f, 0.f, 0.f)) : glm::cross(a, glm::vec3(0.f, 1.f, 0.f));
        axis = glm::normalize(axis);
        return glm::mat3(glm::rotate(glm::mat4(1.f), 3.14159265f, axis));
    }
    axis /= sine;
    return glm::mat3(glm::rotate(glm::mat4(1.f), std::atan2(sine, cosine), axis));
}

void Turn(Transform& aTransform, const glm::mat3& acRotation) noexcept
{
    for (auto& row : aTransform.Rows)
        row = acRotation * row;
}

// A rotation's rows as a quaternion and back, and the shorter way between two
glm::quat ToQuat(const Transform& acTransform) noexcept
{
    // the rows are the axes' directions: as a matrix's columns they're the rotation itself
    return glm::quat_cast(glm::mat3(acTransform.Rows[0], acTransform.Rows[1], acTransform.Rows[2]));
}

void FromQuat(Transform& aTransform, const glm::quat& acQ) noexcept
{
    const glm::mat3 m = glm::mat3_cast(glm::normalize(acQ));
    aTransform.Rows[0] = m[0];
    aTransform.Rows[1] = m[1];
    aTransform.Rows[2] = m[2];
}

// aT of the way from one place on a parent to another (turned the shorter way)
Transform Blend(const Transform& acFrom, const Transform& acTo, float aT) noexcept
{
    Transform blended = acFrom;
    FromQuat(blended, glm::slerp(ToQuat(acFrom), ToQuat(acTo), aT));
    blended.Translation = glm::mix(acFrom.Translation, acTo.Translation, aT);
    return blended;
}

// The left bones mirror the right ones (both skeletons, read from Meshes.ba2: each left bone's axes are its right
// partner's reflected across the body, its third axis turned over), so a right bone's place on its parent is its left
// partner's with the rows' and columns' third parts turned over (both, the third row's third part kept)
Transform Mirror(const Transform& acRight) noexcept
{
    Transform left = acRight;
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            if ((row == 2) != (column == 2))
                left.Rows[row][column] = -left.Rows[row][column];
        }
    }
    left.Translation.z = -left.Translation.z;
    return left;
}

// A hand's grip as this frame has it, from the bones' world transforms
Grip ReadGrip(const Side& acSide) noexcept
{
    Grip grip{};
    grip.Weapon = NodeTools::Relative(NodeTools::Read(acSide.pHand, NodeTools::kWorldOffset), NodeTools::Read(acSide.pWeapon, NodeTools::kWorldOffset));
    for (size_t i = 0; i < kFingerCount; ++i)
    {
        const uint8_t* pFinger = acSide.pFingers[i];
        grip.Fingers[i] = NodeTools::Relative(NodeTools::Read(NodeTools::GetParent(pFinger), NodeTools::kWorldOffset), NodeTools::Read(pFinger, NodeTools::kWorldOffset));
    }
    return grip;
}

Grip MirrorGrip(const Grip& acRight) noexcept
{
    Grip left{};
    left.Weapon = Mirror(acRight.Weapon);
    for (size_t i = 0; i < kFingerCount; ++i)
        left.Fingers[i] = Mirror(acRight.Fingers[i]);
    return left;
}

// A right side bone's place in the view (the Camera bone's frame) as its left partner's there: the mirror is the view's
// across its x (right) axis, so the rows' x parts turn over, and the third row (the bone's third axis turned over, as
// Mirror has it) the other way; the translation's x (checked on the first person skeleton's rest pose: every arm bone
// within 0.0005)
Transform MirrorInView(const Transform& acRight) noexcept
{
    Transform left = acRight;
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            if ((row == 2) != (column == 0))
                left.Rows[row][column] = -left.Rows[row][column];
        }
    }
    left.Translation.x = -left.Translation.x;
    return left;
}

Grip BlendGrip(const Grip& acFrom, const Grip& acTo, float aT) noexcept
{
    Grip blended{};
    blended.Weapon = Blend(acFrom.Weapon, acTo.Weapon, aT);
    for (size_t i = 0; i < kFingerCount; ++i)
        blended.Fingers[i] = Blend(acFrom.Fingers[i], acTo.Fingers[i], aT);
    return blended;
}

// The pose's step under way (for the log of a broken pose), and whether a transform is all numbers
thread_local const char* t_stage = "";

bool IsFinite(const Transform& acTransform) noexcept
{
    const auto finite = [](const glm::vec3& acV) { return std::isfinite(acV.x) && std::isfinite(acV.y) && std::isfinite(acV.z); };
    return finite(acTransform.Rows[0]) && finite(acTransform.Rows[1]) && finite(acTransform.Rows[2]) && finite(acTransform.Translation) &&
           std::isfinite(acTransform.Scale);
}

// A pose that came out not a number (seen 2026-10-25: the grapple gun's muzzle read NaN hanging in third person) is
// never written: the bone keeps this frame's animation, and the log says where and in which step (now and then). Its
// local too: a bone the game doesn't animate keeps the local it's given, and every frame after builds on it (seen the
// same day: the game's own right hand of no number, after a hang)
bool Refused(const uint8_t* apNode, const Transform& acWorld, const Transform* apLocal = nullptr) noexcept
{
    if (IsFinite(acWorld) && (!apLocal || IsFinite(*apLocal)))
        return false;
    static std::atomic<int64_t> s_loggedAt{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if (now - s_loggedAt.load(std::memory_order_relaxed) > 2000)
    {
        s_loggedAt = now;
        spdlog::warn("Dual wield: a pose of no number kept off {} (step: {}{})", NodeTools::GetName(apNode), t_stage, IsFinite(acWorld) ? ", its place on its parent" : "");
    }
    return true;
}

// Everything under a node again, its own world being new (it was at acOldWorld): bones keep where this frame's animation
// had them on it, anything else its local
void Rebuild(uint8_t* apNode, const Transform& acOldWorld) noexcept
{
    const Transform world = NodeTools::Read(apNode, NodeTools::kWorldOffset);
    NodeTools::ForEachChild(apNode, [&](uint8_t* apChild) {
        const Transform childOld = NodeTools::Read(apChild, NodeTools::kWorldOffset);
        Transform local = NodeTools::Read(apChild, NodeTools::kLocalOffset);
        if (IsBone(apChild))
            local = NodeTools::Relative(acOldWorld, childOld);
        const Transform childWorld = NodeTools::Combine(world, local);
        if (Refused(apChild, childWorld, &local))
            return;
        if (IsBone(apChild))
            NodeTools::Write(apChild, NodeTools::kLocalOffset, local);
        NodeTools::Write(apChild, NodeTools::kWorldOffset, childWorld);
        Wrote(apChild);
        Rebuild(apChild, childOld);
    });
}

// A node put at a world transform: its local under its parent (whose world is current) and its world, then its tree
void Place(uint8_t* apNode, const Transform& acWorld) noexcept
{
    uint8_t* pParent = NodeTools::GetParent(apNode);
    if (!pParent)
        return;
    const Transform local = NodeTools::Relative(NodeTools::Read(pParent, NodeTools::kWorldOffset), acWorld);
    if (Refused(apNode, acWorld, &local))
        return;
    const Transform old = NodeTools::Read(apNode, NodeTools::kWorldOffset);
    NodeTools::Write(apNode, NodeTools::kLocalOffset, local);
    NodeTools::Write(apNode, NodeTools::kWorldOffset, acWorld);
    Wrote(apNode);
    Rebuild(apNode, old);
}

// The grip put on a posed hand: its weapon node, then each finger on its (just placed) parent
void ApplyGrip(const Side& acSide, const Grip& acGrip) noexcept
{
    Place(acSide.pWeapon, NodeTools::Combine(NodeTools::Read(acSide.pHand, NodeTools::kWorldOffset), acGrip.Weapon));
    for (size_t i = 0; i < kFingerCount; ++i)
    {
        uint8_t* pFinger = acSide.pFingers[i];
        Place(pFinger, NodeTools::Combine(NodeTools::Read(NodeTools::GetParent(pFinger), NodeTools::kWorldOffset), acGrip.Fingers[i]));
    }
}

// The way the hand turns for the weapon under it to point along acAhead with its top up acTop: the weapon's world rows
// are (ahead x top, ahead, top), its node's the same in the right hand and turned half about the barrel in the left
// (the copy hangs from WeaponLeft so, OffHandModel::OnWeaponLeft); the hand's follow from the node's place on it
Transform HandFor(const Transform& acWeaponOnHand, bool aLeft, const glm::vec3& acAhead, const glm::vec3& acTop) noexcept
{
    const glm::vec3 ahead = glm::normalize(acAhead);
    glm::vec3 right = glm::cross(ahead, acTop);
    right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1.f, 0.f, 0.f);
    const glm::vec3 top = glm::cross(right, ahead);
    const float turn = aLeft ? -1.f : 1.f;
    const glm::vec3 weapon[3] = {right * turn, ahead, top * turn};

    const Transform& local = acWeaponOnHand;
    Transform goal{};
    for (int row = 0; row < 3; ++row)
        goal.Rows[row] = local.Rows[0][row] * weapon[0] + local.Rows[1][row] * weapon[1] + local.Rows[2][row] * weapon[2];
    return goal;
}

// The arm reaching for acTarget (world), the hand turned to acGoal's rotation, aWeight of the way from where the game
// has it (the hand's turn aTurnShare of that: the rest stays as the forearm carries it); the elbow toward acPole
void PoseArm(const Side& acSide, const glm::vec3& acTarget, const Transform& acGoal, const glm::vec3& acPole, float aWeight, float aTurnShare = 1.f) noexcept
{
    Transform upper = NodeTools::Read(acSide.pUpper, NodeTools::kWorldOffset);
    const Transform foreOld = NodeTools::Read(acSide.pFore, NodeTools::kWorldOffset);
    const Transform hand = NodeTools::Read(acSide.pHand, NodeTools::kWorldOffset);
    const glm::vec3 shoulder = upper.Translation;
    const glm::vec3 elbow = foreOld.Translation;
    const glm::vec3 wrist = hand.Translation;
    const float a = glm::distance(shoulder, elbow);
    const float b = glm::distance(elbow, wrist);
    if (a < 1e-3f || b < 1e-3f)
        return;

    // the reach, within what the arm can do (a target or weight not all numbers: none; a clamp of no number is no number)
    const glm::vec3 target = wrist + (acTarget - wrist) * aWeight;
    glm::vec3 toTarget = target - shoulder;
    if (!std::isfinite(toTarget.x) || !std::isfinite(toTarget.y) || !std::isfinite(toTarget.z))
        return;
    const float reach = std::clamp(glm::length(toTarget), std::abs(a - b) + 0.05f, a + b - 0.05f);
    const glm::vec3 n = glm::length(toTarget) > 1e-4f ? glm::normalize(toTarget) : glm::normalize(wrist - shoulder);
    glm::vec3 pole = acPole - n * glm::dot(acPole, n);
    if (glm::length(pole) <= 1e-4f)
        pole = glm::cross(n, glm::vec3(0.f, 0.f, 1.f));
    // (n along z: the target right over or under the shoulder)
    if (glm::length(pole) <= 1e-4f)
        pole = glm::cross(n, glm::vec3(1.f, 0.f, 0.f));
    pole = glm::normalize(pole);
    // the elbow's side: the way it bends now, aWeight of the way to the pole's (a little weight moves it a little; the
    // pole whole at once threw the elbow over each time a pose came on: a gun's aim flickering on and off sprinting showed
    // as a hitch every stride)
    if (glm::vec3 bend = (elbow - shoulder) - n * glm::dot(elbow - shoulder, n); glm::length(bend) > 1e-3f)
    {
        const glm::vec3 mixed = glm::mix(glm::normalize(bend), pole, std::clamp(aWeight, 0.f, 1.f));
        if (glm::length(mixed) > 1e-3f)
            pole = glm::normalize(mixed);
    }
    const float cosine = std::clamp((a * a + reach * reach - b * b) / (2.f * a * reach), -1.f, 1.f);
    const glm::vec3 elbowGoal = shoulder + a * (cosine * n + std::sqrt(1.f - cosine * cosine) * pole);

    // the upper arm turned to put the elbow there, the forearm with it, then turned to put the wrist at the reach
    Turn(upper, FromTo(elbow - shoulder, elbowGoal - shoulder));
    Place(acSide.pUpper, upper);
    Transform fore = NodeTools::Read(acSide.pFore, NodeTools::kWorldOffset);
    const glm::vec3 wristNow = NodeTools::GetWorldPosition(acSide.pHand);
    Turn(fore, FromTo(wristNow - fore.Translation, shoulder + n * reach - fore.Translation));
    Place(acSide.pFore, fore);

    // the hand turned as the weapon needs, where the forearm put it
    Transform handNow = NodeTools::Read(acSide.pHand, NodeTools::kWorldOffset);
    FromQuat(handNow, glm::slerp(ToQuat(handNow), ToQuat(acGoal), aWeight * std::clamp(aTurnShare, 0.f, 1.f)));
    Place(acSide.pHand, handNow);
}

// The shoulder raised with an arm going overhead (the collarbone turned up about its own line's cross with up), as
// shoulders do: by up to kShoulderLift as acAlong points up
void LiftShoulder(const Side& acSide, const glm::vec3& acAlong, const glm::vec3& acUp, float aWeight) noexcept
{
    const float raise = std::clamp(glm::dot(glm::normalize(acAlong), acUp), 0.f, 1.f) * aWeight;
    if (!acSide.pCollar || raise <= 0.f)
        return;
    Transform collar = NodeTools::Read(acSide.pCollar, NodeTools::kWorldOffset);
    const glm::vec3 line = NodeTools::GetWorldPosition(acSide.pUpper) - collar.Translation;
    glm::vec3 axis = glm::cross(line, acUp);
    if (glm::length(axis) < 1e-4f)
        return;
    axis = glm::normalize(axis);
    Turn(collar, glm::mat3(glm::rotate(glm::mat4(1.f), kShoulderLift * raise, axis)));
    Place(acSide.pCollar, collar);
}

// A clip's bones as the limb's (by name), kept for the clip in that slot
const std::vector<int>& BonesOf(ClipIndex& aIndex, const std::vector<LimbPair>& acLimb, const WeaponGrips::LimbClip& acClip) noexcept
{
    if (aIndex.pClip != &acClip)
    {
        aIndex.pClip = &acClip;
        const auto find = [&acClip](const std::string& acName)
        {
            const auto it = std::find(acClip.Bones.begin(), acClip.Bones.end(), acName);
            return it != acClip.Bones.end() ? static_cast<int>(it - acClip.Bones.begin()) : -1;
        };
        aIndex.Bones.assign(acLimb.size(), -1);
        for (size_t i = 0; i < acLimb.size(); ++i)
            aIndex.Bones[i] = find(acLimb[i].ClipName);
        for (size_t i = 0; i < kBodyBones; ++i)
            aIndex.Body[i] = find(kBodyClipBones[i]);
    }
    return aIndex.Bones;
}

// Third person, a left melee weapon's attack or block: the body (the spine to the head) as the clip has it, mirrored
// (a right-handed attack's turn of the body made left-handed: the body moves with a
// left-handed melee attack as it does with a right-handed one), LimbWeight of the way from the game's, so the arms and the
// right weapon turn with it; the pelvis and the legs stay the game's (walking under it, the feet planted)
void PoseBody(Slot& aSlot, float aWeight) noexcept
{
    const DualWieldPose::Arms& arms = aSlot.Arms;
    const float clipWeight = arms.pLimbClip && arms.LimbMovesBody ? std::clamp(arms.LimbWeight, 0.f, 1.f) * aWeight : 0.f;
    if (clipWeight <= 0.f)
        return;
    BonesOf(aSlot.LimbClipBones[0], aSlot.Limb, *arms.pLimbClip);
    const bool blendFrom = arms.pLimbClipFrom && arms.LimbBlend < 1.f;
    if (blendFrom)
        BonesOf(aSlot.LimbClipBones[1], aSlot.Limb, *arms.pLimbClipFrom);
    for (size_t i = 0; i < kBodyBones; ++i)
    {
        uint8_t* pBone = aSlot.Body[i];
        uint8_t* pParent = pBone ? NodeTools::GetParent(pBone) : nullptr;
        const int bone = aSlot.LimbClipBones[0].Body[i];
        if (!pParent || bone < 0)
            continue;
        Transform clip = arms.pLimbClip->LocalAt(static_cast<size_t>(bone), arms.LimbTime);
        if (blendFrom && aSlot.LimbClipBones[1].Body[i] >= 0)
            clip = Blend(arms.pLimbClipFrom->LocalAt(static_cast<size_t>(aSlot.LimbClipBones[1].Body[i]), arms.LimbFromTime), clip, std::clamp(arms.LimbBlend, 0.f, 1.f));
        const Transform parentWorld = NodeTools::Read(pParent, NodeTools::kWorldOffset);
        const Transform game = NodeTools::Relative(parentWorld, NodeTools::Read(pBone, NodeTools::kWorldOffset));
        Transform local = Blend(game, Mirror(clip), clipWeight);
        local.Scale = game.Scale;
        Place(pBone, NodeTools::Combine(parentWorld, local));
    }
}

// The gait from the feet: their difference along the body's forward swings once a stride (its range over the last second
// or so says how much), crossing its swing's middle upward once a stride (a little hysteresis against a jitter)
void UpdateGait(Slot& aSlot, const glm::vec3& acForward, float aDelta) noexcept
{
    if (!aSlot.pRightFoot || !aSlot.pLeftFoot)
    {
        aSlot.Delay = 0.f;
        return;
    }
    const float feet = glm::dot(NodeTools::GetWorldPosition(aSlot.pRightFoot) - NodeTools::GetWorldPosition(aSlot.pLeftFoot), acForward);
    aSlot.FeetHigh = std::max(feet, aSlot.FeetHigh - kFeetDecay * aDelta);
    aSlot.FeetLow = std::min(feet, aSlot.FeetLow + kFeetDecay * aDelta);
    const float range = aSlot.FeetHigh - aSlot.FeetLow;
    // about the swing's middle, not 0 (seen 2026-10-25: a sprint swung 139 and never crossed 0, the stride stayed unmeasured)
    const float middle = (aSlot.FeetHigh + aSlot.FeetLow) * 0.5f;
    if (feet < middle)
        aSlot.LastBelow = aSlot.Time;
    const float side = feet > middle + range * 0.15f ? 1.f : feet < middle - range * 0.15f ? -1.f : aSlot.FeetSide;
    if (side > 0.f && aSlot.FeetSide < 0.f)
    {
        // (it crossed the middle itself when it was last under it, a frame or so before the hysteresis says so)
        const float crossing = aSlot.LastBelow;
        const float stride = crossing - aSlot.LastCrossing;
        if (aSlot.LastCrossing >= 0.f && stride > 0.3f && stride < 2.5f)
            aSlot.Stride = glm::mix(aSlot.Stride, stride, 0.5f);
        aSlot.LastCrossing = crossing;
    }
    aSlot.FeetSide = side;
    // the left arm's delay: half a stride while moving, none standing, changed no faster than kDelayRate
    const float target = range > kFeetSwing ? aSlot.Stride * 0.5f : 0.f;
    const float step = kDelayRate * aDelta;
    aSlot.Delay = std::clamp(target, aSlot.Delay - step, aSlot.Delay + step);
}

// An arm's bones put where a pose has them (world, parents first; the pairs' left ones or their right ones), their places
// on their parents with them; what hangs from the arm that isn't the arm (a weapon's model, our copy, the Pip-Boy's
// nodes) carried along, a bone of those where this frame's animation had it on its parent (acOld: the arm before)
void WriteArm(const std::vector<LimbPair>& acLimb, bool aLeft, const std::vector<Transform>& acOld, const std::vector<Transform>& acNew) noexcept
{
    const auto nodeOf = [aLeft](const LimbPair& acPair) { return aLeft ? acPair.pLeft : acPair.pRight; };
    for (size_t i = 0; i < acLimb.size(); ++i)
    {
        uint8_t* pNode = nodeOf(acLimb[i]);
        const uint8_t* pParent = NodeTools::GetParent(pNode);
        if (!pParent)
            continue;
        const Transform local = NodeTools::Relative(NodeTools::Read(pParent, NodeTools::kWorldOffset), acNew[i]);
        if (Refused(pNode, acNew[i], &local))
            continue;
        NodeTools::Write(pNode, NodeTools::kLocalOffset, local);
        NodeTools::Write(pNode, NodeTools::kWorldOffset, acNew[i]);
        Wrote(pNode);
    }
    for (size_t i = 0; i < acLimb.size(); ++i)
    {
        uint8_t* pNode = nodeOf(acLimb[i]);
        const Transform parentWorld = NodeTools::Read(pNode, NodeTools::kWorldOffset);
        NodeTools::ForEachChild(pNode, [&](uint8_t* apChild) {
            if (std::any_of(acLimb.begin(), acLimb.end(), [&](const LimbPair& acPair) { return nodeOf(acPair) == apChild; }))
                return;
            const Transform childOld = NodeTools::Read(apChild, NodeTools::kWorldOffset);
            const bool bone = IsBone(apChild);
            const Transform local = bone ? NodeTools::Relative(acOld[i], childOld) : NodeTools::Read(apChild, NodeTools::kLocalOffset);
            const Transform childWorld = NodeTools::Combine(parentWorld, local);
            if (Refused(apChild, childWorld, &local))
                return;
            if (bone)
                NodeTools::Write(apChild, NodeTools::kLocalOffset, local);
            NodeTools::Write(apChild, NodeTools::kWorldOffset, childWorld);
            Wrote(apChild);
            Rebuild(apChild, childOld);
        });
    }
}

// Third person: the left arm put where the right one is, mirrored across the chest's middle (the plane halfway between
// the two collarbones, square to the line between them: the skeletons' left bones are their right partners reflected
// across the body, the third axis turned over, read from Meshes.ba2's skeleton for all 22 pairs). The right arm as this
// frame's animation has it (moving, as it was half a stride ago), or the clip's (each bone's place on its parent from
// the clip, the bones it doesn't move as the animation has them, from this chest) LimbWeight of the way; aWeight of the
// way from the game's left arm. Bones that aren't the arm's under it (our weapon's copy) follow. Gives the plane
bool MirrorLimb(Slot& aSlot, float aWeight, const glm::vec3& acForward, float aDelta, glm::vec3& aMiddle, glm::vec3& aAcross) noexcept
{
    // A bone of the arms taken off the skeleton since they were found (the view going from first to third person with
    // two weapons out: one's parent was gone the next frame, 2026-10-26 21:50, the game ended reading it): found again,
    // this frame left as the game has it
    const auto attached = [&aSlot](const uint8_t* apNode)
    {
        for (int depth = 0; apNode && depth < 64; ++depth, apNode = NodeTools::GetParent(apNode))
        {
            if (apNode == aSlot.pRoot)
                return true;
        }
        return false;
    };
    if (!std::all_of(aSlot.Limb.begin(), aSlot.Limb.end(), [&](const LimbPair& acPair) { return attached(acPair.pRight) && attached(acPair.pLeft); }))
    {
        spdlog::info("Dual wield: a bone of the third person arms came off skeleton {}: found them again", fmt::ptr(aSlot.pRoot));
        aSlot.Limb = FindLimb(aSlot.pRoot);
        return false;
    }

    const DualWieldPose::Arms& arms = aSlot.Arms;
    const std::vector<LimbPair>& limb = aSlot.Limb;
    const size_t count = limb.size();
    uint8_t* pChest = NodeTools::GetParent(limb.front().pRight);
    if (!pChest)
        return false;
    const Transform chest = NodeTools::Read(pChest, NodeTools::kWorldOffset);

    std::vector<Transform> right(count);
    for (size_t i = 0; i < count; ++i)
    {
        // (a bone this frame's update hasn't moved yet, its world still the last frame's: on its parent by its own place)
        const int parent = limb[i].Parent;
        if (parent >= 0 && t_pUntouched && t_pUntouched->contains(limb[i].pRight))
            right[i] = NodeTools::Combine(right[parent], NodeTools::Read(limb[i].pRight, NodeTools::kLocalOffset));
        else
            right[i] = NodeTools::Read(limb[i].pRight, NodeTools::kWorldOffset);
    }

    // moving: where the right arm was Delay ago (kept on the chest, so put on this frame's chest)
    UpdateGait(aSlot, acForward, aDelta);
    if ((aSlot.FollowLog -= aDelta) <= 0.f)
    {
        aSlot.FollowLog = 15.f;
        spdlog::info("Dual wield: third person gait: the feet swing {:.1f}, a stride {:.2f} s, the left arm {:.2f} s behind the right", aSlot.FeetHigh - aSlot.FeetLow, aSlot.Stride,
                     aSlot.Delay);
    }
    {
        std::vector<Transform> onChest(count);
        for (size_t i = 0; i < count; ++i)
            onChest[i] = NodeTools::Relative(chest, right[i]);
        auto& history = aSlot.RightHistory;
        history.emplace_back(aSlot.Time, std::move(onChest));
        while (history.size() > 2 && history.front().first < aSlot.Time - kArmHistorySeconds)
            history.pop_front();
        const float at = aSlot.Time - aSlot.Delay;
        if (aSlot.Delay > 0.f && history.size() > 1 && history.front().first <= at)
        {
            size_t after = history.size() - 1;
            while (after > 0 && history[after - 1].first > at)
                --after;
            const auto& [timeA, before] = history[after - 1];
            const auto& [timeB, later] = history[after];
            const float t = timeB > timeA ? std::clamp((at - timeA) / (timeB - timeA), 0.f, 1.f) : 1.f;
            for (size_t i = 0; i < count; ++i)
                right[i] = NodeTools::Combine(chest, Blend(before[i], later[i], t));
        }
    }

    // Two poses of the arm (world, parents first) blended bone by bone on their parents and put together again from the
    // chest out, so a weapon node stays where the hand holds it all through (blended in the world each bone went its own
    // way: seen 2026-10-25 as the knife floating outside the hand after walking, and slipping going into a swing)
    const auto blendPoses = [&limb, count, &chest](const std::vector<Transform>& acFrom, const std::vector<Transform>& acTo, float aT)
    {
        std::vector<Transform> blended(count);
        for (size_t i = 0; i < count; ++i)
        {
            const int parent = limb[i].Parent;
            const Transform local = Blend(NodeTools::Relative(parent >= 0 ? acFrom[parent] : chest, acFrom[i]), NodeTools::Relative(parent >= 0 ? acTo[parent] : chest, acTo[i]), aT);
            blended[i] = NodeTools::Combine(parent >= 0 ? blended[parent] : chest, local);
        }
        return blended;
    };

    // A left melee weapon's own clips in place of the right arm: its idle (moving, the game's own arms: the pose's weight
    // goes to none); each from this chest, the bones a clip doesn't move as the arm under it has them
    const auto clipPose = [&](const WeaponGrips::LimbClip& acClip, ClipIndex& aIndex, float aTime)
    {
        const std::vector<int>& bones = BonesOf(aIndex, limb, acClip);
        std::vector<Transform> pose(count);
        for (size_t i = 0; i < count; ++i)
        {
            const int parent = limb[i].Parent;
            Transform local = bones[i] >= 0 ? acClip.LocalAt(static_cast<size_t>(bones[i]), aTime) : NodeTools::Relative(parent >= 0 ? right[parent] : chest, right[i]);
            local.Scale = NodeTools::Read(limb[i].pRight, NodeTools::kLocalOffset).Scale;
            pose[i] = NodeTools::Combine(parent >= 0 ? pose[parent] : chest, local);
        }
        return pose;
    };
    if (arms.pLimbIdle)
        right = clipPose(*arms.pLimbIdle, aSlot.LimbClipBones[2], arms.LimbIdleTime);

    const float clipWeight = arms.pLimbClip ? std::clamp(arms.LimbWeight, 0.f, 1.f) : 0.f;
    if (clipWeight > 0.f)
    {
        const std::vector<int>& bones = BonesOf(aSlot.LimbClipBones[0], limb, *arms.pLimbClip);
        const bool blendFrom = arms.pLimbClipFrom && arms.LimbBlend < 1.f;
        const std::vector<int>* pFromBones = blendFrom ? &BonesOf(aSlot.LimbClipBones[1], limb, *arms.pLimbClipFrom) : nullptr;
        std::vector<Transform> clip(count);
        for (size_t i = 0; i < count; ++i)
        {
            const int parent = limb[i].Parent;
            const Transform& liveParent = parent >= 0 ? right[parent] : chest;
            Transform local = bones[i] >= 0 ? arms.pLimbClip->LocalAt(static_cast<size_t>(bones[i]), arms.LimbTime) : NodeTools::Relative(liveParent, right[i]);
            if (pFromBones && (*pFromBones)[i] >= 0)
                local = Blend(arms.pLimbClipFrom->LocalAt(static_cast<size_t>((*pFromBones)[i]), arms.LimbFromTime), local, std::clamp(arms.LimbBlend, 0.f, 1.f));
            local.Scale = NodeTools::Read(limb[i].pRight, NodeTools::kLocalOffset).Scale;
            clip[i] = NodeTools::Combine(parent >= 0 ? clip[parent] : chest, local);
        }
        right = blendPoses(right, clip, clipWeight);
    }

    // the plane: halfway between the collarbones (their places on the chest never move), square to the line between them
    const glm::vec3 rightShoulder = NodeTools::GetWorldPosition(limb.front().pRight);
    const glm::vec3 leftShoulder = NodeTools::GetWorldPosition(limb.front().pLeft);
    if (glm::distance(rightShoulder, leftShoulder) < 1.f)
        return false;
    const glm::vec3 across = glm::normalize(rightShoulder - leftShoulder);
    const glm::vec3 middle = (rightShoulder + leftShoulder) * 0.5f;
    aMiddle = middle;
    aAcross = across;
    const auto reflect = [&across](const glm::vec3& acV) { return acV - 2.f * glm::dot(acV, across) * across; };

    std::vector<Transform> game(count);
    std::vector<Transform> mirrored(count);
    for (size_t i = 0; i < count; ++i)
    {
        game[i] = NodeTools::Read(limb[i].pLeft, NodeTools::kWorldOffset);
        mirrored[i] = right[i];
        for (int row = 0; row < 3; ++row)
            mirrored[i].Rows[row] = reflect(right[i].Rows[row]) * (row == 2 ? -1.f : 1.f);
        mirrored[i].Translation = middle + reflect(right[i].Translation - middle);
        mirrored[i].Scale = game[i].Scale;
    }
    // (eased in from the game's left arm the same way: bone by bone on its parent, the left collarbone's the chest too)
    if (aWeight < 1.f)
        mirrored = blendPoses(game, mirrored, aWeight);
    WriteArm(limb, true, game, mirrored);
    return true;
}

// Rotates v about an axis (radians)
glm::vec3 About(const glm::vec3& acV, const glm::vec3& acAxis, float aAngle) noexcept
{
    return glm::mat3(glm::rotate(glm::mat4(1.f), aAngle, glm::normalize(acAxis))) * acV;
}

float EaseInOut(float aT) noexcept
{
    const float t = std::clamp(aT, 0.f, 1.f);
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) * 0.5f;
}

float EaseOut(float aT) noexcept
{
    const float t = std::clamp(aT, 0.f, 1.f);
    return 1.f - std::pow(1.f - t, 3.f);
}

// Past 1 on the way and back (an overshoot of about a tenth at s 1.7)
float EaseOutBack(float aT, float aS) noexcept
{
    const float u = std::clamp(aT, 0.f, 1.f) - 1.f;
    return 1.f + (aS + 1.f) * u * u * u + aS * u * u;
}

// Where a hand is in the reload's dip at a phase (0..1): how far down (1 the bottom; below 0 the lift before the drop
// and the overshoot above the hold coming up), how far back toward the body (ahead of the drop going down, behind the
// rise coming up: the hand travels a curve, not a line), and how far the gun's turned (tipped down and rolled out:
// it leads the drop, settles after the rise with a small wobble)
struct Dip
{
    float Down{0.f};
    float Back{0.f};
    float Turn{0.f};
};

Dip DipAt(float aPhase) noexcept
{
    Dip dip{};
    if (aPhase < 0.f || aPhase >= 1.f)
        return dip;
    if (aPhase < kDipDown)
    {
        constexpr float kLiftShare = 0.14f;
        const float t = aPhase / kDipDown;
        dip.Down = t < kLiftShare ? -kDipLift * std::sin(3.14159265f * t / kLiftShare) : EaseInOut((t - kLiftShare) / (1.f - kLiftShare));
        dip.Back = EaseOut(t);
        dip.Turn = EaseOut(t / 0.8f);
        return dip;
    }
    if (aPhase < 1.f - kDipUp)
    {
        dip.Down = dip.Back = dip.Turn = 1.f;
        return dip;
    }
    const float t = (aPhase - (1.f - kDipUp)) / kDipUp;
    dip.Down = 1.f - EaseOutBack(t, kDipOvershoot);
    dip.Back = 1.f - EaseInOut((t - 0.15f) / 0.85f);
    const float turn = std::clamp((t - 0.1f) / 0.9f, 0.f, 1.f);
    dip.Turn = 1.f - EaseOut(turn) + 0.06f * std::sin(turn * 9.424778f) * 4.f * turn * (1.f - turn);
    return dip;
}

// The left hand's phase in the reload: a beat behind the right, both up together at its end
float LeftPhase(float aPhase) noexcept
{
    if (aPhase < 0.f)
        return aPhase;
    return std::max(0.f, (aPhase - kDipLag) / (1.f - kDipLag));
}

void PoseArms(Slot& aSlot) noexcept
{
    const auto now = Clock::now();
    const float delta = aSlot.Last == Clock::time_point{} ? 0.f : std::min(std::chrono::duration<float>(now - aSlot.Last).count(), 0.1f);
    aSlot.Last = now;
    aSlot.Time += delta;
    ++aSlot.Posed;

    const DualWieldPose::Arms& arms = aSlot.Arms;
    const float weight = std::clamp(arms.Weight, 0.f, 1.f);
    const float gripWeight = std::max(weight, std::clamp(arms.GripWeight, 0.f, 1.f));
    if (gripWeight <= 0.f)
        return;
    // the hands as the game has them this frame, before ours: no number there is the game's (or a past frame's) doing
    t_stage = "the game's own";
    for (const Side* pSide : {&aSlot.Left, &aSlot.Right})
    {
        if (pSide->pHand)
            Refused(pSide->pHand, NodeTools::Read(pSide->pHand, NodeTools::kWorldOffset));
    }
    t_stage = aSlot.FirstPerson ? "first person" : "third person";
    // the left weapon's grip: in third person a melee weapon's third person one (Arms::pLeftGrip3P)
    const Grip* pLeftGrip = !aSlot.FirstPerson && arms.pLeftGrip3P ? arms.pLeftGrip3P : arms.pLeftGrip;
    if (weight <= 0.f)
    {
        // the arms are the game's (the Pip-Boy has the left one): the left hand keeps hold of its weapon
        if (arms.Left && pLeftGrip && IsComplete(aSlot.Left) && HasFingers(aSlot.Left))
            ApplyGrip(aSlot.Left, BlendGrip(ReadGrip(aSlot.Left), MirrorGrip(*pLeftGrip), gripWeight));
        return;
    }
    glm::vec3 forward = glm::normalize(arms.Forward);
    glm::vec3 right = glm::normalize(arms.Right3);
    glm::vec3 up = glm::normalize(arms.Up);
    glm::vec3 origin = aSlot.pChest ? NodeTools::GetWorldPosition(aSlot.pChest) : arms.Origin;
    glm::vec3 hangTo = arms.HangTo;
    std::optional<Transform> cameraWorld;
    if (aSlot.FirstPerson)
    {
        // The first person skeleton isn't in the world's space (its nodes read near the world's origin, 100,000 from the
        // player): the view's frame is its Camera bone, x right, y up, z back (Fallout4 - Meshes.ba2's first person
        // skeleton, as the Pip-Boy glance's clip has it), and the cable's fixing point goes from the world's view into it
        if (!aSlot.pCamera)
            return;
        const Transform camera = NodeTools::Read(aSlot.pCamera, NodeTools::kWorldOffset);
        cameraWorld = camera;
        const glm::vec3 offset = arms.HangTo - arms.Origin;
        const glm::vec3 view(glm::dot(offset, right), glm::dot(offset, forward), glm::dot(offset, up));
        origin = camera.Translation;
        right = glm::normalize(camera.Rows[0]);
        up = glm::normalize(camera.Rows[1]);
        forward = -glm::normalize(camera.Rows[2]);
        hangTo = origin + right * view.x + forward * view.y + up * view.z;
    }
    const glm::vec3 hold = aSlot.FirstPerson ? kHoldFirstPerson : kHoldThirdPerson;
    const glm::vec3 lowered = aSlot.FirstPerson ? kLowerFirstPerson : arms.ReloadDip ? kReloadThirdPerson : kLowerThirdPerson;
    const float dipTurn = !aSlot.FirstPerson && arms.ReloadDip ? kReloadThirdPersonTurn : 1.f;
    const auto toWorld = [&](const glm::vec3& acLocal) { return origin + right * acLocal.x + forward * acLocal.y + up * acLocal.z; };

    // the left hand's grip: its weapon's own, mirrored onto the left bones, eased in from where this frame's animation
    // has them (read before either arm moves)
    std::optional<Grip> leftGame;
    std::optional<Grip> leftGrip;
    if (arms.Left && IsComplete(aSlot.Left) && HasFingers(aSlot.Left))
        leftGame = ReadGrip(aSlot.Left);
    if (leftGame && pLeftGrip)
        leftGrip = BlendGrip(*leftGame, MirrorGrip(*pLeftGrip), gripWeight);

    // First person: the game's animation of the right arm this frame (read before either arm moves) as the right hand's
    // motion from its weapon's plain idle, both in the view: the idle's own breathing and fidgets (the gun tipped up to
    // 27 degrees to be looked at, the view turned up to 5: GunBehavior.hkx picks them at random), walking, jumping,
    // sprinting, the draw and the holster. The right gun's hold moves as the right hand does; the left's mirrored, a beat
    // behind, held where it is while the right gun's own shots kick it
    std::optional<DualWieldPose::Motion> rightMotion;
    if (cameraWorld && arms.pRightIdle && !arms.pRightIdle->Frames.empty() && IsComplete(aSlot.Right) && (arms.RightFollow > 0.f || arms.LeftFollow > 0.f))
        rightMotion = DualWieldPose::MotionBetween(arms.pRightIdle->Frames.front().Hand, NodeTools::Relative(*cameraWorld, NodeTools::Read(aSlot.Right.pHand, NodeTools::kWorldOffset)));
    if (rightMotion && (aSlot.FollowLog -= delta) <= 0.f)
    {
        // standing still in the plain idle this is near nothing (the clip is the game's own for that weapon)
        aSlot.FollowLog = 15.f;
        spdlog::info("Dual wield: the right arm's animation is {:.1f} units and {:.1f} deg from its weapon's plain idle (followed {:.2f} right, {:.2f} left)",
                     glm::length(rightMotion->Move), glm::degrees(glm::angle(rightMotion->Turn)), arms.RightFollow, arms.LeftFollow);
    }
    if (rightMotion && arms.LeftFollow > 0.f)
    {
        if (!arms.LeftFollowHeld)
            aSlot.LeftFollowed = DualWieldPose::BlendMotion(aSlot.LeftFollowed, DualWieldPose::MirrorMotion(*rightMotion), std::min(1.f, delta * kLeftFollowRate));
    }
    else
        aSlot.LeftFollowed = DualWieldPose::Motion{};
    const DualWieldPose::Motion none{};
    const auto handMotion = [&](bool aLeft) -> std::optional<DualWieldPose::Motion>
    {
        if (!cameraWorld)
            return std::nullopt;
        if (!aLeft)
        {
            if (!rightMotion || arms.RightFollow <= 0.f)
                return std::nullopt;
            return DualWieldPose::BlendMotion(none, *rightMotion, std::clamp(arms.RightFollow, 0.f, 1.f));
        }
        // its own clip's motion, then the right hand's it follows
        DualWieldPose::Motion motion = arms.LeftOwn;
        if (arms.LeftFollow > 0.f)
        {
            const DualWieldPose::Motion followed = DualWieldPose::BlendMotion(none, aSlot.LeftFollowed, std::clamp(arms.LeftFollow, 0.f, 1.f));
            motion.Turn = glm::normalize(followed.Turn * motion.Turn);
            motion.Move += followed.Move;
        }
        return motion;
    };

    const auto poseSide = [&](const Side& acSide, bool aLeft, float aKick)
    {
        const float mirror = aLeft ? -1.f : 1.f;
        const float sideWeight = aLeft ? weight : weight * std::clamp(arms.RightWeight, 0.f, 1.f);
        if (sideWeight <= 0.f)
            return;
        // where the weapon node sits on the hand: the grip's for the left, the game's for the right
        const Transform weaponOnHand = aLeft && leftGrip ? leftGrip->Weapon
                                                         : NodeTools::Relative(NodeTools::Read(acSide.pHand, NodeTools::kWorldOffset),
                                                                               NodeTools::Read(acSide.pWeapon, NodeTools::kWorldOffset));
        const auto grip = [&]()
        {
            if (aLeft && leftGrip)
                ApplyGrip(acSide, *leftGrip);
        };

        if (aLeft ? arms.LeftHang : arms.RightHang)
        {
            // up the cable from the shoulder joint itself (the shoulder raised with the arm overhead), the arm all but
            // straight, the elbow out and back; the gun turned only part of the way to point up the cable, the rest of
            // its turn the forearm's own (a wrist bent no further than wrists go)
            glm::vec3 shoulder = NodeTools::GetWorldPosition(acSide.pUpper);
            glm::vec3 along = hangTo - shoulder;
            along = glm::length(along) > 1e-3f ? glm::normalize(along) : up;
            t_stage = "hang: shoulder";
            LiftShoulder(acSide, along, up, sideWeight);
            t_stage = "hang: arm";
            shoulder = NodeTools::GetWorldPosition(acSide.pUpper);
            const float length = glm::distance(shoulder, NodeTools::GetWorldPosition(acSide.pFore)) +
                                 glm::distance(NodeTools::GetWorldPosition(acSide.pFore), NodeTools::GetWorldPosition(acSide.pHand));
            glm::vec3 side = glm::cross(along, up);
            side = glm::length(side) > 1e-3f ? glm::normalize(side) : right;
            const glm::vec3 top = glm::normalize(glm::cross(side, along));
            const glm::vec3 pole = right * mirror - forward * 0.5f;
            const float reach = length > 1.f ? length * kHangStraight : kHangReach;
            const glm::vec3 reachTo = shoulder + along * reach;
            if (!std::isfinite(reachTo.x) || !std::isfinite(reachTo.y) || !std::isfinite(reachTo.z))
            {
                // (seen 2026-10-25: the upper arm of no number in this step; say what made it, now and then)
                static std::atomic<int64_t> s_loggedAt{0};
                const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                if (now - s_loggedAt.load(std::memory_order_relaxed) > 2000)
                {
                    s_loggedAt = now;
                    spdlog::warn("Dual wield: the cable's arm not posed: fixed at ({:.0f}, {:.0f}, {:.0f}), shoulder ({:.0f}, {:.0f}, {:.0f}), along ({:.2f}, {:.2f}, {:.2f}), arm {:.1f}",
                                 hangTo.x, hangTo.y, hangTo.z, shoulder.x, shoulder.y, shoulder.z, along.x, along.y, along.z, length);
                }
                return;
            }
            PoseArm(acSide, reachTo, HandFor(weaponOnHand, aLeft, along, top), pole, sideWeight, kHangWristShare);
            grip();
            return;
        }
        const float phase = aSlot.Time * kSwayRate * 6.2831853f + (aLeft ? 1.3f : 0.f);
        glm::vec3 local(hold.x * mirror, hold.y, hold.z);
        local += glm::vec3(std::sin(phase) * kSwayAmount * 0.6f, 0.f, std::sin(phase * 0.5f) * kSwayAmount);
        local.y -= kKickBack * aKick;
        // the reload's dip: down, back and in along a curve
        const Dip dip = DipAt(aLeft ? LeftPhase(arms.Reload) : arms.Reload);
        const glm::vec3 dipOffset(lowered.x * mirror * dip.Down, lowered.y * dip.Back, lowered.z * dip.Down);
        local += dipOffset;

        // First person, the left weapon's own clip (its idle, an attack, the block): the right arm's place in the clip's
        // view, mirrored, put where the view is now (the hand by IK from this skeleton's shoulder, as this frame's
        // animation may have the chest elsewhere: the melee clips have it 14 units further ahead than the pistol's; the
        // elbow toward the clip's), with the clip's grip; the reload's dip on top
        if (aLeft && cameraWorld && arms.pLeftClip)
        {
            WeaponGrips::ArmFrame frame = arms.pLeftClip->At(arms.LeftClipTime);
            if (arms.pLeftClipFrom && arms.LeftClipBlend < 1.f)
                frame = WeaponGrips::Blend(arms.pLeftClipFrom->At(arms.LeftClipFromTime), frame, std::clamp(arms.LeftClipBlend, 0.f, 1.f));
            const Transform hand = NodeTools::Combine(*cameraWorld, MirrorInView(frame.Hand));
            const glm::vec3 elbow = cameraWorld->Apply(glm::vec3(-frame.Elbow.x, frame.Elbow.y, frame.Elbow.z));
            const glm::vec3 target = hand.Translation + (toWorld(dipOffset) - origin);
            const glm::vec3 shoulder = NodeTools::GetWorldPosition(acSide.pUpper);
            PoseArm(acSide, target, hand, elbow - (shoulder + target) * 0.5f, sideWeight);
            if (leftGame)
                ApplyGrip(acSide, BlendGrip(*leftGame, MirrorGrip(frame.HandGrip), sideWeight));
            return;
        }

        glm::vec3 ahead = forward;
        glm::vec3 top = up;
        const bool melee = aLeft && arms.LeftMelee;
        if (melee)
        {
            // first person positions moved to the third person hold's place
            const glm::vec3 leftHold3P(-kHoldThirdPerson.x, kHoldThirdPerson.y, kHoldThirdPerson.z);
            const glm::vec3 leftHold1P(-kHoldFirstPerson.x, kHoldFirstPerson.y, kHoldFirstPerson.z);
            const glm::vec3 viewShift = aSlot.FirstPerson ? glm::vec3(0.f) : leftHold3P - leftHold1P;

            // held up, blade first; swung from raised and back to across the front
            ahead = glm::normalize(forward * 0.35f + up);
            top = glm::normalize(-forward + up * 0.2f);
            if (arms.Swing >= 0.f)
            {
                const float s = std::clamp(arms.Swing, 0.f, 1.f);
                const float eased = s * s * (3.f - 2.f * s);
                const glm::vec3& from = arms.PowerSwing ? kPowerSwingFrom : kSwingFrom;
                const glm::vec3& to = arms.PowerSwing ? kPowerSwingTo : kSwingTo;
                local = from + (to - from) * eased + dipOffset + viewShift;
                ahead = glm::normalize(About(up, right, (arms.PowerSwing ? -2.2f : -1.9f) * eased) + forward * 0.2f);
                top = glm::normalize(glm::cross(right, ahead));
            }
            else if (arms.Guard > 0.f)
            {
                // raised across the face, the blade pointing right, its edge forward
                const float g = std::clamp(arms.Guard, 0.f, 1.f);
                const float eased = g * g * (3.f - 2.f * g);
                local = glm::mix(local, kGuard + dipOffset + viewShift, eased);
                ahead = glm::normalize(glm::mix(ahead, glm::normalize(right + up * 0.25f), eased));
                top = glm::normalize(glm::mix(top, forward, eased));
            }
        }
        else
        {
            if (aKick > 0.f)
            {
                ahead = About(ahead, right, kKickPitch * aKick);
                top = About(top, right, kKickPitch * aKick);
            }
            // the dip's turn: the muzzle tipped down, then the gun rolled outward about its barrel
            if (dip.Turn != 0.f)
            {
                ahead = About(ahead, right, -kDipPitch * dip.Turn * dipTurn);
                top = About(top, right, -kDipPitch * dip.Turn * dipTurn);
                top = About(top, ahead, kDipRoll * dip.Turn * dipTurn * mirror);
            }
        }

        // a gun's hold moved and turned as the hand's motion has it (the turn is the view's: put in the world's)
        glm::vec3 target = toWorld(local);
        if (const auto motion = melee ? std::nullopt : handMotion(aLeft))
        {
            const glm::quat view = ToQuat(*cameraWorld);
            const glm::quat turn = glm::normalize(view * motion->Turn * glm::inverse(view));
            target += cameraWorld->Apply(motion->Move, false);
            ahead = turn * ahead;
            top = turn * top;
        }

        const glm::vec3 pole = -up + right * (kPoleOut * mirror) - forward * kPoleBack;
        PoseArm(acSide, target, HandFor(weaponOnHand, aLeft, ahead, top), pole, sideWeight);
        grip();
    };

    // Third person, on top of the mirrored arm (or the game's right one): a shot's kick and the dip, by IK from where the
    // arm is (the elbow kept the way it bends)
    const auto nudge = [&](const Side& acSide, bool aLeft, float aKick)
    {
        const float sideWeight = aLeft ? weight : weight * std::clamp(arms.RightWeight, 0.f, 1.f);
        const Dip dip = DipAt(aLeft ? LeftPhase(arms.Reload) : arms.Reload);
        if (sideWeight <= 0.f || (aKick <= 0.f && dip.Down == 0.f && dip.Back == 0.f && dip.Turn == 0.f))
            return;
        const float mirror = aLeft ? -1.f : 1.f;
        const Transform hand = NodeTools::Read(acSide.pHand, NodeTools::kWorldOffset);
        const glm::vec3 offset = right * (lowered.x * mirror * dip.Down) + forward * (lowered.y * dip.Back - kKickBack * aKick) + up * (lowered.z * dip.Down);
        Transform goal = hand;
        Turn(goal, glm::mat3(glm::rotate(glm::mat4(1.f), kKickPitch * aKick - kDipPitch * dip.Turn * dipTurn, right)));
        Turn(goal, glm::mat3(glm::rotate(glm::mat4(1.f), kDipRoll * dip.Turn * dipTurn * mirror, forward)));
        const glm::vec3 shoulder = NodeTools::GetWorldPosition(acSide.pUpper);
        const glm::vec3 elbow = NodeTools::GetWorldPosition(acSide.pFore);
        PoseArm(acSide, hand.Translation + offset, goal, elbow - (shoulder + hand.Translation) * 0.5f, sideWeight);
    };

    // On the motorcycle (third person's skeleton, in either view): the right hand stays on the bars as the ride put it. The
    // left arm comes off its grip to the aim, the weapon's own grip on the hand, the elbow down and out as on foot; up at
    // the sights the hand goes in front of the eyes
    if (!aSlot.FirstPerson && arms.OnBike && !arms.LeftMelee)
    {
        if (arms.Left && IsComplete(aSlot.Left))
        {
            t_stage = "bike";
            if (pLeftGrip && HasFingers(aSlot.Left))
                ApplyGrip(aSlot.Left, BlendGrip(ReadGrip(aSlot.Left), MirrorGrip(*pLeftGrip), gripWeight));
            const float aim = std::clamp(arms.LeftAim, 0.f, 1.f) * weight;
            if (aim > 0.f && glm::length(arms.AimAt) > 1.f)
            {
                const glm::vec3 shoulder = NodeTools::GetWorldPosition(aSlot.Left.pUpper);
                const glm::vec3 elbow = NodeTools::GetWorldPosition(aSlot.Left.pFore);
                const glm::vec3 wrist = NodeTools::GetWorldPosition(aSlot.Left.pHand);
                const float length = glm::distance(shoulder, elbow) + glm::distance(elbow, wrist);
                glm::vec3 target = shoulder + glm::normalize(arms.AimAt - shoulder) * length * kAimReach;
                if (arms.BikeAds > 0.f)
                    target = glm::mix(target, arms.BikeSights, std::clamp(arms.BikeAds, 0.f, 1.f));
                const Transform weaponOnHand = NodeTools::Relative(NodeTools::Read(aSlot.Left.pHand, NodeTools::kWorldOffset),
                                                                   NodeTools::Read(aSlot.Left.pWeapon, NodeTools::kWorldOffset));
                const glm::vec3 pole = -up - right * kPoleOut - forward * kPoleBack;
                PoseArm(aSlot.Left, target, HandFor(weaponOnHand, true, glm::normalize(arms.AimAt - target), up), pole, aim);
            }
            nudge(aSlot.Left, true, arms.LeftKick);
        }
        return;
    }

    // Third person: the left arm mirrors the right one (the game's, or a melee clip's), the left weapon's own grip on it
    // (the clip's fingers while one plays: they hold a one-handed weapon), then its kick and the dip; the right arm the
    // game's but for the dip; on the grapple's cable as ever
    if (!aSlot.FirstPerson && arms.MirrorRight && !aSlot.Limb.empty())
    {
        if (arms.Left && IsComplete(aSlot.Left))
        {
            if (arms.LeftHang)
                poseSide(aSlot.Left, true, arms.LeftKick);
            else
            {
                glm::vec3 middle{0.f};
                glm::vec3 across{0.f};
                // the body first (an attack's turn of it), the arms on it
                t_stage = "body";
                PoseBody(aSlot, weight);
                t_stage = "mirror";
                // (the stride by the body's own forward: the root's, not the view's)
                glm::vec3 bodyForward = aSlot.pRoot ? NodeTools::Read(aSlot.pRoot, NodeTools::kWorldOffset).Rows[1] : forward;
                bodyForward = glm::length(bodyForward) > 1e-4f ? glm::normalize(bodyForward) : forward;
                const bool mirrored = MirrorLimb(aSlot, weight, bodyForward, delta, middle, across);
                t_stage = "grip";
                const float clipWeight = arms.pLimbClip ? std::clamp(arms.LimbWeight, 0.f, 1.f) : 0.f;
                // the left hand holds its weapon by its grip whatever the arm does (every melee weapon held properly in
                // the left hand; the clips' own hand, mirrored and eased in from the game's, left it in between)
                if (pLeftGrip && HasFingers(aSlot.Left))
                    ApplyGrip(aSlot.Left, BlendGrip(ReadGrip(aSlot.Left), MirrorGrip(*pLeftGrip), gripWeight));
                // a hand moved across by IK, its turn and the way its elbow bends kept
                const auto shift = [&](const Side& acSide, const glm::vec3& acBy)
                {
                    const Transform hand = NodeTools::Read(acSide.pHand, NodeTools::kWorldOffset);
                    const glm::vec3 shoulder = NodeTools::GetWorldPosition(acSide.pUpper);
                    const glm::vec3 elbow = NodeTools::GetWorldPosition(acSide.pFore);
                    PoseArm(acSide, hand.Translation + acBy, hand, elbow - (shoulder + hand.Translation) * 0.5f, 1.f);
                };
                const float apart = (1.f - clipWeight) * weight;
                // a gun in the right hand on its own side of the body's middle (the game's pistol holds it there for
                // two hands, which crossed the arms in third person with two pistols)
                t_stage = "right hand apart";
                if (mirrored && apart > 0.f && arms.RightHoldsGun && !arms.RightHang && IsComplete(aSlot.Right))
                {
                    const float rightSide = glm::dot(NodeTools::GetWorldPosition(aSlot.Right.pHand) - middle, across);
                    if (const float push = (kHandOffMiddle - rightSide) * apart; push > 0.f)
                        shift(aSlot.Right, across * push);
                }
                // the left gun aimed where its shots go (the left arm pointed to shoot, shown as it is in
                // third person): as much as the right gun is aimed (its barrel on its line to
                // the crosshair) or more (LeftAim: after its shots, its button held, hanging); the arm out from the
                // shoulder toward the crosshair, the muzzle on it, the elbow down and out
                t_stage = "aim";
                if (mirrored && apart > 0.f && !arms.LeftMelee && glm::length(arms.AimAt) > 1.f)
                {
                    float aim = std::clamp(arms.LeftAim, 0.f, 1.f);
                    if (arms.RightHoldsGun && aSlot.Right.pWeapon)
                    {
                        const Transform gun = NodeTools::Read(aSlot.Right.pWeapon, NodeTools::kWorldOffset);
                        const glm::vec3 toAim = arms.AimAt - gun.Translation;
                        if (glm::length(toAim) > 1.f && glm::length(gun.Rows[1]) > 1e-4f)
                            aim = std::max(aim, glm::smoothstep(kAimLooseCos, kAimTightCos, glm::dot(glm::normalize(gun.Rows[1]), glm::normalize(toAim))));
                    }
                    aim *= apart;
                    if (aim > 0.f)
                    {
                        const glm::vec3 shoulder = NodeTools::GetWorldPosition(aSlot.Left.pUpper);
                        const glm::vec3 elbow = NodeTools::GetWorldPosition(aSlot.Left.pFore);
                        const glm::vec3 wrist = NodeTools::GetWorldPosition(aSlot.Left.pHand);
                        const float length = glm::distance(shoulder, elbow) + glm::distance(elbow, wrist);
                        const glm::vec3 target = shoulder + glm::normalize(arms.AimAt - shoulder) * length * kAimReach;
                        const Transform weaponOnHand = NodeTools::Relative(NodeTools::Read(aSlot.Left.pHand, NodeTools::kWorldOffset),
                                                                           NodeTools::Read(aSlot.Left.pWeapon, NodeTools::kWorldOffset));
                        const glm::vec3 pole = -up - right * kPoleOut - forward * kPoleBack;
                        PoseArm(aSlot.Left, target, HandFor(weaponOnHand, true, glm::normalize(arms.AimAt - target), up), pole, aim);
                    }
                }
                // the left hand on its own side: off the body's middle, and a hand and a weapon from the right one (not
                // while a melee clip plays: an attack sweeps across)
                t_stage = "left hand apart";
                if (mirrored && apart > 0.f)
                {
                    const float leftSide = glm::dot(NodeTools::GetWorldPosition(aSlot.Left.pHand) - middle, across);
                    const float rightSide = glm::dot(NodeTools::GetWorldPosition(aSlot.Right.pHand) - middle, across);
                    if (const float push = (leftSide - std::min(-kHandOffMiddle, rightSide - kHandsApart)) * apart; push > 0.f)
                        shift(aSlot.Left, -across * push);
                }
                t_stage = "kick and dip";
                nudge(aSlot.Left, true, arms.LeftKick);
                // the left weapon's node off the hand (seen 2026-10-25 with the security baton, which didn't stay in the
                // left hand): said now and then
                if (aSlot.Left.pWeapon)
                {
                    const float off = glm::distance(NodeTools::GetWorldPosition(aSlot.Left.pWeapon), NodeTools::GetWorldPosition(aSlot.Left.pHand));
                    static std::atomic<int64_t> s_loggedAt{0};
                    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                    if (off > 25.f && now - s_loggedAt.load(std::memory_order_relaxed) > 2000)
                    {
                        s_loggedAt = now;
                        spdlog::info("Dual wield: the left weapon's node is {:.0f} from the hand (third person, {} clip {:.2f} on)", off,
                                     arms.pLimbClip ? "a" : "no", arms.pLimbClip ? std::clamp(arms.LimbWeight, 0.f, 1.f) : 0.f);
                    }
                }
            }
        }
        if (arms.Right && IsComplete(aSlot.Right))
        {
            if (arms.RightHang)
                poseSide(aSlot.Right, false, arms.RightKick);
            else
                nudge(aSlot.Right, false, 0.f);
        }
        return;
    }

    if (arms.Left && IsComplete(aSlot.Left))
        poseSide(aSlot.Left, true, arms.LeftKick);
    if (arms.Right && IsComplete(aSlot.Right))
        poseSide(aSlot.Right, false, arms.RightKick);
}

// The angle between two transforms' turns (degrees)
float AngleBetween(const Transform& acA, const Transform& acB) noexcept
{
    float trace = 0.f;
    for (int i = 0; i < 3; ++i)
        trace += glm::dot(acA.Rows[i], acB.Rows[i]);
    return glm::degrees(std::acos(std::clamp((trace - 1.f) * 0.5f, -1.f, 1.f)));
}

// Our copy of the left weapon on a skeleton (OffHandModel::kModelName, under WeaponLeft), or nullptr
uint8_t* CopyOn(const Side& acSide) noexcept
{
    uint8_t* pCopy = nullptr;
    if (acSide.pWeapon)
    {
        NodeTools::ForEachChild(acSide.pWeapon, [&pCopy](uint8_t* apChild) {
            if (!pCopy && _stricmp(NodeTools::GetName(apChild), OffHandModel::kModelName) == 0)
                pCopy = apChild;
        });
    }
    return pCopy;
}

// Third person, now and then: where the left weapon's node is on the hand against its grip, and our copy on that node
// against its own place there (OffHandModel::OnWeaponLeft), as this frame's animation left them or as the pose put them
void LogLeftHold(const Slot& acSlot, const Grip* apGrip, const char* acpWhen) noexcept
{
    const Side& left = acSlot.Left;
    if (!left.pHand || !left.pWeapon)
        return;
    const Transform onHand = NodeTools::Relative(NodeTools::Read(left.pHand, NodeTools::kWorldOffset), NodeTools::Read(left.pWeapon, NodeTools::kWorldOffset));
    std::string grip = "no grip";
    if (apGrip)
    {
        const Transform want = Mirror(apGrip->Weapon);
        grip = fmt::format("{:.1f} from its grip, turned {:.0f} deg from it", glm::distance(onHand.Translation, want.Translation), AngleBetween(onHand, want));
    }
    std::string copy = "no copy on it";
    if (const uint8_t* pCopy = CopyOn(left))
    {
        const Transform local = NodeTools::Read(pCopy, NodeTools::kLocalOffset);
        copy = fmt::format("the copy {:.1f} off its place on it, turned {:.0f} deg from it", glm::length(local.Translation), AngleBetween(local, OffHandModel::OnWeaponLeft()));
    }
    spdlog::info("Dual wield: third person left hold, {}: WeaponLeft at ({:.1f}, {:.1f}, {:.1f}) on the hand, {}; {}", acpWhen, onHand.Translation.x, onHand.Translation.y,
                 onHand.Translation.z, grip, copy);
}

// Third person, after the pose: an arm's node (bone or skin helper, under either collarbone) further from its parent than
// any arm's bone is long (the collarbone's 19 the most) said now and then, by name: one run off as the skin helpers did
void CheckArmsAttached(const Slot& acSlot) noexcept
{
    constexpr float kFurthest = 40.f;
    static std::atomic<int64_t> s_loggedAt{0};
    const std::function<void(uint8_t*)> check = [&check](uint8_t* apNode)
    {
        NodeTools::ForEachChild(apNode, [&](uint8_t* apChild) {
            const char* pName = NodeTools::GetName(apChild);
            if (std::strncmp(pName, "LArm_", 5) != 0 && std::strncmp(pName, "RArm_", 5) != 0)
                return;
            const float distance = glm::distance(NodeTools::GetWorldPosition(apChild), NodeTools::GetWorldPosition(apNode));
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
            if (distance > kFurthest && now - s_loggedAt.load(std::memory_order_relaxed) > 3000)
            {
                s_loggedAt = now;
                spdlog::warn("Dual wield: {} is {:.0f} from {} after the pose (third person)", pName, distance, NodeTools::GetName(apNode));
            }
            check(apChild);
        });
    };
    for (const Side* pSide : {&acSlot.Left, &acSlot.Right})
    {
        if (pSide->pCollar)
            check(pSide->pCollar);
    }
}

bool SameTransform(const Transform& acA, const Transform& acB) noexcept
{
    return acA.Rows[0] == acB.Rows[0] && acA.Rows[1] == acB.Rows[1] && acA.Rows[2] == acB.Rows[2] && acA.Translation == acB.Translation && acA.Scale == acB.Scale;
}

// Before the pose: the arms' bones this frame's update hasn't touched, still exactly where the last pose wrote them (the
// tree walked as it is now: a node no longer there is never read). Each one's name said once
void FindUntouched(Slot& aSlot) noexcept
{
    aSlot.Untouched.clear();
    if (aSlot.LeftAs.empty())
        return;
    static std::mutex s_namedLock;
    static std::unordered_set<std::string> s_named;
    const std::function<void(uint8_t*)> walk = [&](uint8_t* apNode)
    {
        if (const auto it = aSlot.LeftAs.find(apNode); it != aSlot.LeftAs.end() && SameTransform(NodeTools::Read(apNode, NodeTools::kWorldOffset), it->second))
        {
            aSlot.Untouched.insert(apNode);
            std::scoped_lock lock(s_namedLock);
            if (s_named.insert(NodeTools::GetName(apNode)).second)
                spdlog::info("Dual wield: {} isn't moved by the game before the pose ({} person): it rides on its parent by its own place", NodeTools::GetName(apNode),
                             aSlot.FirstPerson ? "first" : "third");
        }
        NodeTools::ForEachChild(apNode, [&](uint8_t* apChild) { walk(apChild); });
    };
    for (const Side* pSide : {&aSlot.Left, &aSlot.Right})
    {
        if (pSide->pCollar)
            walk(pSide->pCollar);
    }
}

// After the pose: where it left the bones it wrote (each written this frame, so there)
void RememberWritten(Slot& aSlot) noexcept
{
    aSlot.LeftAs.clear();
    for (const uint8_t* pNode : aSlot.Written)
    {
        if (IsBoneName(pNode))
            aSlot.LeftAs[pNode] = NodeTools::Read(const_cast<uint8_t*>(pNode), NodeTools::kWorldOffset);
    }
}

// The arms posed, then our copy put back on its own place under WeaponLeft (whatever wrote to it: its nodes are named as
// no skeleton's, OffHandModel::kModelName), the left hand's hold said now and then (third person)
void PoseSlot(Slot& aSlot) noexcept
{
    FindUntouched(aSlot);
    aSlot.Written.clear();
    t_pUntouched = &aSlot.Untouched;
    t_pWritten = &aSlot.Written;
    const DualWieldPose::Arms& arms = aSlot.Arms;
    const Grip* pLeftGrip = !aSlot.FirstPerson && arms.pLeftGrip3P ? arms.pLeftGrip3P : arms.pLeftGrip;
    const auto now = Clock::now();
    const bool logHold = !aSlot.FirstPerson && arms.Left && now >= aSlot.NextHoldLog;
    if (logHold)
    {
        aSlot.NextHoldLog = now + std::chrono::seconds(3);
        LogLeftHold(aSlot, pLeftGrip, "as the game left it");
    }
    PoseArms(aSlot);
    if (uint8_t* pCopy = CopyOn(aSlot.Left))
    {
        t_stage = "the copy's place";
        const Transform old = NodeTools::Read(pCopy, NodeTools::kWorldOffset);
        const Transform local = OffHandModel::OnWeaponLeft();
        const Transform world = NodeTools::Combine(NodeTools::Read(aSlot.Left.pWeapon, NodeTools::kWorldOffset), local);
        if (!Refused(pCopy, world, &local))
        {
            NodeTools::Write(pCopy, NodeTools::kLocalOffset, local);
            NodeTools::Write(pCopy, NodeTools::kWorldOffset, world);
            Rebuild(pCopy, old);
        }
    }
    if (logHold)
        LogLeftHold(aSlot, pLeftGrip, "posed");
    if (aSlot.pLayer)
    {
        t_stage = "the layer over the arms";
        t_rideOnLocals = !aSlot.FirstPerson;
        aSlot.pLayer(aSlot.pRoot, aSlot.FirstPerson);
        t_rideOnLocals = false;
    }
    if (!aSlot.FirstPerson)
        CheckArmsAttached(aSlot);
    RememberWritten(aSlot);
    t_pUntouched = nullptr;
    t_pWritten = nullptr;
}
} // namespace

namespace
{
// The slot of that skeleton, made if it has none (the lock held); nullptr with all of them taken
Slot* SlotFor(uint8_t* apRoot, bool aFirstPerson) noexcept
{
    Slot* pSlot = nullptr;
    for (auto& slot : s_slots)
    {
        if (slot.pRoot == apRoot)
        {
            pSlot = &slot;
            break;
        }
    }
    if (!pSlot)
    {
        for (auto& slot : s_slots)
        {
            if (!slot.pRoot)
            {
                pSlot = &slot;
                break;
            }
        }
        if (!pSlot)
            return nullptr;
        static BSFixedString s_chest("Chest");
        static BSFixedString s_camera("Camera");
        *pSlot = Slot{};
        pSlot->pRoot = apRoot;
        s_roots[static_cast<size_t>(pSlot - s_slots.data())] = apRoot;
        pSlot->Left = FindSide(apRoot, true);
        pSlot->Right = FindSide(apRoot, false);
        pSlot->pChest = NodeTools::GetObjectByName(apRoot, s_chest);
        pSlot->pCamera = NodeTools::GetObjectByName(apRoot, s_camera);
        if (!aFirstPerson)
        {
            static BSFixedString s_rightFoot("RLeg_Foot");
            static BSFixedString s_leftFoot("LLeg_Foot");
            pSlot->Limb = FindLimb(apRoot);
            pSlot->pRightFoot = NodeTools::GetObjectByName(apRoot, s_rightFoot);
            pSlot->pLeftFoot = NodeTools::GetObjectByName(apRoot, s_leftFoot);
            for (size_t i = 0; i < kBodyBones; ++i)
                pSlot->Body[i] = NodeTools::GetObjectByName(apRoot, BSFixedString(kBodyNodes[i]));
        }
        spdlog::info("Dual wield: arms pose on {} skeleton {}: left arm {}, right arm {}, left fingers {}{}{}", aFirstPerson ? "the first person" : "a third person", fmt::ptr(apRoot),
                     IsComplete(pSlot->Left) ? "found" : "missing", IsComplete(pSlot->Right) ? "found" : "missing",
                     HasFingers(pSlot->Left) ? "found" : "missing (the left hand keeps the game's grip)",
                     aFirstPerson && !pSlot->pCamera ? ", no Camera bone (not posed)" : "",
                     aFirstPerson ? std::string() : fmt::format(", {} arm bones mirrored", pSlot->Limb.size()));
    }
    pSlot->FirstPerson = aFirstPerson;
    return pSlot;
}
} // namespace

namespace DualWieldPose
{
void Set(uint8_t* apRoot, bool aFirstPerson, const Arms& acArms) noexcept
{
    if (!apRoot)
        return;
    std::scoped_lock _{s_lock};
    Slot* pSlot = SlotFor(apRoot, aFirstPerson);
    if (!pSlot)
        return;
    pSlot->Arms = acArms;
    pSlot->ArmsSet = true;
    s_any = true;
}

void Clear(uint8_t* apRoot) noexcept
{
    std::scoped_lock _{s_lock};
    bool any = false;
    for (size_t i = 0; i < s_slots.size(); ++i)
    {
        if (s_slots[i].pRoot == apRoot)
        {
            if (s_slots[i].pLayer)
            {
                // a layer goes on without the arms
                s_slots[i].Arms = Arms{};
                s_slots[i].ArmsSet = false;
            }
            else
            {
                s_slots[i] = Slot{};
                s_roots[i] = nullptr;
            }
        }
        any = any || s_slots[i].pRoot;
    }
    s_any = any;
}

void SetLayer(uint8_t* apRoot, bool aFirstPerson, Layer apLayer) noexcept
{
    if (!apRoot)
        return;
    std::scoped_lock _{s_lock};
    Slot* pSlot = SlotFor(apRoot, aFirstPerson);
    if (!pSlot)
        return;
    pSlot->pLayer = apLayer;
    s_any = true;
}

void ClearLayer(uint8_t* apRoot) noexcept
{
    std::scoped_lock _{s_lock};
    bool any = false;
    for (size_t i = 0; i < s_slots.size(); ++i)
    {
        if (s_slots[i].pRoot == apRoot)
        {
            s_slots[i].pLayer = nullptr;
            if (!s_slots[i].ArmsSet)
            {
                s_slots[i] = Slot{};
                s_roots[i] = nullptr;
            }
        }
        any = any || s_slots[i].pRoot;
    }
    s_any = any;
}

bool HasArms(uint8_t* apRoot) noexcept
{
    std::scoped_lock _{s_lock};
    for (const auto& slot : s_slots)
    {
        if (slot.pRoot && slot.pRoot == apRoot)
            return slot.ArmsSet && (slot.Arms.Weight > 0.f || slot.Arms.GripWeight > 0.f);
    }
    return false;
}

void PlaceBone(uint8_t* apNode, const NodeTools::Transform& acWorld) noexcept
{
    Place(apNode, acWorld);
}

void RestOnLocals(uint8_t* apTop) noexcept
{
    const uint8_t* pParent = apTop ? NodeTools::GetParent(apTop) : nullptr;
    if (!pParent)
        return;
    const bool was = t_rideOnLocals;
    t_rideOnLocals = true;
    const Transform old = NodeTools::Read(apTop, NodeTools::kWorldOffset);
    const Transform local = NodeTools::Read(apTop, NodeTools::kLocalOffset);
    const Transform world = NodeTools::Combine(NodeTools::Read(pParent, NodeTools::kWorldOffset), local);
    if (!Refused(apTop, world, &local))
    {
        NodeTools::Write(apTop, NodeTools::kWorldOffset, world);
        Wrote(apTop);
        Rebuild(apTop, old);
    }
    t_rideOnLocals = was;
}

void ReachArm(uint8_t* apUpper, uint8_t* apFore, uint8_t* apHand, const glm::vec3& acTarget, const NodeTools::Transform& acGoal, const glm::vec3& acPole, float aWeight) noexcept
{
    Side side{};
    side.pUpper = apUpper;
    side.pFore = apFore;
    side.pHand = apHand;
    PoseArm(side, acTarget, acGoal, acPole, aWeight);
}

void ClearAll() noexcept
{
    std::scoped_lock _{s_lock};
    s_slots.fill(Slot{});
    for (auto& root : s_roots)
        root = nullptr;
    s_any = false;
}

void OnBoneTreeUpdated(uint8_t* apTree) noexcept
{
    if (!s_any.load(std::memory_order_relaxed) || !apTree)
        return;
    uint8_t* pRoot = NodeTools::GetParent(apTree);
    if (!pRoot || std::none_of(s_roots.begin(), s_roots.end(), [pRoot](const std::atomic<uint8_t*>& acRoot) { return acRoot.load(std::memory_order_relaxed) == pRoot; }))
        return;
    // waited for, never skipped: a frame without the pose would show the game's arms for that frame (the main thread
    // holds it only to copy a pose in)
    std::scoped_lock lock(s_lock);
    for (auto& slot : s_slots)
    {
        if (slot.pRoot && slot.pRoot == pRoot)
            PoseSlot(slot);
    }
}

uint32_t TakePoseCount(uint8_t* apRoot) noexcept
{
    std::scoped_lock _{s_lock};
    for (auto& slot : s_slots)
    {
        if (slot.pRoot && slot.pRoot == apRoot)
            return std::exchange(slot.Posed, 0u);
    }
    return 0;
}

Motion MotionBetween(const NodeTools::Transform& acFrom, const NodeTools::Transform& acTo) noexcept
{
    // the rows are each axis' direction in the view: as a matrix's columns, the hand's turn; the motion's turn takes the
    // one to the other
    Motion motion{};
    motion.Turn = glm::normalize(ToQuat(acTo) * glm::inverse(ToQuat(acFrom)));
    motion.Move = acTo.Translation - acFrom.Translation;
    return motion;
}

Motion MirrorMotion(const Motion& acRight) noexcept
{
    // across the view's x: the axis (x, y, z) to (-x, y, z), and the turn's sense over, so the quaternion's x stays and
    // its y and z change sign
    Motion left = acRight;
    left.Turn.y = -left.Turn.y;
    left.Turn.z = -left.Turn.z;
    left.Move.x = -left.Move.x;
    return left;
}

Motion BlendMotion(const Motion& acFrom, const Motion& acTo, float aT) noexcept
{
    Motion blended{};
    blended.Turn = glm::normalize(glm::slerp(acFrom.Turn, acTo.Turn, aT));
    blended.Move = glm::mix(acFrom.Move, acTo.Move, aT);
    return blended;
}

float DipPhase(float aTime, float aDown, float aHold, float aUp) noexcept
{
    if (aTime < 0.f)
        return -1.f;
    if (aTime < aDown)
        return kDipDown * aTime / std::max(aDown, 1e-3f);
    if (aTime < aDown + aHold)
        return kDipDown + (1.f - kDipUp - kDipDown) * (aTime - aDown) / std::max(aHold, 1e-3f);
    if (aTime < aDown + aHold + aUp)
        return 1.f - kDipUp + kDipUp * (aTime - aDown - aHold) / std::max(aUp, 1e-3f);
    return -1.f;
}
} // namespace DualWieldPose
