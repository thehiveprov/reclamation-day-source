#include <HivemindPCH.h>

#include <BetterInteractClip.h>
#include <DualWieldPose.h>
#include <OffHandModel.h>

#include <Misc/BSFixedString.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <mutex>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using NodeTools::Transform;
using BetterInteractClip::Item;
using BetterInteractClip::Kind;
using BetterInteractClip::View;

// A motion's key: when, where the hand is, the way its fingers point and its palm faces, in the motion's frame (x right, y up,
// z back: first person the view's, third person the same directions at the chest)
struct Key
{
    float T;
    float Place[3];
    float Fingers[3];
    float Palm[3];
};

// A hand shape coming on: which, from when, on by when
struct ShapeStep
{
    int Shape;
    float From;
    float To;
};

struct Motion
{
    const Key* pKeys;
    int KeyCount;
    // the key where it touches (it's moved toward what it reaches for), how far the fingertips are past the wrist there
    int Contact;
    float Tip;
    // the shoulder going with the arm: at its most then, back by then, by that much
    float ShoulderPeak;
    float ShoulderFall;
    float ShoulderPush[3];
    const ShapeStep* pShapes;
    int ShapeCount;
    float BlendIn;
    float BlendOut;
    // the item's in the hand from then until then (below 0: it holds no item)
    float Take;
    float Release;
    // third person: how far the body leans into it at the shoulder's peak (radians)
    float Lean;
};

#include "BetterInteractTable.inl"

// The contact goes at most this far toward what it reaches for (units), turning the hand's aim this share of that
constexpr float kContactMost = 15.f;
constexpr float kAimShare = 0.65f;
// Third person: the body's frame turned toward what it reaches for by at most this (radians), and what's this far below the
// chest makes the whole lean (under it, in proportion, down to this share)
constexpr float kFaceMost = 1.2f;
constexpr float kLeanFullBelow = 45.f;
constexpr float kLeanLeast = 0.4f;
// The arm never quite straight (of its length)
constexpr float kReachMost = 0.97f;
// A new hand takes over from one under way in this long (s), and a hand asked off fades in this long
constexpr float kCarry = 0.12f;
constexpr float kFade = 0.10f;

// What makes it loose, none of it in the keys (animation-feel: smoothing must never cost motion). The hand is left behind by
// the view's turns and steps (these shares of them) and comes back on a spring that overshoots (its rate, how damped), at most
// this far away (units)
constexpr float kTurnLag = 0.55f;
constexpr float kMoveLag = 0.30f;
constexpr float kSpringRate = 28.f;
constexpr float kSpringDamping = 0.42f;
constexpr float kSpringMost = 7.f;
// The game's own motion of that hand comes through (walking, breathing, a gun's kick): what's left of it after this slow
// follower (per s), this share of it, at most this far
constexpr float kGameFollow = 3.8f;
constexpr float kGameShare = 0.7f;
constexpr float kGameMost = 5.f;
// A little life in the hand itself (units, cycles a second)
constexpr float kSwayAmount = 0.35f;
constexpr float kSwayRate = 0.9f;
// The wrist trails the hand's way (radians per unit a second, at most, the speed read this fast per s)
constexpr float kTrail = 0.0011f;
constexpr float kTrailMost = 0.32f;
constexpr float kTrailRate = 20.f;
// The fingers drag behind in a wave: how fast each one (index .. little) follows the hand's speed out of its palm per s, how
// far that bends it (radians per unit a second, at most either way), each joint's share of that
constexpr float kFingerFollow[4] = {30.f, 22.f, 16.f, 12.f};
constexpr float kFingerDrag = 0.0012f;
constexpr float kFingerDragOpen = -0.12f;
constexpr float kFingerDragCurl = 0.22f;
constexpr float kFingerJointShare[3] = {1.f, 0.7f, 0.4f};
// A shape comes on past its mark and settles (of the shape, over this much of its time past its end)
constexpr float kShapeOvershoot = 0.08f;
constexpr float kShapeSettle = 0.6f;
// First person, the other arm (and its weapon) goes with the body's reach: all of it this long (s), this far and this turned at
// its peak (units; radians about the view's x, y, z), by the motion
constexpr float kBodyLength = 0.71f;
const glm::vec3 kBodyMove{1.5f, -2.5f, 2.8f};
const glm::vec3 kBodyTurn{-0.044f, -0.052f, 0.070f};
constexpr float kBodyShare[static_cast<size_t>(Kind::kCount)] = {1.f, 0.5f, 0.7f};
// An item flies from where it lay to the hand in this time (s), this high an arc for how far (of the way, at most, units)
constexpr float kPullFrom = 0.08f;
constexpr float kPullTo = 0.20f;
constexpr float kPullArc = 0.12f;
constexpr float kPullArcMost = 6.f;
// ... and in the hand it's at most this long (units) unless it's a weapon; put away shrinking to this share of its size, from
// this long before the motion's release to this long after its last key
constexpr float kHoldMost = 24.f;
constexpr float kHoldLeast = 0.25f;
constexpr float kPutAway = 0.25f;
constexpr float kPutAwayBefore = 0.05f;
constexpr float kPutAwayAfter = 0.03f;

constexpr int kFingerBones = 15;

struct Bones
{
    uint8_t* pRoot{nullptr};
    uint8_t* pCollar{nullptr};
    uint8_t* pUpper{nullptr};
    uint8_t* pFore1{nullptr};
    uint8_t* pFore2{nullptr};
    uint8_t* pFore3{nullptr};
    uint8_t* pHand{nullptr};
    uint8_t* pWeaponLeft{nullptr};
    uint8_t* pRightCollar{nullptr};
    uint8_t* pCamera{nullptr};
    uint8_t* pSpine2{nullptr};
    uint8_t* pChest{nullptr};
    uint8_t* pNeck{nullptr};
    uint8_t* pHead{nullptr};
    std::array<uint8_t*, kFingerBones> pFingers{};

    // A skeleton can be rebuilt while it plays: everything it touches is kept until it's let go
    template <class TVisit> void ForEach(const TVisit& acVisit) const noexcept
    {
        for (uint8_t* pNode : {pRoot, pCollar, pUpper, pFore1, pFore2, pFore3, pHand, pWeaponLeft, pRightCollar, pCamera, pSpine2, pChest, pNeck, pHead})
        {
            if (pNode)
                acVisit(pNode);
        }
        for (uint8_t* pNode : pFingers)
        {
            if (pNode)
                acVisit(pNode);
        }
    }
};

