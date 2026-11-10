#pragma once

#include <Forms/TESForm.h>
#include <Misc/ActorState.h>
#include <Magic/MagicTarget.h>
#include <Structs/ActorValues.h>
#include <Structs/Factions.h>
#include <Structs/Inventory.h>
#include <TESObjectREFR.h>

#include <Events.h>
#include <Games/Events.h>

struct ExActor;
struct ExPlayerCharacter;
struct ActorExtension;
struct TESNPC;
struct AIProcess;
struct TESFaction;
struct TESPackage;

struct Actor : TESObjectREFR
{
    static constexpr FormType Type = FormType::Character;

    static GamePtr<Actor> New() noexcept;
    static GamePtr<Actor> Create(TESNPC* apNpc) noexcept;
    static GamePtr<Actor> Spawn(uint32_t aBaseFormId) noexcept;

    virtual void sub_C4();
    virtual void sub_C5();
    virtual void sub_C6();
    virtual void sub_C7();
    virtual void sub_C8();
    virtual void SetWeaponDrawn(bool aIsDrawn);
    virtual void SetPosition(const NiPoint3& acPosition, bool aSyncHavok = true);
    virtual void sub_CB();
    virtual void Resurrect(bool aResetInventory);
    virtual void sub_CD();
    virtual void sub_CE();
    virtual void sub_CF();
    virtual void sub_D0();
    virtual void sub_D1();
    virtual void sub_D2();
    virtual void sub_D3();
    virtual void sub_D4();
    virtual void sub_D5();
    virtual void sub_D6();
    virtual void sub_D7();
    virtual void sub_D8();
    virtual void sub_D9();
    virtual void sub_DA();
    virtual void sub_DB();
    virtual void sub_DC();
    virtual void sub_DD();
    virtual void sub_DE();
    virtual void sub_DF();
    virtual void sub_E0();
    virtual void sub_E1();
    virtual void sub_E2();
    virtual void sub_E3();
    virtual void sub_E4();
    virtual void sub_E5();
    virtual void sub_E6();
    virtual void sub_E7();
    virtual void sub_E8();
    virtual void sub_E9();
    virtual void sub_EA();
    virtual void sub_EB();
    virtual void sub_EC();
    virtual void sub_ED();
    virtual void sub_EE();
    virtual void sub_EF();
    virtual void sub_F0();
    virtual void sub_F1();
    virtual void sub_F2();
    virtual void sub_F3();
    virtual void sub_F4();
    virtual void sub_F5();
    virtual void sub_F6();
    virtual void sub_F7();
    virtual void sub_F8();
    virtual void sub_F9();
    virtual void PutCreatedPackage(struct TESPackage*, bool = false, bool = true, bool = true);
    virtual void sub_FB();
    virtual void sub_FC();
    virtual void sub_FD();
    virtual void sub_FE();
    virtual void sub_FF(); // UpdateCombat (Skyrim's PutCreatedPackage slot)
    virtual void sub_100();
    //...
    // virtual void sub_132();

    // Casting
    ActorExtension* GetExtension() noexcept;
    ExActor* AsExActor() noexcept;
    ExPlayerCharacter* AsExPlayerCharacter() noexcept;

    // Getters
    float GetSpeed() noexcept;
    TESForm* GetEquippedWeapon(uint32_t aSlotId) const noexcept;
    Inventory GetActorInventory() const noexcept;
    Inventory GetEquipment() const noexcept;
    Factions GetFactions() const noexcept;
    ActorValues GetEssentialActorValues() noexcept;
    float GetActorValue(uint32_t aId) const noexcept;
    float GetActorPermanentValue(uint32_t aId) const noexcept;
    void* GetCurrentWeapon(void* apResult, uint32_t aEquipIndex) noexcept;
    uint16_t GetLevel() const noexcept;
    [[nodiscard]] bool IsDead() const noexcept;
    [[nodiscard]] bool IsInCombat() const noexcept;
    [[nodiscard]] Actor* GetCombatTarget() const noexcept;
    [[nodiscard]] bool HasPerk(uint32_t aPerkFormId) const noexcept;
    [[nodiscard]] uint8_t GetPerkRank(uint32_t aPerkFormId) const noexcept;
    [[nodiscard]] bool IsInPowerArmor() const noexcept;
    // The Havok character controller the actor moves with, null when it has none (low process)
    [[nodiscard]] struct bhkCharacterController* GetCharController() const noexcept;

