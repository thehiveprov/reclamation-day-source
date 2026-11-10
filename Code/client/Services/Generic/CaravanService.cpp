#if TP_FALLOUT4

#include <Services/CaravanService.h>

#include <AI/AIProcess.h>
#include <Actor.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <Forms/TESForm.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESNPC.h>
#include <Forms/TESQuest.h>
#include <Games/ActorExtension.h>
#include <Games/Fallout4/BikeLink.h>
#include <Games/Fallout4/NemesisGame.h>
#include <Games/Misc/SubtitleManager.h>
#include <Games/TES.h>
#include <Interface/HudMarker.h>
#include <Interface/UI.h>
#include <Messages/NotifyMovementAction.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/RequestMovementAction.h>
#include <PlayerCharacter.h>
#include <Services/PapyrusService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Services/WorkshopService.h>
#include <Utils.h>
#include <WinterOfAtom.h>
#include <World.h>
#include <WorldRays.h>

#include <chrono>

namespace
{
constexpr uint32_t kOldCrewBase = 0x295A1; // CaravanGuardTemplate: the crew before they had records of their own (saves can hold them)
constexpr uint32_t kSitIdle = 0x112DD2;    // FurnitureSitInstant
constexpr uint32_t kHoldUpIdle = 0x161F5C; // IdleHoldUpGunPoint: the game's own holding someone up, raiders' behaviour graph
constexpr uint32_t kCowerIdle = 0x20AC3C;  // CowerLooping
constexpr int kCrewSize = 2;               // the driver and the guard beside them in the cab (no third on the load; its seat, kLoadSeat, is unused)
constexpr float kRaidReach = 6000.f;       // a raid only starts this close to a player
constexpr float kRoadLeft = 1500.f;        // and not this close to the ends of the road (the stops are safe)
constexpr float kDrivingSpeed = 100.f;     // what counts as a truck driving: none of this starts at one resting at a stop or stuck
constexpr int kSitTries = 2;               // tries of the game's own sit (one every 2 s) before they're on the riders' seated idle
constexpr double kBetweenRaids = 600.0;    // real seconds: about three game hours
// Never two caravans raided in one place by two gangs (seen 11-05: a raider gang on each, their bikes left in stacks).
// One raid at a time, anywhere; never on a caravan with another this near; never where the last was while anyone's about.
constexpr float kRaidApart = 5000.f;
constexpr float kLastRaidGone = 6000.f;   // as far as everyone has to be for its bodies to go (ClearLeftBehind)
// Unloading: back on after the plugin says it's time, once nobody's looking at them, or after this anyway
constexpr float kBoardAnyway = 6.f;
constexpr int kRaidChance = 40;            // percent of trucks met
constexpr float kLostDays = 3.f;
// Raiders taking a truck whose crew they killed: each gets on once at it, out of every player's sight or after kBoardAnyway there; whoever's on
// after this long, it goes with
constexpr float kBoardReach = 380.f;
constexpr float kTakeOverMost = 60.f;
// A truck whose crew are gone, not killed, gets a new one only once they've been gone this long with it still there, and not again for kRecrewEvery
constexpr float kRecrewAfter = 2.f;
constexpr double kRecrewEvery = 30.0;
// A truck not in the world this long is gone, and its people with it (shorter is the plugin making it again)
constexpr float kGoneAfter = 2.f;

// Raiders only ever appear where no player can see them
constexpr float kRaiderFromPlayer = 2500.f;
constexpr float kRaidRetry = 1.f; // seconds between looks for spots, and for whether a truck's been seen
// The camera's view, with room to spare for the width of a person and a turn of the head (1: the edge of the screen)
constexpr float kViewMargin = 1.35f;
// Another player's view, as far as their heading tells: about 75 degrees to each side
constexpr float kOtherPlayerView = 0.26f;
constexpr float kEyeHeight = 120.f;

// On the bikes: where they first appear behind the truck on its road, and how long a chase runs before they force it to stop
constexpr float kRidersFirstBehind = 2400.f;
constexpr float kRidersLastBehind = 7000.f;
constexpr float kRidersBehindStep = 700.f;
constexpr float kRidersGap = 220.f;
constexpr int kRidersAtLeast = 2;
constexpr int kRidersAtMost = 3;
constexpr uint32_t kLevelledRaider = 0x2898B;  // LvlRaider
// On foot, for the quest (never right behind a player or in their sight: from the area round about, closing
// in on the stopped truck): they start round the truck this far out, on ground about its height, well away from every player and out of
// their sight, and walk in, each to their own place round it (kHoldUpSlots), on the game's travel package
constexpr float kWalkersNear = 1800.f;
constexpr float kWalkersFar = 3200.f;
constexpr int kWalkersRings = 3;               // starts tried at this many distances between those
constexpr int kWalkerDirections = 16;          // and this many ways round
constexpr float kWalkersApart = 900.f;         // one start this far from another at least
constexpr float kWalkerFromPlayer = 4000.f;    // from any player (a bike's start is 2500: it comes on fast, and from far up the road)
constexpr float kWalkerUpDown = 450.f;         // their start no higher or lower than the truck than this: not a roof, not down a cliff
constexpr float kWalkerThere = 170.f;          // this near their place, they're there
constexpr float kWalkerStuck = 20.f;           // seconds without getting nearer: no way there they can find
constexpr float kWalkersGiveUp = 120.f;        // seconds before the hold-up goes on without whoever's still coming
// A raid's raiders come one at a time this far apart, each body's arrival in a frame of its own. (The hitch as a raid started, 154 to 379 ms
// in the frame the first one's body came in, was the first bike's model read from disk: RDVehicles reads them at the main menu now.)
constexpr float kArriveEvery = 0.75f;
constexpr float kArriveRetry = 0.25f;          // their start in someone's sight: looked at again after this
constexpr float kArriveGiveUp = 6.f;           // and after this long in sight, they don't come
// Encounters start once a player's made it this far: this level, or to Diamond City
constexpr int kEncountersFromLevel = 5;
constexpr uint32_t kDiamondCityLocation = 0x2CEF;
constexpr float kDemandHeard = 2500.f;  // the hold-up's words are for someone who can hear them
constexpr float kDemandLineFor = 3.5f;
constexpr const char* kDemandLines[] = {"Hold it right there! Hands where I can see them!", "That cargo's ours now. Get it off the truck and nobody has to die.", "Don't make me ask again!"};
constexpr const char* kCrewAnswers[] = {"Easy. Nobody needs to get hurt.", "You'll have to take it from us.", "We don't want any trouble."};
constexpr float kChaseLeast = 30.f;
constexpr float kChaseSpread = 25.f;
constexpr float kChaseTooFar = 5500.f;   // from every player: they force it to stop while someone can still get there
constexpr float kRiderSettle = 2.5f;
constexpr float kShootReach = 1900.f;
constexpr float kShootAngle = 1.9f;      // radians either side of where the bike's going
constexpr float kAimFirst = 0.4f;
constexpr float kMuzzleHeight = 105.f;
constexpr float kTargetHeight = 85.f;

// In the road around the truck, relative to it (ahead, to the right)
struct Slot
{
    float Ahead;
    float Side;
};
constexpr Slot kChaseSlots[] = {{-300.f, -170.f}, {-300.f, 170.f}, {-60.f, -215.f}, {-60.f, 215.f}, {-560.f, -70.f}, {-560.f, 80.f}};
constexpr Slot kCutOffSlots[] = {{520.f, -120.f}, {520.f, 120.f}, {760.f, 0.f}, {660.f, -220.f}, {660.f, 220.f}};
constexpr Slot kHoldUpSlots[] = {{470.f, -190.f}, {470.f, 190.f}, {700.f, -110.f}, {700.f, 110.f}, {-430.f, -170.f}, {-430.f, 170.f}};

// A hold-up breaks: a player's this close, or has seen it this long, or someone fires
constexpr float kStandOffNear = 1400.f;
constexpr float kStandOffLeast = 14.f;
constexpr float kStandOffSpread = 12.f;
constexpr float kCrewDownSpeed = 5.f;   // nobody gets out until the truck has stopped
constexpr int kLoadSeat = 2;            // the seat on top of the load (Convoy.cpp, kSeats)
constexpr float kStoppedSpeed = 40.f;   // a bike or a truck going slower than this has stopped, for what comes next
constexpr float kStoppedFor = 1.5f;     // and after this long, it's meant it
constexpr float kChaseRoadLeft = 8000.f; // a chase needs this much road before the truck's stop
constexpr float kChaseRoadEnd = 800.f;  // and it's over this near it
// The radiant quest: how often (seconds), how far off, how long there is to get there, and what comes of it
constexpr float kQuestFirst = 1200.f;
constexpr float kQuestBetween = 1500.f;
constexpr float kQuestSpread = 1500.f;
constexpr float kQuestTestWait = 600.f;
constexpr float kQuestRetry = 120.f;
constexpr float kQuestLeast = 9000.f;   // far enough that the truck isn't a real object yet
constexpr float kQuestMost = 70000.f;
constexpr float kQuestTime = 600.f;
constexpr float kQuestStartWait = 15.f; // for the game to have it running
constexpr float kQuestRaidersClose = 500.f;
constexpr float kQuestStandOffNear = 3000.f; // they hold the crew at gunpoint until a player's about this close
constexpr float kQuestRaidAgain = 20.f;      // raiders dead anyway with nobody there: how long until others come
constexpr float kHoldLogEvery = 5.f;
constexpr int32_t kQuestReward = 150;
constexpr float kHappinessSaved = 5.f;
constexpr float kHappinessLost = -5.f;
constexpr float kHappinessPlundered = -15.f;
constexpr uint32_t kWorkshopRatingHappinessModifier = 0x00127237;

// A player's help: thanked if they hit a raider, or are this close when it ends
constexpr float kHelpReach = 2500.f;
// The thanks: how long the line's up, how long after it they wait with nobody near
constexpr float kLineSeconds = 6.f;
constexpr int kDemandSteps = 4;  // raider, guard, raider, and a moment's silence
constexpr uint32_t kDiamondCityMarkers[] = {0x1CAD5, 0x1CAD8};  // the map's Diamond City and Diamond City Market (the location's own marker, 0x9AE24, isn't a map marker)
constexpr float kThanksAway = 900.f;
constexpr float kThanksAwayFor = 4.f;
constexpr float kThanksAwayAnyway = 15.f;
constexpr float kThanksLookingAt = 0.5f;  // within about 60 degrees of straight at them
constexpr float kThanksMost = 240.f;

constexpr const char* kThanksLines[] = {
    "Thank you. They'd have had the lot if you hadn't come along.",
    "You didn't have to do that, stranger. Much obliged.",
    "That was close. I owe you one, friend.",
    "Thought we were done for. Thanks for stepping in.",
    "Thanks for the help. Least I can do is open up the truck for you.",
};

// The raiders' bikes: the Raider, factory or one of the rough paints (NetState's paint byte: 0x40 is which bike)
constexpr uint8_t kRiderPaints[] = {0x40, 0x41, 0x42, 0x44};

NiPoint3 Point(float aX, float aY, float aZ) noexcept
{
    NiPoint3 point;
    point.x = aX;
    point.y = aY;
    point.z = aZ;
    return point;
}

float Random01() noexcept
{
    return static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
}

float DistanceSquared(const NiPoint3& acA, const NiPoint3& acB) noexcept
{
    const float dx = acA.x - acB.x;
    const float dy = acA.y - acB.y;
    return dx * dx + dy * dy;
}

float Facing(const NiPoint3& acFrom, const NiPoint3& acTo) noexcept
{
    return std::atan2(acTo.x - acFrom.x, acTo.y - acFrom.y);
}

float Wrap(float aAngle) noexcept
{
    constexpr float kPi = 3.14159265f;
    while (aAngle > kPi)
        aAngle -= 2.f * kPi;
    while (aAngle < -kPi)
        aAngle += 2.f * kPi;
    return aAngle;
}

Actor* ActorById(uint32_t aId) noexcept
{
    return aId ? Cast<Actor>(TESForm::GetById(aId)) : nullptr;
}

bool IsAPlayer(uint32_t aId) noexcept
{
    const Actor* pActor = ActorById(aId);
    if (!pActor)
        return false;
    if (pActor == PlayerCharacter::Get())
        return true;
    return const_cast<Actor*>(pActor)->GetExtension()->IsRemotePlayer();
}

int CountAlive(const Vector<uint32_t>& acIds) noexcept
{
    int alive = 0;
    for (const uint32_t id : acIds)
    {
        const Actor* pActor = ActorById(id);
        if (pActor && !pActor->IsDead())
            ++alive;
    }
    return alive;
}

Actor* NearestAlive(const Vector<uint32_t>& acIds, const NiPoint3& acFrom) noexcept
{
    Actor* pBest = nullptr;
    float best = 1e12f;
    for (const uint32_t id : acIds)
    {
        Actor* pActor = ActorById(id);
        if (!pActor || pActor->IsDead())
            continue;
        const float d = DistanceSquared(pActor->position, acFrom);
        if (d < best)
        {
            best = d;
            pBest = pActor;
        }
    }
    return pBest;
}

bool LineIsClear(TESObjectCELL* apCell, uint32_t aFilter, const NiPoint3& acFrom, const NiPoint3& acTo) noexcept
{
    const WorldRays::RayHit hit = WorldRays::PickOnce(apCell, aFilter, acFrom, acTo);
    return !hit.Hit || WorldRays::IsCharacterLayer(hit.Layer);
}

float Row(const float (&acMatrix)[4][4], int aRow, const NiPoint3& acPoint) noexcept
{
    return acMatrix[aRow][0] * acPoint.x + acMatrix[aRow][1] * acPoint.y + acMatrix[aRow][2] * acPoint.z + acMatrix[aRow][3];
}

void IdleStop(Actor* apActor) noexcept
{
    static BSFixedString s_idleStop("IdleStop");
    if (apActor)
        apActor->animationGraphHolder.SendAnimationEvent(&s_idleStop);
}

// The raider who goes for caravans: RDVehicles.esp's, the game's LvlRaider on a list without its level-1 raider (raiders of level 3
// at least), or the game's own without it
TESNPC* CaravanRaiderBase() noexcept
{
    if (const uint32_t id = BikeLink::TruckCrewBase(2))
        if (auto* pBase = Cast<TESNPC>(TESForm::GetById(id)))
            return pBase;
    return Cast<TESNPC>(TESForm::GetById(kLevelledRaider));
}

bool IsMenuOpen(const char* apName) noexcept
{
    UI* pUi = UI::Get();
    return pUi && pUi->GetMenuOpen(BSFixedString(apName));
}

// The human graph's foot IK (as EmoteService): on, it pulls the feet down to the ground under the character through a pose
void SetFootIk(Actor* apActor, bool aOn) noexcept
{
    static BSFixedString s_footIkDisable("bHumanoidFootIKDisable");
    if (apActor)
        apActor->animationGraphHolder.SetVariableBool(&s_footIkDisable, !aOn);
}

void PlayIdleOn(Actor* apActor, uint32_t aIdle) noexcept
{
    auto* pIdle = Cast<TESIdleForm>(TESForm::GetById(aIdle));
    if (apActor && pIdle && apActor->currentProcess)
        apActor->currentProcess->PlayIdle(apActor, pIdle, nullptr, false);
}

// Out of the hold-up's cower, as the game takes anyone out of it. CowerLooping (cowerStart) is a state of the human graph
// beside its normal movement (MTBehavior.hkx: MT's CowerState next to MTRoot), and its only way out is idleStop: the exit
// clip, then ReevaluateGraphState. The seat (the riders' seated idle, dyn_ActivationLoop) is a wildcard of MTRoot_SM
// only, so from the cower it never takes (seen 11-05: the guards stuck in the pose, back on their truck).
void StopCowering(const Vector<uint32_t>& acCrew) noexcept
{
    for (const uint32_t id : acCrew)
        if (Actor* pActor = ActorById(id); pActor && !pActor->IsDead())
            IdleStop(pActor);
}

// The game's own shot, as its weaponFire animation event fires a weapon (the handler, CommonLibF4 id 1240981): the weapon and
// instance at the equip index, then TESObjectWEAP::Fire (1056037). From an NPC it takes the muzzle from their fire node and aims
// at their process's target, or along their heading and pitch without one (id 1314608 reads that); its projectile, sound, noise
// and ammo are the game's. Their animation isn't: the weapon doesn't kick, as they aren't in a state to play it.
struct ObjectInstance
{
    TESForm* pObject;
    void* pInstance;
};

void FireGun(Actor* apActor) noexcept
{
    using TFire = void(const ObjectInstance*, TESObjectREFR*, uint32_t, TESForm*, TESForm*);
    POINTER_FALLOUT4(TFire, s_fire, 1056038);
    ObjectInstance instance{nullptr, nullptr};
    apActor->GetCurrentWeapon(&instance, 0);
    if (instance.pObject)
        s_fire.Get()(&instance, apActor, 0, nullptr, nullptr);
    // (the lookup took a reference on the instance)
    if (instance.pInstance)
    {
        auto* pCount = reinterpret_cast<volatile long*>(static_cast<uint8_t*>(instance.pInstance) + 8);
        if (_InterlockedDecrement(pCount) == 0)
            (*static_cast<void (***)(void*, bool)>(instance.pInstance))[0](instance.pInstance, true);
    }
}

void Forget(Vector<uint32_t>& aIds) noexcept
{
    for (const uint32_t id : aIds)
    {
        BikeLink::WatchHits(id, false);
        if (Actor* pActor = ActorById(id))
            pActor->Delete();
    }
    aIds.clear();
}

bool IsTruckCrewName(const Actor* apActor) noexcept
{
    const auto* pNpc = apActor ? Cast<TESNPC>(apActor->baseForm) : nullptr;
    const char* pName = pNpc ? pNpc->fullName.value.AsAscii() : nullptr;
    return pName && (!strcmp(pName, "Caravan Trader") || !strcmp(pName, "Caravan Guard"));
}

constexpr uint8_t kTruckHeldFlag = 1;
constexpr uint8_t kTruckLostFlag = 2;
// (its damage is the game's own on whichever game it was shot in: this says how far it's got, and nothing else)
constexpr uint8_t kTruckBurningFlag = 4;
constexpr uint8_t kTruckWreckedFlag = 8;
constexpr uint8_t kTruckDamageOnlyFlag = 16;
constexpr uint8_t kTruckTakenFlag = 32;  // raiders drive it off (RDVehicles TakeTruck): the others' copies go too
} // namespace

