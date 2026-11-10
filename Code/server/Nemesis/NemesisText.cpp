#include "NemesisText.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace Nemesis::Text
{
bool IsFemale(const Record& acRecord) noexcept
{
    return acRecord.Body == Kind::HumanFemale || acRecord.Body == Kind::GhoulFemale;
}

const char* He(const Record& acRecord) noexcept
{
    return IsFemale(acRecord) ? "she" : "he";
}
const char* Him(const Record& acRecord) noexcept
{
    return IsFemale(acRecord) ? "her" : "him";
}
const char* His(const Record& acRecord) noexcept
{
    return IsFemale(acRecord) ? "her" : "his";
}
const char* HisWord(const Record& acRecord, bool aCapital) noexcept
{
    return aCapital ? (IsFemale(acRecord) ? "Her" : "His") : His(acRecord);
}
const char* Himself(const Record& acRecord) noexcept
{
    return IsFemale(acRecord) ? "herself" : "himself";
}

namespace
{

// "{he}" "{him}" "{his}" "{place}" "{name}" "{scar}" in a line
std::string Fill(std::string aLine, const Record& acRecord, const std::string& acPlace = {}, const std::string& acScar = {})
{
    const auto replace = [&aLine](const std::string& acKey, const std::string& acValue)
    {
        for (size_t pos = aLine.find(acKey); pos != std::string::npos; pos = aLine.find(acKey, pos + acValue.size()))
            aLine.replace(pos, acKey.size(), acValue);
    };
    replace("{he}", He(acRecord));
    replace("{him}", Him(acRecord));
    replace("{his}", His(acRecord));
    replace("{place}", acPlace.empty() ? std::string("out there") : acPlace);
    replace("{name}", acRecord.Name);
    replace("{scar}", acScar);
    return aLine;
}

std::string Pick(Rng& aRng, std::initializer_list<const char*> aLines)
{
    std::vector<const char*> lines(aLines);
    return lines[static_cast<size_t>(aRng.Range(0, static_cast<int>(lines.size()) - 1))];
}
} // namespace

std::string NewName(Rng& aRng, Kind aBody, const GangDef* apGang, const std::set<std::string>& acTaken)
{
    for (int attempt = 0; attempt < 60; ++attempt)
    {
        std::string name;
        if (aBody == Kind::SuperMutant || (apGang && (apGang->Flags & kMutantNames)))
            name = aRng.Pick(MutantNames());
        else if (apGang && (apGang->Flags & kAtomNames))
        {
            // Brother names for men, Sister names for women
            const bool female = aBody == Kind::HumanFemale || aBody == Kind::GhoulFemale;
            std::vector<const char*> names;
            for (const char* pName : AtomNames())
                if ((std::string(pName).rfind("Sister", 0) == 0) == female)
                    names.push_back(pName);
            name = aRng.Pick(names);
        }
        else
        {
            const bool female = aBody == Kind::HumanFemale || aBody == Kind::GhoulFemale;
            name = std::string(female ? aRng.Pick(FemaleNames()) : aRng.Pick(MaleNames())) + " " + aRng.Pick(Surnames());
        }

        // Mutants and the faithful share a few names: a second one gets a number-free epithet later, not a repeat
        if (!acTaken.count(name))
            return name;
    }
    // A full bank: a surname alone is still a name
    return std::string(aRng.Pick(Surnames())) + " " + std::to_string(aRng.Range(2, 99));
}

std::string Nickname(Rng& aRng, const Record& acRecord, const std::string& acWhy)
{
    if (acWhy.rfind("scar:", 0) == 0)
    {
        const std::string wound = acWhy.substr(5);
        if (wound == "leg")
            return Pick(aRng, {"the Limp", "Three-Legs", "Crookshank", "the Lame"});
        if (wound == "arm")
            return Pick(aRng, {"One-Arm", "Crooked Arm", "the Wing", "Lefty"});
        if (wound == "face" || wound == "throat")
            return Pick(aRng, {"Scarface", "the Stitched", "Patchwork", "Half-Face"});
        if (wound == "eye")
            return Pick(aRng, {"One-Eye", "the Squint", "Glass Eye", "Deadeye"});
        if (wound == "burn")
            return Pick(aRng, {"the Burnt", "Cinders", "Ashface", "the Melted"});
        if (wound == "ears")
            return Pick(aRng, {"the Deaf", "Ringing", "Half-Ear"});
        if (wound == "gut")
            return Pick(aRng, {"Gutshot", "the Stitched", "Leadbelly"});
        return Pick(aRng, {"the Scarred", "Stitches", "Old Scars"});
    }
    if (acWhy == "downed")
        return Pick(aRng, {"the Butcher", "Widowmaker", "the Headsman", "Graves", "the Undertaker", "Coffin"});
    if (acWhy == "cheated")
        return Pick(aRng, {"the Unkillable", "Lazarus", "Twice-Buried", "Deadman", "the Ghost", "Gravecrawler"});
    if (acWhy == "kills")
        return Pick(aRng, {"the Reaper", "Tally", "Notches", "Bloody"});
    if (acWhy == "survivor")
        return Pick(aRng, {"the Lucky", "Slippery", "Cat", "Nine Lives"});
    if (acWhy == "rank")
        return Pick(aRng, {"the Hammer", "Iron", "the Boss's Hand", "Big", "the Fist", "Steel"});

    switch (acRecord.Temper)
    {
    case Personality::Hothead: return Pick(aRng, {"Mad Dog", "Hotshot", "the Fuse", "Firecracker", "Rabid"});
    case Personality::Coward: return Pick(aRng, {"the Rabbit", "Skittish", "Weasel", "Jumpy"});
    case Personality::Proud: return Pick(aRng, {"the Duke", "High Hat", "Peacock", "Lordling"});
    case Personality::Cold: return Pick(aRng, {"Ice", "the Quiet", "Stone", "Coldwater"});
    case Personality::Greedy: return Pick(aRng, {"Two-Caps", "the Magpie", "Moneybags", "Grabber"});
    case Personality::Loyal: return Pick(aRng, {"the Hound", "Faithful", "True", "the Shield"});
    default: return Pick(aRng, {"the Drifter", "Dust", "Tumbleweed", "Wanderer"});
    }
}

const char* PersonalityName(Personality aTemper) noexcept
{
    switch (aTemper)
    {
    case Personality::Hothead: return "Hothead";
    case Personality::Coward: return "Coward";
    case Personality::Proud: return "Proud";
    case Personality::Cold: return "Cold";
    case Personality::Greedy: return "Greedy";
    case Personality::Loyal: return "Loyal";
    default: return "Drifter";
    }
}

const char* PersonalityBlurb(Personality aTemper) noexcept
{
    switch (aTemper)
    {
    case Personality::Hothead: return "Quick to anger and quicker to shoot. Comes back for more.";
    case Personality::Coward: return "Runs when it turns bad, and sends others when it can be done.";
    case Personality::Proud: return "Can't stand being bested. A humiliation burns worst.";
    case Personality::Cold: return "Patient and hard to rattle. Holds a grudge without heat.";
    case Personality::Greedy: return "Everything has a price, a life included. Hates being robbed.";
    case Personality::Loyal: return "Stands by the gang and its own. Avenges them.";
    default: return "Belongs nowhere. Forgets easily, unless given a reason not to.";
    }
}

const char* OriginName(Origin aOrigin) noexcept
{
    switch (aOrigin)
    {
    case Origin::DownedYou: return "Put you down";
    case Origin::LeftForDead: return "Left for dead";
    case Origin::Crippled: return "Crippled";
    case Origin::Wounded: return "Wounded";
    case Origin::ComradeKilled: return "Saw you kill one of the gang";
    case Origin::Robbed: return "Robbed";
    case Origin::Gunfight: return "Lived through a gunfight";
    case Origin::Humbled: return "Humbled";
    case Origin::GangOrders: return "Orders";
    case Origin::Bounty: return "Bounty";
    default: return "";
    }
}

std::string OriginLine(const Record& acRecord, Origin aOrigin, const std::string& acPlace)
{
    const std::string at = acPlace.empty() ? std::string() : " at " + acPlace;
    switch (aOrigin)
    {
    case Origin::DownedYou: return "Put you in the dirt" + at + ".";
    case Origin::LeftForDead: return Fill("You left {him} for dead" + at + ", and {he} crawled out of it.", acRecord);
    case Origin::Crippled: return Fill("You crippled {him}" + at + ".", acRecord);
    case Origin::Wounded: return Fill("You shot {him}" + at + " and left {him} breathing.", acRecord);
    case Origin::ComradeKilled: return Fill("Watched you kill {his} own" + at + ".", acRecord);
    case Origin::Robbed: return Fill("You robbed {him}" + at + ".", acRecord);
    case Origin::Gunfight: return "Walked away from a gunfight with you" + at + ".";
    case Origin::Humbled: return "Surrendered to you" + at + " and hasn't forgotten it.";
    case Origin::GangOrders: return Fill("The gang {he} rides with wants you dead.", acRecord);
    case Origin::Bounty: return "Carries a contract with your name on it.";
    default: return "";
    }
}

std::string BornNotice(const Record& acRecord, Origin aOrigin)
{
    switch (aOrigin)
    {
    case Origin::DownedYou: return "A nemesis rises: " + acRecord.FullName() + " put you down, and " + He(acRecord) + " liked it.";
    case Origin::Crippled: return "A nemesis rises: " + acRecord.FullName() + ". You crippled " + Him(acRecord) + "; " + He(acRecord) + " won't forget.";
    case Origin::Wounded: return "A nemesis rises: " + acRecord.FullName() + ". You shot " + Him(acRecord) + " and left " + Him(acRecord) + " breathing.";
    case Origin::ComradeKilled: return "A nemesis rises: " + acRecord.FullName() + " saw what you did to " + His(acRecord) + " own.";
    case Origin::Robbed: return "A nemesis rises: " + acRecord.FullName() + " knows who emptied " + His(acRecord) + " pockets.";
    case Origin::Gunfight: return "A nemesis rises: " + acRecord.FullName() + " walked away from your gunfight.";
    case Origin::Humbled: return acRecord.FullName() + " gave up to you. " + std::string(HeWord(acRecord, true)) + " remembers your face now.";
    case Origin::Bounty: return acRecord.FullName() + " has taken the contract on your head.";
    default: return "A nemesis rises: " + acRecord.FullName() + ".";
    }
}

HateTier TierOf(int aHate) noexcept
{
    if (aHate >= 65)
        return HateTier::Murderous;
    if (aHate >= 35)
        return HateTier::Vengeful;
    return HateTier::Resentful;
}

const char* HateWord(int aHate) noexcept
{
    switch (TierOf(aHate))
    {
    case HateTier::Murderous: return "Murderous";
    case HateTier::Vengeful: return "Vengeful";
    default: return "Resentful";
    }
}

const char* FearWord(int aFear) noexcept
{
    if (aFear >= 75)
        return "Terrified";
    if (aFear >= 50)
        return "Afraid";
    if (aFear >= 25)
        return "Wary";
    return "Unafraid";
}

const char* IntentWord(Intent aIntent) noexcept
{
    switch (aIntent)
    {
    case Intent::Hunts: return "Hunts you";
    case Intent::Hires: return "Sends hired guns";
    case Intent::ProvokedOnly: return "Fights if you meet";
    default: return "Keeps away";
    }
}

const char* StatusWord(Status aStatus) noexcept
{
    switch (aStatus)
    {
    case Status::Alive: return "Alive";
    case Status::PresumedDead: return "Dead";
    case Status::Dead: return "Dead";
    default: return "Gone";
    }
}

int NotorietyLevel(int aValue) noexcept
{
    if (aValue >= 75)
        return 4;
    if (aValue >= 50)
        return 3;
    if (aValue >= 25)
        return 2;
    if (aValue >= 10)
        return 1;
    return 0;
}

const char* NotorietyName(int aLevel) noexcept
{
    switch (aLevel)
    {
    case 1: return "Noticed";
    case 2: return "Wanted";
    case 3: return "Hunted";
    case 4: return "Blood Feud";
    default: return "Unknown";
    }
}

const char* AlignmentWord(int aAlignment) noexcept
{
    switch (aAlignment)
    {
    case -2: return "Very Evil";
    case -1: return "Evil";
    case 1: return "Good";
    case 2: return "Very Good";
    default: return "Neutral";
    }
}

const char* KarmaWord(int aPlayerAlignment) noexcept
{
    return AlignmentWord(aPlayerAlignment - 2);
}

const char* WitsWord(int aWits) noexcept
{
    return aWits <= 0 ? "Dim" : aWits >= 2 ? "Sharp" : "Average";
}

const char* KindWord(Kind aBody) noexcept
{
    switch (aBody)
    {
    case Kind::HumanFemale: return "Woman";
    case Kind::GhoulMale:
    case Kind::GhoulFemale: return "Ghoul";
    case Kind::SuperMutant: return "Super mutant";
    default: return "Man";
    }
}

std::string ScarText(Rng& aRng, const std::string& acWound)
{
    if (acWound == "leg")
        return Pick(aRng, {"a leg that healed crooked", "a knee full of shrapnel", "a thigh scar the size of a hand"});
    if (acWound == "arm")
        return Pick(aRng, {"an arm that never set right", "a shoulder that clicks when it moves", "two fingers that don't bend"});
    if (acWound == "face")
        return Pick(aRng, {"a bullet crease across the cheek", "a jaw wired back together", "a nose broken flat"});
    if (acWound == "eye")
        return Pick(aRng, {"a dead, milky eye", "an empty socket under a strap", "an eye that weeps"});
    if (acWound == "burn")
        return Pick(aRng, {"burn scars down one side", "skin like melted wax on the neck", "hands scorched to the wrists"});
    if (acWound == "ears")
        return Pick(aRng, {"ears that still ring", "half an ear", "a blast deafness, kept hidden"});
    if (acWound == "gut")
        return Pick(aRng, {"a puckered gutshot scar", "a belly stitched shut with wire", "a gut that never sits right"});
    if (acWound == "chest")
        return Pick(aRng, {"ribs that knitted back crooked", "a bullet scar over the heart", "a lung that whistles"});
    if (acWound == "throat")
        return Pick(aRng, {"a ragged scar across the throat", "a voice like gravel since"});
    if (acWound == "back")
        return Pick(aRng, {"a back that won't straighten", "a spine that aches in the cold"});
    return Pick(aRng, {"a bullet scar", "a long, ugly scar"});
}

std::string WoundFor(Rng& aRng, uint8_t aCause, int32_t aFlags)
{
    // Crippled limbs first: they leave the mark
    if (aFlags & ((1 << 4) | (1 << 5)))
        return "leg";
    if (aFlags & ((1 << 6) | (1 << 7)))
        return "arm";
    if (aFlags & (1 << 8))
        return aRng.Chance(50) ? "eye" : "face";
    switch (aCause)
    {
    case 1: return aRng.Chance(50) ? "face" : "gut"; // blade
    case 2: return aRng.Chance(50) ? "face" : "chest"; // blunt
    case 3: return "burn"; // fire
    case 4: return aRng.Chance(60) ? "burn" : "chest"; // energy
    case 5: return aRng.Chance(50) ? "ears" : "back"; // explosion
    case 6: return aRng.Chance(50) ? "face" : "chest"; // claws
    default: break;
    }
    static const std::vector<std::string> s_gunshot = {"gut", "chest", "leg", "arm", "face", "throat"};
    return aRng.Pick(s_gunshot);
}

std::string Taunt(Rng& aRng, const Record& acRecord, const Grudge* apGrudge, uint8_t aKind, int aPlayerAlignment)
{
    const std::string place = apGrudge ? apGrudge->Place : std::string();
    const std::string scar = acRecord.Scars.empty() ? std::string() : acRecord.Scars.back().Text;

    // He came back from the dead: that comes first
    if (acRecord.TimesCheatedDeath > 0 && apGrudge && !acRecord.Scars.empty() && acRecord.Scars.back().ByPlayer == apGrudge->Player)
        return Fill(Pick(aRng, {"Thought you'd buried me? You left me {scar}. Now I'm here to return the favor.",
                                "Remember me? You should. You gave me {scar}.", "They dug me out of the rubble. I've been looking for you ever since."}),
                    acRecord, place, scar);

    if (aKind == 4) // sighting
        return "";

    // The lawful against the wicked, the wicked against the do-gooders
    const GangDef* pGang = FindGang(acRecord.Gang);
    if (pGang && (pGang->Flags & kLawful) && aPlayerAlignment <= 1)
        return Pick(aRng, {"Word of what you've done reached the wall. You're coming with me, or you're staying here.",
                           "We know your face. We know what you did. That ends today.", "Plenty of folks want you answered for. I volunteered."});
    if (pGang && (pGang->Flags & kHunters))
        return Pick(aRng, {"Nothing personal. Your name's just worth more than you are.", "Somebody's paying good caps for your head.",
                           "The contract says dead or alive. I'm not picky."});

    if (!apGrudge)
        return Pick(aRng, {"There you are.", "You're the one they talk about."});

    switch (apGrudge->From)
    {
    case Origin::DownedYou:
        if (apGrudge->TimesDownedThem > 1)
            return Fill(Pick(aRng, {"Again? You never learn.", "How many times do I have to put you down?", "Back for another beating, I see."}), acRecord, place);
        return Fill(Pick(aRng, {"Didn't stay down last time. This time you will.", "Look who crawled back. I finished you once at {place}.",
                                "You again. I remember how you sounded on the ground."}),
                    acRecord, place);
    case Origin::Crippled:
        return Fill(Pick(aRng, {"I walk crooked because of you. Let's see how you walk after today.", "Every step hurts. Every step I thought of you."}),
                    acRecord, place);
    case Origin::Wounded:
        return Fill(Pick(aRng, {"You should've finished the job at {place}.", "You left me bleeding at {place}. Big mistake.",
                                "Still got your bullet in me. Came to give it back."}),
                    acRecord, place);
    case Origin::ComradeKilled:
        return Fill(Pick(aRng, {"They were my people. You'll pay for every one of them.", "I watched you do it. I'll watch you die.",
                                "This is for the ones you left at {place}."}),
                    acRecord, place);
    case Origin::Robbed:
        return Fill(Pick(aRng, {"Thief! I want back what you took, and then some.", "Nobody picks my pockets and keeps their fingers."}), acRecord, place);
    case Origin::Humbled:
        return Fill(Pick(aRng, {"You made me beg. Now it's your turn.", "I got down on my knees for you. Never again."}), acRecord, place);
    case Origin::GangOrders: {
        if (aRng.Chance(34))
            return std::string("The ") + (pGang ? pGang->Short : "boss") + " want you dead. I'm the one who gets to do it.";
        return Pick(aRng, {"Orders came down with your name on them.", "The boss says you're a problem. I solve problems."});
    }
    default:
        break;
    }

    switch (acRecord.Temper)
    {
    case Personality::Hothead: return Pick(aRng, {"I've been waiting for this!", "Come on, then! Let's finish it!"});
    case Personality::Proud: return Pick(aRng, {"Nobody bests me twice.", "You got lucky once. Only once."});
    case Personality::Cold: return Pick(aRng, {"Hello again.", "I told myself I'd be patient. I was."});
    case Personality::Greedy: return Pick(aRng, {"Your gear'll fetch a good price.", "You owe me. I'm collecting."});
    case Personality::Loyal: return Pick(aRng, {"For the gang.", "My people sent me. I didn't argue."});
    default: return Pick(aRng, {"Small world.", "Should've kept walking. So should you."});
    }
}

std::string GloatLine(Rng& aRng, const Record& acRecord)
{
    switch (acRecord.Temper)
    {
    case Personality::Hothead: return Pick(aRng, {"Stay down!", "That's what you get!"});
    case Personality::Proud: return Pick(aRng, {"Remember who did this.", "Tell them my name."});
    case Personality::Cold: return Pick(aRng, {"Done.", "That's that."});
    case Personality::Greedy: return Pick(aRng, {"Let's see what you're carrying.", "Payday."});
    default: return Pick(aRng, {"Not so tough now.", "Should've stayed home."});
    }
}

std::string Refusal(Rng& aRng, Personality aTemper)
{
    switch (aTemper)
    {
    case Personality::Hothead: return Pick(aRng, {"Go to hell.", "Untie your hands and say that.", "That all you got?"});
    case Personality::Coward: return Pick(aRng, {"I can't, they'll kill me!", "Please, I don't know anything, I swear!", "Don't hurt me!"});
    case Personality::Proud: return Pick(aRng, {"I don't crawl for anybody.", "You'll get nothing from me.", "Do your worst."});
    case Personality::Cold: return Pick(aRng, {"...", "You're wasting your time.", "I've been asked harder."});
    case Personality::Greedy: return Pick(aRng, {"What's in it for me?", "Talk's not free.", "Make it worth my while."});
    case Personality::Loyal: return Pick(aRng, {"I don't rat on my own.", "Kill me, then. I won't talk.", "The gang's my family."});
    default: return Pick(aRng, {"Not my business.", "Why would I care?", "Leave me be."});
    }
}

std::string Bargain(Rng& aRng, const Record& acRecord, int aCaps)
{
    if (acRecord.Temper == Personality::Greedy || acRecord.Greed >= 70)
        return "I've got " + std::to_string(aCaps) + " caps stashed. They're yours if I walk.";
    if (acRecord.Temper == Personality::Coward)
        return Pick(aRng, {"Please! I'll tell you anything, I'll go away, you'll never see me again!", "I've got people waiting on me. Please."});
    if (acRecord.Temper == Personality::Proud || acRecord.Temper == Personality::Hothead)
        return Pick(aRng, {"One reason? You'd be making a martyr. They'd come for you.", "Kill me and you'll have the whole gang on you."});
    if (!acRecord.KinRelation.empty())
        return "I've got a " + acRecord.KinRelation + " who'd starve without me.";
    return Pick(aRng, {"Because I'm not worth the bullet.", "Because it won't bring anybody back."});
}

std::string Standing(const Record& acRecord)
{
    const GangDef* pGang = FindGang(acRecord.Gang);
    if (!pGang || acRecord.Gang.empty())
        return acRecord.State == Status::Alive ? "Rides alone" : "Rode alone";
    if (acRecord.Rank >= 5)
        return std::string(pGang->LeaderTitle) + " of " + pGang->Name;
    for (const Tier& tier : pGang->Tiers)
    {
        if (tier.Seats > 0 && acRecord.Rank >= tier.MinRank && *tier.Single)
            return std::string(tier.Single) + " of " + pGang->Name;
    }
    return std::string("One of ") + pGang->Name;
}

std::string At(const std::string& acPlace)
{
    return acPlace.empty() ? std::string() : " at " + acPlace;
}

const char* HeWord(const Record& acRecord, bool aCapital) noexcept
{
    return aCapital ? (IsFemale(acRecord) ? "She" : "He") : He(acRecord);
}

std::string Sentence(std::string aText)
{
    if (!aText.empty())
        aText[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(aText[0])));
    return aText;
}