// Third person a bone tree updates about twice a frame, some of its passes leaving some bones' worlds as they were, and the animation never writes some bones' locals at all (the
// fingers). A local worked out from two worlds of different passes picks up how far the body moved between them, and on a bone the animation doesn't write it stays and adds up:
// seen 11-05, "the hand stretches all over the place", the log's "LArm_Finger13 is 1007225 from LArm_Finger12 after the pose". So third person each pass starts from what the game
// last gave these bones as locals: put back first, the worlds worked out from the top, and the sum is then just what it is in first person, on a skeleton all of one moment.
constexpr size_t kBodyBones = 4;
constexpr size_t kArmBones = 6;
constexpr size_t kBasedBones = kBodyBones + kArmBones + kFingerBones;
// (further from its parent than any of these is: not the game's)
constexpr float kBoneFurthest = 40.f;

struct Play
{
    uint8_t* pRoot{nullptr};
    bool FirstPerson{false};
    Kind Which{Kind::kGrab};
    const Motion* pMotion{nullptr};
    Bones Bone{};
    float Time{0.f};
    float End{0.f};
    float FadeAt{-1.f};
    bool Done{false};
    Clock::time_point Last{};
    Clock::time_point Started{};
    uint32_t Frames{0};
    bool LoggedContact{false};
    bool HasTarget{false};
    glm::vec3 Target{0.f};
    float ContactMoved{0.f};
    View Camera{};
    glm::vec3 BodyForward{0.f, 1.f, 0.f};

    Item Holding{};
    uint8_t* pHolder{nullptr};

    // from the one it took over from
    bool Carrying{false};
    glm::vec3 CarryPlace{0.f};
    glm::vec3 CarryFingers{0.f, 1.f, 0.f};
    glm::vec3 CarryPalm{0.f, 0.f, 1.f};
    float CarryWeight{0.f};
    std::array<glm::quat, kFingerBones> CarryBones{};

    // the loose part: the spring, the frame's last axes and origin in the world, the game's hand followed, the hand's way and
    // the speed each finger's dragged by
    glm::vec3 Spring{0.f};
    glm::vec3 SpringSpeed{0.f};
    bool HasBasis{false};
    glm::vec3 LastX{1.f, 0.f, 0.f};
    glm::vec3 LastY{0.f, 1.f, 0.f};
    glm::vec3 LastZ{0.f, 0.f, 1.f};
    glm::vec3 LastOrigin{0.f};
    bool HasGameLow{false};
    glm::vec3 GameLow{0.f};
    bool HasLastPlace{false};
    glm::vec3 LastPlace{0.f};
    glm::vec3 Way{0.f};
    std::array<float, 4> FingerDrag{};

    // what it did last (the next one takes over from it)
    glm::vec3 OutPlace{0.f};
    glm::vec3 OutFingers{0.f, 1.f, 0.f};
    glm::vec3 OutPalm{0.f, 0.f, 1.f};
    float OutWeight{0.f};
    std::array<glm::quat, kFingerBones> OutBones{};

    // the game's locals (third person): spine to head, collarbone to hand, fingers
    std::array<uint8_t*, kBasedBones> pBased{};
    std::array<Transform, kBasedBones> Base{};
    std::array<Transform, kBasedBones> Written{};
    bool HasBases{false};
    bool Restored{false};
    bool BodyMoves{false};
    uint32_t WroteBody{0};
    uint32_t WroteArm{0};
    uint32_t WroteFingers{0};
};

std::mutex s_lock;
std::vector<Play> s_plays;

const Motion& MotionFor(Kind aKind, bool aFirstPerson) noexcept
{
    switch (aKind)
    {
    case Kind::kPress: return aFirstPerson ? kPress : kPress3P;
    case Kind::kPush: return aFirstPerson ? kPush : kPush3P;
    default: return aFirstPerson ? kGrab : kGrab3P;
    }
}

const char* NameOf(Kind aKind) noexcept
{
    switch (aKind)
    {
    case Kind::kPress: return "press";
    case Kind::kPush: return "push";
    default: return "grab";
    }
}

