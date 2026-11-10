#include <ExtraData/ExtraFactionChanges.h>
#include <Forms/TESFaction.h>
#include <Forms/TESNPC.h>
#include <Games/Memory.h>
#include <Games/References.h>
#include <PlayerCharacter.h>
#include <Games/Fallout4/EquipManager.h>
#include <Forms/BGSObjectInstance.h>
#include <Games/Misc/ActorKnowledge.h>
#include <ExtraData/ExtraDataList.h>

#include <Services/PapyrusService.h>
#include <World.h>

#include <Effects/ValueModifierEffect.h>

#include <Events/HealthChangeEvent.h>
#include <Events/ActorDamageEvent.h>
#include <Events/DialogueEvent.h>
#include <Events/DialogueLockEvent.h>
#include <Services/CharacterService.h>
#include <Services/FactionService.h>

#include <Games/Overrides.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <PerfProbe.h>

TP_THIS_FUNCTION(TActorConstructor, Actor*, Actor, uint8_t aUnk);
TP_THIS_FUNCTION(TActorConstructor2, Actor*, Actor, volatile int** aRefCount, uint8_t aUnk);
TP_THIS_FUNCTION(TActorDestructor, void*, Actor);

static TActorConstructor* RealActorConstructor;
static TActorConstructor2* RealActorConstructor2;
static TActorDestructor* RealActorDestructor;

Actor* TP_MAKE_THISCALL(HookActorContructor, Actor, uint8_t aUnk)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    const auto pActor = TiltedPhoques::ThisCall(RealActorConstructor, apThis, aUnk);

    return pActor;
}

Actor* TP_MAKE_THISCALL(HookActorContructor2, Actor, volatile int** aRefCount, uint8_t aUnk)
{
    TP_EMPTY_HOOK_PLACEHOLDER;

    const auto pActor = TiltedPhoques::ThisCall(RealActorConstructor2, apThis, aRefCount, aUnk);

    return pActor;
}

void* TP_MAKE_THISCALL(HookActorDestructor, Actor)
{
    // TODO: Actor dtor sometimes has garbage actor, causing a crash
    return TiltedPhoques::ThisCall(RealActorDestructor, apThis);
}

GamePtr<Actor> Actor::New() noexcept
{
    const auto pActor = Memory::Allocate<Actor>();

    TiltedPhoques::ThisCall(HookActorContructor, pActor, uint8_t(1));

    return pActor;
}

TESForm* Actor::GetEquippedWeapon(uint32_t aSlotId) const noexcept
{
    using TGetEquippedWeapon = TESForm*(__fastcall)(void*, void*, const Actor*, uint32_t);

    POINTER_FALLOUT4(TGetEquippedWeapon, s_getEquippedWeapon, 811140);

    return s_getEquippedWeapon(nullptr, nullptr, this, aSlotId);
}

Factions Actor::GetFactions() const noexcept
{
    Factions result;

    auto& modSystem = World::Get().GetModSystem();

    auto* pNpc = Cast<TESNPC>(baseForm);
    if (pNpc)
    {
        auto& factions = pNpc->actorData.factions;

        for (auto i = 0u; i < factions.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(factions[i].faction->formID, faction.Id);
            faction.Rank = factions[i].rank;

            result.NpcFactions.push_back(faction);
        }
    }

    auto* pFactionExtras = Cast<ExtraFactionChanges>(extraData->GetByType(ExtraDataType::Faction));
    if (pFactionExtras)
    {
        for (auto i = 0u; i < pFactionExtras->entries.length; ++i)
        {
            Faction faction;

            modSystem.GetServerModId(pFactionExtras->entries[i].faction->formID, faction.Id);
            faction.Rank = pFactionExtras->entries[i].rank;

            result.ExtraFactions.push_back(faction);
        }
    }

    return result;
}

ActorValues Actor::GetEssentialActorValues() noexcept
{
    ActorValues actorValues;

    int essentialValues[] = {ActorValueInfo::kHealth};

    for (auto i : essentialValues)
    {
        ActorValueInfo* pActorValueInfo = GetActorValueInfo(i);
        float value = actorValueOwner.GetValue(pActorValueInfo);
        actorValues.ActorValuesList.insert({i, value});
        float maxValue = actorValueOwner.GetMaxValue(pActorValueInfo);
        actorValues.ActorMaxValuesList.insert({i, maxValue});
    }

    ActorValueInfo* pActorValueInfoRads = GetActorValueInfo(ActorValueInfo::kRads);
    float valueRads = actorValueOwner.GetValue(pActorValueInfoRads);
    actorValues.ActorValuesList.insert({ActorValueInfo::kRads, valueRads});
    ActorValueInfo* pActorValueInfoRadsMax = GetActorValueInfo(ActorValueInfo::kRadHealthMax);
    float valueRadsMax = actorValueOwner.GetValue(pActorValueInfoRadsMax);
    actorValues.ActorValuesList.insert({ActorValueInfo::kRadHealthMax, valueRadsMax});

    return actorValues;
}

