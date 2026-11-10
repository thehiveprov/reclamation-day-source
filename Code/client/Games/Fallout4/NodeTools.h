#pragma once

struct BSFixedString;
struct PlayerCharacter;
struct TESObjectREFR;

// The game's scene nodes (NiAVObject / NiNode, Fallout 4 1.10.163, CommonLibF4's layout), as raw pointers: what the
// left-hand weapon's model, the arm poses and the grapple's visuals work with. NiAVObject: refcount at 0x08 (NiRefObject,
// DeleteThis vtable slot 1), parent 0x28, local transform 0x30, world 0x70 (each a rotation of 3 rows of 4 floats as
// NiTransform stores it, row i the direction of local axis i in the parent's space; the translation at +0x30, the scale
// at +0x3C), world bound 0xB0 (centre, radius), flags 0x108 (bit 0: culled by the app). NiNode: children at 0x128, the
// slots in use at 0x132. Vtable: IsNode 4, GetObjectByName 0x2E, AttachChild 0x3A, DetachChild 0x3D. Main thread, or the
// bone tree's update for the poses.
namespace NodeTools
{
constexpr uintptr_t kRefCountOffset = 0x08;
constexpr uintptr_t kParentOffset = 0x28;
constexpr uintptr_t kLocalOffset = 0x30;
constexpr uintptr_t kWorldOffset = 0x70;
constexpr uintptr_t kWorldBoundOffset = 0xB0;
constexpr uintptr_t kFlagsOffset = 0x108;
constexpr uintptr_t kChildrenOffset = 0x128;
constexpr uintptr_t kChildCountOffset = 0x132;

// A node's transform as NiTransform keeps it: the rotation stored transposed (row i = where local axis i points), so a
// point goes to the parent as a row vector (p * rotation * scale + translation) and transforms compose local first
struct Transform
{
    glm::vec3 Rows[3]{glm::vec3(1.f, 0.f, 0.f), glm::vec3(0.f, 1.f, 0.f), glm::vec3(0.f, 0.f, 1.f)};
    glm::vec3 Translation{0.f};
    float Scale{1.f};

    // a point (or a direction, aPoint false) of this space in the parent's
    [[nodiscard]] glm::vec3 Apply(const glm::vec3& acLocal, bool aPoint = true) const noexcept
    {
        const glm::vec3 turned = Rows[0] * acLocal.x + Rows[1] * acLocal.y + Rows[2] * acLocal.z;
        return aPoint ? Translation + turned * Scale : turned;
    }
};

Transform Read(const uint8_t* apNode, uintptr_t aOffset) noexcept;
void Write(uint8_t* apNode, uintptr_t aOffset, const Transform& acTransform) noexcept;
// The world transform of a node with this local transform under a parent with this world one
Transform Combine(const Transform& acParent, const Transform& acLocal) noexcept;
// The local transform that puts a node at this world transform under this parent (Combine's inverse; rotations kept
// orthonormal)
Transform Relative(const Transform& acParent, const Transform& acWorld) noexcept;

[[nodiscard]] inline uint8_t* GetParent(const uint8_t* apNode) noexcept { return apNode ? *reinterpret_cast<uint8_t* const*>(apNode + kParentOffset) : nullptr; }
[[nodiscard]] inline glm::vec3 GetWorldPosition(const uint8_t* apNode) noexcept
{
    const auto* pFloats = reinterpret_cast<const float*>(apNode + kWorldOffset);
    return {pFloats[12], pFloats[13], pFloats[14]};
}
// The node's own name (NiObjectNET::name, a BSFixedString at 0x10), empty without one
[[nodiscard]] const char* GetName(const uint8_t* apNode) noexcept;
// ... given another (the game's own BSFixedString set: the old one let go)
void SetName(uint8_t* apNode, const char* acpName) noexcept;
inline void SetCulled(uint8_t* apNode, bool aCulled) noexcept
{
    auto& flags = *reinterpret_cast<uint64_t*>(apNode + kFlagsOffset);
    flags = aCulled ? flags | 1ull : flags & ~1ull;
}

// The object of this name in the node's tree (the node itself included), or nullptr
[[nodiscard]] uint8_t* GetObjectByName(uint8_t* apNode, const BSFixedString& acName) noexcept;
// The node's children when it's a node (some slots may be empty: skipped)
template <class TVisit> void ForEachChild(uint8_t* apObject, const TVisit& acVisit) noexcept
{
    using TIsNode = uint8_t* (*)(uint8_t*);
    uint8_t* pNode = (*reinterpret_cast<TIsNode* const*>(apObject))[4](apObject);
    if (!pNode)
        return;
    auto* const* pChildren = *reinterpret_cast<uint8_t* const* const*>(pNode + kChildrenOffset);
    const uint16_t count = *reinterpret_cast<const uint16_t*>(pNode + kChildCountOffset);
    for (uint16_t i = 0; pChildren && i < count; ++i)
    {
        if (pChildren[i])
            acVisit(pChildren[i]);
    }
}

void IncRef(uint8_t* apObject) noexcept;
// The last reference let go deletes it
void DecRef(uint8_t* apObject) noexcept;
// apChild under apNode (the node takes a reference), and out from under its parent again (the parent's let go)
void AttachChild(uint8_t* apNode, uint8_t* apChild) noexcept;
void Detach(uint8_t* apChild) noexcept;
// A deep copy of a node's tree with the game's default cloning (NiObject::Clone, id 604942): nobody's reference yet
[[nodiscard]] uint8_t* Clone(uint8_t* apObject) noexcept;

// A reference's 3D (LOADED_REF_DATA::data3D: the third person one for the player), the player's first person one
[[nodiscard]] uint8_t* GetThirdPersonRoot(const TESObjectREFR* apReference) noexcept;
[[nodiscard]] uint8_t* GetFirstPersonRoot(PlayerCharacter* apPlayer) noexcept;
// The player's camera is in first person (PlayerCamera's current state)
[[nodiscard]] bool IsFirstPerson() noexcept;
} // namespace NodeTools
