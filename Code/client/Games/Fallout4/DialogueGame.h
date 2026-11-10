#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The game's dialogue as the co-op dialogue vote reads and drives it (docs/DIALOGUE_VOTE.md). Fallout 4 1.10.163; the
// functions found in the decrypted exe with Extended Dialogue Interface's byte patterns (github.com/reg2k/xdi, the mod
// Reclamation Day bundles), each to one place. Main thread.
namespace DialogueGame
{
struct Option
{
    // what the game's pick takes: the vanilla menu's 0 positive, 1 negative, 2 neutral, 3 question; XDI's 5 + its index
    int32_t Id{};
    std::string Prompt;
    std::string Line;
    // a speech challenge (XDI's levels): 0 none, 1 easy, 2 medium, 3 hard, 4 always succeeds, 5-7 the repeatable ones
    uint8_t Challenge{};
    bool Enabled{true};
};

// The player's options are up and the dialogue menu waits for a pick (MenuTopicManager's awaitingPlayerInput)
[[nodiscard]] bool AwaitingPick() noexcept;
// The scene's player dialogue action the options on screen belong to (another one when the conversation goes on); 0:
// none
[[nodiscard]] uintptr_t CurrentAction() noexcept;
// The options as the dialogue menu shows them: XDI's list when XDI drives the menu (its GetDialogueOptions, on the
// menu's code object), else the game's own four
[[nodiscard]] std::vector<Option> ReadOptions() noexcept;
// Picks an option as the dialogue menu would (the handler doesn't see it); false when the menu isn't waiting
bool Pick(int32_t aId) noexcept;
// Every pick the player makes goes to the handler first (the dialogue menu's and XDI's, and a pick the menu kept from
// while the NPC was talking); it returns true to let the pick through
void SetPickHandler(bool (*apHandler)(int32_t aOption)) noexcept;
// Hooks the game's pick function for the handler (once; main thread). Not with the hooks put in at start: XDI finds
// that function by its first bytes when F4SE loads it, later, and a hook already there left it with none, so its
// picks called address 0 and ended the game (2026-10-28, the Mirage Trader). Put in once the player exists, after
// F4SE has loaded its mods; XDI's calls still come through the hook, which is on the function itself.
void InstallHook() noexcept;
// Ends a line of the player's that never finishes, as the game ends one (StopCurrentDialogue): still talking (the
// game's IsTalking) half a second past the line's length. Every frame, main thread.
void UpdatePlayerVoice(float aDelta) noexcept;
} // namespace DialogueGame