// use this for fallout 4 projectile launching
void* Actor::GetCurrentWeapon(void* apResult, uint32_t aEquipIndex) noexcept
{
    TP_THIS_FUNCTION(TGetCurrentWeapon, void*, Actor, void* apResult, uint32_t aEquipIndex);
    POINTER_FALLOUT4(TGetCurrentWeapon, getCurrentWeapon, 1277202);
    return TiltedPhoques::ThisCall(getCurrentWeapon, this, apResult, aEquipIndex);
}

float Actor::GetActorValue(uint32_t aId) const noexcept
{
    ActorValueInfo* pActorValueInfo = GetActorValueInfo(aId);
    return actorValueOwner.GetValue(pActorValueInfo);
}

float Actor::GetActorPermanentValue(uint32_t aId) const noexcept
{
    ActorValueInfo* pActorValueInfo = GetActorValueInfo(aId);
    return actorValueOwner.GetMaxValue(pActorValueInfo);
}

Inventory Actor::GetActorInventory() const noexcept
{
    return GetInventory();
}

Inventory Actor::GetEquipment() const noexcept
{
    Inventory inventory = GetInventory();
    inventory.RemoveByFilter([](const auto& entry) { return !entry.IsWorn(); });
    return inventory;
}

void Actor::SetActorValue(uint32_t aId, float aValue) noexcept
{
    ActorValueInfo* pActorValueInfo = GetActorValueInfo(aId);
    actorValueOwner.SetValue(pActorValueInfo, aValue);
}

void Actor::ForceActorValue(ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue) noexcept
{
    // A permanent change moves the maximum: from the maximum, not the current value (upstream fix #756)
    const float initial = aMode == ActorValueOwner::ForceMode::PERMANENT ? GetActorPermanentValue(aId) : GetActorValue(aId);
    if (aValue == initial)
        return;

    ActorValueInfo* pActorValueInfo = GetActorValueInfo(aId);
    actorValueOwner.ForceCurrent(aMode, pActorValueInfo, aValue - initial);
}

void Actor::SetActorValues(const ActorValues& acActorValues) noexcept
{
    for (auto& value : acActorValues.ActorMaxValuesList)
        ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, value.first, value.second);

    for (auto& value : acActorValues.ActorValuesList)
    {
        if (value.first == ActorValueInfo::kRads || value.first == ActorValueInfo::kRadHealthMax)
            actorValueOwner.SetValue(GetActorValueInfo(value.first), value.second);
        ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, value.first, value.second);
    }
}

void Actor::SetFactions(const Factions& acFactions) noexcept
{
    RemoveFromAllFactions();

    auto& modSystem = World::Get().GetModSystem();

    for (const auto& entry : acFactions.NpcFactions)
    {
        auto* pForm = GetById(modSystem.GetGameId(entry.Id));
        auto* pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }

    for (const auto& entry : acFactions.ExtraFactions)
    {
        auto* pForm = GetById(modSystem.GetGameId(entry.Id));
        auto* pFaction = Cast<TESFaction>(pForm);
        if (pFaction)
        {
            SetFactionRank(pFaction, entry.Rank);
        }
    }

    // Another player's character stays in PlayerFaction (SetPlayerRespawnMode), which removing all factions took away,
    // and in the factions of its owner's standing (FactionService, which may put it in PlayerAllyFaction instead)
    if (formID != 0x14 && GetExtension()->IsPlayer())
    {
        if (auto* pPlayerFaction = Cast<TESFaction>(GetById(0x1C21C)))
            SetFactionRank(pPlayerFaction, 1);
        World::Get().ctx().at<FactionService>().ApplyStanding(this);
    }
}

void Actor::SetFactionRank(const TESFaction* acpFaction, int8_t aRank) noexcept
{
    PAPYRUS_FUNCTION(void, Actor, SetFactionRank, const TESFaction*, int8_t);
    s_pSetFactionRank(this, acpFaction, aRank);
}