bool CaravanService::IsTruckCrew(const Actor* apActor) noexcept
{
    return apActor && apActor->baseForm && (apActor->baseForm->formID == kOldCrewBase || IsTruckCrewName(apActor));
}

CaravanService::CaravanService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CaravanService::OnUpdate>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&CaravanService::OnNotifyMovementAction>(this);
    m_playerJoinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&CaravanService::OnPlayerJoined>(this);
    m_playerLevelConnection = aDispatcher.sink<NotifyPlayerLevel>().connect<&CaravanService::OnPlayerLevel>(this);
    m_playerLeftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&CaravanService::OnPlayerLeft>(this);
}

void CaravanService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (!BikeLink::IsPresent())
        return;
    const float delta = std::clamp(static_cast<float>(acEvent.Delta), 0.f, 0.1f);
    m_time += delta;
    m_delta = delta;
    m_frameBeforeMs = m_frameMs;
    m_frameMs = static_cast<float>(acEvent.Delta) * 1000.f;
    const int count = BikeLink::TruckCount();
    if (count <= 0)
        return;
    m_trucks.resize(count);

    // In a party one game looks after the people
    const auto& party = m_world.ctx().at<PartyService>();
    const bool mine = !party.IsInParty() || party.IsLeader();
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    if ((m_strayCheck -= delta) <= 0.f)
    {
        m_strayCheck = 15.f;
        if (mine)
            ClearStrays(pPlayer);
    }
    if ((m_leftCheck -= delta) <= 0.f)
    {
        m_leftCheck = 3.f;
        ClearLeftBehind(pPlayer);
        // Caravans on the roads at all, as raids on them: from level 5 or Diamond City reached, whichever's first, the party's best;
        // the plugin rolls each trip's chance from there
        BikeLink::SetCaravansOpen(EncountersOpen());
    }
    if (mine)
        UpdateCaravanQuest(pPlayer, delta);

    const bool sendNow = (m_sendTimer -= delta) <= 0.f;
    if (sendNow)
        m_sendTimer = 1.f / 15.f;

    for (int i = 0; i < count; ++i)
    {
        Truck& truck = m_trucks[i];
        BikeLink::Truck info{};
        if (!BikeLink::GetTruck(i, info))
            continue;
        SendTruckDamage(i, truck, info);
        // Not there for a moment isn't gone: the plugin making it again where it was (the game takes its reference with the cell it was made in: 11-05 22:34,
        // a raid's three raiders and the crew cleared with it as it came into Sanctuary). Its people stay as they are unless it's away a while.
        truck.NotReadyFor = info.Ready ? 0.f : truck.NotReadyFor + delta;
        if (!mine || (!info.Ready && truck.NotReadyFor >= kGoneAfter))
        {
            if (truck.Now != Stage::kAway)
                Clear(i, truck);
            continue;
        }
        if (!info.Ready)
            continue;
        if (truck.Now == Stage::kAway)
        {
            // (nobody's put on one already on fire or gone up)
            if (info.Burning || info.Wrecked)
                continue;
            BringOutCrew(i, truck);
            truck.Now = Stage::kRiding;
        }
        if (info.Burning && !truck.OnFire)
        {
            truck.OnFire = true;
            TruckOnFire(i, truck);
        }
        if (info.Wrecked && truck.Now != Stage::kLost)
            LoseTruck(i, truck);
        // Its crew gone from under it, not killed: no body's left of any of them. Nothing that's happened to the truck says lost: it's a new crew. (Seen 11-05:
        // the game connecting to the server just after the save loaded deleted them, and the raid that came next found nobody alive and lost the truck.)
        // Never as the world comes down, though (the 22:05 crash: quitting deleted the crew, this put a new one on twice in half a second, and the game's own work on
        // the new ones' outfits on another thread found them deleted under it): gone a while with the truck still there, no menu up that takes the world down, and
        // at most once in kRecrewEvery
        const bool crewGone = !truck.Crew.empty() && truck.Now != Stage::kLost && truck.Now != Stage::kBurning && truck.Now != Stage::kTakeOver && truck.Now != Stage::kTaken && !info.Lost &&
                              !info.Burning && !info.Wrecked && std::none_of(truck.Crew.begin(), truck.Crew.end(), [](uint32_t aId) { return ActorById(aId) != nullptr; });
        truck.CrewGoneFor = crewGone ? truck.CrewGoneFor + delta : 0.f;
        const bool worldGoing = IsMenuOpen("PauseMenu") || IsMenuOpen("LoadingMenu") || IsMenuOpen("MainMenu") || IsMenuOpen("FaderMenu");
        if (crewGone && truck.CrewGoneFor >= kRecrewAfter && !worldGoing && m_time - truck.Recrewed < kRecrewEvery && !truck.RecrewHeldLogged)
        {
            truck.RecrewHeldLogged = true;
            spdlog::warn("Caravan: truck {}'s crew are gone again {:.0f} s after its last new crew: none put on till {:.0f} s have gone by", i, m_time - truck.Recrewed, kRecrewEvery);
        }
        if (crewGone && truck.CrewGoneFor >= kRecrewAfter && !worldGoing && m_time - truck.Recrewed >= kRecrewEvery)
        {
            truck.Recrewed = m_time;
            truck.RecrewHeldLogged = false;
            truck.CrewGoneFor = 0.f;
            spdlog::warn("Caravan: truck {}'s crew are gone, not killed (none of them is there, dead or alive): a new crew", i);
            if (truck.Trader)
                BikeLink::SetTrader(0, -1);
            truck.Trader = 0;
            truck.Unloaders.clear();
            truck.CrewDown = false;
            truck.CrewFree = false;
            BringOutCrew(i, truck);
            if (truck.Now == Stage::kUnload || truck.Now == Stage::kPullOver || truck.Now == Stage::kThanks || truck.Now == Stage::kAfter || truck.Now == Stage::kPlunder)
                BackOnTheRoad(i, truck);
        }
        // The caravan the quest's about: it's stopped, and as soon as there's somewhere out of sight to come from the raiders are on it, on foot from round about
        if (i == m_questTruck && !m_questRaidOn && truck.Now == Stage::kRiding && (m_questLook -= delta) <= 0.f && RaidAllowed(pPlayer, i, Point(info.At[0], info.At[1], info.At[2])))
        {
            m_questLook = 2.f;
            truck.Planned = Plan::kHoldUp;
            truck.PlanChosen = true;
            m_questRaidOn = TryStartWalkers(pPlayer, i, truck, info);
            if (m_questRaidOn && NearestPlayer(pPlayer, Point(info.At[0], info.At[1], info.At[2])) >= kQuestStandOffNear)
                HoldForQuest(i, truck);
        }

        BringInRaiders(pPlayer, i, truck, info, delta);
        WatchBodies(i, truck);
        UpdateRaiders(pPlayer, i, truck, delta);
        UpdateWalkers(pPlayer, i, truck, delta);
        CheckAttacked(pPlayer, i, truck, delta);
        if (sendNow)
            SendRiders(i, truck);
        SendTruck(i, truck, info);

        switch (truck.Now)
        {
        case Stage::kRiding:
            // In its bay at a stop: the crew down at the tail while its supplies are set down
            if (info.Unloading && !info.Hold && !info.Lost)
            {
                StartUnload(i, truck);
                break;
            }
            SeatCrew(i, truck);
            WatchRiding(pPlayer, i, truck, info, delta);
            break;
        case Stage::kUnload:
            WatchUnload(pPlayer, i, truck, info, delta);
            break;
        case Stage::kChase:
            SeatCrew(i, truck);
            WatchChase(pPlayer, i, truck, info, delta);
            break;
        case Stage::kCutOff:
            SeatCrew(i, truck);
            WatchCutOff(pPlayer, i, truck, info, delta);
            break;
        case Stage::kStandOff:
            WatchStandOff(pPlayer, i, truck, info, delta);
            break;
        case Stage::kFight:
            WatchFight(pPlayer, i, truck, info, delta);
            break;
        case Stage::kPullOver:
            if (!truck.CrewDown)
                SeatCrew(i, truck);
            if (info.Pulled)
                BeginThanks(pPlayer, i, truck);
            break;
        case Stage::kThanks:
            WatchThanks(pPlayer, i, truck, info, delta);
            break;
        case Stage::kPlunder:
            WatchPlunder(pPlayer, i, truck, info, delta);
            break;
        case Stage::kAfter:
            if ((truck.Timer -= delta) <= 0.f)
                BackOnTheRoad(i, truck);
            break;
        case Stage::kTakeOver:
            WatchTakeOver(pPlayer, i, truck, info, delta);
            break;
        case Stage::kTaken:
            WatchTaken(i, truck);
            break;
        default:
            break;
        }
    }
}

void CaravanService::BringOutCrew(int aIndex, Truck& aTruck) noexcept
{
    const uint32_t traderId = BikeLink::TruckCrewBase(0);
    const uint32_t guardId = BikeLink::TruckCrewBase(1);
    if (!traderId || !guardId)
        spdlog::warn("Caravan: RDVehicles.esp has no crew records, the old guard takes their place, with nothing to trade");
    Forget(aTruck.Crew);
    for (int seat = 0; seat < kCrewSize; ++seat)
    {
        float at[3]{};
        float heading = 0.f;
        if (!BikeLink::GetTruckSeat(aIndex, seat, at, heading))
            continue;
        const uint32_t baseId = seat == 0 ? (traderId ? traderId : kOldCrewBase) : (guardId ? guardId : kOldCrewBase);
        auto* pBase = Cast<TESNPC>(TESForm::GetById(baseId));
        Actor* pActor = pBase ? NemesisGame::Spawn(pBase, Point(at[0], at[1], at[2])) : nullptr;
        if (!pActor)
        {
            spdlog::warn("Caravan: couldn't place crew on truck {}", aIndex);
            continue;
        }
        NemesisGame::Restrain(pActor, true);
        BikeLink::WatchHits(pActor->formID, true);
        aTruck.Crew.push_back(pActor->formID);
    }
    aTruck.Seat = 0.f;
    BikeLink::SetTruckCrewed(aIndex, !aTruck.Crew.empty());
    spdlog::info("Caravan: truck {} has a crew of {}", aIndex, aTruck.Crew.size());
}

// Decided once, when it's met. Test mode goes through them in turn: one play session shows each.
void CaravanService::ChoosePlan(int aIndex, Truck& aTruck) noexcept
{
    aTruck.Planned = Plan::kNone;
    aTruck.Seen = false;
    aTruck.Look = 0.f;
    aTruck.WaitLogged = false;
    aTruck.KeptApartLogged = false;
    if (BikeLink::TruckTestMode())
    {
        static constexpr Plan s_cycle[] = {Plan::kHoldUp, Plan::kNone};
        aTruck.Planned = EncountersOpen() ? s_cycle[m_testTurn++ % 2] : Plan::kNone;
        aTruck.RaidIn = 5.f;
        spdlog::info("Caravan: test, truck {} will be {}", aIndex, aTruck.Planned == Plan::kChase ? "chased" : aTruck.Planned == Plan::kHoldUp ? "held up" : "left alone: plunder it");
        return;
    }
    // (a hold-up, always: raiders threaten first. The chase's shooting from the saddle came before any word.)
    if (EncountersOpen() && m_time >= m_nextRaid && rand() % 100 < kRaidChance)
        aTruck.Planned = Plan::kHoldUp;
    // Counted from when it's first seen driving: the truck going by comes first
    aTruck.RaidIn = 8.f + Random01() * 12.f;
}

// Held on their seats, as the rider's held on the bike; they sit there as on a bench
void CaravanService::SeatCrew(int aIndex, Truck& aTruck, const Vector<uint32_t>* apSkip) noexcept
{
    const bool refresh = aTruck.Seat <= 0.f;
    aTruck.Seat = refresh ? 2.f : aTruck.Seat - m_delta;
    auto* pIdle = refresh ? Cast<TESIdleForm>(TESForm::GetById(kSitIdle)) : nullptr;
    if (refresh && !aTruck.SatGoodLogged && aTruck.SitTries <= kSitTries)
        ++aTruck.SitTries;
    // Whoever's left drives: with the driver dead, the first one alive takes the wheel
    size_t wheel = 0;
    for (size_t seat = 0; seat < aTruck.Crew.size(); ++seat)
    {
        const Actor* pActor = ActorById(aTruck.Crew[seat]);
        if (pActor && !pActor->IsDead())
        {
            wheel = seat;
            break;
        }
    }
    for (size_t seat = 0; seat < aTruck.Crew.size(); ++seat)
    {
        Actor* pActor = ActorById(aTruck.Crew[seat]);
        if (!pActor || pActor->IsDead() || pActor->formID == aTruck.Trader)
            continue;
        if (apSkip && std::find(apSkip->begin(), apSkip->end(), pActor->formID) != apSkip->end())
            continue;
        const int32_t place = seat == wheel ? 0 : static_cast<int32_t>(seat);
        float at[3]{};
        float heading = 0.f;
        if (!BikeLink::GetTruckSeat(aIndex, place, at, heading))
            continue;
        // Test mode says how far they were from their seat since they were last put on it: anything much is the collision pushing them out
        if (refresh && BikeLink::TruckTestMode())
        {
            const float dx = pActor->position.x - at[0];
            const float dy = pActor->position.y - at[1];
            const float dz = pActor->position.z - at[2];
            spdlog::info("Caravan: truck {} seat {}: {:X} is {:.1f} off it ({:.1f} across, {:.1f} high)", aIndex, seat, pActor->formID, std::sqrt(dx * dx + dy * dy + dz * dz), std::sqrt(dx * dx + dy * dy), dz);
        }
        pActor->ForcePosition(Point(at[0], at[1], at[2]));
        pActor->SetRotation(0.f, 0.f, heading);
        // On top of the load it's the floor sit: on the tarp with their legs out along it. The feet would be pulled down to the ground otherwise
        if (place == kLoadSeat)
        {
            if (refresh)
            {
                SetFootIk(pActor, false);
                BikeLink::PlayFloorSit(pActor->formID);
            }
            continue;
        }
        if (pIdle && pActor->currentProcess)
        {
            bool sat = false;
            if (aTruck.SitTries < kSitTries)
            {
                sat = pActor->currentProcess->PlayIdle(pActor, pIdle, nullptr, false);
                if (sat && !aTruck.SatGoodLogged)
                {
                    aTruck.SatGoodLogged = true;
                    spdlog::info("Caravan: crew of truck {} sits, on the game's own idle", aIndex);
                }
            }
            // The game's sit is for furniture and may not take from a character held in place. The riders' seated idle is proven on the
            // bike: without it the driver stands, with their head through the cab's roof.
            if (!aTruck.SatGoodLogged && aTruck.SitTries >= kSitTries)
            {
                BikeLink::PlaySeatedIdle(pActor->formID);
                if (!aTruck.SatRiderLogged)
                {
                    aTruck.SatRiderLogged = true;
                    spdlog::info("Caravan: crew of truck {} wouldn't take the game's sit in {} tries, on the riders' seated idle", aIndex, kSitTries);
                }
            }
        }
    }
}

