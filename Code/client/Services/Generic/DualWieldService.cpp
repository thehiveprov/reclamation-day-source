#include <Services/DualWieldService.h>

#if TP_FALLOUT4

#include <Games/Fallout4/BikeLink.h>
#include <Services/EmoteService.h>
#include <Services/GrappleService.h>
#include <Services/OverlayService.h>
#include <Services/ParkourService.h>
#include <Services/PipboyMapService.h>
#include <Services/TransportService.h>
#include <Events/OverlayRestartedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestMovementAction.h>
#include <Messages/NotifyMovementAction.h>
#include <World.h>
#include <Utils.h>

#include <PlayerCharacter.h>
#include <AI/AIProcess.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESAmmo.h>
#include <Forms/TESGlobal.h>
#include <Forms/ActorValueInfo.h>
#include <ExtraData/BGSObjectInstanceExtra.h>
#include <Components/BGSKeywordForm.h>
#include <ExtraData/ExtraDataList.h>
#include <Games/ActorExtension.h>
#include <Games/Memory.h>
#include <Games/TES.h>
#include <Audio/FileSound.h>
#include <Interface/GameHud.h>
#include <Interface/HudMarker.h>
#include <Interface/IMenu.h>
#include <Interface/Scaleform.h>
#include <Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <Projectiles/Projectile.h>
#include <DualWieldPose.h>
#include <NodeTools.h>
#include <OffHandModel.h>
#include <WeaponGrips.h>
#include <WorldRays.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <PerfProbe.h>

namespace
{
// What can go in the left hand (docs/DUAL_WIELD.md): pistols and the SMG by their keywords (the SMG is a rifle to the
// game, its animations say what it is), one-handed melee weapons, and the grappling gun (RD_GrapplingGun.esp 0x800)
constexpr uint32_t kWeaponTypePistol = 0x4A0A0;
constexpr uint32_t kAnimsSubmachineGun = 0x1A6AEF;
constexpr uint32_t kWeaponTypeMelee1H = 0x4A0A4;
// An empty right hand: no weapon, the game's own UnarmedDefault (0x1F4) or an unarmed weapon (WeaponTypeUnarmed, a
// boxing glove's punches: Fallout4.esm 1.10.163)
constexpr uint32_t kUnarmedDefault = 0x1F4;
constexpr uint32_t kWeaponTypeUnarmed = 0x5240E;
constexpr char kGrapplePlugin[] = "RD_GrapplingGun.esp";
constexpr uint32_t kGrappleGunId = 0x800;

// TESObjectWEAP (CommonLibF4, 1.10.163): its keywords (BGSKeywordForm) at 0x150, its own instance data (the weapon
// without mods) at 0x198. TESObjectWEAP::InstanceData: attack sound 0x10, 2D attack sound 0x18, dry fire sound 0x28,
// equip sound 0x38, ammo 0x68, keywords (BGSKeywordForm*) 0x80, ranged data 0x98, reload speed 0xC4, speed 0xC8,
// attack delay 0xD8, flags 0x110 (0x8000 automatic, read from the SMG's DNAM against the 10mm's), capacity (u16) 0x130.
// RangedData: override projectile 0x00, reload seconds 0x18, projectile count (i8) 0x2C.
constexpr uintptr_t kWeaponKeywordsOffset = 0x150;
constexpr uintptr_t kWeaponDataOffset = 0x198;
constexpr uintptr_t kAttackSoundOffset = 0x10;
constexpr uintptr_t kAttackSound2DOffset = 0x18;
constexpr uintptr_t kDryFireSoundOffset = 0x28;
constexpr uintptr_t kEquipSoundOffset = 0x38;
constexpr uintptr_t kAmmoOffset = 0x68;
constexpr uintptr_t kInstanceKeywordsOffset = 0x80;
constexpr uintptr_t kRangedDataOffset = 0x98;
constexpr uintptr_t kReloadSpeedOffset = 0xC4;
constexpr uintptr_t kSpeedOffset = 0xC8;
constexpr uintptr_t kAttackDelayOffset = 0xD8;
constexpr uintptr_t kFlagsOffset = 0x110;
constexpr uintptr_t kCapacityOffset = 0x130;
constexpr uint32_t kFlagAutomatic = 0x8000;
constexpr uintptr_t kOverrideProjectileOffset = 0x00;
constexpr uintptr_t kReloadSecondsOffset = 0x18;
constexpr uintptr_t kProjectileCountOffset = 0x2C;
// TESAmmo's projectile (AMMO_DATA at 0x160); BGSProjectile's muzzle flash duration (its data at 0xC0, +0x38) and model
// (TESModel at 0x150, its path a BSFixedString at +0x08)
constexpr uintptr_t kAmmoProjectileOffset = 0x160;
constexpr uintptr_t kFlashDurationOffset = 0xF8;
constexpr uintptr_t kFlashModelOffset = 0x158;
// ExtraInstanceData's TBO_InstanceData*
constexpr uintptr_t kExtraInstanceDataOffset = 0x20;

// PlayerCamera (CommonLibF4 id 1171980): the world's field of view at 0x168 (degrees)
constexpr uintptr_t kWorldFovOffset = 0x168;

// The left gun: no faster than this between shots (s); how long a shot's kick lasts
constexpr float kMinInterval = 0.06f;
constexpr float kKickSeconds = 0.14f;
// The left melee weapon: a swing this long at speed 1 (s), the game's melee hit at its middle (SwingHit)
constexpr float kSwingSeconds = 0.45f;
// The lowered reload: at least and at most this long (s); DualWieldPose takes the guns down and up in it
constexpr float kMinReloadSeconds = 0.9f;
constexpr float kMaxReloadSeconds = 3.5f;
// A reload tap: the reload key let go before the game takes it for holstering (s)
constexpr float kReloadTapSeconds = 0.4f;
// On the motorcycle (UpdateBike): how long the left hand stays up after its buttons are let go, how fast it comes up to
// the sights and how much the view closes in there (degrees), how far the arm has to be up before a shot, and where the
// hand is at the sights from the eyes (game units); the game's gun state for aiming down them (GUN_STATE)
constexpr float kBikeHoldSeconds = 2.5f;
constexpr float kBikeAdsRate = 10.f;
constexpr float kBikeAdsZoom = 20.f;
constexpr float kBikeAimReady = 0.6f;
constexpr float kBikeSightsAhead = 30.f;
constexpr float kBikeSightsBelow = 6.f;
constexpr uint32_t kGunStateSighted = 6;
// The arms ease into and out of their pose this fast (per s)
constexpr float kWeightRate = 7.f;
// ... and out of it this fast when a parkour move starts (its climb is 0.78 s: the hands go to the ledge at once)
constexpr float kMoveWeightRate = 25.f;
// The zoom with two guns: the field of view this much narrower all the way in, eased in and out this fast (per s)
constexpr float kZoomAmount = 0.18f;
constexpr float kZoomRate = 6.f;
// The left weapon checked against the inventory this often, the copies put on the skeletons this often, the state sent
// this often while there is one (s); another player's left hand forgotten when nothing came for this long
constexpr float kCheckInterval = 0.5f;
constexpr float kAttachInterval = 0.25f;
constexpr float kSendInterval = 5.f;
constexpr float kRemoteTimeout = 15.f;
// After a load, the save's left hand weapon is looked for in the inventory for this long (s)
constexpr float kRestoreTimeout = 5.f;
// Another player's arms: the reload as long as this (s)
constexpr float kRemoteReloadSeconds = 1.5f;

// The left melee weapon: the race's attacks a left swing is (BGSAttackData by its event, Fallout4.esm's HumanRace:
// x1, and x1.5 with the power attack flag)
constexpr char kLeftAttack[] = "meleeattackLeftStart";
constexpr char kLeftPowerAttack[] = "meleeattackPowerLeftStart";
// A power swing lasts this much longer than a plain one
constexpr float kPowerSwingStretch = 1.35f;
// A left melee weapon's clips: one blends into the next over this long (s)
constexpr float kClipBlendSeconds = 0.12f;
// An emptied left gun reloads by itself this long after its last shot (s), as the game's own guns do
constexpr float kAutoReloadAfter = 0.25f;
// A consumable used with both hands full: the weapons this long going down and coming back up (s), held down this long
// for one without an animation of its own, the pose let go (and taken back) over this long around the game's animation;
// the items that play one (Fallout4.esm: PlayAnimOnItemUse, a stimpak's and a Stealth Boy's) and the Stealth Boy's own
constexpr float kConsumeDown = 0.35f;
constexpr float kConsumeUp = 0.55f;
constexpr float kConsumeNoAnimation = 0.35f;
constexpr float kConsumeFade = 0.12f;
// third person, a consumable's animation whose length couldn't be read: the stimpak's (Common\UseStimpackOnSelf.hkx)
constexpr float kConsumeThirdPersonFallback = 2.f;
constexpr uint32_t kPlayAnimOnItemUse = 0x184C3D;
constexpr uint32_t kStealthBoyKeyword = 0x9AC96;
constexpr uint32_t kChemTypeStealthBoy = 0x842A1;
// The guard: the arm eases into and out of it this fast (per s)
constexpr float kGuardRate = 10.f;
// The Pip-Boy's mark: the list checked this often while the inventory is up (s), and the mark
constexpr float kMarkInterval = 0.1f;
constexpr char kLeftMark[] = " [L]";
// The left gun's projectiles: remembered this long for their muzzle flash (s)
constexpr float kProjectileMemory = 2.f;
// First person with two guns, the game's animation of the right arm carried onto both holds (DualWieldPose
// Arms::RightFollow, LeftFollow): eased in this fast, and out this fast for the reload's dip or a consumable's lowering
// (the right gun's reload clip plays under them); the left gun's held where it is this long after each shot of the right
// one (the right gun's fire clip kicks the right hand alone)
constexpr float kFollowInRate = 6.f;
constexpr float kFollowOutRate = 16.f;
constexpr float kRightShotHold = 0.35f;
// The game's weapon states (ActorState::GetWeaponState, CommonLibF4 WEAPON_STATE)
constexpr uint32_t kSheathed = 0;
constexpr uint32_t kWantToDraw = 1;
constexpr uint32_t kDrawing = 2;
constexpr uint32_t kDrawn = 3;
// The left weapon's draw and holster where no first person clip plays it (third person, other players): the reload's
// dip coming up over this long, and going down over this long (s)
constexpr float kDrawSeconds = 0.6f;
constexpr float kHolsterSeconds = 0.5f;
// A left gun's own idle (first person, the right hand holding no gun to follow): one of its fidgets after this many of
// the idle's loops at the least and the most (the game's graph, GunBehavior.hkx, counts the idle's countdown ticks to a
// random 1 to 9 the same way, then picks a fidget at random); one of its clips blends into the next over this long (s)
constexpr uint32_t kFidgetLoopsMin = 2;
constexpr uint32_t kFidgetLoopsMax = 5;
constexpr float kOwnBlendSeconds = 0.2f;
// Third person, a left melee weapon's own clip (its attack or block, the game's) over the mirrored right arm: eased in
// this fast (per s, all but at once), and back out to the mirror this fast after
constexpr float kLimbInRate = 30.f;
constexpr float kLimbOutRate = 8.f;
// The left weapon's own first person walk and run (a right-handed weapon's sway moving): over this speed it's moving,
// over this one running (units/s over the ground, smoothed this fast)
constexpr float kMoveSpeed = 40.f;
constexpr float kRunSpeed = 220.f;
constexpr float kSpeedRate = 6.f;
// ... and another player is sprinting at this speed (their sprint state may not come through on their copy)
constexpr float kSprintSpeed = 450.f;
// Third person, the left gun's arm aimed at the crosshair (DualWieldPose Arms::AimAt, LeftAim): the crosshair taken this
// far along the view; aimed for this long after a left shot (s), eased in and out this fast (per s)
constexpr float kAimDistance = 3000.f;
constexpr float kAimHoldSeconds = 2.5f;
constexpr float kAimRate = 8.f;
// Third person moving, the game's own walk, jog, run and sprint have the arms (DualWieldService::GameMovesArms): not
// while a weapon is in use (the right one for this long after the left mouse's press goes to the game: a right melee
// weapon's swing; the guns for kAimHoldSeconds after their shots); the right gun asked into its relaxed state again
// after this long when the game doesn't take it (s: the game's own fGunDownRetryTime)
constexpr float kRightAttackHold = 1.f;
constexpr float kRelaxRetrySeconds = 1.f;
// The game's ready, alert and relaxed state of a drawn gun (WeaponBehavior.hkx GunPlayBehavior's sync variable: its
// states' ids) and the gun state asked for to relax it (1057231 Actor::PerformAction of Action Gun Relaxed, default
// object 0x91, in 777817)
constexpr char kGunPlayState[] = "iSyncReadyAlertRelaxed";
constexpr uint32_t kGunPlayRelaxed = 2;
constexpr int32_t kGunStateRelaxed = 1;

// --- the left weapon in the game's own weapon code (docs/DUAL_WIELD.md "The left weapon in the game's systems")

// Actor: its race at 0x418 (its process at 0x300: AIProcess' middle high process at 0x08, high process at 0x10).
// MiddleHighProcessData: the equipped items' lock (a BSSpinLock) at 0x280, the equipped items (BSTArray<EquippedItem>:
// data 0x288, size 0x298). EquippedItem (0x28 bytes): the item {object, instance} at 0x00, its equip index at 0x18, its
// data (NiPointer<EquippedItemData>) at 0x20. HighProcessData: the attack under way (NiPointer<BGSAttackData>) at 0x320.
constexpr uintptr_t kRaceOffset = 0x418;
constexpr uintptr_t kHighProcessOffset = 0x10;
constexpr uintptr_t kEquippedLockOffset = 0x280;
constexpr uintptr_t kEquippedItemsOffset = 0x288;
constexpr uintptr_t kEquippedCountOffset = 0x298;
constexpr uintptr_t kEquippedItemSize = 0x28;
constexpr uintptr_t kEquippedInstanceOffset = 0x08;
constexpr uintptr_t kEquippedIndexOffset = 0x18;
constexpr uintptr_t kEquippedDataOffset = 0x20;
constexpr uintptr_t kAttackDataOffset = 0x320;
// EquippedWeaponData (0xA8 bytes, an NiRefObject: its count at 0x08): its ammo at 0x10, the magazine at 0x18, the aim
// model at 0x20, the muzzle flash at 0x28, the fire node at 0x30
constexpr size_t kWeaponDataSize = 0xA8;
constexpr uintptr_t kRefCountOffset = 0x08;
constexpr uintptr_t kDataAmmoOffset = 0x10;
constexpr uintptr_t kDataMagazineOffset = 0x18;
constexpr uintptr_t kDataAimModelOffset = 0x20;
constexpr uintptr_t kDataMuzzleFlashOffset = 0x28;
constexpr uintptr_t kDataFireNodeOffset = 0x30;
// TESRace's attacks (its BGSAttackDataForm at 0x98, the BGSAttackDataMap at 0xA0); the map's BSTHashMap<BSFixedString,
// NiPointer<BGSAttackData>> at 0x10: its capacity at +0x0C, its entries at +0x28 ({key, value, next}, 0x18 bytes each;
// next null: an empty slot)
constexpr uintptr_t kRaceAttacksOffset = 0xA0;
constexpr uintptr_t kAttackMapCapacityOffset = 0x10 + 0x0C;
constexpr uintptr_t kAttackMapEntriesOffset = 0x10 + 0x28;
constexpr uintptr_t kAttackMapEntrySize = 0x18;
constexpr uintptr_t kAttackMapNextOffset = 0x10;
// VATS (CommonLibF4 id 570121): the attacks being played (pointers: the list's size at 0x38, the one under way first),
// its mode at 0x40 (1: playing them), the attacker at 0x98, the actor its time scale is for at 0x90. The VATS menu: how
// many attacks are queued at 0x1228.
constexpr uintptr_t kVatsAttacksSizeOffset = 0x38;
constexpr uintptr_t kVatsModeOffset = 0x40;
constexpr uintptr_t kVatsTimeActorOffset = 0x90;
constexpr uintptr_t kVatsAttackerOffset = 0x98;
constexpr uintptr_t kVatsQueuedOffset = 0x1228;
constexpr uint32_t kVatsPlaying = 1;

DualWieldService* s_pService = nullptr;
// From the launch hook: the local player's own shots since the last update, and whether the launch under way is the
// left gun's (the thread firing it: the main thread, or the game's animation thread in VATS)
std::atomic<uint32_t> s_rightShots{0};
thread_local bool s_launchingLeft = false;

// BSSpinLock: locked by the game's own function (id 1425657), unlocked as the game does it (owning thread, then the
// count), as GameHud does with the compass markers
void SpinLock(uint8_t* apLock) noexcept
{
    using TLock = void(void*, const char*);
    POINTER_FALLOUT4(TLock, s_lock, 1425658);
    s_lock.Get()(apLock, nullptr);
}

void SpinUnlock(uint8_t* apLock) noexcept
{
    auto* pCount = reinterpret_cast<volatile long*>(apLock + 4);
    if (*pCount == 1)
    {
        *reinterpret_cast<volatile uint32_t*>(apLock) = 0;
        _InterlockedCompareExchange(pCount, 0, 1);
    }
    else
    {
        _InterlockedDecrement(pCount);
    }
}

// An NiRefObject's reference: taken, and let go as the game does (its deleting destructor, vtable slot 0, at none left)
void AddRef(uint8_t* apObject) noexcept
{
    _InterlockedIncrement(reinterpret_cast<volatile long*>(apObject + kRefCountOffset));
}

void DecRef(uint8_t* apObject) noexcept
{
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(apObject + kRefCountOffset)) != 0)
        return;
    using TDelete = void(void*, uint32_t);
    auto* pDelete = reinterpret_cast<TDelete*>((*reinterpret_cast<void***>(apObject))[0]);
    pDelete(apObject, 1);
}

// How long the attack button is held for a power attack: the game's fInitialPowerAttackDelay:Controls (its Setting at
// CommonLibF4 id 1229236, the value at +8)
float PowerAttackDelay() noexcept
{
    POINTER_FALLOUT4(uint8_t, s_setting, 1229237);
    const float delay = *reinterpret_cast<const float*>(s_setting.Get() + 8);
    return std::isfinite(delay) && delay > 0.05f ? std::min(delay, 2.f) : 0.3f;
}

uint8_t* GetVats() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_vats, 570122);
    return *s_vats.Get();
}

// The game's frame time (what it ticks aim models and muzzle flashes by)
float GameFrameTime() noexcept
{
    POINTER_FALLOUT4(float, s_frameTime, 112551);
    return *s_frameTime.Get();
}

// The actor's main hand weapon's EquippedWeaponData (a reference taken: DecRef it), null without one
uint8_t* MainWeaponData(Actor* apActor) noexcept
{
    AIProcess* pProcess = apActor ? apActor->currentProcess : nullptr;
    auto* pMiddle = pProcess ? reinterpret_cast<uint8_t*>(pProcess->middleProcess) : nullptr;
    if (!pMiddle)
        return nullptr;
    uint8_t* pLock = pMiddle + kEquippedLockOffset;
    SpinLock(pLock);
    uint8_t* pData = nullptr;
    auto* pItems = *reinterpret_cast<uint8_t**>(pMiddle + kEquippedItemsOffset);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pMiddle + kEquippedCountOffset);
    for (uint32_t i = 0; pItems && i < count && !pData; ++i)
    {
        uint8_t* pEntry = pItems + i * kEquippedItemSize;
        const auto* pObject = *reinterpret_cast<const TESForm* const*>(pEntry);
        if (pObject && pObject->formType == FormType::Weapon && *reinterpret_cast<const uint32_t*>(pEntry + kEquippedIndexOffset) == 0)
            pData = *reinterpret_cast<uint8_t**>(pEntry + kEquippedDataOffset);
    }
    if (pData)
        AddRef(pData);
    SpinUnlock(pLock);
    return pData;
}

// The left weapon as the actor's main hand weapon for the scope: its equipped item at equip index 0 holds the left
// weapon, its instance and its EquippedWeaponData, so what the game's weapon code asks the actor for (the weapon at the
// index, its ammo, magazine, fire node, aim model, muzzle flash, sounds) is the left one's. The equipped items' lock is
// held throughout (the game's other threads wait), and everything is put back as it was.
class EquipSwap
{
public:
    EquipSwap(Actor* apActor, TESForm* apWeapon, void* apInstance, uint8_t* apData) noexcept
    {
        AIProcess* pProcess = apActor ? apActor->currentProcess : nullptr;
        auto* pMiddle = pProcess ? reinterpret_cast<uint8_t*>(pProcess->middleProcess) : nullptr;
        if (!pMiddle || !apWeapon || !apData)
            return;
        m_pLock = pMiddle + kEquippedLockOffset;
        SpinLock(m_pLock);
        auto* pItems = *reinterpret_cast<uint8_t**>(pMiddle + kEquippedItemsOffset);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(pMiddle + kEquippedCountOffset);
        for (uint32_t i = 0; pItems && i < count && !m_pEntry; ++i)
        {
            uint8_t* pEntry = pItems + i * kEquippedItemSize;
            const auto* pObject = *reinterpret_cast<const TESForm* const*>(pEntry);
            if (pObject && pObject->formType == FormType::Weapon && *reinterpret_cast<const uint32_t*>(pEntry + kEquippedIndexOffset) == 0)
                m_pEntry = pEntry;
        }
        if (!m_pEntry)
        {
            SpinUnlock(m_pLock);
            m_pLock = nullptr;
            return;
        }
        m_pObject = std::exchange(*reinterpret_cast<TESForm**>(m_pEntry), apWeapon);
        m_pInstance = std::exchange(*reinterpret_cast<void**>(m_pEntry + kEquippedInstanceOffset), apInstance);
        m_pData = std::exchange(*reinterpret_cast<uint8_t**>(m_pEntry + kEquippedDataOffset), apData);
    }

    ~EquipSwap() noexcept
    {
        if (m_pEntry)
        {
            *reinterpret_cast<TESForm**>(m_pEntry) = m_pObject;
            *reinterpret_cast<void**>(m_pEntry + kEquippedInstanceOffset) = m_pInstance;
            *reinterpret_cast<uint8_t**>(m_pEntry + kEquippedDataOffset) = m_pData;
        }
        if (m_pLock)
            SpinUnlock(m_pLock);
    }

    TP_NOCOPYMOVE(EquipSwap);

    explicit operator bool() const noexcept { return m_pEntry != nullptr; }

private:
    uint8_t* m_pLock{nullptr};
    uint8_t* m_pEntry{nullptr};
    TESForm* m_pObject{nullptr};
    void* m_pInstance{nullptr};
    uint8_t* m_pData{nullptr};
};

// The actor's race's attack of that event (null: its race has none)
uint8_t* RaceAttack(const Actor* apActor, const BSFixedString& acEvent) noexcept
{
    const auto* pRace = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<const uint8_t*>(apActor) + kRaceOffset);
    const auto* pMap = pRace ? *reinterpret_cast<const uint8_t* const*>(pRace + kRaceAttacksOffset) : nullptr;
    if (!pMap || !acEvent.data)
        return nullptr;
    const uint32_t capacity = *reinterpret_cast<const uint32_t*>(pMap + kAttackMapCapacityOffset);
    const auto* pEntries = *reinterpret_cast<const uint8_t* const*>(pMap + kAttackMapEntriesOffset);
    for (uint32_t i = 0; pEntries && i < capacity; ++i)
    {
        const uint8_t* pEntry = pEntries + i * kAttackMapEntrySize;
        if (!*reinterpret_cast<const void* const*>(pEntry + kAttackMapNextOffset))
            continue;
        if (*reinterpret_cast<const void* const*>(pEntry) == acEvent.data)
            return *reinterpret_cast<uint8_t* const*>(pEntry + 8);
    }
    return nullptr;
}

// The game's own check before an attack starts (CommonLibF4 id 56113, run by the attack action's checks, 661649), for
// a weapon and the race's attack (BGSAttackData: its flags at 0x28, 0x04 a power attack): a power attack waits out the
// actor's power attack cooldown (Actor vfunc 0x10F, IsPowerAttackCoolingDown; the hit starts it, 0x10E), and an attack
// whose action point cost (1294986: the actor's ActorValueOwner at 0x58, the attack, the weapon) is more than the
// actor has of that cost's actor value (360869) doesn't start; the player's HUD flashes that meter (244187)
constexpr uintptr_t kActorValueOwnerOffset = 0x58;
constexpr uintptr_t kAttackFlagsOffset = 0x28;
constexpr uint32_t kAttackPowerFlag = 0x04;
constexpr uintptr_t kIsPowerAttackCoolingDownSlot = 0x10F;