std::string Json(const std::string& acText)
{
    std::string out = "\"";
    for (const char c : acText)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static const char s_hex[] = "0123456789abcdef";
                out += "\\u00";
                out += s_hex[(c >> 4) & 0xF];
                out += s_hex[c & 0xF];
            }
            else
                out += c;
        }
    }
    return out + "\"";
}

namespace
{
constexpr int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr const char* kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
} // namespace

int64_t ToMinutes(int aYear, int aMonth, int aDay, int aHour, int aMinute) noexcept
{
    // Fallout 4's GameYear global, what the server's calendar holds, is the year less 2000: 287 is 2287
    const int year = aYear < 1000 ? 2000 + aYear : aYear;
    int64_t days = static_cast<int64_t>(std::max(0, year - 2000)) * 365;
    for (int month = 0; month < std::max(0, std::min(11, aMonth)); ++month)
        days += kMonthDays[month];
    days += std::max(1, aDay) - 1;
    return days * kDay + aHour * kHour + aMinute;
}

std::string DateText(int64_t aMinutes)
{
    int64_t days = aMinutes / kDay;
    const int minuteOfDay = static_cast<int>(aMinutes % kDay);
    const int year = 2000 + static_cast<int>(days / 365);
    days %= 365;
    int month = 0;
    while (month < 11 && days >= kMonthDays[month])
        days -= kMonthDays[month++];
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%s %d, %d %02d:%02d", kMonthNames[month], static_cast<int>(days) + 1, year, minuteOfDay / 60, minuteOfDay % 60);
    return buffer;
}
} // namespace Nemesis::Text
