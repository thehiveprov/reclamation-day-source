#pragma once

#include <TiltedCore/Buffer.hpp>

// A player's standing with the Commonwealth's factions (Fallout 4), as their own game has it (docs/FACTIONS.md): which
// they're in, which put them out, which fight them. One bit per faction in each.
struct FactionStanding
{
    enum Faction : uint8_t
    {
        kMinutemen,
        kBrotherhood,
        kRailroad,
        kInstitute,
        kNukaWorldGangs, // the Disciples, the Operators, the Pack (DLCNukaWorld.esm)
        kGunners,        // only through a mod: the game has no way to join them
        kRaiders,
        kCount
    };

    uint8_t Members{}; // joined
    uint8_t Out{};     // kicked out (or the faction destroyed)
    uint8_t Hostile{}; // its members fight the player
    uint8_t Allied{};  // its members are the player's allies or friends

    [[nodiscard]] static constexpr uint8_t Bit(Faction aFaction) noexcept { return static_cast<uint8_t>(1u << aFaction); }
    [[nodiscard]] bool IsMember(Faction aFaction) const noexcept { return (Members & Bit(aFaction)) && !(Out & Bit(aFaction)); }
    [[nodiscard]] bool IsHostile(Faction aFaction) const noexcept { return (Hostile & Bit(aFaction)) != 0; }
    [[nodiscard]] bool IsAllied(Faction aFaction) const noexcept { return (Allied & Bit(aFaction)) != 0; }

    bool operator==(const FactionStanding& acRhs) const noexcept { return Members == acRhs.Members && Out == acRhs.Out && Hostile == acRhs.Hostile && Allied == acRhs.Allied; }
    bool operator!=(const FactionStanding& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