float Smooth(float aX) noexcept
{
    const float x = std::clamp(aX, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

glm::vec3 V(const float (&acV)[3]) noexcept
{
    return glm::vec3(acV[0], acV[1], acV[2]);
}

glm::vec3 Limited(const glm::vec3& acV, float aMost) noexcept
{
    const float length = glm::length(acV);
    return length > aMost && length > 1e-6f ? acV * (aMost / length) : acV;
}

// A rotation's quaternion from where a transform's axes point (its rows, as NiTransform stores them), and back
glm::quat ToQuat(const Transform& acTransform) noexcept
{
    return glm::quat_cast(glm::mat3(acTransform.Rows[0], acTransform.Rows[1], acTransform.Rows[2]));
}

void SetQuat(Transform& aTransform, const glm::quat& acQ) noexcept
{
    const glm::mat3 m = glm::mat3_cast(glm::normalize(acQ));
    aTransform.Rows[0] = m[0];
    aTransform.Rows[1] = m[1];
    aTransform.Rows[2] = m[2];
}

glm::mat3 RowsOf(const Transform& acTransform) noexcept
{
    return glm::mat3(acTransform.Rows[0], acTransform.Rows[1], acTransform.Rows[2]);
}

// aU of the way, the short way round (a little past the end is fine: a shape's overshoot)
glm::quat NLerp(const glm::quat& acFrom, const glm::quat& acTo, float aU) noexcept
{
    const glm::quat to = glm::dot(acFrom, acTo) < 0.f ? -acTo : acTo;
    return glm::normalize(glm::quat(acFrom.w + (to.w - acFrom.w) * aU, acFrom.x + (to.x - acFrom.x) * aU, acFrom.y + (to.y - acFrom.y) * aU, acFrom.z + (to.z - acFrom.z) * aU));
}

// The turn taking one direction to another the short way (not for near opposite ones: small moves here)
glm::quat FromTo(const glm::vec3& acFrom, const glm::vec3& acTo) noexcept
{
    const glm::vec3 a = glm::normalize(acFrom), b = glm::normalize(acTo);
    const glm::vec3 axis = glm::cross(a, b);
    return glm::normalize(glm::quat(1.f + glm::dot(a, b), axis.x, axis.y, axis.z));
}

// The motion's frame: x right, y up, z back
struct Frame
{
    glm::vec3 Origin{0.f};
    glm::vec3 X{1.f, 0.f, 0.f};
    glm::vec3 Y{0.f, 1.f, 0.f};
    glm::vec3 Z{0.f, 0.f, 1.f};

    [[nodiscard]] glm::vec3 Dir(const glm::vec3& acV) const noexcept { return X * acV.x + Y * acV.y + Z * acV.z; }
    [[nodiscard]] glm::vec3 At(const glm::vec3& acV) const noexcept { return Origin + Dir(acV); }
    [[nodiscard]] glm::vec3 Of(const glm::vec3& acWorld) const noexcept
    {
        const glm::vec3 v = acWorld - Origin;
        return glm::vec3(glm::dot(v, X), glm::dot(v, Y), glm::dot(v, Z));
    }
};

// A world direction in the view's terms (x right, y up, z back)
glm::vec3 InView(const View& acView, const glm::vec3& acV) noexcept
{
    return glm::vec3(glm::dot(acV, acView.Right), glm::dot(acV, acView.Up), -glm::dot(acV, acView.Forward));
}

// The motion's keys at a time: a Catmull-Rom through them, as make_hands.py's drawing has it
glm::vec3 SplineAt(const Motion& acMotion, float (Key::*apField)[3], float aT) noexcept
{
    const Key* pKeys = acMotion.pKeys;
    const int count = acMotion.KeyCount;
    const float t = std::clamp(aT, pKeys[0].T, pKeys[count - 1].T);
    int i = 0;
    while (i < count - 2 && t >= pKeys[i + 1].T)
        ++i;
    const Key& k0 = pKeys[std::max(i - 1, 0)];
    const Key& k1 = pKeys[i];
    const Key& k2 = pKeys[i + 1];
    const Key& k3 = pKeys[std::min(i + 2, count - 1)];
    const glm::vec3 p0 = V(k0.*apField), p1 = V(k1.*apField), p2 = V(k2.*apField), p3 = V(k3.*apField);
    const float span = k2.T - k1.T;
    const float u = (t - k1.T) / std::max(span, 1e-3f);
    const glm::vec3 m1 = (p2 - p0) / std::max(k2.T - k0.T, 1e-3f) * span;
    const glm::vec3 m2 = (p3 - p1) / std::max(k3.T - k1.T, 1e-3f) * span;
    const float u2 = u * u, u3 = u2 * u;
    return (2.f * u3 - 3.f * u2 + 1.f) * p1 + (u3 - 2.f * u2 + u) * m1 + (-2.f * u3 + 3.f * u2) * p2 + (u3 - u2) * m2;
}

// When the shoulder (and the third person body) has gone with the arm: up to its peak, back by its fall
float ShoulderAt(const Motion& acMotion, float aT) noexcept
{
    if (aT < acMotion.ShoulderPeak)
        return Smooth(aT / acMotion.ShoulderPeak);
    return 1.f - Smooth((aT - acMotion.ShoulderPeak) / std::max(acMotion.ShoulderFall - acMotion.ShoulderPeak, 1e-3f));
}

// Third person: the body's two upper bones turned forward about its right, each bone above them where it was on its parent
void LeanBody(const Bones& acBones, const glm::vec3& acRight, float aAngle) noexcept
{
    if (!acBones.pSpine2 || !acBones.pChest || std::abs(aAngle) < 1e-3f)
        return;
    const Transform chestLocal = NodeTools::Relative(NodeTools::Read(acBones.pSpine2, NodeTools::kWorldOffset), NodeTools::Read(acBones.pChest, NodeTools::kWorldOffset));
    Transform neckLocal{}, headLocal{};
    if (acBones.pNeck)
        neckLocal = NodeTools::Relative(NodeTools::Read(acBones.pChest, NodeTools::kWorldOffset), NodeTools::Read(acBones.pNeck, NodeTools::kWorldOffset));
    if (acBones.pNeck && acBones.pHead)
        headLocal = NodeTools::Relative(NodeTools::Read(acBones.pNeck, NodeTools::kWorldOffset), NodeTools::Read(acBones.pHead, NodeTools::kWorldOffset));

    const glm::mat3 half = glm::mat3_cast(glm::angleAxis(aAngle * 0.5f, acRight));
    Transform spine = NodeTools::Read(acBones.pSpine2, NodeTools::kWorldOffset);
    for (auto& row : spine.Rows)
        row = half * row;
    DualWieldPose::PlaceBone(acBones.pSpine2, spine);
    Transform chest = NodeTools::Combine(spine, chestLocal);
    for (auto& row : chest.Rows)
        row = half * row;
    DualWieldPose::PlaceBone(acBones.pChest, chest);
    if (acBones.pNeck)
    {
        const Transform neck = NodeTools::Combine(chest, neckLocal);
        DualWieldPose::PlaceBone(acBones.pNeck, neck);
        if (acBones.pHead)
            DualWieldPose::PlaceBone(acBones.pHead, NodeTools::Combine(neck, headLocal));
    }
}

// The hand's turn about the forearm shared a third each by the two twist bones and the hand, as the game's own clips share it
// (a wrist taking all of it twists the skin at the wrist)
void ShareTwist(const Bones& acBones, float aWeight) noexcept
{
    if (!acBones.pFore2 || !acBones.pFore3)
        return;
    const auto quat = [](const float (&acQ)[4]) { return glm::quat(acQ[0], acQ[1], acQ[2], acQ[3]); };
    const glm::quat rest2 = quat(kRestFore2), rest3 = quat(kRestFore3), restHand = quat(kRestHand);

    const Transform fore1 = NodeTools::Read(acBones.pFore1, NodeTools::kWorldOffset);
    const Transform handWorld = NodeTools::Read(acBones.pHand, NodeTools::kWorldOffset);
    glm::quat relative = glm::inverse(ToQuat(fore1) * rest2 * rest3 * restHand) * ToQuat(handWorld);
    if (relative.w < 0.f)
        relative = -relative;
    const float twist = 2.f * std::atan2(relative.x, relative.w);
    const glm::quat third = glm::angleAxis(twist / 3.f, glm::vec3(1.f, 0.f, 0.f));

    Transform local2 = NodeTools::Relative(fore1, NodeTools::Read(acBones.pFore2, NodeTools::kWorldOffset));
    SetQuat(local2, NLerp(ToQuat(local2), rest2 * third, aWeight));
    const Transform world2 = NodeTools::Combine(fore1, local2);
    Transform local3 = NodeTools::Relative(NodeTools::Read(acBones.pFore2, NodeTools::kWorldOffset), NodeTools::Read(acBones.pFore3, NodeTools::kWorldOffset));
    SetQuat(local3, NLerp(ToQuat(local3), rest3 * third, aWeight));
    DualWieldPose::PlaceBone(acBones.pFore2, world2);
    DualWieldPose::PlaceBone(acBones.pFore3, NodeTools::Combine(world2, local3));

    // the hand back to the turn it was given, where its forearm has it
    Transform hand = handWorld;
    hand.Translation = NodeTools::GetWorldPosition(acBones.pHand);
    DualWieldPose::PlaceBone(acBones.pHand, hand);
}

void PoseFingers(Play& aPlay, float aT, float aWeight, float aCarry) noexcept
{
    const Motion& motion = *aPlay.pMotion;
    for (int i = 0; i < kFingerBones; ++i)
    {
        uint8_t* pFinger = aPlay.Bone.pFingers[static_cast<size_t>(i)];
        uint8_t* pParent = pFinger ? NodeTools::GetParent(pFinger) : nullptr;
        if (!pParent)
            continue;
        const int finger = i / 3;
        const Transform parentWorld = NodeTools::Read(pParent, NodeTools::kWorldOffset);
        Transform local = NodeTools::Relative(parentWorld, NodeTools::Read(pFinger, NodeTools::kWorldOffset));
        const glm::quat game = ToQuat(local);

        glm::quat turn = game;
        for (int step = 0; step < motion.ShapeCount; ++step)
        {
            const ShapeStep& shape = motion.pShapes[step];
            const float x = (aT - shape.From + kFingerWave * static_cast<float>(finger)) / std::max(shape.To - shape.From, 1e-3f);
            float u = Smooth(x);
            if (x > 1.f && x < 1.f + kShapeSettle)
                u += kShapeOvershoot * std::sin((x - 1.f) / kShapeSettle * 3.14159265f);
            const float* pShape = kShapes[shape.Shape][i];
            turn = NLerp(turn, glm::quat(pShape[0], pShape[1], pShape[2], pShape[3]), u);
        }
        // the fingers dragging behind the hand (toward the palm is -z on every joint of them; the thumb's its own way)
        if (finger > 0)
        {
            const float drag = std::clamp(aPlay.FingerDrag[static_cast<size_t>(finger - 1)] * kFingerDrag, kFingerDragOpen, kFingerDragCurl);
            turn = turn * glm::angleAxis(-drag * kFingerJointShare[i % 3], glm::vec3(0.f, 0.f, 1.f));
        }
        turn = NLerp(game, turn, aWeight);
        if (aPlay.Carrying && aCarry < 1.f)
            turn = NLerp(aPlay.CarryBones[static_cast<size_t>(i)], turn, aCarry);
        aPlay.OutBones[static_cast<size_t>(i)] = turn;
        SetQuat(local, turn);
        DualWieldPose::PlaceBone(pFinger, NodeTools::Combine(parentWorld, local));
    }
}

void PoseItem(Play& aPlay, const Frame& acFrame, float aT) noexcept
{
    const Motion& motion = *aPlay.pMotion;
    uint8_t* pModel = aPlay.Holding.pModel;
    if (!pModel || !aPlay.pHolder)
        return;
    const Transform holder = NodeTools::Read(aPlay.pHolder, NodeTools::kWorldOffset);
    Transform world = NodeTools::Combine(holder, aPlay.Holding.InHand);
    bool shown = aPlay.FadeAt < 0.f;
    if (aPlay.Holding.Lay)
    {
        if (aT < kPullTo)
        {
            // where it lay: in first person its place in the view, in the skeleton's own field of view (so it stays put on the
            // screen as the game takes the real one away)
            Transform lay = aPlay.Holding.LayAt;
            if (aPlay.FirstPerson)
            {
                const glm::vec3 at = InView(aPlay.Camera, lay.Translation - aPlay.Camera.Eye);
                lay.Translation = acFrame.At(glm::vec3(at.x * aPlay.Camera.FovRatio, at.y * aPlay.Camera.FovRatio, at.z));
                for (auto& row : lay.Rows)
                    row = acFrame.Dir(InView(aPlay.Camera, row));
            }
            const float u = Smooth((aT - kPullFrom) / (kPullTo - kPullFrom));
            Transform flying = world;
            SetQuat(flying, glm::slerp(ToQuat(lay), ToQuat(world), u));
            flying.Translation = glm::mix(lay.Translation, world.Translation, u);
            flying.Translation += acFrame.Y * std::min(glm::distance(lay.Translation, world.Translation) * kPullArc, kPullArcMost) * std::sin(u * 3.14159265f);
            flying.Scale = lay.Scale + (world.Scale - lay.Scale) * u;
            world = flying;
        }
    }
    else if (aT < motion.Take)
        shown = false;
    // put away as the hand drops out of view (first person it's off the screen by then; the arm then goes back to whatever it was
    // doing, a gun's grip, with nothing in it)
    if (motion.Release >= 0.f)
    {
        const float gone = motion.pKeys[motion.KeyCount - 1].T + kPutAwayAfter;
        if (aT >= gone)
            shown = false;
        else
            world.Scale *= 1.f - (1.f - kPutAway) * Smooth((aT - motion.Release + kPutAwayBefore) / (gone - motion.Release + kPutAwayBefore));
    }
    DualWieldPose::PlaceBone(pModel, world);
    NodeTools::SetCulled(pModel, !shown);
}

bool SameLocal(const Transform& acA, const Transform& acB) noexcept
{
    return acA.Rows[0] == acB.Rows[0] && acA.Rows[1] == acB.Rows[1] && acA.Rows[2] == acB.Rows[2] && acA.Translation == acB.Translation && acA.Scale == acB.Scale;
}

void ListBased(Play& aPlay) noexcept
{
    const Bones& bones = aPlay.Bone;
    aPlay.pBased = {};
    size_t i = 0;
    for (uint8_t* pNode : {bones.pSpine2, bones.pChest, bones.pNeck, bones.pHead, bones.pCollar, bones.pUpper, bones.pFore1, bones.pFore2, bones.pFore3, bones.pHand})
        aPlay.pBased[i++] = pNode;
    for (uint8_t* pNode : bones.pFingers)
        aPlay.pBased[i++] = pNode;
}

// What the game's given these bones since the last pass: a local that isn't what this wrote is the game's. False: one's so far off its parent that it can't be (nothing's written)
bool TakeBases(Play& aPlay) noexcept
{
    bool body = false, arm = false, fingers = false;
    for (size_t i = 0; i < kBasedBones; ++i)
    {
        const uint8_t* pNode = aPlay.pBased[i];
        if (!pNode)
            continue;
        const Transform local = NodeTools::Read(pNode, NodeTools::kLocalOffset);
        if (aPlay.HasBases && SameLocal(local, aPlay.Written[i]))
            continue;
        if (glm::length(local.Translation) > kBoneFurthest)
        {
            spdlog::warn("Better Interact: {} is {:.0f} from its parent as the game has it, no hand", NodeTools::GetName(pNode), glm::length(local.Translation));
            return false;
        }
        aPlay.Base[i] = local;
        (i < kBodyBones ? body : i < kBodyBones + kArmBones ? arm : fingers) = true;
    }
    if (aPlay.HasBases)
    {
        aPlay.BodyMoves = aPlay.BodyMoves || body;
        aPlay.WroteBody += body ? 1u : 0u;
        aPlay.WroteArm += arm ? 1u : 0u;
        aPlay.WroteFingers += fingers ? 1u : 0u;
    }
    aPlay.HasBases = true;
    return true;
}

// They're put back, and everything from the top of them down goes where its local says
void PutBases(Play& aPlay) noexcept
{
    const size_t first = aPlay.BodyMoves ? 0 : kBodyBones;
    uint8_t* pTop = nullptr;
    for (size_t i = first; i < kBasedBones; ++i)
    {
        uint8_t* pNode = aPlay.pBased[i];
        if (!pNode)
            continue;
        NodeTools::Write(pNode, NodeTools::kLocalOffset, aPlay.Base[i]);
        if (!pTop)
            pTop = pNode;
    }
    DualWieldPose::RestOnLocals(pTop);
}

void KeepWritten(Play& aPlay) noexcept
{
    for (size_t i = 0; i < kBasedBones; ++i)
    {
        if (const uint8_t* pNode = aPlay.pBased[i])
            aPlay.Written[i] = NodeTools::Read(pNode, NodeTools::kLocalOffset);
    }
}

void PoseHand(Play& aPlay, float aDelta) noexcept
{
    const Motion& motion = *aPlay.pMotion;
    const Bones& bones = aPlay.Bone;
    aPlay.Time += aDelta;
    ++aPlay.Frames;
    const float t = aPlay.Time;
    const float lastKey = motion.pKeys[motion.KeyCount - 1].T;
    if (!aPlay.FirstPerson)
    {
        if (!TakeBases(aPlay))
        {
            aPlay.Done = true;
            if (aPlay.Holding.pModel)
                NodeTools::SetCulled(aPlay.Holding.pModel, true);
            return;
        }
        PutBases(aPlay);
    }
    if (t >= aPlay.End || (aPlay.FadeAt >= 0.f && t >= aPlay.FadeAt + kFade))
    {
        // (third person its bones have just been put back, and stay so)
        aPlay.Restored = true;
        aPlay.Done = true;
        if (aPlay.Holding.pModel)
            NodeTools::SetCulled(aPlay.Holding.pModel, true);
        return;
    }

    // how much of it is on
    float weight = Smooth(t / motion.BlendIn) * (1.f - Smooth((t - lastKey) / motion.BlendOut));
    const float carry = aPlay.Carrying ? Smooth(t / kCarry) : 1.f;
    if (aPlay.Carrying)
        weight = std::max(weight, aPlay.CarryWeight * (1.f - carry));
    if (aPlay.FadeAt >= 0.f)
        weight *= 1.f - Smooth((t - aPlay.FadeAt) / kFade);
    const float shoulder = ShoulderAt(motion, t);

    // the frame
    Frame frame{};
    if (aPlay.FirstPerson)
    {
        const Transform camera = NodeTools::Read(bones.pCamera, NodeTools::kWorldOffset);
        frame.Origin = camera.Translation;
        frame.X = glm::normalize(camera.Rows[0]);
        frame.Y = glm::normalize(camera.Rows[1]);
        frame.Z = glm::normalize(camera.Rows[2]);
    }
    else
    {
        const glm::vec3 chest = NodeTools::GetWorldPosition(bones.pChest);
        glm::vec3 forward = aPlay.BodyForward;
        if (aPlay.HasTarget)
        {
            // the way the character faces turned toward it, so the arm goes there however it's standing
            glm::vec3 toTarget = aPlay.Target - chest;
            toTarget.z = 0.f;
            if (glm::length(toTarget) > 1.f)
            {
                const float angle = std::clamp(std::atan2(glm::cross(forward, toTarget).z, glm::dot(forward, toTarget)), -kFaceMost, kFaceMost);
                forward = glm::angleAxis(angle, glm::vec3(0.f, 0.f, 1.f)) * forward;
            }
        }
        frame.X = glm::normalize(glm::cross(forward, glm::vec3(0.f, 0.f, 1.f)));
        frame.Y = glm::vec3(0.f, 0.f, 1.f);
        frame.Z = -forward;
        frame.Origin = chest;
    }

    // the game's own motion of the hand
    const glm::vec3 gameHand = frame.Of(NodeTools::GetWorldPosition(bones.pHand));
    if (!aPlay.HasGameLow)
    {
        aPlay.GameLow = gameHand;
        aPlay.HasGameLow = true;
    }
    aPlay.GameLow += (gameHand - aPlay.GameLow) * std::min(1.f, aDelta * kGameFollow);
    const glm::vec3 gameMotion = Limited(gameHand - aPlay.GameLow, kGameMost) * kGameShare;

    if (!aPlay.FirstPerson)
    {
        // what's below the chest makes the whole lean
        float below = 1.f;
        if (aPlay.HasTarget)
            below = std::clamp((frame.Origin.z - aPlay.Target.z) / kLeanFullBelow, kLeanLeast, 1.f);
        // (a body the game isn't writing locals for has nothing to lean from each pass: the log at its end says so)
        if (aPlay.BodyMoves)
            LeanBody(bones, frame.X, -motion.Lean * below * shoulder * weight);
        frame.Origin = NodeTools::GetWorldPosition(bones.pChest);
    }

    // the keys
    glm::vec3 place = SplineAt(motion, &Key::Place, t);
    glm::vec3 fingers = glm::normalize(SplineAt(motion, &Key::Fingers, t));
    glm::vec3 palm = glm::normalize(SplineAt(motion, &Key::Palm, t));

    // the contact moved toward what it reaches for
    if (aPlay.HasTarget)
    {
        glm::vec3 target{};
        if (aPlay.FirstPerson)
        {
            const glm::vec3 at = InView(aPlay.Camera, aPlay.Target - aPlay.Camera.Eye);
            target = glm::vec3(at.x * aPlay.Camera.FovRatio, at.y * aPlay.Camera.FovRatio, at.z);
        }
        else
            target = frame.Of(aPlay.Target);
        const float away = glm::length(target);
        const Key& contact = motion.pKeys[motion.Contact];
        const glm::vec3 tip = V(contact.Place) + glm::normalize(V(contact.Fingers)) * motion.Tip;
        if (away > 1.f && glm::length(tip) > 1.f)
        {
            // as far along the way to it as the fingertips are at the key (the arm goes no further than that)
            const glm::vec3 want = target / away * std::min(away, glm::length(tip));
            const glm::vec3 moved = Limited(want - tip, kContactMost);
            const float back = motion.pKeys[std::min(motion.Contact + 2, motion.KeyCount - 1)].T;
            const float at = t < contact.T ? Smooth(t / contact.T) : 1.f - Smooth((t - contact.T) / std::max(back - contact.T, 1e-3f));
            place += moved * at;
            aPlay.ContactMoved = glm::length(moved);
            const glm::quat aim = glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), FromTo(tip, tip + moved), kAimShare * at);
            fingers = aim * fingers;
            palm = aim * palm;
        }
    }

    // the hand left behind by the frame's turns and steps, back on its spring
    const glm::vec3 basisX = aPlay.FirstPerson ? aPlay.Camera.Right : frame.X;
    const glm::vec3 basisY = aPlay.FirstPerson ? aPlay.Camera.Up : frame.Y;
    const glm::vec3 basisZ = aPlay.FirstPerson ? -aPlay.Camera.Forward : frame.Z;
    const glm::vec3 origin = aPlay.FirstPerson ? aPlay.Camera.Eye : frame.Origin;
    if (aPlay.HasBasis)
    {
        const glm::vec3 was = aPlay.LastX * place.x + aPlay.LastY * place.y + aPlay.LastZ * place.z;
        const glm::vec3 turned(glm::dot(was, basisX), glm::dot(was, basisY), glm::dot(was, basisZ));
        aPlay.Spring += (turned - place) * kTurnLag;
        const glm::vec3 stepped = origin - aPlay.LastOrigin;
        // (never a load's or a teleport's)
        if (glm::length(stepped) < 50.f)
            aPlay.Spring -= glm::vec3(glm::dot(stepped, basisX), glm::dot(stepped, basisY), glm::dot(stepped, basisZ)) * kMoveLag;
    }
    aPlay.HasBasis = true;
    aPlay.LastX = basisX;
    aPlay.LastY = basisY;
    aPlay.LastZ = basisZ;
    aPlay.LastOrigin = origin;
    for (float left = aDelta; left > 1e-5f; left -= 0.008f)
    {
        const float step = std::min(left, 0.008f);
        aPlay.SpringSpeed += (-kSpringRate * kSpringRate * aPlay.Spring - 2.f * kSpringDamping * kSpringRate * aPlay.SpringSpeed) * step;
        aPlay.Spring += aPlay.SpringSpeed * step;
    }
    aPlay.Spring = Limited(aPlay.Spring, kSpringMost);

    const float phase = aPlay.Time * kSwayRate * 6.2831853f;
    place += aPlay.Spring + gameMotion + glm::vec3(std::sin(phase), std::sin(phase * 0.5f + 1.f), 0.f) * kSwayAmount;

    // the wrist trailing the hand's way, the fingers dragged by it
    if (aPlay.HasLastPlace && aDelta > 1e-5f)
        aPlay.Way += ((place - aPlay.LastPlace) / aDelta - aPlay.Way) * std::min(1.f, aDelta * kTrailRate);
    aPlay.HasLastPlace = true;
    aPlay.LastPlace = place;
    const float speed = glm::length(aPlay.Way);
    if (speed > 1.f)
    {
        const glm::vec3 axis = glm::cross(fingers, -aPlay.Way / speed);
        if (glm::length(axis) > 1e-3f)
        {
            const glm::quat trail = glm::angleAxis(std::min(speed * kTrail, kTrailMost), glm::normalize(axis));
            fingers = trail * fingers;
            palm = trail * palm;
        }
    }
    const float outOfPalm = -glm::dot(aPlay.Way, palm);
    for (size_t i = 0; i < aPlay.FingerDrag.size(); ++i)
        aPlay.FingerDrag[i] += (outOfPalm - aPlay.FingerDrag[i]) * std::min(1.f, aDelta * kFingerFollow[i]);

    if (aPlay.Carrying && carry < 1.f)
    {
        place = glm::mix(aPlay.CarryPlace, place, carry);
        fingers = glm::normalize(glm::mix(aPlay.CarryFingers, fingers, carry));
        palm = glm::normalize(glm::mix(aPlay.CarryPalm, palm, carry));
    }
    palm = glm::normalize(palm - glm::dot(palm, fingers) * fingers);
    aPlay.OutPlace = place;
    aPlay.OutFingers = fingers;
    aPlay.OutPalm = palm;
    aPlay.OutWeight = weight;

    // (nothing of the arm's when none of it is on: the game's, untouched)
    if (weight > 1e-3f)
    {
        // the shoulder going with the arm
        if (bones.pCollar)
        {
            Transform collar = NodeTools::Read(bones.pCollar, NodeTools::kWorldOffset);
            collar.Translation += frame.Dir(V(motion.ShoulderPush)) * shoulder * weight;
            DualWieldPose::PlaceBone(bones.pCollar, collar);
        }

        // the arm
        glm::vec3 reachTo = frame.At(place);
        const glm::vec3 shoulderAt = NodeTools::GetWorldPosition(bones.pUpper);
        const float armLength = glm::distance(shoulderAt, NodeTools::GetWorldPosition(bones.pFore1)) + glm::distance(NodeTools::GetWorldPosition(bones.pFore1), NodeTools::GetWorldPosition(bones.pHand));
        reachTo = shoulderAt + Limited(reachTo - shoulderAt, armLength * kReachMost);

        const glm::vec3 fingersWorld = frame.Dir(fingers);
        const glm::vec3 palmWorld = frame.Dir(palm);
        const glm::mat3 want(fingersWorld, palmWorld, glm::cross(fingersWorld, palmWorld));
        const glm::mat3 have(V(kHandFingers), V(kHandPalm), V(kHandSide));
        const glm::mat3 handTurn = want * glm::transpose(have);
        Transform goal{};
        goal.Rows[0] = handTurn[0];
        goal.Rows[1] = handTurn[1];
        goal.Rows[2] = handTurn[2];
        DualWieldPose::ReachArm(bones.pUpper, bones.pFore1, bones.pHand, reachTo, goal, frame.Dir(V(kPole)), weight);
        ShareTwist(bones, weight);

        PoseFingers(aPlay, t, weight, carry);
    }

    // first person: the other arm goes with the body's reach
    if (aPlay.FirstPerson && bones.pRightCollar)
    {
        const float q = std::clamp(t / kBodyLength, 0.f, 1.f);
        const float body = 4.f * q * (1.f - q) * Smooth(t / 0.18f) * kBodyShare[static_cast<size_t>(aPlay.Which)] * (aPlay.FadeAt >= 0.f ? 1.f - Smooth((t - aPlay.FadeAt) / kFade) : 1.f);
        if (body > 1e-3f)
        {
            Transform collar = NodeTools::Read(bones.pRightCollar, NodeTools::kWorldOffset);
            const glm::vec3 turn = kBodyTurn * body;
            const glm::mat3 r = glm::mat3_cast(glm::angleAxis(turn.x, frame.X) * glm::angleAxis(turn.y, frame.Y) * glm::angleAxis(turn.z, frame.Z));
            for (auto& row : collar.Rows)
                row = r * row;
            collar.Translation += frame.Dir(kBodyMove * body);
            DualWieldPose::PlaceBone(bones.pRightCollar, collar);
        }
    }

    PoseItem(aPlay, frame, t);

    if (!aPlay.FirstPerson)
        KeepWritten(aPlay);

    if (!aPlay.LoggedContact && t >= motion.pKeys[motion.Contact].T)
    {
        aPlay.LoggedContact = true;
        spdlog::info("Better Interact: {} at its contact ({} person, {:.2f} s, frame {}): hand at ({:.0f}, {:.0f}, {:.0f}), {:.1f} moved toward its target, spring {:.1f}, weight {:.2f}",
                     NameOf(aPlay.Which), aPlay.FirstPerson ? "first" : "third", t, aPlay.Frames, place.x, place.y, place.z, aPlay.ContactMoved, glm::length(aPlay.Spring), weight);
    }
}