void Actor::SetActorInventory(const Inventory& acInventory) noexcept
{
    spdlog::info("Setting actor inventory, form id: {:X}", formID);

    UnEquipAll();

    SetInventory(acInventory);
}

void Actor::UnEquipAll() noexcept
{
    TP_THIS_FUNCTION(TUnEquipAll, void, Actor);
    POINTER_FALLOUT4(TUnEquipAll, s_unequipAll, 1260318);
    TiltedPhoques::ThisCall(s_unequipAll, this);
}

void Actor::RemoveFromAllFactions() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, RemoveFromAllFactions);
    s_pRemoveFromAllFactions(this);
}

bool Actor::IsDead() const noexcept
{
    PAPYRUS_FUNCTION(bool, Actor, IsDead);
    return s_pIsDead(this);
}

void Actor::Kill() noexcept
{
    PAPYRUS_FUNCTION(void, Actor, Kill, void*);
    s_pKill(this, NULL);
}

void Actor::Reset() noexcept
{
    using ObjectReference = TESObjectREFR;
    PAPYRUS_FUNCTION(void, ObjectReference, Reset, int, TESObjectREFR*);
    s_pReset(this, 0, nullptr);
}

void Actor::Respawn() noexcept
{
    Resurrect(false);
    Reset();
}

// TODO(cosideci): this is flawed, since we need to equip specific stacks.
void Actor::ProcessScriptedEquip(TESBoundObject* apObj, bool abEquipLockState, bool abSilent) noexcept
{
    ScopedEquipOverride _;

    TP_THIS_FUNCTION(TProcessScriptedEquip, void, Actor, TESBoundObject*, bool, bool);
    POINTER_FALLOUT4(TProcessScriptedEquip, processScriptedEquip, 868003);
    TiltedPhoques::ThisCall(processScriptedEquip, this, apObj, abEquipLockState, abSilent);
}

void Actor::DropOrPickUpObject(const Inventory::Entry& arEntry, NiPoint3* apPoint, NiPoint3* apRotate) noexcept
{
    ModSystem& modSystem = World::Get().GetModSystem();

    uint32_t objectId = modSystem.GetGameId(arEntry.BaseId);
    TESBoundObject* pObject = Cast<TESBoundObject>(TESForm::GetById(objectId));
    if (!pObject)
    {
        spdlog::warn("Object to drop not found, {:X}:{:X}.", arEntry.BaseId.ModId, arEntry.BaseId.BaseId);
        return;
    }

    if (arEntry.Count < 0)
        DropObject(pObject, -arEntry.Count, apPoint, apRotate);
    // TODO: pick up
}

void Actor::DropObject(TESBoundObject* apObject, int32_t aCount, NiPoint3* apPoint, NiPoint3* apRotate) noexcept
{
    BGSObjectInstance object(apObject, nullptr);

    TP_THIS_FUNCTION(TDropObject, BSPointerHandle<TESObjectREFR>*, Actor, BSPointerHandle<TESObjectREFR>*, BGSObjectInstance*, void*, int32_t, NiPoint3*, NiPoint3*);
    POINTER_FALLOUT4(TDropObject, dropObject, 1482294);

    BSPointerHandle<TESObjectREFR> result{};
    TiltedPhoques::ThisCall(dropObject, this, &result, &object, nullptr, aCount, apPoint, apRotate);
}

TESObjectREFR* Actor::DropStack(TESBoundObject* apObject, uint32_t aStackIndex, int32_t aCount, const NiPoint3* apPoint, const NiPoint3* apRotate) noexcept
{
    // BSTSmallArray<uint32_t, 4> holding just the stack index, stored inline
    struct StackData
    {
        uint32_t capacityAndLocal; // capacity in bits 0-30, "inline storage" in bit 31
        uint32_t pad;
        uint32_t inlineData[4];
        uint32_t size;
        uint32_t pad2;
    };
    static_assert(sizeof(StackData) == 0x20);

    StackData stackData{};
    stackData.capacityAndLocal = 4u | (1u << 31);
    stackData.inlineData[0] = aStackIndex;
    stackData.size = 1;

    BGSObjectInstance object(apObject, nullptr);

    // Actor::DropObject, vtable slot 0xEB, CommonLibF4 id 1482293
    TP_THIS_FUNCTION(TDropObject, uint32_t*, Actor, uint32_t*, BGSObjectInstance*, StackData*, int32_t, const NiPoint3*, const NiPoint3*);
    POINTER_FALLOUT4(TDropObject, dropObject, 1482294);

    uint32_t handle = 0;
    TiltedPhoques::ThisCall(dropObject, this, &handle, &object, &stackData, aCount, apPoint, apRotate);

    return handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
}