// In its bay at a stop (the plugin's Convoy::Deliver): those not at the wheel (the guard, with a crew of two) get down behind the tail and stand by its supplies as they're set down; the driver stays
// at the wheel. It's held there till they're back on.
void CaravanService::StartUnload(int aIndex, Truck& aTruck) noexcept
{
    aTruck.Unloaders.clear();
    // (whoever's first alive drives, as SeatCrew has it)
    size_t wheel = aTruck.Crew.size();
    for (size_t seat = 0; seat < aTruck.Crew.size(); ++seat)
    {
        const Actor* pActor = ActorById(aTruck.Crew[seat]);
        if (pActor && !pActor->IsDead())
        {
            wheel = seat;
            break;
        }
    }
    for (size_t seat = 0; seat < aTruck.Crew.size() && aTruck.Unloaders.size() < 2; ++seat)
    {
        Actor* pActor = ActorById(aTruck.Crew[seat]);
        if (seat == wheel || !pActor || pActor->IsDead())
            continue;
        float at[3]{};
        float heading = 0.f;
        if (!BikeLink::GetTruckSeat(aIndex, aTruck.Unloaders.empty() ? -3 : -4, at, heading))
            continue;
        NiPoint3 ground = Point(at[0], at[1], at[2] + 100.f);
        if (!NemesisGame::GroundAt(ground))
            continue;
        IdleStop(pActor);
        SetFootIk(pActor, true);
        pActor->ForcePosition(ground);
        pActor->SetRotation(0.f, 0.f, heading);
        NemesisGame::Restrain(pActor, true);
        aTruck.Unloaders.push_back(pActor->formID);
    }
    BikeLink::SetTruckHold(aIndex, true);
    aTruck.Now = Stage::kUnload;
    aTruck.BoardWait = 0.f;
    aTruck.Seat = 0.f;
    spdlog::info("Caravan: truck {} unloads at its stop, {} of its crew down at the tail", aIndex, aTruck.Unloaders.size());
}

void CaravanService::WatchUnload(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    SeatCrew(aIndex, aTruck, &aTruck.Unloaders);
    if (acInfo.Unloading && !acInfo.Lost)
        return;
    // Time to go: back on, not in front of someone looking right at them unless that goes on a while (as after the thanks)
    bool seen = false;
    for (const uint32_t id : aTruck.Unloaders)
    {
        const Actor* pActor = ActorById(id);
        seen |= pActor && !pActor->IsDead() && CanBeSeen(apPlayer, pActor->position, 150.f);
    }
    aTruck.BoardWait += aDelta;
    if (seen && aTruck.BoardWait < kBoardAnyway)
        return;
    aTruck.Unloaders.clear();
    aTruck.Seat = 0.f;
    aTruck.Now = Stage::kRiding;
    BikeLink::SetTruckHold(aIndex, false);
    spdlog::info("Caravan: truck {}'s crew are back on after unloading{}", aIndex, seen ? ", in sight of a player" : "");
}

// Off the truck and onto the ground beside the cab: the driver and the man behind him to the left, the other to the right.
// aFree: the game's AI has them from here (at this target, or the nearest raider). Else they're held still, cowering.
void CaravanService::DropCrew(int aIndex, Truck& aTruck, bool aFree, Actor* apTarget) noexcept
{
    BikeLink::Truck info{};
    if (!BikeLink::GetTruck(aIndex, info))
        return;
    const float fx = std::sin(info.Heading);
    const float fy = std::cos(info.Heading);
    Vector<uint32_t> raiders;
    for (const Raider& raider : aTruck.Raiders)
        raiders.push_back(raider.Id);
    for (size_t seat = 0; seat < aTruck.Crew.size(); ++seat)
    {
        Actor* pActor = ActorById(aTruck.Crew[seat]);
        if (!pActor || pActor->IsDead())
            continue;
        if (!aTruck.CrewDown)
        {
            float at[3]{};
            float heading = 0.f;
            // (the driver out on their own side, the other two on the right: the guard in the cab has that door)
            if (BikeLink::GetTruckSeat(aIndex, seat == 0 ? -1 : -2, at, heading))
            {
                NiPoint3 ground = Point(at[0] - fx * 90.f * static_cast<float>(seat), at[1] - fy * 90.f * static_cast<float>(seat), at[2] + 100.f);
                if (NemesisGame::GroundAt(ground))
                    pActor->ForcePosition(ground);
            }
            IdleStop(pActor);
            SetFootIk(pActor, true);
        }
        if (!aFree)
        {
            NemesisGame::Restrain(pActor, true);
            continue;
        }
        NemesisGame::Restrain(pActor, false);
        IdleStop(pActor);
        Actor* pTarget = apTarget ? apTarget : NearestAlive(raiders, pActor->position);
        if (pTarget)
            NemesisGame::Attack(pActor, pTarget);
    }
    aTruck.CrewDown = true;
    aTruck.CrewFree = aFree;
    aTruck.Idle = 0.f;
    // With nobody in it, it stays where it is until they're back on
    BikeLink::SetTruckHold(aIndex, true);
}

// Could a player see someone or something at this point, up to this height? Inside a player's view, with a clear line from
// their eye to it. When it can't be told it counts as seen.
bool CaravanService::CanBeSeen(PlayerCharacter* apPlayer, const NiPoint3& acPoint, float aHeight) noexcept
{
    TESObjectCELL* pCell = apPlayer->GetParentCellEx();
    float clip[4][4];
    NiPoint3 eye;
    if (!pCell || !HudMarker::GetWorldToClip(clip) || !HudMarker::GetCameraPosition(eye))
        return true;
    const uint32_t filter = WorldRays::GetLineOfSightFilter(apPlayer);
    const float heights[] = {aHeight * 0.15f, aHeight * 0.55f, aHeight};

    const auto lineTo = [&](const NiPoint3& acEye)
    {
        for (const float height : heights)
        {
            if (LineIsClear(pCell, filter, acEye, Point(acPoint.x, acPoint.y, acPoint.z + height)))
                return true;
        }
        return false;
    };

    const NiPoint3 middle = Point(acPoint.x, acPoint.y, acPoint.z + aHeight * 0.5f);
    const float w = Row(clip, 3, middle);
    if (w > 0.f && std::abs(Row(clip, 0, middle)) <= w * kViewMargin && std::abs(Row(clip, 1, middle)) <= w * kViewMargin && lineTo(eye))
        return true;

    // The rest of the party: where they're facing is all that's known of their view
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        const Actor* pOther = ActorById(view.get<FormIdComponent>(entity).Id);
        if (!pOther || pOther == apPlayer)
            continue;
        const NiPoint3 otherEye = Point(pOther->position.x, pOther->position.y, pOther->position.z + kEyeHeight);
        const float dx = acPoint.x - otherEye.x;
        const float dy = acPoint.y - otherEye.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        const float facing = length > 1.f ? (dx * std::sin(pOther->rotation.z) + dy * std::cos(pOther->rotation.z)) / length : 1.f;
        if (facing >= kOtherPlayerView && lineTo(otherEye))
            return true;
    }
    return false;
}

bool CaravanService::FarFromPlayers(PlayerCharacter* apPlayer, const NiPoint3& acPoint, float aDistance) noexcept
{
    return NearestPlayer(apPlayer, acPoint) >= aDistance;
}

float CaravanService::NearestPlayer(PlayerCharacter* apPlayer, const NiPoint3& acPoint, Actor** appPlayer) noexcept
{
    Actor* pBest = apPlayer;
    float best = DistanceSquared(apPlayer->position, acPoint);
    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : view)
    {
        Actor* pOther = ActorById(view.get<FormIdComponent>(entity).Id);
        if (!pOther || pOther == apPlayer)
            continue;
        const float d = DistanceSquared(pOther->position, acPoint);
        if (d < best)
        {
            best = d;
            pBest = pOther;
        }
    }
    if (appPlayer)
        *appPlayer = pBest;
    return std::sqrt(best);
}

// A raid is this sighting going wrong: a truck a player has seen driving, on its road and moving, and it starts from out of sight. Never
// at one resting at a stop or stuck behind something; in a hold-up the raiders are what stops it.
void CaravanService::WatchRiding(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    const bool driving = acInfo.OnRoad && !acInfo.Hold && !acInfo.Pulled && !acInfo.Lost && acInfo.Speed >= kDrivingSpeed;
    if (!driving || NemesisGame::IsBusy())
        return;
    // Chosen at the first sight of it driving, so test mode's turns aren't used up on trucks resting at their stops
    if (!aTruck.PlanChosen)
    {
        aTruck.PlanChosen = true;
        ChoosePlan(aIndex, aTruck);
    }
    if (aTruck.Planned == Plan::kNone)
        return;
    if ((aTruck.Look -= aDelta) > 0.f)
        return;
    aTruck.Look = kRaidRetry;
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    // A chase needs road to happen on. Too near its stop for one, they cut it off instead.
    if (aTruck.Planned == Plan::kChase && acInfo.AlongFromEnd < kChaseRoadLeft)
        aTruck.Planned = Plan::kHoldUp;
    const bool roadLeft = acInfo.AlongFromStart > kRoadLeft && acInfo.AlongFromEnd > kRoadLeft;
    const bool close = NearestPlayer(apPlayer, at) < kRaidReach;
    if (!aTruck.Seen)
    {
        aTruck.Seen = CanBeSeen(apPlayer, at, 150.f);
        if (aTruck.Seen)
            spdlog::info("Caravan: truck {} seen driving at ({:.0f}, {:.0f}, {:.0f}) {:.0f} u/s, {} in {:.0f} s", aIndex, at.x, at.y, at.z, acInfo.Speed, aTruck.Planned == Plan::kChase ? "a chase" : "a hold-up", aTruck.RaidIn);
        return;
    }
    if ((aTruck.RaidIn -= kRaidRetry) > 0.f || !roadLeft || !close)
        return;
    if (!RaidAllowed(apPlayer, aIndex, at))
    {
        if (!aTruck.KeptApartLogged)
        {
            aTruck.KeptApartLogged = true;
            spdlog::info("Caravan: no raid on truck {} for now: another raid's on, another caravan's near it, or it's where the last raid was", aIndex);
        }
        return;
    }
    if (TryStartRiders(apPlayer, aIndex, aTruck, acInfo))
    {
        spdlog::info("Caravan: raid on truck {} starts as it drives: at ({:.0f}, {:.0f}, {:.0f}) {:.0f} u/s, {:.0f} along its road with {:.0f} to go, {:.0f} from the nearest player", aIndex, at.x, at.y, at.z, acInfo.Speed, acInfo.AlongFromStart,
                     acInfo.AlongFromEnd, NearestPlayer(apPlayer, at));
    }
    else if (!aTruck.WaitLogged)
    {
        aTruck.WaitLogged = true;
        spdlog::info("Caravan: raiders for truck {} wait: nowhere on the road behind it is out of sight", aIndex);
    }
}

// On the truck's own road behind it, somewhere nobody can see, far enough off that they're heard before they're seen. They all
// fit or it's the next stretch back; nothing falls back to a place that can be seen.
bool CaravanService::RaidAllowed(PlayerCharacter* apPlayer, int aIndex, const NiPoint3& acAt) noexcept
{
    for (int j = 0; j < static_cast<int>(m_trucks.size()); ++j)
    {
        if (j == aIndex)
            continue;
        const Truck& other = m_trucks[j];
        const bool raidOn = other.Now == Stage::kChase || other.Now == Stage::kCutOff || other.Now == Stage::kStandOff || other.Now == Stage::kFight;
        if (raidOn || !other.Raiders.empty())
            return false;
        BikeLink::Truck info{};
        if (BikeLink::GetTruck(j, info) && info.Ready && DistanceSquared(Point(info.At[0], info.At[1], info.At[2]), acAt) < kRaidApart * kRaidApart)
            return false;
    }
    if (m_lastRaidSet && DistanceSquared(m_lastRaidAt, acAt) < kRaidApart * kRaidApart && NearestPlayer(apPlayer, m_lastRaidAt) < kLastRaidGone)
        return false;
    // (the last ones' dead, still lying there)
    for (const uint32_t id : m_leftBehind)
    {
        const Actor* pActor = ActorById(id);
        if (pActor && DistanceSquared(pActor->position, acAt) < kRaidApart * kRaidApart)
            return false;
    }
    return true;
}

