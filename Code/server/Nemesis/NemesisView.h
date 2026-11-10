#pragma once

#include "NemesisTypes.h"
#include "NemesisWire.h"

// Nemeses: what a player is shown of the world, shared by the Pip-Boy's pages (NemesisPipboy.cpp) and the overlay's
// screen (NemesisScreen.cpp). A man's name once met or told, his temper once questioned or fought twice, his kin, camp
// and habits once learned.
namespace Nemesis::View
{
// What a player knows of a man
struct Knows
{
    bool Name;
    bool Temper;
    uint32_t Bits;
};

inline Knows KnowsOf(const Record& acRecord, const std::string& acPlayer)
{
    const auto known = acRecord.Known.find(acPlayer);
    const uint32_t bits = known == acRecord.Known.end() ? 0 : known->second;
    const Grudge* pGrudge = acRecord.GrudgeOf(acPlayer);
    return {(bits & kKnowsName) || pGrudge || acRecord.Protected, (bits & kKnowsPersonality) || (pGrudge && pGrudge->TimesMet >= 2), bits};
}

// A gang's men against what it started with
inline std::string Strength(int aMen, int aStart)
{
    if (aStart <= 0)
        return "unknown strength";
    const int percent = aMen * 100 / aStart;
    if (percent >= 110)
        return "swelling";
    if (percent >= 85)
        return "strong";
    if (percent >= 60)
        return "holding";
    if (percent >= 35)
        return "weakened";
    return "broken";
}

inline std::string Injuries(uint8_t aInjuries)
{
    std::string out;
    const auto add = [&out](const char* acpText) { out += std::string(out.empty() ? "" : ", ") + acpText; };
    if (aInjuries & Wire::kLimp)
        add("walks with a limp");
    if (aInjuries & Wire::kBadArm)
        add("a bad arm spoils his aim");
    if (aInjuries & Wire::kBurned)
        add("burned");
    if (aInjuries & Wire::kOneEye)
        add("one eye");
    if (aInjuries & Wire::kRinging)
        add("his ears ring, he's slow to notice");
    return out;
}
} // namespace Nemesis::View
