#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Co-op dialogue votes (Fallout 4, see docs/DIALOGUE_VOTE.md): which option a vote chose. Plain C++, the tests run it.
namespace DialogueVotes
{
// How long the party has to vote (decided 2026-10-22)
constexpr int64_t kVoteMilliseconds = 30'000;
// Who votes: the talker's party members within this of the talker (30 m, Fallout 4 units at 70 a metre)
constexpr float kVoterRange = 2100.f;

struct Outcome
{
    // the option chosen; -1: nobody voted
    int Choice = -1;
    // two or more options had the most votes and the host's vote was one of them
    bool HostBrokeTie = false;
};

// acChoices: each voter's vote (an option's index, -1: none yet). aHost and aTalker: their place among the voters (-1:
// not voting). The most votes wins; a tie goes to the host's vote among the tied options, else the talker's, else the
// first of them.
[[nodiscard]] Outcome Decide(const std::vector<int>& acChoices, size_t aOptionCount, int aHost, int aTalker) noexcept;

// Every voter has voted
[[nodiscard]] bool AllVoted(const std::vector<int>& acChoices) noexcept;
} // namespace DialogueVotes