void Layer(uint8_t* apRoot, bool) noexcept
{
    std::scoped_lock lock(s_lock);
    for (auto& play : s_plays)
    {
        if (play.pRoot != apRoot || play.Done)
            continue;
        const auto now = Clock::now();
        const float delta = play.Last == Clock::time_point{} ? 0.f : std::min(std::chrono::duration<float>(now - play.Last).count(), 0.1f);
        play.Last = now;
        PoseHand(play, delta);
        return;
    }
}

bool FindBones(uint8_t* apRoot, bool aFirstPerson, Bones& aBones) noexcept
{
    const auto find = [apRoot](const char* acpName) { return NodeTools::GetObjectByName(apRoot, BSFixedString(acpName)); };
    aBones.pRoot = apRoot;
    aBones.pCollar = find("LArm_Collarbone");
    aBones.pUpper = find("LArm_UpperArm");
    aBones.pFore1 = find("LArm_ForeArm1");
    aBones.pFore2 = find("LArm_ForeArm2");
    aBones.pFore3 = find("LArm_ForeArm3");
    aBones.pHand = find("LArm_Hand");
    aBones.pWeaponLeft = find("WeaponLeft");
    aBones.pRightCollar = find("RArm_Collarbone");
    aBones.pChest = find("Chest");
    if (aFirstPerson)
        aBones.pCamera = find("Camera");
    else
    {
        aBones.pSpine2 = find("SPINE2");
        aBones.pNeck = find("Neck");
        aBones.pHead = find("HEAD");
    }
    size_t i = 0;
    for (int finger = 1; finger <= 5; ++finger)
    {
        for (int joint = 1; joint <= 3; ++joint)
            aBones.pFingers[i++] = find(fmt::format("LArm_Finger{}{}", finger, joint).c_str());
    }
    const bool arm = aBones.pUpper && aBones.pFore1 && aBones.pHand;
    return arm && (aFirstPerson ? aBones.pCamera != nullptr : aBones.pChest != nullptr);
}

