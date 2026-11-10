#pragma once

#if TP_FALLOUT4

// Steam friends (see docs/STEAM_SOCIAL.md): the Pip-Boy's DATA > SOCIAL tab. PipboyTabs shows RD_Social.esp's 64
// messages (GameFiles/Fallout4/PBT/RD_SocialTab.ini) while their globals say so; SteamService fills them: their names
// on those messages, their globals (1 shown, 2 with a dot, 0 hidden), their text (PipboyPages' marked lines) through
// the shared GetDescription hook. Main thread, in a world (the plugin's forms).
namespace SocialPipboy
{
// RD_Social.esp's entries (Tools/BuildSocialPlugin.js PAGES)
constexpr size_t kPages = 64;

struct Page
{
    std::string Name;
    // PipboyPages::ToHtml's marked lines
    std::string Text;
    bool Dot{false};
};

// The entries, in order (the first kPages); the rest hidden. False without the plugin.
bool SetPages(const std::vector<Page>& acPages) noexcept;
// No entries
void Clear() noexcept;
// The entry pressed since the last call (RD_Social.esp's script set its global): its number, -1 for none
[[nodiscard]] int TakePressed() noexcept;
} // namespace SocialPipboy

#endif
