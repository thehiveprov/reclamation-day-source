#if TP_FALLOUT4

#include <Games/Fallout4/BikeLink.h>

namespace
{
constexpr int32_t kApiVersion = 4;

struct Functions
{
    bool (*IsOnBike)(){nullptr};
    bool (*IsRiding)(){nullptr};
    bool (*GetHud)(BikeLink::Hud*){nullptr};
    bool (*GetNetState)(BikeLink::NetState*){nullptr};
    bool (*GetParked)(float*, float*, float*, uint32_t*, uint8_t*){nullptr};
    bool (*GetBodyTilt)(float*, float*){nullptr};
    void (*SetViewHeld)(bool){nullptr};
    void (*Bail)(){nullptr};
    void (*SetAttackInput)(bool){nullptr};
    void (*SetAimZoom)(float){nullptr};
    void (*SetDog)(uint32_t){nullptr};
    int32_t (*TruckCount)(){nullptr};
    bool (*GetTruck)(int32_t, BikeLink::Truck*){nullptr};
    bool (*GetTruckSeat)(int32_t, int32_t, float*, float*){nullptr};
    void (*SetTruckHold)(int32_t, bool){nullptr};
    void (*SetCaravansOpen)(bool){nullptr}; // optional: plugins from before 11-06 lack it
    void (*SetTruckLost)(int32_t, float){nullptr};
    void (*TakeTruck)(int32_t, float){nullptr};
    void (*DamageTruckTo)(int32_t, int32_t){nullptr};
    bool (*TruckTestMode)(){nullptr};
    bool (*GetTruckRoad)(int32_t, float, float, float*, float*){nullptr};
    void (*PullTruckOver)(int32_t, bool){nullptr};
    void (*SnapTruckOver)(int32_t){nullptr};
    void (*SetTruckCrewed)(int32_t, bool){nullptr};
    bool (*TakeTruckPlunder)(int32_t){nullptr};
    bool (*StartRider)(uint32_t, int32_t, float, float, uint8_t){nullptr};
    void (*SetRiderGoal)(uint32_t, float, float){nullptr};
    void (*SetRiderAim)(uint32_t, bool){nullptr};
    void (*RiderGetOff)(uint32_t, int32_t){nullptr};
    bool (*GetRider)(uint32_t, BikeLink::Rider*){nullptr};
    bool (*GetRiderNet)(uint32_t, BikeLink::NetState*){nullptr};
    void (*ForgetRider)(uint32_t){nullptr};
    void (*PlaceRaiderBike)(float, float, float, float, uint8_t){nullptr};
    void (*SetTrader)(uint32_t, int32_t){nullptr};
    bool (*TakeTraded)(){nullptr};
    void (*WatchHits)(uint32_t, bool){nullptr};
    uint32_t (*TakeHit)(uint32_t){nullptr};
    uint32_t (*TruckCrewBase)(int32_t){nullptr};
    void (*PlaySeatedIdle)(uint32_t){nullptr};
    void (*PlayFloorSit)(uint32_t){nullptr};
    const char* (*TruckHome)(int32_t){nullptr};
    uint32_t (*TruckHomeWorkshop)(int32_t){nullptr};
    int32_t (*PickRaidedCaravan)(float, float, float, float, float){nullptr};
    void (*EndRaidedCaravan)(int32_t){nullptr};
    uint32_t (*PlaceMarker)(float, float, float){nullptr};
    void (*RemoveMarker)(uint32_t){nullptr};
    void (*GiveCaps)(int32_t){nullptr};
    uint32_t (*CaravanQuest)(){nullptr};
    void (*Notify)(const char*){nullptr};
    void (*SetRemotePlayerTest)(bool (*)(uint32_t)){nullptr};
    void (*SetRemote)(uint32_t, const BikeLink::NetState*){nullptr};
    void (*SetRemoteParked)(uint32_t, float, float, float, float, float, uint32_t, uint8_t, bool){nullptr};
    bool Present{false};
};

const Functions& Link() noexcept
{
    static const Functions s_functions = []
    {
        Functions functions{};
        const HMODULE hPlugin = GetModuleHandleA("RDVehicles.dll");
        if (!hPlugin)
            return functions;
        using TVersion = int32_t (*)();
        const auto version = reinterpret_cast<TVersion>(GetProcAddress(hPlugin, "RDV_Version"));
        if (!version || version() != kApiVersion)
        {
            spdlog::warn("Motorcycle: RDVehicles.dll is another version ({}), no link to it", version ? version() : 0);
            return functions;
        }
        const auto get = [hPlugin](auto& aFunction, const char* acpName)
        {
            aFunction = reinterpret_cast<std::remove_reference_t<decltype(aFunction)>>(GetProcAddress(hPlugin, acpName));
            return aFunction != nullptr;
        };
        const bool all = get(functions.IsOnBike, "RDV_IsOnBike") & get(functions.IsRiding, "RDV_IsRiding") & get(functions.GetHud, "RDV_GetHud") &
                         get(functions.GetNetState, "RDV_GetNetState") & get(functions.GetParked, "RDV_GetParked") & get(functions.GetBodyTilt, "RDV_GetBodyTilt") &
                         get(functions.SetViewHeld, "RDV_SetViewHeld") & get(functions.Bail, "RDV_Bail") & get(functions.SetAttackInput, "RDV_SetAttackInput") &
                         get(functions.SetAimZoom, "RDV_SetAimZoom") & get(functions.SetDog, "RDV_SetDog") & get(functions.SetRemotePlayerTest, "RDV_SetRemotePlayerTest") &
                         get(functions.SetRemote, "RDV_SetRemote") & get(functions.SetRemoteParked, "RDV_SetRemoteParked") & get(functions.TruckCount, "RDV_TruckCount") &
                         get(functions.GetTruck, "RDV_GetTruck") & get(functions.GetTruckSeat, "RDV_GetTruckSeat") & get(functions.SetTruckHold, "RDV_SetTruckHold") &
                         get(functions.SetTruckLost, "RDV_SetTruckLost") & get(functions.TakeTruck, "RDV_TakeTruck") & get(functions.DamageTruckTo, "RDV_DamageTruckTo") & get(functions.TruckTestMode, "RDV_TruckTestMode") & get(functions.GetTruckRoad, "RDV_GetTruckRoad") &
                         get(functions.PullTruckOver, "RDV_PullTruckOver") & get(functions.SnapTruckOver, "RDV_SnapTruckOver") & get(functions.SetTruckCrewed, "RDV_SetTruckCrewed") &
                         get(functions.TakeTruckPlunder, "RDV_TakeTruckPlunder") & get(functions.StartRider, "RDV_StartRider") & get(functions.SetRiderGoal, "RDV_SetRiderGoal") &
                         get(functions.SetRiderAim, "RDV_SetRiderAim") & get(functions.RiderGetOff, "RDV_RiderGetOff") & get(functions.GetRider, "RDV_GetRider") &
                         get(functions.GetRiderNet, "RDV_GetRiderNet") & get(functions.ForgetRider, "RDV_ForgetRider") & get(functions.PlaceRaiderBike, "RDV_PlaceRaiderBike") &
                         get(functions.SetTrader, "RDV_SetTrader") & get(functions.TakeTraded, "RDV_TakeTraded") & get(functions.WatchHits, "RDV_WatchHits") &
                         get(functions.TakeHit, "RDV_TakeHit") & get(functions.TruckCrewBase, "RDV_TruckCrewBase") & get(functions.PlaySeatedIdle, "RDV_PlaySeatedIdle") & get(functions.PlayFloorSit, "RDV_PlayFloorSit") &
                         get(functions.TruckHome, "RDV_TruckHome") & get(functions.TruckHomeWorkshop, "RDV_TruckHomeWorkshop") & get(functions.PickRaidedCaravan, "RDV_PickRaidedCaravan") &
                         get(functions.EndRaidedCaravan, "RDV_EndRaidedCaravan") & get(functions.PlaceMarker, "RDV_PlaceMarker") & get(functions.RemoveMarker, "RDV_RemoveMarker") &
                         get(functions.GiveCaps, "RDV_GiveCaps") & get(functions.CaravanQuest, "RDV_CaravanQuest") & get(functions.Notify, "RDV_Notify");
        // Optional, not part of the API version: caravans on the roads at all (level 5 or Diamond City, the party's best)
        get(functions.SetCaravansOpen, "RDV_SetCaravansOpen");
        functions.Present = all;
        spdlog::info("Motorcycle: {}", all ? "linked to RDVehicles.dll" : "RDVehicles.dll lacks some of its calls, no link to it");
        return functions;
    }();
    return s_functions;
}
} // namespace

