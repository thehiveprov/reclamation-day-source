#pragma once

#include "Message.h"

#include <Structs/GameId.h>

using TiltedPhoques::Vector;

// Quest team-ups, see RequestQuestTeam.
struct NotifyQuestTeam final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestTeam;

    enum Kind : uint8_t
    {
        kInvite,    // PlayerId invites us to do the quest together
        kTeam,      // who's on the quest's team now, and its progress
        kDisbanded, // we're not on a team for the quest anymore
        kDeclined,  // PlayerId declined our invite
        kMainQuestSync, // our party shares every main quest (PlayerId 1) or stopped (0): report ours
    };

    NotifyQuestTeam()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyQuestTeam& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Type == acRhs.Type && QuestId == acRhs.QuestId && PlayerId == acRhs.PlayerId && Participants == acRhs.Participants &&
               Helpers == acRhs.Helpers && Stages == acRhs.Stages;
    }

    uint8_t Type{};
    GameId QuestId{};
    uint32_t PlayerId{};
    Vector<uint32_t> Participants{}; // share the quest's progress
    Vector<uint32_t> Helpers{};      // already did it (or got further on another path): their own progress stays as it is
    Vector<uint16_t> Stages{};       // the team's done stages, ascending: participants catch up to them
};