void Actor::EquipItem(TESBoundObject* apObject, uint32_t aStackIndex, uint32_t aCount) noexcept
{
    BGSObjectInstance object(apObject, nullptr);
    EquipManager::Get()->EquipObject(this, object, aStackIndex, aCount, nullptr, false, true, false, false, false);

    // A weapon put in the hands while they hold one out is hidden until the character's own draw animation says it's in
    // the hand, and another player's character doesn't play one for a weapon of the same kind (their Glock or MAC-10
    // stayed invisible until they put it away and drew it again). So it's put away and drawn again, as they would.
    const uint32_t weaponState = actorState.GetWeaponState();
    if (apObject->formType == FormType::Weapon && GetExtension()->IsRemote() && weaponState >= 2 && GetEquippedWeapon(0) == apObject)
        World::Get().ctx().at<CharacterService>().RedrawWeapon(this);
}

void Actor::UnequipItem(TESBoundObject* apObject, const Vector<Inventory::ObjectMod>* apMods) noexcept
{
    // The game's own unequip, of the stack that's worn. Papyrus' UnequipItem did nothing on another player's character:
    // it takes the item off later, by when nothing says anymore that this unequip is ours and not the game's (those
    // are dropped on remote characters, see EquipManager.cpp).
    const auto stackId = FindWornStackIndex(apObject);
    if (!stackId)
        return;

    if (apMods && !Inventory::ObjectMod::SameMods(GetStackObjectMods(apObject, *stackId), *apMods))
        return;

    BGSObjectInstance object(apObject, nullptr);
    EquipManager::Get()->UnequipObject(this, object, 1, nullptr, *stackId, false, true, false, true, nullptr);
}

bool Actor::IsInPowerArmor() const noexcept
{
    // PowerArmor::ActorInPowerArmor(const Actor&), CommonLibF4 id 1176757
    using TActorInPowerArmor = bool(const Actor&);
    POINTER_FALLOUT4(TActorInPowerArmor, s_actorInPowerArmor, 1176758);

    return s_actorInPowerArmor.Get()(*this);
}

bool Actor::CullWeapons(bool aCull) noexcept
{
    // AIProcess's weapon culling (CommonLibF4 id 397172), with the counted reason 0 (InitPipboy / ClosedownPipboy's);
    // the animation events CullWeapons / UncullWeapons use a flag of their own (reason 1). The weapons' 3D is hidden
    // while either is set.
    using TCullWeapons = bool(AIProcess*, Actor*, bool, uint32_t);
    POINTER_FALLOUT4(TCullWeapons, s_cullWeapons, 397173);

    if (!currentProcess)
        return false;
    return s_cullWeapons.Get()(currentProcess, this, aCull, 0);
}

bool Actor::IsWeaponCulledAsSheathed() const noexcept
{
    // The culling state is a byte at the middle-high process + 0x498: the counted hides in its high bits, flags in the
    // low ones. Flag 2 is set when the weapon is put away (WeaponSheatheHandler) or swapped while out, and only cleared
    // by the weaponDraw animation event (WeaponDrawHandler, id 1151906 -> 222070).
    if (!currentProcess || !currentProcess->middleProcess)
        return false;
    return (*(reinterpret_cast<const uint8_t*>(currentProcess->middleProcess) + 0x498) & 2) != 0;
}

bhkCharacterController* Actor::GetCharController() const noexcept
{
    // only actors in high/middle-high process have one (the player always does)
    if (!currentProcess || !currentProcess->middleProcess)
        return nullptr;

    return currentProcess->middleProcess->charController;
}

bool Actor::SwitchToPowerArmor(TESObjectREFR* apFrame) noexcept
{
    if (!World::Get().ctx().at<PapyrusService>().Get("Actor", "SwitchToPowerArmor"))
    {
        spdlog::warn("{}: Actor.SwitchToPowerArmor not found", __FUNCTION__);
        return false;
    }

    // Getting in equips the frame's pieces, which is otherwise blocked for remote characters
    ScopedEquipOverride _;

    PAPYRUS_FUNCTION(void, Actor, SwitchToPowerArmor, TESObjectREFR*);
    spdlog::info("{}: {:X} into {:X}", __FUNCTION__, formID, apFrame ? apFrame->formID : 0);
    s_pSwitchToPowerArmor(this, apFrame);
    return true;
}

