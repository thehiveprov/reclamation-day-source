#pragma once

#include "NemesisText.h"

// Nemeses: the engine. Holds the nemesis world (WorldState) and every rule: deeds, deaths, grudges, encounters,
// questioning, ranks and seats, the living world. NemesisService feeds it the games' messages and the clock and sends
// what it queues (notices, names, orders, karma, spawns). Single-threaded: the server's thread only.
namespace Nemesis
{
// What a game reports (RequestNemesis::kDeed / kDeath), resolved by the service
struct DeedInput
{
    uint8_t Deed = 0;
    int Amount = 0;
    uint32_t RecordId = 0;
    uint32_t ActorServerId = 0;
    Kind Body = Kind::HumanMale;
    std::string Appearance;
    uint32_t AppearanceFlags = 0;
    std::string Gang;
    std::string World;
    float X = 0.f;
    float Y = 0.f;
    std::string Place;
};

struct DeathInput
{
    uint32_t RecordId = 0;
    uint32_t ActorServerId = 0;
    uint8_t Cause = 0;
    int32_t Flags = 0;
    std::string Gang;
    std::string World;
    float X = 0.f;
    float Y = 0.f;
    std::string Place;
};

// A player's hits on a man (RequestNemesis::kWound), for the wound log; a stranger as in DeedInput
struct WoundInput
{
    uint32_t RecordId = 0;
    uint32_t ActorServerId = 0;
    Part Where = Part::Unknown;
    bool Crippled = false;
    uint8_t Cause = 0;
    int Damage = 0;
    int Hits = 1;
    int Range = 0; // centimetres
    std::string Weapon;
    std::string Ammo;
    std::string Gang;
    Kind Body = Kind::HumanMale;
    std::string World;
    float X = 0.f;
    float Y = 0.f;
    std::string Place;
};

// Where a player is, for an encounter roll
struct PlayerSpot
{
    std::string Player;
    bool CanMeet = false; // outside, not in a settlement or C.A.M.P., not busy (the game says, the service checks)
    std::string World;
    float X = 0.f;
    float Y = 0.f;
};

// An encounter for one player's game to spawn (becomes NemesisSpawn)
struct SpawnPlan
{
    struct Man
    {
        uint32_t RecordId = 0;
        Kind Body = Kind::HumanMale;
        std::string Appearance;
        uint32_t AppearanceFlags = 0;
        std::string Name;
        int Rank = 1;
        uint8_t Injuries = 0;
        int Alignment = 0;
        bool Legendary = false;
    };

    std::string Player;
    uint32_t EncounterId = 0;
    uint8_t Kind = 0;
    std::string Gang;
    std::string Taunt;
    std::string Location; // "plugin:localid" for camps
    float Distance = 5000.f;
    std::vector<Man> Men;
};

// Settings the host's MCM page sets (the service passes them on)
struct Settings
{
    bool Enabled = true;
    float EncounterHours = 5.f; // between rolls per player, before notoriety
    int EncounterChance = 50;    // percent per roll
    int CheatDeathBase = 25;     // percent at rank 1
    int CheatDeathPerRank = 10;
    int CheatDeathMax = 65;
    float HateScale = 1.f;
    float FearScale = 1.f;
    bool WorldNews = true;
    int MaxGrudgesPerPlayer = 24; // living grudges before new ones get rarer
};

class Engine
{
public:
    explicit Engine(uint32_t aSeed);

    WorldState& State() noexcept { return m_state; }
    const WorldState& State() const noexcept { return m_state; }
    Settings& Config() noexcept { return m_settings; }

    // A world for this campaign, if there's none yet; gangs whose plugin is loaded are added (now or later)
    void Start(const std::string& acCampaign, const std::vector<std::string>& acPlugins, int64_t aNow);
    bool IsStarted() const noexcept { return !m_state.Campaign.empty(); }
    void Reset(int64_t aNow);

    // Runs the world up to aNow (off-screen events, the daily turn), in bounded steps
    void Advance(int64_t aNow);

    void PlayerJoined(const std::string& acPlayer, int64_t aNow);
    void PlayerLeft(const std::string& acPlayer);
    void SetKarma(const std::string& acPlayer, int aKarma, int aAlignment);