bool CanAttack(Actor* apActor, uint8_t* apAttack, TESForm* apWeapon, void* apInstance) noexcept
{
    if (!apActor || !apAttack)
        return true;
    using TIsCoolingDown = bool(Actor*);
    auto* pIsCoolingDown = reinterpret_cast<TIsCoolingDown*>((*reinterpret_cast<void***>(apActor))[kIsPowerAttackCoolingDownSlot]);
    using TCostValue = ActorValueInfo*(Actor*);
    POINTER_FALLOUT4(TCostValue, s_costValue, 360870);
    using TFlashMeter = void(ActorValueInfo*);
    POINTER_FALLOUT4(TFlashMeter, s_flashMeter, 244188);
    ActorValueInfo* pValue = s_costValue.Get()(apActor);
    const auto refuse = [&]()
    {
        if (apActor == PlayerCharacter::Get() && pValue)
            s_flashMeter.Get()(pValue);
        return false;
    };

    const uint32_t flags = *reinterpret_cast<const uint32_t*>(apAttack + kAttackFlagsOffset);
    if ((flags & kAttackPowerFlag) && pIsCoolingDown(apActor))
        return refuse();

    struct WeaponInstance
    {
        TESForm* pObject;
        void* pInstance;
    };
    using TCost = float(void*, uint8_t*, const WeaponInstance*);
    POINTER_FALLOUT4(TCost, s_cost, 1294987);
    const WeaponInstance weapon{apWeapon, apInstance};
    auto* pOwner = reinterpret_cast<uint8_t*>(apActor) + kActorValueOwnerOffset;
    const float cost = s_cost.Get()(pOwner, apAttack, &weapon);
    if (!(cost > 0.f) || !pValue)
        return true;
    // ActorValueOwner::GetActorValue (vtable slot 1)
    using TGetValue = float(void*, ActorValueInfo*);
    auto* pGetValue = reinterpret_cast<TGetValue*>((*reinterpret_cast<void***>(pOwner))[1]);
    const float current = pGetValue(pOwner, pValue);
    if (current <= 0.f || current < cost)
        return refuse();
    return true;
}

// The left swing's hit under way (main thread, set only around the game's melee hit): the weapon the game is given as
// the player's
struct LeftSwing
{
    const Actor* pActor{nullptr};
    TESForm* pWeapon{nullptr};
    void* pInstance{nullptr};
};
LeftSwing s_leftSwing{};
// the main hand's equip index (BGSEquipIndex 0), the one the game's melee hit and its weapon lookup are asked for
constexpr uint32_t kMainEquipIndex = 0;

// The weapon an actor holds at an equip index, as the game asks for it (CommonLibF4 id 1074514: the equipped item at
// that index if it's a weapon, constructed into apOut with a reference to its instance): the left weapon for the
// player while the left swing's hit runs
struct ObjectInstance
{
    TESForm* pObject;
    void* pInstance;
};
using TGetWeaponInstance = ObjectInstance*(Actor*, ObjectInstance*, uint32_t);
TGetWeaponInstance* RealGetWeaponInstance = nullptr;

ObjectInstance* HookGetWeaponInstance(Actor* apActor, ObjectInstance* apOut, uint32_t aIndex)
{
    if (s_leftSwing.pActor && apActor == s_leftSwing.pActor && aIndex == kMainEquipIndex)
    {
        // BGSObjectInstance's constructor (CommonLibF4 id 1095748), as the game's lookup builds its answer: the form
        // and its instance, the instance's reference taken (the caller lets it go)
        using TConstruct = void(ObjectInstance*, TESForm*, void*);
        POINTER_FALLOUT4(TConstruct, s_construct, 1095749);
        s_construct.Get()(apOut, s_leftSwing.pWeapon, s_leftSwing.pInstance);
        return apOut;
    }
    return RealGetWeaponInstance(apActor, apOut, aIndex);
}

TiltedPhoques::Initializer s_dualWieldHooks(
    []()
    {
        POINTER_FALLOUT4(TGetWeaponInstance, s_getWeaponInstance, 1074515);
        RealGetWeaponInstance = s_getWeaponInstance.Get();
        TP_HOOK(&RealGetWeaponInstance, HookGetWeaponInstance);
    });

const BSFixedString& UserEvent(const char* acpName) noexcept
{
    static BSFixedString s_primary("PrimaryAttack");
    static BSFixedString s_secondary("SecondaryAttack");
    return acpName[0] == 'P' ? s_primary : s_secondary;
}

// ButtonEvent (CommonLibF4): the user event at 0x28, the value at 0x38, how long it's been down at 0x3C; InputEvent's next
// at 0x18
const BSFixedString::Data* UserEventOf(const void* apEvent) noexcept
{
    return *reinterpret_cast<BSFixedString::Data* const*>(static_cast<const uint8_t*>(apEvent) + 0x28);
}
float ValueOf(const void* apEvent) noexcept
{
    return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(apEvent) + 0x38);
}
float HeldOf(const void* apEvent) noexcept
{
    return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(apEvent) + 0x3C);
}

// InputEvent (CommonLibF4): its device at 0x08 (1 the mouse), its type at 0x10 (0 a button), the next at 0x18, how it's
// been handled at 0x24 (2 kStop: the controls' handlers after that skip it, PlayerControls' and MenuControls' alike);
// IDEvent's idCode at 0x30 (a mouse button's: 0 the left, 1 the right)
constexpr uintptr_t kEventDeviceOffset = 0x08;
constexpr uintptr_t kEventTypeOffset = 0x10;
constexpr uintptr_t kEventNextOffset = 0x18;
constexpr uintptr_t kEventHandledOffset = 0x24;
constexpr uintptr_t kEventIdCodeOffset = 0x30;
constexpr int32_t kDeviceMouse = 1;
constexpr int32_t kEventButton = 0;
constexpr int32_t kHandledStop = 2;

uint8_t* NextEvent(uint8_t* apEvent) noexcept
{
    return *reinterpret_cast<uint8_t**>(apEvent + kEventNextOffset);
}
bool IsRightMouseButton(const uint8_t* apEvent) noexcept
{
    return *reinterpret_cast<const int32_t*>(apEvent + kEventDeviceOffset) == kDeviceMouse &&
           *reinterpret_cast<const int32_t*>(apEvent + kEventTypeOffset) == kEventButton && *reinterpret_cast<const int32_t*>(apEvent + kEventIdCodeOffset) == 1;
}

bool GameHasKeyboard() noexcept
{
    HWND window = GetForegroundWindow();
    DWORD processId = 0;
    if (window)
        GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

void ShowHudMessage(const std::string& acText) noexcept
{
    // SendHUDMessage::ShowHUDMessage (CommonLibF4 id 1163005): message, sound, throttle, warning
    using TShowHUDMessage = void(const char*, const char*, bool, bool);
    POINTER_FALLOUT4(TShowHUDMessage, s_showHUDMessage, 1163006);
    s_showHUDMessage.Get()(acText.c_str(), "", false, false);
}

uint32_t GrappleGunId() noexcept
{
    static const uint32_t s_id = []
    {
        const Mod* pMod = ModManager::Get()->GetByName(kGrapplePlugin);
        return pMod && pMod->IsLoaded() ? pMod->GetFormId(kGrappleGunId) : 0u;
    }();
    return s_id;
}

template <class T> T Read(const void* apBase, uintptr_t aOffset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(apBase) + aOffset);
}

float Smooth(float aT) noexcept
{
    const float t = std::clamp(aT, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// A stack of an item in a reference's inventory, with its index among the item's stacks
struct StackRef
{
    TESBoundObject* pObject{nullptr};
    BGSInventoryItem::Stack* pStack{nullptr};
    uint32_t Index{0};
};

// The object mods on a stack as the game has them (its BGSObjectInstanceExtra, the disabled ones left out): game form
// ids, which need no server (the inventory sync's lists are the server's, empty offline)
Vector<OffHandMod> StackMods(const BGSInventoryItem::Stack* apStack) noexcept
{
    Vector<OffHandMod> mods;
    const ExtraDataList* pExtra = apStack ? apStack->spExtra : nullptr;
    const auto* pInstance = pExtra ? static_cast<const BGSObjectInstanceExtra*>(pExtra->GetByType(ExtraDataType::ObjectInstance)) : nullptr;
    if (!pInstance)
        return mods;
    for (const auto& data : pInstance->GetIndexData())
    {
        if (!data.disabled)
            mods.push_back({data.objectID, data.index, data.rank});
    }
    return mods;
}

// The same mods in any order (the game can rebuild an instance's list in another)
bool SameMods(const Vector<OffHandMod>& acLhs, const Vector<OffHandMod>& acRhs) noexcept
{
    if (acLhs.size() != acRhs.size())
        return false;
    Vector<uint32_t> lhs;
    Vector<uint32_t> rhs;
    for (const auto& mod : acLhs)
        lhs.push_back(mod.FormId);
    for (const auto& mod : acRhs)
        rhs.push_back(mod.FormId);
    std::sort(lhs.begin(), lhs.end());
    std::sort(rhs.begin(), rhs.end());
    return lhs == rhs;
}

// The mods as the network has them (the server's ids) and back
Vector<Inventory::ObjectMod> ToWire(const Vector<OffHandMod>& acMods) noexcept
{
    auto& modSystem = World::Get().GetModSystem();
    Vector<Inventory::ObjectMod> wire;
    for (const auto& mod : acMods)
    {
        Inventory::ObjectMod out{};
        if (!modSystem.GetServerModId(mod.FormId, out.ModId))
            continue;
        out.AttachIndex = mod.AttachIndex;
        out.Rank = mod.Rank;
        wire.push_back(out);
    }
    return wire;
}

Vector<OffHandMod> FromWire(const Vector<Inventory::ObjectMod>& acWire) noexcept
{
    auto& modSystem = World::Get().GetModSystem();
    Vector<OffHandMod> mods;
    for (const auto& mod : acWire)
    {
        if (const uint32_t formId = modSystem.GetGameId(mod.ModId))
            mods.push_back({formId, mod.AttachIndex, mod.Rank});
    }
    return mods;
}

std::optional<StackRef> FindStack(TESObjectREFR* apOwner, uint32_t aFormId, const Vector<OffHandMod>* apMods, bool aWorn) noexcept
{
    if (!apOwner || !apOwner->pInventoryList)
        return std::nullopt;
    for (auto& item : apOwner->pInventoryList->DataA)
    {
        if (!item.pObject || item.pObject->formID != aFormId)
            continue;
        uint32_t index = 0;
        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack, ++index)
        {
            if (aWorn && !pStack->IsEquipped())
                continue;
            if (apMods && !SameMods(StackMods(pStack), *apMods))
                continue;
            return StackRef{item.pObject, pStack, index};
        }
    }
    return std::nullopt;
}

// --- the left hand kept in the save (RD_DualWield.esp, Tools/BuildDualWieldPlugin.js): three float globals, each a
// whole number under 2^24 (a float holds those exactly), plugin-relative so a changed load order finds the same weapon

constexpr char kSavePlugin[] = "RD_DualWield.esp";
constexpr uint32_t kGlobalPlugin = 0x800;
constexpr uint32_t kGlobalForm = 0x801;
constexpr uint32_t kGlobalMods = 0x802;
constexpr uint32_t kKeyMask = 0xFFFFFF;

TESGlobal* SaveGlobal(uint32_t aLocalId) noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(kSavePlugin);
    return pMod && pMod->IsLoaded() ? Cast<TESGlobal>(TESForm::GetById(pMod->GetFormId(aLocalId))) : nullptr;
}

// FNV-1a, folded to 24 bits
uint32_t Fnv(const uint8_t* apData, size_t aSize, uint32_t aHash = 2166136261u) noexcept
{
    for (size_t i = 0; i < aSize; ++i)
        aHash = (aHash ^ apData[i]) * 16777619u;
    return aHash;
}
uint32_t Fold(uint32_t aHash) noexcept
{
    return ((aHash >> 24) ^ aHash) & kKeyMask;
}

// A plugin by its lowercase file name: never 0 (0 is an empty hand)
uint32_t PluginKey(const Mod& acMod) noexcept
{
    std::string name = acMod.filename;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const uint32_t key = Fold(Fnv(reinterpret_cast<const uint8_t*>(name.data()), name.size()));
    return key ? key : 1u;
}

// The loaded plugin a form id comes from (a light plugin's: FE, its index, a 12-bit id)
const Mod* PluginOf(uint32_t aFormId) noexcept
{
    const bool light = (aFormId >> 24) == 0xFE;
    for (auto* pEntry = &ModManager::Get()->mods.entry; pEntry && pEntry->data; pEntry = pEntry->next)
    {
        const Mod* pMod = pEntry->data;
        if (!pMod->IsLoaded() || pMod->IsLite() != light)
            continue;
        if (light ? pMod->liteId == ((aFormId >> 12) & 0xFFF) : pMod->standardId == (aFormId >> 24))
            return pMod;
    }
    return nullptr;
}

uint32_t LocalId(uint32_t aFormId, const Mod& acMod) noexcept
{
    return acMod.IsLite() ? aFormId & 0xFFF : aFormId & kKeyMask;
}

// The mods, each by its plugin and id, in any order
uint32_t ModsKey(const Vector<OffHandMod>& acMods) noexcept
{
    Vector<uint32_t> keys;
    for (const auto& mod : acMods)
    {
        const Mod* pMod = PluginOf(mod.FormId);
        keys.push_back(pMod ? (PluginKey(*pMod) * 16777619u) ^ LocalId(mod.FormId, *pMod) : mod.FormId);
    }
    std::sort(keys.begin(), keys.end());
    return keys.empty() ? 0u : Fold(Fnv(reinterpret_cast<const uint8_t*>(keys.data()), keys.size() * sizeof(uint32_t)));
}

// (rounded in double: past 2^23 a float has no halves, so the float sum f + 0.5 rounds an odd value up to the next even
// one; seen 2026-10-25, Fallout4.esm's key E3C7BF read back as E3C7C0 and the save's knife not found)
uint32_t ReadGlobal(const TESGlobal* apGlobal) noexcept
{
    return apGlobal && std::isfinite(apGlobal->f) && apGlobal->f > 0.f ? static_cast<uint32_t>(std::llround(static_cast<double>(apGlobal->f))) & kKeyMask : 0u;
}

std::optional<StackRef> StackAt(TESObjectREFR* apOwner, TESBoundObject* apObject, uint32_t aIndex) noexcept
{
    if (!apOwner || !apOwner->pInventoryList || !apObject)
        return std::nullopt;
    for (auto& item : apOwner->pInventoryList->DataA)
    {
        if (item.pObject != apObject)
            continue;
        uint32_t index = 0;
        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack, ++index)
        {
            if (index == aIndex)
                return StackRef{item.pObject, pStack, index};
        }
    }
    return std::nullopt;
}

uint32_t CountOf(TESObjectREFR* apOwner, const TESForm* apItem) noexcept
{
    if (!apOwner || !apOwner->pInventoryList || !apItem)
        return 0;
    uint32_t count = 0;
    for (auto& item : apOwner->pInventoryList->DataA)
    {
        if (item.pObject != apItem)
            continue;
        for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack)
            count += pStack->uiCount;
    }
    return count;
}


// What the stack's mods make of the weapon (its ExtraInstanceData), null for a weapon as the form has it
const uint8_t* StackInstance(const BGSInventoryItem::Stack* apStack) noexcept
{
    const ExtraDataList* pExtra = apStack ? apStack->spExtra : nullptr;
    const BSExtraData* pData = pExtra ? pExtra->GetByType(ExtraDataType::InstanceData) : nullptr;
    return pData ? Read<const uint8_t*>(pData, kExtraInstanceDataOffset) : nullptr;
}

bool HasKeyword(const BGSKeywordForm* apKeywords, uint32_t aKeywordId) noexcept
{
    const TESForm* pKeyword = TESForm::GetById(aKeywordId);
    return apKeywords && pKeyword && apKeywords->HasKeyword(pKeyword);
}

// A weapon's keywords: its instance's (the mods change them: a pipe gun with a pistol grip is a pistol), else its own
const BGSKeywordForm* KeywordsOf(const TESForm* apWeapon, const uint8_t* apInstance) noexcept
{
    if (const BGSKeywordForm* pKeywords = apInstance ? Read<const BGSKeywordForm*>(apInstance, kInstanceKeywordsOffset) : nullptr)
        return pKeywords;
    return apWeapon ? reinterpret_cast<const BGSKeywordForm*>(reinterpret_cast<const uint8_t*>(apWeapon) + kWeaponKeywordsOffset) : nullptr;
}

// What a weapon is for dual wielding, from its instance's keywords (the mods can make a pipe gun a pistol) or its own
uint8_t KindOf(const TESForm* apWeapon, const uint8_t* apInstance) noexcept
{
    // 0 none, 1 gun, 2 melee, 3 grapple (DualWieldService::Kind)
    if (!apWeapon)
        return 0;
    if (const uint32_t grapple = GrappleGunId(); grapple && apWeapon->formID == grapple)
        return 3;
    if (apWeapon->formType != FormType::Weapon)
        return 0;
    const BGSKeywordForm* pKeywords = apInstance ? Read<const BGSKeywordForm*>(apInstance, kInstanceKeywordsOffset) : nullptr;
    if (!pKeywords)
        pKeywords = reinterpret_cast<const BGSKeywordForm*>(reinterpret_cast<const uint8_t*>(apWeapon) + kWeaponKeywordsOffset);
    if (HasKeyword(pKeywords, kWeaponTypePistol) || HasKeyword(pKeywords, kAnimsSubmachineGun))
        return 1;
    if (HasKeyword(pKeywords, kWeaponTypeMelee1H))
        return 2;
    return 0;
}

// Nothing in the hand but the fists: no weapon, the game's UnarmedDefault, or an unarmed weapon (its instance's keywords
// first, as KindOf reads them)
bool IsUnarmed(const TESForm* apWeapon, const uint8_t* apInstance) noexcept
{
    if (!apWeapon || apWeapon->formID == kUnarmedDefault)
        return true;
    if (apWeapon->formType != FormType::Weapon)
        return false;
    const BGSKeywordForm* pKeywords = apInstance ? Read<const BGSKeywordForm*>(apInstance, kInstanceKeywordsOffset) : nullptr;
    if (!pKeywords)
        pKeywords = reinterpret_cast<const BGSKeywordForm*>(reinterpret_cast<const uint8_t*>(apWeapon) + kWeaponKeywordsOffset);
    return HasKeyword(pKeywords, kWeaponTypeUnarmed);
}

// How long a gun's reload takes (its ranged data's reload time at its reload speed), 0 without ranged data
float ReloadSecondsOf(const uint8_t* apInstance) noexcept
{
    const uint8_t* pRanged = apInstance ? Read<const uint8_t*>(apInstance, kRangedDataOffset) : nullptr;
    if (!pRanged)
        return 0.f;
    const float reloadSpeed = std::max(Read<float>(apInstance, kReloadSpeedOffset), 0.1f);
    return std::clamp(Read<float>(pRanged, kReloadSecondsOffset) / reloadSpeed, kMinReloadSeconds, kMaxReloadSeconds);
}

// The projectile a weapon instance fires: its own override, else its ammo's
TESForm* ProjectileOf(const uint8_t* apInstance) noexcept
{
    const uint8_t* pRanged = Read<const uint8_t*>(apInstance, kRangedDataOffset);
    if (TESForm* pOverride = pRanged ? Read<TESForm*>(pRanged, kOverrideProjectileOffset) : nullptr)
        return pOverride;
    const TESForm* pAmmo = Read<const TESForm*>(apInstance, kAmmoOffset);
    return pAmmo ? Read<TESForm*>(pAmmo, kAmmoProjectileOffset) : nullptr;
}

void FlashOf(const TESForm* apProjectile, std::string& aModel, float& aSeconds) noexcept
{
    aModel.clear();
    aSeconds = 0.05f;
    if (!apProjectile)
        return;
    const auto* pPath = reinterpret_cast<const BSFixedString*>(reinterpret_cast<const uint8_t*>(apProjectile) + kFlashModelOffset);
    if (const char* pText = pPath->data ? pPath->AsAscii() : nullptr)
        aModel = pText;
    const float seconds = Read<float>(apProjectile, kFlashDurationOffset);
    if (std::isfinite(seconds) && seconds > 0.f)
        aSeconds = seconds;
}

// The view: where the camera looks, its right and up (world, unit), from its world to clip matrix; the player's own look
// without one
void GetView(const Actor* apActor, glm::vec3& aForward, glm::vec3& aRight, glm::vec3& aUp) noexcept
{
    float m[4][4];
    if (HudMarker::GetWorldToClip(m))
    {
        const glm::vec3 forward(m[3][0], m[3][1], m[3][2]);
        const glm::vec3 right(m[0][0], m[0][1], m[0][2]);
        if (glm::length(forward) > 1e-6f && glm::length(right) > 1e-6f)
        {
            aForward = glm::normalize(forward);
            aRight = glm::normalize(right);
            aUp = glm::normalize(glm::cross(aRight, aForward));
            return;
        }
    }
    const float pitch = apActor->rotation.x;
    const float yaw = apActor->rotation.z;
    aForward = glm::vec3(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
    aRight = glm::vec3(std::cos(yaw), -std::sin(yaw), 0.f);
    aUp = glm::normalize(glm::cross(aRight, aForward));
}

float* GetWorldFov() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    uint8_t* pCamera = *s_playerCamera.Get();
    return pCamera ? reinterpret_cast<float*>(pCamera + kWorldFovOffset) : nullptr;
}

bool IsPipboyOpen() noexcept
{
    static BSFixedString s_pipboy("PipboyMenu");
    const UI* pUI = UI::Get();
    return pUI && pUI->GetMenuOpen(s_pipboy);
}

// The Pip-Boy's inventory page and its list (List_mc: entryList, selectedIndex, UpdateList). PipboyMenu.swf: root.Menu_mc
// is the PipboyMenu, its CurrentPage getter the page shown, DataObj.CurrentPage its number (1 the inventory, read in game
// 2026-10-24 with PipboyTabs' tabs too).
bool GetInventoryList(GFx::Movie*& apMovie, GFx::Value& aList, bool aSayWhyNot = false) noexcept
{
    UI* pUI = UI::Get();
    IMenu* pMenu = pUI ? pUI->FindMenuByName("PipboyMenu") : nullptr;
    apMovie = pMenu ? static_cast<GFx::Movie*>(pMenu->pMovie) : nullptr;
    if (!apMovie)
        return false;
    GFx::Value number;
    GFx::Value page;
    if (apMovie->GetVariable(&number, "root.Menu_mc.DataObj.CurrentPage") && static_cast<int>(number.GetNumber()) == 1 &&
        apMovie->GetVariable(&page, "root.Menu_mc.CurrentPage") && page.IsObject() && page.GetMember("List_mc", &aList) && aList.IsObject())
        return true;
    if (aSayWhyNot)
        spdlog::info("Dual wield: the Pip-Boy's page {} isn't the inventory (page type {})", number.GetNumber(), static_cast<int>(page.GetType()));
    return false;
}

// The inventory list's entry at that index, as the game reads it to act on it (1195452: the Pip-Boy's equip): the
// entries are the Pip-Boy data's (DataObj.InvItems; the Scaleform copies carry no handle), PipboyDataManager's
// (CommonLibF4 id 1178971) inventory objects at +0x4A8 (count at +0x4B8), in the list's order. Each PipboyObject's
// members are found by name (1430389, a null for none): HandleID a PipboyPrimitiveValue (its value at +0x18,
// BGSInventoryInterface's handle of the item), StackID a PipboyArray (its elements at +0x18, count at +0x28; the
// first element's value at +0x18, the stack the Pip-Boy acts on).
bool ReadItem(int32_t aIndex, uint32_t& aHandle, uint32_t& aStack) noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_dataManager, 1178972);
    using TFindMember = const uint8_t*(const uint8_t*, const BSFixedString*);
    POINTER_FALLOUT4(TFindMember, s_findMember, 1430390);
    static BSFixedString s_handleName("HandleID");
    static BSFixedString s_stackName("StackID");

    const uint8_t* pManager = *s_dataManager.Get();
    if (!pManager || aIndex < 0)
        return false;
    const auto* pItems = *reinterpret_cast<const uint8_t* const* const*>(pManager + 0x4A8);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pManager + 0x4B8);
    if (!pItems || static_cast<uint32_t>(aIndex) >= count || !pItems[aIndex])
        return false;
    const uint8_t* pHandle = s_findMember.Get()(pItems[aIndex], &s_handleName);
    const uint8_t* pStacks = s_findMember.Get()(pItems[aIndex], &s_stackName);
    if (!pHandle || !pStacks)
        return false;
    const auto* pElements = *reinterpret_cast<const uint8_t* const* const*>(pStacks + 0x18);
    const uint32_t stackCount = *reinterpret_cast<const uint32_t*>(pStacks + 0x28);
    if (!pElements || stackCount == 0 || !pElements[0])
        return false;
    aHandle = *reinterpret_cast<const uint32_t*>(pHandle + 0x18);
    aStack = *reinterpret_cast<const uint32_t*>(pElements[0] + 0x18);
    return true;
}

