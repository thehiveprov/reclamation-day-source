#pragma once

using TiltedPhoques::String;
using TiltedPhoques::Vector;

// Nemeses (Fallout 4, see docs/NEMESIS.md): one man of an encounter the server sends a game to spawn
struct NemesisMan
{
    // NemesisMan::Injuries
    enum Injury : uint8_t
    {
        kLimp = 1 << 0,    // slower
        kBadArm = 1 << 1,  // worse aim
        kBurned = 1 << 2,
        kOneEye = 1 << 3,
        kRinging = 1 << 4, // ears that ring: slower to notice
    };

    // NemesisMan::Body
    enum BodyKind : uint8_t
    {
        kHumanMale,
        kHumanFemale,
        kGhoulMale,
        kGhoulFemale,
        kSuperMutant,
    };

    uint32_t RecordId{}; // 0: an unnamed man of the nemesis' crew
    uint8_t Body{};
    // His face (TESNPC::Serialize of the NPC he was, with its change flags), put on the gang's encounter NPC he's
    // spawned as; empty until a game has seen him (he gets the encounter NPC's own, and his game sends it back)
    String Appearance{};
    uint32_t AppearanceFlags{};
    String Name{};       // with his nickname, "" for an unnamed man
    uint8_t Rank{1};     // 1-5
    uint8_t Injuries{};
    int8_t Alignment{};  // KARMA: -2 very evil .. 2 very good
    bool Legendary{};

    bool operator==(const NemesisMan& acRhs) const noexcept
    {
        return RecordId == acRhs.RecordId && Body == acRhs.Body && Appearance == acRhs.Appearance && AppearanceFlags == acRhs.AppearanceFlags && Name == acRhs.Name && Rank == acRhs.Rank && Injuries == acRhs.Injuries && Alignment == acRhs.Alignment &&
               Legendary == acRhs.Legendary;
    }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};

// An encounter for one player: the nemesis (first) and his men, where and how they come
struct NemesisSpawn
{
    enum Kind : uint8_t
    {
        kAmbush,   // out of sight, attacks
        kCallOut,  // walks up, says his piece, then fights
        kCrew,     // an officer with his men
        kHunters,  // hired guns with his name on the contract (he isn't among them)
        kSighting, // seen in the distance with his crew; runs if afraid
        kCamp,     // waits at a location his gang holds (LocationId), at its boss marker
        kPatrol,   // his gang's men walking their ground
    };

    uint32_t EncounterId{};
    uint8_t Kind{};
    String Gang{};      // the gang key (NemesisGangs)
    String Taunt{};     // the nemesis' line, "" for none
    uint32_t LocationLocalId{}; // kCamp: the location's form id in its plugin
    String LocationPlugin{};
    float Distance{};   // how far from the player to set them down
    Vector<NemesisMan> Men{};

    bool operator==(const NemesisSpawn& acRhs) const noexcept
    {
        return EncounterId == acRhs.EncounterId && Kind == acRhs.Kind && Gang == acRhs.Gang && Taunt == acRhs.Taunt && LocationLocalId == acRhs.LocationLocalId &&
               LocationPlugin == acRhs.LocationPlugin && Distance == acRhs.Distance && Men == acRhs.Men;
    }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
