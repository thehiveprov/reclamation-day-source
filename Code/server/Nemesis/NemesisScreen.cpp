#include "NemesisEngine.h"
#include "NemesisView.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

// Nemeses: the overlay's nemesis screen (skyrim_ui components/nemesis-screen), one page at a time as JSON, and the bar
// over a man held at gunpoint. Each player reads what they know (NemesisView.h): unknown men are shown by their seat,
// not their name. Our own words throughout.
namespace Nemesis
{
namespace
{
// A JSON object, field by field
class Object
{
public:
    Object& Str(const char* acpKey, const std::string& acValue)
    {
        Key(acpKey);
        m_text += Text::Json(acValue);
        return *this;
    }
    Object& Num(const char* acpKey, int64_t aValue)
    {
        Key(acpKey);
        m_text += std::to_string(aValue);
        return *this;
    }
    Object& Real(const char* acpKey, float aValue)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(aValue));
        Key(acpKey);
        m_text += buffer;
        return *this;
    }
    Object& Bool(const char* acpKey, bool aValue)
    {
        Key(acpKey);
        m_text += aValue ? "true" : "false";
        return *this;
    }
    // A value already in JSON (an object, an array, null)
    Object& Raw(const char* acpKey, const std::string& acJson)
    {
        Key(acpKey);
        m_text += acJson;
        return *this;
    }
    [[nodiscard]] std::string Done() const { return "{" + m_text + "}"; }

private:
    void Key(const char* acpKey)
    {
        if (!m_text.empty())
            m_text += ',';
        m_text += '"';
        m_text += acpKey;
        m_text += "\":";
    }

    std::string m_text;
};

std::string Array(const std::vector<std::string>& acItems)
{
    std::string out = "[";
    for (size_t i = 0; i < acItems.size(); ++i)
        out += (i ? "," : "") + acItems[i];
    return out + "]";
}

std::string Upper(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return aText;
}

std::string Lower(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return aText;
}

const char* PartWord(Part aPart) noexcept
{
    switch (aPart)
    {
    case Part::Head: return "head";
    case Part::Torso: return "chest";
    case Part::LeftArm: return "left arm";
    case Part::RightArm: return "right arm";
    case Part::LeftLeg: return "left leg";
    case Part::RightLeg: return "right leg";
    default: return "body";
    }
}

// RequestNemesis::Cause
const char* CauseWord(uint8_t aCause) noexcept
{
    switch (aCause)
    {
    case 0: return "gunshot";
    case 1: return "blade";
    case 2: return "blunt";
    case 3: return "fire";
    case 4: return "energy";
    case 5: return "explosion";
    case 6: return "claws";
    default: return "other";
    }
}

const char* Severity(const Wound& acWound) noexcept
{
    if (acWound.Lethal)
        return "lethal";
    if (acWound.Crippled)
        return "crippling";
    if (acWound.Damage >= 60)
        return "severe";
    if (acWound.Damage >= 25)
        return "serious";
    return "light";
}

// How a man stands with a player, as the screen colours him: "murderous", "vengeful", "resentful" or "none"
const char* ToneOf(const Grudge* apGrudge) noexcept
{
    if (!apGrudge)
        return "none";
    switch (Text::TierOf(apGrudge->Hate))
    {
    case HateTier::Murderous: return "murderous";
    case HateTier::Vengeful: return "vengeful";
    default: return "resentful";
    }
}

const char* StatusKey(Status aStatus) noexcept
{
    switch (aStatus)
    {
    case Status::Alive: return "alive";
    case Status::PresumedDead: return "presumed";
    case Status::Dead: return "dead";
    default: return "gone";
    }
}

// What a gang wants with a player, by its notoriety level with them
const char* AimWord(int aLevel) noexcept
{
    switch (aLevel)
    {
    case 0: return "They don't know you";
    case 1: return "They've heard of you";
    case 2: return "They want you";
    case 3: return "They're hunting you";
    default: return "They want you dead";
    }
}

const char* RelationWord(int aValue) noexcept
{
    if (aValue <= -70)
        return "blood enemies";
    if (aValue <= -30)
        return "at odds";
    if (aValue >= 60)
        return "allies";
    if (aValue >= 30)
        return "on good terms";
    return "wary";
}

