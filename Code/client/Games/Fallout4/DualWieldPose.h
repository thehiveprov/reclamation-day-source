#pragma once

#include <NodeTools.h>

#include <glm/gtc/quaternion.hpp>

// The arms of a character with a weapon in each hand (docs/DUAL_WIELD.md): put on its skeleton at the end of every bone
// tree update (the hook PipboyGlancePose has), over whatever the game animates. Each arm reaches for a hold in front of
// its shoulder (two-bone IK: upper arm, forearm, hand), the hand turned so the weapon in it points the way the character
// aims, with a little sway, recoil kicks, the left hand's melee swing and both lowered out of view for the reload; in
// first person the game's own animation of the right arm (its idle and fidgets, walking, the draw and holster) moves both
// holds as it moves the right hand, the left mirrored. The
// left hand grips its weapon as the game's own animations grip that weapon (WeaponGrips: its first person idle clip's
// right hand, mirrored onto the left bones). First person: the player's first person skeleton, in the view's frame;
// third person: any character's (the player's own body, other players'), in a frame at its chest.
namespace WeaponGrips
{
struct Grip;
struct ArmClip;
struct LimbClip;
}

namespace DualWieldPose
{
// A hand's motion in the first person view (its skeleton's Camera bone: x right, y up, z back): turned about the hand,
// then moved; none by default
struct Motion
{
    glm::quat Turn{1.f, 0.f, 0.f, 0.f};
    glm::vec3 Move{0.f};
};

// The way a hand goes from one place in the view to another (two frames of a clip, or a clip's frame and this frame's
// animation)
[[nodiscard]] Motion MotionBetween(const NodeTools::Transform& acFrom, const NodeTools::Transform& acTo) noexcept;
// The right hand's motion as the left hand's: mirrored across the view's x (a turn's axis mirrored, its sense turned over)
[[nodiscard]] Motion MirrorMotion(const Motion& acRight) noexcept;
// aT of the way from one motion to another
[[nodiscard]] Motion BlendMotion(const Motion& acFrom, const Motion& acTo, float aT) noexcept;

// What a character's arms do now (main thread, every frame the pose is wanted)
struct Arms
{
    // the left arm holds the left-hand weapon; the right arm is posed too (a weapon its own animation holds with both
    // hands, or the lowered reload)
    bool Left{false};
    bool Right{false};
    // the left hand holds a melee weapon (up, blade first) rather than a gun
    bool LeftMelee{false};
    // the left weapon's own grip (WeaponGrips: the right hand's in its idle clip; it's mirrored here); none: the game's;
    // in third person a melee weapon's from its third person idle, the hand its third person attacks hold it in (the first
    // person one there, the baton moved in the hand going into and out of each attack and still slipped out a little,
    // seen 2026-10-25)
    const WeaponGrips::Grip* pLeftGrip{nullptr};
    const WeaponGrips::Grip* pLeftGrip3P{nullptr};
    // first person: the left arm plays this clip of the left weapon's (its idle, an attack, the block: the right arm's
    // in the clip, mirrored, where it is in the view) at this time, blended from another (the one before) by
    // LeftClipBlend; none: the hold, swing and guard below (third person always)
    const WeaponGrips::ArmClip* pLeftClip{nullptr};
    float LeftClipTime{0.f};
    const WeaponGrips::ArmClip* pLeftClipFrom{nullptr};
    float LeftClipFromTime{0.f};
    float LeftClipBlend{1.f};
    // 0..1: how much of the pose the right arm takes (eased by the caller: a gun beside a left melee weapon is the
    // game's but for the reload's dip)
    float RightWeight{1.f};
    // First person, a gun held by the pose: the game's animation of the right arm this frame (its idle and the idle's
    // fidgets, walking, jumping, sprinting, the draw and the holster) read as the right hand's motion from its weapon's
    // plain idle (this clip's first frame, in the view), and carried onto the holds: RightFollow of it onto the right
    // gun's, LeftFollow of it mirrored onto the left gun's (a beat behind; held where it is while LeftFollowHeld: the
    // right gun's own shots and their kick)
    const WeaponGrips::ArmClip* pRightIdle{nullptr};
    float RightFollow{0.f};
    float LeftFollow{0.f};
    bool LeftFollowHeld{false};
    // First person: the left gun's own motion on its hold besides (its own clips' when the right hand has no gun to
    // follow: its idle and fidgets, its draw and holster)
    Motion LeftOwn{};
    // 0..1: how much of the left hand's grip goes on even where the arms aren't posed (Weight 0: the Pip-Boy has the
    // left arm, the weapon stays in the hand)
    float GripWeight{0.f};
    // Third person: the left arm mirrors the right one bone for bone (every bone from RArm_Collarbone down, across the
    // chest's middle), the right one as this frame's animation has it, or LimbWeight of the way as
    // this clip has it on this chest (a melee weapon's attack or block: the game's own third person clips), blended from
    // the clip before by LimbBlend; the left weapon's own grip, its kicks and the dip on top. The right arm stays the
    // game's but for the dip and the grapple's cable
    bool MirrorRight{false};
    // third person: the right hand holds a gun (kept on its own side with a weapon in the left: the game's pistol holds
    // it on the body's middle line, for both hands); where the view's crosshair is (world, far along the view); 0..1: how
    // much the left gun's arm reaches to aim at it however the right arm is (after its shots, the button held, hanging
    // from the grapple: it aims anyway as much as the right gun does)
    bool RightHoldsGun{false};
    glm::vec3 AimAt{0.f};
    float LeftAim{0.f};
    // ... on the motorcycle: the left arm alone comes off the bars to the aim, the rest of the body stays as the ride has
    // it; 0..1 up at the sights, the hand at BikeSights in front of the rider's eyes
    bool OnBike{false};
    float BikeAds{0.f};
    glm::vec3 BikeSights{0.f};
    // ... and a left melee weapon's own clips in place of the right arm (every melee weapon in the left hand uses its own
    // animations and grips): its idle at this time, its attacks and the rest over it
    const WeaponGrips::LimbClip* pLimbIdle{nullptr};
    float LimbIdleTime{0.f};
    const WeaponGrips::LimbClip* pLimbClip{nullptr};
    float LimbTime{0.f};
    // the clip turns the body too (an attack, the block; not the draw or holster, under the right weapon's own)
    bool LimbMovesBody{true};
    const WeaponGrips::LimbClip* pLimbClipFrom{nullptr};
    float LimbFromTime{0.f};
    float LimbBlend{1.f};
    float LimbWeight{0.f};
    // the frame the holds are in: first person the view (where the eye is, which way it looks, its right and up);
    // third person only the directions (the frame's origin is the chest bone)
    glm::vec3 Origin{0.f};
    glm::vec3 Forward{0.f, 1.f, 0.f};
    glm::vec3 Right3{1.f, 0.f, 0.f};
    glm::vec3 Up{0.f, 0.f, 1.f};
    // the lowered reload's phase, 0..1 (below 0: none): both arms dip out of view and come back up; each hand's recoil,
    // 0..1; the left swing's phase (below 0: none)
    float Reload{-1.f};
    // the dip is a gun's reload (third person a shorter one, out in front), not a draw, holster or consumable's lowering
    bool ReloadDip{false};
    float LeftKick{0.f};
    float RightKick{0.f};
    float Swing{-1.f};
    // the swing is a power attack (wound further back, across further); 0..1: the left melee weapon raised across the
    // face to block
    bool PowerSwing{false};
    float Guard{0.f};
    // hanging on the grappling gun's cable: that hand's arm reaches up along it, the gun pointing at where the cable is
    // fixed (world)
    bool LeftHang{false};
    bool RightHang{false};
    glm::vec3 HangTo{0.f};
    // 0..1: how much of the pose is on (eased in and out by the caller)
    float Weight{0.f};
};

// Main thread: the pose for the skeleton under apRoot (a character's 3D root: the player's first person one, or a
// character's third person one); finds the bones on the first call for that root (which can make nodes, so never during
// the bone tree's update)
void Set(uint8_t* apRoot, bool aFirstPerson, const Arms& acArms) noexcept;
// Main thread: off that skeleton from the next update
void Clear(uint8_t* apRoot) noexcept;
// Main thread: off every skeleton (a load, the service going)
void ClearAll() noexcept;

// Another layer over the arms on a skeleton (Better Interact's hand, BetterInteractClip): called from the bone tree hook
// (any thread, the pose's lock held: it must never call back in with Set, Clear or SetLayer) after this frame's arms, for as
// long as it's set, with the same care for what the game did or didn't move this frame (FindUntouched). A skeleton with only
// a layer has no arms posed. Main thread, both
using Layer = void (*)(uint8_t* apRoot, bool aFirstPerson);
void SetLayer(uint8_t* apRoot, bool aFirstPerson, Layer apLayer) noexcept;
void ClearLayer(uint8_t* apRoot) noexcept;
// The arms on that skeleton are posed now (a weapon in the left hand, the reload's dip, the grapple's arm); main thread
[[nodiscard]] bool HasArms(uint8_t* apRoot) noexcept;
// Inside a layer: a node put at a world transform, its tree with it
void PlaceBone(uint8_t* apNode, const NodeTools::Transform& acWorld) noexcept;
// Everything from that node down put where its locals say, on its parent as it is now: the game's own sum, for bones that have just been given their locals
void RestOnLocals(uint8_t* apTop) noexcept;
// Inside a layer: an arm reaching for acTarget (world), the hand turned to acGoal's rotation, aWeight of the way from where
// the game has it, the elbow toward acPole
void ReachArm(uint8_t* apUpper, uint8_t* apFore, uint8_t* apHand, const glm::vec3& acTarget, const NodeTools::Transform& acGoal, const glm::vec3& acPole, float aWeight) noexcept;

// From the bone tree hook (any thread): poses the skeleton the tree belongs to, if it has a pose
void OnBoneTreeUpdated(uint8_t* apTree) noexcept;
// How many times the skeleton under apRoot was posed since the last call (the view probe: 1 a frame is right)
uint32_t TakePoseCount(uint8_t* apRoot) noexcept;
// The dip's phase (Arms::Reload) for a lowering of its own timing: aDown s going down, aHold s down, aUp s coming back
// up, aTime s in (below 0 past its end)
[[nodiscard]] float DipPhase(float aTime, float aDown, float aHold, float aUp) noexcept;
} // namespace DualWieldPose