// BGSInventoryInterface (CommonLibF4 id 501899) and its RequestInventoryItem (1200959): the handle's item
const BGSInventoryItem* InventoryItem(uint32_t aHandle) noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_inventoryInterface, 501900);
    TP_THIS_FUNCTION(TRequestInventoryItem, const BGSInventoryItem*, const uint8_t, const uint32_t& acHandle);
    POINTER_FALLOUT4(TRequestInventoryItem, s_requestInventoryItem, 1200960);
    const uint8_t* pInterface = *s_inventoryInterface.Get();
    return pInterface ? TiltedPhoques::ThisCall(s_requestInventoryItem.Get(), pInterface, aHandle) : nullptr;
}

int ParseKey(const std::wstring& acText, int aDefault, std::string& aName) noexcept
{
    std::wstring value = acText;
    value.erase(std::remove_if(value.begin(), value.end(), iswspace), value.end());
    std::transform(value.begin(), value.end(), value.begin(), towupper);
    int key = 0;
    if (value.size() == 1 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'0' && value[0] <= L'9')))
        key = value[0], aName = std::string(1, static_cast<char>(value[0]));
    else if (value.size() >= 2 && value[0] == L'F' && iswdigit(value[1]))
    {
        const int number = std::stoi(value.substr(1));
        if (number >= 1 && number <= 12)
            key = VK_F1 + number - 1, aName = "F" + std::to_string(number);
    }
    else if (value.rfind(L"0X", 0) == 0)
    {
        key = static_cast<int>(std::wcstoul(value.c_str() + 2, nullptr, 16));
        aName = fmt::format("key 0x{:X}", key);
    }
    if (key <= 0 || key > 0xFE)
    {
        key = aDefault;
        aName = std::string(1, static_cast<char>(aDefault));
    }
    return key;
}
} // namespace

// The game's functions the left gun goes through, hooked (docs/DUAL_WIELD.md "The left weapon in the game's systems")
struct DualWieldHooks
{
    // A projectile's muzzle flash (CommonLibF4 id 962811: shooter, projectile): the flash of the shooter's weapon at the
    // projectile's equip index set off (made from the projectile, on its fire node, the first time). The left gun's
    // projectiles set off the left gun's.
    using TShowMuzzleFlash = void(TESObjectREFR*, TESObjectREFR*);
    static inline TShowMuzzleFlash* RealShowMuzzleFlash = nullptr;
    static void ShowMuzzleFlash(TESObjectREFR* apShooter, TESObjectREFR* apProjectile)
    {
        DualWieldService* pService = s_pService;
        auto* pShooter = apShooter ? Cast<Actor>(apShooter) : nullptr;
        const uint32_t shooterId = pService && pShooter && apProjectile ? pService->LeftShooterOf(apProjectile->GetHandle().handle.iBits) : 0u;
        if (!shooterId || shooterId != pShooter->formID)
        {
            RealShowMuzzleFlash(apShooter, apProjectile);
            return;
        }
        // that left gun gone since: no flash (the right gun's isn't this shot's)
        const auto data = pService->AcquireLeftData(shooterId);
        if (!data.pData)
            return;
        {
            EquipSwap swap(pShooter, data.pWeapon, data.pInstance, data.pData);
            if (swap)
                RealShowMuzzleFlash(apShooter, apProjectile);
        }
        DecRef(data.pData);
    }

    // The weaponFire animation event's handler (1240981: this, actor, event): the weapon at the event's equip index
    // fires (TESObjectWEAP::Fire). In VATS with two guns, every other attack the left gun's.
    using TWeaponFire = uint64_t(void*, Actor*, void*, void*);
    static inline TWeaponFire* RealWeaponFire = nullptr;
    static uint64_t WeaponFire(void* apThis, Actor* apActor, void* apEvent, void* apExtra)
    {
        DualWieldService* pService = s_pService;
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (!pService || !pPlayer || apActor != pPlayer)
            return RealWeaponFire(apThis, apActor, apEvent, apExtra);
        if (!pService->FiresLeftInVats(pPlayer))
            return FireRight(pPlayer, apThis, apActor, apEvent, apExtra);
        const auto ref = pService->AcquireLeftData(pPlayer->formID);
        if (!ref.pData)
            return RealWeaponFire(apThis, apActor, apEvent, apExtra);

        // as FireLeft: the magazine one over while the game's ammo use runs
        auto* pMagazine = reinterpret_cast<uint32_t*>(ref.pData + kDataMagazineOffset);
        *pMagazine = pService->m_vatsMagazine.load() + 1;
        uint64_t result = 0;
        {
            EquipSwap swap(pPlayer, ref.pWeapon, ref.pInstance, ref.pData);
            s_launchingLeft = static_cast<bool>(swap);
            result = RealWeaponFire(apThis, apActor, apEvent, apExtra);
            s_launchingLeft = false;
        }
        const uint32_t left = *pMagazine > 0 ? *pMagazine - 1 : 0;
        *pMagazine = left;
        pService->m_vatsMagazine = left;
        pService->m_vatsLeftShots.fetch_add(1);
        DecRef(ref.pData);
        return result;
    }

    // The right hand's shot. The grappling gun reloads itself without a reload: its magazine is one
    // over while the game fires it (the game's ammo use asks for the reload when a magazine empties), then holds a spike
    // while the inventory has one (KeepGrappleLoaded tops it up later too)
    static uint64_t FireRight(PlayerCharacter* apPlayer, void* apThis, Actor* apActor, void* apEvent, void* apExtra)
    {
        const TESForm* pRight = apPlayer->GetEquippedWeapon(0);
        uint8_t* pData = pRight && GrappleGunId() && pRight->formID == GrappleGunId() ? MainWeaponData(apPlayer) : nullptr;
        if (!pData)
            return RealWeaponFire(apThis, apActor, apEvent, apExtra);
        auto* pMagazine = reinterpret_cast<uint32_t*>(pData + kDataMagazineOffset);
        *pMagazine = std::max(*pMagazine, 1u) + 1;
        const uint64_t result = RealWeaponFire(apThis, apActor, apEvent, apExtra);
        const auto* pAmmo = *reinterpret_cast<const TESForm* const*>(pData + kDataAmmoOffset);
        *pMagazine = pAmmo && CountOf(apPlayer, pAmmo) > 0 ? 1u : 0u;
        DecRef(pData);
        return result;
    }

    // The VATS menu queueing an attack (32662: the menu): with two guns, every other one is the left gun's (its action
    // points, its ammo, its hit chance)
    using TQueueVats = uint64_t(void*, void*, void*, void*);
    static inline TQueueVats* RealQueueVats = nullptr;
    static uint64_t QueueVats(void* apThis, void* apSecond, void* apThird, void* apFourth)
    {
        DualWieldService* pService = s_pService;
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (!pService || !pPlayer || !apThis || !pService->QueuesLeft(*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(apThis) + kVatsQueuedOffset)))
            return RealQueueVats(apThis, apSecond, apThird, apFourth);
        const auto ref = pService->AcquireLeftData(pPlayer->formID);
        if (!ref.pData)
            return RealQueueVats(apThis, apSecond, apThird, apFourth);
        uint64_t result = 0;
        {
            EquipSwap swap(pPlayer, ref.pWeapon, ref.pInstance, ref.pData);
            result = RealQueueVats(apThis, apSecond, apThird, apFourth);
        }
        DecRef(ref.pData);
        return result;
    }

    // The VATS menu drawn (VATSMenu::PreDisplay, vtable slot 5, CommonLibF4 id 1227068): every so often
    // (fVATSPartChanceUpdateSec) it works out each target part's hit chance with the game's formula (984570), for the
    // weapon the player holds at VATS' equip index (1277201). With two guns, while the next attack queued would be the
    // left gun's (QueuesLeft), the chances shown are the left gun's, as that attack's will be.
    using TVatsPreDisplay = void(void*);
    static inline TVatsPreDisplay* RealVatsPreDisplay = nullptr;
    static inline thread_local uint8_t* s_pDrawnVatsMenu = nullptr;
    static void VatsPreDisplay(void* apThis)
    {
        s_pDrawnVatsMenu = static_cast<uint8_t*>(apThis);
        RealVatsPreDisplay(apThis);
        s_pDrawnVatsMenu = nullptr;
    }
    // the formula: the target, the weapon, the part, a distance term; the chance (percent) back
    using THitChance = float(void*, const ObjectInstance*, void*, float);
    static inline THitChance* RealHitChance = nullptr;
    static float HitChance(void* apTarget, const ObjectInstance* apWeapon, void* apPart, float aDistance)
    {
        DualWieldService* pService = s_pService;
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        const uint8_t* pMenu = s_pDrawnVatsMenu;
        if (!pService || !pPlayer || !pMenu || !pService->QueuesLeft(*reinterpret_cast<const uint32_t*>(pMenu + kVatsQueuedOffset)))
            return RealHitChance(apTarget, apWeapon, apPart, aDistance);
        const auto ref = pService->AcquireLeftData(pPlayer->formID);
        if (!ref.pData)
            return RealHitChance(apTarget, apWeapon, apPart, aDistance);
        const ObjectInstance left{ref.pWeapon, ref.pInstance};
        const float chance = RealHitChance(apTarget, &left, apPart, aDistance);
        DecRef(ref.pData);
        return chance;
    }

    // The controls' input (BSInputEventReceiver::PerformInputProcessing: this, the queue's first event): MenuControls'
    // (CommonLibF4 id 1280156) and PlayerControls' (663896) give each event to their handlers in turn, and none takes
    // one that's been handled (kStop). In the Pip-Boy's inventory the right mouse button is the left hand's (left click
    // stays the game's: it selects): its events are handled before either sees them (the game would bring the Pip-Boy up
    // to the face), and its press puts the selected weapon in the left hand (UpdatePipboyKey).
    using TPerformInput = void(void*, uint8_t*);
    static inline TPerformInput* RealMenuInput = nullptr;
    static inline TPerformInput* RealPlayerInput = nullptr;
    static void TakeInventoryClicks(uint8_t* apHead, const char* acpControls)
    {
        DualWieldService* pService = s_pService;
        bool click = false;
        for (uint8_t* pEvent = apHead; pEvent && !click; pEvent = NextEvent(pEvent))
            click = IsRightMouseButton(pEvent) && *reinterpret_cast<const int32_t*>(pEvent + kEventHandledOffset) != kHandledStop;
        // first test: what reaches the hooks in the Pip-Boy
        if (IsPipboyOpen())
        {
            for (uint8_t* pEvent = apHead; pEvent; pEvent = NextEvent(pEvent))
            {
                if (*reinterpret_cast<const int32_t*>(pEvent + kEventDeviceOffset) != kDeviceMouse ||
                    *reinterpret_cast<const int32_t*>(pEvent + kEventTypeOffset) != kEventButton || (ValueOf(pEvent) != 0.f && HeldOf(pEvent) != 0.f))
                    continue;
                spdlog::info("Dual wield: {} in the Pip-Boy: mouse button {}, value {}, held {:.2f}, handled {}", acpControls,
                             *reinterpret_cast<const int32_t*>(pEvent + kEventIdCodeOffset), ValueOf(pEvent), HeldOf(pEvent),
                             *reinterpret_cast<const int32_t*>(pEvent + kEventHandledOffset));
            }
        }
        GFx::Movie* pMovie = nullptr;
        GFx::Value list;
        if (!pService || !click || !IsPipboyOpen())
            return;
        if (!GetInventoryList(pMovie, list, true))
            return;
        for (uint8_t* pEvent = apHead; pEvent; pEvent = NextEvent(pEvent))
        {
            if (!IsRightMouseButton(pEvent))
                continue;
            if (ValueOf(pEvent) != 0.f && HeldOf(pEvent) == 0.f)
                pService->m_pipboyClick = true;
            *reinterpret_cast<int32_t*>(pEvent + kEventHandledOffset) = kHandledStop;
        }
    }
    static void MenuInput(void* apThis, uint8_t* apHead)
    {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            spdlog::info("Dual wield: MenuControls' input comes through our hook");
        TakeInventoryClicks(apHead, "MenuControls");
        RealMenuInput(apThis, apHead);
    }
    static void PlayerInput(void* apThis, uint8_t* apHead)
    {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            spdlog::info("Dual wield: PlayerControls' input comes through our hook");
        TakeInventoryClicks(apHead, "PlayerControls");
        RealPlayerInput(apThis, apHead);
    }
};

namespace
{
TiltedPhoques::Initializer s_dualWieldGameHooks(
    []()
    {
        POINTER_FALLOUT4(DualWieldHooks::TShowMuzzleFlash, s_showMuzzleFlash, 962812);
        POINTER_FALLOUT4(DualWieldHooks::TWeaponFire, s_weaponFire, 1240982);
        POINTER_FALLOUT4(DualWieldHooks::TQueueVats, s_queueVats, 32663);
        DualWieldHooks::RealShowMuzzleFlash = s_showMuzzleFlash.Get();
        DualWieldHooks::RealWeaponFire = s_weaponFire.Get();
        DualWieldHooks::RealQueueVats = s_queueVats.Get();
        POINTER_FALLOUT4(DualWieldHooks::TPerformInput, s_menuInput, 1280157);
        POINTER_FALLOUT4(DualWieldHooks::TPerformInput, s_playerInput, 663897);
        DualWieldHooks::RealMenuInput = s_menuInput.Get();
        DualWieldHooks::RealPlayerInput = s_playerInput.Get();
        POINTER_FALLOUT4(DualWieldHooks::TVatsPreDisplay, s_vatsPreDisplay, 1227069);
        POINTER_FALLOUT4(DualWieldHooks::THitChance, s_hitChance, 984571);
        DualWieldHooks::RealVatsPreDisplay = s_vatsPreDisplay.Get();
        DualWieldHooks::RealHitChance = s_hitChance.Get();
        TP_HOOK(&DualWieldHooks::RealShowMuzzleFlash, DualWieldHooks::ShowMuzzleFlash);
        TP_HOOK(&DualWieldHooks::RealWeaponFire, DualWieldHooks::WeaponFire);
        TP_HOOK(&DualWieldHooks::RealQueueVats, DualWieldHooks::QueueVats);
        TP_HOOK(&DualWieldHooks::RealMenuInput, DualWieldHooks::MenuInput);
        TP_HOOK(&DualWieldHooks::RealPlayerInput, DualWieldHooks::PlayerInput);
        TP_HOOK(&DualWieldHooks::RealVatsPreDisplay, DualWieldHooks::VatsPreDisplay);
        TP_HOOK(&DualWieldHooks::RealHitChance, DualWieldHooks::HitChance);
    });
} // namespace

DualWieldService::DualWieldService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_pModel(std::make_unique<OffHandModel>())
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&DualWieldService::OnUpdate>(this);
    m_overlayRestartedConnection = aDispatcher.sink<OverlayRestartedEvent>().connect<&DualWieldService::OnOverlayRestarted>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&DualWieldService::OnNotifyMovementAction>(this);
    s_pService = this;
    LoadConfig();
    // the game's equip events (EquipEventSource, CommonLibF4 id 485633: the object itself)
    POINTER_FALLOUT4(uint8_t, s_equipSource, 485634);
    reinterpret_cast<EventDispatcher<TESEquipEvent>*>(s_equipSource.Get())->RegisterSink(this);
}

DualWieldService::~DualWieldService() noexcept
{
    POINTER_FALLOUT4(uint8_t, s_equipSource, 485634);
    reinterpret_cast<EventDispatcher<TESEquipEvent>*>(s_equipSource.Get())->UnRegisterSink(this);
    s_pService = nullptr;
    if (float* pFov = GetWorldFov(); pFov && m_baseFov > 0.f)
        *pFov = m_baseFov;
    ReleaseLeftData();
    ReleaseRemoteData(0);
    DualWieldPose::ClearAll();
    ReleaseConsumeCull(PlayerCharacter::Get());
}

BSTEventResult DualWieldService::OnEvent(const TESEquipEvent* apEvent, const EventDispatcher<TESEquipEvent>*)
{
    // the player's consumable (equipped, as using one is to the game)
    if (apEvent && apEvent->IsEquip && apEvent->pActor && apEvent->pActor == PlayerCharacter::Get())
    {
        if (const TESForm* pItem = TESForm::GetById(apEvent->FormId); pItem && pItem->formType == FormType::Alchemy)
            m_consumeItem = apEvent->FormId;
    }
    return BSTEventResult::kOk;
}

void DualWieldService::UpdateConsume(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    if (const uint32_t item = m_consumeItem.exchange(0); item && m_consume < 0.f && m_activeNow)
    {
        TESForm* pItem = TESForm::GetById(item);
        const auto* pKeywords = pItem ? Cast<BGSKeywordForm>(pItem) : nullptr;
        // the game's own animation for it, if it has one (the stimpak's third person one is spelled "Stimpack")
        const bool animated = pKeywords && HasKeyword(pKeywords, kPlayAnimOnItemUse);
        const bool stealthBoy = animated && (HasKeyword(pKeywords, kStealthBoyKeyword) || HasKeyword(pKeywords, kChemTypeStealthBoy));
        if (!NodeTools::IsFirstPerson())
        {
            // Third person (every consumable: the weapons culled and the game's own animation played):
            // no lowering; both weapons culled and the arms the game's for as long as its own third person
            // animation plays (Animations.ba2's Common\UseStimpackOnSelf 2.00 s, UseStealthBoy 2.17 s: that one culls
            // the weapons itself from 0 to 1.77 s, the stimpak's leaves them out). Without one the game plays nothing,
            // and nothing changes here
            if (animated)
            {
                const char* pClip = stealthBoy ? "UseStealthBoy.hkx" : "UseStimpackOnSelf.hkx";
                const WeaponGrips::LimbClip* pAnimation = WeaponGrips::ThirdPersonClipIn("Common", pClip);
                m_consumeThirdPerson = true;
                m_consumeThirdPersonLength = pAnimation ? pAnimation->Duration : kConsumeThirdPersonFallback;
                m_consume = 0.f;
                m_reload = -1.f;
                m_swing = -1.f;
                if (m_guard)
                    SetGuard(apPlayer, false);
                spdlog::info("Dual wield: {:X} used in third person, the weapons culled for the game's {} ({:.2f} s{})", item, pClip, m_consumeThirdPersonLength,
                             pAnimation ? "" : ", its length not read");
            }
        }
        else
        {
            const char* pClip = animated ? (stealthBoy ? "UseStealthBoy.hkx" : "UseStimpakOnSelf.hkx") : nullptr;
            const WeaponGrips::ArmClip* pAnimation = pClip ? WeaponGrips::ClipIn("Common", pClip) : nullptr;
            m_consumeThirdPerson = false;
            m_consumeHold = pAnimation ? pAnimation->Duration : kConsumeNoAnimation;
            m_consumeRelease = pAnimation != nullptr;
            m_consume = 0.f;
            m_reload = -1.f;
            m_swing = -1.f;
            if (m_guard)
                SetGuard(apPlayer, false);
            spdlog::info("Dual wield: {:X} used, the weapons lowered {} ({:.2f} s)", item, pAnimation ? fmt::format("for the game's {}", pClip) : std::string("a moment (no animation of its own)"),
                         m_consumeHold);
        }
    }
    if (m_consume < 0.f)
    {
        m_consumeWeight = 1.f;
        ReleaseConsumeCull(apPlayer);
        return;
    }
    m_consume += aDelta;
    // third person: culled, the pose off (the grip too) until the game's animation has played
    if (m_consumeThirdPerson)
    {
        m_consumeWeight = 0.f;
        if (!m_consumeCulled)
        {
            apPlayer->CullWeapons(true);
            m_consumeCulled = true;
        }
        if (m_consume >= m_consumeThirdPersonLength || !m_activeNow)
        {
            m_consume = -1.f;
            m_consumeWeight = 1.f;
            ReleaseConsumeCull(apPlayer);
        }
        return;
    }
    // with the game's animation: the pose let go once the weapons are down, taken back just before they come up
    if (m_consumeRelease)
    {
        const float out = std::clamp((m_consume - (kConsumeDown - kConsumeFade)) / kConsumeFade, 0.f, 1.f);
        const float in = std::clamp((m_consume - (kConsumeDown + m_consumeHold - kConsumeFade)) / kConsumeFade, 0.f, 1.f);
        m_consumeWeight = std::clamp(1.f - out + in, 0.f, 1.f);
    }
    if (m_consume >= kConsumeDown + m_consumeHold + kConsumeUp || !m_activeNow)
    {
        m_consume = -1.f;
        m_consumeWeight = 1.f;
    }
}

void DualWieldService::ReleaseConsumeCull(PlayerCharacter* apPlayer) noexcept
{
    // (the game's cull is counted: ours taken back once)
    if (std::exchange(m_consumeCulled, false) && apPlayer)
        apPlayer->CullWeapons(false);
}

void DualWieldService::LoadConfig() noexcept
{
    const std::filesystem::path path = TiltedPhoques::GetPath() / "config" / "DualWield.ini";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path);
        file << "; Reclamation Day's dual wielding (docs/DUAL_WIELD.md): the key held for the slight zoom with two guns.\n"
                "; A letter, a digit, F1-F12 or a virtual key code such as 0x5A. Z is free in Fallout 4's own controls.\n"
                "; A weapon goes in the left hand with a right click on it in the Pip-Boy's inventory.\n"
                "[Keys]\nZoom=Z\n";
    }

    wchar_t text[32]{};
    std::string zoomName;
    GetPrivateProfileStringW(L"Keys", L"Zoom", L"Z", text, static_cast<DWORD>(std::size(text)), path.c_str());
    m_zoomKey = ParseKey(text, 'Z', zoomName);
    spdlog::info("Dual wield: {} held zooms with two guns, a right click in the Pip-Boy's inventory puts a weapon in the left hand (config/DualWield.ini)", zoomName);
}

// --- input (the game's input handlers, main thread)

DualWieldService::AttackInput DualWieldService::OnAttackButton(const void* apEvent) noexcept
{
    DualWieldService* pService = s_pService;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pService || !apEvent || !pPlayer)
        return AttackInput::kPass;

    const BSFixedString::Data* pUserEvent = UserEventOf(apEvent);
    const bool primary = pUserEvent == UserEvent("PrimaryAttack").data;
    const bool secondary = pUserEvent == UserEvent("SecondaryAttack").data;
    if (!primary && !secondary)
        return AttackInput::kPass;

    const float value = ValueOf(apEvent);
    const bool press = value != 0.f && HeldOf(apEvent) == 0.f;
    const bool release = value == 0.f;
    const bool active = pService->IsActive(pPlayer);

    // Riding: only the left hand slot (the right hand stays on the bars): left mouse fires it, right mouse
    // holds it up to the sights. Neither reaches the game, whatever's in the right hand.
    if (pService->m_onBike)
    {
        if (secondary)
            pService->m_bikeAds = !release;
        else
        {
            if (press)
                pService->m_leftPressed = true;
            if (release)
            {
                if (pService->m_leftDown)
                    pService->m_leftReleased = true;
                pService->m_leftDown = false;
            }
            else
                pService->m_leftDown = true;
        }
        return AttackInput::kSwallow;
    }

    // the left weapon on its way up or down: right mouse is still the left hand's (never the sights), nothing to do yet
    if (secondary && !active && pService->IsOut(pPlayer) && !IsPipboyOpen())
        return AttackInput::kSwallow;

    // Right mouse with a weapon in the left hand: the left hand's, never the sights
    if (secondary && active)
    {
        if (press)
            pService->m_leftPressed = true;
        if (release)
        {
            if (pService->m_leftDown)
                pService->m_leftReleased = true;
            pService->m_leftDown = false;
        }
        else
            pService->m_leftDown = true;
        return AttackInput::kSwallow;
    }

    // Left mouse while right mouse holds a left melee weapon: both held, the guard. The right weapon's attack doesn't
    // start, and one the game already has is let go (a release in place of this event); the guard's own presses and
    // releases never reach the game.
    if (primary)
    {
        pService->m_primaryDown = !release;
        if (press)
            pService->m_primarySwallowed = false;
        const bool guard = active && pService->m_left.Type == Kind::kMelee && pService->m_leftDown && !release;
        if (guard && !pService->m_primarySwallowed)
        {
            // from here until it's let go, left mouse is the guard's
            pService->m_primarySwallowed = true;
            if (std::exchange(pService->m_primaryGiven, false) && !press)
            {
                std::memcpy(pService->m_releaseEvent, apEvent, sizeof(pService->m_releaseEvent));
                *reinterpret_cast<void**>(pService->m_releaseEvent + 0x18) = nullptr;
                *reinterpret_cast<float*>(pService->m_releaseEvent + 0x38) = 0.f;
                return AttackInput::kReleaseInstead;
            }
            return AttackInput::kSwallow;
        }
        if (pService->m_primarySwallowed)
        {
            if (release)
                pService->m_primarySwallowed = false;
            return AttackInput::kSwallow;
        }
        if (press)
        {
            pService->m_primaryGiven = true;
            pService->m_sinceRightPress = 0.f;
        }
        if (release)
            pService->m_primaryGiven = false;
    }

    // Left mouse with the grappling gun in the right hand (with or without a left-hand weapon): held, it aims (the HUD
    // marks the landing point); let go, the press goes to the game, which fires
    if (primary && pPlayer->actorState.IsWeaponDrawn() && pService->RightKind(pPlayer) == Kind::kGrapple)
    {
        if (press)
        {
            // the spike still out: nothing to fire
            if (GrappleService::IsLineOut())
                return AttackInput::kSwallow;
            std::memcpy(pService->m_heldPress, apEvent, sizeof(pService->m_heldPress));
            // on its own when it's handed back, not with the rest of its frame's events
            *reinterpret_cast<void**>(pService->m_heldPress + 0x18) = nullptr;
            pService->m_rightGrappleHeld = true;
            return AttackInput::kSwallow;
        }
        if (release)
        {
            if (!pService->m_rightGrappleHeld)
                return AttackInput::kSwallow;
            pService->m_rightGrappleHeld = false;
            return GrappleService::IsLineOut() ? AttackInput::kSwallow : AttackInput::kReplayPress;
        }
        return AttackInput::kSwallow;
    }

    // The grapple no longer drawn with its press held back: forgotten
    if (primary)
        pService->m_rightGrappleHeld = false;
    return AttackInput::kPass;
}

