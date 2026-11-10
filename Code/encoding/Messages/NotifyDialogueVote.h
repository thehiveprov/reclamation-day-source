#pragma once

#include "Message.h"
#include "RequestDialogueVote.h"

// Co-op dialogue (Fallout 4, see docs/DIALOGUE_VOTE.md): a vote on the talker's dialogue options, sent to the talker and
// every voter. kOpen once (the options, the voters, the time left; the talker alone among the voters: no vote, their
// own pick goes), kTally after each vote, then kResult, or kClosed when the talker's conversation went on without it.
struct NotifyDialogueVote final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyDialogueVote;

    enum Stage : uint8_t
    {
        kOpen,
        kTally,
        // Result: the option chosen (kNoChoice: no vote was cast), the talker's game picks it
        kResult,
        // the talker left the conversation, or left the server
        kClosed,
    };

    static constexpr int8_t kNoChoice = -1;

    struct Voter
    {
        uint32_t PlayerId{};
        String Name;
        int8_t Choice = kNoChoice; // the option's index, kNoChoice until they vote

        void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
        void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

        bool operator==(const Voter& acRhs) const noexcept { return PlayerId == acRhs.PlayerId && Name == acRhs.Name && Choice == acRhs.Choice; }
    };

    NotifyDialogueVote()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyDialogueVote& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Stage == acRhs.Stage && VoteId == acRhs.VoteId && TalkerId == acRhs.TalkerId && NpcServerId == acRhs.NpcServerId &&
               NpcName == acRhs.NpcName && Options == acRhs.Options && Voters == acRhs.Voters && MillisecondsLeft == acRhs.MillisecondsLeft && Result == acRhs.Result &&
               HostBrokeTie == acRhs.HostBrokeTie;
    }

    uint8_t Stage{};
    uint32_t VoteId{};
    uint32_t TalkerId{}; // the talker's player id (their name is among the voters)
    uint32_t NpcServerId{};
    String NpcName{};
    Vector<RequestDialogueVote::Option> Options{}; // kOpen
    Vector<Voter> Voters{};                        // the talker first
    uint32_t MillisecondsLeft{};                   // kOpen, kTally
    int8_t Result = kNoChoice;                     // kResult
    bool HostBrokeTie{};                           // kResult: a tie the host's vote settled
};