const char* EventTag(EventKind aKind) noexcept
{
    switch (aKind)
    {
    case EventKind::Deed: return "deed";
    case EventKind::Encounter: return "came for you";
    case EventKind::Clash: return "clash";
    case EventKind::Feud: return "feud";
    case EventKind::Life: return "life";
    case EventKind::Rivalry: return "rivalry";
    case EventKind::Turmoil: return "turmoil";
    case EventKind::Word: return "word";
    case EventKind::War: return "war";
    case EventKind::Succession: return "new leader";
    case EventKind::Funeral: return "funeral";
    default: return "rise";
    }
}

std::string GangShort(const std::string& acGang)
{
    const GangDef* pGang = FindGang(acGang);
    return pGang ? pGang->Short : std::string();
}

// The last time anything happened to a man (his history's newest line)
int64_t LastSeen(const Record& acRecord) noexcept
{
    return acRecord.History.empty() ? acRecord.BornAt : acRecord.History.back().At;
}
} // namespace

std::string Engine::ScreenJson(const std::string& acPlayer, const std::vector<std::string>& acParty, const std::string& acPage) const
{
    const auto colon = acPage.find(':');
    const std::string page = acPage.substr(0, colon);
    const std::string arg = colon == std::string::npos ? std::string() : acPage.substr(colon + 1);
    const uint32_t argId = arg.empty() || !std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); })
                               ? 0u
                               : static_cast<uint32_t>(std::stoul(arg));

    const auto playerIt = m_state.Players.find(acPlayer);
    const PlayerState player = playerIt == m_state.Players.end() ? PlayerState{} : playerIt->second;
    const int year = m_state.Campaign == "winter-of-atom" ? 2282 : 2287;

    // A man as a box on the hierarchy or a line in a list
    const auto box = [&](const Record& acRecord, const char* acpSeat)
    {
        const View::Knows knows = View::KnowsOf(acRecord, acPlayer);
        const Grudge* pGrudge = acRecord.GrudgeOf(acPlayer);
        const bool fresh = acRecord.IsAlive() && pGrudge &&
                           (m_state.Clock - pGrudge->Since < kDay || (acRecord.LastCameAt && m_state.Clock - acRecord.LastCameAt < kDay));
        Object man;
        man.Num("id", acRecord.Id)
            .Bool("known", knows.Name)
            .Str("name", knows.Name ? acRecord.Name : std::string())
            .Str("nickname", knows.Name ? acRecord.Nickname : std::string())
            .Str("seat", acpSeat)
            .Str("gang", GangShort(acRecord.Gang))
            .Num("rank", acRecord.Rank)
            .Str("status", StatusKey(acRecord.State == Status::PresumedDead ? Status::Dead : acRecord.State))
            .Str("tone", ToneOf(pGrudge))
            .Str("standing", pGrudge ? Text::HateWord(pGrudge->Hate) : "")
            .Num("hate", pGrudge ? pGrudge->Hate : 0)
            .Str("temper", knows.Temper ? Text::PersonalityName(acRecord.Temper) : "")
            .Bool("limp", (acRecord.Injuries & Wire::kLimp) != 0)
            .Bool("scarred", !acRecord.Scars.empty())
            .Bool("liar", acRecord.Liar && knows.Name)
            .Bool("informant", pGrudge && pGrudge->Informant)
            .Bool("fresh", fresh);
        return man.Done();
    };

    // The gang tabs: this campaign's gangs in their order
    std::vector<std::string> tabs;
    std::string firstGang;
    int bestNotoriety = -1;
    for (const GangDef& def : Gangs())
    {
        const auto it = m_state.Gangs.find(def.Id);
        if (it == m_state.Gangs.end())
            continue;
        const int notoriety = NotorietyOf(def.Id, acPlayer);
        if (notoriety > bestNotoriety)
            bestNotoriety = notoriety, firstGang = def.Id;
        tabs.push_back(Object{}.Str("id", def.Id).Str("name", def.Short).Bool("destroyed", it->second.Destroyed).Num("notoriety", notoriety).Done());
    }

    Object out;
    out.Str("page", page).Str("date", Text::DateText(m_state.Clock)).Str("player", acPlayer).Raw("gangs", Array(tabs)).Bool("enabled", m_settings.Enabled);

    if (page == "record")
    {
        out.Raw("karma", Object{}.Num("value", player.Karma).Num("alignment", player.Alignment).Str("word", Text::KarmaWord(player.Alignment)).Done());

        const std::pair<const char*, int> tallies[] = {
            {"gang men killed", player.GangKills}, {"nemeses killed", player.Kills}, {"leaders killed", player.Leaders}, {"swore revenge", player.NemesesMade},
            {"spared", player.Spared},             {"executed", player.Executed},    {"put you down", player.Downed},   {"pockets picked", player.Robbed},
            {"stashes emptied", player.Stashes},   {"holds cleared", player.Camps},  {"threatened", player.Threats},  {"bought off", player.Bribes},
        };
        std::vector<std::string> tiles;
        for (const auto& [label, value] : tallies)
            tiles.push_back(Object{}.Str("label", label).Num("value", value).Done());
        out.Raw("tallies", Array(tiles));

        std::vector<std::pair<int, std::string>> notoriety;
        for (const auto& [id, gang] : m_state.Gangs)
        {
            const int value = NotorietyOf(id, acPlayer);
            if (value <= 0)
                continue;
            const int level = Text::NotorietyLevel(value);
            notoriety.push_back({value, Object{}.Str("gang", id).Str("name", GangShort(id)).Num("value", value).Num("level", level).Str("word", Text::NotorietyName(level)).Done()});
        }
        std::sort(notoriety.begin(), notoriety.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::string> bars;
        for (const auto& [value, json] : notoriety)
            bars.push_back(json);
        out.Raw("notoriety", Array(bars));

        // His nemeses: the living first, by hate
        std::vector<const Record*> mine;
        for (const auto& [id, record] : m_state.Records)
            if (record.GrudgeOf(acPlayer))
                mine.push_back(&record);
        std::sort(mine.begin(), mine.end(),
                  [&](const Record* a, const Record* b)
                  {
                      const bool aAlive = a->State == Status::Alive || a->State == Status::PresumedDead;
                      const bool bAlive = b->State == Status::Alive || b->State == Status::PresumedDead;
                      if (aAlive != bAlive)
                          return aAlive;
                      return a->GrudgeOf(acPlayer)->Hate > b->GrudgeOf(acPlayer)->Hate;
                  });
        std::vector<std::string> nemeses;
        for (const Record* pRecord : mine)
            nemeses.push_back(box(*pRecord, ""));
        out.Raw("nemeses", Array(nemeses));

        // Their story: their own deeds, the newest first
        std::vector<std::string> story;
        int storyTotal = 0;
        for (auto it = m_state.Chronicle.rbegin(); it != m_state.Chronicle.rend(); ++it)
        {
            if (it->Player != acPlayer)
                continue;
            if (++storyTotal > 12)
                continue;
            std::string tag = it->Gangs.empty() ? (it->Men.empty() ? "you" : "a stranger") : GangShort(it->Gangs.front());
            story.push_back(Object{}.Str("at", Text::DateText(it->At)).Str("tag", tag).Str("text", it->Title + (it->Text.empty() ? std::string() : ". " + Text::Sentence(it->Text))).Done());
        }
        out.Raw("story", Array(story)).Num("storyMore", std::max(0, storyTotal - 12));

        // The named dead: men they killed for good, the newest first
        std::vector<const Record*> dead;
        for (const auto& [id, record] : m_state.Records)
            if (record.State == Status::Dead && record.KilledBy == acPlayer)
                dead.push_back(&record);
        std::sort(dead.begin(), dead.end(), [](const Record* a, const Record* b) { return LastSeen(*a) > LastSeen(*b); });
        std::vector<std::string> deadJson;
        for (size_t i = 0; i < dead.size() && i < 12; ++i)
            deadJson.push_back(Object{}.Num("id", dead[i]->Id).Str("name", dead[i]->FullName()).Done());
        out.Raw("dead", Array(deadJson)).Num("deadMore", static_cast<int64_t>(dead.size() > 12 ? dead.size() - 12 : 0));

        std::vector<std::string> party;
        for (const std::string& member : acParty)
        {
            if (member == acPlayer)
                continue;
            std::vector<std::string> theirs;
            for (const auto& [id, record] : m_state.Records)
            {
                const Grudge* pGrudge = record.GrudgeOf(member);
                if (pGrudge && record.IsAlive())
                    theirs.push_back(Object{}.Num("id", id).Str("name", record.FullName()).Str("standing", Text::HateWord(pGrudge->Hate)).Done());
            }
            party.push_back(Object{}.Str("player", member).Raw("nemeses", Array(theirs)).Done());
        }
        out.Raw("party", Array(party));
    }
    else if (page == "gang")
    {
        const std::string gangId = arg.empty() || !m_state.Gangs.count(arg) ? firstGang : arg;
        const auto it = m_state.Gangs.find(gangId);
        const GangDef* pDef = FindGang(gangId);
        if (it == m_state.Gangs.end() || !pDef)
            return out.Raw("gang", "null").Done();
        const GangState& gang = it->second;

        const int notoriety = NotorietyOf(gangId, acPlayer);
        const int level = Text::NotorietyLevel(notoriety);
        float groundStart = 0.f, groundNow = 0.f;
        for (const auto& [district, share] : pDef->Ground)
            groundStart += share;
        for (const auto& [district, share] : gang.Ground)
            groundNow += share;
        const auto ratio = [](float aNow, float aStart) { return aStart > 0.f ? std::clamp(aNow / aStart, 0.f, 1.f) : 0.f; };

        Object g;
        g.Str("id", gangId)
            .Str("name", pDef->Name)
            .Str("short", pDef->Short)
            .Str("leaderTitle", pDef->LeaderTitle)
            .Bool("destroyed", gang.Destroyed)
            .Bool("loners", (pDef->Flags & kLoners) != 0)
            .Raw("notoriety", Object{}.Num("value", notoriety).Num("level", level).Str("word", Text::NotorietyName(level)).Done())
            .Str("aim", AimWord(level))
            .Raw("strength", Object{}
                                 .Str("word", View::Strength(gang.Men, pDef->StartMen))
                                 .Num("men", gang.Men)
                                 .Real("ground", ratio(groundNow, groundStart))
                                 .Real("manpower", ratio(static_cast<float>(gang.Men), static_cast<float>(pDef->StartMen)))
                                 .Real("caps", ratio(static_cast<float>(gang.Purse), static_cast<float>(pDef->StartPurse)))
                                 .Real("supplies", std::clamp(gang.Supplies / 100.f, 0.f, 1.f))
                                 .Done());

        std::vector<std::string> wars;
        for (const War& war : m_state.Wars)
        {
            if (war.End || (war.A != gangId && war.B != gangId))
                continue;
            const District* pDistrict = FindDistrict(war.District);
            wars.push_back(Object{}.Str("against", GangShort(war.A == gangId ? war.B : war.A)).Str("over", pDistrict ? pDistrict->Name : war.District).Done());
        }
        g.Raw("wars", Array(wars));

        // The leader's seat
        const Record* pLeader = Find(gang.LeaderId);
        if (pLeader && (pLeader->State == Status::Alive || pLeader->State == Status::PresumedDead))
            g.Raw("leader", box(*pLeader, pDef->LeaderTitle));
        else
            g.Raw("leader", "null");
        g.Str("leaderNote", pLeader && pLeader->IsAlive() ? std::string()
                            : gang.FallenLeader.empty() ? std::string("The seat is empty.")
                                                         : "The seat is empty since " + gang.FallenLeader + " fell.");

        // The tiers: their men by rank, empty seats for the two above the bottom, the unnamed under the bottom
        std::vector<const Record*> men;
        for (const auto& [id, record] : m_state.Records)
            if (record.Gang == gangId && id != gang.LeaderId && (record.State == Status::Alive || record.State == Status::PresumedDead))
                men.push_back(&record);
        std::sort(men.begin(), men.end(), [](const Record* a, const Record* b) { return a->Rank != b->Rank ? a->Rank > b->Rank : a->RankXp > b->RankXp; });
        std::vector<std::string> tiers;
        int named = 0;
        for (size_t tier = 0; tier < 3 && !(pDef->Flags & kLoners); ++tier)
        {
            const auto [lo, hi] = TierRange(*pDef, tier);
            std::vector<std::string> seated;
            for (const Record* pMan : men)
                if ((pMan->Rank >= lo && pMan->Rank <= hi) || (tier == 0 && pMan->Rank >= 5))
                    seated.push_back(box(*pMan, pDef->Tiers[tier].Single));
            named += static_cast<int>(seated.size());
            const int vacant = tier < 2 ? std::max(0, pDef->Tiers[tier].Seats - static_cast<int>(seated.size())) : 0;
            tiers.push_back(Object{}.Str("label", pDef->Tiers[tier].Label).Str("single", pDef->Tiers[tier].Single).Raw("men", Array(seated)).Num("vacant", vacant).Done());
        }
        g.Raw("tiers", Array(tiers));
        if (pDef->Flags & kLoners)
        {
            std::vector<std::string> loners;
            for (const Record* pMan : men)
                loners.push_back(box(*pMan, ""));
            g.Raw("men", Array(loners));
        }
        g.Num("unnamed", std::max(0, gang.Men - named - (pLeader && pLeader->IsAlive() ? 1 : 0)));

        std::vector<const Record*> fallen;
        for (const auto& [id, record] : m_state.Records)
            if (record.Gang == gangId && record.State == Status::Dead)
                fallen.push_back(&record);
        std::sort(fallen.begin(), fallen.end(), [](const Record* a, const Record* b) { return LastSeen(*a) > LastSeen(*b); });
        std::vector<std::string> fallenJson;
        for (size_t i = 0; i < fallen.size() && i < 12; ++i)
            fallenJson.push_back(box(*fallen[i], ""));
        g.Raw("fallen", Array(fallenJson)).Num("fallenCount", static_cast<int64_t>(fallen.size()));
        out.Raw("gang", g.Done());
    }
    else if (page == "man")
    {
        const Record* pRecord = Find(argId);
        if (!pRecord)
            return out.Raw("man", "null").Done();
        const Record& record = *pRecord;
        const View::Knows knows = View::KnowsOf(record, acPlayer);
        const GangDef* pGang = FindGang(record.Gang);
        const Grudge* pGrudge = record.GrudgeOf(acPlayer);
        const Status shown = record.State == Status::PresumedDead ? Status::Dead : record.State;

        // His seat in the gang
        std::string seat = "rides alone";
        if (pGang)
        {
            seat = std::string("one of ") + pGang->Name;
            if (m_state.Gangs.count(record.Gang) && m_state.Gangs.at(record.Gang).LeaderId == record.Id)
                seat = std::string(pGang->LeaderTitle) + " of " + pGang->Name;
            else
                for (size_t tier = 0; tier < 3 && !(pGang->Flags & kLoners); ++tier)
                {
                    const auto [lo, hi] = TierRange(*pGang, tier);
                    if ((record.Rank >= lo && record.Rank <= hi) || (tier == 0 && record.Rank >= 5))
                    {
                        seat = std::string(pGang->Tiers[tier].Single) + " of " + pGang->Name;
                        break;
                    }
                }
        }

        Object man;
        man.Num("id", record.Id)
            .Bool("known", knows.Name)
            .Str("name", knows.Name ? record.Name : std::string())
            .Str("nickname", knows.Name ? record.Nickname : std::string())
            .Str("seat", seat)
            .Str("gang", record.Gang)
            .Num("rank", record.Rank)
            .Str("kind", Text::KindWord(record.Body))
            .Num("age", knows.Name ? year - record.BornYear : 0)
            .Str("trade", record.Trade)
            .Str("status", StatusKey(shown))
            .Str("statusWord", Text::StatusWord(shown))
            .Str("fate", shown == Status::Alive ? std::string() : record.State == Status::PresumedDead ? std::string("Presumed dead.") : record.Fate + ".")
            .Str("tone", ToneOf(pGrudge));
        if (knows.Temper)
            man.Str("temper", Text::PersonalityName(record.Temper))
                .Str("temperText", Text::PersonalityBlurb(record.Temper))
                .Str("wits", Text::WitsWord(record.Wits))
                .Str("home", record.Home)
                .Str("faith", record.Faith);
        if (pGrudge)
            man.Raw("grudge", Object{}
                                  .Num("hate", pGrudge->Hate)
                                  .Str("hateWord", Text::HateWord(pGrudge->Hate))
                                  .Num("fear", pGrudge->Fear)
                                  .Str("fearWord", Text::FearWord(pGrudge->Fear))
                                  .Str("intent", shown == Status::Alive ? Text::IntentWord(IntentOf(record, *pGrudge)) : "")
                                  .Str("origin", Text::OriginLine(record, pGrudge->From, pGrudge->Place))
                                  .Str("since", Text::DateText(pGrudge->Since))
                                  .Num("downedYou", pGrudge->TimesDownedThem)
                                  .Bool("mercy", pGrudge->Mercy)
                                  .Bool("informant", pGrudge->Informant)
                                  .Bool("owes", pGrudge->OwesThem)
                                  .Done());
        else
            man.Raw("grudge", "null");

        std::vector<std::string> scars;
        for (const Scar& scar : record.Scars)
            scars.push_back(Object{}.Str("text", scar.Text).Str("place", scar.Place).Done());
        man.Raw("scars", Array(scars)).Str("injuries", View::Injuries(record.Injuries));

        // What the player knows of his ties, as on the Pip-Boy
        std::vector<std::string> ties;
        const auto tie = [&ties](const std::string& acKind, const std::string& acText, uint32_t aId) { ties.push_back(Object{}.Str("kind", acKind).Str("text", acText).Num("id", aId).Done()); };
        if (const Record* pBoss = Find(record.BossId); pBoss && ((knows.Bits & kKnowsBoss) || pBoss->Known.count(acPlayer)))
            tie("boss", "Answers to " + pBoss->FullName(), pBoss->Id);
        if ((knows.Bits & kKnowsKin) && record.KinId)
            if (const Record* pKin = Find(record.KinId))
                tie("kin", std::string(Text::HisWord(record, true)) + " " + record.KinRelation + ", " + pKin->FullName(), pKin->Id);
        if ((knows.Bits & kKnowsCamp) && !record.Camp.empty())
            for (const CampHold& hold : m_state.Camps)
                if (hold.Location == record.Camp)
                    tie("camp", "Holds " + hold.Name, 0);
        if (knows.Bits & kKnowsHabits)
            for (const Habit& habit : record.Habits)
                tie("habit", "Goes to " + habit.Place + " around " + std::to_string(habit.Hour) + ":00", 0);
        if (knows.Temper)
            for (const Goal& goal : record.Goals)
                tie("goal", std::string(Text::HeWord(record, true)) + " " + (goal.Text.rfind("wants", 0) == 0 ? goal.Text : "wants to " + goal.Text), 0);
        for (const Grudge& other : record.Grudges)
            if (other.Player != acPlayer)
                tie("player", "Hates " + other.Player + " (" + Text::HateWord(other.Hate) + ")", 0);
        man.Raw("ties", Array(ties));

        std::vector<std::string> history;
        if (knows.Name)
            for (auto it = record.History.rbegin(); it != record.History.rend(); ++it)
                history.push_back(Object{}.Str("at", Text::DateText(it->At)).Num("tone", static_cast<int>(it->Style)).Str("text", it->Text).Done());
        man.Raw("history", Array(history));

        int wounds = 0;
        for (const Wound& wound : record.Wounds)
            wounds += wound.ByPlayer == acPlayer;
        man.Num("wounds", wounds).Bool("female", Text::IsFemale(record));
        out.Raw("man", man.Done());
    }
    else if (page == "world")
    {
        std::vector<std::string> wars;
        int ended = 0;
        for (auto it = m_state.Wars.rbegin(); it != m_state.Wars.rend(); ++it)
        {
            if (it->End && ++ended > 6)
                continue;
            const District* pDistrict = FindDistrict(it->District);
            wars.push_back(Object{}
                               .Str("a", GangShort(it->A))
                               .Str("b", GangShort(it->B))
                               .Str("over", pDistrict ? pDistrict->Name : it->District)
                               .Str("since", Text::DateText(it->Start))
                               .Num("battles", it->Battles)
                               .Num("deadA", it->DeadA)
                               .Num("deadB", it->DeadB)
                               .Num("heat", it->Heat)
                               .Bool("ended", it->End != 0)
                               .Str("result", Text::Sentence(it->Result))
                               .Done());
        }
        out.Raw("wars", Array(wars));

        std::vector<std::pair<int, std::string>> relations;
        for (const Relation& relation : m_state.Relations)
        {
            const auto a = m_state.Gangs.find(relation.A);
            const auto b = m_state.Gangs.find(relation.B);
            if (a == m_state.Gangs.end() || b == m_state.Gangs.end() || a->second.Destroyed || b->second.Destroyed || std::abs(relation.Value) < 30)
                continue;
            relations.push_back({std::abs(relation.Value), Object{}
                                                               .Str("a", GangShort(relation.A))
                                                               .Str("b", GangShort(relation.B))
                                                               .Num("value", relation.Value)
                                                               .Str("word", RelationWord(relation.Value))
                                                               .Str("why", relation.Why)
                                                               .Done()});
        }
        std::sort(relations.begin(), relations.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::string> relationJson;
        for (size_t i = 0; i < relations.size() && i < 16; ++i)
            relationJson.push_back(relations[i].second);
        out.Raw("relations", Array(relationJson));

        std::vector<std::string> chronicle;
        for (auto it = m_state.Chronicle.rbegin(); it != m_state.Chronicle.rend() && chronicle.size() < 60; ++it)
        {
            if (!it->Player.empty() && it->Player != acPlayer && it->Kind != static_cast<uint8_t>(EventKind::Deed))
                continue;
            chronicle.push_back(Object{}
                                    .Str("at", Text::DateText(it->At))
                                    .Str("tag", it->Player == acPlayer ? std::string("you") : std::string(EventTag(static_cast<EventKind>(it->Kind))))
                                    .Str("gang", it->Gangs.empty() ? std::string() : GangShort(it->Gangs.front()))
                                    .Str("title", it->Title)
                                    .Str("text", Text::Sentence(it->Text))
                                    .Str("place", it->Place)
                                    .Done());
        }
        out.Raw("chronicle", Array(chronicle));

        std::vector<std::string> funerals;
        for (const Funeral& funeral : m_state.Funerals)
        {
            const Record* pDead = Find(funeral.DeadId);
            funerals.push_back(Object{}
                                   .Str("who", pDead ? pDead->FullName() : std::string("One of theirs"))
                                   .Str("gang", GangShort(funeral.Gang))
                                   .Str("place", funeral.Place)
                                   .Str("at", Text::DateText(funeral.Start))
                                   .Done());
        }
        out.Raw("funerals", Array(funerals));
    }
    else if (page == "wounds")
    {
        // Men this player shot, the latest first; each with his wounds from them, in order
        std::vector<std::pair<int64_t, const Record*>> victims;
        for (const auto& [id, record] : m_state.Records)
        {
            int64_t latest = -1;
            for (const Wound& wound : record.Wounds)
                if (wound.ByPlayer == acPlayer)
                    latest = std::max(latest, wound.At);
            if (latest >= 0)
                victims.push_back({latest, &record});
        }
        std::sort(victims.begin(), victims.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        int total = 0, hits = 0;
        std::vector<std::string> list;
        for (const auto& [latest, pRecord] : victims)
        {
            const Record& record = *pRecord;
            const View::Knows knows = View::KnowsOf(record, acPlayer);
            std::string outcome = "survived";
            if (record.State == Status::Dead)
                outcome = record.KilledBy == acPlayer ? "killed" : "dead";
            else if (record.State == Status::PresumedDead)
                outcome = "left for dead";
            else if (record.State == Status::Gone)
                outcome = "gone";

            std::vector<std::string> wounds;
            for (const Wound& wound : record.Wounds)
            {
                if (wound.ByPlayer != acPlayer)
                    continue;
                ++total;
                hits += wound.Hits;
                wounds.push_back(Object{}
                                     .Num("part", static_cast<int>(wound.Where))
                                     .Str("where", PartWord(wound.Where))
                                     .Str("cause", CauseWord(wound.Cause))
                                     .Str("weapon", wound.Weapon)
                                     .Str("ammo", wound.Ammo)
                                     .Real("range", wound.Range / 100.f)
                                     .Num("damage", wound.Damage)
                                     .Num("hits", wound.Hits)
                                     .Str("severity", Severity(wound))
                                     .Bool("crippled", wound.Crippled)
                                     .Bool("lethal", wound.Lethal)
                                     .Str("at", Text::DateText(wound.At))
                                     .Str("place", wound.Place)
                                     .Done());
            }
            list.push_back(Object{}
                               .Num("id", record.Id)
                               .Str("name", knows.Name ? record.FullName() : "An unknown " + Lower(Text::KindWord(record.Body)))
                               .Str("gang", GangShort(record.Gang))
                               .Bool("nemesis", record.GrudgeOf(acPlayer) != nullptr)
                               .Str("outcome", outcome)
                               .Bool("female", Text::IsFemale(record))
                               .Raw("wounds", Array(wounds))
                               .Done());
        }
        out.Raw("victims", Array(list)).Num("men", static_cast<int64_t>(victims.size())).Num("total", total).Num("hits", hits).Num("selected", argId);
    }
    return out.Done();
}

std::string Engine::GunpointJson(const std::string& acPlayer, uint32_t aRecordId, uint32_t aActorServerId) const
{
    const uint32_t id = aRecordId ? aRecordId : RecordOfActor(aActorServerId);
    const Record* pRecord = Find(id);
    if (!pRecord)
        return "null";
    const Record& record = *pRecord;
    const View::Knows knows = View::KnowsOf(record, acPlayer);
    const Grudge* pGrudge = record.GrudgeOf(acPlayer);
    const GangDef* pGang = FindGang(record.Gang);

    // What the player sees on him: his temper once known, kin, the gun on him, how often they hit him just now
    std::vector<std::string> chips;
    const auto chip = [&chips](const std::string& acText, const char* acpTone) { chips.push_back(Object{}.Str("text", Upper(acText)).Str("tone", acpTone).Done()); };
    if (knows.Temper)
        chip(Text::PersonalityName(record.Temper), record.Temper == Personality::Greedy ? "gold" : "normal");
    if (record.KinId && (knows.Bits & kKnowsKin))
        chip("has kin", "normal");
    chip("at gunpoint", "aim");
    int recentHits = 0;
    for (const Wound& wound : record.Wounds)
        if (wound.ByPlayer == acPlayer && m_state.Clock - wound.At <= kHour)
            recentHits += wound.Hits;
    if (recentHits)
        chip("hit x" + std::to_string(recentHits), "blood");
    if (record.Liar && knows.Name)
        chip("liar", "blood");
    if (pGrudge && pGrudge->Informant)
        chip("informant", "good");
    else if (pGrudge && pGrudge->OwesThem)
        chip("owes you", "good");

    Object bar;
    bar.Num("id", record.Id)
        .Num("actor", record.LiveActor)
        .Str("name", knows.Name ? record.FullName() : "Unknown")
        .Str("gang", pGang ? std::string(pGang->Name) : std::string("rides alone"))
        .Num("rank", record.Rank)
        .Num("fear", pGrudge ? pGrudge->Fear : 0)
        .Str("fearWord", Text::FearWord(pGrudge ? pGrudge->Fear : 0))
        .Num("hate", pGrudge ? pGrudge->Hate : 0)
        .Str("tone", ToneOf(pGrudge))
        .Raw("chips", Array(chips));
    return bar.Done();
}
} // namespace Nemesis