const void* DualWieldService::HeldPress() noexcept
{
    return s_pService ? s_pService->m_heldPress : nullptr;
}

const void* DualWieldService::ReleaseEvent() noexcept
{
    return s_pService ? s_pService->m_releaseEvent : nullptr;
}

void DualWieldService::OnReadyWeaponButton(const void* apEvent) noexcept
{
    DualWieldService* pService = s_pService;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pService || !apEvent || !pPlayer)
        return;
    // Riding, a tap brings the left hand slot up to reload it, out or not
    if (pService->m_onBike && pService->m_left.Type != Kind::kNone && ValueOf(apEvent) == 0.f && HeldOf(apEvent) < kReloadTapSeconds)
    {
        pService->m_reloadTapped = true;
        pService->m_bikeUp = kBikeHoldSeconds;
        return;
    }
    if (!pService->IsActive(pPlayer))
        return;
    // ReadyWeapon is reload when tapped, holster when held: a tap is let go early
    if (ValueOf(apEvent) == 0.f && HeldOf(apEvent) < kReloadTapSeconds)
        pService->m_reloadTapped = true;
}

void DualWieldService::OnLaunch(const ProjectileLaunchData& acData, uint32_t aHandle) noexcept
{
    DualWieldService* pService = s_pService;
    if (!pService || !acData.pShooter || acData.pShooter != PlayerCharacter::Get())
        return;
    if (!s_launchingLeft)
    {
        s_rightShots.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (aHandle)
    {
        std::scoped_lock lock(pService->m_leftDataLock);
        pService->m_leftShots.push_back({aHandle, acData.pShooter->formID, pService->m_time});
    }
}

bool DualWieldService::IsLaunchingLeft() noexcept
{
    return s_launchingLeft;
}

bool DualWieldService::PrepareRemoteShot(Actor* apShooter, ProjectileLaunchData& aData) noexcept
{
    DualWieldService* pService = s_pService;
    if (!pService || !apShooter)
        return false;
    const auto it = std::find_if(pService->m_remotes.begin(), pService->m_remotes.end(), [apShooter](const RemoteHand& acHand) { return acHand.ActorId == apShooter->formID; });
    TESForm* pWeapon = it != pService->m_remotes.end() && it->Weapon.Type == Kind::kGun ? TESForm::GetById(it->Weapon.FormId) : nullptr;
    if (!pWeapon)
        return false;
    // the left gun as their game fired it: its stack in their copy's inventory (the inventory sync's) for its mods
    const auto stack = FindStack(apShooter, it->Weapon.FormId, &it->Weapon.Mods, false);
    aData.FromWeapon.pWeapon = pWeapon;
    aData.FromWeapon.pWeaponData = stack ? const_cast<uint8_t*>(StackInstance(stack->pStack)) : nullptr;
    return true;
}

void DualWieldService::OnRemoteShot(Actor* apShooter, uint32_t aHandle) noexcept
{
    DualWieldService* pService = s_pService;
    if (!pService || !apShooter || !aHandle)
        return;
    std::scoped_lock lock(pService->m_leftDataLock);
    pService->m_leftShots.push_back({aHandle, apShooter->formID, pService->m_time});
}

// --- the update

void DualWieldService::OnOverlayRestarted(const OverlayRestartedEvent&) noexcept
{
    m_sentCrosshair.clear();
}

void DualWieldService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("DualWield");
    const float delta = std::clamp(static_cast<float>(acEvent.Delta), 0.f, 0.1f);
    m_time += delta;
    m_pModel->Update();

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
    {
        // a load: the copies go with the old skeletons, and are put on the new ones once there
        m_pFirstRoot = m_pThirdRoot = nullptr;
        if (m_pPosedRoot)
        {
            DualWieldPose::Clear(m_pPosedRoot);
            m_pPosedRoot = nullptr;
        }
        m_weight = 0.f;
        m_gripWeight = 0.f;
        m_drawTime = m_holsterTime = -1.f;
        m_hadPlayer = false;
        // a third person consumable's cull taken back (the player kept across a load keeps the game's count)
        m_consume = -1.f;
        m_consumeWeight = 1.f;
        ReleaseConsumeCull(pPlayer);
        // the left gun's data points at the old copy's muzzle: made again once the copies are back
        ReleaseLeftData();
        m_vatsLeftReady = false;
        UpdateRemotes(delta);
        return;
    }

    // Back in the world: the save's left hand read back first (nothing checked against the new inventory, or written,
    // before that), for a while if its stack isn't there yet
    if (!m_hadPlayer)
    {
        m_hadPlayer = true;
        m_restorePending = true;
        m_restoreTime = 0.f;
    }
    if (m_restorePending)
    {
        if (RestoreFromGlobals(pPlayer))
            m_restorePending = false;
        else if ((m_restoreTime += delta) > kRestoreTimeout)
        {
            m_restorePending = false;
            spdlog::info("Dual wield: the save's left hand weapon isn't in the inventory, the hand stays empty");
            ClearLeftWeapon(pPlayer, "the save's weapon isn't carried");
            SaveToGlobals();
        }
        else
            return;
    }

    if ((m_checkTimer -= delta) <= 0.f)
    {
        m_checkTimer = kCheckInterval;
        CheckLeftWeapon(pPlayer);
    }
    UpdatePipboyKey(pPlayer);
    m_rightNow = RightKind(pPlayer);
    m_rightEmpty = RightIsEmpty(pPlayer);
    UpdateRightIdle(pPlayer);
    UpdateBike(pPlayer, delta);
    if (!m_onBike)
        DrawForLeftHand(pPlayer);
    UpdateDraw(pPlayer, delta);
    m_activeNow = IsActive(pPlayer);
    m_shownNow = IsOut(pPlayer);
    if (m_rightNow == Kind::kGrapple)
        KeepGrappleLoaded(pPlayer);

    // the right gun's shots since the last update (the launch hook)
    m_sinceRightShot += delta;
    m_sinceRightPress += delta;
    if (const uint32_t shots = s_rightShots.exchange(0, std::memory_order_relaxed))
    {
        m_shots += shots;
        m_rightKick = 1.f;
        m_sinceRightShot = 0.f;
    }

    UpdateVats(pPlayer);
    UpdateConsume(pPlayer, delta);
    UpdateLeftHand(pPlayer, delta);
    UpdatePose(pPlayer, delta);
    UpdateLeftData(pPlayer);
    UpdateZoom(pPlayer, delta);
    UpdateCrosshair(pPlayer);
    UpdatePipboyMark(pPlayer, delta);
    ProbeView(pPlayer, static_cast<float>(acEvent.Delta));

    // the grappling gun's aim: its button held in whichever hand it's in
    // (and the spikes it fires: the left one's, or the right hand's stack's, a High Velocity chamber's reaching further)
    const bool leftGrapple = m_activeNow && m_left.Type == Kind::kGrapple;
    const TESForm* pAimAmmo = leftGrapple ? m_stats.pAmmo : nullptr;
    if (!leftGrapple && m_rightGrappleHeld)
    {
        const TESForm* pRight = pPlayer->GetEquippedWeapon(0);
        const auto worn = pRight ? FindStack(pPlayer, pRight->formID, nullptr, true) : std::nullopt;
        const uint8_t* pInstance = worn ? StackInstance(worn->pStack) : nullptr;
        if (!pInstance && pRight)
            pInstance = reinterpret_cast<const uint8_t*>(pRight) + kWeaponDataOffset;
        pAimAmmo = pInstance ? Read<const TESForm*>(pInstance, kAmmoOffset) : nullptr;
    }
    GrappleService::SetAiming((leftGrapple && m_leftDown) || m_rightGrappleHeld, leftGrapple, pAimAmmo);

    if (m_left.FormId && (m_sendTimer -= delta) <= 0.f)
        SendState();
    UpdateRemotes(delta);
}

// On the motorcycle the left hand slot is all there is (no second weapon, nothing of the right hand's drawn): any of its
// buttons brings it up off the bars, and it goes back a while after they're let go. A spike out from it when the cable
// takes the rider off keeps it in the hand, the right hand holding nothing, until the line's back.
void DualWieldService::UpdateBike(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const bool riding = BikeLink::IsRiding();
    if (riding != m_onBike)
    {
        m_onBike = riding;
        m_bikeUp = 0.f;
        m_bikeAds = false;
        m_leftDown = false;
        m_leftPressed = false;
        m_leftReleased = false;
        spdlog::info("Dual wield: {} the motorcycle", riding ? "on" : "off");
    }
    if (m_bikeLine && !GrappleService::IsLineOut())
        m_bikeLine = false;
    if (riding && (m_bikeAds || m_leftDown || m_leftPressed) && m_left.Type != Kind::kNone)
        m_bikeUp = kBikeHoldSeconds;
    else
        m_bikeUp = std::max(0.f, m_bikeUp - aDelta);

    const float adsTarget = riding && m_bikeAds && m_left.Type != Kind::kNone && m_left.Type != Kind::kMelee ? 1.f : 0.f;
    m_bikeAdsWeight += (adsTarget - m_bikeAdsWeight) * std::min(1.f, aDelta * kBikeAdsRate);
    if (m_bikeAdsWeight < 0.001f)
        m_bikeAdsWeight = 0.f;
    BikeLink::SetAimZoom(kBikeAdsZoom * m_bikeAdsWeight);
    (void)apPlayer;
}

bool DualWieldService::IsActive(PlayerCharacter* apPlayer) const noexcept
{
    if (m_onBike || m_bikeLine)
        return IsOut(apPlayer) && !IsPipboyOpen() && !m_world.ctx().at<PipboyMapService>().IsShowing();
    // out, and drawn all the way (not on its way up or down)
    if (!IsOut(apPlayer) || !apPlayer->actorState.IsWeaponFullyDrawn())
        return false;
    // the Pip-Boy has the left arm (the game's, or our map's glance)
    return !IsPipboyOpen() && !m_world.ctx().at<PipboyMapService>().IsShowing();
}

bool DualWieldService::IsOut(PlayerCharacter* apPlayer) const noexcept
{
    if (m_left.Type == Kind::kNone || !apPlayer)
        return false;
    if (apPlayer->IsDead() || apPlayer->actorState.IsBleedingOut() || apPlayer->IsInPowerArmor())
        return false;
    if (m_onBike)
        return m_bikeUp > 0.f || m_reload >= 0.f;
    if (m_bikeLine)
        return true;
    // being drawn, drawn or being put away, and put away until its holster has played out
    if (apPlayer->actorState.GetWeaponState() == kSheathed && m_holsterTime < 0.f)
        return false;
    return RightKind(apPlayer) != Kind::kNone || RightIsEmpty(apPlayer);
}

void DualWieldService::UpdateDraw(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const uint32_t state = apPlayer->actorState.GetWeaponState();
    if (state != m_weaponState)
    {
        // out: drawing, drawn (from the Pip-Boy, a load: at once), or put away again
        const bool out = state >= kWantToDraw && state <= kDrawn;
        const bool wasOut = m_weaponState >= kWantToDraw && m_weaponState <= kDrawn;
        const bool firstPerson = NodeTools::IsFirstPerson();
        // first person with two guns the right arm's own draw and holster carry over (no motion of the left's own);
        // else the left weapon's own clips, and the reload's dip in third person
        const bool follows = firstPerson && m_pRightIdle && (m_rightNow == Kind::kGun || m_rightNow == Kind::kGrapple) &&
                             (m_left.Type == Kind::kGun || m_left.Type == Kind::kGrapple);
        const bool melee = m_left.Type == Kind::kMelee;
        if (out && !wasOut && state != kDrawn)
        {
            const WeaponGrips::ArmClip* pClip = melee ? m_meleeClips.pDraw : m_gunClips.pDraw;
            m_drawTime = 0.f;
            // (third person a melee weapon's own draw, else the dip)
            m_drawLength = !firstPerson ? (melee && m_meleeLimbs.pDraw ? m_meleeLimbs.pDraw->Duration : kDrawSeconds) : follows ? 0.f : pClip ? pClip->Duration : 0.f;
            m_holsterTime = -1.f;
            if (m_left.Type != Kind::kNone)
                spdlog::info("Dual wield: the left weapon drawn {}", !firstPerson ? "by the dip (third person)" : follows ? "with the right arm's own draw, mirrored" : pClip ? fmt::format("by its own clip ({:.2f} s)", pClip->Duration) : std::string("at once (no clip)"));
        }
        else if (!out && wasOut && state != kSheathed)
        {
            const WeaponGrips::ArmClip* pClip = melee ? m_meleeClips.pHolster : m_gunClips.pHolster;
            m_holsterTime = 0.f;
            m_holsterLength = !firstPerson ? (melee && m_meleeLimbs.pHolster ? m_meleeLimbs.pHolster->Duration : kHolsterSeconds) : follows ? 0.f : pClip ? pClip->Duration : 0.f;
            m_drawTime = -1.f;
            if (m_left.Type != Kind::kNone)
                spdlog::info("Dual wield: the left weapon put away {}", !firstPerson ? "by the dip (third person)" : follows ? "with the right arm's own holster, mirrored" : pClip ? fmt::format("by its own clip ({:.2f} s)", pClip->Duration) : std::string("at once (no clip)"));
        }
        m_weaponState = state;
    }
    if (m_drawTime >= 0.f && (m_drawTime += aDelta) >= m_drawLength && state >= kDrawn)
        m_drawTime = -1.f;
    // put away: until the game has it away and the left one's own holster has played out
    if (m_holsterTime >= 0.f && (m_holsterTime += aDelta) >= m_holsterLength && state == kSheathed)
        m_holsterTime = -1.f;
}

DualWieldService::MeleeLimbClips DualWieldService::ThirdPersonMeleeClips(const BGSKeywordForm* apKeywords) noexcept
{
    MeleeLimbClips clips{};
    if (!apKeywords)
        return clips;
    clips.pAttackA = WeaponGrips::ThirdPersonClipFor(apKeywords, true, "AttackStandingA.hkx");
    clips.pAttackB = WeaponGrips::ThirdPersonClipFor(apKeywords, true, "AttackStandingB.hkx");
    clips.pPower = WeaponGrips::ThirdPersonClipFor(apKeywords, true, "AttackStandingPower.hkx");
    clips.pBlock = WeaponGrips::ThirdPersonClipFor(apKeywords, true, "Block.hkx");
    clips.pIdle = WeaponGrips::ThirdPersonClipFor(apKeywords, true, "Idle.hkx");
    clips.pDraw = WeaponGrips::ThirdPersonClipFor(apKeywords, true, "Equip.hkx");
    clips.pHolster = WeaponGrips::ThirdPersonClipFor(apKeywords, true, "UnEquip.hkx");
    return clips;
}

bool DualWieldService::GameMovesArms(Actor* apActor, bool aRightGun, float aSpeed, bool aSprinting) noexcept
{
    if (!apActor || aSpeed < kMoveSpeed)
        return false;
    // a melee weapon's or the fists' own locomotion: the left hand is free in it
    if (!aRightGun || aSprinting)
        return true;
    // a gun's: its relaxed walk and jog swing both arms, its ready ones hold the gun in both hands
    static BSFixedString s_gunPlayState(kGunPlayState);
    uint32_t state = 0;
    return apActor->animationGraphHolder.GetVariableInt(&s_gunPlayState, &state) && state == kGunPlayRelaxed;
}

float DualWieldService::UpdateGameMove(PlayerCharacter* apPlayer, float aDelta, bool aSprinting, bool aHanging) noexcept
{
    // the game's melee attack state and gun state, said as they change
    const uint32_t attack = apPlayer->actorState.GetMeleeAttackState();
    if (attack != m_meleeAttackState)
    {
        static uint32_t s_logged = 0;
        if (s_logged++ < 24)
            spdlog::info("Dual wield: the game's melee attack state reads {} (was {})", attack, m_meleeAttackState);
        m_meleeAttackState = attack;
    }
    static BSFixedString s_gunPlayState(kGunPlayState);
    uint32_t gunPlay = 0;
    apPlayer->animationGraphHolder.GetVariableInt(&s_gunPlayState, &gunPlay);
    const uint32_t gunState = apPlayer->actorState.GetGunState() | (gunPlay << 8);
    if (gunState != m_gunState)
    {
        static uint32_t s_logged = 0;
        if (s_logged++ < 80)
            spdlog::info("Dual wield: the game's gun state reads {} and its graph's {} (0 ready, 1 alert, 2 relaxed), moving {:.0f} a second{}{}{}", gunState & 0xFF, gunState >> 8, m_speed,
                         aSprinting ? ", sprinting" : "", NodeTools::IsFirstPerson() ? ", first person" : "", m_sinceShot < kAimHoldSeconds || m_sinceRightShot < kAimHoldSeconds ? ", after a shot" : "");
        m_gunState = gunState;
    }

    // either weapon in use: the right one's melee attack, the left mouse held or just pressed, either gun's shots and the
    // guns kept up after them, the left one's swing or guard, the zoom; drawn or put away, the dip, a consumable
    const bool rightGun = m_rightNow == Kind::kGun || m_rightNow == Kind::kGrapple;
    const bool inUse = attack != 0 || m_primaryDown || m_sinceRightPress < kRightAttackHold || m_sinceRightShot < kAimHoldSeconds || m_leftDown ||
                       m_sinceShot < kAimHoldSeconds || m_swing >= 0.f || m_guardTime > 0.f || m_zoom > 0.f || m_weaponState != kDrawn || m_reload >= 0.f ||
                       m_consume >= 0.f || m_drawTime >= 0.f || m_holsterTime >= 0.f;
    const bool may = !NodeTools::IsFirstPerson() && !aHanging && !inUse && m_speed >= kMoveSpeed;
    // the right gun put in its relaxed state (its ready one holds it in both hands), asked as the game asks itself
    m_relaxRetry -= aDelta;
    if (may && rightGun && !aSprinting && gunPlay != kGunPlayRelaxed && m_relaxRetry <= 0.f)
    {
        m_relaxRetry = kRelaxRetrySeconds;
        using TSetGunState = bool(Actor*, int32_t);
        POINTER_FALLOUT4(TSetGunState, s_setGunState, 777818);
        const bool taken = s_setGunState.Get()(apPlayer, kGunStateRelaxed);
        spdlog::info("Dual wield: the right gun asked into its relaxed state ({})", taken ? "taken" : "not taken");
    }
    const float target = may && GameMovesArms(apPlayer, rightGun, m_speed, aSprinting) ? 1.f : 0.f;
    m_gameMove += (target - m_gameMove) * std::min(1.f, aDelta * kWeightRate);
    return Smooth(m_gameMove);
}

void DualWieldService::SetLimbClip(DualWieldPose::Arms& aArms, float aDelta) noexcept
{
    // its own idle under everything (moving, the unarmed gait goes over it)
    if (m_left.Type == Kind::kMelee && m_meleeLimbs.pIdle)
    {
        aArms.pLimbIdle = m_meleeLimbs.pIdle;
        aArms.LimbIdleTime = std::fmod(m_time, std::max(m_meleeLimbs.pIdle->Duration, 0.1f));
    }

    // the draw's or the holster's clip, the swing's clip (the attack the first person swing plays, the swing's phase
    // mapped onto it so the clip's HitFrame comes with the hit), else the block's (raised to where it holds while
    // guarding), else none: its idle and walk (or with no clips of its own, the right arm mirrored)
    const WeaponGrips::LimbClip* pClip = nullptr;
    float time = 0.f;
    if (m_left.Type == Kind::kMelee && m_drawTime >= 0.f && m_meleeLimbs.pDraw)
    {
        pClip = m_meleeLimbs.pDraw;
        time = m_drawTime;
    }
    else if (m_left.Type == Kind::kMelee && m_holsterTime >= 0.f && m_meleeLimbs.pHolster)
    {
        pClip = m_meleeLimbs.pHolster;
        time = m_holsterTime;
    }
    else if (m_left.Type == Kind::kMelee && m_swing >= 0.f)
    {
        pClip = m_powerSwing ? m_meleeLimbs.pPower : m_pSwingClip && m_pSwingClip == m_meleeClips.pAttackB ? m_meleeLimbs.pAttackB : m_meleeLimbs.pAttackA;
        if (pClip)
        {
            const float duration = pClip->Duration;
            const float hit = std::clamp(pClip->EventTime("HitFrame", duration * 0.5f), 0.01f, duration);
            const float phase = std::clamp(m_swing, 0.f, 1.f);
            time = phase < m_swingHitAt ? phase / std::max(m_swingHitAt, 0.01f) * hit : hit + (phase - m_swingHitAt) / std::max(1.f - m_swingHitAt, 0.01f) * (duration - hit);
        }
    }
    else if (m_left.Type == Kind::kMelee && m_guardTime > 0.f && m_meleeLimbs.pBlock && m_meleeClips.pBlock)
    {
        const WeaponGrips::ArmClip& first = *m_meleeClips.pBlock;
        pClip = m_meleeLimbs.pBlock;
        const float hold = std::min(first.EventTime("attackWinStart", first.Duration * 0.45f), first.Duration);
        const float holdHere = std::min(pClip->EventTime("attackWinStart", pClip->Duration * 0.45f), pClip->Duration);
        time = m_guardTime / std::max(hold, 0.01f) * holdHere;
    }

    // another clip (or the same one again from its start): blended from where the last one was
    if (pClip && (pClip != m_pLimbShown || time < m_limbShownTime - 0.05f))
    {
        m_pLimbFrom = m_limbWeight > 0.01f ? m_pLimbShown : nullptr;
        m_limbFromTime = m_limbShownTime;
        m_limbBlend = m_pLimbFrom ? 0.f : 1.f;
    }
    m_limbBlend = std::min(1.f, m_limbBlend + aDelta / kClipBlendSeconds);
    if (pClip)
    {
        m_pLimbShown = pClip;
        m_limbShownTime = time;
    }
    // on at once for a clip, off after it eased (the last one held as it ended)
    const float target = pClip ? 1.f : 0.f;
    m_limbWeight += (target - m_limbWeight) * std::min(1.f, aDelta * (pClip ? kLimbInRate : kLimbOutRate));
    if (!pClip && m_limbWeight < 0.01f)
    {
        m_limbWeight = 0.f;
        m_pLimbShown = m_pLimbFrom = nullptr;
    }
    aArms.pLimbClip = m_pLimbShown;
    aArms.LimbMovesBody = m_pLimbShown != m_meleeLimbs.pDraw && m_pLimbShown != m_meleeLimbs.pHolster;
    aArms.LimbTime = m_limbShownTime;
    aArms.pLimbClipFrom = m_pLimbFrom;
    aArms.LimbFromTime = m_limbFromTime;
    aArms.LimbBlend = m_limbBlend;
    aArms.LimbWeight = Smooth(m_limbWeight);
}

void DualWieldService::UpdateRightIdle(PlayerCharacter* apPlayer) noexcept
{
    // a gun's (the grappling gun's are the 10mm's); its instance's keywords, its mods' changes counted
    const TESForm* pRight = m_rightNow == Kind::kGun || m_rightNow == Kind::kGrapple ? apPlayer->GetEquippedWeapon(0) : nullptr;
    const auto worn = pRight ? FindStack(apPlayer, pRight->formID, nullptr, true) : std::nullopt;
    const BGSKeywordForm* pKeywords = pRight ? KeywordsOf(pRight, worn ? StackInstance(worn->pStack) : nullptr) : nullptr;
    if (pKeywords == m_pRightIdleFor)
        return;
    m_pRightIdleFor = pKeywords;
    m_pRightIdle = pKeywords ? WeaponGrips::ClipFor(pKeywords, false, "WPNIdleReady.hkx") : nullptr;
    if (pRight)
        spdlog::info("Dual wield: the right {:X}'s arm followed from its plain idle{}", pRight->formID, m_pRightIdle ? "" : ": none read, the holds keep still");
}