bool CaravanService::TryStartRiders(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept
{
    // The game's own leveled raider, what it places itself: which one comes depends on the player's level, as anywhere
    auto* pBase = CaravanRaiderBase();
    if (!pBase)
        pBase = NemesisGame::SpawnBase("", 1, false);
    if (!pBase)
        return false;
    const int wanted = kRidersAtLeast + rand() % (kRidersAtMost - kRidersAtLeast + 1);
    for (int count = wanted; count >= kRidersAtLeast; --count)
    {
        for (float behind = kRidersFirstBehind; behind <= kRidersLastBehind; behind += kRidersBehindStep)
        {
            struct Start
            {
                float Ahead;
                float Side;
                NiPoint3 At;
            };
            Vector<Start> starts;
            bool fits = true;
            for (int k = 0; k < count && fits; ++k)
            {
                const float ahead = -behind - kRidersGap * static_cast<float>(k);
                const float side = k % 2 == 0 ? 90.f : -90.f;
                float at[3]{};
                float heading = 0.f;
                fits = BikeLink::GetTruckRoad(aIndex, ahead, side, at, heading);
                if (!fits)
                    break;
                const NiPoint3 spot = Point(at[0], at[1], at[2]);
                fits = FarFromPlayers(apPlayer, spot, kRaiderFromPlayer) && !CanBeSeen(apPlayer, spot, 180.f);
                if (fits)
                    starts.push_back({ahead, side, spot});
            }
            if (!fits)
                continue;

            // Every start was free; the first comes now and the rest one at a time after it (BringInRaiders), each where their start is then
            aTruck.Paint = kRiderPaints[rand() % std::size(kRiderPaints)];
            aTruck.Arrivals.clear();
            size_t first = 0;
            while (first < starts.size() && !PlaceRider(aIndex, aTruck, starts[first].Ahead, starts[first].Side, starts[first].At))
                ++first;
            if (aTruck.Raiders.empty())
                return false;
            for (size_t k = first + 1; k < starts.size(); ++k)
            {
                Arrival arrival;
                arrival.Ahead = starts[k].Ahead;
                arrival.Side = starts[k].Side;
                aTruck.Arrivals.push_back(arrival);
            }
            aTruck.ArriveIn = kArriveEvery;
            aTruck.Now = aTruck.Planned == Plan::kChase ? Stage::kChase : Stage::kCutOff;
            aTruck.Timer = aTruck.Planned == Plan::kChase ? kChaseLeast + Random01() * kChaseSpread : 20.f;
            aTruck.PullAsked = false;
            aTruck.Helped = false;
            aTruck.OnFoot = false;
            m_nextRaid = m_time + kBetweenRaids;
            m_lastRaidAt = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
            m_lastRaidSet = true;
            spdlog::info("Caravan: {} raiders on bikes {} truck {}, from {:.0f} back on the road, out of sight: {} now, the rest one every {:.2f} s", aTruck.Raiders.size() + aTruck.Arrivals.size(),
                         aTruck.Planned == Plan::kChase ? "chase" : "go to cut off", aIndex, behind, aTruck.Raiders.size(), kArriveEvery);
            return true;
        }
    }
    return false;
}

bool CaravanService::PlaceRider(int aIndex, Truck& aTruck, float aAhead, float aSide, const NiPoint3& acAt) noexcept
{
    auto* pBase = CaravanRaiderBase();
    if (!pBase)
        pBase = NemesisGame::SpawnBase("", 1, false);
    if (!pBase)
        return false;
    const auto started = std::chrono::steady_clock::now();
    Actor* pRaider = NemesisGame::Spawn(pBase, acAt);
    if (!pRaider)
        return false;
    if (!BikeLink::StartRider(pRaider->formID, aIndex, aAhead, aSide, aTruck.Paint))
    {
        pRaider->Delete();
        return false;
    }
    BikeLink::WatchHits(pRaider->formID, true);
    Raider raider;
    raider.Id = pRaider->formID;
    raider.OnBike = true;
    raider.Settle = kRiderSettle;
    raider.NewSlot = 0.f;
    raider.NextShot = 0.5f + Random01() * 2.f;
    raider.PlacedAt = m_time;
    // The game's AI gets their gun out; then they're held to the bike
    NemesisGame::Attack(pRaider, NearestAlive(aTruck.Crew, pRaider->position));
    raider.PlaceMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    aTruck.Raiders.push_back(raider);
    return true;
}

bool CaravanService::PlaceWalker(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const NiPoint3& acStart, const NiPoint3& acGoal, const NiPoint3& acTruckAt) noexcept
{
    auto* pBase = CaravanRaiderBase();
    if (!pBase)
        pBase = NemesisGame::SpawnBase("", 1, false);
    if (!pBase)
        return false;
    const auto started = std::chrono::steady_clock::now();
    const float goal[3] = {acGoal.x, acGoal.y, acGoal.z};
    const uint32_t marker = BikeLink::PlaceMarker(goal);
    Actor* pRaider = NemesisGame::Spawn(pBase, acStart);
    if (!pRaider)
    {
        if (marker)
            BikeLink::RemoveMarker(marker);
        return false;
    }
    pRaider->SetRotation(0.f, 0.f, Facing(acStart, acGoal));
    BikeLink::WatchHits(pRaider->formID, true);
    Raider raider;
    raider.Id = pRaider->formID;
    raider.Walking = true;
    raider.Marker = marker;
    raider.Goal = acGoal;
    raider.PlacedAt = m_time;
    raider.Aggression = NemesisGame::Aggression(pRaider);
    NemesisGame::SetAggression(pRaider, 0.f);
    auto* pMarker = marker ? Cast<TESObjectREFR>(TESForm::GetById(marker)) : nullptr;
    const bool going = NemesisGame::TravelTo(pRaider, pMarker);
    raider.PlaceMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    spdlog::info("Caravan: {:X} walks in to truck {} from {:.0f} off, {:.0f} from the nearest player, aggression {:.0f} (0 on the way){}", raider.Id, aIndex,
                 std::sqrt(DistanceSquared(acStart, acTruckAt)), NearestPlayer(apPlayer, acStart), raider.Aggression, going ? "" : ": the travel package couldn't be put on them");
    aTruck.Raiders.push_back(raider);
    return true;
}

void CaravanService::BringInRaiders(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    if (aTruck.Arrivals.empty())
        return;
    // Only while they're still on their way to it: a raid that's gone on to a stand-off or a fight, or ended, goes on with those there
    if (aTruck.Now != Stage::kChase && aTruck.Now != Stage::kCutOff)
    {
        spdlog::info("Caravan: {} raiders still to come for truck {} don't: the raid's gone on without them", aTruck.Arrivals.size(), aIndex);
        aTruck.Arrivals.clear();
        return;
    }
    if ((aTruck.ArriveIn -= aDelta) > 0.f)
        return;
    Arrival& next = aTruck.Arrivals.front();
    bool placed = false;
    bool free = false;
    if (next.OnFoot)
    {
        free = FarFromPlayers(apPlayer, next.Start, kWalkerFromPlayer) && !CanBeSeen(apPlayer, next.Start, 180.f);
        placed = free && PlaceWalker(apPlayer, aIndex, aTruck, next.Start, next.Goal, Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]));
    }
    else
    {
        // Their start goes along with the truck: where it is now
        float at[3]{};
        float heading = 0.f;
        if (BikeLink::GetTruckRoad(aIndex, next.Ahead, next.Side, at, heading))
        {
            const NiPoint3 spot = Point(at[0], at[1], at[2]);
            free = FarFromPlayers(apPlayer, spot, kRaiderFromPlayer) && !CanBeSeen(apPlayer, spot, 180.f);
            placed = free && PlaceRider(aIndex, aTruck, next.Ahead, next.Side, spot);
        }
    }
    if (free)
    {
        // (free and still not placed: the game wouldn't give us one there, and trying again won't change that)
        if (!placed)
            spdlog::info("Caravan: a raider for truck {} couldn't be placed: one fewer", aIndex);
        aTruck.Arrivals.erase(aTruck.Arrivals.begin());
        aTruck.ArriveIn = kArriveEvery;
        return;
    }
    next.Waited += kArriveRetry;
    aTruck.ArriveIn = kArriveRetry;
    if (next.Waited >= kArriveGiveUp)
    {
        spdlog::info("Caravan: a raider for truck {} doesn't come: their start was in sight for {:.0f} s", aIndex, kArriveGiveUp);
        aTruck.Arrivals.erase(aTruck.Arrivals.begin());
        aTruck.ArriveIn = kArriveEvery;
    }
}

// The game builds a placed actor's body a second or more after, on its own: the frame it comes in is where a raid's hitch was. Logged with the frames
// either side, and what placing them took, to see what each one costs.
void CaravanService::WatchBodies(int aIndex, Truck& aTruck) noexcept
{
    for (Raider& raider : aTruck.Raiders)
    {
        if (raider.BodyStep >= 2)
            continue;
        if (raider.BodyStep == 1)
        {
            raider.BodyStep = 2;
            spdlog::info("Caravan: {:X}'s body is in for truck {}, {:.2f} s after they were placed (that took {:.1f} ms): frames {:.1f} ms before it, {:.1f} ms as it came, {:.1f} ms after",
                         raider.Id, aIndex, m_time - raider.PlacedAt, raider.PlaceMs, raider.BodyBefore, raider.BodyFrame, m_frameMs);
            continue;
        }
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || !pRaider->GetNiNode())
            continue;
        raider.BodyStep = 1;
        raider.BodyBefore = m_frameBeforeMs;
        raider.BodyFrame = m_frameMs;
    }
}

// The quest's raid. Starts round the truck, every way round at a few distances, a different way first each time; each on ground about the truck's height,
// out of every player's sight and well away from them, apart from the others. Each walks in to their own place round it (the hold-up's, the free one nearest
// the way they come from) on the game's travel package, their weapon out, unaggressive on the way so the sight of the crew doesn't start the shooting.
bool CaravanService::TryStartWalkers(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept
{
    auto* pBase = CaravanRaiderBase();
    if (!pBase)
        pBase = NemesisGame::SpawnBase("", 1, false);
    if (!pBase)
        return false;
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    const int wanted = kRidersAtLeast + rand() % (kRidersAtMost - kRidersAtLeast + 1);
    Vector<NiPoint3> starts;
    const int turn = rand() % kWalkerDirections;
    for (int d = 0; d < kWalkerDirections && static_cast<int>(starts.size()) < wanted; ++d)
    {
        // (5 is prime to 16: each next way is well round from the last)
        const float angle = 6.2831853f * static_cast<float>((turn + d * 5) % kWalkerDirections) / static_cast<float>(kWalkerDirections);
        for (int ring = 0; ring < kWalkersRings; ++ring)
        {
            const float out = kWalkersNear + (kWalkersFar - kWalkersNear) * static_cast<float>(ring) / static_cast<float>(kWalkersRings - 1);
            NiPoint3 spot = Point(at.x + std::sin(angle) * out, at.y + std::cos(angle) * out, at.z);
            if (!NemesisGame::GroundAt(spot) || std::abs(spot.z - at.z) > kWalkerUpDown)
                continue;
            if (!FarFromPlayers(apPlayer, spot, kWalkerFromPlayer) || CanBeSeen(apPlayer, spot, 180.f))
                continue;
            if (std::any_of(starts.begin(), starts.end(), [&spot](const NiPoint3& acOther) { return DistanceSquared(acOther, spot) < kWalkersApart * kWalkersApart; }))
                continue;
            starts.push_back(spot);
            break;
        }
    }
    if (static_cast<int>(starts.size()) < kRidersAtLeast)
        return false;

    // Their places round it, where the hold-up stands (ahead and to the side: the truck's forward and right)
    const float fx = std::sin(acInfo.Heading);
    const float fy = std::cos(acInfo.Heading);
    Vector<NiPoint3> places;
    for (const Slot& slot : kHoldUpSlots)
    {
        NiPoint3 place = Point(at.x + fx * slot.Ahead + fy * slot.Side, at.y + fy * slot.Ahead - fx * slot.Side, at.z);
        if (!NemesisGame::GroundAt(place))
            place.z = at.z;
        places.push_back(place);
    }
    Vector<bool> taken(places.size(), false);
    Vector<Arrival> walkers;
    for (const NiPoint3& start : starts)
    {
        size_t best = places.size();
        for (size_t p = 0; p < places.size(); ++p)
            if (!taken[p] && (best == places.size() || DistanceSquared(places[p], start) < DistanceSquared(places[best], start)))
                best = p;
        if (best == places.size())
            break;
        taken[best] = true;
        Arrival walker;
        walker.OnFoot = true;
        walker.Start = start;
        walker.Goal = places[best];
        walkers.push_back(walker);
    }
    // The first now, the rest one at a time after it (BringInRaiders)
    aTruck.Arrivals.clear();
    size_t first = 0;
    while (first < walkers.size() && !PlaceWalker(apPlayer, aIndex, aTruck, walkers[first].Start, walkers[first].Goal, at))
        ++first;
    if (aTruck.Raiders.empty())
        return false;
    for (size_t k = first + 1; k < walkers.size(); ++k)
        aTruck.Arrivals.push_back(walkers[k]);
    aTruck.ArriveIn = kArriveEvery;
    aTruck.Now = Stage::kCutOff;
    aTruck.Timer = kWalkersGiveUp;
    aTruck.PullAsked = false;
    aTruck.Helped = false;
    aTruck.OnFoot = true;
    m_nextRaid = m_time + kBetweenRaids;
    m_lastRaidAt = at;
    m_lastRaidSet = true;
    BikeLink::PullTruckOver(aIndex, true);
    spdlog::info("Caravan: {} raiders on foot close in on truck {} from round about, out of sight: {} now, the rest one every {:.2f} s", aTruck.Raiders.size() + aTruck.Arrivals.size(), aIndex,
                 aTruck.Raiders.size(), kArriveEvery);
    return true;
}

// Those walking in: there, they hold the crew at gunpoint; shot at on the way, or once it's a fight, they fight from where they are. One that can't get
// nearer is put at its place where no player sees it go or arrive, as raiders only ever appear where nobody sees.
void CaravanService::UpdateWalkers(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, float aDelta) noexcept
{
    const bool coming = aTruck.Now == Stage::kCutOff || aTruck.Now == Stage::kStandOff;
    const Vector<uint32_t> crew = aTruck.Crew;
    for (Raider& raider : aTruck.Raiders)
    {
        if (!raider.Walking)
            continue;
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || pRaider->IsDead())
        {
            StopWalking(raider, pRaider);
            continue;
        }
        if (!coming || raider.ShotBy)
        {
            StopWalking(raider, pRaider);
            spdlog::info("Caravan: {:X} stops walking in to truck {}: {}", raider.Id, aIndex, raider.ShotBy ? "shot at" : "it's a fight");
            RiderOff(aTruck, raider, pRaider, true);
            continue;
        }
        const float dx = pRaider->position.x - raider.Goal.x;
        const float dy = pRaider->position.y - raider.Goal.y;
        const float off = std::sqrt(dx * dx + dy * dy);
        if (off < raider.Best - 40.f)
        {
            raider.Best = off;
            raider.NoNearer = 0.f;
        }
        else
            raider.NoNearer += aDelta;
        if ((raider.WalkLog -= aDelta) <= 0.f)
        {
            raider.WalkLog = 5.f;
            spdlog::info("Caravan: {:X} walking in to truck {}: {:.0f} from their place", raider.Id, aIndex, off);
        }
        bool there = off < kWalkerThere;
        if (!there && raider.NoNearer >= kWalkerStuck)
        {
            if (!CanBeSeen(apPlayer, pRaider->position, 180.f) && !CanBeSeen(apPlayer, raider.Goal, 180.f))
            {
                spdlog::info("Caravan: {:X} got no nearer in {:.0f} s, {:.0f} from their place: put there, out of sight", raider.Id, kWalkerStuck, off);
                pRaider->ForcePosition(raider.Goal);
                there = true;
            }
            else
                raider.NoNearer = kWalkerStuck - 2.f;
        }
        if (!there)
            continue;
        StopWalking(raider, pRaider);
        Actor* pCrew = NearestAlive(crew, pRaider->position);
        HoldStillAtGunpoint(raider, pRaider, pCrew ? pCrew->position : raider.Goal);
        spdlog::info("Caravan: {:X} is at their place round truck {}", raider.Id, aIndex);
    }
}

void CaravanService::StopWalking(Raider& aRaider, Actor* apRaider) noexcept
{
    if (aRaider.Marker)
        BikeLink::RemoveMarker(aRaider.Marker);
    aRaider.Marker = 0;
    if (apRaider && !apRaider->IsDead() && aRaider.Aggression >= 0.f)
        NemesisGame::SetAggression(apRaider, aRaider.Aggression);
    aRaider.Aggression = -1.f;
    aRaider.Walking = false;
}

void CaravanService::HoldStillAtGunpoint(Raider& aRaider, Actor* apRaider, const NiPoint3& acFacing) noexcept
{
    NemesisGame::Restrain(apRaider, true);
    aRaider.Restrained = true;
    aRaider.OnBike = false;
    apRaider->SetRotation(0.f, 0.f, Facing(apRaider->position, acFacing));
    apRaider->SetWeaponDrawnEx(true);
    aRaider.Idle = 0.f;
}

// Every frame: those on bikes are held where the plugin has them, and shoot
void CaravanService::UpdateRaiders(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, float aDelta) noexcept
{
    // Every raider's death, with who the game says did it (seen 11-05, the 22:51 run: both raiders walking to take a truck died in the same moment, nobody
    // having hit them, and the player didn't; and the quest's raiders before them, "killed by 0")
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || !pRaider->IsDead() || raider.DeathLogged)
            continue;
        raider.DeathLogged = true;
        Actor* pKiller = nullptr;
        if (World::Get().ctx().at<PapyrusService>().Get("Actor", "GetKiller"))
        {
            PAPYRUS_FUNCTION(Actor*, Actor, GetKiller);
            pKiller = s_pGetKiller(pRaider);
        }
        BikeLink::Truck info{};
        const bool known = BikeLink::GetTruck(aIndex, info);
        const NiPoint3 at = Point(info.At[0], info.At[1], info.At[2]);
        const auto* pKillerBase = pKiller ? pKiller->baseForm : nullptr;
        spdlog::info("Caravan: {:X} at truck {} is dead, killed by {:X} (base {:X}{}), at ({:.0f}, {:.0f}, {:.0f}), {:.0f} from the truck, {:.0f} from the nearest player", raider.Id, aIndex,
                     pKiller ? pKiller->formID : 0u, pKillerBase ? pKillerBase->formID : 0u, pKiller && pKiller->formID == 0x14 ? ", the player" : "", pRaider->position.x, pRaider->position.y,
                     pRaider->position.z, known ? std::sqrt(DistanceSquared(pRaider->position, at)) : -1.f, NearestPlayer(apPlayer, pRaider->position));
    }
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || !raider.OnBike)
            continue;
        BikeLink::Rider rider{};
        if (!BikeLink::GetRider(raider.Id, rider))
        {
            raider.OnBike = false;
            continue;
        }
        if (rider.Stage >= 4)
        {
            const bool standing = rider.Stage == 4;
            BikeLink::ForgetRider(raider.Id);
            raider.OnBike = false;
            raider.Aiming = false;
            if (pRaider->IsDead())
                continue;
            if (standing)
                pRaider->ForcePosition(Point(rider.StandAt[0], rider.StandAt[1], rider.StandAt[2]));
            // Came off it hurt or got off to fight, or held at gunpoint in a hold-up
            RiderOff(aTruck, raider, pRaider, !standing || aTruck.Now != Stage::kStandOff);
            continue;
        }
        pRaider->ForcePosition(Point(rider.At[0], rider.At[1], rider.At[2]));
        // Nobody sits on a stopped bike in a fight, whatever's stopped it: off it and on foot. (Cutting a truck off, they stop on purpose, and
        // get off together when it has.)
        raider.StoppedFor = rider.Stage == 2 && rider.Speed < kStoppedSpeed && aTruck.Now != Stage::kCutOff ? raider.StoppedFor + aDelta : 0.f;
        if (raider.StoppedFor >= kStoppedFor)
            BikeLink::RiderGetOff(raider.Id, rand() % 2 == 0 ? -1 : 1);
        if (!raider.Restrained)
        {
            raider.Settle -= aDelta;
            if (pRaider->actorState.IsWeaponDrawn() || raider.Settle <= 0.f)
            {
                raider.HasGun = !NemesisGame::AmmoName(pRaider).empty();
                NemesisGame::Restrain(pRaider, true);
                raider.Restrained = true;
                spdlog::info("Caravan: {:X} on the bike, {} {}", pRaider->formID, raider.HasGun ? NemesisGame::WeaponName(pRaider) : "no gun,", pRaider->actorState.IsWeaponDrawn() ? "out" : "not out yet");
            }
        }
        if (aTruck.Now == Stage::kChase || (aTruck.Now == Stage::kCutOff && raider.ShotBy))
            AimAndFire(apPlayer, aTruck, raider, pRaider, rider, aDelta);
        else if (raider.Aiming)
        {
            raider.Aiming = false;
            BikeLink::SetRiderAim(raider.Id, false);
        }
        if (!raider.Aiming)
            pRaider->SetRotation(0.f, 0.f, rider.Heading);
    }
}