    std::optional<SpawnPlan> RollEncounter(const PlayerSpot& acSpot, int64_t aNow);
    // A player entered a location: a camp officer, a man's habit, a funeral there
    std::optional<SpawnPlan> OnPlace(const std::string& acPlayer, const std::string& acLocation, int64_t aNow);

    void OnDeed(const std::string& acPlayer, const DeedInput& acDeed, int64_t aNow);
    void OnDeath(const std::string& acPlayer, const DeathInput& acDeath, int64_t aNow);
    // Hits on a named man go to his wound log; on a stranger, they wait with him in case he becomes one
    void OnWound(const std::string& acPlayer, const WoundInput& acWound, int64_t aNow);
    void OnOutcome(const std::string& acPlayer, uint32_t aEncounterId, uint8_t aOutcome, int64_t aNow);
    void OnSpawned(uint32_t aEncounterId, const std::vector<uint32_t>& acServerIds);
    // A game saw a man's face (his generated NPC): kept, the first time, so he looks the same when he comes back
    void OnLook(uint32_t aRecordId, uint32_t aActorServerId, const std::string& acAppearance, uint32_t aFlags);
    // Questioning a surrendered man: the card's JSON (his answer, what's revealed, what's left to ask)
    std::string OnQuestion(const std::string& acPlayer, uint32_t aRecordId, uint32_t aActorServerId, uint8_t aQuestion, int aValue, int64_t aNow);
    // A character the server removed: strangers and live bindings let go
    void ActorGone(uint32_t aActorServerId);

    const Record* Find(uint32_t aId) const noexcept;
    uint32_t RecordOfActor(uint32_t aActorServerId) const noexcept;

    // Queued for the service
    std::vector<Notice> Notices;
    std::vector<LiveChange> Live;
    std::vector<KarmaChange> Karma;
    std::vector<std::pair<std::string, uint32_t>> EndedEncounters; // player, encounter id
    bool Dirty = false;

    // The Pip-Boy's NEMESIS tab (NemesisPipboy.cpp): at most kPipboyPages pages for this player, each "title<US>text<US>dot"
    // (dot 1 or 0) and ended by <RS> (kPipboyFieldSeparator, kPipboyEntrySeparator)
    std::string PipboyPages(const std::string& acPlayer, const std::vector<std::string>& acParty) const;

    // The overlay's nemesis screen (NemesisScreen.cpp), a page as JSON for this player: "record", "gang" (the first one
    // they know of) or "gang:<id>", "man:<record id>", "world", "wounds" or "wounds:<record id>". Each player reads what
    // they know, as on the Pip-Boy.
    std::string ScreenJson(const std::string& acPlayer, const std::vector<std::string>& acParty, const std::string& acPage) const;
    // The bar over a surrendered man the player holds at gunpoint, and the top of his questioning card
    std::string GunpointJson(const std::string& acPlayer, uint32_t aRecordId, uint32_t aActorServerId) const;

    // nemeses.txt (NemesisStore.cpp)
    std::string Save() const;
    // Replaces the state; aBadLines counts lines skipped
    bool Load(const std::string& acText, int& aBadLines);

    // The rules (public for the harness)
    Intent IntentOf(const Record& acRecord, const Grudge& acGrudge) const noexcept;
    int EffectiveFear(const Record& acRecord, const Grudge& acGrudge) const noexcept;
    int NotorietyOf(const std::string& acGang, const std::string& acPlayer) const noexcept;
    float HateGain(const Record& acRecord, Origin aWhy, const std::string& acPlayer) const noexcept;
    float FearGain(const Record& acRecord, const std::string& acPlayer) const noexcept;
    int CheatDeathChance(const Record& acRecord) const noexcept;
    static int XpNeed(int aRank) noexcept { return 4 + 3 * aRank; }

private:
    // Records
    Record& NewRecord(const std::string& acGang, Kind aBody, int aRank, int64_t aNow);
    void RollTraits(Record& aRecord);
    Grudge& EnsureGrudge(Record& aRecord, const std::string& acPlayer, Origin aOrigin, const std::string& acPlace, int64_t aNow);
    void AddHate(Record& aRecord, Grudge& aGrudge, int aDelta, Origin aWhy);
    void AddFear(Record& aRecord, Grudge& aGrudge, int aDelta);
    void History(Record& aRecord, Tone aTone, const std::string& acText);
    void Remember(Record& aRecord, Emotion aFeeling, int aIntensity, const std::string& acCause);
    void GainXp(Record& aRecord, int aAmount, const std::string& acWhy);
    void RankUp(Record& aRecord, const std::string& acWhy);
    void GiveNickname(Record& aRecord, const std::string& acWhy);
    void Identify(Record& aRecord);
    void Retire(uint32_t aId);
    std::set<std::string> TakenNames() const;
    Kind RollBody(const GangDef* apGang);
    int LivingGrudges(const std::string& acPlayer) const noexcept;