DualWieldPose::Motion DualWieldService::UpdateOwnMotion(float aDelta, bool aSprinting) noexcept
{
    // the clip now and its time, its motion from its last frame (a draw ends where the idle is) or from its first (a
    // sprint's from the idle's first: it holds the gun lowered all through)
    const WeaponGrips::ArmClip* pClip = nullptr;
    const WeaponGrips::ArmClip* pFrom = nullptr;
    float time = 0.f;
    bool fromEnd = false;
    // moving: its sprint, run or walk, as the game picks them by the speed (their motion from the idle's first frame)
    const WeaponGrips::ArmClip* pMove = aSprinting ? m_gunClips.pSprint
                                        : m_speed > kRunSpeed ? m_gunClips.pRun
                                        : m_speed > kMoveSpeed ? m_gunClips.pWalk
                                                               : nullptr;
    if (pMove && m_drawTime < 0.f && m_holsterTime < 0.f)
    {
        pClip = pMove;
        pFrom = m_gunClips.pIdle;
        time = std::fmod(m_time, std::max(pClip->Duration, 0.1f));
        m_pIdleClip = nullptr;
    }
    else if (m_drawTime >= 0.f && m_gunClips.pDraw)
    {
        pClip = m_gunClips.pDraw;
        time = m_drawTime;
        fromEnd = true;
        m_pIdleClip = nullptr;
    }
    else if (m_holsterTime >= 0.f && m_gunClips.pHolster)
    {
        pClip = m_gunClips.pHolster;
        time = m_holsterTime;
        m_pIdleClip = nullptr;
    }
    else if (m_gunClips.pIdle)
    {
        // the idle looped; after a few of its loops one of its fidgets, picked at random, then the idle from its start
        static std::minstd_rand s_random{std::random_device{}()};
        if (!m_pIdleClip)
        {
            m_pIdleClip = m_gunClips.pIdle;
            m_idleTime = 0.f;
        }
        m_idleTime += aDelta;
        if (m_idleTime >= std::max(m_pIdleClip->Duration, 0.1f))
        {
            if (m_pIdleClip == m_gunClips.pIdle)
            {
                m_idleTime = std::fmod(m_idleTime, std::max(m_pIdleClip->Duration, 0.1f));
                if (++m_idleLoops >= m_loopsToFidget && m_gunClips.FidgetCount > 0)
                {
                    m_pIdleClip = m_gunClips.Fidgets[s_random() % m_gunClips.FidgetCount];
                    m_idleTime = 0.f;
                    m_idleLoops = 0;
                    m_loopsToFidget = kFidgetLoopsMin + static_cast<uint32_t>(s_random() % (kFidgetLoopsMax - kFidgetLoopsMin + 1));
                }
            }
            else
            {
                m_pIdleClip = m_gunClips.pIdle;
                m_idleTime = 0.f;
            }
        }
        pClip = m_pIdleClip;
        time = m_idleTime;
    }

    DualWieldPose::Motion now{};
    if (pClip && !pClip->Frames.empty())
    {
        const WeaponGrips::ArmClip& anchor = pFrom && !pFrom->Frames.empty() ? *pFrom : *pClip;
        const NodeTools::Transform& from = (fromEnd ? anchor.Frames.back() : anchor.Frames.front()).Hand;
        now = DualWieldPose::MirrorMotion(DualWieldPose::MotionBetween(from, pClip->At(time).Hand));
    }
    // another clip: blended in from where the last one had the hand
    if (pClip != m_pOwnClip)
    {
        m_ownFrom = m_ownLast;
        m_ownBlend = 0.f;
        m_pOwnClip = pClip;
    }
    m_ownBlend = std::min(1.f, m_ownBlend + aDelta / kOwnBlendSeconds);
    m_ownLast = DualWieldPose::BlendMotion(m_ownFrom, now, Smooth(m_ownBlend));
    return m_ownLast;
}

DualWieldService::Kind DualWieldService::RightKind(PlayerCharacter* apPlayer) const noexcept
{
    const TESForm* pWeapon = apPlayer ? apPlayer->GetEquippedWeapon(0) : nullptr;
    if (!pWeapon)
        return Kind::kNone;
    const auto worn = FindStack(apPlayer, pWeapon->formID, nullptr, true);
    return static_cast<Kind>(KindOf(pWeapon, worn ? StackInstance(worn->pStack) : nullptr));
}

bool DualWieldService::RightIsEmpty(PlayerCharacter* apPlayer) noexcept
{
    if (!apPlayer)
        return false;
    const TESForm* pWeapon = apPlayer->GetEquippedWeapon(0);
    const auto worn = pWeapon ? FindStack(apPlayer, pWeapon->formID, nullptr, true) : std::nullopt;
    return IsUnarmed(pWeapon, worn ? StackInstance(worn->pStack) : nullptr);
}

void DualWieldService::DrawForLeftHand(PlayerCharacter* apPlayer) noexcept
{
    if (!m_drawPending)
        return;
    if (!m_left.FormId || !m_rightEmpty || apPlayer->IsInPowerArmor())
    {
        m_drawPending = false;
        return;
    }
    if (IsPipboyOpen() || m_world.ctx().at<PipboyMapService>().IsShowing())
        return;
    m_drawPending = false;
    if (!apPlayer->actorState.IsWeaponDrawn())
    {
        apPlayer->SetWeaponDrawn(true);
        spdlog::info("Dual wield: the fists drawn beside the left hand's {:X}", m_left.FormId);
    }
}

// --- the weapon in the left hand

void DualWieldService::SetLeftWeapon(PlayerCharacter* apPlayer, TESForm* apWeapon, uint32_t aStackIndex, bool aQuiet) noexcept
{
    auto* pObject = static_cast<TESBoundObject*>(apWeapon);
    const auto stack = StackAt(apPlayer, pObject, aStackIndex);
    if (!stack)
        return;
    const uint8_t kind = KindOf(apWeapon, StackInstance(stack->pStack));
    const char* pName = apWeapon->GetName();
    const String customName = stack->pStack->spExtra ? stack->pStack->spExtra->GetCustomName() : String{};
    const std::string name = !customName.empty() ? std::string(customName.c_str()) : std::string(pName && *pName ? pName : "that weapon");
    if (kind == 0)
    {
        ShowHudMessage("Only a pistol, an SMG, a one-handed melee weapon or the grappling gun goes in the left hand");
        return;
    }
    // the very one in the right hand (a stack of one that's worn)
    if (stack->pStack->IsEquipped() && stack->pStack->uiCount <= 1)
    {
        ShowHudMessage(fmt::format("{} is in your right hand", name));
        return;
    }

    m_left.FormId = apWeapon->formID;
    m_left.Mods = StackMods(stack->pStack);
    m_left.Type = static_cast<Kind>(kind);
    // the grip the game's own first person animations give it (its folder's idle clip)
    m_pLeftGrip = WeaponGrips::ForKeywords(KeywordsOf(apWeapon, StackInstance(stack->pStack)), m_left.Type == Kind::kMelee);
    // a melee weapon's third person one, its third person idle's (where its third person attacks hold it)
    m_pLeftGrip3P = m_left.Type == Kind::kMelee ? WeaponGrips::ThirdPersonGripFor(KeywordsOf(apWeapon, StackInstance(stack->pStack)), true) : nullptr;
    // a melee weapon's first person clips, played on the left arm mirrored (DualWieldPose)
    m_meleeClips = MeleeClips{};
    if (m_left.Type == Kind::kMelee)
    {
        const BGSKeywordForm* pKeywords = KeywordsOf(apWeapon, StackInstance(stack->pStack));
        m_meleeClips.pIdle = WeaponGrips::ClipFor(pKeywords, true, "Idle.hkx");
        m_meleeClips.pAttackA = WeaponGrips::ClipFor(pKeywords, true, "AttackA.hkx");
        m_meleeClips.pAttackB = WeaponGrips::ClipFor(pKeywords, true, "AttackB.hkx");
        m_meleeClips.pPower = WeaponGrips::ClipFor(pKeywords, true, "AttackPowerStanding.hkx");
        m_meleeClips.pBlock = WeaponGrips::ClipFor(pKeywords, true, "Block.hkx");
        m_meleeClips.pDraw = WeaponGrips::ClipFor(pKeywords, true, "Equip.hkx");
        m_meleeClips.pHolster = WeaponGrips::ClipFor(pKeywords, true, "UnEquip.hkx");
        m_meleeClips.pRun = WeaponGrips::ClipFor(pKeywords, true, "RunForward.hkx");
        m_meleeClips.pSprint = WeaponGrips::ClipFor(pKeywords, true, "SprintForward.hkx");
    }
    // and its third person ones (its right arm's, mirrored onto the left)
    m_meleeLimbs = m_left.Type == Kind::kMelee ? ThirdPersonMeleeClips(KeywordsOf(apWeapon, StackInstance(stack->pStack))) : MeleeLimbClips{};
    m_pLimbShown = m_pLimbFrom = nullptr;
    m_limbWeight = 0.f;
    m_limbBlend = 1.f;
    // a gun's: its motion when the right hand has no gun to follow (DualWieldService::UpdateOwnMotion)
    m_gunClips = GunClips{};
    if (m_left.Type == Kind::kGun || m_left.Type == Kind::kGrapple)
    {
        const BGSKeywordForm* pKeywords = KeywordsOf(apWeapon, StackInstance(stack->pStack));
        m_gunClips.pIdle = WeaponGrips::ClipFor(pKeywords, false, "WPNIdleReady.hkx");
        for (const char* pFidget : {"WPNIdleReadyA.hkx", "WPNIdleReadyB.hkx", "WPNIdleReadyC.hkx", "WPNIdleReadyD.hkx"})
        {
            if (const WeaponGrips::ArmClip* pClip = WeaponGrips::ClipFor(pKeywords, false, pFidget))
                m_gunClips.Fidgets[m_gunClips.FidgetCount++] = pClip;
        }
        m_gunClips.pDraw = WeaponGrips::ClipFor(pKeywords, false, "WPNEquip.hkx");
        m_gunClips.pHolster = WeaponGrips::ClipFor(pKeywords, false, "WPNUnEquip.hkx");
        m_gunClips.pWalk = WeaponGrips::ClipFor(pKeywords, false, "WPNWalkForwardReady.hkx");
        m_gunClips.pRun = WeaponGrips::ClipFor(pKeywords, false, "WPNRunForwardReady.hkx");
        m_gunClips.pSprint = WeaponGrips::ClipFor(pKeywords, false, "WPNSprint.hkx");
    }
    m_pOwnClip = m_pIdleClip = nullptr;
    m_ownLast = m_ownFrom = DualWieldPose::Motion{};
    m_idleLoops = 0;
    m_pSwingClip = nullptr;
    m_guardTime = 0.f;
    m_pShownClip = m_pClipFrom = nullptr;
    m_clipBlend = 1.f;
    ReadStats(apPlayer);
    // put in the hand loaded, as a weapon taken up from the Pip-Boy is
    m_magazine = LoadableRounds(apPlayer);
    m_magazineKnown = true;
    UpdateReserve(apPlayer);
    m_reload = -1.f;
    m_swing = -1.f;
    m_leftDown = m_leftPressed = m_leftReleased = false;
    m_leftHeld = 0.f;
    m_holdUsed = false;
    if (m_guard)
        SetGuard(apPlayer, false);

    m_pModel->Build(apWeapon, m_left.Mods, apPlayer);
    m_pFirstRoot = m_pThirdRoot = nullptr;
    // nothing in the right hand: the fists come up beside it once the Pip-Boy closes
    m_drawPending = !aQuiet && RightIsEmpty(apPlayer) && !apPlayer->actorState.IsWeaponDrawn();
    if (!aQuiet)
    {
        FileSound::PlayDescriptor(m_stats.pEquipSound, apPlayer);
        ShowHudMessage(fmt::format("Left hand: {}", name));
    }
    spdlog::info("Dual wield: {:X} ({}, {} mods, stack {}) in the left hand{}, a {} ({} rounds of {}, {:.2f} s between shots{})", m_left.FormId, name,
                 m_left.Mods.size(), aStackIndex, aQuiet ? " (from the save)" : "", kind == 1 ? "gun" : kind == 2 ? "melee weapon" : "grappling gun", m_magazine,
                 m_stats.Capacity, m_stats.Interval, m_stats.Automatic ? ", automatic" : "");
    SaveToGlobals();
    SendState();
}

void DualWieldService::SaveToGlobals() noexcept
{
    TESGlobal* pPlugin = SaveGlobal(kGlobalPlugin);
    TESGlobal* pForm = SaveGlobal(kGlobalForm);
    TESGlobal* pMods = SaveGlobal(kGlobalMods);
    if (!pPlugin || !pForm || !pMods)
    {
        static bool s_warned = false;
        if (!std::exchange(s_warned, true))
            spdlog::warn("Dual wield: {} isn't loaded, the left hand isn't kept in the save", kSavePlugin);
        return;
    }

    uint32_t pluginKey = 0;
    uint32_t localId = 0;
    uint32_t modsKey = 0;
    if (const Mod* pMod = m_left.FormId ? PluginOf(m_left.FormId) : nullptr)
    {
        pluginKey = PluginKey(*pMod);
        localId = LocalId(m_left.FormId, *pMod);
        modsKey = ModsKey(m_left.Mods);
    }
    pPlugin->f = static_cast<float>(pluginKey);
    pForm->f = static_cast<float>(localId);
    pMods->f = static_cast<float>(modsKey);
}

bool DualWieldService::RestoreFromGlobals(PlayerCharacter* apPlayer) noexcept
{
    const TESGlobal* pPlugin = SaveGlobal(kGlobalPlugin);
    const TESGlobal* pForm = SaveGlobal(kGlobalForm);
    const TESGlobal* pMods = SaveGlobal(kGlobalMods);
    if (!pPlugin || !pForm || !pMods)
        return true;
    const uint32_t pluginKey = ReadGlobal(pPlugin);
    const uint32_t localId = ReadGlobal(pForm);
    const uint32_t modsKey = ReadGlobal(pMods);

    // the save's left hand is empty
    if (!pluginKey)
    {
        ClearLeftWeapon(apPlayer, "the loaded save's left hand is empty");
        return true;
    }

    // its weapon: the loaded plugin of that name, the form of that id in it
    const Mod* pMod = nullptr;
    for (auto* pEntry = &ModManager::Get()->mods.entry; pEntry && pEntry->data && !pMod; pEntry = pEntry->next)
    {
        if (pEntry->data->IsLoaded() && PluginKey(*pEntry->data) == pluginKey)
            pMod = pEntry->data;
    }
    TESForm* pWeapon = pMod ? TESForm::GetById(pMod->GetFormId(localId)) : nullptr;
    if (!pWeapon)
    {
        spdlog::warn("Dual wield: the save's left hand weapon (plugin {:06X}, id {:X}) isn't loaded", pluginKey, localId);
        ClearLeftWeapon(apPlayer, "the save's weapon isn't loaded");
        return true;
    }

    // already in the hand (a door, a fast travel, or the same save loaded again): its stats read again, as a loaded
    // save's inventory is new and the old stack's instance is gone (PrepareLeftData built the gun's data from it: crash
    // 2026-10-29 23:26)
    if (m_left.FormId == pWeapon->formID && ModsKey(m_left.Mods) == modsKey && FindStack(apPlayer, m_left.FormId, &m_left.Mods, false))
    {
        ReadStats(apPlayer);
        return true;
    }

    // its stack: the one with those mods
    if (apPlayer->pInventoryList)
    {
        for (auto& item : apPlayer->pInventoryList->DataA)
        {
            if (item.pObject != pWeapon)
                continue;
            uint32_t index = 0;
            for (auto* pStack = item.spStackData; pStack; pStack = pStack->spNextStack, ++index)
            {
                if (ModsKey(StackMods(pStack)) != modsKey)
                    continue;
                SetLeftWeapon(apPlayer, pWeapon, index, true);
                return m_left.FormId == pWeapon->formID;
            }
        }
    }
    return false;
}

void DualWieldService::ClearLeftWeapon(PlayerCharacter* apPlayer, const char* acpReason) noexcept
{
    if (!m_left.FormId)
        return;
    spdlog::info("Dual wield: the left hand is empty again, {}", acpReason);
    m_pModel->Reset();
    if (m_pPosedRoot)
    {
        DualWieldPose::Clear(m_pPosedRoot);
        m_pPosedRoot = nullptr;
    }
    m_pFirstRoot = m_pThirdRoot = nullptr;
    m_left = HeldWeapon{};
    m_pLeftGrip = nullptr;
    m_meleeClips = MeleeClips{};
    m_pSwingClip = nullptr;
    m_guardTime = 0.f;
    m_pShownClip = m_pClipFrom = nullptr;
    m_clipBlend = 1.f;
    m_stats = Stats{};
    m_magazine = 0;
    m_magazineKnown = false;
    m_reload = -1.f;
    m_swing = -1.f;
    m_weight = 0.f;
    m_leftDown = m_leftPressed = m_leftReleased = false;
    if (m_guard)
        SetGuard(apPlayer, false);
    ReleaseLeftData();
    SaveToGlobals();
    SendState();
}

void DualWieldService::CheckLeftWeapon(PlayerCharacter* apPlayer) noexcept
{
    if (!m_left.FormId)
        return;
    const auto stack = FindStack(apPlayer, m_left.FormId, &m_left.Mods, false);
    if (!stack)
    {
        // dropped, sold, stored, or modded at a workbench (its mods aren't these any more)
        ClearLeftWeapon(apPlayer, "it isn't in the inventory as it was");
        ShowHudMessage("The left hand is empty");
        return;
    }
    if (stack->pStack->IsEquipped() && stack->pStack->uiCount <= 1)
    {
        ClearLeftWeapon(apPlayer, "it was put in the right hand");
        return;
    }
    ReadStats(apPlayer);
    // rounds taken from the inventory another way (sold, dropped, the right gun's)
    m_magazine = std::min(m_magazine, CountOf(apPlayer, m_stats.pAmmo));
    UpdateReserve(apPlayer);
}

void DualWieldService::UpdateReserve(PlayerCharacter* apPlayer) noexcept
{
    m_reserve = static_cast<int64_t>(CountOf(apPlayer, m_stats.pAmmo)) - m_magazine;
    // the same ammo as the right gun: its magazine's aren't the left's to show
    const TESForm* pRight = apPlayer->GetEquippedWeapon(0);
    if (const auto worn = pRight ? FindStack(apPlayer, pRight->formID, nullptr, true) : std::nullopt)
    {
        const uint8_t* pInstance = StackInstance(worn->pStack);
        if (!pInstance)
            pInstance = reinterpret_cast<const uint8_t*>(pRight) + kWeaponDataOffset;
        const auto ammo = GameHud::GetAmmo();
        if (Read<const TESForm*>(pInstance, kAmmoOffset) == m_stats.pAmmo && ammo.Valid)
            m_reserve -= ammo.First;
    }
    m_reserve = std::max<int64_t>(m_reserve, 0);
}

bool DualWieldService::RightNeedsRounds(PlayerCharacter* apPlayer) const noexcept
{
    const TESForm* pRight = apPlayer->GetEquippedWeapon(0);
    const auto worn = pRight ? FindStack(apPlayer, pRight->formID, nullptr, true) : std::nullopt;
    if (!worn)
        return false;
    const uint8_t* pInstance = StackInstance(worn->pStack);
    if (!pInstance)
        pInstance = reinterpret_cast<const uint8_t*>(pRight) + kWeaponDataOffset;
    const auto ammo = GameHud::GetAmmo();
    return ammo.Valid && ammo.First < Read<uint16_t>(pInstance, kCapacityOffset) && ammo.Second > 0;
}

void DualWieldService::ReadStats(PlayerCharacter* apPlayer) noexcept
{
    const TESForm* pWeapon = TESForm::GetById(m_left.FormId);
    const auto stack = FindStack(apPlayer, m_left.FormId, &m_left.Mods, false);
    if (!pWeapon || !stack)
        return;
    const uint8_t* pStackInstance = StackInstance(stack->pStack);
    const uint8_t* pInstance = pStackInstance ? pStackInstance : reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponDataOffset;

    Stats stats{};
    stats.pInstance = const_cast<uint8_t*>(pStackInstance);
    stats.pAmmo = Read<TESForm*>(pInstance, kAmmoOffset);
    stats.pProjectile = ProjectileOf(pInstance);
    stats.pAttackSound = Read<TESForm*>(pInstance, kAttackSound2DOffset);
    if (!stats.pAttackSound)
        stats.pAttackSound = Read<TESForm*>(pInstance, kAttackSoundOffset);
    stats.pDryFireSound = Read<TESForm*>(pInstance, kDryFireSoundOffset);
    stats.pEquipSound = Read<TESForm*>(pInstance, kEquipSoundOffset);
    stats.Capacity = Read<uint16_t>(pInstance, kCapacityOffset);
    const float speed = std::max(Read<float>(pInstance, kSpeedOffset), 0.1f);
    stats.Automatic = (Read<uint32_t>(pInstance, kFlagsOffset) & kFlagAutomatic) != 0;
    // the game's rate of fire: the attack delay at the weapon's speed (10mm 0.21 s, the SMG 0.11 s)
    stats.Interval = m_left.Type == Kind::kMelee ? kSwingSeconds / speed : std::clamp(Read<float>(pInstance, kAttackDelayOffset) / speed, kMinInterval, 1.5f);
    if (const uint8_t* pRanged = Read<const uint8_t*>(pInstance, kRangedDataOffset))
    {
        stats.Projectiles = static_cast<uint32_t>(std::max<int8_t>(Read<int8_t>(pRanged, kProjectileCountOffset), 1));
        stats.ReloadSeconds = ReloadSecondsOf(pInstance);
    }
    FlashOf(stats.pProjectile, stats.MuzzleFlash, stats.FlashSeconds);
    m_stats = std::move(stats);
}

uint32_t DualWieldService::LoadableRounds(PlayerCharacter* apPlayer) const noexcept
{
    if (!m_stats.pAmmo)
        return 0;
    int64_t rounds = CountOf(apPlayer, m_stats.pAmmo);
    // the same ammo as the right gun: what's in its magazine isn't there to load
    const TESForm* pRight = apPlayer->GetEquippedWeapon(0);
    if (const auto worn = pRight ? FindStack(apPlayer, pRight->formID, nullptr, true) : std::nullopt)
    {
        const uint8_t* pInstance = StackInstance(worn->pStack);
        if (!pInstance)
            pInstance = reinterpret_cast<const uint8_t*>(pRight) + kWeaponDataOffset;
        if (Read<const TESForm*>(pInstance, kAmmoOffset) == m_stats.pAmmo)
        {
            const auto ammo = GameHud::GetAmmo();
            if (ammo.Valid)
                rounds -= ammo.First;
        }
    }
    return static_cast<uint32_t>(std::clamp<int64_t>(rounds, 0, m_stats.Capacity));
}

// --- the Pip-Boy's right click, and its mark on the left hand's weapon

void DualWieldService::UpdatePipboyMark(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    if (!IsPipboyOpen())
    {
        m_markTimer = 0.f;
        return;
    }
    if ((m_markTimer -= aDelta) > 0.f)
        return;
    m_markTimer = kMarkInterval;

    // The inventory's list entries: the left hand's weapon (its item and stack) gets the mark after its name, any other
    // loses it. The page builds its entries again from the game's data when the inventory changes, so they're looked
    // at all the while the inventory is up.
    GFx::Movie* pMovie = nullptr;
    GFx::Value list;
    GFx::Value entries;
    if (!GetInventoryList(pMovie, list) || !list.GetMember("entryList", &entries))
        return;
    const size_t markLength = std::strlen(kLeftMark);
    bool changed = false;
    const uint32_t count = entries.GetArraySize();
    for (uint32_t i = 0; i < count; ++i)
    {
        GFx::Value entry;
        GFx::Value text;
        if (!entries.GetElement(i, &entry) || !entry.IsObject() || !entry.GetMember("text", &text))
            continue;
        const char* pText = text.GetString();
        if (!pText)
            continue;
        std::string name = pText;
        const bool marked = name.size() >= markLength && name.compare(name.size() - markLength, markLength, kLeftMark) == 0;

        bool left = false;
        uint32_t handle = 0;
        uint32_t stackIndex = 0;
        if (m_left.FormId && ReadItem(static_cast<int32_t>(i), handle, stackIndex))
        {
            const BGSInventoryItem* pItem = InventoryItem(handle);
            if (pItem && pItem->pObject && pItem->pObject->formID == m_left.FormId)
            {
                const auto stack = StackAt(apPlayer, pItem->pObject, stackIndex);
                left = stack && SameMods(StackMods(stack->pStack), m_left.Mods);
            }
        }
        if (left == marked)
            continue;
        name = left ? name + kLeftMark : name.substr(0, name.size() - markLength);
        GFx::Value newText;
        pMovie->CreateString(&newText, name.c_str());
        entry.SetMember("text", newText);
        changed = true;
    }
    if (changed)
        list.Invoke("UpdateList");
}

