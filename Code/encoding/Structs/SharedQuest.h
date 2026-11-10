#pragma once

#include <Structs/GameId.h>

using TiltedPhoques::Vector;

// A faction's radiant quest as the player who got it has it (Fallout 4, docs/FACTIONS.md): which quest, for which
// faction (FactionStanding::Faction), and what its aliases were filled with, for the party members in the same faction
// to start the same quest with the same places and targets. Characters the quest made or found that the character sync
// hadn't picked up yet go later (Update), once it has: the others wait for those instead of making or finding their own.
struct SharedQuest
{
    struct Fill
    {
        enum Kind : uint8_t
        {
            kReference,
            kLocation,
            kCollectionEntry // one of a reference collection alias's references
        };

        uint32_t AliasId{};
        uint8_t Type{kReference};
        GameId FormId{};       // the world's own reference or location (from a plugin)
        uint32_t ServerId{};   // a character made while playing: its server id, 0 while it has none yet
        bool Character{};      // a character made while playing: the others wait for the starter's

        bool operator==(const Fill& acRhs) const noexcept
        {
            return AliasId == acRhs.AliasId && Type == acRhs.Type && FormId == acRhs.FormId && ServerId == acRhs.ServerId && Character == acRhs.Character;
        }
    };

    // Upper bound on what's read: quests have a few dozen aliases at most
    static constexpr size_t kMaxFills = 128;

    GameId QuestId{};
    uint8_t Faction{};
    bool Update{}; // characters synced since: their server ids for fills sent before (and collection entries)
    Vector<Fill> Fills{};

    bool operator==(const SharedQuest& acRhs) const noexcept { return QuestId == acRhs.QuestId && Faction == acRhs.Faction && Update == acRhs.Update && Fills == acRhs.Fills; }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
