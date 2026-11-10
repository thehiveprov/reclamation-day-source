#include <HivemindPCH.h>

#include <NodeTools.h>

#include <PlayerCharacter.h>
#include <Misc/BSFixedString.h>

#include <intrin.h>

namespace
{
// PlayerCharacter::firstPerson3D (CommonLibF4, 1.10.163); TESObjectREFR::loadedData at 0xF0, its data3D at 0x08
constexpr uintptr_t kFirstPerson3DOffset = 0xB78;
constexpr uintptr_t kLoadedDataOffset = 0xF0;
constexpr uintptr_t kData3DOffset = 0x08;
// NiObjectNET::name
constexpr uintptr_t kNameOffset = 0x10;
} // namespace

namespace NodeTools
{
Transform Read(const uint8_t* apNode, uintptr_t aOffset) noexcept
{
    const auto* pFloats = reinterpret_cast<const float*>(apNode + aOffset);
    Transform transform{};
    for (int row = 0; row < 3; ++row)
        transform.Rows[row] = glm::vec3(pFloats[row * 4 + 0], pFloats[row * 4 + 1], pFloats[row * 4 + 2]);
    transform.Translation = glm::vec3(pFloats[12], pFloats[13], pFloats[14]);
    transform.Scale = pFloats[15];
    return transform;
}

void Write(uint8_t* apNode, uintptr_t aOffset, const Transform& acTransform) noexcept
{
    auto* pFloats = reinterpret_cast<float*>(apNode + aOffset);
    for (int row = 0; row < 3; ++row)
    {
        pFloats[row * 4 + 0] = acTransform.Rows[row].x;
        pFloats[row * 4 + 1] = acTransform.Rows[row].y;
        pFloats[row * 4 + 2] = acTransform.Rows[row].z;
    }
    pFloats[12] = acTransform.Translation.x;
    pFloats[13] = acTransform.Translation.y;
    pFloats[14] = acTransform.Translation.z;
    pFloats[15] = acTransform.Scale;
}

Transform Combine(const Transform& acParent, const Transform& acLocal) noexcept
{
    Transform world{};
    for (int row = 0; row < 3; ++row)
        world.Rows[row] = acParent.Apply(acLocal.Rows[row], false);
    world.Translation = acParent.Apply(acLocal.Translation);
    world.Scale = acParent.Scale * acLocal.Scale;
    return world;
}

Transform Relative(const Transform& acParent, const Transform& acWorld) noexcept
{
    const float parentScale = acParent.Scale != 0.f ? acParent.Scale : 1.f;
    Transform local{};
    for (int row = 0; row < 3; ++row)
    {
        local.Rows[row] = glm::vec3(glm::dot(acWorld.Rows[row], acParent.Rows[0]), glm::dot(acWorld.Rows[row], acParent.Rows[1]),
                                    glm::dot(acWorld.Rows[row], acParent.Rows[2]));
    }
    const glm::vec3 moved = acWorld.Translation - acParent.Translation;
    local.Translation = glm::vec3(glm::dot(moved, acParent.Rows[0]), glm::dot(moved, acParent.Rows[1]), glm::dot(moved, acParent.Rows[2])) / parentScale;
    local.Scale = acWorld.Scale / parentScale;
    return local;
}

const char* GetName(const uint8_t* apNode) noexcept
{
    if (!apNode)
        return "";
    const auto* pName = reinterpret_cast<const BSFixedString*>(apNode + kNameOffset);
    const char* pText = pName->AsAscii();
    return pText ? pText : "";
}

void SetName(uint8_t* apNode, const char* acpName) noexcept
{
    if (apNode)
        reinterpret_cast<BSFixedString*>(apNode + kNameOffset)->Set(acpName);
}

uint8_t* GetObjectByName(uint8_t* apNode, const BSFixedString& acName) noexcept
{
    if (!apNode)
        return nullptr;
    using TGetObjectByName = uint8_t* (*)(uint8_t*, const BSFixedString&);
    return (*reinterpret_cast<TGetObjectByName* const*>(apNode))[0x2E](apNode, acName);
}

void IncRef(uint8_t* apObject) noexcept
{
    if (apObject)
        _InterlockedIncrement(reinterpret_cast<volatile long*>(apObject + kRefCountOffset));
}

void DecRef(uint8_t* apObject) noexcept
{
    if (!apObject)
        return;
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(apObject + kRefCountOffset)) == 0)
    {
        using TDeleteThis = void (*)(uint8_t*);
        (*reinterpret_cast<TDeleteThis* const*>(apObject))[1](apObject);
    }
}

void AttachChild(uint8_t* apNode, uint8_t* apChild) noexcept
{
    if (!apNode || !apChild)
        return;
    using TAttachChild = void (*)(uint8_t*, uint8_t*, bool);
    (*reinterpret_cast<TAttachChild* const*>(apNode))[0x3A](apNode, apChild, true);
}

void Detach(uint8_t* apChild) noexcept
{
    uint8_t* pParent = GetParent(apChild);
    if (!pParent)
        return;
    using TDetachChild = void (*)(uint8_t*, uint8_t*);
    (*reinterpret_cast<TDetachChild* const*>(pParent))[0x3D](pParent, apChild);
}

uint8_t* Clone(uint8_t* apObject) noexcept
{
    if (!apObject)
        return nullptr;
    using TClone = uint8_t*(uint8_t*);
    POINTER_FALLOUT4(TClone, s_clone, 604943);
    return s_clone.Get()(apObject);
}

uint8_t* GetThirdPersonRoot(const TESObjectREFR* apReference) noexcept
{
    if (!apReference)
        return nullptr;
    const uint8_t* pLoaded = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<const uint8_t*>(apReference) + kLoadedDataOffset);
    return pLoaded ? *reinterpret_cast<uint8_t* const*>(pLoaded + kData3DOffset) : nullptr;
}

uint8_t* GetFirstPersonRoot(PlayerCharacter* apPlayer) noexcept
{
    return apPlayer ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(apPlayer) + kFirstPerson3DOffset) : nullptr;
}

// PlayerCamera (CommonLibF4 id 1171980): TESCamera::currentState at 0x28, TESCameraState::id at 0x20, 0 = first person
bool IsFirstPerson() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    const uint8_t* pCamera = *s_playerCamera.Get();
    if (!pCamera)
        return false;
    const uint8_t* pState = *reinterpret_cast<const uint8_t* const*>(pCamera + 0x28);
    return pState && *reinterpret_cast<const uint32_t*>(pState + 0x20) == 0;
}
} // namespace NodeTools
