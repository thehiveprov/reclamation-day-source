#pragma once

#if TP_FALLOUT4

// The motorcycle's plugin (RDVehicles.dll; its own source has the same layouts in src/plugin/Api.h). Looked up the first
// time it's asked for; with no plugin every call here does nothing and says no.
namespace BikeLink
{
struct Hud
{
    float SpeedKmh;
    float Rpm;
    float Redline;
    float Fuel; // litres
    float Tank; // litres it holds
    int32_t Gear;
    bool EngineOn;
    bool Headlight;
    bool EyeView; // first person from the rider's eyes
};

struct NetState
{
    uint8_t Stage; // 0 off, 1 getting on, 2 riding, 3 getting off
    int8_t Side;   // the side getting on or off, the foot down when stopped: -1 left, +1 right
    uint8_t Paint;
    uint8_t Flags;    // 1 headlight, 2 sidecar, 4 engine running, 8 skidding, 16 in the air, 32 ridden by an NPC, 64 their right arm's out with a gun
    float Heading;    // radians
    float Lean;       // radians, right is positive
    float Pitch;      // radians, nose up
    float Steer;      // radians
    float Suspension; // metres, compressed
    float WheelSpeed; // m/s along the ground
    float Rpm;
    float Throttle;    // 0..1
    float StageTime;   // seconds into getting on or off
    float Stand;       // 0..1: how far a foot is down
    float StandGround; // game units, the ground under that foot against the bike's own
};

struct Parked
{
    float At[3];
    float Heading;
    float Roll;
    uint32_t CellOrWorld; // form id: the world space, or the interior cell
    uint8_t Paint;
};

// A caravan truck (the plugin's Convoy): RD puts the crew on it and raises the raids (Services/CaravanService.h)
struct Truck
{
    uint32_t RefId; // 0: only a place on its route for now
    float At[3];
    float Heading;        // radians
    float Speed;          // units a second
    float AlongFromStart; // units along the road it's on
    float AlongFromEnd;
    bool OnRoad; // false: resting at a stop
    bool Ready;  // its model's in the world
    bool Lost;   // its crew's dead
    bool Hold;
    bool Pulled; // pulled over and stopped
    float Side;  // units off the middle of the road, to the right
    bool Burning; // on fire: stopped, and everyone's getting off it
    bool Wrecked; // gone up: what's there is a hulk
    bool Unloading; // stopped in its bay at a stop, its supplies being set down: the crew's off at its tail (seats -3, -4) till it's nearly time to go
};

// An NPC riding a bike along a truck's road: the plugin moves it, RD holds them to it
struct Rider
{
    float At[3];
    float Heading;
    float Speed;
    float StandAt[3]; // stage 4: where they stand beside the bike
    uint8_t Stage;    // 2 riding, 3 getting off, 4 off and standing, 5 fell off (dead or knocked off)
};

[[nodiscard]] int32_t TruckCount() noexcept;
[[nodiscard]] bool GetTruck(int32_t aIndex, Truck& aOut) noexcept;
// 0 the driver, 1.. on the bed; -1, -2 the ground beside the cab
[[nodiscard]] bool GetTruckSeat(int32_t aIndex, int32_t aSeat, float aAt[3], float& aHeading) noexcept;
void SetTruckHold(int32_t aIndex, bool aHold) noexcept;
// Caravans on the roads at all: from level 5 or Diamond City reached, the party's best (the plugin goes by its player's level without it)
void SetCaravansOpen(bool aOpen) noexcept;
void SetTruckLost(int32_t aIndex, float aGameDays) noexcept;
// Raiders have it, aboard: it drives off toward the nearest raider camp and goes once nobody sees it (plugin API 4). SetTruckLost after it: stopped, abandoned.
void TakeTruck(int32_t aIndex, float aGameDays) noexcept;
// Another game says this truck's on fire (1) or gone up (2)
void DamageTruckTo(int32_t aIndex, int32_t aHow) noexcept;
[[nodiscard]] bool TruckTestMode() noexcept;
// A place on the truck's road from where it is now: on along it, and to the right; false past the road's ends
[[nodiscard]] bool GetTruckRoad(int32_t aIndex, float aAhead, float aSide, float aAt[3], float& aHeading) noexcept;
void PullTruckOver(int32_t aIndex, bool aOn) noexcept;
void SnapTruckOver(int32_t aIndex) noexcept;
void SetTruckCrewed(int32_t aIndex, bool aCrewed) noexcept;
[[nodiscard]] bool TakeTruckPlunder(int32_t aIndex) noexcept;
[[nodiscard]] bool StartRider(uint32_t aActorId, int32_t aIndex, float aAhead, float aSide, uint8_t aPaint) noexcept;
void SetRiderGoal(uint32_t aActorId, float aAhead, float aSide) noexcept;
void SetRiderAim(uint32_t aActorId, bool aAiming) noexcept;
void RiderGetOff(uint32_t aActorId, int32_t aSide) noexcept;
[[nodiscard]] bool GetRider(uint32_t aActorId, Rider& aOut) noexcept;
[[nodiscard]] bool GetRiderNet(uint32_t aActorId, NetState& aOut) noexcept;
void ForgetRider(uint32_t aActorId) noexcept;
void PlaceRaiderBike(float aX, float aY, float aZ, float aHeading, uint8_t aPaint) noexcept;
// Activating this character is the game's barter menu, with this truck's cargo as the stock (0: nobody)
void SetTrader(uint32_t aActorId, int32_t aIndex) noexcept;
[[nodiscard]] bool TakeTraded() noexcept;
void WatchHits(uint32_t aActorId, bool aWatch) noexcept;
// Who hit them last, since the last call (0: nobody)
[[nodiscard]] uint32_t TakeHit(uint32_t aActorId) noexcept;
// The crew's records: 0 the trader, 1 a guard
[[nodiscard]] uint32_t TruckCrewBase(int32_t aWhich) noexcept;
void PlaySeatedIdle(uint32_t aActorId) noexcept;
// Sitting on the surface they're held at, legs out in front (the game's floor sit): the guard on top of a truck's load
void PlayFloorSit(uint32_t aActorId) noexcept;
// The settlement a truck carries for ("" if it's the road's own), and that settlement's workshop
[[nodiscard]] const char* TruckHome(int32_t aIndex) noexcept;
[[nodiscard]] uint32_t TruckHomeWorkshop(int32_t aIndex) noexcept;
// The radiant quest: a caravan hit on its road between these distances from here (-1: none), which stays there until EndRaidedCaravan;
// a marker for the objective to point at; the reward; the quest; a line on the HUD
[[nodiscard]] int32_t PickRaidedCaravan(const float aFrom[3], float aLeast, float aMost) noexcept;
void EndRaidedCaravan(int32_t aIndex) noexcept;
[[nodiscard]] uint32_t PlaceMarker(const float aAt[3]) noexcept;
void RemoveMarker(uint32_t aId) noexcept;
void GiveCaps(int32_t aCount) noexcept;
[[nodiscard]] uint32_t CaravanQuest() noexcept;
void Notify(const char* acpText) noexcept;

[[nodiscard]] bool IsPresent() noexcept;
[[nodiscard]] bool IsOnBike() noexcept;
[[nodiscard]] bool IsRiding() noexcept;
[[nodiscard]] bool GetHud(Hud& aOut) noexcept;
[[nodiscard]] bool GetNetState(NetState& aOut) noexcept;
[[nodiscard]] bool GetParked(Parked& aOut) noexcept;
// The body's tilt with the bike around its feet (rows of the rotation, level to tilted): our arm pose runs before it
[[nodiscard]] bool GetBodyTilt(float aRotation[9], float aPivot[3]) noexcept;
void SetViewHeld(bool aHeld) noexcept;
void Bail() noexcept;
void SetAttackInput(bool aOn) noexcept;
void SetAimZoom(float aDegrees) noexcept;
// The local player's dog (0: none), which rides in a fitted sidecar
void SetDog(uint32_t aFormId) noexcept;
void SetRemotePlayerTest(bool (*apTest)(uint32_t)) noexcept;
void SetRemote(uint32_t aActorFormId, const NetState* apState) noexcept;
void SetRemoteParked(uint32_t aOwnerId, const Parked& acParked, bool aShow) noexcept;
} // namespace BikeLink

#endif
