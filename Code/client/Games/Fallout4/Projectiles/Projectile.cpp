#include "Projectile.h"
#include <World.h>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <Events/ProjectileLaunchedEvent.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESAmmo.h>
#include <Services/GrappleService.h>
#include <Services/DualWieldService.h>
#include <Services/Vats76Service.h>

TP_THIS_FUNCTION(TLaunch, BSPointerHandle<Projectile>*, BSPointerHandle<Projectile>, ProjectileLaunchData& arData);

static TLaunch* RealLaunch = nullptr;

BSPointerHandle<Projectile>* Projectile::Launch(BSPointerHandle<Projectile>* apResult, ProjectileLaunchData& arData) noexcept
{
    return TiltedPhoques::ThisCall(RealLaunch, apResult, arData);
}

BSPointerHandle<Projectile>* TP_MAKE_THISCALL(HookLaunch, BSPointerHandle<Projectile>, ProjectileLaunchData& arData)
{
    // The local player's grappling gun spike: GrappleService flies the gun's own spike on its cable instead (nothing is
    // launched, and nothing is sent to the other players)
    if (GrappleService::TakeLaunch(arData))
    {
        apThis->handle.iBits = 0;
        return apThis;
    }
    if (arData.pShooter)
    {
        Actor* pActor = Cast<Actor>(arData.pShooter);
        if (pActor)
        {
            ActorExtension* pExtendedActor = pActor->GetExtension();
            if (pExtendedActor->IsRemote())
            {
                apThis->handle.iBits = 0;
                return apThis;
            }
        }
    }

    // RD VATS 76': before it's told to the other players, so that they see the shot go where it goes
    Vats76Service::AimLaunch(arData);

    ProjectileLaunchedEvent Event{};
    Event.Origin = arData.Origin;
    if (arData.pProjectileBase)
        Event.ProjectileBaseID = arData.pProjectileBase->formID;
    Event.ZAngle = arData.fZAngle;
    Event.XAngle = arData.fXAngle;
    Event.YAngle = arData.fYAngle;
    if (arData.pShooter)
        Event.ShooterID = arData.pShooter->formID;
    if (arData.pParentCell)
        Event.ParentCellID = arData.pParentCell->formID;
    if (arData.pFromAmmo)
        Event.AmmoID = arData.pFromAmmo->formID;
    Event.Area = arData.iArea;
    Event.Power = arData.fPower;
    Event.Scale = arData.fScale;
    Event.AlwaysHit = arData.bAlwaysHit;
    Event.ConeOfFireRadiusMult = arData.fConeOfFireRadiusMult;
    Event.NoDamageOutsideCombat = arData.bNoDamageOutsideCombat;
    Event.AutoAim = arData.bAutoAim;
    Event.DeferInitialization = arData.bDeferInitialization;
    Event.Tracer = arData.bTracer;
    Event.ForceConeOfFire = arData.bForceConeOfFire;
    Event.IntentionalMiss = arData.bIntentionalMiss;
    Event.Allow3D = arData.bAllow3D;
    Event.Penetrates = arData.bPenetrates;
    Event.IgnoreNearCollisions = arData.bIgnoreNearCollisions;
    Event.OffHand = DualWieldService::IsLaunchingLeft();

    World::Get().GetRunner().Trigger(Event);

    auto* pResult = TiltedPhoques::ThisCall(RealLaunch, apThis, arData);
    // a RD VATS 76' critical strike's projectile carries the game's own critical flag
    Vats76Service::OnLaunched(pResult ? pResult->handle.iBits : 0u);
    // the local player's shots kick the dual wield crosshair; the left gun's are remembered for its muzzle flash
    DualWieldService::OnLaunch(arData, pResult ? pResult->handle.iBits : 0u);
    return pResult;
}

BSPointerHandle<Projectile>* Projectile::LaunchShared(BSPointerHandle<Projectile>* apResult, ProjectileLaunchData& arData) noexcept
{
    return HookLaunch(apResult, arData);
}

TP_THIS_FUNCTION(TFire, void, void, TESObjectREFR* apSource, uint32_t aEquipIndex, TESAmmo* apAmmo, AlchemyItem* apPoison);
static TFire* RealFire = nullptr;

void TP_MAKE_THISCALL(HookFire, void, TESObjectREFR* apSource, uint32_t aEquipIndex, TESAmmo* apAmmo, AlchemyItem* apPoison)
{
    if (apSource)
    {
        Actor* pActor = Cast<Actor>(apSource);
        if (pActor)
        {
            ActorExtension* pExtension = pActor->GetExtension();
            if (pExtension)
            {
                if (pExtension->IsRemote())
                    return;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealFire, apThis, apSource, aEquipIndex, apAmmo, apPoison);
}

// TESObjectWEAP::Fire (CommonLibF4 1056037) keeps every projectile its launches give back in a list, then reads the
// first one's flags (0x260, bit 20) for the shot count it passes to Actor::UseAmmo. It never expects a launch to give
// nothing back, as the grapple's taken launch does (HookLaunch): its two instructions there (Fire+0x5EC, 9 bytes) become
// a call to this stub, which reads the flags when there is a projectile and gives none set when there isn't, so the
// taken shot counts as fired and its spike is used as any gun's ammo.
#pragma section(".rdfire", read, execute)
__declspec(allocate(".rdfire")) static const uint8_t kFireFlagsStub[] = {
    0x48, 0x8B, 0x03,                   // mov rax, [rbx]        the list's first projectile
    0x48, 0x85, 0xC0,                   // test rax, rax
    0x74, 0x07,                         // jz none
    0x8B, 0x88, 0x60, 0x02, 0x00, 0x00, // mov ecx, [rax+260h]   its flags
    0xC3,                               // ret
    0x31, 0xC9,                         // none: xor ecx, ecx
    0xC3,                               // ret
};
constexpr uintptr_t kFireFlagsRead = 0x5EC;
constexpr uint8_t kFireFlagsReadBytes[] = {0x48, 0x8B, 0x03, 0x8B, 0x88, 0x60, 0x02, 0x00, 0x00};

static TiltedPhoques::Initializer s_projectileHooks(
    []()
    {
        POINTER_FALLOUT4(TLaunch, s_launch, 1452335);
        POINTER_FALLOUT4(TFire, fire, 1056038);

        RealLaunch = s_launch.Get();
        RealFire = fire.Get();

        uint8_t* pSite = RealFire ? reinterpret_cast<uint8_t*>(RealFire) + kFireFlagsRead : nullptr;
        const intptr_t reach = reinterpret_cast<intptr_t>(kFireFlagsStub) - reinterpret_cast<intptr_t>(pSite + 5);
        if (pSite && std::memcmp(pSite, kFireFlagsReadBytes, sizeof(kFireFlagsReadBytes)) == 0 && reach == static_cast<int32_t>(reach))
        {
            TiltedPhoques::Put<uint8_t>(pSite, 0xE8);
            TiltedPhoques::Put<int32_t>(pSite + 1, static_cast<int32_t>(reach));
            TiltedPhoques::Nop(pSite + 5, sizeof(kFireFlagsReadBytes) - 5);
        }
        else
            spdlog::warn("Projectiles: the weapon's Fire isn't the expected one (another mod patched it?), a grappling gun shot would end the game");

        TP_HOOK(&RealLaunch, HookLaunch);
        TP_HOOK(&RealFire, HookFire);
    });