// From the bike, one arm out: at whoever's on the truck, or the player who just shot them. Short bursts, the game's own shot.
void CaravanService::AimAndFire(PlayerCharacter* apPlayer, Truck& aTruck, Raider& aRaider, Actor* apRaider, const BikeLink::Rider& acRider, float aDelta) noexcept
{
    aRaider.NextShot -= aDelta;
    const auto stopAiming = [&]()
    {
        if (aRaider.Aiming)
        {
            aRaider.Aiming = false;
            aRaider.AimedFor = 0.f;
            BikeLink::SetRiderAim(aRaider.Id, false);
        }
    };
    if (!aRaider.Restrained || !aRaider.HasGun || acRider.Stage != 2)
        return stopAiming();

    Actor* pTarget = nullptr;
    if (Actor* pShooter = ActorById(aRaider.ShotBy); pShooter && !pShooter->IsDead() && DistanceSquared(pShooter->position, apRaider->position) < kShootReach * kShootReach)
        pTarget = pShooter;
    if (!pTarget)
        pTarget = NearestAlive(aTruck.Crew, apRaider->position);
    if (!pTarget)
        return stopAiming();

    const NiPoint3 muzzle = Point(acRider.At[0], acRider.At[1], acRider.At[2] + kMuzzleHeight);
    const NiPoint3 aim = Point(pTarget->position.x, pTarget->position.y, pTarget->position.z + kTargetHeight);
    const float dx = aim.x - muzzle.x;
    const float dy = aim.y - muzzle.y;
    const float dz = aim.z - muzzle.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float heading = std::atan2(dx, dy);
    if (length > kShootReach || length < 1.f || std::abs(Wrap(heading - acRider.Heading)) > kShootAngle)
        return stopAiming();

    if (!aRaider.Aiming)
    {
        aRaider.Aiming = true;
        aRaider.AimedFor = 0.f;
        BikeLink::SetRiderAim(aRaider.Id, true);
    }
    aRaider.AimedFor += aDelta;
    // (the game's pitch is positive looking down)
    apRaider->SetRotation(-std::asin(std::clamp(dz / length, -1.f, 1.f)), 0.f, heading);

    if (!apRaider->actorState.IsWeaponDrawn())
    {
        if ((aRaider.Draw -= aDelta) <= 0.f)
        {
            aRaider.Draw = 2.f;
            apRaider->SetWeaponDrawnEx(true);
        }
        return;
    }
    if (aRaider.AimedFor < kAimFirst || aRaider.NextShot > 0.f)
        return;
    TESObjectCELL* pCell = apPlayer->GetParentCellEx();
    if (!pCell || !LineIsClear(pCell, WorldRays::GetLineOfSightFilter(apPlayer), muzzle, aim))
    {
        aRaider.NextShot = 0.4f;
        return;
    }
    FireGun(apRaider);
    if (aRaider.Burst > 0)
    {
        --aRaider.Burst;
        aRaider.NextShot = 0.25f + Random01() * 0.35f;
    }
    else
    {
        aRaider.Burst = 1 + rand() % 3;
        aRaider.NextShot = 1.4f + Random01() * 1.8f;
    }
}

void CaravanService::RiderOff(Truck& aTruck, Raider& aRaider, Actor* apRaider, bool aFight) noexcept
{
    // (taking the truck, its people are raiders too: nobody to go for but whoever shot them)
    const bool takingIt = aTruck.Now == Stage::kTakeOver || aTruck.Now == Stage::kTaken;
    Vector<uint32_t> crew = takingIt ? Vector<uint32_t>{} : aTruck.Crew;
    if (aFight)
    {
        NemesisGame::Restrain(apRaider, false);
        aRaider.Restrained = false;
        IdleStop(apRaider);
        Actor* pTarget = ActorById(aRaider.ShotBy);
        if (!pTarget || pTarget->IsDead())
            pTarget = NearestAlive(crew, apRaider->position);
        if (pTarget)
            NemesisGame::Attack(apRaider, pTarget);
        return;
    }
    Actor* pCrew = NearestAlive(crew, apRaider->position);
    HoldStillAtGunpoint(aRaider, apRaider, pCrew ? pCrew->position : apRaider->position);
}

void CaravanService::WatchChase(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    int alive = 0;
    float nearest = 1e9f;
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || pRaider->IsDead())
            continue;
        ++alive;
        nearest = std::min(nearest, std::sqrt(DistanceSquared(pRaider->position, at)));
        // Not holding one place behind it the whole way
        if (raider.OnBike && (raider.NewSlot -= aDelta) <= 0.f)
        {
            raider.NewSlot = 7.f + Random01() * 8.f;
            const Slot& slot = kChaseSlots[rand() % std::size(kChaseSlots)];
            BikeLink::SetRiderGoal(raider.Id, slot.Ahead, slot.Side);
        }
    }
    // (none alive with some still to come: it isn't over)
    if (alive == 0 && !aTruck.Arrivals.empty())
        return;
    if (alive == 0)
        return RaidersBeaten(apPlayer, aIndex, aTruck, acInfo);
    const int crew = CountAlive(aTruck.Crew);
    if (crew == 0)
        return LoseTruck(aIndex, aTruck);
    // The driver's hit, it's been long enough, or it's all getting too far from anyone: they run it off the road
    const Actor* pDriver = aTruck.Crew.empty() ? nullptr : ActorById(aTruck.Crew[0]);
    const bool driverDown = !pDriver || pDriver->IsDead();
    const bool tooFar = nearest < 1500.f && NearestPlayer(apPlayer, at) > kChaseTooFar;
    // Its stop, or anything else that's stopped it, is where the chase ends. It's a fight from there: the truck doesn't rest and drive off out of it.
    aTruck.StoppedFor = acInfo.Speed < kStoppedSpeed ? aTruck.StoppedFor + aDelta : 0.f;
    const bool atItsStop = !acInfo.OnRoad || acInfo.AlongFromEnd < kChaseRoadEnd;
    const bool stopped = aTruck.StoppedFor >= kStoppedFor;
    if ((aTruck.Timer -= aDelta) <= 0.f || driverDown || tooFar || atItsStop || stopped)
    {
        spdlog::info("Caravan: raiders run truck {} off the road ({})", aIndex, driverDown ? "the driver's down" : tooFar ? "far from anyone" : atItsStop ? "at its stop" : stopped ? "it's stopped" : "long enough");
        StartFootFight(aIndex, aTruck);
    }
}

void CaravanService::WatchCutOff(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    int alive = 0;
    float nearest = 1e9f;
    size_t next = 0;
    bool walking = false;
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || pRaider->IsDead())
            continue;
        ++alive;
        walking |= raider.Walking;
        nearest = std::min(nearest, std::sqrt(DistanceSquared(pRaider->position, at)));
        if (raider.OnBike)
        {
            const Slot& slot = kCutOffSlots[next++ % std::size(kCutOffSlots)];
            BikeLink::SetRiderGoal(raider.Id, slot.Ahead, slot.Side);
        }
    }
    // (none alive with some still to come: it isn't over; and on foot, those still to come are walking in too)
    walking |= aTruck.OnFoot && !aTruck.Arrivals.empty();
    if (alive == 0 && !aTruck.Arrivals.empty())
        return;
    if (alive == 0)
        return RaidersBeaten(apPlayer, aIndex, aTruck, acInfo);
    if (CountAlive(aTruck.Crew) == 0)
        return LoseTruck(aIndex, aTruck);
    if (!aTruck.PullAsked && nearest < 1200.f)
    {
        aTruck.PullAsked = true;
        BikeLink::PullTruckOver(aIndex, true);
    }
    aTruck.Timer -= aDelta;
    // (the quest's truck is stopped already: it's them getting there that counts. On foot, that's all of them at their places.)
    const bool waiting = aTruck.OnFoot ? walking : aIndex == m_questTruck ? nearest > kQuestRaidersClose : !acInfo.Pulled;
    if (waiting && aTruck.Timer > 0.f)
        return;

    // Stopped, or as near as it's going to be: off the bikes and the crew's held there
    BikeLink::PullTruckOver(aIndex, true);
    for (Raider& raider : aTruck.Raiders)
    {
        if (raider.OnBike)
            BikeLink::RiderGetOff(raider.Id, rand() % 2 == 0 ? -1 : 1);
    }
    aTruck.Now = Stage::kStandOff;
    aTruck.Timer = BikeLink::TruckTestMode() ? 8.f : kStandOffLeast + Random01() * kStandOffSpread;
    aTruck.Seen = true;
    spdlog::info("Caravan: truck {} stopped, {} raiders around it", aIndex, alive);
}

// Nobody's fired yet. A player gets close, or has been watching long enough, or someone shoots, and it starts.
void CaravanService::WatchStandOff(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    if (!aTruck.CrewDown)
    {
        SeatCrew(aIndex, aTruck);
        if (acInfo.Speed < kCrewDownSpeed)
            DropCrew(aIndex, aTruck, false, nullptr);
    }
    // The poses held, in case something plays over them
    if ((aTruck.Idle -= aDelta) <= 0.f)
    {
        aTruck.Idle = 3.f;
        for (const uint32_t id : aTruck.Crew)
        {
            Actor* pCrew = ActorById(id);
            if (aTruck.CrewDown && pCrew && !pCrew->IsDead())
                PlayIdleOn(pCrew, kCowerIdle);
        }
        for (Raider& raider : aTruck.Raiders)
        {
            Actor* pRaider = ActorById(raider.Id);
            if (!pRaider || pRaider->IsDead() || raider.OnBike || raider.Walking)
                continue;
            if (!pRaider->actorState.IsWeaponDrawn())
                pRaider->SetWeaponDrawnEx(true);
            PlayIdleOn(pRaider, kHoldUpIdle);
        }
    }
    int alive = 0;
    bool anyDead = false;
    bool anyOnBike = false;
    for (const Raider& raider : aTruck.Raiders)
    {
        const Actor* pRaider = ActorById(raider.Id);
        if (pRaider && !pRaider->IsDead())
            ++alive;
        else
            anyDead = true;
        // (one still walking in counts as one still riding up: nothing goes on till they're all there)
        anyOnBike |= raider.OnBike || raider.Walking;
    }
    if (alive == 0)
        return RaidersBeaten(apPlayer, aIndex, aTruck, acInfo);
    if (!aTruck.Seen && (aTruck.Look -= aDelta) <= 0.f)
    {
        aTruck.Look = kRaidRetry;
        aTruck.Seen = CanBeSeen(apPlayer, at, 190.f);
        if (aTruck.Seen)
            spdlog::info("Caravan: the hold-up at truck {} is seen", aIndex);
    }
    const float nearest = NearestPlayer(apPlayer, at);
    // Those on the quest's truck hold it for as long as it takes someone to come: nobody shoots, and nobody wins, with nobody there to see it (the 01:43 run: over
    // in 10 s from 5,000 away, the quest done and the truck gone by the time the player got there). Stopping combat wasn't enough: in the 04:38 run they were all dead every time, in the
    // same moment, 20.2 s after the truck stopped, with no sign of a fight (13 raids of it in that run and the one before). What kills them isn't known, and the lines below are to find out; until then
    // they can't die. A player's shot at them from further off: it's on.
    const bool questHold = aIndex == m_questTruck && nearest >= kQuestStandOffNear && !aTruck.Helped;
    if (questHold && !aTruck.QuestHold)
        HoldForQuest(aIndex, aTruck);
    else if (!questHold)
        ReleaseHold(aIndex, aTruck);
    if (anyOnBike)
        return;
    if (questHold)
    {
        if ((aTruck.Look -= aDelta) > 0.f)
            return;
        aTruck.Look = kRaidRetry;
        HoldForQuest(aIndex, aTruck);
        const bool sayAll = BikeLink::TruckTestMode() && (aTruck.HoldLog -= kRaidRetry) <= 0.f;
        if (sayAll)
            aTruck.HoldLog = kHoldLogEvery;
        for (const uint32_t id : aTruck.Crew)
            if (Actor* pCrew = ActorById(id); pCrew && !pCrew->IsDead())
                pCrew->StopCombat();
        for (Raider& raider : aTruck.Raiders)
            if (Actor* pRaider = ActorById(raider.Id); pRaider && !pRaider->IsDead())
            {
                pRaider->StopCombat();
                const float health = NemesisGame::HealthPercent(pRaider);
                if (sayAll || health < raider.HeldHealth - 0.01f)
                    spdlog::info("Caravan: {:X} held at truck {}: {:.0f}% health (was {:.0f}%), at ({:.0f}, {:.0f}, {:.0f}), the truck at {:.0f} high", raider.Id, aIndex, health * 100.f, raider.HeldHealth * 100.f, pRaider->position.x, pRaider->position.y, pRaider->position.z, at.z);
                raider.HeldHealth = health;
            }
        return;
    }
    // Their demand first, with an answer, in anyone's hearing
    if (aTruck.DemandStep < kDemandSteps && (aTruck.Seen || nearest < kDemandHeard))
        SayDemand(aIndex, aTruck, aDelta, nearest < kDemandHeard);
    if (aTruck.Seen)
        aTruck.Timer -= aDelta;
    const bool demanded = aTruck.DemandStep >= kDemandSteps;
    if (anyDead || (demanded && (aTruck.Timer <= 0.f || nearest < kStandOffNear)))
    {
        spdlog::info("Caravan: the hold-up at truck {} turns into a fight: {}", aIndex, anyDead ? "someone's down" : aTruck.Timer <= 0.f ? "time's up" : "a player's close");
        if (aTruck.DemandSpeaker)
            if (Actor* pSpeaker = ActorById(aTruck.DemandSpeaker))
                if (auto* pSubtitles = SubtitleManager::Get())
                    pSubtitles->HideSubtitle(pSpeaker);
        StartFootFight(aIndex, aTruck);
    }
}

// Has a player got far enough for raids? Their level, or having got to Diamond City: remembered by the save's map as much as by being there
bool CaravanService::EncountersOpen() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return false;
    int level = pPlayer->GetLevel();
    // Any of the party, as this game looks after the party's people
    const auto& party = m_world.ctx().at<PartyService>();
    if (party.IsInParty())
        for (const uint32_t player : party.GetPartyMembers())
            if (const auto it = m_playerLevels.find(player); it != m_playerLevels.end())
                level = std::max(level, it->second);
    bool diamondCity = false;
    for (const uint32_t markerId : kDiamondCityMarkers)
    {
        auto* pMarker = Cast<TESObjectREFR>(TESForm::GetById(markerId));
        ExtraDataList* pExtra = pMarker ? pMarker->GetExtraDataList() : nullptr;
        BSExtraData* pData = pExtra ? pExtra->GetByType(ExtraDataType::MapMarker) : nullptr;
        const uint8_t* pFlags = pData ? *reinterpret_cast<uint8_t* const*>(reinterpret_cast<const uint8_t*>(pData) + 0x18) : nullptr;
        // (flags 2: discovered, can be travelled to)
        diamondCity |= pFlags && (pFlags[0x10] & 2) != 0;
    }
    // Asked each time, not remembered: another save can be loaded without leaving the game
    const bool open = level >= kEncountersFromLevel || diamondCity;
    if (open != m_encountersOpen)
    {
        m_encountersOpen = open;
        spdlog::info("Caravan: encounters {}: level {}, Diamond City {}", open ? "open" : "closed", level, diamondCity ? "reached" : "not yet");
    }
    return open;
}

