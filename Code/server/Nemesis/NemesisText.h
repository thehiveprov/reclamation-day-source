#pragma once

#include "NemesisData.h"

#include <random>
#include <set>

// Nemeses: every line the players read. Our own words throughout.
namespace Nemesis
{
struct Rng
{
    std::mt19937 Gen;

    explicit Rng(uint32_t aSeed)
        : Gen(aSeed)
    {
    }
    // lo..hi inclusive
    int Range(int aLo, int aHi) noexcept
    {
        if (aHi <= aLo)
            return aLo;
        return std::uniform_int_distribution<int>(aLo, aHi)(Gen);
    }
    bool Chance(int aPercent) noexcept { return Range(0, 99) < aPercent; }
    float Unit() noexcept { return std::uniform_real_distribution<float>(0.f, 1.f)(Gen); }
    template <class T> const T& Pick(const std::vector<T>& acItems) noexcept { return acItems[static_cast<size_t>(Range(0, static_cast<int>(acItems.size()) - 1))]; }
};

namespace Text
{
// "Abe Carver"; mutants one word, Atomites a Brother or Sister name. Never one in acTaken.
std::string NewName(Rng& aRng, Kind aBody, const GangDef* apGang, const std::set<std::string>& acTaken);
// A nickname earned by why: "scar:<wound>", "downed", "cheated", "rank", "temper", "kills", "survivor"
std::string Nickname(Rng& aRng, const Record& acRecord, const std::string& acWhy);

const char* PersonalityName(Personality aTemper) noexcept;
const char* PersonalityBlurb(Personality aTemper) noexcept;
const char* OriginName(Origin aOrigin) noexcept;
// How it began, for his card: "you shot him at Lexington and left him breathing"
std::string OriginLine(const Record& acRecord, Origin aOrigin, const std::string& acPlace);
// The notice when he's born: "A nemesis rises: ..."
std::string BornNotice(const Record& acRecord, Origin aOrigin);
HateTier TierOf(int aHate) noexcept;
const char* HateWord(int aHate) noexcept;
const char* FearWord(int aFear) noexcept;
const char* IntentWord(Intent aIntent) noexcept;
const char* StatusWord(Status aStatus) noexcept;
// Notoriety: 0 Unknown, 1 Noticed, 2 Wanted, 3 Hunted, 4 Blood Feud
int NotorietyLevel(int aValue) noexcept;
const char* NotorietyName(int aLevel) noexcept;
const char* AlignmentWord(int aAlignment) noexcept; // a man's: -2..2
const char* KarmaWord(int aPlayerAlignment) noexcept; // KARMA's 0..4
const char* WitsWord(int aWits) noexcept;
const char* KindWord(Kind aBody) noexcept;

// A scar from a wound ("leg", "arm", "face", "gut", "chest", "burn", "eye", "ears", "back", "throat") and the damage
std::string ScarText(Rng& aRng, const std::string& acWound);
// The wound a death leaves by its cause (RequestNemesis::Cause) and the limbs crippled (DeathFlags)
std::string WoundFor(Rng& aRng, uint8_t aCause, int32_t aFlags);
// What a man says when he comes (NemesisSpawn::Kind), by his grudge, scars and the player's karma (0-4)
std::string Taunt(Rng& aRng, const Record& acRecord, const Grudge* apGrudge, uint8_t aKind, int aPlayerAlignment);
// What a man says when the player is put down by him
std::string GloatLine(Rng& aRng, const Record& acRecord);
// Questioning: his refusal, by temper
std::string Refusal(Rng& aRng, Personality aTemper);
// Questioning: begging or bargaining when asked for one reason to live
std::string Bargain(Rng& aRng, const Record& acRecord, int aCaps);
// "a Captain of the Gunners", "rides alone"
std::string Standing(const Record& acRecord);
// " at <place>", or nothing when the place isn't known
std::string At(const std::string& acPlace);
// "he" or "she" ("He", "She")
const char* HeWord(const Record& acRecord, bool aCapital = false) noexcept;
// A man's or a woman's words, by the record's body
bool IsFemale(const Record& acRecord) noexcept;
const char* He(const Record& acRecord) noexcept;
const char* Him(const Record& acRecord) noexcept;
const char* His(const Record& acRecord) noexcept;
const char* HisWord(const Record& acRecord, bool aCapital = false) noexcept;
const char* Himself(const Record& acRecord) noexcept;
// A JSON string literal, quotes included
std::string Json(const std::string& acText);
// A line's text as a sentence after another: its first letter capital ("the Gunners came out on top")
std::string Sentence(std::string aText);
// Game minutes (since Jan 1, 2000, months as the game counts them, no leap days) as "Oct 23, 2287 14:05"
std::string DateText(int64_t aMinutes);
// And back: the service's clock (the year as 2287 or as the game's GameYear, 287)
int64_t ToMinutes(int aYear, int aMonth, int aDay, int aHour, int aMinute) noexcept;
} // namespace Text
} // namespace Nemesis