    // Deeds and deaths
    Record* RecordFor(const DeedInput& acDeed);
    Record* StrangerToRecord(const std::string& acPlayer, const DeedInput& acDeed, Origin aOrigin, int64_t aNow);
    void KillRecord(Record& aRecord, const std::string& acKiller, const DeathInput& acDeath, int64_t aNow);
    void KinGrieve(const Record& acDead, const std::string& acKiller, int64_t aNow);
    void AddNotoriety(const std::string& acGang, const std::string& acPlayer, int aAmount, const std::string& acWhy, int64_t aNow);
    void GangLosesMan(const std::string& acGang, int aCount);
    void SeatFallen(GangState& aGang, const Record& acDead, int64_t aNow);

    // Encounters
    float EncounterWeight(const Record& acRecord, const Grudge* apGrudge, const std::string& acPlayer, const std::string& acDistrict, int aHour) const noexcept;
    bool InRange(const Record& acRecord, const PlayerSpot& acSpot, const std::string& acDistrict) const noexcept;
    SpawnPlan PlanFor(Record& aRecord, const std::string& acPlayer, uint8_t aKind, int64_t aNow);
    SpawnPlan::Man ManOf(const Record& acRecord) const;

    // World (NemesisWorld.cpp)
    void Seed(const GangDef& acGang, int64_t aNow);
    void DailyTurn(int64_t aNow);
    void RunEvent(int64_t aNow);
    void Promotions(GangState& aGang, int64_t aNow);
    void Succession(GangState& aGang, int64_t aNow);
    void Purses(GangState& aGang, int64_t aNow);
    void WarTurn(War& aWar, int64_t aNow);
    void MaybeDeclareWar(int64_t aNow);
    void SimClash(int64_t aNow);
    void SimFeud(int64_t aNow);
    void SimLife(int64_t aNow);
    void SimRivalry(int64_t aNow);
    void SimTurmoil(int64_t aNow);
    void SimWord(int64_t aNow);
    void OffscreenHurt(Record& aRecord, const std::string& acBy, int aDeathPercent, const std::string& acPlace, int64_t aNow);
    Relation& RelationOf(const std::string& acA, const std::string& acB);
    void ShiftRelation(const std::string& acA, const std::string& acB, int aDelta, const std::string& acWhy);
    float GroundOf(const std::string& acGang, const std::string& acDistrict) const noexcept;
    void ShiftGround(const std::string& acWinner, const std::string& acLoser, const std::string& acDistrict, float aShare);
    Event& Chronicle(EventKind aKind, const std::string& acTitle, const std::string& acText, const std::string& acPlace, int64_t aNow,
                     const std::string& acPlayer = {});
    void WorldNotice(const std::string& acText);
    std::vector<Record*> NamedMen(const std::string& acGang, bool aIncludeProtected = false);
    Record* Leader(const std::string& acGang);
    War* LiveWar(const std::string& acA, const std::string& acB);
    bool GangActive(const std::string& acGang) const noexcept;

    WorldState m_state;
    Settings m_settings;
    Rng m_rng;
    std::map<uint32_t, Stranger> m_strangers;
    std::map<uint32_t, Encounter> m_encounters;
    uint32_t m_nextEncounterId = 1;
    std::map<std::string, int> m_unnamedKills; // "gang|player": towards a point of notoriety
    std::vector<std::string> m_plugins;
};
} // namespace Nemesis
