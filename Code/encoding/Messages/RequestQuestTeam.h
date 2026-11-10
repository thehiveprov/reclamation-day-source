#pragma once

#include "Message.h"

#include <Structs/GameId.h>

using TiltedPhoques::Vector;

// Quest team-ups (Fallout 4, like Fallout 76): each player's quest progress is their own, until one invites another to
// do a quest together. While teamed up on it, its progress is shared between them. Sent with the sender's own
// progress in that quest, which decides how someone joins (see the server's QuestService).
struct RequestQuestTeam final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestQuestTeam;

    enum Action : uint8_t
    {
        kInvite,  // PlayerId: who to invite
        kAccept,  // PlayerId: who invited us
        kDecline, // PlayerId: who invited us
        kLeave,
        kSetMainQuestSync, // party leader: PlayerId 1 shares every main quest within the party, 0 stops it
        kJoinMainQuest,    // our progress in a main quest, for the party's team on it (main quest sync)
    };

    enum Progress : uint8_t
    {
        kNotStarted,
        kRunning,
        kCompleted, // or failed, or stopped for good: can only help
    };

    RequestQuestTeam()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestQuestTeam& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && QuestId == acRhs.QuestId && PlayerId == acRhs.PlayerId && QuestProgress == acRhs.QuestProgress &&
               Stages == acRhs.Stages;
    }

    uint8_t Action{};
    GameId QuestId{};
    uint32_t PlayerId{};
    uint8_t QuestProgress{};
    Vector<uint16_t> Stages{}; // the sender's done stages, ascending
};