void CaravanService::SayDemand(int aIndex, Truck& aTruck, float aDelta, bool aHeard) noexcept
{
    if (aHeard)
        WinterOfAtom::KeepSubtitlesOn();
    if ((aTruck.DemandIn -= aDelta) > 0.f)
        return;
    aTruck.DemandIn = kDemandLineFor;
    auto* pSubtitles = SubtitleManager::Get();
    if (aTruck.DemandSpeaker && pSubtitles)
        if (Actor* pLast = ActorById(aTruck.DemandSpeaker))
            pSubtitles->HideSubtitle(pLast);
    aTruck.DemandSpeaker = 0;
    if (aTruck.DemandStep >= kDemandSteps - 1)
    {
        aTruck.DemandStep = kDemandSteps;
        return;
    }
    // A raider, a guard, a raider
    const bool raidersTurn = aTruck.DemandStep % 2 == 0;
    Actor* pSpeaker = nullptr;
    if (raidersTurn)
    {
        for (const Raider& raider : aTruck.Raiders)
            if (Actor* pRaider = ActorById(raider.Id); pRaider && !pRaider->IsDead())
            {
                pSpeaker = pRaider;
                break;
            }
    }
    else
    {
        for (const uint32_t id : aTruck.Crew)
            if (Actor* pCrew = ActorById(id); pCrew && !pCrew->IsDead())
            {
                pSpeaker = pCrew;
                break;
            }
    }
    const char* pLine = raidersTurn ? kDemandLines[aTruck.DemandStep == 0 ? rand() % 2 : 2] : kCrewAnswers[rand() % std::size(kCrewAnswers)];
    if (pSpeaker && pSubtitles && aHeard)
    {
        pSubtitles->ShowSubtitle(pSpeaker, pLine, nullptr);
        aTruck.DemandSpeaker = pSpeaker->formID;
    }
    spdlog::info("Caravan: truck {} hold-up: \"{}\"", aIndex, pLine);
    ++aTruck.DemandStep;
}

void CaravanService::StartFootFight(int aIndex, Truck& aTruck) noexcept
{
    ReleaseHold(aIndex, aTruck);
    BikeLink::PullTruckOver(aIndex, true);
    Vector<uint32_t> crew = aTruck.Crew;
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || pRaider->IsDead())
            continue;
        if (raider.OnBike)
            BikeLink::RiderGetOff(raider.Id, rand() % 2 == 0 ? -1 : 1);
        else
            RiderOff(aTruck, raider, pRaider, true);
    }
    if (aTruck.CrewDown)
        DropCrew(aIndex, aTruck, true, nullptr);
    aTruck.Now = Stage::kFight;
    aTruck.Timer = 1.f;
}

void CaravanService::WatchFight(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    if (!aTruck.CrewDown)
    {
        SeatCrew(aIndex, aTruck);
        if (acInfo.Speed < kCrewDownSpeed)
            DropCrew(aIndex, aTruck, true, nullptr);
    }
    if ((aTruck.Timer -= aDelta) > 0.f)
        return;
    aTruck.Timer = 1.f;
    int raiders = 0;
    for (const Raider& raider : aTruck.Raiders)
    {
        const Actor* pRaider = ActorById(raider.Id);
        if (pRaider && !pRaider->IsDead())
            ++raiders;
    }
    if (CountAlive(aTruck.Crew) == 0)
        return LoseTruck(aIndex, aTruck);
    if (raiders == 0)
        RaidersBeaten(apPlayer, aIndex, aTruck, acInfo);
}

// With no player near, nobody on the quest's truck can die: not by whatever's been killing raiders, and not by each other. Taken off when a player's near, or has shot at them.
void CaravanService::HoldForQuest(int aIndex, Truck& aTruck) noexcept
{
    // (again each second, for anyone new)
    for (const uint32_t id : aTruck.Crew)
        if (Actor* pCrew = ActorById(id); pCrew && !pCrew->IsDead())
            pCrew->SetEssential(true);
    for (const Raider& raider : aTruck.Raiders)
        if (Actor* pRaider = ActorById(raider.Id); pRaider && !pRaider->IsDead())
            pRaider->SetEssential(true);
    if (!aTruck.QuestHold)
    {
        aTruck.QuestHold = true;
        aTruck.HoldLog = 0.f;
        spdlog::info("Caravan: nobody dies at truck {} until a player's near", aIndex);
    }
}

void CaravanService::ReleaseHold(int aIndex, Truck& aTruck) noexcept
{
    if (!aTruck.QuestHold)
        return;
    aTruck.QuestHold = false;
    // (whatever happened to them with nobody there, it isn't to count against anyone)
    const auto release = [](Actor* apActor) {
        apActor->SetEssential(false);
        const float most = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
        if (most > 0.f && apActor->GetActorValue(ActorValueInfo::kHealth) < most)
            apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, most);
    };
    for (const uint32_t id : aTruck.Crew)
        if (Actor* pCrew = ActorById(id); pCrew && !pCrew->IsDead())
            release(pCrew);
    for (const Raider& raider : aTruck.Raiders)
        if (Actor* pRaider = ActorById(raider.Id); pRaider && !pRaider->IsDead())
            release(pRaider);
    spdlog::info("Caravan: truck {}'s people can die again", aIndex);
}

