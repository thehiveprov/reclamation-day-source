#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Messages/NotifyDialogueVote.h>
#include <Messages/NotifyMovementAction.h>
#include <Messages/RequestDialogueVote.h>
#include <Messages/RequestMovementAction.h>

#include <Dialogue/DialogueVotes.h>

using namespace TiltedPhoques;

namespace
{
// A message through the wire and back, as the client and the server read them (the opcode popped first)
template <class T> T RoundTrip(const T& acMessage)
{
    Buffer buff(4000);
    Buffer::Writer writer(&buff);
    acMessage.Serialize(writer);

    Buffer::Reader reader(&buff);
    uint64_t opcode = 0;
    reader.ReadBits(opcode, 8);

    T received;
    received.DeserializeRaw(reader);
    return received;
}
} // namespace

TEST_CASE("Co-op dialogue votes", "[dialogue.vote]")
{
    SECTION("RequestDialogueVote: the options come up")
    {
        RequestDialogueVote message;
        message.Action = RequestDialogueVote::kOpen;
        message.NpcServerId = 0x2A;
        message.NpcName = "Piper";
        RequestDialogueVote::Option sarcastic;
        sarcastic.Prompt = "Sarcastic";
        sarcastic.Line = "Oh, sure. I'll get right on that.";
        RequestDialogueVote::Option persuade;
        persuade.Prompt = "Get more caps";
        persuade.Line = "That's not enough for this kind of job.";
        persuade.Challenge = 3;
        RequestDialogueVote::Option locked;
        locked.Prompt = "[Locked]";
        locked.Enabled = false;
        message.Options = {sarcastic, persuade, locked};

        REQUIRE(RoundTrip(message) == message);
    }

    SECTION("RequestDialogueVote: a vote")
    {
        RequestDialogueVote message;
        message.Action = RequestDialogueVote::kCast;
        message.VoteId = 12;
        message.Choice = 2;

        REQUIRE(RoundTrip(message) == message);
    }

    SECTION("NotifyDialogueVote: the tally and the result")
    {
        NotifyDialogueVote message;
        message.Stage = NotifyDialogueVote::kTally;
        message.VoteId = 12;
        message.TalkerId = 1;
        message.NpcServerId = 0x2A;
        message.NpcName = "Piper";
        message.MillisecondsLeft = 17'250;
        NotifyDialogueVote::Voter talker;
        talker.PlayerId = 1;
        talker.Name = "Alex";
        talker.Choice = 0;
        NotifyDialogueVote::Voter friendly;
        friendly.PlayerId = 2;
        friendly.Name = "Robin";
        message.Voters = {talker, friendly};

        REQUIRE(RoundTrip(message) == message);

        message.Stage = NotifyDialogueVote::kResult;
        message.Result = 1;
        message.HostBrokeTie = true;
        REQUIRE(RoundTrip(message) == message);

        message.Result = NotifyDialogueVote::kNoChoice;
        message.HostBrokeTie = false;
        REQUIRE(RoundTrip(message).Result == NotifyDialogueVote::kNoChoice);
    }

    SECTION("Who wins")
    {
        using DialogueVotes::Decide;

        // the most votes
        REQUIRE(Decide({0, 2, 2}, 4, -1, 0).Choice == 2);
        // nobody voted
        REQUIRE(Decide({-1, -1}, 4, 0, 0).Choice == -1);
        // a tie goes to the host's vote
        auto tie = Decide({0, 1, 1, 0}, 4, 3, 0);
        REQUIRE(tie.Choice == 0);
        REQUIRE(tie.HostBrokeTie);
        // the host voted for neither tied option: the talker's
        tie = Decide({1, 0, 1, 0, 3}, 4, 4, 0);
        REQUIRE(tie.Choice == 1);
        REQUIRE_FALSE(tie.HostBrokeTie);
        // neither the host nor the talker among the tied: the first of them
        REQUIRE(Decide({-1, 3, 2}, 4, -1, 0).Choice == 2);
        // votes for options that aren't there count for nothing
        REQUIRE(Decide({7, 1}, 2, -1, 0).Choice == 1);

        REQUIRE(DialogueVotes::AllVoted({0, 1}));
        REQUIRE_FALSE(DialogueVotes::AllVoted({0, -1}));
        REQUIRE_FALSE(DialogueVotes::AllVoted({}));
    }
}

TEST_CASE("Better Interact", "[encoding.interact]")
{
    SECTION("RequestMovementAction and NotifyMovementAction: a hand going out")
    {
        RequestMovementAction request;
        request.Type = RequestMovementAction::kInteract;
        request.Interact.Kind = 2;
        request.Interact.Item = GameId(3, 0x1234);
        request.Interact.HasTarget = true;
        request.Interact.Target[0] = 1.5f;
        request.Interact.Target[1] = -20.25f;
        request.Interact.Target[2] = 300.f;
        REQUIRE(RoundTrip(request) == request);

        NotifyMovementAction notify;
        notify.ServerId = 77;
        notify.Type = RequestMovementAction::kInteract;
        notify.Interact = request.Interact;
        REQUIRE(RoundTrip(notify) == notify);

        // no target: only the kind and the item go
        request.Interact.HasTarget = false;
        request.Interact.Target[0] = request.Interact.Target[1] = request.Interact.Target[2] = 0.f;
        REQUIRE(RoundTrip(request) == request);
    }
}

TEST_CASE("Emotes", "[encoding.emotes]")
{
    SECTION("RequestMovementAction and NotifyMovementAction: an emote")
    {
        RequestMovementAction request;
        request.Type = RequestMovementAction::kEmote;
        request.Emote.Emote = 17;
        request.Emote.Stage = RequestMovementAction::kEmoteHold;
        REQUIRE(RoundTrip(request) == request);

        NotifyMovementAction notify;
        notify.ServerId = 0x1234;
        notify.Type = RequestMovementAction::kEmote;
        notify.Emote = request.Emote;
        REQUIRE(RoundTrip(notify) == notify);
    }
}
