#pragma once

struct World;
struct UpdateEvent;
struct NotifyMovementAction;
struct NotifyPlayerJoined;
struct NotifyPlayerLeft;
struct NotifyPlayerLevel;
struct PlayerCharacter;
struct NiPoint3;
struct Actor;
struct TESQuest;

namespace BikeLink
{
struct Truck;
struct Rider;
}

/**
 * @brief The caravan trucks' people. The trucks themselves are the RDVehicles plugin's (Convoy): they drive the roads between the
 * game's caravan stops on a schedule, and are real objects only near a player. This puts a crew on each while it's near, and
 * some of those sightings go wrong, in two ways: raiders on bikes come up the road behind it and chase it, or hold it up at the
 * roadside. Raiders only ever appear where no player can see them. Beat them and the truck pulls over, the driver gets out,
 * thanks you (a silent line, the game's subtitle) and offers the game's barter menu. Or plunder it yourself: the crew fights,
 * and once they're dead the cargo opens. RDVehicles' docs/CARAVAN_TRUCKS.md.
 * Fallout 4 only. In a party only the leader's game spawns anyone: the others get them as they get all characters, the bikes
 * under them and what the truck's doing by message.
 */
class CaravanService
{
public:
    CaravanService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CaravanService() noexcept = default;

    TP_NOCOPYMOVE(CaravanService);