namespace BikeLink
{
bool IsPresent() noexcept
{
    return Link().Present;
}

int32_t TruckCount() noexcept
{
    return Link().Present ? Link().TruckCount() : 0;
}

bool GetTruck(int32_t aIndex, Truck& aOut) noexcept
{
    return Link().Present && Link().GetTruck(aIndex, &aOut);
}

bool GetTruckSeat(int32_t aIndex, int32_t aSeat, float aAt[3], float& aHeading) noexcept
{
    return Link().Present && Link().GetTruckSeat(aIndex, aSeat, aAt, &aHeading);
}

void SetTruckHold(int32_t aIndex, bool aHold) noexcept
{
    if (Link().Present)
        Link().SetTruckHold(aIndex, aHold);
}

void SetCaravansOpen(bool aOpen) noexcept
{
    if (Link().Present && Link().SetCaravansOpen)
        Link().SetCaravansOpen(aOpen);
}

void SetTruckLost(int32_t aIndex, float aGameDays) noexcept
{
    if (Link().Present)
        Link().SetTruckLost(aIndex, aGameDays);
}

void TakeTruck(int32_t aIndex, float aGameDays) noexcept
{
    if (Link().Present)
        Link().TakeTruck(aIndex, aGameDays);
}

void DamageTruckTo(int32_t aIndex, int32_t aHow) noexcept
{
    if (Link().Present)
        Link().DamageTruckTo(aIndex, aHow);
}

bool TruckTestMode() noexcept
{
    return Link().Present && Link().TruckTestMode();
}

bool GetTruckRoad(int32_t aIndex, float aAhead, float aSide, float aAt[3], float& aHeading) noexcept
{
    return Link().Present && Link().GetTruckRoad(aIndex, aAhead, aSide, aAt, &aHeading);
}

void PullTruckOver(int32_t aIndex, bool aOn) noexcept
{
    if (Link().Present)
        Link().PullTruckOver(aIndex, aOn);
}

void SnapTruckOver(int32_t aIndex) noexcept
{
    if (Link().Present)
        Link().SnapTruckOver(aIndex);
}

void SetTruckCrewed(int32_t aIndex, bool aCrewed) noexcept
{
    if (Link().Present)
        Link().SetTruckCrewed(aIndex, aCrewed);
}

bool TakeTruckPlunder(int32_t aIndex) noexcept
{
    return Link().Present && Link().TakeTruckPlunder(aIndex);
}

bool StartRider(uint32_t aActorId, int32_t aIndex, float aAhead, float aSide, uint8_t aPaint) noexcept
{
    return Link().Present && Link().StartRider(aActorId, aIndex, aAhead, aSide, aPaint);
}

void SetRiderGoal(uint32_t aActorId, float aAhead, float aSide) noexcept
{
    if (Link().Present)
        Link().SetRiderGoal(aActorId, aAhead, aSide);
}

void SetRiderAim(uint32_t aActorId, bool aAiming) noexcept
{
    if (Link().Present)
        Link().SetRiderAim(aActorId, aAiming);
}

void RiderGetOff(uint32_t aActorId, int32_t aSide) noexcept
{
    if (Link().Present)
        Link().RiderGetOff(aActorId, aSide);
}

bool GetRider(uint32_t aActorId, Rider& aOut) noexcept
{
    return Link().Present && Link().GetRider(aActorId, &aOut);
}

bool GetRiderNet(uint32_t aActorId, NetState& aOut) noexcept
{
    return Link().Present && Link().GetRiderNet(aActorId, &aOut);
}

void ForgetRider(uint32_t aActorId) noexcept
{
    if (Link().Present)
        Link().ForgetRider(aActorId);
}

void PlaceRaiderBike(float aX, float aY, float aZ, float aHeading, uint8_t aPaint) noexcept
{
    if (Link().Present)
        Link().PlaceRaiderBike(aX, aY, aZ, aHeading, aPaint);
}

void SetTrader(uint32_t aActorId, int32_t aIndex) noexcept
{
    if (Link().Present)
        Link().SetTrader(aActorId, aIndex);
}

bool TakeTraded() noexcept
{
    return Link().Present && Link().TakeTraded();
}

void WatchHits(uint32_t aActorId, bool aWatch) noexcept
{
    if (Link().Present)
        Link().WatchHits(aActorId, aWatch);
}

uint32_t TakeHit(uint32_t aActorId) noexcept
{
    return Link().Present ? Link().TakeHit(aActorId) : 0u;
}

uint32_t TruckCrewBase(int32_t aWhich) noexcept
{
    return Link().Present ? Link().TruckCrewBase(aWhich) : 0u;
}

void PlaySeatedIdle(uint32_t aActorId) noexcept
{
    if (Link().Present)
        Link().PlaySeatedIdle(aActorId);
}

void PlayFloorSit(uint32_t aActorId) noexcept
{
    if (Link().Present)
        Link().PlayFloorSit(aActorId);
}

const char* TruckHome(int32_t aIndex) noexcept
{
    return Link().Present ? Link().TruckHome(aIndex) : "";
}

uint32_t TruckHomeWorkshop(int32_t aIndex) noexcept
{
    return Link().Present ? Link().TruckHomeWorkshop(aIndex) : 0u;
}

int32_t PickRaidedCaravan(const float aFrom[3], float aLeast, float aMost) noexcept
{
    return Link().Present ? Link().PickRaidedCaravan(aFrom[0], aFrom[1], aFrom[2], aLeast, aMost) : -1;
}

void EndRaidedCaravan(int32_t aIndex) noexcept
{
    if (Link().Present)
        Link().EndRaidedCaravan(aIndex);
}

uint32_t PlaceMarker(const float aAt[3]) noexcept
{
    return Link().Present ? Link().PlaceMarker(aAt[0], aAt[1], aAt[2]) : 0u;
}

void RemoveMarker(uint32_t aId) noexcept
{
    if (Link().Present)
        Link().RemoveMarker(aId);
}

void GiveCaps(int32_t aCount) noexcept
{
    if (Link().Present)
        Link().GiveCaps(aCount);
}

uint32_t CaravanQuest() noexcept
{
    return Link().Present ? Link().CaravanQuest() : 0u;
}

void Notify(const char* acpText) noexcept
{
    if (Link().Present)
        Link().Notify(acpText);
}

bool IsOnBike() noexcept
{
    return Link().Present && Link().IsOnBike();
}

bool IsRiding() noexcept
{
    return Link().Present && Link().IsRiding();
}

bool GetHud(Hud& aOut) noexcept
{
    return Link().Present && Link().GetHud(&aOut);
}

bool GetNetState(NetState& aOut) noexcept
{
    return Link().Present && Link().GetNetState(&aOut);
}

bool GetParked(Parked& aOut) noexcept
{
    return Link().Present && Link().GetParked(aOut.At, &aOut.Heading, &aOut.Roll, &aOut.CellOrWorld, &aOut.Paint);
}

bool GetBodyTilt(float aRotation[9], float aPivot[3]) noexcept
{
    return Link().Present && Link().GetBodyTilt(aRotation, aPivot);
}

void SetViewHeld(bool aHeld) noexcept
{
    if (Link().Present)
        Link().SetViewHeld(aHeld);
}

void Bail() noexcept
{
    if (Link().Present)
        Link().Bail();
}

void SetAttackInput(bool aOn) noexcept
{
    if (Link().Present)
        Link().SetAttackInput(aOn);
}

void SetAimZoom(float aDegrees) noexcept
{
    if (Link().Present)
        Link().SetAimZoom(aDegrees);
}

void SetDog(uint32_t aFormId) noexcept
{
    if (Link().Present)
        Link().SetDog(aFormId);
}

void SetRemotePlayerTest(bool (*apTest)(uint32_t)) noexcept
{
    if (Link().Present)
        Link().SetRemotePlayerTest(apTest);
}

void SetRemote(uint32_t aActorFormId, const NetState* apState) noexcept
{
    if (Link().Present)
        Link().SetRemote(aActorFormId, apState);
}

void SetRemoteParked(uint32_t aOwnerId, const Parked& acParked, bool aShow) noexcept
{
    if (Link().Present)
        Link().SetRemoteParked(aOwnerId, acParked.At[0], acParked.At[1], acParked.At[2], acParked.Heading, acParked.Roll, acParked.CellOrWorld, acParked.Paint, aShow);
}
} // namespace BikeLink

#endif
