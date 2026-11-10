#pragma once

#if TP_FALLOUT4

struct TESForm;

// Our Pip-Boy note tabs (PipboyTabs, TabType 3: a list of messages and a large text field for the one picked): DATA >
// NEMESIS (NemesisPipboy) and DATA > SOCIAL (SocialPipboy). PipboyTabs reads a page's text with
// TESDescription::GetDescription on the message cast to TESDescription, with no parent form; that's hooked here once
// for every tab (one hook on the function: two would clash), our messages' text from the tabs, every other form's from
// the game.
namespace PipboyPages
{
// A page's marked text as the Pip-Boy's HTML, in the overlay's look on the Pip-Boy (white is the Pip-Boy's colour
// there, grey a dimmer one). Each line's first character says how it's drawn:
//   '!' a big bold name, '#' a small spaced uppercase heading, '~' a dim info line, '^' a dim label and its value
//   (split by '\x1D'), '=' a 24-cell bar ("percent\x1Dvalue"), anything else a plain line without its first character
//   (start plain lines with a space). An empty line is a gap.
[[nodiscard]] std::string ToHtml(const std::string& acMarked);

// One tab's page texts (by the message's TESDescription), replacing that tab's earlier ones; empty clears them
void SetTexts(const char* acpTab, std::unordered_map<const void*, std::string> aTexts) noexcept;

// A form's name (its TESFullName), from the game's string pool; left alone when it's already that
void SetName(TESForm* apForm, const std::string& acName) noexcept;
} // namespace PipboyPages

#endif
