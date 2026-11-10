#pragma once

#include <NodeTools.h>

// Better Interact's hand (docs/BETTER_INTERACT.md): the left arm going out to grab an item, press a button or push a door,
// put on a skeleton at the end of its bone tree's update over whatever the game animates (DualWieldPose's layer), so nothing
// in the animation graph changes and the player keeps walking, running, looking and shooting as they were. The motions are
// keys in Tools/BetterInteractHands/make_hands.py's table; the sum on them makes it loose: the contact moved toward what the
// hand reaches for, a spring on the view's turns and steps, the wrist trailing and the fingers dragging, the game's own bob
// coming through, the shoulder and (first person) the other arm going with it. Main thread but for the layer itself.
namespace BetterInteractClip
{
enum class Kind : uint8_t
{
    kGrab,
    kPress,
    kPush,
    kCount,
};

// What the hand takes: a copy of the thing's 3D (our reference, given over to the hand with Start, named so no lookup of the
// skeleton's bones finds anything in it) on the left hand's weapon node (a weapon, by its grip) or on the hand itself
struct Item
{
    uint8_t* pModel{nullptr};
    bool OnWeaponNode{false};
    // its place on that node
    NodeTools::Transform InHand{};
    // it lay here in the world, and flies to the hand; without that it's in the hand from the motion's take
    bool Lay{false};
    NodeTools::Transform LayAt{};
};

// The world's camera this frame (first person: the first person skeleton isn't in the world's space, Item::LayAt and the
// target come into it through this)
struct View
{
    glm::vec3 Eye{0.f};
    glm::vec3 Forward{0.f, 1.f, 0.f};
    glm::vec3 Right{1.f, 0.f, 0.f};
    glm::vec3 Up{0.f, 0.f, 1.f};
    // what a world point's offset across the view is multiplied by to be in the same place on screen in the first person
    // skeleton's own field of view
    float FovRatio{1.f};
};

// The hand starts on the skeleton under apRoot (a character's 3D root, or the player's first person one), taking over from
// one under way there without a jump; apTarget: what it reaches for (world), acBodyForward: the way the character faces (third
// person). False when the skeleton lacks its bones (the item's let go)
bool Start(uint8_t* apRoot, bool aFirstPerson, Kind aKind, const glm::vec3* apTarget, const Item& acItem, const View& acView, const glm::vec3& acBodyForward) noexcept;
// Every frame while it plays
void Update(uint8_t* apRoot, const View& acView, const glm::vec3& acBodyForward) noexcept;
// Something else wants the arm now: the hand fades out early (its item gone at once)
void FadeOut(uint8_t* apRoot) noexcept;
// It's run its course (Stop it)
[[nodiscard]] bool IsDone(const uint8_t* apRoot) noexcept;
// How long since it was last put on the skeleton (a bone tree that stopped updating), s
[[nodiscard]] float SecondsSincePosed(const uint8_t* apRoot) noexcept;
// Off that skeleton, its item let go
void Stop(uint8_t* apRoot) noexcept;
void StopAll() noexcept;

// An item's place on the hand: from its model's bounds (the form's, in the model's own axes): its longest side across the
// palm, the thumb's way, its shortest out of the palm, in the fingers; its scale aScale but smaller when it's too big for a hand
[[nodiscard]] NodeTools::Transform HoldInHand(const glm::vec3& acMin, const glm::vec3& acMax, float aScale) noexcept;
// A weapon's place on the left hand's weapon node (held by its grip, as the left hand holds a weapon: OffHandModel)
[[nodiscard]] NodeTools::Transform HoldWeapon(float aScale) noexcept;
} // namespace BetterInteractClip
