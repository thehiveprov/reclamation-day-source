#pragma once

#include <NodeTools.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

struct BGSKeywordForm;

// How the game's own first person animations hold and swing a weapon, for the left hand's weapon (docs/DUAL_WIELD.md):
// the weapon's first person animation folder (Fallout4.esm's HumanRaceSubGraphData: the subgraph whose target keywords
// the weapon has, the one with the most of them), that folder's clips (a gun's WPNIdleReady; a melee weapon's Idle,
// AttackA, AttackB, AttackPowerStanding, Block), their right hand. Read from the player's own game files (loose or
// archived, through the game's BSResource streams); the clips are hkaLosslessCompressedAnimation (the idles) and
// hkaSplineCompressedAnimation (the attacks), decoded here (Tools/DualWieldGrips/lossless.py has the first's format,
// PyNifly's anim_fo4 the second's). Each clip once, kept for as long as the game runs.
namespace WeaponGrips
{
// RArm_Finger11, 12, 13, 21... 53: each bone after its parent
constexpr size_t kFingerCount = 15;

// A hand's grip: its weapon bone (the clips' "Weapon", the skeletons' WEAPON) on the hand, each finger bone on its parent
struct Grip
{
    NodeTools::Transform Weapon{};
    std::array<NodeTools::Transform, kFingerCount> Fingers{};
};

// One frame of a clip's right arm: the hand bone's place in the view (the clip's Camera bone: x right, y up, z back),
// the elbow's (ForeArm1), and the hand's grip
struct ArmFrame
{
    NodeTools::Transform Hand{};
    glm::vec3 Elbow{0.f};
    Grip HandGrip{};
};

// A clip's right arm, frame by frame (FrameTime apart), and its events (HitFrame, weaponSwing...) by time
struct ArmClip
{
    float Duration{0.f};
    float FrameTime{1.f / 30.f};
    std::vector<ArmFrame> Frames;
    std::vector<std::pair<float, std::string>> Events;

    // the arm at a time (clamped to the clip), between its two nearest frames
    [[nodiscard]] ArmFrame At(float aTime) const noexcept;
    // when the first event of that name comes (aFallback: none)
    [[nodiscard]] float EventTime(const char* acpEvent, float aFallback) const noexcept;
};

// A third person clip's right arm bone by bone (Meshes\Actors\Character\Animations: the game's own third person clips,
// against the third person skeleton): every bone from RArm_Collarbone down that the clip moves, then the body's (Spine1,
// Spine2, Chest, Neck, Head), each frame its place on its parent; and the clip's events
struct LimbClip
{
    float Duration{0.f};
    float FrameTime{1.f / 30.f};
    // the bones (parents before their children) and each one's parent among them (-1: RArm_Collarbone's, the Chest; the
    // body's bones, after the arm's, -1 too)
    std::vector<std::string> Bones;
    std::vector<int> Parents;
    // frame * Bones.size() + bone
    std::vector<NodeTools::Transform> Locals;
    std::vector<std::pair<float, std::string>> Events;

    // a bone's place on its parent at a time (clamped to the clip), between its two nearest frames
    [[nodiscard]] NodeTools::Transform LocalAt(size_t aBone, float aTime) const noexcept;
    [[nodiscard]] float EventTime(const char* acpEvent, float aFallback) const noexcept;
};

// aT of the way from one arm to another (turned the shorter way)
[[nodiscard]] ArmFrame Blend(const ArmFrame& acFrom, const ArmFrame& acTo, float aT) noexcept;
[[nodiscard]] Grip Blend(const Grip& acFrom, const Grip& acTo, float aT) noexcept;

// Main thread: the right hand's grip the game's first person animations give a weapon with these keywords (its
// instance's: the mods change them); null when no clip of its folders could be read
[[nodiscard]] const Grip* ForKeywords(const BGSKeywordForm* apKeywords, bool aMelee) noexcept;
// Main thread: that clip (a file name: "AttackA.hkx") from the first of the weapon's folders that has it; null when none
[[nodiscard]] const ArmClip* ClipFor(const BGSKeywordForm* apKeywords, bool aMelee, const char* acpClip) noexcept;
// Main thread: a clip of one folder (under the first person animations: "Common", "UseStimpakOnSelf.hkx"); null when
// it can't be read
[[nodiscard]] const ArmClip* ClipIn(const char* acpFolder, const char* acpClip) noexcept;
// Main thread: that third person clip ("AttackStandingA.hkx") from the first of the weapon's folders that has it (the
// third person folders go by the first person ones' names: 1HMShort, then 1HM); null when none
[[nodiscard]] const LimbClip* ThirdPersonClipFor(const BGSKeywordForm* apKeywords, bool aMelee, const char* acpClip) noexcept;
// Main thread: a third person clip of one folder (under the third person animations: "Common", "UseStealthBoy.hkx");
// null when it can't be read
[[nodiscard]] const LimbClip* ThirdPersonClipIn(const char* acpFolder, const char* acpClip) noexcept;
// Main thread: the right hand's grip the game's third person idle gives a weapon with these keywords (a melee weapon's
// Idle: the same hand as its third person attacks, so a weapon going from one to the other keeps its place in it); null
// when it can't be read
[[nodiscard]] const Grip* ThirdPersonGripFor(const BGSKeywordForm* apKeywords, bool aMelee) noexcept;
} // namespace WeaponGrips
