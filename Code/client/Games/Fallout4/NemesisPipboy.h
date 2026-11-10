#pragma once

#if TP_FALLOUT4

// Nemeses (see docs/NEMESIS.md): the Pip-Boy's DATA > NEMESIS tab. PipboyTabs shows RD_Nemeses.esp's 40 messages (the
// pages, GameFiles/Fallout4/PBT/RD_NemesesTab.ini) while their globals say so; here the pages the server sent get their
// names on those messages, their globals (1 shown, 2 with a dot, 0 hidden), and their text through
// TESDescription::GetDescription (hooked: our messages' text from here, every other form's from the game).
namespace NemesisPipboy
{
// The server's pages (Nemesis::Engine::PipboyPages): "title<US>text<US>dot<RS>"..., the text's lines marked
void SetPages(const std::string& acPages) noexcept;
// No pages (off a server): the tab is empty
void Clear() noexcept;
// The nemesis screen's hotkey, as the pages name it ({KEY})
void SetKeyName(const std::string& acName) noexcept;
// The first entry (OPEN FULL SCREEN) was pressed since the last call: RD_Nemeses.esp's script set its global
[[nodiscard]] bool TakeOpenRequest() noexcept;
} // namespace NemesisPipboy

#endif