// A player helped, or at least was there: it pulls over and says so. Nobody near: they fought them off themselves and go on
void CaravanService::RaidersBeaten(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept
{
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    // All at once, from full health, essential or not, with nobody having hit them (the 13:09 run): who the game says did it
    if (World::Get().ctx().at<PapyrusService>().Get("Actor", "GetKiller"))
    {
        PAPYRUS_FUNCTION(Actor*, Actor, GetKiller);
        for (const Raider& raider : aTruck.Raiders)
            if (Actor* pRaider = ActorById(raider.Id))
            {
                Actor* pKiller = s_pGetKiller(pRaider);
                spdlog::info("Caravan: {:X} at truck {} is dead, killed by {:X}, at ({:.0f}, {:.0f}, {:.0f})", raider.Id, aIndex, pKiller ? pKiller->formID : 0u, pRaider->position.x, pRaider->position.y, pRaider->position.z);
            }
    }
    // The quest's raid has to be there when they come: whatever got these, with nobody there it's no win. Bodies stay, others come, the quest goes on.
    if (aIndex == m_questTruck && !aTruck.Helped && NearestPlayer(apPlayer, at) >= kQuestStandOffNear)
    {
        spdlog::warn("Caravan: the raiders at truck {} are dead with no player near, {} of its crew left: others come, the quest goes on", aIndex, CountAlive(aTruck.Crew));
        LeaveBehind(aTruck);
        if (aTruck.CrewDown)
            StopCowering(aTruck.Crew);
        for (const uint32_t id : aTruck.Crew)
            if (Actor* pActor = ActorById(id); pActor && !pActor->IsDead())
            {
                pActor->StopCombat();
                NemesisGame::Restrain(pActor, true);
            }
        aTruck.CrewDown = false;
        aTruck.CrewFree = false;
        aTruck.Seat = 0.f;
        aTruck.Planned = Plan::kNone;
        aTruck.Now = Stage::kRiding;
        m_questRaidOn = false;
        m_questLook = kQuestRaidAgain;
        return;
    }
    ReleaseHold(aIndex, aTruck);
    aTruck.Planned = Plan::kNone;
    const bool helped = aTruck.Helped || NearestPlayer(apPlayer, at) < kHelpReach;
    LeaveBehind(aTruck);
    spdlog::info("Caravan: the raiders on truck {} are beaten, {} of its crew left, {}", aIndex, CountAlive(aTruck.Crew), helped ? "a player helped" : "nobody saw");
    if (aIndex == m_questTruck)
        EndCaravanQuest(true, helped);
    // Held cowering through it, they stand up now: for the thanks, or to wait by the truck before they're back on
    if (aTruck.CrewDown && !aTruck.CrewFree)
        StopCowering(aTruck.Crew);
    if (helped)
    {
        BikeLink::PullTruckOver(aIndex, true);
        aTruck.Now = Stage::kPullOver;
        return;
    }
    if (aTruck.CrewDown)
    {
        aTruck.Now = Stage::kAfter;
        aTruck.Timer = 12.f;
    }
    else
        BackOnTheRoad(aIndex, aTruck);
}

// The driver gets out and thanks whoever's nearest, and until they leave there's trading to be had. Non-voiced: the game's
// subtitle, with the player's own subtitle settings set aside for it (as for Winter of Atom's silent lines).
void CaravanService::BeginThanks(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck) noexcept
{
    Actor* pTrader = nullptr;
    for (const uint32_t id : aTruck.Crew)
    {
        Actor* pActor = ActorById(id);
        if (pActor && !pActor->IsDead())
        {
            pTrader = pActor;
            break;
        }
    }
    if (!pTrader)
        return LoseTruck(aIndex, aTruck);

    // Those who were down stop fighting and stand; those still up there stay in their seats
    for (const uint32_t id : aTruck.Crew)
    {
        Actor* pActor = ActorById(id);
        if (!pActor || pActor->IsDead())
            continue;
        if (aTruck.CrewDown)
        {
            pActor->StopCombat();
            NemesisGame::Restrain(pActor, true);
        }
    }
    aTruck.Trader = pTrader->formID;
    if (!aTruck.CrewDown)
    {
        float at[3]{};
        float heading = 0.f;
        if (BikeLink::GetTruckSeat(aIndex, -1, at, heading))
        {
            NiPoint3 ground = Point(at[0], at[1], at[2] + 100.f);
            if (NemesisGame::GroundAt(ground))
                pTrader->ForcePosition(ground);
        }
        IdleStop(pTrader);
    }
    Actor* pNearest = nullptr;
    (void)NearestPlayer(apPlayer, pTrader->position, &pNearest);
    if (pNearest)
        pTrader->SetRotation(0.f, 0.f, Facing(pTrader->position, pNearest->position));
    BikeLink::SetTrader(pTrader->formID, aIndex);
    aTruck.Now = Stage::kThanks;
    aTruck.SaidThanks = false;
    aTruck.Timer = 1.f;
    aTruck.SinceThanks = 0.f;
    aTruck.AwayFor = 0.f;
    spdlog::info("Caravan: the trader of truck {} gets out", aIndex);
}

void CaravanService::WatchThanks(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    if (!aTruck.CrewDown)
        SeatCrew(aIndex, aTruck);
    Actor* pTrader = ActorById(aTruck.Trader);
    if (!pTrader || pTrader->IsDead())
    {
        BikeLink::SetTrader(0, -1);
        aTruck.Trader = 0;
        if (CountAlive(aTruck.Crew) == 0)
            return LoseTruck(aIndex, aTruck);
        return BackOnTheRoad(aIndex, aTruck);
    }

    Actor* pNearest = nullptr;
    const float nearest = NearestPlayer(apPlayer, pTrader->position, &pNearest);
    if (pNearest && nearest < kThanksAway)
        pTrader->SetRotation(0.f, 0.f, Facing(pTrader->position, pNearest->position));

    if (!aTruck.SaidThanks && (aTruck.Timer -= aDelta) <= 0.f)
    {
        aTruck.SaidThanks = true;
        aTruck.SubtitleFor = kLineSeconds;
        WinterOfAtom::KeepSubtitlesOn();
        // One of the player's own caravans knows who it's working for
        const char* pHome = BikeLink::TruckHome(aIndex);
        const std::string homeLine = pHome && *pHome ? fmt::format("Thanks, boss. {} would've gone without if you hadn't come along.", pHome) : std::string{};
        const char* pLine = !homeLine.empty() ? homeLine.c_str() : kThanksLines[rand() % std::size(kThanksLines)];
        if (auto* pSubtitles = SubtitleManager::Get())
            pSubtitles->ShowSubtitle(pTrader, pLine, nullptr);
        spdlog::info("Caravan: \"{}\"", pLine);
    }
    if (aTruck.SubtitleFor > 0.f)
    {
        WinterOfAtom::KeepSubtitlesOn();
        if ((aTruck.SubtitleFor -= aDelta) <= 0.f)
        {
            if (auto* pSubtitles = SubtitleManager::Get())
                pSubtitles->HideSubtitle(pTrader);
        }
    }

    // Not going anywhere while there's trade going on, and they don't wait all day
    if (BikeLink::TakeTraded())
        aTruck.SinceThanks = 0.f;
    if (NemesisGame::IsBusy())
        return;
    aTruck.SinceThanks += aDelta;
    aTruck.AwayFor = nearest > kThanksAway ? aTruck.AwayFor + aDelta : 0.f;
    (void)acInfo;
    // Walked off: they get back in and go. They're put back on their seats, so not in front of someone looking right at them, unless that goes on a while.
    bool lookingAway = true;
    if (pNearest)
    {
        const float toTruck = Facing(pNearest->position, pTrader->position);
        lookingAway = std::cos(toTruck - pNearest->rotation.z) < kThanksLookingAt;
    }
    if ((aTruck.AwayFor >= kThanksAwayFor && lookingAway) || aTruck.AwayFor >= kThanksAwayAnyway || aTruck.SinceThanks >= kThanksMost)
    {
        spdlog::info("Caravan: the trader of truck {} goes back to the truck", aIndex);
        BackOnTheRoad(aIndex, aTruck);
    }
}

// Everyone back on and away, the raiders' bodies left where they fell
void CaravanService::BackOnTheRoad(int aIndex, Truck& aTruck) noexcept
{
    if (aTruck.Trader)
        BikeLink::SetTrader(0, -1);
    if (Actor* pTrader = ActorById(aTruck.Trader); pTrader && aTruck.SubtitleFor > 0.f)
    {
        if (auto* pSubtitles = SubtitleManager::Get())
            pSubtitles->HideSubtitle(pTrader);
    }
    aTruck.Trader = 0;
    aTruck.SubtitleFor = 0.f;
    LeaveBehind(aTruck);
    // (whatever pose they're in on the ground ends before they're on their seats, the cower above all)
    if (aTruck.CrewDown)
        StopCowering(aTruck.Crew);
    for (const uint32_t id : aTruck.Crew)
    {
        if (Actor* pActor = ActorById(id); pActor && !pActor->IsDead())
        {
            pActor->StopCombat();
            NemesisGame::Restrain(pActor, true);
        }
    }
    aTruck.CrewDown = false;
    aTruck.CrewFree = false;
    aTruck.Seat = 0.f;
    aTruck.Planned = Plan::kNone;
    aTruck.Now = Stage::kRiding;
    BikeLink::PullTruckOver(aIndex, false);
    BikeLink::SetTruckHold(aIndex, false);
    spdlog::info("Caravan: truck {} goes on, {} of its crew left", aIndex, CountAlive(aTruck.Crew));
}

// Four times a second: has a player gone for the crew? Hitting one, or E on the truck. One stray hit in the middle of a raid isn't
// plundering, three are. A hit on a raider is help.
void CaravanService::CheckAttacked(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, float aDelta) noexcept
{
    if ((aTruck.Watch -= aDelta) > 0.f)
        return;
    aTruck.Watch = 0.25f;
    if ((aTruck.HitWindow -= 0.25f) <= 0.f)
        aTruck.CrewHits = 0;

    for (Raider& raider : aTruck.Raiders)
    {
        const uint32_t attacker = BikeLink::TakeHit(raider.Id);
        if (attacker && !raider.HitLogged)
        {
            raider.HitLogged = true;
            spdlog::info("Caravan: {:X} at truck {} first hit by {:X}", raider.Id, aIndex, attacker);
        }
        if (attacker && IsAPlayer(attacker))
        {
            raider.ShotBy = attacker;
            aTruck.Helped = true;
        }
    }
    // (raiders on it, or on their way to it, are its people now: shooting them isn't going for the cargo)
    if (aTruck.Now == Stage::kPlunder || aTruck.Now == Stage::kLost || aTruck.Now == Stage::kTakeOver || aTruck.Now == Stage::kTaken)
        return;
    for (const uint32_t id : aTruck.Crew)
    {
        const uint32_t attacker = BikeLink::TakeHit(id);
        if (attacker && IsAPlayer(attacker))
        {
            ++aTruck.CrewHits;
            aTruck.HitWindow = 8.f;
            aTruck.Attacker = attacker;
        }
    }
    const bool raidOn = aTruck.Now == Stage::kChase || aTruck.Now == Stage::kCutOff || aTruck.Now == Stage::kStandOff || aTruck.Now == Stage::kFight;
    const bool asked = BikeLink::TakeTruckPlunder(aIndex);
    if (asked)
        aTruck.Attacker = apPlayer->formID;
    if (asked || aTruck.CrewHits >= (raidOn ? 3 : 1))
        StartPlunder(apPlayer, aIndex, aTruck);
}

void CaravanService::StartPlunder(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck) noexcept
{
    ReleaseHold(aIndex, aTruck);
    Actor* pAttacker = ActorById(aTruck.Attacker);
    if (!pAttacker)
        pAttacker = apPlayer;
    if (aTruck.Trader)
        BikeLink::SetTrader(0, -1);
    if (Actor* pTrader = ActorById(aTruck.Trader); pTrader && aTruck.SubtitleFor > 0.f)
    {
        if (auto* pSubtitles = SubtitleManager::Get())
            pSubtitles->HideSubtitle(pTrader);
    }
    aTruck.Trader = 0;
    aTruck.SubtitleFor = 0.f;
    aTruck.Planned = Plan::kNone;
    aTruck.Now = Stage::kPlunder;
    // One of their own settlements' caravans: it'll hear about it
    if (const char* pHome = BikeLink::TruckHome(aIndex); !aTruck.HomeHurt && pHome && *pHome)
    {
        aTruck.HomeHurt = true;
        ChangeHappiness(aIndex, kHappinessPlundered);
        BikeLink::Notify(fmt::format("That caravan was carrying supplies for {}", pHome).c_str());
        spdlog::info("Caravan: truck {} carries for {}, who won't like this", aIndex, pHome);
    }
    if (aIndex == m_questTruck)
        EndCaravanQuest(false, false);
    aTruck.Timer = 0.f;
    aTruck.AwayFor = 0.f;
    BikeLink::PullTruckOver(aIndex, true);
    // Whoever's still on foot and on bikes in a raid carries on as they were
    if (aTruck.CrewDown)
        DropCrew(aIndex, aTruck, true, pAttacker);
    spdlog::info("Caravan: {:X} goes for truck {}: its crew fights", pAttacker->formID, aIndex);
}

// The crew fights; they come off the truck as it slows. Dead, the cargo's there. The player's gone, they go on.
void CaravanService::WatchPlunder(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    Actor* pAttacker = ActorById(aTruck.Attacker);
    if (!pAttacker || pAttacker->IsDead())
        pAttacker = apPlayer;
    if (!aTruck.CrewDown)
    {
        SeatCrew(aIndex, aTruck);
        if (acInfo.Speed < kCrewDownSpeed)
            DropCrew(aIndex, aTruck, true, pAttacker);
    }
    if ((aTruck.Timer -= aDelta) > 0.f)
        return;
    aTruck.Timer = 1.f;
    if (CountAlive(aTruck.Crew) == 0)
        return LoseTruck(aIndex, aTruck);
    for (const uint32_t id : aTruck.Crew)
    {
        Actor* pActor = ActorById(id);
        if (aTruck.CrewDown && pActor && !pActor->IsDead() && !pActor->IsInCombat())
            NemesisGame::Attack(pActor, pAttacker);
    }
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    aTruck.AwayFor = NearestPlayer(apPlayer, at) > kRaidReach ? aTruck.AwayFor + 1.f : 0.f;
    if (aTruck.AwayFor >= 20.f)
    {
        spdlog::info("Caravan: the crew of truck {} gave up on the player", aIndex);
        BackOnTheRoad(aIndex, aTruck);
    }
}

void CaravanService::ChangeHappiness(int aIndex, float aAmount) noexcept
{
    auto* pWorkshop = Cast<TESObjectREFR>(TESForm::GetById(BikeLink::TruckHomeWorkshop(aIndex)));
    auto* pRating = Cast<ActorValueInfo>(TESForm::GetById(kWorkshopRatingHappinessModifier));
    if (!pWorkshop || !pRating || !World::Get().ctx().at<PapyrusService>().Get("ObjectReference", "ModValue"))
        return;
    using ObjectReference = TESObjectREFR;
    PAPYRUS_FUNCTION(void, ObjectReference, ModValue, ActorValueInfo*, float);
    s_pModValue(pWorkshop, pRating, aAmount);
    spdlog::info("Caravan: {:X}'s happiness modified by {:.0f}", pWorkshop->formID, aAmount);
}

// Every so often, somewhere out on the roads, raiders stop a caravan carrying for one of the player's settlements. The game tells them as a quest
// does, its objective pointing at the place. The truck stays stopped there until they come or it's too late. Near enough that it's a real object, it's
// the same hold-up as any other: raiders come up the road from out of sight, and hold the crew at gunpoint until a player's coming.
void CaravanService::UpdateCaravanQuest(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    auto* pQuest = Cast<TESQuest>(TESForm::GetById(BikeLink::CaravanQuest()));
    if (!pQuest)
        return;
    if (m_questStopIn > 0.f)
    {
        if ((m_questStopIn -= aDelta) <= 0.f)
            pQuest->SetStopped();
    }
    else if (m_questTruck < 0 && pQuest->IsRunning())
    {
        // From a save made while one was on: the truck and the raiders weren't saved with it
        spdlog::info("Caravan: a caravan quest from a save is over");
        pQuest->SetStopped();
    }
    if (m_questTruck >= 0)
    {
        // The game starts a quest a little later than it's asked to (the first run: this took that for a save, stopped it and left the truck where it was hit)
        if (!m_questShown)
        {
            if (pQuest->IsRunning())
                ShowCaravanQuest(pQuest);
            else if ((m_questStarting -= aDelta) <= 0.f)
            {
                spdlog::warn("Caravan: the caravan quest never started");
                BikeLink::RemoveMarker(m_questMarker);
                BikeLink::EndRaidedCaravan(m_questTruck);
                m_questTruck = -1;
                m_questMarker = 0;
            }
            return;
        }
        if (!pQuest->IsRunning())
        {
            // A save from before it, loaded
            BikeLink::EndRaidedCaravan(m_questTruck);
            m_questTruck = -1;
            m_questMarker = 0;
            m_questRaidOn = false;
            return;
        }
        if (m_trucks[m_questTruck].Now == Stage::kAway && (m_questLeft -= aDelta) <= 0.f)
        {
            spdlog::info("Caravan: nobody came to truck {}", m_questTruck);
            BikeLink::SetTruckLost(m_questTruck, kLostDays);
            EndCaravanQuest(false, false);
        }
        return;
    }
    if (m_nextQuest < 0.f)
        m_nextQuest = BikeLink::TruckTestMode() ? kQuestTestWait : kQuestFirst;
    if ((m_nextQuest -= aDelta) > 0.f || NemesisGame::IsBusy())
        return;
    m_nextQuest = kQuestRetry;
    if (!EncountersOpen())
        return;
    StartCaravanQuest(apPlayer, pQuest);
}

void CaravanService::StartCaravanQuest(PlayerCharacter* apPlayer, TESQuest* apQuest) noexcept
{
    auto& papyrus = World::Get().ctx().at<PapyrusService>();
    if (!papyrus.Get("Quest", "SetObjectiveDisplayed") || !papyrus.Get("Quest", "Reset") || !papyrus.Get("ReferenceAlias", "ForceRefTo"))
    {
        spdlog::warn("Caravan: the game's quest functions weren't found");
        return;
    }
    const float from[3]{apPlayer->position.x, apPlayer->position.y, apPlayer->position.z};
    // The nearest in reach whose settlement is claimed by a player here. (Picking one marks it, so the next pick doesn't find those turned down.)
    Vector<int> turnedDown;
    int index = -1;
    while ((index = BikeLink::PickRaidedCaravan(from, kQuestLeast, kQuestMost)) >= 0 && !HomeIsClaimed(index))
        turnedDown.push_back(index);
    for (const int other : turnedDown)
        BikeLink::EndRaidedCaravan(other);
    if (index < 0)
        return;
    BikeLink::Truck info{};
    uint32_t marker = 0;
    if (BikeLink::GetTruck(index, info))
        marker = BikeLink::PlaceMarker(info.At);
    auto* pMarker = Cast<TESObjectREFR>(TESForm::GetById(marker));
    if (!pMarker || apQuest->aliases.length < 1)
    {
        spdlog::warn("Caravan: no marker for the caravan quest");
        BikeLink::EndRaidedCaravan(index);
        return;
    }

    using Quest = TESQuest;
    static PapyrusFunction<void, Quest> s_pReset(papyrus.Get("Quest", "Reset"));
    s_pReset(apQuest);
    bool started = false;
    apQuest->EnsureQuestStarted(started, false);
    if (!started)
    {
        spdlog::warn("Caravan: the caravan quest wouldn't start");
        BikeLink::RemoveMarker(marker);
        BikeLink::EndRaidedCaravan(index);
        return;
    }

    m_questTruck = index;
    m_questMarker = marker;
    m_questShown = false;
    m_questStarting = kQuestStartWait;
    m_questRaidOn = false;
    m_questLook = 0.f;
    m_questLeft = kQuestTime;
    spdlog::info("Caravan: quest: truck {} hit at ({:.0f}, {:.0f}, {:.0f}), {:.0f} from the player", index, info.At[0], info.At[1], info.At[2],
                 std::sqrt(DistanceSquared(apPlayer->position, Point(info.At[0], info.At[1], info.At[2]))));
}

// Whose robbery could this be? Playing alone, supply lines only ever run between the player's own settlements. On a server it has to be a claim of
// this player's or of someone in their party. (Test mode raids any truck, supply line or not.)
bool CaravanService::HomeIsClaimed(int aIndex) noexcept
{
    if (BikeLink::TruckTestMode() || !m_world.GetTransport().IsOnline())
        return true;
    const uint32_t workshop = BikeLink::TruckHomeWorkshop(aIndex);
    const auto& party = m_world.ctx().at<PartyService>();
    return workshop && m_world.ctx().at<WorkshopService>().IsClaimedBy(workshop, party.IsInParty() ? party.GetPartyMembers() : Vector<uint32_t>{});
}

void CaravanService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    m_playerLevels[acMessage.PlayerId] = acMessage.Level;
}

void CaravanService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    m_playerLevels[acMessage.PlayerId] = acMessage.NewLevel;
}

void CaravanService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_playerLevels.erase(acMessage.PlayerId);
}

// It's running: where it points, its objective on the screen, and the player told
void CaravanService::ShowCaravanQuest(TESQuest* apQuest) noexcept
{
    m_questShown = true;
    using Quest = TESQuest;
    using ReferenceAlias = BGSBaseAlias;
    PAPYRUS_FUNCTION(void, Quest, SetObjectiveDisplayed, int32_t, bool, bool);
    PAPYRUS_FUNCTION(void, ReferenceAlias, ForceRefTo, TESObjectREFR*);
    if (auto* pMarker = Cast<TESObjectREFR>(TESForm::GetById(m_questMarker)); pMarker && apQuest->aliases.length > 0)
        s_pForceRefTo(apQuest->aliases.data[0], pMarker);
    apQuest->ScriptSetStage(10);
    s_pSetObjectiveDisplayed(apQuest, 10, true, false);
    apQuest->SetActive(true);
    const char* pHome = BikeLink::TruckHome(m_questTruck);
    const std::string line = pHome && *pHome ? fmt::format("Raiders are hitting the caravan from {}", pHome) : std::string("Raiders are hitting one of your caravans");
    BikeLink::Notify(line.c_str());
    spdlog::info("Caravan: quest shown: {}", line);
}

void CaravanService::EndCaravanQuest(bool aSaved, bool aHelped) noexcept
{
    const int index = m_questTruck;
    if (index < 0)
        return;
    auto* pQuest = Cast<TESQuest>(TESForm::GetById(BikeLink::CaravanQuest()));
    if (pQuest)
    {
        using Quest = TESQuest;
        auto& papyrus = World::Get().ctx().at<PapyrusService>();
        if (papyrus.Get("Quest", "SetObjectiveCompleted") && papyrus.Get("Quest", "SetObjectiveFailed"))
        {
            PAPYRUS_FUNCTION(void, Quest, SetObjectiveCompleted, int32_t, bool);
            PAPYRUS_FUNCTION(void, Quest, SetObjectiveFailed, int32_t, bool);
            if (aSaved)
                s_pSetObjectiveCompleted(pQuest, 10, true);
            else
                s_pSetObjectiveFailed(pQuest, 10, true);
        }
        pQuest->ScriptSetStage(aSaved ? 20 : 30);
        m_questStopIn = 5.f;
    }
    const char* pHome = BikeLink::TruckHome(index);
    const std::string home = pHome && *pHome ? pHome : "the settlement";
    if (aSaved && aHelped)
    {
        BikeLink::GiveCaps(kQuestReward);
        ChangeHappiness(index, kHappinessSaved);
        BikeLink::Notify(fmt::format("The supplies will get through to {}", home).c_str());
    }
    else if (!aSaved)
    {
        ChangeHappiness(index, kHappinessLost);
        BikeLink::Notify(fmt::format("The caravan's supplies for {} are gone", home).c_str());
    }
    BikeLink::RemoveMarker(m_questMarker);
    BikeLink::EndRaidedCaravan(index);
    m_questTruck = -1;
    m_questMarker = 0;
    m_questRaidOn = false;
    m_nextQuest = BikeLink::TruckTestMode() ? kQuestTestWait : kQuestBetween + Random01() * kQuestSpread;
    spdlog::info("Caravan: quest over, {}", aSaved ? (aHelped ? "saved, with the player's help" : "the crew won by themselves") : "the caravan's lost");
}

void CaravanService::LoseTruck(int aIndex, Truck& aTruck) noexcept
{
    // Killed in a raid with raiders standing and the truck whole: they take it. Not with a player there to stop them, or who's been in it:
    // then it's lost, its cargo there, and the raiders fight on (the 22:51 run: the crew fell beside the player, "Raiders have taken the caravan", the
    // player killed them, and the caravan was lost anyway)
    if (aTruck.Now == Stage::kChase || aTruck.Now == Stage::kCutOff || aTruck.Now == Stage::kStandOff || aTruck.Now == Stage::kFight)
    {
        BikeLink::Truck info{};
        const bool whole = BikeLink::GetTruck(aIndex, info) && !info.Burning && !info.Wrecked;
        const bool raiders = std::any_of(aTruck.Raiders.begin(), aTruck.Raiders.end(), [](const Raider& acRaider) {
            const Actor* pRaider = ActorById(acRaider.Id);
            return pRaider && !pRaider->IsDead();
        });
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        const bool playerThere = aTruck.Helped || !pPlayer || NearestPlayer(pPlayer, Point(info.At[0], info.At[1], info.At[2])) < kHelpReach;
        if (whole && raiders && !playerThere)
            return StartTakeOver(aIndex, aTruck);
        if (whole && raiders)
            spdlog::info("Caravan: truck {}'s crew are dead with a player there: lost, not taken", aIndex);
    }
    ReleaseHold(aIndex, aTruck);
    if (aIndex == m_questTruck)
        EndCaravanQuest(false, false);
    if (aTruck.Trader)
        BikeLink::SetTrader(0, -1);
    aTruck.Trader = 0;
    aTruck.Now = Stage::kLost;
    int raiders = 0;
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || pRaider->IsDead())
            continue;
        ++raiders;
        if (raider.OnBike)
            BikeLink::RiderGetOff(raider.Id, rand() % 2 == 0 ? -1 : 1);
        else if (raider.Restrained)
            RiderOff(aTruck, raider, pRaider, true);
    }
    BikeLink::SetTruckLost(aIndex, kLostDays);
    BikeLink::SetTruckCrewed(aIndex, false);
    BikeLink::Truck info{};
    const bool wrecked = BikeLink::GetTruck(aIndex, info) && info.Wrecked;
    NemesisGame::ShowHud(wrecked ? "The caravan's truck is burned out. Its cargo's still in the wreck" : "The caravan is lost. Its cargo's still on the truck");
    spdlog::info("Caravan: truck {} lost, {} raiders still standing", aIndex, raiders);
}

