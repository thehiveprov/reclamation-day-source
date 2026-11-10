#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Nemeses (Fallout 4, see docs/NEMESIS.md): the nemesis world's state. Plain C++ (no server types), so the engine runs
// in the server and in the offline harness alike. Times are game minutes (see NemesisService::GameMinutes).
namespace Nemesis
{
constexpr int64_t kHour = 60;
constexpr int64_t kDay = 24 * kHour;

enum class Personality : uint8_t
{
    Hothead,
    Coward,
    Proud,
    Cold,
    Greedy,
    Loyal,
    Drifter,
    Count
};

// How a grudge began (RequestNemesis::Deed maps onto these)
enum class Origin : uint8_t
{
    DownedYou,
    LeftForDead,
    Crippled,
    Wounded,
    ComradeKilled,
    Robbed,
    Gunfight,
    Humbled,
    GangOrders,
    Bounty,
    Count
};

enum class Status : uint8_t
{
    Alive,
    PresumedDead, // cheated death: back at BackAt
    Dead,
    Gone, // left the life, left the region, in a cell: Fate says
};

enum class Kind : uint8_t
{
    HumanMale,
    HumanFemale,
    GhoulMale,
    GhoulFemale,
    SuperMutant,
};

enum class HateTier : uint8_t
{
    Resentful,
    Vengeful,
    Murderous,
};

enum class Intent : uint8_t
{
    Hunts,
    Hires,        // won't come himself, sends hunters
    ProvokedOnly, // only if met by chance
    WontHunt,
};

enum class Emotion : uint8_t
{
    Pride,
    Fear,
    Anger,
    Grief,
    Shame,
    Joy,
    Gratitude,
};

enum class GoalType : uint8_t
{
    Avenge, // kin or friend (TargetId) killed by TargetPlayer
    Prove,  // beat TargetPlayer
    Rise,   // wants a higher seat
    Settle, // wants out of the life
};

enum class Tone : uint8_t
{
    Normal,
    Blood,
    Gold,
    Dim,
    Good,
};

// What a player knows about a man (Grudge::Knows bits; also for men without a grudge, in Record::Known)
enum Knowledge : uint32_t
{
    kKnowsName = 1 << 0,
    kKnowsPersonality = 1 << 1,
    kKnowsCamp = 1 << 2,
    kKnowsBoss = 1 << 3,
    kKnowsKin = 1 << 4,
    kKnowsHabits = 1 << 5,
    kKnowsStash = 1 << 6,
};

struct Grudge
{
    std::string Player;
    Origin From = Origin::Gunfight;
    int Hate = 0; // 0-100
    int Fear = 0; // 0-100
    int TimesDownedThem = 0;
    int TimesMet = 0;
    int TimesSparedBy = 0;
    int TimesRobbedBy = 0;
    int TimesHumbledBy = 0;
    std::string Place;
    int64_t Since = 0;
    int64_t LastDealt = 0;
    bool Mercy = false;     // the player let him go; killing him later takes it back
    bool Informant = false; // grateful: answers truthfully, sends word
    bool OwesThem = false;
};

struct Scar
{
    std::string Wound; // "leg", "arm", "face", "gut", "chest", "burn", "eye", "ears", "back", "throat"
    std::string Text;
    std::string Place;
    std::string ByPlayer; // "" if not a player
    int64_t At = 0;
};

// Where a hit landed: by the limb whose condition it took (RequestNemesis::WoundPart)
enum class Part : uint8_t
{
    Head,
    Torso,
    LeftArm,
    RightArm,
    LeftLeg,
    RightLeg,
    Unknown,
    Count
};

// A player's hits on a man within a moment (the wound log): one line per landing spot
struct Wound
{
    int64_t At = 0;
    Part Where = Part::Unknown;
    uint8_t Cause = 0; // RequestNemesis::Cause
    int Damage = 0;
    int Hits = 1;
    int Range = 0; // centimetres
    std::string Weapon;
    std::string Ammo;
    std::string Place;
    std::string ByPlayer;
    bool Crippled = false; // the limb gave out
    bool Lethal = false;   // the last a man took before he went down for good (or was left for dead)
};

struct HistoryLine
{
    int64_t At = 0;
    Tone Style = Tone::Normal;
    std::string Text;
};

struct Memory
{
    Emotion Feeling = Emotion::Anger;
    int Intensity = 0; // fades
    std::string Cause;
    int64_t At = 0;
};

struct Goal
{
    GoalType Type = GoalType::Rise;
    uint32_t TargetId = 0;
    std::string TargetPlayer;
    std::string Text;
    int64_t Since = 0;
};

// A routine an officer keeps: a place he's at for a few hours on some days
struct Habit
{
    std::string Place;    // its name
    std::string Location; // "plugin:localid" of the location (the game spawns him there)
    int Hour = 20;        // starts
    int Length = 3;       // hours
    int DaysMask = 0x7F;  // bit 0 = day % 7 == 0
};

struct Record
{
    uint32_t Id = 0;
    std::string Name;
    std::string Nickname;
    std::string Gang; // "" for a loner
    int Rank = 1;     // 1-5
    int RankXp = 0;
    Kind Body = Kind::HumanMale;
    // His face as the game saved it (TESNPC::Serialize of his generated NPC, with its change flags): put back on the
    // gang's encounter NPC he's spawned as, so he looks the same each time. Empty until a game has seen him.
    std::string Appearance;
    uint32_t AppearanceFlags = 0;