TP_THIS_FUNCTION(TDamageActor, bool, Actor, float aDamage, Actor* apHitter);
static TDamageActor* RealDamageActor = nullptr;

bool TP_MAKE_THISCALL(HookDamageActor, Actor, float aDamage, Actor* apHitter)
{
    const auto pExHittee = apThis->GetExtension();

    // Nemeses: who fought the local player (NemesisService), hits with the local player on one side only
    if (apHitter && aDamage > 0.f && (pExHittee->IsLocalPlayer() || apHitter->GetExtension()->IsLocalPlayer()))
        World::Get().GetRunner().Trigger(ActorDamageEvent(apThis->formID, apHitter->formID, aDamage));
    if (pExHittee->IsLocalPlayer())
    {
        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -aDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter);
    }
    else if (pExHittee->IsRemotePlayer())
    {
        return false;
    }

    if (apHitter)
    {
        const auto pExHitter = apHitter->GetExtension();
        if (pExHitter->IsLocalPlayer())
        {
            World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -aDamage));
            return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter);
        }
        if (pExHitter->IsRemotePlayer())
        {
            return false;
        }
    }

    if (pExHittee->IsLocal())
    {
        World::Get().GetRunner().Trigger(HealthChangeEvent(apThis->formID, -aDamage));
        return TiltedPhoques::ThisCall(RealDamageActor, apThis, aDamage, apHitter);
    }
    else
    {
        return false;
    }
}

TP_THIS_FUNCTION(TApplyActorEffect, void, ActiveEffect, Actor* apTarget, float aEffectValue, ActorValueInfo* apActorValueInfo);
static TApplyActorEffect* RealApplyActorEffect = nullptr;

