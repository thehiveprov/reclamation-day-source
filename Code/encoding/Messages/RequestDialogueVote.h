#pragma once

#include "Message.h"

using TiltedPhoques::String;
using TiltedPhoques::Vector;

// Co-op dialogue (Fallout 4, see docs/DIALOGUE_VOTE.md): the player talking to an NPC (who holds its dialogue lock)
// puts their dialogue options to the vote of the party members near them; each voter casts a vote (the talker's own
// through the game's dialogue menu); the talker closes it when the conversation moves on without it.
struct RequestDialogueVote final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestDialogueVote;

    enum Action : uint8_t
    {
        kOpen,  // the talker's options came up: NpcServerId, NpcName, Options
        kCast,  // a vote on VoteId: Choice = the option's index (in Options)
        kClose, // the talker's conversation left VoteId behind (they walked away, the scene moved on)
    };

    // An option as the talker's dialogue menu shows it
    struct Option
    {
        String Prompt;         // the short text on the button
        String Line;           // what the player character says ("" if the game has no line for it)
        uint8_t Challenge = 0; // a speech challenge: 0 none, 1 easy ... 3 hard, 4 always succeeds (XDI's levels)
        bool Enabled = true;   // false: its conditions fail (XDI shows it greyed out), not votable

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const Option& acRhs) const noexcept { return Prompt == acRhs.Prompt && Line == acRhs.Line && Challenge == acRhs.Challenge && Enabled == acRhs.Enabled; }
    };

    RequestDialogueVote()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestDialogueVote& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && VoteId == acRhs.VoteId && NpcServerId == acRhs.NpcServerId && NpcName == acRhs.NpcName &&
               Options == acRhs.Options && Choice == acRhs.Choice;
    }

    uint8_t Action{};
    uint32_t VoteId{};
    uint32_t NpcServerId{};
    String NpcName{};
    Vector<Option> Options{};
    uint8_t Choice{};
};