    Personality Temper = Personality::Drifter;
    int Aggression = 50;
    int Nerve = 50;
    int Honesty = 50;
    int Greed = 50;
    int Ambition = 50;
    int Loyalty = 50;
    int Wits = 1;      // 0 dim, 1 average, 2 sharp
    int Alignment = 0; // KARMA -2..2
    std::string Trade;
    std::string Faith;
    std::string Home;
    int BornYear = 2250;

    uint32_t KinId = 0;
    std::string KinRelation; // what the kin is to him ("brother")
    std::vector<uint32_t> Friends;
    uint32_t BossId = 0;
    std::string Camp; // "plugin:localid" of the location he holds, "" none

    Status State = Status::Alive;
    std::string Fate; // how he died or left
    std::string KilledBy; // the player who put him down for good (or left him for dead), "" for none
    int64_t BackAt = 0;
    uint8_t Injuries = 0; // NemesisMan::Injury bits
    bool Protected = false; // a lore leader: never killed or changed off screen
    bool Seeded = false;    // a seeded gang man (hunts by order once his gang knows the player)
    int64_t BornAt = 0;
    int64_t RestUntil = 0;
    int64_t LastCameAt = 0;
    int TimesCheatedDeath = 0;
    int TimesSurrendered = 0;
    bool Liar = false; // caught lying

    // Where he roams: a worldspace key ("Fallout4.esm:3C") and a spot
    std::string World;
    float X = 0.f;
    float Y = 0.f;

    std::vector<Grudge> Grudges;
    std::vector<Scar> Scars;
    std::vector<Wound> Wounds; // oldest first, at most kMaxWounds
    std::vector<HistoryLine> History;
    std::vector<Memory> Memories;
    std::vector<Goal> Goals;
    std::vector<Habit> Habits;
    // What each player knows about him without a grudge (Knowledge bits, by player)
    std::map<std::string, uint32_t> Known;

    // Live, not kept: the character he is right now in some game, and his encounter
    uint32_t LiveActor = 0;
    uint32_t LiveEncounter = 0;

