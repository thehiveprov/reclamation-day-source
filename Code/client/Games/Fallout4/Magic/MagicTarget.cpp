#include "MagicTarget.h"

#include <Actor.h>
#include <Games/ActorExtension.h>
#include <World.h>
#include <Games/ActorExtension.h>
#include "EffectItem.h"
#include "EffectSetting.h"
#include "MagicItem.h"

#include <Games/Overrides.h>

#include <Events/AddTargetEvent.h>

TP_THIS_FUNCTION(TAddTarget, bool, MagicTarget, MagicTarget::AddTargetData& arData);
TP_THIS_FUNCTION(TCheckAddEffectTargetData, bool, MagicTarget::AddTargetData, void* arArgs, float afResistance);

static TAddTarget* RealAddTarget = nullptr;
static TCheckAddEffectTargetData* RealCheckAddEffectTargetData = nullptr;

static thread_local bool s_autoSucceedEffectCheck = false;

bool MagicTarget::AddTarget(AddTargetData& arData) noexcept
{
    s_autoSucceedEffectCheck = true;
    bool result = TiltedPhoques::ThisCall(RealAddTarget, this, arData);
    s_autoSucceedEffectCheck = false;
    return result;
}

Actor* MagicTarget::GetTargetAsActor()
{
    TP_THIS_FUNCTION(TGetTargetAsActor, Actor*, MagicTarget);
    POINTER_FALLOUT4(TGetTargetAsActor, getTargetAsActor, 129826);
    return TiltedPhoques::ThisCall(getTargetAsActor, this);
}

// Fallout 4: an effect lands only in the game that owns its target, or of the local player's doing; other players'
// characters get none here (their own game applies them, health is synced). Sent for the other games to apply
// (MagicService, which lets them through here with ScopedSpellCastOverride): what the local player does to an actor
// another game owns (cryo, fire, poison, a legendary weapon's effect, a stimpak on someone's companion), as that game
// blocks the hit's replay below; and what the others see (EffectSetting::IsSeenByOthers) on the local player or an
// actor this game owns. Never what spawns something: the caster's game did.
bool TP_MAKE_THISCALL(HookAddTarget, MagicTarget, MagicTarget::AddTargetData& arData)
{
    Actor* pTargetActor = apThis->GetTargetAsActor();
    if (!pTargetActor)
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);

    ActorExtension* pTargetActorEx = pTargetActor->GetExtension();

    if (!pTargetActorEx)
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);

    if (ScopedSpellCastOverride::IsOverriden())
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);

    const EffectSetting* pSetting = arData.pEffectItem ? arData.pEffectItem->pEffectSetting : nullptr;
    if (!pSetting || !arData.pSpell)
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);

    AddTargetEvent addTargetEvent{};
    addTargetEvent.TargetID = pTargetActor->formID;
    addTargetEvent.CasterID = arData.pCaster ? arData.pCaster->formID : 0;
    addTargetEvent.SpellID = arData.pSpell->formID;
    addTargetEvent.EffectID = pSetting->formID;
    addTargetEvent.Magnitude = arData.fMagnitude;

    const auto apply = [&](bool aSend)
    {
        const bool result = TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
        if (result && aSend && !pSetting->ChangesTheWorld())
            World::Get().GetRunner().Trigger(addTargetEvent);
        return result;
    };

    if (pTargetActorEx->IsLocalPlayer())
        return apply(pSetting->IsSeenByOthers());
    else if (pTargetActorEx->IsRemotePlayer())
        return false;

    if (arData.pCaster)
    {
        ActorExtension* pCasterExtension = arData.pCaster->GetExtension();
        if (pCasterExtension->IsLocalPlayer())
            return apply(!pTargetActorEx->IsLocal() || pSetting->IsSeenByOthers());
        else if (pCasterExtension->IsRemotePlayer())
            return false;
    }

    if (pTargetActorEx->IsLocal())
        return apply(pSetting->IsSeenByOthers());
    else
        return false;
}

bool TP_MAKE_THISCALL(HookCheckAddEffectTargetData, MagicTarget::AddTargetData, void* arArgs, float afResistance)
{
    if (s_autoSucceedEffectCheck)
        return true;

    return TiltedPhoques::ThisCall(RealCheckAddEffectTargetData, apThis, arArgs, afResistance);
}

static TiltedPhoques::Initializer s_magicTargetHooks(
    []()
    {
        POINTER_FALLOUT4(TAddTarget, addTarget, 385036);
        POINTER_FALLOUT4(TCheckAddEffectTargetData, checkAddEffectTargetData, 113034);

        RealAddTarget = addTarget.Get();
        RealCheckAddEffectTargetData = checkAddEffectTargetData.Get();

        TP_HOOK(&RealAddTarget, HookAddTarget);
        TP_HOOK(&RealCheckAddEffectTargetData, HookCheckAddEffectTargetData);
    });
