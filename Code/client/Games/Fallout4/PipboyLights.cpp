#include <PipboyLights.h>

#include <Actor.h>
#include <PlayerCharacter.h>
#include <DefaultObjectManager.h>
#include <Forms/TESForm.h>

namespace
{
// Read from ShowPipboyLight (1.10.163, CommonLibF4 ids; RD's are + 1)
using TIsPipboyLightOn = bool(PlayerCharacter*);                 // 426550: the player's BSLight (+0xC38) is set
using TFindLightNode = uint32_t(const void* apRoot, void* apContext); // 1575000: {void** light, void** node} out
// 30546: TESObjectLIGH's dynamic light on a node: the NiLight (its BSLight goes to apOutLight)
using TGenDynamic = void*(TESForm* apLight, void* apRef, void* apNode, bool, bool, bool, void** apOutLight, float, bool);
using TRemoveLight = void(void* apShadowSceneNode, void** apLight); // 1410391

constexpr size_t kGet3DFirstPerson = 0x8B; // TESObjectREFR::Get3D(bool first person): the third person body with false
constexpr size_t kDetachChild = 0x3D;      // NiNode, slot 0x1E8 / 8: takes the child off
constexpr size_t kPipboyLightOffset = 0xBC8; // BGSDefaultObjectManager: PipboyLight_DO
constexpr uint8_t kLightFormType = 0x22;

struct Light
{
    void* pNiLight = nullptr; // ours (a reference held)
    void* pBSLight = nullptr; // the game's NiPointer, filled by GenDynamic
    const void* pRoot = nullptr;
};

// By character form id
TiltedPhoques::Map<uint32_t, Light> s_lights;

template <class T, class... Args> T CallVirtual(const void* apThis, size_t aSlot, Args... aArgs)
{
    using TFunc = T(const void*, Args...);
    return (*reinterpret_cast<TFunc* const* const*>(apThis))[aSlot](apThis, aArgs...);
}

void Release(void* apObject) noexcept
{
    // NiRefObject: count at +8, DeleteThis in slot 1
    if (apObject && _InterlockedDecrement(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(apObject) + 8)) == 0)
        CallVirtual<void>(apObject, 1);
}

const void* GetBody(Actor* apActor) noexcept
{
    return CallVirtual<const void*>(apActor, kGet3DFirstPerson, false);
}

void Remove(Light& arLight, bool aDetach) noexcept
{
    POINTER_FALLOUT4(void*, s_shadowSceneNode, 1327070);
    POINTER_FALLOUT4(TRemoveLight, s_removeLight, 1410392);

    if (void* pShadowSceneNode = *s_shadowSceneNode.Get(); pShadowSceneNode && arLight.pBSLight)
        s_removeLight.Get()(pShadowSceneNode, &arLight.pBSLight);

    // Off its node, when that's still the character's body (a body that was replaced takes its children with it)
    if (aDetach && arLight.pNiLight)
    {
        if (void* pParent = *reinterpret_cast<void**>(static_cast<uint8_t*>(arLight.pNiLight) + 0x28))
            CallVirtual<void>(pParent, kDetachChild, arLight.pNiLight);
    }

    Release(std::exchange(arLight.pBSLight, nullptr));
    Release(std::exchange(arLight.pNiLight, nullptr));
    arLight.pRoot = nullptr;
}

bool Attach(Actor* apActor, Light& arLight) noexcept
{
    POINTER_FALLOUT4(TFindLightNode, s_findLightNode, 1575001);
    POINTER_FALLOUT4(TGenDynamic, s_genDynamic, 30547);
    POINTER_FALLOUT4(float, s_fade, 1111174);

    const void* pRoot = GetBody(apActor);
    if (!pRoot)
        return false;

    // The Pip-Boy's own light and node in the body, as the game looks for them
    void* pFoundLight = nullptr;
    void* pFoundNode = nullptr;
    void* context[2] = {&pFoundLight, &pFoundNode};
    s_findLightNode.Get()(pRoot, context);

    TESForm* pLightForm = pFoundLight && pFoundNode ? static_cast<TESForm*>(pFoundLight) : nullptr;
    if (!pLightForm)
    {
        auto* pDefault = *reinterpret_cast<TESForm**>(reinterpret_cast<uint8_t*>(&DefaultObjectManager::Get()) + kPipboyLightOffset);
        if (pDefault && static_cast<uint8_t>(pDefault->formType) == kLightFormType)
            pLightForm = pDefault;
    }

    void* pNode = pFoundNode ? pFoundNode : const_cast<void*>(pRoot);
    void* pNiLight = pLightForm ? s_genDynamic.Get()(pLightForm, nullptr, pNode, true, true, false, &arLight.pBSLight, *s_fade.Get(), false) : nullptr;
    Release(pFoundNode);

    if (!pNiLight)
    {
        Release(std::exchange(arLight.pBSLight, nullptr));
        return false;
    }

    _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<uint8_t*>(pNiLight) + 8));
    arLight.pNiLight = pNiLight;
    arLight.pRoot = pRoot;
    return true;
}
} // namespace

namespace PipboyLights
{
bool IsLocalOn() noexcept
{
    POINTER_FALLOUT4(TIsPipboyLightOn, s_isPipboyLightOn, 426551);

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && s_isPipboyLightOn.Get()(pPlayer);
}

void Update(const Vector<Actor*>& acLit) noexcept
{
    TiltedPhoques::Set<uint32_t> lit;
    for (Actor* pActor : acLit)
    {
        if (!pActor)
            continue;

        lit.insert(pActor->formID);
        Light& light = s_lights[pActor->formID];

        // A body rebuilt (a cell change, an outfit swap) lost the light with it
        const void* pRoot = GetBody(pActor);
        if (light.pNiLight && light.pRoot == pRoot)
            continue;

        if (light.pNiLight)
            Remove(light, false);

        if (pRoot && Attach(pActor, light))
            spdlog::info("Pip-Boy light on for {:X}", pActor->formID);
    }

    for (auto it = s_lights.begin(); it != s_lights.end();)
    {
        if (lit.contains(it->first))
        {
            ++it;
            continue;
        }

        // Still the same body (the character is still here): take the light off it
        auto* pActor = Cast<Actor>(TESForm::GetById(it->first));
        Remove(it.value(), pActor && GetBody(pActor) == it->second.pRoot);
        spdlog::info("Pip-Boy light off for {:X}", it->first);
        it = s_lights.erase(it);
    }
}
} // namespace PipboyLights
