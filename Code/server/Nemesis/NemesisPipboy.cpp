#include "NemesisEngine.h"
#include "NemesisView.h"
#include "NemesisWire.h"

#include <algorithm>

// Nemeses: the Pip-Boy's DATA > NEMESIS tab (PipboyTabs, RD_Nemeses.esp's entries): a page on top, one per nemesis, the
// gangs and the word on the road. Each player reads what they know: a man's name once met or told, his temper once
// questioned or fought twice, his kin, camp and habits once learned. The lines carry a marker (kPipboyTitle and the
// rest, NemesisTypes.h) the game turns into the overlay's look; the text is plain ASCII (the Pip-Boy's font).
namespace Nemesis
{
namespace
{
std::string Upper(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return aText;
}

using View::Injuries;
using View::Knows;
using View::KnowsOf;
using View::Strength;

// A page's text, line by line, each with its marker
struct Page
{
    std::string Text;

    Page& Line(char aMarker, const std::string& acText)
    {
        Text += aMarker + acText + "\n";
        return *this;
    }
    Page& Title(const std::string& acText) { return Line(kPipboyTitle, acText); }
    Page& Heading(const std::string& acText) { return Line(kPipboyHeading, acText); }
    Page& Dim(const std::string& acText) { return Line(kPipboyDim, acText); }
    Page& Body(const std::string& acText) { return Line(kPipboyBody, acText); }
    Page& Bar(int aPercent, const std::string& acLabel) { return Line(kPipboyBar, std::to_string(std::clamp(aPercent, 0, 100)) + kPipboyPartSeparator + acLabel); }
    Page& Dated(int64_t aAt, const std::string& acText) { return Line(kPipboyDated, Text::DateText(aAt) + kPipboyPartSeparator + acText); }
    Page& Space()
    {
        Text += "\n";
        return *this;
    }
};

void Add(std::string& aPages, size_t& aCount, const std::string& acTitle, const Page& acPage, bool aDot)
{
    if (aCount >= kPipboyPages)
        return;
    // The separators never appear in a title or a text (the part separator only where a line puts it)
    const auto separator = [](char c) { return c == kPipboyEntrySeparator || c == kPipboyFieldSeparator; };
    std::string title = acTitle;
    std::string text = acPage.Text;
    title.erase(std::remove_if(title.begin(), title.end(), [&](char c) { return separator(c) || c == kPipboyPartSeparator; }), title.end());
    text.erase(std::remove_if(text.begin(), text.end(), separator), text.end());
    aPages += title + kPipboyFieldSeparator + text + kPipboyFieldSeparator + (aDot ? "1" : "0") + kPipboyEntrySeparator;
    ++aCount;
}
} // namespace

std::string Engine::PipboyPages(const std::string& acPlayer, const std::vector<std::string>& acParty) const
{
    std::string pages;
    size_t count = 0;
    const auto playerIt = m_state.Players.find(acPlayer);
    const PlayerState player = playerIt == m_state.Players.end() ? PlayerState{} : playerIt->second;
    const int year = m_state.Campaign == "winter-of-atom" ? 2282 : 2287;

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

    // First, the entry that opens the overlay's full screen (pressed, PipboyTabs tells RD_Nemeses.esp's script, the
    // client sees its global); the client puts the hotkey's name for {KEY}
    {
        Page page;
        page.Title("FULL SCREEN").Space();
        page.Body("Press this entry to open the nemesis screen: your record, the gangs and who leads them, the world, and the "
                  "wound log.");
        page.Body("Or press {KEY} anywhere, in the Pip-Boy or out of it.");
        Add(pages, count, "OPEN FULL SCREEN", page, false);
    }

    // The top page
    {
        Page page;
        page.Title("YOUR NEMESES").Dim(Text::DateText(m_state.Clock)).Space();
        int living = 0;
        for (const Record* pRecord : mine)
            living += pRecord->State == Status::Alive || pRecord->State == Status::PresumedDead;
        page.Body("Karma: " + std::string(Text::KarmaWord(player.Alignment)) + " (" + std::to_string(player.Karma) + ")");
        page.Body("Men with a grudge against you: " + std::to_string(living));
        page.Body("Nemeses killed for good: " + std::to_string(player.Kills));
        if (!m_settings.Enabled)
            page.Space().Dim("Nemeses are switched off on this server.");

        bool heading = false;
        for (const auto& [id, gang] : m_state.Gangs)
        {
            const int value = NotorietyOf(id, acPlayer);
            const GangDef* pGang = FindGang(id);
            if (Text::NotorietyLevel(value) == 0 || !pGang)
                continue;
            if (!heading)
                page.Space().Heading("What the gangs think of you");
            heading = true;
            page.Bar(value, std::string(pGang->Short) + ": " + Text::NotorietyName(Text::NotorietyLevel(value)));
        }

        heading = false;
        for (const std::string& member : acParty)
        {
            if (member == acPlayer)
                continue;
            std::string names;
            for (const auto& [id, record] : m_state.Records)
            {
                const Grudge* pGrudge = record.GrudgeOf(member);
                if (pGrudge && record.IsAlive())
                    names += std::string(names.empty() ? "" : ", ") + record.FullName() + " (" + Text::HateWord(pGrudge->Hate) + ")";
            }
            if (names.empty())
                continue;
            if (!heading)
                page.Space().Heading("Your party's nemeses");
            heading = true;
            page.Body(member + ": " + names);
        }

        if (!m_state.Funerals.empty())
        {
            page.Space().Heading("Funerals");
            for (const Funeral& funeral : m_state.Funerals)
            {
                const Record* pDead = Find(funeral.DeadId);
                const GangDef* pGang = FindGang(funeral.Gang);
                page.Dated(funeral.Start, (pDead ? pDead->FullName() : std::string("One of theirs")) + " (" + (pGang ? pGang->Short : "") + "), " + funeral.Place);
            }
        }

        if (mine.empty())
            page.Space().Dim("Nobody holds a grudge against you yet. Men you shoot and leave alive, rob, cripple or humble will remember you.");
        Add(pages, count, "YOUR NEMESES", page, false);
    }

    // One page a nemesis (the Gangs and Word pages keep their two)
    for (const Record* pRecord : mine)
    {
        if (count >= kPipboyPages - 2)
            break;
        const Record& record = *pRecord;
        const Grudge& grudge = *record.GrudgeOf(acPlayer);
        const Knows knows = KnowsOf(record, acPlayer);
        const Status shown = record.State == Status::PresumedDead ? Status::Dead : record.State;
        const bool alive = shown == Status::Alive;

        Page page;
        page.Title(record.FullName());
        page.Dim(Text::Standing(record) + ", rank " + std::to_string(record.Rank) + " of 5");
        std::string who = Text::KindWord(record.Body);
        if (knows.Name)
            who += ", " + std::to_string(year - record.BornYear) + " years old";
        if (!record.Trade.empty())
            who += ", " + record.Trade;
        page.Dim(who);
        if (!alive)
            page.Space().Heading(Text::StatusWord(shown)).Body(record.State == Status::Dead || record.State == Status::Gone ? record.Fate + "." : "Presumed dead.");

        page.Space().Bar(grudge.Hate, Upper(Text::HateWord(grudge.Hate)) + ", " + Text::FearWord(grudge.Fear) + (alive ? ". " + std::string(Text::IntentWord(IntentOf(record, grudge))) : ""));
        page.Body(Text::OriginLine(record, grudge.From, grudge.Place)).Dim("Since " + Text::DateText(grudge.Since));
        if (grudge.TimesDownedThem)
            page.Body(std::string(Text::HeWord(record, true)) + " has put you down " + std::to_string(grudge.TimesDownedThem) + (grudge.TimesDownedThem == 1 ? " time." : " times."));
        if (grudge.Mercy)
            page.Body("You let " + std::string(Text::Him(record)) + " live once.");
        if (grudge.Informant)
            page.Body(std::string(Text::HeWord(record, true)) + " owes you, and sends word.");

        if (knows.Temper)
        {
            page.Space().Heading(Text::PersonalityName(record.Temper)).Body(Text::PersonalityBlurb(record.Temper));
            page.Dim(std::string("Wits: ") + Text::WitsWord(record.Wits) + (record.Home.empty() ? "" : ". From " + record.Home) +
                     (record.Faith.empty() ? "" : ", faithful to " + record.Faith) + ".");
        }

        const std::string injuries = Injuries(record.Injuries);
        if (!record.Scars.empty() || !injuries.empty())
        {
            page.Space().Heading("Scars");
            for (const Scar& scar : record.Scars)
                page.Body(scar.Text + (scar.Place.empty() ? std::string() : " (" + scar.Place + ")"));
            if (!injuries.empty())
                page.Dim(injuries);
        }

        Page ties;
        if (const Record* pBoss = Find(record.BossId); pBoss && ((knows.Bits & kKnowsBoss) || pBoss->Known.count(acPlayer)))
            ties.Body("Answers to " + pBoss->FullName() + ".");
        if ((knows.Bits & kKnowsKin) && record.KinId)
            if (const Record* pKin = Find(record.KinId))
                ties.Body(std::string(Text::HisWord(record, true)) + " " + record.KinRelation + ": " + pKin->FullName() + ".");
        if ((knows.Bits & kKnowsCamp) && !record.Camp.empty())
            for (const CampHold& hold : m_state.Camps)
                if (hold.Location == record.Camp)
                    ties.Body("Holds " + hold.Name + ".");
        if (knows.Bits & kKnowsHabits)
            for (const Habit& habit : record.Habits)
                ties.Body("Goes to " + habit.Place + " around " + std::to_string(habit.Hour) + ":00.");
        if (knows.Temper)
            for (const Goal& goal : record.Goals)
                ties.Body(std::string(Text::HeWord(record, true)) + " " + (goal.Text.rfind("wants", 0) == 0 ? goal.Text : "wants to " + goal.Text) + ".");
        std::string others;
        for (const Grudge& other : record.Grudges)
            if (other.Player != acPlayer)
                others += std::string(others.empty() ? "" : ", ") + other.Player + " (" + Text::HateWord(other.Hate) + ")";
        if (!others.empty())
            ties.Body(std::string(Text::HeWord(record, true)) + " hates " + others + " too.");
        if (!ties.Text.empty())
        {
            page.Space().Heading("What you know");
            page.Text += ties.Text;
        }

        if (knows.Name && !record.History.empty())
        {
            page.Space().Heading("History");
            for (auto it = record.History.rbegin(); it != record.History.rend(); ++it)
                page.Dated(it->At, it->Text);
        }

        // The dot: new, or came for the player in the last day
        const bool fresh = alive && (m_state.Clock - grudge.Since < kDay || (record.LastCameAt && m_state.Clock - record.LastCameAt < kDay));
        Add(pages, count, record.FullName() + (alive ? std::string() : " (" + std::string(Text::StatusWord(shown)) + ")"), page, fresh);
    }

    // The gangs
    {
        Page page;
        page.Title("THE GANGS").Dim(Text::DateText(m_state.Clock));
        for (const auto& [id, gang] : m_state.Gangs)
        {
            const GangDef* pGang = FindGang(id);
            if (!pGang)
                continue;
            page.Space().Heading(pGang->Name);
            if (gang.Destroyed)
            {
                page.Dim("Finished.");
                continue;
            }
            const Record* pLeader = Find(gang.LeaderId);
            if (pLeader && pLeader->IsAlive())
                page.Body(std::string(pGang->LeaderTitle) + ": " + (KnowsOf(*pLeader, acPlayer).Name ? pLeader->FullName() : "unknown to you"));
            else if (!(pGang->Flags & kLoners))
                page.Body(std::string("The ") + pGang->LeaderTitle + "'s seat is empty" + (gang.FallenLeader.empty() ? "" : " since " + gang.FallenLeader + " fell") + ".");
            page.Dim("About " + std::to_string(gang.Men) + " men, " + Strength(gang.Men, pGang->StartMen) + ".");

            std::string ground;
            for (const auto& [district, share] : gang.Ground)
            {
                if (share < 0.05f)
                    continue;
                const District* pDistrict = FindDistrict(district);
                ground += std::string(ground.empty() ? "" : ", ") + (pDistrict ? pDistrict->Name : district) + " " + std::to_string(static_cast<int>(share * 100.f + 0.5f)) + "%";
            }
            if (!ground.empty())
                page.Dim("Ground: " + ground);
            for (const War& war : m_state.Wars)
            {
                if (war.End || (war.A != id && war.B != id))
                    continue;
                const GangDef* pOther = FindGang(war.A == id ? war.B : war.A);
                const District* pDistrict = FindDistrict(war.District);
                page.Body(std::string("At war with ") + (pOther ? pOther->Name : "") + " over " + (pDistrict ? pDistrict->Name : war.District) + ".");
            }
            for (const CampHold& camp : m_state.Camps)
            {
                if (camp.Gang != id)
                    continue;
                const Record* pOfficer = Find(camp.OfficerId);
                if (pOfficer && pOfficer->Known.count(acPlayer) && (pOfficer->Known.at(acPlayer) & kKnowsCamp))
                    page.Body(pOfficer->FullName() + " holds " + camp.Name + (camp.ClearedAt ? " (cleared out for now)" : "") + ".");
            }
            if (const int value = NotorietyOf(id, acPlayer); Text::NotorietyLevel(value) > 0)
                page.Bar(value, std::string("You are ") + Text::NotorietyName(Text::NotorietyLevel(value)) + " to them");
        }
        Add(pages, count, "THE GANGS", page, false);
    }

    // The word on the road: the newest first, the player's own and everyone's
    {
        Page page;
        page.Title("WORD ON THE ROAD").Space();
        int lines = 0;
        for (auto it = m_state.Chronicle.rbegin(); it != m_state.Chronicle.rend() && lines < 40; ++it)
        {
            if (!it->Player.empty() && it->Player != acPlayer && static_cast<EventKind>(it->Kind) != EventKind::Deed)
                continue;
            ++lines;
            page.Dated(it->At, it->Title + (it->Text.empty() ? std::string() : ". " + Text::Sentence(it->Text)));
        }
        Add(pages, count, "WORD ON THE ROAD", page, false);
    }
    return pages;
}
} // namespace Nemesis