    // Setters
    void SetSpeed(float aSpeed) noexcept;
    void SetLevelMod(uint32_t aLevel) noexcept;
    void SetActorInventory(const Inventory& acInventory) noexcept;
    void SetActorValue(uint32_t aId, float aValue) noexcept;
    void ForceActorValue(ActorValueOwner::ForceMode aMode, uint32_t aId, float aValue) noexcept;
    void SetActorValues(const ActorValues& acActorValues) noexcept;
    void SetFactions(const Factions& acFactions) noexcept;
    void SetFactionRank(const TESFaction* acpFaction, int8_t aRank) noexcept;
    void ForcePosition(const NiPoint3& acPosition) noexcept;
    void SetWeaponDrawnEx(bool aDraw) noexcept;
    // Hides the weapons the actor holds (or shows them again), as the game's own Pip-Boy does while it's up. Counted:
    // every hide needs its show, and the game's own hiding is kept apart. True when the weapons' visibility changed.
    bool CullWeapons(bool aCull) noexcept;
    // Whether the weapon is hidden the way the game hides one that was put away or swapped, until the draw animation
    // says it's in the hand (its weaponDraw event)
    bool IsWeaponCulledAsSheathed() const noexcept;
    void SetPackage(TESPackage* apPackage) noexcept;
    void SetNoBleedoutRecovery(bool aSet) noexcept;
    void SetEssentialEx(bool aSet) noexcept;
    void SetPlayerRespawnMode(bool aSet = true) noexcept;

    // Actions
    void UnEquipAll() noexcept;
    void QueueUpdate() noexcept;
    void RemoveFromAllFactions() noexcept;
    void DispelAllSpells(bool aNow = false) noexcept;
    void Kill() noexcept;
    void Respawn() noexcept;
    void Reset() noexcept;
    void SpeakSound(const char* pFile);
    void ProcessScriptedEquip(TESBoundObject* apObj, bool abEquipLockState = false, bool abSilent = true) noexcept;
    void DropObject(TESBoundObject* apObject, int32_t aCount, NiPoint3* apPoint, NiPoint3* apRotate) noexcept;
    void DropOrPickUpObject(const Inventory::Entry& arEntry, NiPoint3* apPoint, NiPoint3* apRotate) noexcept;
    // Puts on the given stack of apObject, on a character whose equipment is set here and not by the game: another
    // player's, or one being built from what its owner sent
    void EquipItem(TESBoundObject* apObject, uint32_t aStackIndex, uint32_t aCount = 1) noexcept;
    // Takes off apObject. With apMods: only if the one that's worn has these mods.
    void UnequipItem(TESBoundObject* apObject, const Vector<Inventory::ObjectMod>* apMods = nullptr) noexcept;
    // Drops aCount of the given inventory stack at apPoint, returns the reference created in the world
    TESObjectREFR* DropStack(TESBoundObject* apObject, uint32_t aStackIndex, int32_t aCount, const NiPoint3* apPoint, const NiPoint3* apRotate) noexcept;
    // Gets into the power armor frame apFrame, or out of the current one with nullptr. False if not available.
    bool SwitchToPowerArmor(TESObjectREFR* apFrame) noexcept;
    void StartCombatEx(Actor* apTarget) noexcept;
    void SetCombatTargetEx(Actor* apTarget) noexcept;
    void StartCombat(Actor* apTarget) noexcept;
    void StopCombat() noexcept;

    enum ActorFlags
    {
        IS_A_MOUNT = 1 << 1,
        NO_BLEEDOUT_RECOVERY = 1 << 5,
        IS_ESSENTIAL = 1 << 18,
    };

    struct ActorValueModifiers
    {
        float permanentModifier;
        float temporaryModifier;
        float damageModifier;
    };

    // Fallout 4 has no mounts, so this stays false; kept for the shared code paths that check it.
    bool IsMount() const noexcept { return flags2 & ActorFlags::IS_A_MOUNT; }

    bool IsEssential() const noexcept { return flags2 & ActorFlags::IS_ESSENTIAL; }
    void SetEssential(bool aSetEssential) noexcept
    {
        if (aSetEssential)
            flags2 |= ActorFlags::IS_ESSENTIAL;
        else
            flags2 &= ~ActorFlags::IS_ESSENTIAL;
    }
    // Set by SetNoBleedoutRecovery: a bleeding out actor stays down
    bool IsBleedoutRecoveryLocked() const noexcept { return flags2 & ActorFlags::NO_BLEEDOUT_RECOVERY; }

    MagicTarget magicTarget;
    ActorState actorState;
    BSTEventSink<BSMovementDataChangedEvent> movementDataChangedSink;
    BSTEventSink<BSTransformDeltaEvent> transformDeltaSink;
    BSTEventSink<BSSubGraphActivationUpdate> subGraphActivationUpdateSink;
    BSTEventSink<bhkCharacterMoveFinishEvent> characterMoveFinishSink;
    BSTEventSink<bhkNonSupportContactEvent> nonSupportContactSink;
    BSTEventSink<bhkCharacterStateChangeEvent> characterStateChangeSink;
    void* postAnimationChannelUpdateFunctor;
    uint8_t unk170[0x2D0 - 0x170];
    uint32_t actorFlags;
    uint8_t unk2D8[0x300 - 0x2D8];
    AIProcess* currentProcess;

    uint8_t pad308[0x3E8 - 0x308];
    TESForm* magicItems[4];
    uint8_t pad408[0x43C - 0x408];
    uint32_t flags2;
    uint8_t pad440[0x4];
    ActorValueModifiers healthModifiers;
    uint8_t padActorEnd[0x490 - 0x450];
};

static_assert(sizeof(Actor) == 0x490);
static_assert(offsetof(Actor, magicItems) == 0x3E8);