// The crew's dead and the raiders have won: the dead stay where they fell, to be searched; the raiders leave off the hold-up, get off their bikes, and go to the truck.
// It's lost from here (its cargo searchable, off the roads for kLostDays), and once they're on, it's theirs (WatchTakeOver).
void CaravanService::StartTakeOver(int aIndex, Truck& aTruck) noexcept
{
    ReleaseHold(aIndex, aTruck);
    if (aIndex == m_questTruck)
        EndCaravanQuest(false, false);
    if (aTruck.Trader)
        BikeLink::SetTrader(0, -1);
    aTruck.Trader = 0;
    for (const uint32_t id : aTruck.Crew)
    {
        BikeLink::WatchHits(id, false);
        m_leftBehind.push_back(id);
    }
    aTruck.Crew.clear();
    aTruck.Unloaders.clear();
    aTruck.CrewDown = false;
    aTruck.CrewFree = false;
    aTruck.Seat = 0.f;
    int raiders = 0;
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || pRaider->IsDead())
            continue;
        ++raiders;
        StopWalking(raider, pRaider);
        pRaider->StopCombat();
        if (raider.OnBike)
            BikeLink::RiderGetOff(raider.Id, rand() % 2 == 0 ? -1 : 1);
        raider.Boarding = true;
        raider.SentToTruck = false;
        raider.AtTruckFor = 0.f;
    }
    aTruck.Now = Stage::kTakeOver;
    aTruck.Timer = kTakeOverMost;
    BikeLink::SetTruckLost(aIndex, kLostDays);
    BikeLink::SetTruckCrewed(aIndex, false);
    NemesisGame::ShowHud("Raiders have taken the caravan");
    spdlog::info("Caravan: truck {}'s crew are dead: {} raiders go to take it", aIndex, raiders);
}

// To the truck on the game's travel package (a fight with a player comes first); at it, on, out of every player's sight or once they've been there a while,
// as the crew get back on after unloading. Once nobody's still coming (or after kTakeOverMost), whoever's on drives it off; with nobody on, it's just lost.
void CaravanService::WatchTakeOver(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept
{
    SeatCrew(aIndex, aTruck);
    const NiPoint3 at = Point(acInfo.At[0], acInfo.At[1], acInfo.At[2]);
    auto* pTruck = acInfo.RefId ? Cast<TESObjectREFR>(TESForm::GetById(acInfo.RefId)) : nullptr;
    aTruck.Timer -= aDelta;
    int coming = 0;
    for (Raider& raider : aTruck.Raiders)
    {
        Actor* pRaider = ActorById(raider.Id);
        if (!pRaider || pRaider->IsDead() || !raider.Boarding)
            continue;
        if (raider.OnBike || pRaider->IsInCombat())
        {
            raider.SentToTruck = false;
            ++coming;
            continue;
        }
        if (raider.Restrained)
        {
            NemesisGame::Restrain(pRaider, false);
            raider.Restrained = false;
            IdleStop(pRaider);
        }
        if (!raider.SentToTruck)
            raider.SentToTruck = NemesisGame::TravelTo(pRaider, pTruck);
        const float dx = pRaider->position.x - at.x;
        const float dy = pRaider->position.y - at.y;
        if (dx * dx + dy * dy > kBoardReach * kBoardReach)
        {
            raider.AtTruckFor = 0.f;
            ++coming;
            continue;
        }
        raider.AtTruckFor += aDelta;
        if (CanBeSeen(apPlayer, pRaider->position, 180.f) && raider.AtTruckFor < kBoardAnyway)
        {
            ++coming;
            continue;
        }
        // On: one of its people now, held on their seat as the crew were (SeatCrew)
        raider.Boarding = false;
        BikeLink::WatchHits(raider.Id, false);
        BikeLink::ForgetRider(raider.Id);
        pRaider->StopCombat();
        IdleStop(pRaider);
        NemesisGame::Restrain(pRaider, true);
        aTruck.Crew.push_back(raider.Id);
        aTruck.Seat = 0.f;
        spdlog::info("Caravan: {:X} is on truck {}, {} of them on", raider.Id, aIndex, aTruck.Crew.size());
    }
    // (those on are the truck's people now, not raiders round it)
    const Vector<uint32_t>& on = aTruck.Crew;
    aTruck.Raiders.erase(std::remove_if(aTruck.Raiders.begin(), aTruck.Raiders.end(), [&on](const Raider& acRaider) { return std::find(on.begin(), on.end(), acRaider.Id) != on.end(); }),
                         aTruck.Raiders.end());
    if (coming > 0 && aTruck.Timer > 0.f)
        return;
    if (CountAlive(aTruck.Crew) == 0)
    {
        spdlog::info("Caravan: nobody got on truck {}: it's left where it is", aIndex);
        LeaveBehind(aTruck);
        aTruck.Now = Stage::kLost;
        return;
    }
    // Whoever didn't make it stays about as raiders do
    LeaveBehind(aTruck);
    BikeLink::SetTruckHold(aIndex, false);
    BikeLink::PullTruckOver(aIndex, false);
    BikeLink::TakeTruck(aIndex, kLostDays);
    aTruck.Now = Stage::kTaken;
    spdlog::info("Caravan: raiders drive truck {} off, {} of them on it", aIndex, CountAlive(aTruck.Crew));
}

// Theirs: they sit it out as the crew did. Every one of them dead, it stops where it is, abandoned, the cargo there for whoever got them.
void CaravanService::WatchTaken(int aIndex, Truck& aTruck) noexcept
{
    SeatCrew(aIndex, aTruck);
    if (CountAlive(aTruck.Crew) > 0)
        return;
    BikeLink::SetTruckLost(aIndex, kLostDays);
    aTruck.Now = Stage::kLost;
    NemesisGame::ShowHud("The raiders who took the caravan are dead. Its cargo's still on the truck");
    spdlog::info("Caravan: the raiders who took truck {} are dead: it's stopped where it is", aIndex);
}

// The raiders' bodies stay to be searched, and go once nobody's near
void CaravanService::LeaveBehind(Truck& aTruck) noexcept
{
    for (Raider& raider : aTruck.Raiders)
    {
        StopWalking(raider, ActorById(raider.Id));
        BikeLink::WatchHits(raider.Id, false);
        BikeLink::ForgetRider(raider.Id);
        m_leftBehind.push_back(raider.Id);
    }
    aTruck.Raiders.clear();
}

void CaravanService::ClearLeftBehind(PlayerCharacter* apPlayer) noexcept
{
    for (auto it = m_leftBehind.begin(); it != m_leftBehind.end();)
    {
        Actor* pActor = ActorById(*it);
        if (!pActor)
        {
            it = m_leftBehind.erase(it);
            continue;
        }
        if (NearestPlayer(apPlayer, pActor->position) > 6000.f)
        {
            pActor->Delete();
            it = m_leftBehind.erase(it);
            continue;
        }
        ++it;
    }
}

void CaravanService::Clear(int aIndex, Truck& aTruck) noexcept
{
    // (the raiders go with it: back again when a player is)
    if (aIndex == m_questTruck)
        m_questRaidOn = false;
    ReleaseHold(aIndex, aTruck);
    if (aTruck.Trader)
        BikeLink::SetTrader(0, -1);
    // An abandoned truck goes with the player not far off, maybe still fighting the raiders that took it: they and the bodies go when nobody's near, as bodies do (LeaveBehind).
    // Otherwise it's a truck leaving the world, and whoever's alive on it goes with it.
    const bool abandoned = aTruck.Now == Stage::kLost;
    const auto leave = [&](uint32_t aId) {
        BikeLink::WatchHits(aId, false);
        Actor* pActor = ActorById(aId);
        if (!pActor)
            return;
        if (abandoned || pActor->IsDead())
            m_leftBehind.push_back(aId);
        else
            pActor->Delete();
    };
    for (const uint32_t id : aTruck.Crew)
        leave(id);
    aTruck.Crew.clear();
    for (Raider& raider : aTruck.Raiders)
    {
        StopWalking(raider, ActorById(raider.Id));
        BikeLink::ForgetRider(raider.Id);
        leave(raider.Id);
    }
    BikeLink::SetTruckCrewed(aIndex, false);
    BikeLink::PullTruckOver(aIndex, false);
    BikeLink::SetTruckHold(aIndex, false);
    aTruck = Truck{};
}

// Crew a save kept from before it was loaded: nobody else places these. Not those we've left behind (a lost truck's dead, there to be searched: the 22:51 run
// had them deleted 12 s after a raid, with the player there), and never where a player can see them go.
void CaravanService::ClearStrays(PlayerCharacter* apPlayer) noexcept
{
    ProcessLists* pLists = ProcessLists::Get();
    if (!pLists)
        return;
    const auto isOurs = [this](uint32_t aId)
    {
        for (const Truck& truck : m_trucks)
        {
            if (std::find(truck.Crew.begin(), truck.Crew.end(), aId) != truck.Crew.end())
                return true;
        }
        return std::find(m_leftBehind.begin(), m_leftBehind.end(), aId) != m_leftBehind.end();
    };
    Vector<Actor*> strays;
    for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
    {
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(pLists->highActorHandleArray[i]));
        if (!pActor || !pActor->baseForm || !pActor->IsTemporary() || pActor->GetExtension()->IsRemote())
            continue;
        if ((pActor->baseForm->formID == kOldCrewBase || IsTruckCrewName(pActor)) && !isOurs(pActor->formID) && !CanBeSeen(apPlayer, pActor->position, 180.f))
            strays.push_back(pActor);
    }
    for (Actor* pStray : strays)
    {
        spdlog::info("Caravan: crew left over from before, {:X}, gone", pStray->formID);
        pStray->Delete();
    }
}

// --- The rest of the party: they get these characters as they get all characters. What the game doesn't send is the bikes under
// them and what the truck's doing, so those go as messages. Their games' trucks drive by the same clock.

void CaravanService::SendRiders(int aIndex, Truck& aTruck) noexcept
{
    (void)aIndex;
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;
    auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (Raider& raider : aTruck.Raiders)
    {
        if (!raider.OnBike)
            continue;
        BikeLink::NetState state{};
        if (!BikeLink::GetRiderNet(raider.Id, state))
            continue;
        const auto it = std::find_if(view.begin(), view.end(), [&view, &raider](const auto aEntity) { return view.template get<FormIdComponent>(aEntity).Id == raider.Id; });
        if (it == view.end())
            continue;
        RequestMovementAction request{};
        request.Type = RequestMovementAction::kNpcBike;
        request.Npc = view.get<LocalComponent>(*it).Id;
        auto& out = request.Bike;
        out.Stage = state.Stage;
        out.Side = state.Side;
        out.Paint = state.Paint;
        out.Flags = state.Flags;
        out.Heading = state.Heading;
        out.Lean = state.Lean;
        out.Pitch = state.Pitch;
        out.Steer = state.Steer;
        out.Suspension = state.Suspension;
        out.WheelSpeed = state.WheelSpeed;
        out.Rpm = state.Rpm;
        out.Throttle = state.Throttle;
        out.StageTime = state.StageTime;
        out.Stand = state.Stand;
        out.StandGround = state.StandGround;
        transport.Send(request);
    }
}

// On fire: everyone off it, as it can go up any moment. With raiders there it's a fight on foot by now.
void CaravanService::TruckOnFire(int aIndex, Truck& aTruck) noexcept
{
    spdlog::info("Caravan: truck {} is on fire, everyone off it", aIndex);
    switch (aTruck.Now)
    {
    case Stage::kRiding:
    case Stage::kUnload:
    case Stage::kPullOver:
    case Stage::kThanks:
    case Stage::kAfter:
        aTruck.Unloaders.clear();
        if (aTruck.Trader)
            BikeLink::SetTrader(0, -1);
        aTruck.Trader = 0;
        DropCrew(aIndex, aTruck, true, nullptr);
        aTruck.Now = Stage::kBurning;
        break;
    case Stage::kChase:
    case Stage::kCutOff:
    case Stage::kStandOff:
        StartFootFight(aIndex, aTruck);
        break;
    case Stage::kTakeOver:
    case Stage::kTaken:
        // (the raiders on it get off; it stops where it is, theirs or not)
        BikeLink::SetTruckLost(aIndex, kLostDays);
        DropCrew(aIndex, aTruck, true, nullptr);
        LeaveBehind(aTruck);
        aTruck.Now = Stage::kLost;
        break;
    default:
        break;
    }
}

void CaravanService::SendTruckDamage(int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept
{
    if (acInfo.Burning == aTruck.SentBurning && acInfo.Wrecked == aTruck.SentWrecked)
        return;
    aTruck.SentBurning = acInfo.Burning;
    aTruck.SentWrecked = acInfo.Wrecked;
    auto& transport = m_world.GetTransport();
    if ((!acInfo.Burning && !acInfo.Wrecked) || !transport.IsOnline())
        return;
    RequestMovementAction request{};
    request.Type = RequestMovementAction::kTruck;
    request.Truck.Index = static_cast<uint8_t>(aIndex);
    request.Truck.Flags = static_cast<uint8_t>(kTruckDamageOnlyFlag | (acInfo.Burning ? kTruckBurningFlag : 0) | (acInfo.Wrecked ? kTruckWreckedFlag : 0));
    transport.Send(request);
}

void CaravanService::SendTruck(int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept
{
    const bool taken = aTruck.Now == Stage::kTaken;
    const bool held = !taken && (acInfo.Hold || (aTruck.Now != Stage::kRiding && aTruck.Now != Stage::kChase && aTruck.Now != Stage::kAway));
    const bool lost = acInfo.Lost;
    if (held == aTruck.SentHold && lost == aTruck.SentLost && taken == aTruck.SentTaken && aTruck.Trader == aTruck.SentTrader)
        return;
    aTruck.SentHold = held;
    aTruck.SentLost = lost;
    aTruck.SentTaken = taken;
    aTruck.SentTrader = aTruck.Trader;
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;
    RequestMovementAction request{};
    request.Type = RequestMovementAction::kTruck;
    request.Truck.Index = static_cast<uint8_t>(aIndex);
    request.Truck.Flags = static_cast<uint8_t>((held ? kTruckHeldFlag : 0) | (lost ? kTruckLostFlag : 0) | (taken ? kTruckTakenFlag : 0));
    if (aTruck.Trader)
    {
        auto view = m_world.view<FormIdComponent, LocalComponent>();
        const uint32_t trader = aTruck.Trader;
        const auto it = std::find_if(view.begin(), view.end(), [&view, trader](const auto aEntity) { return view.template get<FormIdComponent>(aEntity).Id == trader; });
        if (it != view.end())
            request.Truck.Trader = view.get<LocalComponent>(*it).Id;
    }
    transport.Send(request);
}

void CaravanService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    if (acMessage.Type == RequestMovementAction::kTruck && (acMessage.Truck.Flags & kTruckDamageOnlyFlag))
    {
        BikeLink::DamageTruckTo(acMessage.Truck.Index, (acMessage.Truck.Flags & kTruckWreckedFlag) ? 2 : 1);
        return;
    }
    if (acMessage.Type == RequestMovementAction::kTruck)
    {
        BikeLink::PullTruckOver(acMessage.Truck.Index, (acMessage.Truck.Flags & kTruckHeldFlag) != 0);
        // (taken: lost, and driven off by the raiders on it, as in the game that saw it taken)
        if (acMessage.Truck.Flags & kTruckTakenFlag)
            BikeLink::TakeTruck(acMessage.Truck.Index, kLostDays);
        else if (acMessage.Truck.Flags & kTruckLostFlag)
            BikeLink::SetTruckLost(acMessage.Truck.Index, kLostDays);
        // Their copy of the trader trades too, from this game's copy of the cargo
        const Actor* pTrader = acMessage.Truck.Trader ? Utils::GetByServerId<Actor>(acMessage.Truck.Trader) : nullptr;
        BikeLink::SetTrader(pTrader ? pTrader->formID : 0u, pTrader ? acMessage.Truck.Index : -1);
    }
}

#endif