void DualWieldService::UpdatePipboyKey(PlayerCharacter* apPlayer) noexcept
{
    if (!m_pipboyClick.exchange(false) || !IsPipboyOpen())
        return;

    GFx::Movie* pMovie = nullptr;
    GFx::Value list;
    GFx::Value index;
    if (!GetInventoryList(pMovie, list))
        return;
    if (!list.GetMember("selectedIndex", &index) || index.GetNumber() < 0.0)
    {
        spdlog::info("Dual wield: no entry selected in the Pip-Boy's inventory");
        return;
    }
    uint32_t handle = 0;
    uint32_t stackIndex = 0;
    if (!ReadItem(static_cast<int32_t>(index.GetNumber()), handle, stackIndex))
    {
        spdlog::warn("Dual wield: the Pip-Boy's inventory has no item {} with a HandleID and a StackID", index.GetNumber());
        return;
    }
    const BGSInventoryItem* pItem = InventoryItem(handle);
    TESForm* pObject = pItem ? pItem->pObject : nullptr;
    spdlog::info("Dual wield: Pip-Boy right click on handle {} stack {}: {:X}", handle, stackIndex, pObject ? pObject->formID : 0);
    if (!pObject || pObject->formType != FormType::Weapon)
        return;

    // the one in the left hand: taken out again
    const auto selected = StackAt(apPlayer, pItem->pObject, stackIndex);
    if (pObject->formID == m_left.FormId && selected && SameMods(StackMods(selected->pStack), m_left.Mods))
    {
        ClearLeftWeapon(apPlayer, "put away from the Pip-Boy");
        ShowHudMessage("The left hand is empty");
        return;
    }
    SetLeftWeapon(apPlayer, pObject, stackIndex);
}

// --- what the left hand does

void DualWieldService::UpdateLeftHand(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const bool pressed = std::exchange(m_leftPressed, false);
    const bool released = std::exchange(m_leftReleased, false);
    const bool tapped = std::exchange(m_reloadTapped, false);
    m_sinceShot += aDelta;
    m_leftKick = std::max(0.f, m_leftKick - aDelta / kKickSeconds);
    m_rightKick = std::max(0.f, m_rightKick - aDelta / kKickSeconds);

    if (!m_activeNow)
    {
        m_leftDown = false;
        m_swing = -1.f;
        m_reload = -1.f;
        m_leftHeld = 0.f;
        m_holdUsed = false;
        if (m_guard)
            SetGuard(apPlayer, false);
        m_guardWeight = std::max(0.f, m_guardWeight - aDelta * kGuardRate);
        return;
    }

    // a consumable's lowering: nothing else meanwhile
    if (m_consume >= 0.f)
        return;

    // The reload, whatever the left hand holds: the reload key with either gun short of rounds takes both arms down and up
    // again (the left gun loaded at the bottom, a right gun by the game's own reload under them)
    if (m_reload >= 0.f)
    {
        UpdateReload(apPlayer, aDelta);
        return;
    }
    if (tapped)
    {
        const bool leftShort = m_left.Type == Kind::kGun && m_magazine < LoadableRounds(apPlayer);
        if (leftShort || RightNeedsRounds(apPlayer))
        {
            StartReload(apPlayer, true);
            return;
        }
        spdlog::info("Dual wield: the reload key with both guns full");
    }
    // The right gun emptied beside a left gun: the game reloads it by itself (its auto reload), and both go down for it as
    // for the reload key (with its arm's animation followed, the right gun's reload clip would show at its side)
    if (m_rightNow == Kind::kGun && (m_left.Type == Kind::kGun || m_left.Type == Kind::kGrapple))
    {
        const auto ammo = GameHud::GetAmmo();
        if (ammo.Valid && ammo.First > 0)
            m_rightDryDipped = false;
        else if (ammo.Valid && ammo.Second > 0 && !m_rightDryDipped && m_sinceRightShot < 1.f)
        {
            m_rightDryDipped = true;
            spdlog::info("Dual wield: the right gun emptied");
            StartReload(apPlayer, true);
            return;
        }
    }

    switch (m_left.Type)
    {
    case Kind::kGun:
    {
        // emptied: the game reloads its guns by themselves after the last round, and so does the left one
        if (m_magazine == 0 && m_sinceShot >= kAutoReloadAfter && LoadableRounds(apPlayer) > 0)
        {
            StartReload(apPlayer, false);
            break;
        }
        const bool wantShot = pressed || (m_leftDown && m_stats.Automatic);
        // Riding the arm comes up off the bars first: the shot waits for it (the press kept for then)
        if (m_onBike && wantShot && m_leftAim < kBikeAimReady)
        {
            m_leftPressed = pressed;
            break;
        }
        if (!wantShot || m_sinceShot < m_stats.Interval)
            break;
        if (m_magazine == 0)
        {
            // a click, then the reload when there's anything to load
            if (pressed)
            {
                FileSound::PlayDescriptor(m_stats.pDryFireSound, apPlayer);
                if (LoadableRounds(apPlayer) > 0)
                    StartReload(apPlayer, false);
            }
            break;
        }
        FireLeft(apPlayer);
        break;
    }
    case Kind::kMelee:
    {
        UpdateLeftMelee(apPlayer, aDelta, pressed, released);
        break;
    }
    case Kind::kGrapple:
    {
        // held: the HUD marks the landing point (GrappleService); let go: the shot
        if (released && !GrappleService::IsLineOut())
        {
            FireLeftGrapple(apPlayer);
            // from the motorcycle: it stays in the hand if the cable takes the rider off
            if (m_onBike)
                m_bikeLine = true;
        }
        break;
    }
    default: break;
    }
}

void DualWieldService::FireLeft(PlayerCharacter* apPlayer) noexcept
{
    TESForm* pWeapon = TESForm::GetById(m_left.FormId);
    if (!pWeapon || !PrepareLeftData(apPlayer))
    {
        spdlog::warn("Dual wield: the left gun {:X} has no weapon data to fire with (no copy in the hand yet?)", m_left.FormId);
        m_sinceShot = 0.f;
        return;
    }

    // The game's own shot, as its weaponFire event fires a weapon (the handler, CommonLibF4 id 1240981):
    // TESObjectWEAP::Fire (1056037), then its follow up (208990, a debug trace when that's on), with the left gun as the
    // main hand's weapon (EquipSwap). Fire takes the muzzle from the weapon data's fire node (the left copy's
    // ProjectileNode), aims through its aim model (the cone of fire, which grows with each shot), launches the weapon's
    // projectiles (the launch hook sends them to the other players), plays its fire sounds, uses its ammo (the
    // inventory's and the magazine's) and runs the perks on firing; the projectile sets off its muzzle flash
    // (DualWieldHooks::ShowMuzzleFlash).
    // The magazine is one more than it holds while that runs: the game's ammo use asks for the reload action when it
    // empties a magazine, and the action is the player's, so the right gun would reload. The left gun has its own.
    auto* pMagazine = reinterpret_cast<uint32_t*>(m_pLeftData + kDataMagazineOffset);
    *pMagazine = m_magazine + 1;
    const ObjectInstance instance{pWeapon, m_stats.pInstance};
    bool fired = false;
    // Riding with the sights up: the game's own aimed shot, its cone as tight as it has it for sights (the gun state the
    // game reads for that), for the length of the shot; unaimed it stays as it is, the hip's wider cone
    const bool sighted = m_onBike && m_bikeAdsWeight > 0.5f;
    const uint32_t gunStateWas = apPlayer->actorState.GetGunState();
    if (sighted)
        apPlayer->actorState.SetGunStateRaw(kGunStateSighted);
    {
        EquipSwap swap(apPlayer, pWeapon, m_stats.pInstance, m_pLeftData);
        if (swap)
        {
            using TFire = void(const ObjectInstance*, TESObjectREFR*, uint32_t, TESForm*, TESForm*);
            POINTER_FALLOUT4(TFire, s_fire, 1056038);
            using TAfterFire = void(const ObjectInstance*, Actor*, uint32_t);
            POINTER_FALLOUT4(TAfterFire, s_afterFire, 208991);
            s_launchingLeft = true;
            s_fire.Get()(&instance, apPlayer, kMainEquipIndex, nullptr, nullptr);
            s_afterFire.Get()(&instance, apPlayer, kMainEquipIndex);
            s_launchingLeft = false;
            fired = true;
        }
    }
    if (sighted)
        apPlayer->actorState.SetGunStateRaw(gunStateWas);
    m_magazine = *pMagazine > 0 ? *pMagazine - 1 : 0;
    *pMagazine = m_magazine;
    m_sinceShot = 0.f;
    if (!fired)
    {
        spdlog::warn("Dual wield: no main hand weapon to fire the left gun as");
        return;
    }
    m_leftKick = 1.f;
    ++m_shots;
    UpdateReserve(apPlayer);
    SendCue(RequestMovementAction::kCueFire);
}

void DualWieldService::UpdateLeftMelee(PlayerCharacter* apPlayer, float aDelta, bool aPressed, bool aReleased) noexcept
{
    // the guard: both buttons held (not in the middle of a swing); the graph's IsBlocking kept on while it's up
    const bool guard = m_leftDown && m_primaryDown && m_swing < 0.f;
    if (guard != m_guard)
        SetGuard(apPlayer, guard);
    else if (m_guard)
    {
        static BSFixedString s_isBlocking("IsBlocking");
        apPlayer->animationGraphHolder.SetVariableBool(&s_isBlocking, true);
    }
    m_guardWeight = guard ? std::min(1.f, m_guardWeight + aDelta * kGuardRate) : std::max(0.f, m_guardWeight - aDelta * kGuardRate);

    // the hold: a power attack once it's as long as the game's power attack delay, a plain swing when it's let go
    // before; a hold that guarded does neither
    if (aPressed)
    {
        m_leftHeld = 0.f;
        m_holdUsed = false;
    }
    if (m_leftDown)
        m_leftHeld += aDelta;
    if (m_guard)
        m_holdUsed = true;
    // ready: no swing, or the one under way past where the game lets the next start (its clip's initiateStart)
    const bool ready = (m_swing < 0.f || m_swing >= m_swingChainAt) && m_sinceShot >= m_stats.Interval;
    if (m_leftDown && !m_holdUsed && ready && m_leftHeld >= PowerAttackDelay())
    {
        m_holdUsed = true;
        StartSwing(apPlayer, true);
    }
    else if (aReleased && !m_holdUsed && ready)
    {
        m_holdUsed = true;
        StartSwing(apPlayer, false);
    }
    if (!m_leftDown)
        m_leftHeld = 0.f;

    // the swing: its clip's length at the weapon's speed, the game's melee hit at its HitFrame
    if (m_swing >= 0.f)
    {
        m_swing += aDelta / std::max(m_swingLength, 0.1f);
        if (!m_swingHit && m_swing >= m_swingHitAt)
        {
            m_swingHit = true;
            SwingHit(apPlayer);
        }
        if (m_swing >= 1.f)
            m_swing = -1.f;
    }
    // the block clip: raised to where it holds (its attackWinStart) while guarding, played back down when let go
    if (const WeaponGrips::ArmClip* pBlock = m_meleeClips.pBlock)
    {
        const float hold = std::min(pBlock->EventTime("attackWinStart", pBlock->Duration * 0.45f), pBlock->Duration);
        m_guardTime = m_guard ? std::min(hold, m_guardTime + aDelta) : std::max(0.f, m_guardTime - aDelta);
    }
}

void DualWieldService::StartSwing(PlayerCharacter* apPlayer, bool aPower) noexcept
{
    // the game's check before an attack, as the race's left attack with the left weapon: a power attack cooling down,
    // or not enough action points for it, and the swing doesn't start (the meter flashes)
    static BSFixedString s_attack(kLeftAttack);
    static BSFixedString s_powerAttack(kLeftPowerAttack);
    if (!CanAttack(apPlayer, RaceAttack(apPlayer, aPower ? s_powerAttack : s_attack), TESForm::GetById(m_left.FormId), m_stats.pInstance))
    {
        spdlog::info("Dual wield: the left {}swing refused (the game's attack check: cooling down or not enough action points)", aPower ? "power " : "");
        return;
    }

    // its clip: the power attack, else the two attacks in turn, at the weapon's speed; the hit at the clip's HitFrame, the
    // next swing from its initiateStart (none of them: the swing of old, its hit half way)
    const float speed = std::max(kSwingSeconds / std::max(m_stats.Interval, 0.05f), 0.1f);
    m_pSwingClip = aPower ? m_meleeClips.pPower : (std::exchange(m_swingAlternate, !m_swingAlternate) ? m_meleeClips.pAttackB : m_meleeClips.pAttackA);
    if (!m_pSwingClip && !aPower)
        m_pSwingClip = m_meleeClips.pAttackA ? m_meleeClips.pAttackA : m_meleeClips.pAttackB;
    if (m_pSwingClip && m_pSwingClip->Duration > 0.05f)
    {
        const float duration = m_pSwingClip->Duration;
        m_swingLength = duration / speed;
        m_swingHitAt = std::clamp(m_pSwingClip->EventTime("HitFrame", duration * 0.5f) / duration, 0.05f, 0.95f);
        const float chain = std::max(m_pSwingClip->EventTime("initiateStartA", -1.f), m_pSwingClip->EventTime("initiateStartB", -1.f));
        m_swingChainAt = chain > 0.f ? std::clamp(chain / duration, m_swingHitAt, 1.f) : 1.f;
    }
    else
    {
        m_pSwingClip = nullptr;
        m_swingLength = std::max(m_stats.Interval, 0.1f) * (aPower ? kPowerSwingStretch : 1.f);
        m_swingHitAt = 0.5f;
        m_swingChainAt = 1.f;
    }

    m_swing = 0.f;
    m_swingHit = false;
    m_powerSwing = aPower;
    m_sinceShot = 0.f;
    FileSound::PlayDescriptor(m_stats.pAttackSound, apPlayer);
    SendCue(aPower ? RequestMovementAction::kCuePowerSwing : RequestMovementAction::kCueSwing);
}

void DualWieldService::SwingHit(PlayerCharacter* apPlayer) noexcept
{
    TESForm* pWeapon = TESForm::GetById(m_left.FormId);
    if (!pWeapon)
        return;

    // The game's own melee hit, what a swing's HitFrame does for the hand that swings (its handler, CommonLibF4 id
    // 88020, queues it for the event's equip index): the target the game's melee detection finds in the weapon's reach
    // (or a sweep's, by perk), the hit built from the weapon (HitData's melee initialization: damage, perks, criticals,
    // sneak attacks) and taken by the target as any other (its reaction, aggression and crime, the kill's experience).
    // The left weapon isn't equipped, so while it runs the weapon the game asks the player for is the left one.
    // The attack it reads (the high process' attack under way, what an attack's animation sets) is the race's left
    // attack, or its left power attack: its damage multiplier, its power attack flag (the stagger, the action points).
    static BSFixedString s_attack(kLeftAttack);
    static BSFixedString s_powerAttack(kLeftPowerAttack);
    uint8_t* pAttack = RaceAttack(apPlayer, m_powerSwing ? s_powerAttack : s_attack);
    AIProcess* pProcess = apPlayer->currentProcess;
    auto* pHigh = pProcess ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(pProcess) + kHighProcessOffset) : nullptr;
    auto** ppAttackSlot = pHigh && pAttack ? reinterpret_cast<uint8_t**>(pHigh + kAttackDataOffset) : nullptr;
    uint8_t* pPrevious = nullptr;
    if (ppAttackSlot)
    {
        AddRef(pAttack);
        pPrevious = std::exchange(*ppAttackSlot, pAttack);
    }

    s_leftSwing.pActor = apPlayer;
    s_leftSwing.pWeapon = pWeapon;
    s_leftSwing.pInstance = m_stats.pInstance;
    using TMeleeHit = void(Actor*, uint32_t, bool);
    POINTER_FALLOUT4(TMeleeHit, s_meleeHit, 216551);
    s_meleeHit.Get()(apPlayer, kMainEquipIndex, true);
    s_leftSwing = LeftSwing{};

    if (ppAttackSlot)
    {
        *ppAttackSlot = pPrevious;
        DecRef(pAttack);
    }
    spdlog::info("Dual wield: the left {}swing's hit ({:X}, {})", m_powerSwing ? "power " : "", m_left.FormId,
                 pAttack ? "the race's left attack" : "no left attack in the race");
}

void DualWieldService::SetGuard(PlayerCharacter* apPlayer, bool aOn) noexcept
{
    // The game's block: a hit taken while the graph says IsBlocking is blocked (its damage cut, the block's stagger
    // rules and perks), whatever the weapon
    m_guard = aOn;
    static BSFixedString s_isBlocking("IsBlocking");
    if (apPlayer)
        apPlayer->animationGraphHolder.SetVariableBool(&s_isBlocking, aOn);
    SendCue(aOn ? RequestMovementAction::kCueGuardOn : RequestMovementAction::kCueGuardOff);
}

// --- the left gun's EquippedWeaponData

uint8_t* DualWieldService::GetLeftFireNode(PlayerCharacter* apPlayer) const noexcept
{
    static BSFixedString s_muzzle("ProjectileNode");
    uint8_t* pRoot = NodeTools::IsFirstPerson() ? NodeTools::GetFirstPersonRoot(apPlayer) : NodeTools::GetThirdPersonRoot(apPlayer);
    uint8_t* pCopy = pRoot ? m_pModel->GetAttached(pRoot) : nullptr;
    if (!pCopy)
        return nullptr;
    uint8_t* pMuzzle = NodeTools::GetObjectByName(pCopy, s_muzzle);
    return pMuzzle ? pMuzzle : pCopy;
}

bool DualWieldService::PrepareLeftData(PlayerCharacter* apPlayer) noexcept
{
    TESForm* pWeapon = m_left.Type == Kind::kGun || m_left.Type == Kind::kGrapple ? TESForm::GetById(m_left.FormId) : nullptr;
    uint8_t* pFireNode = pWeapon ? GetLeftFireNode(apPlayer) : nullptr;
    if (!pWeapon || !pFireNode)
        return false;
    if (m_pLeftData && (m_pLeftDataWeapon != pWeapon || m_pLeftDataInstance != m_stats.pInstance || m_pLeftFireNode != pFireNode))
        ReleaseLeftData();

    if (!m_pLeftData)
    {
        // as equipping the gun builds it (AIProcess::SetEquippedItem, CommonLibF4 id 1200276): the game's allocation,
        // its constructor (420919: the actor, the weapon and its instance, the equip index; the aim model and fire sounds
        // from the instance), one reference; the instance's reference kept while it's in use
        void* pMemory = Memory::AllocatePlain(kWeaponDataSize);
        if (!pMemory)
            return false;
        using TConstruct = uint8_t*(void*, Actor*, const ObjectInstance*, uint32_t);
        POINTER_FALLOUT4(TConstruct, s_construct, 420920);
        const ObjectInstance instance{pWeapon, m_stats.pInstance};
        uint8_t* pData = s_construct.Get()(pMemory, apPlayer, &instance, kMainEquipIndex);
        AddRef(pData);
        if (m_stats.pInstance)
            AddRef(static_cast<uint8_t*>(m_stats.pInstance));
        std::scoped_lock lock(m_leftDataLock);
        m_pLeftData = pData;
        m_pLeftDataWeapon = pWeapon;
        m_pLeftDataInstance = m_stats.pInstance;
        m_pLeftFireNode = pFireNode;
        spdlog::info("Dual wield: the left gun's weapon data made ({:X}, {} view)", m_left.FormId, NodeTools::IsFirstPerson() ? "first person" : "third person");
    }

    *reinterpret_cast<TESForm**>(m_pLeftData + kDataAmmoOffset) = m_stats.pAmmo;
    *reinterpret_cast<uint8_t**>(m_pLeftData + kDataFireNodeOffset) = pFireNode;
    // VATS' shots keep it themselves while they play
    if (!m_vatsLongest.load())
        *reinterpret_cast<uint32_t*>(m_pLeftData + kDataMagazineOffset) = m_magazine;
    return true;
}

void DualWieldService::ReleaseLeftData() noexcept
{
    uint8_t* pData = nullptr;
    void* pInstance = nullptr;
    {
        std::scoped_lock lock(m_leftDataLock);
        pData = std::exchange(m_pLeftData, nullptr);
        pInstance = std::exchange(m_pLeftDataInstance, nullptr);
        m_pLeftDataWeapon = nullptr;
        m_pLeftFireNode = nullptr;
    }
    if (pData)
        DecRef(pData);
    if (pData && pInstance)
        DecRef(static_cast<uint8_t*>(pInstance));
}

DualWieldService::LeftData DualWieldService::AcquireLeftData(uint32_t aActorId) noexcept
{
    std::scoped_lock lock(m_leftDataLock);
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (pPlayer && aActorId == pPlayer->formID)
    {
        if (!m_pLeftData)
            return {};
        AddRef(m_pLeftData);
        return {aActorId, m_pLeftData, m_pLeftDataWeapon, m_pLeftDataInstance};
    }
    for (const auto& data : m_remoteData)
    {
        if (data.ActorId != aActorId || !data.pData)
            continue;
        AddRef(data.pData);
        return data;
    }
    return {};
}

uint32_t DualWieldService::LeftShooterOf(uint32_t aHandle) noexcept
{
    std::scoped_lock lock(m_leftDataLock);
    const auto it = std::find_if(m_leftShots.begin(), m_leftShots.end(), [aHandle](const LeftShot& acShot) { return acShot.Handle == aHandle; });
    return it != m_leftShots.end() ? it->ActorId : 0u;
}

void DualWieldService::UpdateLeftData(PlayerCharacter* apPlayer) noexcept
{
    {
        std::scoped_lock lock(m_leftDataLock);
        const float now = m_time;
        m_leftShots.erase(std::remove_if(m_leftShots.begin(), m_leftShots.end(), [now](const LeftShot& acShot) { return now - acShot.Time > kProjectileMemory; }), m_leftShots.end());
    }

    // made (and kept up with the view's copy) while the left gun is out; kept while it's put away, so its flash goes out
    if (m_activeNow && (m_left.Type == Kind::kGun || m_left.Type == Kind::kGrapple))
        PrepareLeftData(apPlayer);
    if (!m_pLeftData)
        return;

    // what the game does every frame for each equipped weapon's data (the visitors 85173 and 1144352): its aim model
    // (the cone of fire settling), its muzzle flash (lit, faded, put out), in VATS at VATS' time scale
    const float frameTime = GameFrameTime();
    if (void* pAimModel = Read<void*>(m_pLeftData, kDataAimModelOffset))
    {
        using TUpdateAimModel = void(void*, float);
        POINTER_FALLOUT4(TUpdateAimModel, s_updateAimModel, 890570);
        s_updateAimModel.Get()(pAimModel, frameTime);
    }
    if (void* pFlash = Read<void*>(m_pLeftData, kDataMuzzleFlashOffset))
    {
        float flashTime = frameTime;
        const uint8_t* pVats = GetVats();
        if (pVats && Read<uint32_t>(pVats, kVatsModeOffset) == kVatsPlaying && Read<const void*>(pVats, kVatsTimeActorOffset) == apPlayer)
        {
            using TVatsTimeScale = float(const void*);
            POINTER_FALLOUT4(TVatsTimeScale, s_vatsTimeScale, 819286);
            flashTime *= s_vatsTimeScale.Get()(pVats);
        }
        using TUpdateMuzzleFlash = void(void*, float, Actor*);
        POINTER_FALLOUT4(TUpdateMuzzleFlash, s_updateMuzzleFlash, 447432);
        s_updateMuzzleFlash.Get()(pFlash, flashTime, apPlayer);
    }
}

void DualWieldService::PrepareRemoteData(RemoteHand& aHand, Actor* apActor, uint8_t* apRoot) noexcept
{
    static BSFixedString s_muzzle("ProjectileNode");
    TESForm* pWeapon = TESForm::GetById(aHand.Weapon.FormId);
    uint8_t* pCopy = pWeapon && apRoot ? aHand.pModel->GetAttached(apRoot) : nullptr;
    uint8_t* pFireNode = pCopy ? NodeTools::GetObjectByName(pCopy, s_muzzle) : nullptr;
    if (!pFireNode)
        pFireNode = pCopy;
    if (!pFireNode || pFireNode == aHand.pFireNode)
        return;
    ReleaseRemoteData(aHand.ActorId);

    // as for the local left gun (PrepareLeftData), with their copy's stack (the inventory sync's) for its mods
    const auto stack = FindStack(apActor, aHand.Weapon.FormId, &aHand.Weapon.Mods, false);
    auto* pInstance = stack ? const_cast<uint8_t*>(StackInstance(stack->pStack)) : nullptr;
    void* pMemory = Memory::AllocatePlain(kWeaponDataSize);
    if (!pMemory)
        return;
    using TConstruct = uint8_t*(void*, Actor*, const ObjectInstance*, uint32_t);
    POINTER_FALLOUT4(TConstruct, s_construct, 420920);
    const ObjectInstance instance{pWeapon, pInstance};
    uint8_t* pData = s_construct.Get()(pMemory, apActor, &instance, kMainEquipIndex);
    AddRef(pData);
    if (pInstance)
        AddRef(pInstance);
    const uint8_t* pWeaponData = pInstance ? pInstance : reinterpret_cast<const uint8_t*>(pWeapon) + kWeaponDataOffset;
    *reinterpret_cast<TESForm**>(pData + kDataAmmoOffset) = Read<TESForm*>(pWeaponData, kAmmoOffset);
    *reinterpret_cast<uint8_t**>(pData + kDataFireNodeOffset) = pFireNode;
    aHand.pFireNode = pFireNode;
    std::scoped_lock lock(m_leftDataLock);
    m_remoteData.push_back({aHand.ActorId, pData, pWeapon, pInstance});
}