    [[nodiscard]] bool IsAlive() const noexcept { return State == Status::Alive; }
    [[nodiscard]] std::string FullName() const { return Nickname.empty() ? Name : Name + " " + Nickname; }
    [[nodiscard]] Grudge* GrudgeOf(const std::string& acPlayer) noexcept
    {
        for (Grudge& grudge : Grudges)
            if (grudge.Player == acPlayer)
                return &grudge;
        return nullptr;
    }
    [[nodiscard]] const Grudge* GrudgeOf(const std::string& acPlayer) const noexcept { return const_cast<Record*>(this)->GrudgeOf(acPlayer); }
};

// A gang's state in this world (its definition is in NemesisData)
struct GangState
{
    std::string Id;
    int Men = 0;      // strength: men in the field, named ones included
    int Purse = 0;    // caps
    int Supplies = 0; // 0-100
    bool Destroyed = false;
    uint32_t LeaderId = 0;
    // A fallen leader's seat: who fell, when the successor takes it
    std::string FallenLeader;
    uint32_t SuccessorId = 0;
    int64_t SeatFillAt = 0;
    std::map<std::string, int> Notoriety;     // by player, 0-100
    std::map<std::string, int64_t> NotorietyAt; // by player: last raised (fading starts a day later)
    std::map<std::string, float> Ground;      // by district key, 0..1 share
    std::map<std::string, int> Grievance;     // against other gangs
    int64_t BrokeSince = 0;
};

struct Relation
{
    std::string A;
    std::string B;
    int Value = 0; // -100..100
    std::string Why;
};

struct War
{
    uint32_t Id = 0;
    std::string A;
    std::string B;
    std::string District;
    std::string Goal; // "ground", "camp:<location>", "leader"
    int64_t Start = 0;
    int64_t End = 0; // 0: going on
    int Battles = 0;
    int DeadA = 0;
    int DeadB = 0;
    int Heat = 50;
    std::string Result; // when it ended
};

// A line of the chronicle (the World tab)
struct Event
{
    uint32_t Id = 0;
    int64_t At = 0;
    uint8_t Kind = 0; // EventKind
    std::string Title;
    std::string Text;
    std::string Place;
    std::vector<std::string> Gangs;
    std::vector<uint32_t> Men;
    std::string Player; // "" for everyone's world news, else whose deed it was
};

enum class EventKind : uint8_t
{
    Deed,       // a player's deed (killed a nemesis, a nemesis born)
    Encounter,  // a nemesis came for a player
    Clash,
    Feud,
    Life,
    Rivalry,
    Turmoil,
    Word,
    War,
    Succession,
    Funeral,
    Promotion,
};

// A location a gang holds (a camp), its officer, and when it was cleared
struct CampHold
{
    std::string Location; // "plugin:localid"
    std::string Name;
    std::string Gang;
    uint32_t OfficerId = 0;
    int64_t ClearedAt = 0;
    int64_t RetakeAt = 0;
};

struct Funeral
{
    uint32_t DeadId = 0;
    std::string Gang;
    std::string Place;    // the cemetery's name
    std::string Location; // "plugin:localid"
    int64_t Start = 0;
    int64_t End = 0;
};

struct PlayerState
{
    std::string Name;
    int Karma = 0;
    int Alignment = 2; // KARMA's 0 very evil .. 4 very good
    int64_t NextRollAt = 0;
    uint32_t ActiveEncounter = 0;
    int64_t LastEncounterAt = 0;
    int Kills = 0;       // nemeses killed for good
    // The record page's tallies
    int GangKills = 0;   // gang men killed, named or not
    int NemesesMade = 0; // strangers who swore revenge on them
    int Spared = 0;      // surrendered men let go
    int Executed = 0;    // surrendered or downed men killed
    int Robbed = 0;      // pockets picked
    int Stashes = 0;     // gang stashes emptied
    int Camps = 0;       // gang holds cleared
    int Leaders = 0;     // gang leaders killed
    int Downed = 0;      // times a gang man put them down
    int Threats = 0;
    int Bribes = 0;
    int Hits = 0;        // hits on named men (the wound log)
    bool Online = false; // connected now (not kept)
};

struct Encounter
{
    uint32_t Id = 0;
    std::string Player;
    uint32_t RecordId = 0; // the nemesis (or who hired the hunters)
    uint8_t Kind = 0;      // NemesisSpawn::Kind
    std::vector<uint32_t> Men; // record ids (0: unnamed)
    std::vector<uint32_t> ServerIds;
    int64_t StartedAt = 0;
};

// A stranger the server has heard of from a game, while his character exists (by server id)
struct Stranger
{
    uint32_t ActorServerId = 0;
    std::string Gang;
    Kind Body = Kind::HumanMale;
    std::map<std::string, int> Score; // grudge score by player
    std::map<std::string, Origin> Strongest;
    std::string World;
    float X = 0.f;
    float Y = 0.f;
    std::string Place;
    int64_t SeenAt = 0;
    std::vector<Wound> Wounds; // his, if he becomes a nemesis
};

// Something for a player's HUD, or for everyone (Player "")
struct Notice
{
    std::string Player;
    uint8_t Style = 0; // NotifyNemesis::Notice
    std::string Text;
};

// An actor to name in every game (NotifyNemesis::kIdentity), or an order for the game running it
struct LiveChange
{
    enum Type : uint8_t
    {
        kIdentity,
        kOrder,
    };
    Type What = kIdentity;
    uint32_t ActorServerId = 0;
    uint32_t RecordId = 0;
    uint8_t Order = 0;
    std::string ForPlayer; // kOrder kAttack: whom
};

struct KarmaChange
{
    std::string Player;
    int Amount = 0;
    bool Silent = false;
};

struct WorldState
{
    std::string Campaign; // "2287" or "winter-of-atom", "" until the host's game tells
    int64_t Clock = 0;    // last game minute run
    uint32_t NextId = 1;
    uint32_t NextEventId = 1;
    uint32_t NextWarId = 1;
    int64_t NextEventAt = 0;
    int64_t NextDayAt = 0;
    std::map<uint32_t, Record> Records;
    std::map<std::string, GangState> Gangs;
    std::vector<Relation> Relations;
    std::vector<War> Wars;
    std::vector<Event> Chronicle; // newest last, at most kMaxChronicle
    std::vector<CampHold> Camps;
    std::vector<Funeral> Funerals;
    std::map<std::string, PlayerState> Players;
    std::vector<std::string> RetiredNames;
};

constexpr size_t kMaxChronicle = 500;
constexpr size_t kMaxHistory = 60;
constexpr size_t kMaxMemories = 12;
constexpr size_t kMaxRecords = 400;
constexpr size_t kMaxWounds = 30;
// The Pip-Boy tab: RD_Nemeses.esp has this many entries; a page is "title<US>text<US>dot<RS>". The text's lines each start
// with a marker the game styles (NemesisPipboy.cpp in the client): a title, a heading, a dim line, a body line, a bar
// ("=percent<GS>label") or a dated line ("^date<GS>text"); an empty line is a gap.
constexpr size_t kPipboyPages = 40;
constexpr char kPipboyEntrySeparator = '';
constexpr char kPipboyFieldSeparator = '';
constexpr char kPipboyPartSeparator = '';
constexpr char kPipboyTitle = '!';
constexpr char kPipboyHeading = '#';
constexpr char kPipboyDim = '~';
constexpr char kPipboyBody = '.';
constexpr char kPipboyBar = '=';
constexpr char kPipboyDated = '^';
} // namespace Nemesis