void LetGo(Play& aPlay) noexcept
{
    if (aPlay.Holding.pModel)
    {
        NodeTools::Detach(aPlay.Holding.pModel);
        NodeTools::DecRef(aPlay.Holding.pModel);
        aPlay.Holding.pModel = nullptr;
    }
    aPlay.Bone.ForEach([](uint8_t* apNode) { NodeTools::DecRef(apNode); });
    aPlay.Bone = Bones{};
}
} // namespace

namespace BetterInteractClip
{
bool Start(uint8_t* apRoot, bool aFirstPerson, Kind aKind, const glm::vec3* apTarget, const Item& acItem, const View& acView, const glm::vec3& acBodyForward) noexcept
{
    Play play{};
    if (!apRoot || !FindBones(apRoot, aFirstPerson, play.Bone))
    {
        if (acItem.pModel)
            NodeTools::DecRef(acItem.pModel);
        spdlog::warn("Better Interact: the {} person skeleton lacks the arm's bones, no hand", aFirstPerson ? "first" : "third");
        return false;
    }
    play.Bone.ForEach([](uint8_t* apNode) { NodeTools::IncRef(apNode); });
    ListBased(play);
    play.pRoot = apRoot;
    play.FirstPerson = aFirstPerson;
    play.Which = aKind;
    play.pMotion = &MotionFor(aKind, aFirstPerson);
    play.Started = Clock::now();
    play.Last = Clock::time_point{};
    play.End = play.pMotion->pKeys[play.pMotion->KeyCount - 1].T + play.pMotion->BlendOut;
    if (aFirstPerson)
        play.End = std::max(play.End, kBodyLength);
    play.HasTarget = apTarget != nullptr;
    if (apTarget)
        play.Target = *apTarget;
    play.Camera = acView;
    play.BodyForward = acBodyForward;
    play.Holding = acItem;
    if (acItem.pModel)
    {
        // on the hand from now, not shown until it's first put in its place
        play.pHolder = acItem.OnWeaponNode && play.Bone.pWeaponLeft ? play.Bone.pWeaponLeft : play.Bone.pHand;
        NodeTools::SetCulled(acItem.pModel, true);
        NodeTools::AttachChild(play.pHolder, acItem.pModel);
    }

    Play old{};
    bool replaced = false;
    {
        std::scoped_lock lock(s_lock);
        const auto it = std::find_if(s_plays.begin(), s_plays.end(), [apRoot](const Play& acPlay) { return acPlay.pRoot == apRoot; });
        if (it != s_plays.end())
        {
            // takes over from where that one's hand is, its spring and all
            if (it->Frames > 0 && !it->Done)
            {
                play.Carrying = true;
                play.CarryPlace = it->OutPlace;
                play.CarryFingers = it->OutFingers;
                play.CarryPalm = it->OutPalm;
                play.CarryWeight = it->OutWeight;
                play.CarryBones = it->OutBones;
                play.Spring = it->Spring;
                play.SpringSpeed = it->SpringSpeed;
            }
            // (what's on the bones now is that one's: the game's are what it kept)
            if (it->HasBases && !it->Restored && it->pBased == play.pBased)
            {
                play.Base = it->Base;
                play.Written = it->Written;
                play.HasBases = true;
                play.BodyMoves = it->BodyMoves;
            }
            old = *it;
            replaced = true;
            *it = play;
        }
        else
            s_plays.push_back(play);
    }
    if (replaced)
        LetGo(old);
    DualWieldPose::SetLayer(apRoot, aFirstPerson, &Layer);
    return true;
}

void Update(uint8_t* apRoot, const View& acView, const glm::vec3& acBodyForward) noexcept
{
    std::scoped_lock lock(s_lock);
    for (auto& play : s_plays)
    {
        if (play.pRoot == apRoot)
        {
            play.Camera = acView;
            play.BodyForward = acBodyForward;
            return;
        }
    }
}

void FadeOut(uint8_t* apRoot) noexcept
{
    std::scoped_lock lock(s_lock);
    for (auto& play : s_plays)
    {
        if (play.pRoot == apRoot && play.FadeAt < 0.f)
            play.FadeAt = play.Time;
    }
}

bool IsDone(const uint8_t* apRoot) noexcept
{
    std::scoped_lock lock(s_lock);
    for (const auto& play : s_plays)
    {
        if (play.pRoot == apRoot)
            return play.Done;
    }
    return true;
}

float SecondsSincePosed(const uint8_t* apRoot) noexcept
{
    std::scoped_lock lock(s_lock);
    for (const auto& play : s_plays)
    {
        if (play.pRoot == apRoot)
            return std::chrono::duration<float>(Clock::now() - (play.Last == Clock::time_point{} ? play.Started : play.Last)).count();
    }
    return 0.f;
}

void Stop(uint8_t* apRoot) noexcept
{
    Play old{};
    bool found = false;
    {
        std::scoped_lock lock(s_lock);
        const auto it = std::find_if(s_plays.begin(), s_plays.end(), [apRoot](const Play& acPlay) { return acPlay.pRoot == apRoot; });
        if (it != s_plays.end())
        {
            old = *it;
            found = true;
            s_plays.erase(it);
        }
    }
    if (!found)
        return;
    DualWieldPose::ClearLayer(apRoot);
    // Ended early: the bones the animation doesn't write would keep the pose
    if (old.HasBases && !old.Restored)
        PutBases(old);
    if (old.FirstPerson)
        spdlog::info("Better Interact: {} over after {:.2f} s, {} frames", NameOf(old.Which), old.Time, old.Frames);
    else
        spdlog::info("Better Interact: {} over after {:.2f} s, {} passes; the game wrote new locals on the body in {} of them, the arm {}, the fingers {}", NameOf(old.Which), old.Time, old.Frames, old.WroteBody, old.WroteArm, old.WroteFingers);
    LetGo(old);
}

void StopAll() noexcept
{
    std::vector<Play> plays;
    {
        std::scoped_lock lock(s_lock);
        plays.swap(s_plays);
    }
    for (auto& play : plays)
    {
        DualWieldPose::ClearLayer(play.pRoot);
        LetGo(play);
    }
}

Transform HoldInHand(const glm::vec3& acMin, const glm::vec3& acMax, float aScale) noexcept
{
    glm::vec3 size = acMax - acMin;
    glm::vec3 centre = (acMin + acMax) * 0.5f;
    // (a form without bounds: taken as a small thing about its origin)
    if (std::max({size.x, size.y, size.z}) < 0.5f)
    {
        size = glm::vec3(4.f);
        centre = glm::vec3(0.f);
    }
    int longest = 0;
    int shortest = 0;
    for (int axis = 1; axis < 3; ++axis)
    {
        if (size[axis] > size[longest])
            longest = axis;
        if (size[axis] <= size[shortest])
            shortest = axis;
    }
    if (longest == shortest)
        shortest = (longest + 1) % 3;
    const int middle = 3 - longest - shortest;

    const float scale = aScale * std::clamp(kHoldMost / std::max(size[longest] * aScale, 0.01f), kHoldLeast, 1.f);
    const glm::vec3 thumbward = -V(kHandSide);
    const glm::vec3 palm = V(kHandPalm);
    Transform hold{};
    hold.Rows[longest] = thumbward;
    hold.Rows[shortest] = palm;
    // the third axis as a right-handed frame has it
    const bool even = (longest + 1) % 3 == middle;
    hold.Rows[middle] = even ? glm::cross(palm, thumbward) : glm::cross(thumbward, palm);
    hold.Scale = scale;
    // its middle at the grip, a thick thing further out of the palm
    const glm::vec3 grip = V(kHandGrip) + palm * (size[shortest] * scale * 0.35f);
    hold.Translation = grip - (hold.Rows[0] * centre.x + hold.Rows[1] * centre.y + hold.Rows[2] * centre.z) * scale;
    return hold;
}

Transform HoldWeapon(float aScale) noexcept
{
    Transform hold = OffHandModel::OnWeaponLeft();
    hold.Scale = aScale;
    return hold;
}
} // namespace BetterInteractClip