    // One of a truck's crew (RDVehicles.esp's Caravan Trader or Caravan Guard, or the old guard template). CharacterService keeps them when the game
    // connects: they're placed as a truck comes near, and only once (seen 11-05: deleted 1.7 s after the save loaded, the truck lost)
    [[nodiscard]] static bool IsTruckCrew(const Actor* apActor) noexcept;

private:
    enum class Stage
    {
        kAway,     // not a real truck right now
        kRiding,   // the crew's on it
        kChase,    // raiders on bikes are after it, it's still driving
        kCutOff,   // raiders on bikes are getting ahead of it to stop it
        kStandOff, // stopped at the roadside, raiders around it, nobody's fired yet
        kFight,    // raiders and crew on foot
        kPullOver, // beaten: it's pulling over to say thanks
        kThanks,   // the driver's out: thanks, and trade
        kPlunder,  // a player went for it: the crew fights them
        kAfter,    // the raiders are dead and nobody helped: a moment, then back on and away
        kBurning,  // on fire with no fight on: the crew's off it, and it's to go up or not
        kLost,     // the crew's dead, or it's gone up
        kUnload,   // in its bay at a stop: two of the crew down behind it while its supplies are set down, then back on and away
        kTakeOver, // the crew's dead and raiders are left: they go to the truck and get on it
        kTaken,    // raiders aboard, its crew now: it drives off toward their camp, and goes once nobody sees it
    };
    enum class Plan
    {
        kNone,
        kChase,
        kHoldUp,
    };
    struct Raider
    {
        uint32_t Id{0};
        uint32_t ShotBy{0}; // the player who last hit them
        bool OnBike{false};
        bool Restrained{false};
        bool HasGun{false};
        bool Aiming{false};
        float Settle{0.f};  // seconds of the game's AI before they're held to the bike: it gets their gun out
        float AimedFor{0.f};
        float NextShot{0.f};
        float Draw{0.f};
        float NewSlot{0.f};
        float Idle{0.f};
        float StoppedFor{0.f}; // on a bike that isn't moving
        int Burst{0};
        bool HitLogged{false};  // who first hit them has gone in the log
        bool DeathLogged{false}; // who the game says killed them has gone in the log
        float HeldHealth{1.f};  // held for the quest: how much of it they had when last looked at
        // On foot, closing in on the stopped truck from round about (the quest's raiders): the game's travel package to a marker at
        // their place round it (NemesisGame::TravelTo), unaggressive till they're there
        bool Walking{false};
        uint32_t Marker{0};     // the plugin's marker at their place
        NiPoint3 Goal{};        // where it is
        float Aggression{-1.f}; // what theirs was
        float Best{1e9f};       // the nearest they've got to it
        float NoNearer{0.f};    // seconds since they got nearer
        float WalkLog{0.f};
        // Taking the truck: on their way to it, and how long they've been at it
        bool Boarding{false};
        bool SentToTruck{false};
        float AtTruckFor{0.f};
        // When they were placed, and their body coming in (the game builds it a second or more after): 0 not in yet, 1 in (the frame
        // times either side kept), 2 logged. What a raider's arrival costs, for the raid hitch.
        double PlacedAt{0.0};
        float PlaceMs{0.f};
        int BodyStep{0};
        float BodyBefore{0.f};
        float BodyFrame{0.f};
    };
    // A raider still to come. They come one at a time, kArriveEvery apart, each body's arrival in a frame of its own (the hitch
    // as a raid started was the first bike's model read from disk, read at the main menu now: RDVehicles Remote::OnDataLoaded).
    struct Arrival
    {
        bool OnFoot{false};
        float Ahead{0.f};  // on a bike: their start on the truck's road (behind it) and to which side, as it is when they come
        float Side{0.f};
        NiPoint3 Start{};  // on foot: where they come from, and their place round the truck
        NiPoint3 Goal{};
        float Waited{0.f}; // seconds their start has been in someone's sight
    };
    struct Truck
    {
        Stage Now{Stage::kAway};
        bool QuestHold{false};  // the quest's truck with no player near: nobody on it or around it can die yet
        float HoldLog{0.f};
        int DemandStep{0};
        float DemandIn{0.f};
        uint32_t DemandSpeaker{0};
        Plan Planned{Plan::kNone};
        Vector<uint32_t> Crew;
        Vector<Raider> Raiders;
        bool SatLogged{false};
        bool SatGoodLogged{false};
        bool SatRiderLogged{false};
        int SitTries{0};
        bool PlanChosen{false};
        bool Seen{false};
        bool WaitLogged{false};
        bool KeptApartLogged{false};
        bool CrewDown{false};
        bool CrewFree{false};
        bool Helped{false};
        bool SaidThanks{false};
        bool PullAsked{false};
        bool SentHold{false};
        bool SentLost{false};
        bool OnFire{false};
        bool SentBurning{false};
        bool SentWrecked{false};
        bool HomeHurt{false};
        uint32_t SentTrader{0};
        int CrewHits{0};
        uint32_t Attacker{0};
        uint32_t Trader{0};
        float Look{0.f};
        float RaidIn{0.f};
        float Timer{0.f};
        float Seat{0.f};
        float Watch{0.f};
        float HitWindow{0.f};
        float Idle{0.f};
        float SubtitleFor{0.f};
        float SinceThanks{0.f};
        float AwayFor{0.f};
        float StoppedFor{0.f}; // the truck, during a chase
        Vector<uint32_t> Unloaders; // who's down at the tail
        float BoardWait{0.f};
        bool OnFoot{false};         // its raiders came on foot (TryStartWalkers), not on bikes
        bool SentTaken{false};
        // Its crew gone, not killed: how long, and when it was last given a new one (none of it in the world coming down: the 22:05 crash)
        float CrewGoneFor{0.f};
        double Recrewed{-1e9};
        bool RecrewHeldLogged{false};
        float NotReadyFor{0.f};     // seconds the plugin's had no truck in the world for it
        Vector<Arrival> Arrivals;   // its raid's raiders still to come (BringInRaiders)
        float ArriveIn{0.f};
        uint8_t Paint{0};           // the riders' bikes

    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;
    void BringOutCrew(int aIndex, Truck& aTruck) noexcept;
    void ChoosePlan(int aIndex, Truck& aTruck) noexcept;
    // apSkip: those left where they are (unloading)
    void SeatCrew(int aIndex, Truck& aTruck, const Vector<uint32_t>* apSkip = nullptr) noexcept;
    void StartUnload(int aIndex, Truck& aTruck) noexcept;
    void WatchUnload(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    // One raid at a time, and never on caravans together or where the last one was
    [[nodiscard]] bool RaidAllowed(PlayerCharacter* apPlayer, int aIndex, const NiPoint3& acAt) noexcept;
    void DropCrew(int aIndex, Truck& aTruck, bool aFree, Actor* apTarget) noexcept;
    [[nodiscard]] bool CanBeSeen(PlayerCharacter* apPlayer, const NiPoint3& acPoint, float aHeight) noexcept;
    [[nodiscard]] bool FarFromPlayers(PlayerCharacter* apPlayer, const NiPoint3& acPoint, float aDistance) noexcept;
    [[nodiscard]] float NearestPlayer(PlayerCharacter* apPlayer, const NiPoint3& acPoint, Actor** appPlayer = nullptr) noexcept;
    void WatchRiding(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    [[nodiscard]] bool TryStartRiders(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept;
    // The quest's raid: raiders on foot from round about, out of every player's sight and well away from them, closing in on the stopped truck
    [[nodiscard]] bool TryStartWalkers(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept;
    void UpdateWalkers(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, float aDelta) noexcept;
    // One raider placed: on a bike (its start on the truck's road), or on foot walking in to their place round it
    [[nodiscard]] bool PlaceRider(int aIndex, Truck& aTruck, float aAhead, float aSide, const NiPoint3& acAt) noexcept;
    [[nodiscard]] bool PlaceWalker(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const NiPoint3& acStart, const NiPoint3& acGoal, const NiPoint3& acTruckAt) noexcept;
    // The rest of a raid's raiders, one every kArriveEvery, each where nobody sees them come
    void BringInRaiders(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    // Each raider's body coming in: how long after they were placed, and the frames either side
    void WatchBodies(int aIndex, Truck& aTruck) noexcept;
    // No longer walking in: their marker gone, their aggression back as it was
    void StopWalking(Raider& aRaider, Actor* apRaider) noexcept;
    void UpdateRaiders(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, float aDelta) noexcept;
    void AimAndFire(PlayerCharacter* apPlayer, Truck& aTruck, Raider& aRaider, Actor* apRaider, const BikeLink::Rider& acRider, float aDelta) noexcept;
    void RiderOff(Truck& aTruck, Raider& aRaider, Actor* apRaider, bool aFight) noexcept;
    void HoldStillAtGunpoint(Raider& aRaider, Actor* apRaider, const NiPoint3& acFacing) noexcept;
    void WatchChase(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    void WatchCutOff(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    void WatchStandOff(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    void StartFootFight(int aIndex, Truck& aTruck) noexcept;
    void WatchFight(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    void RaidersBeaten(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept;
    void HoldForQuest(int aIndex, Truck& aTruck) noexcept;
    void ReleaseHold(int aIndex, Truck& aTruck) noexcept;
    void BeginThanks(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck) noexcept;
    void WatchThanks(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    void BackOnTheRoad(int aIndex, Truck& aTruck) noexcept;
    void CheckAttacked(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, float aDelta) noexcept;
    void StartPlunder(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck) noexcept;
    bool EncountersOpen() noexcept;
    void SayDemand(int aIndex, Truck& aTruck, float aDelta, bool aHeard) noexcept;
    [[nodiscard]] bool HomeIsClaimed(int aIndex) noexcept;
    void OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept;
    void OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void WatchPlunder(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    void LoseTruck(int aIndex, Truck& aTruck) noexcept;
    // The crew dead in a raid, raiders standing, the truck whole: they take it (kTakeOver, then kTaken)
    void StartTakeOver(int aIndex, Truck& aTruck) noexcept;
    void WatchTakeOver(PlayerCharacter* apPlayer, int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo, float aDelta) noexcept;
    void WatchTaken(int aIndex, Truck& aTruck) noexcept;
    void LeaveBehind(Truck& aTruck) noexcept;
    void Clear(int aIndex, Truck& aTruck) noexcept;
    void ClearStrays(PlayerCharacter* apPlayer) noexcept;
    void ClearLeftBehind(PlayerCharacter* apPlayer) noexcept;
    void SendRiders(int aIndex, Truck& aTruck) noexcept;
    void SendTruck(int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept;
    void SendTruckDamage(int aIndex, Truck& aTruck, const BikeLink::Truck& acInfo) noexcept;
    void TruckOnFire(int aIndex, Truck& aTruck) noexcept;
    // The radiant quest: one of the player's settlements' caravans under attack somewhere, and they're told where
    void UpdateCaravanQuest(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void StartCaravanQuest(PlayerCharacter* apPlayer, TESQuest* apQuest) noexcept;
    void ShowCaravanQuest(TESQuest* apQuest) noexcept;
    void EndCaravanQuest(bool aSaved, bool aHelped) noexcept;
    // What a settlement thinks of what's happened to its caravan: its workshop's happiness modifier, as the game's own events change it
    void ChangeHappiness(int aIndex, float aAmount) noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_movementActionConnection;
    entt::scoped_connection m_playerJoinedConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_playerLeftConnection;
    Map<uint32_t, int32_t> m_playerLevels; // the others', as the server says
    Vector<Truck> m_trucks;
    bool m_encountersOpen{false};
    Vector<uint32_t> m_leftBehind; // raiders' bodies: there to be looted, and gone once nobody's near
    double m_time{0.0};
    double m_nextRaid{0.0};
    NiPoint3 m_lastRaidAt{};   // where the last raid started
    bool m_lastRaidSet{false};
    float m_delta{0.f};
    float m_frameMs{0.f};       // this frame and the one before, as they were (m_delta is held to 0.1 s)
    float m_frameBeforeMs{0.f};
    float m_strayCheck{1.f};
    float m_leftCheck{3.f};
    float m_sendTimer{0.f};
    uint32_t m_testTurn{0};
    int m_questTruck{-1};      // the truck the radiant quest is about
    uint32_t m_questMarker{0}; // where its objective points
    bool m_questShown{false};  // the game has it running, and the player's been told
    float m_questStarting{0.f}; // how much longer to wait for that
    bool m_questRaidOn{false}; // the raiders are there: the truck's real and they've been put on the road to it
    float m_questLook{0.f};
    float m_questLeft{0.f};    // until it's too late
    float m_nextQuest{-1.f};   // until the next one (-1: not chosen yet)
    float m_questStopIn{0.f};  // the quest's finished: stopped after this, with the log line shown
};