void TP_MAKE_THISCALL(HookApplyActorEffect, ActiveEffect, Actor* apTarget, float aEffectValue, ActorValueInfo* apActorValueInfo)
{
    const auto* pValueModEffect = Cast<ValueModifierEffect>(apThis);

    if (pValueModEffect)
    {
        ActorValueInfo* pHealthActorValueInfo = apTarget->GetActorValueInfo(ActorValueInfo::kHealth);
        if (pValueModEffect->actorValueInfo == pHealthActorValueInfo && aEffectValue > 0.0f)
        {
            if (apTarget && apTarget->GetExtension())
            {
                const auto pExTarget = apTarget->GetExtension();
                if (pExTarget->IsLocal())
                {
                    World::Get().GetRunner().Trigger(HealthChangeEvent(apTarget->formID, aEffectValue));
                    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, apActorValueInfo);
                }
                return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealApplyActorEffect, apThis, apTarget, aEffectValue, apActorValueInfo);
}

TP_THIS_FUNCTION(TRunDetection, void, void, ActorKnowledge*);
static TRunDetection* RealRunDetection = nullptr;

void TP_MAKE_THISCALL(HookRunDetection, void, ActorKnowledge* apTarget)
{
    static PerfProbe::HookStat s_perf("RunDetection");
    PerfProbe::Hook _perf(s_perf);
    auto pOwner = TESObjectREFR::GetByHandle(apTarget->hOwner);
    auto pTarget = TESObjectREFR::GetByHandle(apTarget->hTarget);

    if (pOwner && pTarget)
    {
        auto pOwnerActor = Cast<Actor>(pOwner);
        auto pTargetActor = Cast<Actor>(pTarget);
        if (pOwnerActor && pTargetActor)
        {
            if (pOwnerActor->GetExtension()->IsRemotePlayer() && pTargetActor->GetExtension()->IsLocalPlayer())
            {
                spdlog::info("Cancelling detection from remote player to local player");
                return;
            }
        }
    }

    PerfProbe::GameCall _game;
    return TiltedPhoques::ThisCall(RealRunDetection, apThis, apTarget);
}

TP_THIS_FUNCTION(TSpeakSoundFunction, float, Actor, const char* apName, uint32_t* aSoundHand, void* apArchTypeAnimation, int32_t aiStringLength, bool abSetEmotion, void* apOutputModel, bool abQueue, bool abLip, bool abPCapcall);
static TSpeakSoundFunction* RealSpeakSoundFunction = nullptr;

bool TP_MAKE_THISCALL(HookSpeakSoundFunction, Actor, const char* apName, uint32_t* aSoundHand, void* apArchTypeAnimation, int32_t aiStringLength, bool abSetEmotion, void* apOutputModel, bool abQueue, bool abLip, bool abPCapcall)
{
    if (apThis->GetExtension()->IsLocal())
        World::Get().GetRunner().Trigger(DialogueEvent(apThis->formID, apName));

    return TiltedPhoques::ThisCall(RealSpeakSoundFunction, apThis, apName, aSoundHand, apArchTypeAnimation, aiStringLength, abSetEmotion, apOutputModel, abQueue, abLip, abPCapcall);
}

void Actor::SpeakSound(const char* pFile)
{
    uint32_t handle[4]{};
    handle[0] = -1;
    TiltedPhoques::ThisCall(RealSpeakSoundFunction, this, pFile, handle, nullptr, 0, false, nullptr, false, true, false);
}

// TESObjectREFR::SetDialoguewithPlayer (vtable slot 0x52, CommonLibF4): called on the NPC when a conversation with
// the player starts (aFlag) and ends (!aFlag). Whoever starts talking to an NPC first makes the choices; the others
// can't start their own conversation with it and follow along through the synced voice lines and subtitles.
TP_THIS_FUNCTION(TSetDialogueWithPlayer, bool, TESObjectREFR, bool aFlag, bool aForceGreet, void* apTopicInfo);
static TSetDialogueWithPlayer* RealSetDialogueWithPlayer = nullptr;

bool TP_MAKE_THISCALL(HookSetDialogueWithPlayer, TESObjectREFR, bool aFlag, bool aForceGreet, void* apTopicInfo)
{
    Actor* pActor = Cast<Actor>(apThis);
    if (pActor && !pActor->GetExtension()->IsPlayer())
    {
        if (aFlag && World::Get().GetCharacterService().IsInDialogueWithOtherPlayer(pActor->formID))
        {
            spdlog::info("Not starting a conversation with {:X}: another player is talking to them", pActor->formID);
            return false;
        }

        World::Get().GetRunner().Trigger(DialogueLockEvent(pActor->formID, aFlag));
    }

    return TiltedPhoques::ThisCall(RealSetDialogueWithPlayer, apThis, aFlag, aForceGreet, apTopicInfo);
}

static TiltedPhoques::Initializer s_specificReferencesHooks(
    []()
    {
        POINTER_FALLOUT4(TActorConstructor, s_actorCtor, 1027501);
        POINTER_FALLOUT4(TActorConstructor2, s_actorCtor2, 1331729);
        POINTER_FALLOUT4(TActorDestructor, s_actorDtor, 1104083);
        POINTER_FALLOUT4(TDamageActor, s_damageActor, 1539011);
        // TODO: not sure about this ID, seems to interfere with jump when hooked?
        POINTER_FALLOUT4(TApplyActorEffect, s_applyActorEffect, 703727);
        POINTER_FALLOUT4(TRunDetection, s_runDetection, 906785);
        POINTER_FALLOUT4(TSpeakSoundFunction, s_speakSoundFunction, 1567997);
        // Actor's vtable (CommonLibF4 VTABLE::Actor[0] + 1), NPCs use its SetDialoguewithPlayer
        POINTER_FALLOUT4(void*, s_actorVtable, 1455517);
        RealSetDialogueWithPlayer = reinterpret_cast<TSetDialogueWithPlayer*>(s_actorVtable.Get()[0x52]);
        TP_HOOK(&RealSetDialogueWithPlayer, HookSetDialogueWithPlayer);

        RealActorConstructor = s_actorCtor.Get();
        RealActorConstructor2 = s_actorCtor2.Get();
        RealActorDestructor = s_actorDtor.Get();
        RealDamageActor = s_damageActor.Get();
        RealApplyActorEffect = s_applyActorEffect.Get();
        RealRunDetection = s_runDetection.Get();
        RealSpeakSoundFunction = s_speakSoundFunction.Get();

        TP_HOOK(&RealActorConstructor, HookActorContructor);
        TP_HOOK(&RealActorConstructor2, HookActorContructor2);
        TP_HOOK(&RealActorDestructor, HookActorDestructor);
        TP_HOOK(&RealDamageActor, HookDamageActor);
        TP_HOOK(&RealApplyActorEffect, HookApplyActorEffect);
        TP_HOOK(&RealRunDetection, HookRunDetection);
        TP_HOOK(&RealSpeakSoundFunction, HookSpeakSoundFunction);
    });