void DualWieldService::ReleaseRemoteData(uint32_t aActorId) noexcept
{
    Vector<LeftData> released;
    {
        std::scoped_lock lock(m_leftDataLock);
        for (auto it = m_remoteData.begin(); it != m_remoteData.end();)
        {
            if (aActorId && it->ActorId != aActorId)
            {
                ++it;
                continue;
            }
            released.push_back(*it);
            it = m_remoteData.erase(it);
        }
    }
    for (const auto& data : released)
    {
        if (data.pData)
            DecRef(data.pData);
        if (data.pData && data.pInstance)
            DecRef(static_cast<uint8_t*>(data.pInstance));
    }
}

// --- VATS with two guns

bool DualWieldService::QueuesLeft(uint32_t aQueued) const noexcept
{
    // right, left, right, left: the second, fourth... attack queued is the left gun's
    return m_vatsLeftReady.load() && (aQueued & 1) != 0;
}

bool DualWieldService::FiresLeftInVats(PlayerCharacter* apPlayer) noexcept
{
    if (!m_vatsLeftReady.load() || m_vatsMagazine.load() == 0)
        return false;
    const uint8_t* pVats = GetVats();
    if (!pVats || Read<uint32_t>(pVats, kVatsModeOffset) != kVatsPlaying || Read<const void*>(pVats, kVatsAttackerOffset) != apPlayer)
        return false;
    // the attack under way: VATS takes each off its list once played, so the list's length counts them (the longest
    // seen in this playback is where it started); an automatic weapon's burst is one attack
    const uint32_t size = Read<uint32_t>(pVats, kVatsAttacksSizeOffset);
    uint32_t longest = m_vatsLongest.load();
    while (size > longest && !m_vatsLongest.compare_exchange_weak(longest, size))
    {
    }
    longest = std::max(longest, size);
    return ((longest - size) & 1) != 0;
}

void DualWieldService::UpdateVats(PlayerCharacter* apPlayer) noexcept
{
    // the left gun's shots VATS played: its magazine, the kicks and the crosshair, as any shot of it
    if (const uint32_t shots = m_vatsLeftShots.exchange(0))
    {
        m_magazine = m_vatsMagazine.load();
        m_sinceShot = 0.f;
        m_leftKick = 1.f;
        m_shots += shots;
        UpdateReserve(apPlayer);
        SendCue(RequestMovementAction::kCueFire);
    }
    const uint8_t* pVats = GetVats();
    const bool playing = pVats && Read<uint32_t>(pVats, kVatsModeOffset) == kVatsPlaying && Read<const void*>(pVats, kVatsAttackerOffset) == apPlayer;
    if (!playing)
    {
        m_vatsLongest = 0;
        m_vatsMagazine = m_magazine;
    }
    m_vatsLeftReady = m_activeNow && m_left.Type == Kind::kGun && m_rightNow == Kind::kGun && m_reload < 0.f && m_magazine > 0 && m_pLeftData;
}

void DualWieldService::FireLeftGrapple(PlayerCharacter* apPlayer) noexcept
{
    if (!m_stats.pAmmo || CountOf(apPlayer, m_stats.pAmmo) == 0)
    {
        FileSound::PlayDescriptor(m_stats.pDryFireSound, apPlayer);
        ShowHudMessage("No grapple spikes");
        return;
    }
    TESForm* pWeapon = TESForm::GetById(m_left.FormId);
    if (!pWeapon || !PrepareLeftData(apPlayer))
    {
        spdlog::warn("Dual wield: the left grappling gun has no weapon data to fire with (no copy in the hand yet?)");
        return;
    }

    // The game's own shot, as the left gun's (FireLeft): Fire launches the spike's projectile from the left copy's muzzle
    // toward the crosshair and uses a spike from the inventory; the launch hook gives the shot to GrappleService (its
    // line, the cable from the left hand's gun). The one-spike magazine is kept from emptying (the spike comes back to
    // the gun on its cable; an empty magazine would have the game reload the right hand's weapon).
    auto* pMagazine = reinterpret_cast<uint32_t*>(m_pLeftData + kDataMagazineOffset);
    *pMagazine = std::max(m_magazine, 1u) + 1;
    const ObjectInstance instance{pWeapon, m_stats.pInstance};
    {
        EquipSwap swap(apPlayer, pWeapon, m_stats.pInstance, m_pLeftData);
        if (!swap)
            return;
        using TFire = void(const ObjectInstance*, TESObjectREFR*, uint32_t, TESForm*, TESForm*);
        POINTER_FALLOUT4(TFire, s_fire, 1056038);
        s_launchingLeft = true;
        s_fire.Get()(&instance, apPlayer, kMainEquipIndex, nullptr, nullptr);
        s_launchingLeft = false;
    }
    *pMagazine = m_magazine;
    m_leftKick = 1.f;
    m_sinceShot = 0.f;
    SendCue(RequestMovementAction::kCueFire);
}

void DualWieldService::KeepGrappleLoaded(PlayerCharacter* apPlayer) noexcept
{
    uint8_t* pData = MainWeaponData(apPlayer);
    if (!pData)
        return;
    auto* pMagazine = reinterpret_cast<uint32_t*>(pData + kDataMagazineOffset);
    const auto* pAmmo = Read<const TESForm*>(pData, kDataAmmoOffset);
    if (*pMagazine == 0 && pAmmo && CountOf(apPlayer, pAmmo) > 0)
    {
        *pMagazine = 1;
        spdlog::info("Dual wield: the grappling gun in the right hand loaded a spike itself");
    }
    DecRef(pData);
}

void DualWieldService::StartReload(PlayerCharacter* apPlayer, bool aBoth) noexcept
{
    m_reload = 0.f;
    m_reloadBoth = aBoth;
    // as long as the longer of the two guns' reloads (a melee weapon or the grappling gun beside a gun: the gun's)
    const TESForm* pRight = apPlayer->GetEquippedWeapon(0);
    const auto worn = pRight ? FindStack(apPlayer, pRight->formID, nullptr, true) : std::nullopt;
    const uint8_t* pRightInstance = worn ? StackInstance(worn->pStack) : nullptr;
    if (!pRightInstance && pRight && pRight->formType == FormType::Weapon)
        pRightInstance = reinterpret_cast<const uint8_t*>(pRight) + kWeaponDataOffset;
    const float rightSeconds = m_rightNow == Kind::kGun ? ReloadSecondsOf(pRightInstance) : 0.f;
    m_reloadSeconds = std::clamp(std::max(m_left.Type == Kind::kGun ? m_stats.ReloadSeconds : 0.f, rightSeconds), kMinReloadSeconds, kMaxReloadSeconds);
    m_magazineKnown = m_left.Type != Kind::kGun;
    if (m_guard)
        SetGuard(apPlayer, false);
    spdlog::info("Dual wield: the lowered reload ({:.1f} s{}), {} of {} in the left gun", m_reloadSeconds, aBoth ? ", both guns" : "", m_magazine, m_stats.Capacity);
    SendCue(RequestMovementAction::kCueReload);
}

void DualWieldService::UpdateReload(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    m_reload += aDelta / std::max(m_reloadSeconds, 0.1f);
    // loaded while it's out of view
    if (!m_magazineKnown && m_reload >= 0.5f)
    {
        m_magazine = LoadableRounds(apPlayer);
        m_magazineKnown = true;
        UpdateReserve(apPlayer);
    }
    if (m_reload >= 1.f)
        m_reload = -1.f;
}

// --- what's shown

void DualWieldService::UpdatePose(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    // on the grappling gun's cable: the gun's arm up along it (the left arm with the gun in the left hand)
    glm::vec3 anchor{};
    bool hangLeft = false;
    const bool hanging = GrappleService::GetHang(anchor, hangLeft);
    // the left weapon out: drawn, on its way up or down, or held while the Pip-Boy (the game's, or our map's glance) has
    // the left arm (it stays in the hand; the arms are the Pip-Boy's then)
    const bool shown = m_shownNow;
    const bool pipboy = IsPipboyOpen() || m_world.ctx().at<PipboyMapService>().IsShowing();
    // a parkour move (a vault, a climb, the grapple's hand-over to a ledge grab) plays its own arms: the pose lets go of
    // them at once, the left weapon hidden as the move culls the right one; an emote too (docs/EMOTES.md)
    const bool moving = m_world.ctx().at<ParkourService>().IsPlayerMoving() || EmoteService::IsPlayerEmoting();
    const float target = ((shown && !pipboy && m_pModel->IsReady()) || hanging) && !moving ? 1.f : 0.f;
    m_weight += (target - m_weight) * std::min(1.f, aDelta * (moving ? kMoveWeightRate : kWeightRate));
    if (target == 0.f && m_weight < 0.01f)
        m_weight = 0.f;
    const float gripTarget = shown && m_pModel->IsReady() && !moving ? 1.f : 0.f;
    m_gripWeight += (gripTarget - m_gripWeight) * std::min(1.f, aDelta * (moving ? kMoveWeightRate : kWeightRate));
    if (gripTarget == 0.f && m_gripWeight < 0.01f)
        m_gripWeight = 0.f;

    // the copies on both skeletons (each view's), shown while both hands are out
    uint8_t* pFirst = NodeTools::GetFirstPersonRoot(apPlayer);
    uint8_t* pThird = NodeTools::GetThirdPersonRoot(apPlayer);
    m_attachTimer -= aDelta;
    if (m_pModel->IsReady() && (pFirst != m_pFirstRoot || pThird != m_pThirdRoot || m_attachTimer <= 0.f))
    {
        m_attachTimer = kAttachInterval;
        if (m_pFirstRoot && m_pFirstRoot != pFirst)
            m_pModel->DetachFrom(m_pFirstRoot);
        if (m_pThirdRoot && m_pThirdRoot != pThird)
            m_pModel->DetachFrom(m_pThirdRoot);
        m_pFirstRoot = pFirst;
        m_pThirdRoot = pThird;
        m_pModel->AttachTo(pFirst);
        m_pModel->AttachTo(pThird);
    }
    // hidden while the game's animation for a consumable has the arms
    // (and culled with the right one while a third person consumable's animation plays)
    m_pModel->SetShown(shown && !moving && !(m_consume >= 0.f && (m_consumeThirdPerson || (m_consumeRelease && m_consumeWeight < 0.5f))));

    const bool firstPerson = NodeTools::IsFirstPerson();
    const bool rightGun = m_rightNow == Kind::kGun || m_rightNow == Kind::kGrapple;
    const bool leftGun = m_left.Type == Kind::kGun || m_left.Type == Kind::kGrapple;
    // First person with two guns: the game's animation of the right arm (its idle and fidgets, walking, the draw and
    // holster) carried onto both holds (both follow the game), off for the reload's dip and a
    // consumable's lowering (the right gun's reload clip plays under them); kept on while nothing's out, so a draw starts
    // from it
    const bool follow = firstPerson && rightGun && leftGun && m_pRightIdle;
    const float followTarget = follow && m_reload < 0.f && m_consume < 0.f ? 1.f : 0.f;
    const float followRate = followTarget > m_rightFollow ? kFollowInRate : kFollowOutRate;
    m_rightFollow += (followTarget - m_rightFollow) * std::min(1.f, aDelta * followRate);
    m_leftFollow += (followTarget - m_leftFollow) * std::min(1.f, aDelta * (followTarget > m_leftFollow ? kFollowInRate : kFollowOutRate));
    // a left gun with no gun to follow in the right hand: its own clips (its idle and fidgets, its sprint, its draw and
    // holster)
    const bool sprinting = apPlayer->actorState.IsSprinting();
    // the speed over the ground (a teleport or a load spikes it only for a moment)
    const glm::vec3 position(apPlayer->position.x, apPlayer->position.y, 0.f);
    if (aDelta > 0.f)
    {
        const float speed = std::min(glm::distance(position, m_lastPosition) / aDelta, 2000.f);
        m_speed += (speed - m_speed) * std::min(1.f, aDelta * kSpeedRate);
    }
    m_lastPosition = position;
    DualWieldPose::Motion own{};
    if (firstPerson && shown && leftGun && !follow)
        own = UpdateOwnMotion(aDelta, sprinting);
    else
    {
        m_pOwnClip = m_pIdleClip = nullptr;
        m_ownLast = m_ownFrom = DualWieldPose::Motion{};
        m_idleLoops = 0;
    }

    uint8_t* pRoot = firstPerson ? pFirst : pThird;
    if (m_pPosedRoot && m_pPosedRoot != pRoot)
    {
        DualWieldPose::Clear(m_pPosedRoot);
        m_pPosedRoot = nullptr;
    }
    if ((m_weight <= 0.f && m_gripWeight <= 0.f) || !pRoot)
    {
        if (m_pPosedRoot)
        {
            DualWieldPose::Clear(m_pPosedRoot);
            m_pPosedRoot = nullptr;
        }
        return;
    }

    DualWieldPose::Arms arms{};
    arms.Left = shown || (hanging && hangLeft);
    // the right arm: with two guns (its own animation holds a gun with both hands), on the grapple's cable, and a gun
    // beside a left melee weapon only for the reload's dip (else the game's: its own fire and reload animations). On its
    // way up or down the right gun is the game's own draw or holster (first person carried over at its side, followed)
    const bool twoGuns = shown && rightGun && leftGun && (follow || m_weaponState == kDrawn);
    const bool rightHang = hanging && !hangLeft;
    const float rightTarget = twoGuns || rightHang || (shown && rightGun && (m_reload >= 0.f || m_consume >= 0.f)) ? 1.f : 0.f;
    m_rightWeight += (rightTarget - m_rightWeight) * std::min(1.f, aDelta * kWeightRate);
    if (rightTarget == 0.f && m_rightWeight < 0.01f)
        m_rightWeight = 0.f;
    arms.Right = (shown || hanging) && m_rightWeight > 0.f;
    arms.RightWeight = rightHang ? 1.f : Smooth(m_rightWeight);
    arms.LeftMelee = shown && m_left.Type == Kind::kMelee;
    if (follow)
    {
        arms.pRightIdle = m_pRightIdle;
        arms.RightFollow = Smooth(m_rightFollow);
        arms.LeftFollow = Smooth(m_leftFollow);
        arms.LeftFollowHeld = m_sinceRightShot < kRightShotHold;
    }
    arms.LeftOwn = own;
    // (none while a third person consumable's animation has the hands)
    arms.GripWeight = m_consume >= 0.f && m_consumeThirdPerson ? 0.f : Smooth(m_gripWeight);
    arms.LeftHang = hanging && hangLeft;
    arms.RightHang = hanging && !hangLeft;
    arms.HangTo = anchor;
    GetView(apPlayer, arms.Forward, arms.Right3, arms.Up);
    NiPoint3 eye;
    if (firstPerson && HudMarker::GetCameraPosition(eye))
        arms.Origin = eye;
    else
        arms.Origin = glm::vec3(apPlayer->position) + glm::vec3(0.f, 0.f, 110.f);
    arms.Reload = m_consume < 0.f        ? m_reload
                  : m_consumeThirdPerson ? -1.f
                                         : DualWieldPose::DipPhase(m_consume, kConsumeDown, m_consumeHold, kConsumeUp);
    arms.ReloadDip = m_consume < 0.f && m_reload >= 0.f;
    // third person (no first person clip there): the draw and holster are the dip, coming up and going down
    if (!firstPerson && arms.Reload < 0.f && !(m_left.Type == Kind::kMelee && m_meleeLimbs.pDraw))
    {
        if (m_drawTime >= 0.f)
            arms.Reload = DualWieldPose::DipPhase(m_drawTime, 0.f, 0.f, kDrawSeconds);
        else if (m_holsterTime >= 0.f)
            arms.Reload = DualWieldPose::DipPhase(m_holsterTime, kHolsterSeconds, 1e6f, 0.f);
    }
    arms.pLeftGrip = m_pLeftGrip;
    arms.pLeftGrip3P = m_pLeftGrip3P;
    // a left melee weapon's clip now: its draw's or its holster's, the swing's, the block's, else its idle, blended from
    // the one before
    const WeaponGrips::ArmClip* pClip = nullptr;
    float clipTime = 0.f;
    if (shown && m_left.Type == Kind::kMelee)
    {
        if (m_drawTime >= 0.f && m_meleeClips.pDraw)
        {
            pClip = m_meleeClips.pDraw;
            clipTime = m_drawTime;
        }
        else if (m_holsterTime >= 0.f && m_meleeClips.pHolster)
        {
            pClip = m_meleeClips.pHolster;
            clipTime = m_holsterTime;
        }
        else if (m_swing >= 0.f && m_pSwingClip)
        {
            pClip = m_pSwingClip;
            clipTime = m_swing * m_pSwingClip->Duration;
        }
        else if (m_guardTime > 0.f && m_meleeClips.pBlock)
        {
            pClip = m_meleeClips.pBlock;
            clipTime = m_guardTime;
        }
        else if (sprinting && m_meleeClips.pSprint)
        {
            // its own sprint (the idle stayed up meanwhile, which looked wrong, 2026-10-25)
            pClip = m_meleeClips.pSprint;
            clipTime = std::fmod(m_time, std::max(m_meleeClips.pSprint->Duration, 0.1f));
        }
        else if (m_speed > kMoveSpeed && m_meleeClips.pRun)
        {
            // its own run, walking too (a left melee weapon sways as a right-handed one does; the game's first person
            // melee has no walk clip of its own)
            pClip = m_meleeClips.pRun;
            clipTime = std::fmod(m_time, std::max(m_meleeClips.pRun->Duration, 0.1f));
        }
        else if (m_meleeClips.pIdle)
        {
            pClip = m_meleeClips.pIdle;
            clipTime = std::fmod(m_time, std::max(m_meleeClips.pIdle->Duration, 0.1f));
        }
    }
    if (pClip != m_pShownClip || (pClip && pClip == m_pSwingClip && clipTime < m_shownClipTime))
    {
        m_pClipFrom = m_pShownClip;
        m_clipFromTime = m_shownClipTime;
        m_clipBlend = m_pClipFrom && pClip ? 0.f : 1.f;
    }
    m_clipBlend = std::min(1.f, m_clipBlend + aDelta / kClipBlendSeconds);
    m_pShownClip = pClip;
    m_shownClipTime = clipTime;
    arms.pLeftClip = pClip;
    arms.LeftClipTime = clipTime;
    arms.pLeftClipFrom = m_pClipFrom;
    arms.LeftClipFromTime = m_clipFromTime;
    arms.LeftClipBlend = m_clipBlend;
    arms.LeftKick = m_leftKick * m_leftKick;
    // the right gun's own fire clip kicks it when its arm's animation is followed
    arms.RightKick = m_rightKick * m_rightKick * (1.f - arms.RightFollow);
    arms.Swing = m_swing;
    arms.PowerSwing = m_powerSwing;
    arms.Guard = m_guardWeight;
    // Third person (2026-10-25: left-handed melee attacks the same as the game's right-handed ones, and in the idles the
    // left arm and hand matching what the right arm does): the left arm the
    // right one mirrored, a left melee weapon's attacks and block its own third person clips (the swing and guard of old
    // when they can't be read)
    arms.MirrorRight = !m_onBike && !firstPerson && (m_left.Type != Kind::kMelee || m_meleeLimbs.pAttackA);
    if (arms.MirrorRight)
        SetLimbClip(arms, aDelta);
    // moving, the game's own walk, jog, run or sprint has the arms (the weapons stay in the hands while the game's own
    // jog and sprint play): the pose eases off them, the left weapon kept in the hand by its
    // grip (DualWieldPose with no weight: as under the Pip-Boy)
    const float gameMove = m_onBike ? 0.f : UpdateGameMove(apPlayer, aDelta, sprinting, hanging);
    // the right hand's gun kept on its side, and the left gun aimed at the crosshair (far along the view from the camera)
    // as the right one is, and after its shots, with its button held and hanging from the right hand's grapple
    arms.RightHoldsGun = rightGun;
    if (NiPoint3 camera; HudMarker::GetCameraPosition(camera))
        arms.AimAt = glm::vec3(camera) + glm::normalize(arms.Forward) * kAimDistance;
    const float aimTarget = leftGun && (m_onBike ? m_reload < 0.f : (m_sinceShot < kAimHoldSeconds || m_leftDown || (hanging && !hangLeft))) ? 1.f : 0.f;
    m_leftAim += (aimTarget - m_leftAim) * std::min(1.f, aDelta * kAimRate);
    arms.LeftAim = Smooth(m_leftAim);
    arms.Weight = Smooth(m_weight) * Smooth(m_consumeWeight) * (1.f - gameMove);
    // Riding: only the left arm moves, off the bars to the aim, and the body it hangs from is tilted with the bike after
    // this pose, so where it reaches is worked out against that tilt. Up at the sights it comes in front of the eyes.
    arms.OnBike = m_onBike;
    if (m_onBike)
    {
        float rotation[9];
        float pivot[3];
        if (BikeLink::GetBodyTilt(rotation, pivot))
        {
            // the tilt's inverse (its transpose) about the feet
            const glm::vec3 at(pivot[0], pivot[1], pivot[2]);
            const auto untilt = [&rotation](const glm::vec3& acV)
            {
                return glm::vec3(rotation[0] * acV.x + rotation[3] * acV.y + rotation[6] * acV.z, rotation[1] * acV.x + rotation[4] * acV.y + rotation[7] * acV.z,
                                 rotation[2] * acV.x + rotation[5] * acV.y + rotation[8] * acV.z);
            };
            arms.AimAt = at + untilt(arms.AimAt - at);
            arms.Forward = untilt(arms.Forward);
            arms.Right3 = untilt(arms.Right3);
            arms.Up = untilt(arms.Up);
            BikeLink::Hud hud{};
            if (BikeLink::GetHud(hud) && hud.EyeView)
            {
                if (NiPoint3 camera; HudMarker::GetCameraPosition(camera))
                {
                    arms.BikeSights = at + untilt(glm::vec3(camera) - at) + arms.Forward * kBikeSightsAhead - arms.Up * kBikeSightsBelow;
                    arms.BikeAds = Smooth(m_bikeAdsWeight);
                }
            }
        }
    }
    arms.Weight = m_onBike ? Smooth(m_weight) : arms.Weight;
    DualWieldPose::Set(pRoot, firstPerson, arms);
    m_pPosedRoot = pRoot;
}

void DualWieldService::UpdateZoom(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    // two guns out, in play (the game would show its crosshair: no menu, no VATS), the key held
    const bool twoGuns = m_activeNow && (m_left.Type == Kind::kGun || m_left.Type == Kind::kGrapple) &&
                         (m_rightNow == Kind::kGun || m_rightNow == Kind::kGrapple);
    (void)apPlayer;
    const bool held = twoGuns && GameHud::WouldShow(GameHud::Piece::kCrosshair) && GameHasKeyboard() && (GetAsyncKeyState(m_zoomKey) & 0x8000) != 0;
    const float target = held ? 1.f : 0.f;
    if (m_zoom == target && m_baseFov == 0.f)
        return;
    m_zoom = target > m_zoom ? std::min(target, m_zoom + aDelta * kZoomRate) : std::max(target, m_zoom - aDelta * kZoomRate);

    float* pFov = GetWorldFov();
    if (!pFov)
        return;
    if (m_baseFov == 0.f)
        m_baseFov = *pFov;
    // not what the zoom wrote: the field of view was changed meanwhile (the settings' slider), and that's the one to
    // zoom from and go back to
    else if (*pFov != m_writtenFov)
    {
        spdlog::info("Dual wield: the field of view changed to {:.1f} while zoomed, kept", *pFov);
        m_baseFov = *pFov;
    }
    if (m_zoom <= 0.f)
    {
        *pFov = m_baseFov;
        m_baseFov = 0.f;
        return;
    }
    *pFov = m_baseFov * (1.f - kZoomAmount * Smooth(m_zoom));
    m_writtenFov = *pFov;
}

void DualWieldService::UpdateCrosshair(PlayerCharacter* apPlayer) noexcept
{
    // Two guns: the overlay's crosshair (it closes in again after each shot), the game's hidden
    const bool twoGuns = m_activeNow && m_left.Type == Kind::kGun && (m_rightNow == Kind::kGun || m_rightNow == Kind::kGrapple);
    // (riding, the left hand slot's gun has it too: the game's wouldn't show with nothing of its own drawn)
    const bool bikeGun = m_onBike && m_activeNow && m_left.Type == Kind::kGun;
    const bool shown = bikeGun || (twoGuns && GameHud::WouldShow(GameHud::Piece::kCrosshair));
    GameHud::HideCrosshair(twoGuns || bikeGun);
    (void)apPlayer;

    std::string json;
    if (shown)
        json = fmt::format("{{\"on\":true,\"shots\":{},\"left\":{{\"clip\":{},\"reserve\":{}}}}}", m_shots, m_magazine, m_reserve);
    else
        json = "{\"on\":false}";
    if (json == m_sentCrosshair)
        return;
    m_sentCrosshair = json;
    m_world.ctx().at<OverlayService>().SendEvent("crosshair", json);
}

void DualWieldService::ProbeView(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    // first person only (the jerk was felt there); the eye is the world camera's, the view where its clip matrix looks.
    // Not with the Pip-Boy (or our map's glance) up: the game's own camera bone takes the view down to the wrist (seen
    // 2026-10-24, 16 deg off the look), nor across a load (its first frame's time is the load's)
    ViewProbeState& probe = m_probe;
    NiPoint3 eyePoint;
    if (!NodeTools::IsFirstPerson() || !HudMarker::GetCameraPosition(eyePoint) || !(aDelta > 0.f) || aDelta > 1.f || IsPipboyOpen() ||
        m_world.ctx().at<PipboyMapService>().IsShowing())
    {
        probe.Primed = false;
        return;
    }
    glm::vec3 view{};
    glm::vec3 viewRight{};
    glm::vec3 viewUp{};
    GetView(apPlayer, view, viewRight, viewUp);
    const float pitch = apPlayer->rotation.x;
    const float yaw = apPlayer->rotation.z;
    const glm::vec3 look(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
    const float off = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(view), look), -1.f, 1.f)));
    const glm::vec3 eye = glm::vec3(eyePoint) - glm::vec3(apPlayer->position);
    const uint32_t posed = m_pPosedRoot ? DualWieldPose::TakePoseCount(m_pPosedRoot) : 0;

    // once a minute: that it runs, and the worst frame
    probe.MinuteTime += aDelta;
    ++probe.Frames;
    probe.Slowest = std::max(probe.Slowest, aDelta);
    if (probe.MinuteTime >= 60.f)
    {
        spdlog::info("View probe: {} frames this minute, the slowest {:.1f} ms (usually {:.1f}), the view usually {:.2f} deg off the look, {} odd frames logged", probe.Frames,
                     probe.Slowest * 1000.f, probe.UsualDelta * 1000.f, probe.UsualOff, probe.Logged);
        probe.MinuteTime = 0.f;
        probe.Frames = 0;
        probe.Slowest = 0.f;
        probe.Logged = 0;
    }
    if (!probe.Primed)
    {
        probe.Primed = true;
        probe.UsualDelta = aDelta;
        probe.UsualOff = off;
        probe.UsualTurn = 0.f;
        probe.LastYaw = yaw;
        probe.LastPitch = pitch;
        probe.LastEye = eye;
        return;
    }

    float yawTurn = yaw - probe.LastYaw;
    while (yawTurn > 3.14159265f)
        yawTurn -= 6.2831853f;
    while (yawTurn < -3.14159265f)
        yawTurn += 6.2831853f;
    const float turn = glm::degrees(std::hypot(yawTurn, pitch - probe.LastPitch));
    const float eyeMoved = glm::distance(eye, probe.LastEye);

    std::string what;
    const auto add = [&what](const char* acpWhat)
    {
        if (!what.empty())
            what += ", ";
        what += acpWhat;
    };
    if (aDelta > std::max(0.04f, probe.UsualDelta * 2.5f))
        add("a long frame");
    if (off > probe.UsualOff + 1.5f)
        add("the view off the look");
    if (probe.UsualTurn > 0.05f && turn > std::max(2.f, probe.UsualTurn * 5.f))
        add("a sudden turn");
    if (eyeMoved > 3.f && !GrappleService::HoldsPlayer() && !m_world.ctx().at<ParkourService>().IsPlayerMoving())
        add("the eye moved on the body");
    if (!what.empty() && probe.Logged < 40)
    {
        ++probe.Logged;
        spdlog::info("View probe: {}: frame {:.1f} ms (usually {:.1f}), the view {:.2f} deg off the look (usually {:.2f}), turned {:.2f} deg (usually {:.2f}), the eye "
                     "moved {:.1f} on the body; left hand {}, pose {:.2f} (run {} times), reload {:.2f}, zoom {:.2f}",
                     what, aDelta * 1000.f, probe.UsualDelta * 1000.f, off, probe.UsualOff, turn, probe.UsualTurn, eyeMoved,
                     m_activeNow ? "in use" : (m_left.FormId ? "not in use" : "empty"), m_weight, posed, m_reload, m_zoom);
    }

    // the usual values follow slowly, a spike moving them little
    constexpr float kFollow = 0.05f;
    probe.UsualDelta += (std::min(aDelta, probe.UsualDelta * 3.f + 0.005f) - probe.UsualDelta) * kFollow;
    probe.UsualOff += (std::min(off, probe.UsualOff + 3.f) - probe.UsualOff) * kFollow;
    probe.UsualTurn += (std::min(turn, probe.UsualTurn * 3.f + 1.f) - probe.UsualTurn) * kFollow;
    probe.LastYaw = yaw;
    probe.LastPitch = pitch;
    probe.LastEye = eye;
}

// --- the other players

void DualWieldService::SendState() noexcept
{
    m_sendTimer = kSendInterval;
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;
    RequestMovementAction request{};
    request.Type = RequestMovementAction::kOffHand;
    if (m_left.FormId)
        World::Get().GetModSystem().GetServerModId(m_left.FormId, request.OffHand.Weapon);
    request.OffHand.Mods = ToWire(m_left.Mods);
    transport.Send(request);
}

void DualWieldService::SendCue(uint8_t aCue) noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline() || !m_left.FormId)
        return;
    RequestMovementAction request{};
    request.Type = RequestMovementAction::kOffHandCue;
    World::Get().GetModSystem().GetServerModId(m_left.FormId, request.OffHand.Weapon);
    request.OffHand.Mods = ToWire(m_left.Mods);
    request.OffHand.Cue = aCue;
    transport.Send(request);
}

void DualWieldService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    if (acMessage.Type != RequestMovementAction::kOffHand && acMessage.Type != RequestMovementAction::kOffHandCue)
        return;
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor)
        return;

    auto it = std::find_if(m_remotes.begin(), m_remotes.end(), [pActor](const RemoteHand& acHand) { return acHand.ActorId == pActor->formID; });
    if (it == m_remotes.end())
    {
        RemoteHand hand{};
        hand.ActorId = pActor->formID;
        hand.pModel = std::make_unique<OffHandModel>();
        m_remotes.push_back(std::move(hand));
        it = m_remotes.end() - 1;
    }
    RemoteHand& hand = *it;
    hand.LastSeen = m_time;

    // the weapon, whichever message it came with
    const uint32_t weaponId = acMessage.OffHand.Weapon ? World::Get().GetModSystem().GetGameId(acMessage.OffHand.Weapon) : 0u;
    TESForm* pWeapon = weaponId ? TESForm::GetById(weaponId) : nullptr;
    const Vector<OffHandMod> mods = FromWire(acMessage.OffHand.Mods);
    const bool changed = (pWeapon ? pWeapon->formID : 0u) != hand.Weapon.FormId || !SameMods(mods, hand.Weapon.Mods);
    if (changed && (acMessage.Type == RequestMovementAction::kOffHand || pWeapon))
    {
        if (hand.pRoot)
            DualWieldPose::Clear(hand.pRoot);
        hand.pModel->Reset();
        hand.pRoot = nullptr;
        hand.Weapon = HeldWeapon{};
        hand.Weight = 0.f;
        hand.Guard = false;
        hand.pFireNode = nullptr;
        ReleaseRemoteData(hand.ActorId);
        if (pWeapon && KindOf(pWeapon, nullptr) != 0)
        {
            hand.Weapon.FormId = pWeapon->formID;
            hand.Weapon.Mods = mods;
            hand.Weapon.Type = static_cast<Kind>(KindOf(pWeapon, nullptr));
            hand.pModel->Build(pWeapon, hand.Weapon.Mods, pActor);
            spdlog::info("Dual wield: {:X} has {:X} in the left hand", pActor->formID, pWeapon->formID);
        }
    }

    if (acMessage.Type == RequestMovementAction::kOffHandCue && hand.Weapon.FormId)
    {
        switch (acMessage.OffHand.Cue)
        {
        // the flash comes with the shot's projectile (DualWieldHooks::ShowMuzzleFlash, on the copy's muzzle)
        case RequestMovementAction::kCueFire:
            hand.Kick = 1.f;
            hand.SinceShot = 0.f;
            break;
        case RequestMovementAction::kCueSwing:
        case RequestMovementAction::kCuePowerSwing:
            hand.Swing = 0.f;
            hand.PowerSwing = acMessage.OffHand.Cue == RequestMovementAction::kCuePowerSwing;
            // their two standing attacks in turn, as their own game takes them
            if (!hand.PowerSwing)
                hand.SwingB = !hand.SwingB;
            break;
        case RequestMovementAction::kCueReload: hand.Lower = 0.f; break;
        case RequestMovementAction::kCueGuardOn: hand.Guard = true; break;
        case RequestMovementAction::kCueGuardOff: hand.Guard = false; break;
        default: break;
        }
    }
}

void DualWieldService::UpdateRemotes(float aDelta) noexcept
{
    // the other players pulled along or hanging on their grapple's cable: the gun's arm up it
    const auto hangs = GrappleService::GetRemoteHangs();
    const auto hangOf = [&hangs](uint32_t aActorId) -> const glm::vec3*
    {
        const auto it = std::find_if(hangs.begin(), hangs.end(), [aActorId](const auto& acHang) { return acHang.first == aActorId; });
        return it != hangs.end() ? &it->second : nullptr;
    };

    for (size_t i = 0; i < m_remotes.size();)
    {
        RemoteHand& hand = m_remotes[i];
        auto* pActor = Cast<Actor>(TESForm::GetById(hand.ActorId));
        const bool gone = !pActor || !pActor->parentCell || m_time - hand.LastSeen > kRemoteTimeout || !hand.Weapon.FormId;
        if (gone)
        {
            if (hand.pRoot)
                DualWieldPose::Clear(hand.pRoot);
            hand.pModel->Reset();
            ReleaseRemoteData(hand.ActorId);
            m_remotes[i] = std::move(m_remotes.back());
            m_remotes.pop_back();
            continue;
        }
        ++i;

        hand.pModel->Update();
        uint8_t* pRoot = NodeTools::GetThirdPersonRoot(pActor);
        if (pRoot != hand.pRoot)
        {
            if (hand.pRoot)
            {
                DualWieldPose::Clear(hand.pRoot);
                hand.pModel->DetachFrom(hand.pRoot);
            }
            hand.pRoot = pRoot;
            hand.AttachTimer = 0.f;
        }
        if (!pRoot)
            continue;
        if (hand.pModel->IsReady() && (hand.AttachTimer -= aDelta) <= 0.f)
        {
            hand.AttachTimer = kAttachInterval;
            hand.pModel->AttachTo(pRoot);
        }

        // their draw and holster (the weapon state as their copy has it): the reload's dip coming up and going down
        const uint32_t state = pActor->actorState.GetWeaponState();
        if (state != hand.WeaponState)
        {
            const bool out = state >= kWantToDraw && state <= kDrawn;
            const bool wasOut = hand.WeaponState >= kWantToDraw && hand.WeaponState <= kDrawn;
            if (out && !wasOut)
            {
                hand.Draw = 0.f;
                hand.Holster = -1.f;
            }
            else if (!out && wasOut)
            {
                hand.Holster = 0.f;
                hand.Draw = -1.f;
            }
            hand.WeaponState = state;
        }
        // (a melee weapon's own draw and holster as long as its clips)
        const bool ownClips = hand.Weapon.Type == Kind::kMelee && hand.LimbsFor == hand.Weapon.FormId;
        const float drawLength = ownClips && hand.Limbs.pDraw ? hand.Limbs.pDraw->Duration : kDrawSeconds;
        const float holsterLength = ownClips && hand.Limbs.pHolster ? hand.Limbs.pHolster->Duration : kHolsterSeconds;
        if (hand.Draw >= 0.f && (hand.Draw += aDelta) >= drawLength)
            hand.Draw = -1.f;
        if (hand.Holster >= 0.f && (hand.Holster += aDelta) >= holsterLength && state == kSheathed)
            hand.Holster = -1.f;
        const bool drawn = (state != kSheathed || hand.Holster >= 0.f) && !pActor->IsDead() && !pActor->IsInPowerArmor();
        hand.pModel->SetShown(drawn);
        // their left gun's weapon data on this copy's muzzle (their shots' muzzle flash), the flash run every frame
        if (drawn && hand.Weapon.Type == Kind::kGun && hand.pModel->IsReady())
            PrepareRemoteData(hand, pActor, pRoot);
        void* pFlash = nullptr;
        {
            std::scoped_lock lock(m_leftDataLock);
            for (const auto& data : m_remoteData)
            {
                if (data.ActorId == hand.ActorId && data.pData)
                    pFlash = Read<void*>(data.pData, kDataMuzzleFlashOffset);
            }
        }
        if (pFlash)
        {
            using TUpdateMuzzleFlash = void(void*, float, Actor*);
            POINTER_FALLOUT4(TUpdateMuzzleFlash, s_updateMuzzleFlash, 447432);
            s_updateMuzzleFlash.Get()(pFlash, GameFrameTime(), pActor);
        }
        const float target = drawn && hand.pModel->IsReady() ? 1.f : 0.f;
        hand.Weight += (target - hand.Weight) * std::min(1.f, aDelta * kWeightRate);
        hand.Kick = std::max(0.f, hand.Kick - aDelta / kKickSeconds);
        // the swing as long as its third person clip (their weapon's speed isn't sent), else the swing of old
        const WeaponGrips::LimbClip* pSwingLimb = hand.PowerSwing ? hand.Limbs.pPower : hand.SwingB ? hand.Limbs.pAttackB : hand.Limbs.pAttackA;
        const float swingSeconds = pSwingLimb && hand.LimbsFor == hand.Weapon.FormId ? pSwingLimb->Duration : kSwingSeconds * (hand.PowerSwing ? kPowerSwingStretch : 1.f);
        if (hand.Swing >= 0.f && (hand.Swing += aDelta / std::max(swingSeconds, 0.1f)) >= 1.f)
            hand.Swing = -1.f;
        const bool guard = hand.Guard && drawn && hand.Weapon.Type == Kind::kMelee;
        hand.GuardWeight = guard ? std::min(1.f, hand.GuardWeight + aDelta * kGuardRate) : std::max(0.f, hand.GuardWeight - aDelta * kGuardRate);
        if (hand.Lower >= 0.f && (hand.Lower += aDelta / kRemoteReloadSeconds) >= 1.f)
            hand.Lower = -1.f;

        if (hand.Weight < 0.01f)
        {
            DualWieldPose::Clear(pRoot);
            continue;
        }
        DualWieldPose::Arms arms{};
        arms.Left = true;
        const uint8_t right = KindOf(pActor->GetEquippedWeapon(0), nullptr);
        // their right arm as ours: two guns, or their gun beside a melee weapon for the reload
        const bool rightGun = right == 1 || right == 3;
        const bool leftGun = hand.Weapon.Type == Kind::kGun || hand.Weapon.Type == Kind::kGrapple;
        // (on its way up or down their right gun is the game's own draw or holster)
        const float rightTarget = rightGun && ((leftGun && state == kDrawn) || hand.Lower >= 0.f) ? 1.f : 0.f;
        hand.RightWeight += (rightTarget - hand.RightWeight) * std::min(1.f, aDelta * kWeightRate);
        if (rightTarget == 0.f && hand.RightWeight < 0.01f)
            hand.RightWeight = 0.f;
        arms.Right = hand.RightWeight > 0.f;
        arms.RightWeight = Smooth(hand.RightWeight);
        arms.LeftMelee = hand.Weapon.Type == Kind::kMelee;
        if (const glm::vec3* pHang = hangOf(hand.ActorId))
        {
            arms.RightHang = right == 3;
            arms.LeftHang = !arms.RightHang && hand.Weapon.Type == Kind::kGrapple;
            arms.Right = arms.Right || arms.RightHang;
            if (arms.RightHang)
                arms.RightWeight = 1.f;
            arms.HangTo = *pHang;
        }
        const float yaw = pActor->rotation.z;
        const float pitch = pActor->rotation.x;
        arms.Forward = glm::vec3(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
        arms.Right3 = glm::vec3(std::cos(yaw), -std::sin(yaw), 0.f);
        arms.Up = glm::normalize(glm::cross(arms.Right3, arms.Forward));
        arms.Origin = glm::vec3(pActor->position) + glm::vec3(0.f, 0.f, 110.f);
        arms.Reload = hand.Lower >= 0.f   ? hand.Lower
                      : hand.Draw >= 0.f  ? DualWieldPose::DipPhase(hand.Draw, 0.f, 0.f, kDrawSeconds)
                      : hand.Holster >= 0.f ? DualWieldPose::DipPhase(hand.Holster, kHolsterSeconds, 1e6f, 0.f)
                                            : -1.f;
        // (their reload's cue)
        arms.ReloadDip = hand.Lower >= 0.f;
        // their left weapon's grip, from its own keywords (their mods' changes aren't sent)
        const TESForm* pTheirLeft = TESForm::GetById(hand.Weapon.FormId);
        arms.pLeftGrip = pTheirLeft ? WeaponGrips::ForKeywords(KeywordsOf(pTheirLeft, nullptr), hand.Weapon.Type == Kind::kMelee) : nullptr;
        arms.pLeftGrip3P = pTheirLeft && hand.Weapon.Type == Kind::kMelee ? WeaponGrips::ThirdPersonGripFor(KeywordsOf(pTheirLeft, nullptr), true) : nullptr;
        // their left arm their right one mirrored, a melee weapon's swings and block its own third person clips (as ours)
        if (hand.Weapon.Type == Kind::kMelee && hand.LimbsFor != hand.Weapon.FormId)
        {
            hand.Limbs = ThirdPersonMeleeClips(pTheirLeft ? KeywordsOf(pTheirLeft, nullptr) : nullptr);
            hand.LimbsFor = hand.Weapon.FormId;
        }
        arms.MirrorRight = hand.Weapon.Type != Kind::kMelee || hand.Limbs.pAttackA;
        // their right gun on its side, their left gun aimed where they look as their right one is, and after its shots
        arms.RightHoldsGun = rightGun;
        arms.AimAt = arms.Origin + arms.Forward * kAimDistance;
        hand.SinceShot += aDelta;
        const float aimTarget = leftGun && (hand.SinceShot < kAimHoldSeconds || hangOf(hand.ActorId)) ? 1.f : 0.f;
        hand.LeftAim += (aimTarget - hand.LeftAim) * std::min(1.f, aDelta * kAimRate);
        arms.LeftAim = Smooth(hand.LeftAim);
        // moving, the game's own walk, jog, run or sprint has their arms (as ours: their gun's relaxed state comes with
        // their game's Action Gun Relaxed), not while they use a weapon or hang from the cable
        const glm::vec3 position(pActor->position.x, pActor->position.y, 0.f);
        if (aDelta > 0.f)
            hand.Speed += (std::min(glm::distance(position, hand.LastPosition) / aDelta, 2000.f) - hand.Speed) * std::min(1.f, aDelta * kSpeedRate);
        hand.LastPosition = position;
        const bool sprinting = pActor->actorState.IsSprinting() || hand.Speed > kSprintSpeed;
        const bool inUse = pActor->actorState.GetMeleeAttackState() != 0 || state != kDrawn || hand.Lower >= 0.f || hand.Draw >= 0.f || hand.Holster >= 0.f ||
                           hand.Swing >= 0.f || hand.GuardWeight > 0.f || hand.SinceShot < kAimHoldSeconds || hangOf(hand.ActorId);
        const float moveTarget = !inUse && GameMovesArms(pActor, rightGun, hand.Speed, sprinting) ? 1.f : 0.f;
        hand.GameMove += (moveTarget - hand.GameMove) * std::min(1.f, aDelta * kWeightRate);
        if (arms.MirrorRight && hand.Weapon.Type == Kind::kMelee)
        {
            // its own idle (as ours)
            if (hand.Limbs.pIdle)
            {
                arms.pLimbIdle = hand.Limbs.pIdle;
                arms.LimbIdleTime = std::fmod(m_time, std::max(hand.Limbs.pIdle->Duration, 0.1f));
            }
            // their draw and holster its own (not the dip)
            const WeaponGrips::LimbClip* pLimb = nullptr;
            float limbTime = 0.f;
            if (hand.Draw >= 0.f && hand.Limbs.pDraw)
            {
                pLimb = hand.Limbs.pDraw;
                limbTime = hand.Draw;
                arms.Reload = hand.Lower;
            }
            else if (hand.Holster >= 0.f && hand.Limbs.pHolster)
            {
                pLimb = hand.Limbs.pHolster;
                limbTime = hand.Holster;
                arms.Reload = hand.Lower;
            }
            else if (hand.Swing >= 0.f)
            {
                pLimb = hand.PowerSwing ? hand.Limbs.pPower : hand.SwingB ? hand.Limbs.pAttackB : hand.Limbs.pAttackA;
                limbTime = pLimb ? std::clamp(hand.Swing, 0.f, 1.f) * pLimb->Duration : 0.f;
            }
            else if (hand.GuardWeight > 0.f && hand.Limbs.pBlock)
            {
                pLimb = hand.Limbs.pBlock;
                limbTime = hand.GuardWeight * std::min(pLimb->EventTime("attackWinStart", pLimb->Duration * 0.45f), pLimb->Duration);
            }
            const float limbTarget = pLimb ? 1.f : 0.f;
            hand.LimbWeight += (limbTarget - hand.LimbWeight) * std::min(1.f, aDelta * (pLimb ? kLimbInRate : kLimbOutRate));
            // eased out from where the last one ended
            if (pLimb)
            {
                hand.pLastLimb = pLimb;
                hand.LastLimbTime = limbTime;
            }
            arms.pLimbClip = hand.pLastLimb;
            arms.LimbMovesBody = hand.pLastLimb != hand.Limbs.pDraw && hand.pLastLimb != hand.Limbs.pHolster;
            arms.LimbTime = hand.LastLimbTime;
            arms.LimbWeight = Smooth(hand.LimbWeight);
        }
        arms.LeftKick = hand.Kick * hand.Kick;
        arms.Swing = hand.Swing;
        arms.PowerSwing = hand.PowerSwing;
        arms.Guard = hand.GuardWeight;
        arms.Weight = Smooth(hand.Weight) * (1.f - Smooth(hand.GameMove));
        DualWieldPose::Set(pRoot, false, arms);
    }

    // the hanging players with nothing in the left hand: their right arm, eased in and out
    for (const auto& [actorId, spike] : hangs)
    {
        const bool posedAbove = std::any_of(m_remotes.begin(), m_remotes.end(), [actorId = actorId](const RemoteHand& acHand) { return acHand.ActorId == actorId; });
        if (!posedAbove && std::none_of(m_remoteHangs.begin(), m_remoteHangs.end(), [actorId = actorId](const RemoteHang& acHang) { return acHang.ActorId == actorId; }))
            m_remoteHangs.push_back({actorId, nullptr, 0.f});
    }
    for (size_t i = 0; i < m_remoteHangs.size();)
    {
        RemoteHang& hang = m_remoteHangs[i];
        auto* pActor = Cast<Actor>(TESForm::GetById(hang.ActorId));
        uint8_t* pRoot = pActor && pActor->parentCell ? NodeTools::GetThirdPersonRoot(pActor) : nullptr;
        const glm::vec3* pSpike = hangOf(hang.ActorId);
        const bool posedAbove = std::any_of(m_remotes.begin(), m_remotes.end(), [&hang](const RemoteHand& acHand) { return acHand.ActorId == hang.ActorId; });
        const float target = pSpike && pRoot && !posedAbove ? 1.f : 0.f;
        hang.Weight += (target - hang.Weight) * std::min(1.f, aDelta * kWeightRate);
        if (hang.pRoot && hang.pRoot != pRoot)
            DualWieldPose::Clear(std::exchange(hang.pRoot, nullptr));
        if (posedAbove || !pRoot || (target == 0.f && hang.Weight < 0.01f))
        {
            if (hang.pRoot && !posedAbove)
                DualWieldPose::Clear(hang.pRoot);
            m_remoteHangs[i] = m_remoteHangs.back();
            m_remoteHangs.pop_back();
            continue;
        }
        ++i;
        if (pSpike)
            hang.Spike = *pSpike;
        DualWieldPose::Arms arms{};
        arms.Right = true;
        arms.RightHang = true;
        arms.HangTo = hang.Spike;
        const float yaw = pActor->rotation.z;
        const float pitch = pActor->rotation.x;
        arms.Forward = glm::vec3(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
        arms.Right3 = glm::vec3(std::cos(yaw), -std::sin(yaw), 0.f);
        arms.Up = glm::normalize(glm::cross(arms.Right3, arms.Forward));
        arms.Origin = glm::vec3(pActor->position) + glm::vec3(0.f, 0.f, 110.f);
        arms.Weight = Smooth(hang.Weight);
        DualWieldPose::Set(pRoot, false, arms);
        hang.pRoot = pRoot;
    }
}

#endif
