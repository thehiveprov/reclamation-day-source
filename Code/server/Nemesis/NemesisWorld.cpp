#include "NemesisEngine.h"
#include "NemesisWire.h"

#include <algorithm>
#include <cmath>

// Nemeses: the living world. Gangs are seeded, promote and succeed, earn and go broke, fight over ground; named men
// live their lives off screen. Everything here runs on game time from Engine::Advance.
namespace Nemesis
{
namespace
{
int Clamp(int aValue, int aLo = 0, int aHi = 100) noexcept
{
    return std::max(aLo, std::min(aHi, aValue));
}

const char* KinWord(Rng& aRng, bool aFemale)
{
    static const std::vector<const char*> s_men = {"brother", "cousin", "father", "son", "uncle", "nephew"};
    static const std::vector<const char*> s_women = {"sister", "cousin", "mother", "daughter", "aunt", "niece"};
    return aFemale ? aRng.Pick(s_women) : aRng.Pick(s_men);
}

bool IsFemale(Kind aBody) noexcept
{
    return aBody == Kind::HumanFemale || aBody == Kind::GhoulFemale;
}

std::string HeShe(const Record& acRecord, bool aCapital = false)
{
    const bool female = IsFemale(acRecord.Body);
    return aCapital ? (female ? "She" : "He") : (female ? "she" : "he");
}
} // namespace

// --- seeding

void Engine::Seed(const GangDef& acGang, int64_t aNow)
{
    GangState state;
    state.Id = acGang.Id;
    state.Men = acGang.StartMen;
    state.Purse = acGang.StartPurse;
    state.Supplies = 60;
    for (const auto& [district, share] : acGang.Ground)
        state.Ground[district] = share;
    m_state.Gangs[acGang.Id] = state;

    const int year = m_state.Campaign == "winter-of-atom" ? 2282 : 2287;
    const auto joinLine = [&](Record& aRecord)
    {
        History(aRecord, Tone::Dim, "Ran with " + std::string(acGang.Name) + " since " + std::to_string(year - m_rng.Range(0, 9)) + ".");
    };

    // Bands without a hierarchy: a few named men
    if (acGang.Flags & kLoners)
    {
        for (int i = m_rng.Range(6, 9); i > 0; --i)
        {
            Record& man = NewRecord(acGang.Id, RollBody(&acGang), m_rng.Range(1, 2), aNow);
            man.Seeded = true;
            man.RestUntil = 0;
            joinLine(man);
        }
        return;
    }

    // The leader
    Record* pLeader = nullptr;
    if (*acGang.LeaderName)
    {
        Kind body = Kind::HumanMale;
        const std::string name = acGang.LeaderName;
        if (name == "Mother Isolde" || name == "Ivey" || name == "Nisha" || name == "Mags Black")
            body = Kind::HumanFemale;
        else if (name == "Thrash")
            body = Kind::SuperMutant;
        pLeader = &NewRecord(acGang.Id, body, 5, aNow);
        pLeader->Name = name;
        pLeader->Protected = true;
        pLeader->Temper = Personality::Proud;
        History(*pLeader, Tone::Gold, "Leads " + std::string(acGang.Name) + ".");
    }
    else
    {
        pLeader = &NewRecord(acGang.Id, RollBody(&acGang), 5, aNow);
        GiveNickname(*pLeader, "rank");
        joinLine(*pLeader);
        History(*pLeader, Tone::Gold, "Took the " + std::string(acGang.LeaderTitle) + "'s seat of " + acGang.Name + ".");
    }
    pLeader->Seeded = true;
    pLeader->RestUntil = 0;
    m_state.Gangs[acGang.Id].LeaderId = pLeader->Id;

    // Officers and named soldiers
    std::vector<Record*> officers, middles, soldiers;
    const auto [lo0, hi0] = TierRange(acGang, 0);
    const auto [lo1, hi1] = TierRange(acGang, 1);
    for (int i = 0; i < acGang.Tiers[0].Seats; ++i)
    {
        Record& man = NewRecord(acGang.Id, RollBody(&acGang), m_rng.Range(lo0, hi0), aNow);
        man.BossId = pLeader->Id;
        officers.push_back(&man);
    }
    for (int i = 0; i < acGang.Tiers[1].Seats; ++i)
    {
        Record& man = NewRecord(acGang.Id, RollBody(&acGang), m_rng.Range(lo1, hi1), aNow);
        man.BossId = officers.empty() ? pLeader->Id : officers[static_cast<size_t>(m_rng.Range(0, static_cast<int>(officers.size()) - 1))]->Id;
        middles.push_back(&man);
    }
    for (int i = std::min(acGang.Tiers[2].Seats, 6); i > 0; --i)
    {
        Record& man = NewRecord(acGang.Id, RollBody(&acGang), 1, aNow);
        const std::vector<Record*>& bosses = middles.empty() ? officers : middles;
        man.BossId = bosses.empty() ? pLeader->Id : bosses[static_cast<size_t>(m_rng.Range(0, static_cast<int>(bosses.size()) - 1))]->Id;
        soldiers.push_back(&man);
    }

    std::vector<Record*> everyone = officers;
    everyone.insert(everyone.end(), middles.begin(), middles.end());
    everyone.insert(everyone.end(), soldiers.begin(), soldiers.end());
    for (Record* pMan : everyone)
    {
        pMan->Seeded = true;
        pMan->RestUntil = 0;
        joinLine(*pMan);
        if (pMan->Rank >= 3 && m_rng.Chance(50))
            GiveNickname(*pMan, "temper");
    }

    // Kin and friends among them
    for (Record* pMan : everyone)
    {
        if (pMan->KinId || !m_rng.Chance(25))
            continue;
        Record* pOther = everyone[static_cast<size_t>(m_rng.Range(0, static_cast<int>(everyone.size()) - 1))];
        if (pOther == pMan || pOther->KinId)
            continue;
        pMan->KinId = pOther->Id;
        pMan->KinRelation = KinWord(m_rng, IsFemale(pOther->Body));
        pOther->KinId = pMan->Id;
        pOther->KinRelation = KinWord(m_rng, IsFemale(pMan->Body));
        // Kin share a surname (not mutants and the faithful)
        const size_t space = pMan->Name.find(' ');
        if (space != std::string::npos && pOther->Name.find(' ') != std::string::npos && !(acGang.Flags & (kAtomNames | kMutantNames)))
            pOther->Name = pOther->Name.substr(0, pOther->Name.find(' ')) + pMan->Name.substr(space);
    }
    for (Record* pMan : everyone)
    {
        if (!m_rng.Chance(30))
            continue;
        Record* pOther = everyone[static_cast<size_t>(m_rng.Range(0, static_cast<int>(everyone.size()) - 1))];
        if (pOther != pMan && std::find(pMan->Friends.begin(), pMan->Friends.end(), pOther->Id) == pMan->Friends.end())
        {
            pMan->Friends.push_back(pOther->Id);
            pOther->Friends.push_back(pMan->Id);
        }
    }

    // Camps: officers first, then the middle tier
    std::vector<Record*> holders = officers;
    holders.insert(holders.end(), middles.begin(), middles.end());
    for (size_t i = 0; i < acGang.Camps.size(); ++i)
    {
        CampHold camp;
        camp.Location = acGang.Camps[i].first;
        camp.Name = acGang.Camps[i].second;
        camp.Gang = acGang.Id;
        if (i < holders.size())
        {
            camp.OfficerId = holders[i]->Id;
            holders[i]->Camp = camp.Location;
            History(*holders[i], Tone::Normal, "Holds " + camp.Name + " for " + acGang.Name + ".");
        }
        m_state.Camps.push_back(camp);
    }

    // Officers' habits: they visit another of the gang's holds at night or at dawn
    for (Record* pOfficer : officers)
    {
        if (acGang.Camps.size() < 2 || !m_rng.Chance(50))
            continue;
        const auto& [location, name] = acGang.Camps[static_cast<size_t>(m_rng.Range(0, static_cast<int>(acGang.Camps.size()) - 1))];
        if (pOfficer->Camp == location)
            continue;
        Habit habit;
        habit.Place = name;
        habit.Location = location;
        habit.Hour = m_rng.Chance(60) ? m_rng.Range(20, 23) : m_rng.Range(5, 8);
        habit.Length = m_rng.Range(2, 4);
        habit.DaysMask = 0;
        for (int day = m_rng.Range(2, 4); day > 0; --day)
            habit.DaysMask |= 1 << m_rng.Range(0, 6);
        pOfficer->Habits.push_back(habit);
    }

    Chronicle(EventKind::Word, std::string(acGang.Short) + " in the Commonwealth",
              std::string(acGang.Name) + ", about " + std::to_string(state.Men) + " strong, under " + pLeader->FullName() + ".", "", aNow)
        .Gangs.push_back(acGang.Id);
}

// --- the clock

void Engine::Advance(int64_t aNow)
{
    if (!IsStarted())
        return;
    if (aNow < m_state.Clock)
    {
        // The calendar went back (a host set the time): carry on from here
        m_state.Clock = aNow;
        m_state.NextEventAt = std::min(m_state.NextEventAt, aNow + 3 * kHour);
        m_state.NextDayAt = std::min(m_state.NextDayAt, (aNow / kDay + 1) * kDay + 6 * kHour);
        return;
    }

    // Bounded catch-up after a long sleep or wait: the rest is skipped, not queued
    for (int events = 0; m_state.NextEventAt <= aNow && events < 16; ++events)
    {
        m_state.Clock = m_state.NextEventAt;
        RunEvent(m_state.Clock);
        const bool war = std::any_of(m_state.Wars.begin(), m_state.Wars.end(), [](const War& acWar) { return acWar.End == 0; });
        m_state.NextEventAt += war ? m_rng.Range(60, 120) : m_rng.Range(90, 180);
    }
    if (m_state.NextEventAt <= aNow)
        m_state.NextEventAt = aNow + m_rng.Range(60, 120);

    for (int days = 0; m_state.NextDayAt <= aNow && days < 3; ++days)
    {
        m_state.Clock = m_state.NextDayAt;
        DailyTurn(m_state.Clock);
        m_state.NextDayAt += kDay;
    }
    if (m_state.NextDayAt <= aNow)
        m_state.NextDayAt = (aNow / kDay + 1) * kDay + 6 * kHour;

    m_state.Clock = aNow;

    // Back from the dead
    for (auto& [id, record] : m_state.Records)
    {
        if (record.State != Status::PresumedDead || aNow < record.BackAt)
            continue;
        record.State = Status::Alive;
        record.RestUntil = aNow + 6 * kHour;
        History(record, Tone::Blood, "Came back from the dead.");
        for (Grudge& grudge : record.Grudges)
        {
            AddHate(record, grudge, 8, grudge.From);
            Notices.push_back({grudge.Player, Wire::kRises, "Word is " + record.FullName() + " isn't dead after all."});
        }
        Dirty = true;
    }

    // Funerals over
    m_state.Funerals.erase(std::remove_if(m_state.Funerals.begin(), m_state.Funerals.end(), [aNow](const Funeral& acFuneral) { return aNow > acFuneral.End; }),
                           m_state.Funerals.end());
}

void Engine::DailyTurn(int64_t aNow)
{
    // Feelings fade
    for (auto& [id, record] : m_state.Records)
    {
        if (!record.IsAlive())
            continue;
        for (Memory& memory : record.Memories)
            memory.Intensity = std::max(0, memory.Intensity - 5);
        record.Memories.erase(std::remove_if(record.Memories.begin(), record.Memories.end(), [](const Memory& acMemory) { return acMemory.Intensity <= 0; }),
                              record.Memories.end());
        for (Grudge& grudge : record.Grudges)
        {
            if (!grudge.Informant)
                grudge.Fear = std::max(0, grudge.Fear - (record.Temper == Personality::Coward ? 1 : 3));
            const int64_t quiet = aNow - grudge.LastDealt;
            if (grudge.Hate < 35)
                grudge.Hate = std::max(5, grudge.Hate - 1);
            else if (grudge.Hate < 65 && quiet > 7 * kDay && (aNow / kDay) % 2 == 0)
                grudge.Hate = std::max(30, grudge.Hate - 1);
        }
        // Older men with scars start thinking about getting out
        const int age = (m_state.Campaign == "winter-of-atom" ? 2282 : 2287) - record.BornYear;
        if (!record.Protected && (age > 48 || record.Scars.size() >= 3) && m_rng.Chance(2) &&
            std::none_of(record.Goals.begin(), record.Goals.end(), [](const Goal& acGoal) { return acGoal.Type == GoalType::Settle; }))
        {
            record.Goals.push_back({GoalType::Settle, 0, "", "wants out of the life", aNow});
            History(record, Tone::Dim, "Talks about getting out.");
        }
    }

    // Notoriety fades a day after it last rose; KARMA's wicked draw the lawful, its saints the killers for hire
    for (auto& [gangId, gang] : m_state.Gangs)
    {
        const GangDef* pGang = FindGang(gangId);
        for (auto& [player, value] : gang.Notoriety)
        {
            if (aNow - gang.NotorietyAt[player] > kDay)
                value = std::max(0, value - 2);
        }
        if (!pGang || gang.Destroyed)
            continue;
        for (const auto& [name, player] : m_state.Players)
        {
            if (!player.Online)
                continue;
            if ((pGang->Flags & kLawful) && player.Alignment <= 1 && NotorietyOf(gangId, name) < 50)
                AddNotoriety(gangId, name, player.Alignment == 0 ? 3 : 1, "word of your deeds", aNow);
        }
    }

    // KARMA's extremes: a named hunter takes the contract on the very good or the very evil
    for (const auto& [name, player] : m_state.Players)
    {
        if (!player.Online || (player.Alignment != 0 && player.Alignment != 4) || !m_rng.Chance(25))
            continue;
        const std::string hunters = player.Alignment == 4 ? "blackledger" : "marshals";
        if (!m_state.Gangs.count(hunters))
            continue;
        bool hunted = false;
        for (const auto& [id, record] : m_state.Records)
            if (record.Gang == hunters && record.IsAlive() && record.GrudgeOf(name))
                hunted = true;
        if (hunted)
            continue;
        Record& hunter = NewRecord(hunters, RollBody(FindGang(hunters)), 3, aNow);
        EnsureGrudge(hunter, name, Origin::Bounty, "", aNow);
        GiveNickname(hunter, "temper");
        History(hunter, Tone::Normal, "Took a contract on " + name + ".");
        Notices.push_back({name, Wire::kRises, "Word is " + std::string(FindGang(hunters)->Name) + " put a price on your head. " + hunter.FullName() + " took it."});
    }

    for (auto& [gangId, gang] : m_state.Gangs)
    {
        if (gang.Destroyed)
            continue;
        Purses(gang, aNow);
        Succession(gang, aNow);
        Promotions(gang, aNow);

        // Collapse: no men, no ground, no leader
        float ground = 0.f;
        for (const auto& [district, share] : gang.Ground)
            ground += share;
        const GangDef* pGang = FindGang(gangId);
        if (pGang && gang.Men < 6 && ground < 0.05f && !gang.LeaderId && !(pGang->Flags & (kLoners | kHunters)))
        {
            gang.Destroyed = true;
            for (Record* pMan : NamedMen(gangId, true))
            {
                pMan->Gang.clear();
                pMan->BossId = 0;
                pMan->Camp.clear();
                History(*pMan, Tone::Dim, std::string("Rides alone since ") + pGang->Name + " fell apart.");
            }
            Chronicle(EventKind::Turmoil, std::string(pGang->Short) + " are finished", std::string(pGang->Name) + " fell apart. Their last men scattered.", "", aNow)
                .Gangs.push_back(gangId);
            WorldNotice(std::string(pGang->Name) + " are finished.");
        }
    }

    // Camps retaken
    for (CampHold& camp : m_state.Camps)
    {
        if (!camp.ClearedAt || aNow < camp.RetakeAt || !GangActive(camp.Gang))
            continue;
        camp.ClearedAt = 0;
        const Record* pOfficer = Find(camp.OfficerId);
        if (!pOfficer || !pOfficer->IsAlive())
        {
            camp.OfficerId = 0;
            for (Record* pMan : NamedMen(camp.Gang))
            {
                if (pMan->Camp.empty() && pMan->Rank >= 2)
                {
                    camp.OfficerId = pMan->Id;
                    pMan->Camp = camp.Location;
                    History(*pMan, Tone::Normal, "Took over " + camp.Name + ".");
                    break;
                }
            }
        }
        Chronicle(EventKind::Word, std::string(FindGang(camp.Gang)->Short) + " are back at " + camp.Name, "They moved back in.", camp.Name, aNow).Gangs.push_back(camp.Gang);
    }

    // Relations drift back toward the old blood; wars start and go on
    for (Relation& relation : m_state.Relations)
    {
        int baseline = 0;
        if (const GangDef* pGang = FindGang(relation.A))
            for (const auto& [other, value] : pGang->Relations)
                if (relation.B == other)
                    baseline = value;
        if (relation.Value < baseline)
            ++relation.Value;
        else if (relation.Value > baseline && m_rng.Chance(50))
            --relation.Value;
    }
    for (War& war : m_state.Wars)
        if (war.End == 0)
            WarTurn(war, aNow);
    MaybeDeclareWar(aNow);

    // Keep the last 30 wars
    if (m_state.Wars.size() > 30)
    {
        auto ended = std::find_if(m_state.Wars.begin(), m_state.Wars.end(), [](const War& acWar) { return acWar.End != 0; });
        if (ended != m_state.Wars.end())
            m_state.Wars.erase(ended);
    }
    Dirty = true;
}

// --- gangs

void Engine::Purses(GangState& aGang, int64_t aNow)
{
    const GangDef* pGang = FindGang(aGang.Id);
    if (!pGang)
        return;
    float ground = 0.f;
    for (const auto& [district, share] : aGang.Ground)
        ground += share;
    int held = 0;
    for (const CampHold& camp : m_state.Camps)
        if (camp.Gang == aGang.Id && !camp.ClearedAt)
            ++held;

    float startGround = 0.f;
    for (const auto& [district, share] : pGang->Ground)
        startGround += share;
    const float groundRatio = startGround > 0.f ? std::min(1.5f, ground / startGround) : 1.f;
    const int income = static_cast<int>(pGang->StartMen * 2 * (0.5f + 0.5f * groundRatio)) + held * 20 + ((pGang->Flags & kRaiders) ? 20 : 0);
    const int costs = aGang.Men * 2;
    aGang.Purse += income - costs;
    aGang.Supplies = Clamp(aGang.Supplies + 4);
    if (aGang.Purse < 0)
    {
        if (!aGang.BrokeSince)
            aGang.BrokeSince = aNow;
        aGang.Men = std::max(0, aGang.Men - std::max(1, aGang.Men * 3 / 100));
        aGang.Purse = 0;
        if (aNow - aGang.BrokeSince > 3 * kDay && m_rng.Chance(30))
            Chronicle(EventKind::Turmoil, std::string(pGang->Short) + " are broke", "Men are walking away from " + std::string(pGang->Name) + " for want of pay.", "", aNow)
                .Gangs.push_back(aGang.Id);
        return;
    }
    aGang.BrokeSince = 0;

    // Hiring
    if (aGang.Men < pGang->StartMen * 6 / 5 && aGang.Purse > 300)
    {
        const int hired = std::min(3, (aGang.Purse - 300) / 40);
        aGang.Men += hired;
        aGang.Purse -= hired * 40;
    }
}

void Engine::Promotions(GangState& aGang, int64_t aNow)
{
    const GangDef* pGang = FindGang(aGang.Id);
    if (!pGang || (pGang->Flags & kLoners))
        return;

    std::vector<Record*> men = NamedMen(aGang.Id);
    men.erase(std::remove_if(men.begin(), men.end(), [&](const Record* apMan) { return apMan->Id == aGang.LeaderId || apMan->Rank >= 5; }), men.end());

    // Rank ups inside a tier
    for (Record* pMan : men)
    {
        for (size_t tier = 0; tier < 3; ++tier)
        {
            const auto [lo, hi] = TierRange(*pGang, tier);
            if (pMan->Rank >= lo && pMan->Rank < hi && pMan->RankXp >= XpNeed(pMan->Rank))
            {
                pMan->RankXp -= XpNeed(pMan->Rank);
                RankUp(*pMan, "earned it");
                break;
            }
        }
    }

    // Free seats filled from the tier below: the most XP, then ambition
    for (size_t tier = 0; tier + 1 < 3; ++tier)
    {
        const auto [lo, hi] = TierRange(*pGang, tier);
        const auto [belowLo, belowHi] = TierRange(*pGang, tier + 1);
        int seated = 0;
        for (const Record* pMan : men)
            if (pMan->Rank >= lo && pMan->Rank <= hi)
                ++seated;
        while (seated < pGang->Tiers[tier].Seats)
        {
            Record* pBest = nullptr;
            for (Record* pMan : men)
            {
                if (pMan->Rank < belowLo || pMan->Rank > belowHi)
                    continue;
                if (std::any_of(pMan->Goals.begin(), pMan->Goals.end(), [](const Goal& acGoal) { return acGoal.Type == GoalType::Settle; }))
                    continue;
                if (!pBest || pMan->RankXp * 3 + pMan->Ambition > pBest->RankXp * 3 + pBest->Ambition)
                    pBest = pMan;
            }
            if (!pBest)
                break;
            const int before = pBest->Rank;
            pBest->Rank = lo;
            pBest->RankXp = 0;
            History(*pBest, Tone::Gold, "Promoted to " + std::string(pGang->Tiers[tier].Single) + " (rank " + std::to_string(before) + " to " + std::to_string(lo) + ").");
            Remember(*pBest, Emotion::Pride, 50, "promoted");
            if (pBest->Nickname.empty() && lo >= 3)
                GiveNickname(*pBest, "rank");
            for (const Grudge& grudge : pBest->Grudges)
                Notices.push_back({grudge.Player, Wire::kRises, pBest->FullName() + " is a " + pGang->Tiers[tier].Single + " of " + pGang->Name + " now."});
            Chronicle(EventKind::Promotion, pBest->FullName() + " moves up", "Now a " + std::string(pGang->Tiers[tier].Single) + " of " + pGang->Name + ".", "", aNow)
                .Men.push_back(pBest->Id);
            Identify(*pBest);
            ++seated;
        }
    }

    // The bottom tier: unnamed men step up into names while there are men to name
    int named = 0;
    const auto [bottomLo, bottomHi] = TierRange(*pGang, 2);
    for (const Record* pMan : men)
        if (pMan->Rank >= bottomLo && pMan->Rank <= bottomHi)
            ++named;
    const int wanted = std::min(pGang->Tiers[2].Seats, 6);
    if (named < wanted && aGang.Men > static_cast<int>(men.size()) + 8)
    {
        Record& man = NewRecord(aGang.Id, RollBody(pGang), 1, aNow);
        man.Seeded = true;
        man.RestUntil = 0;
        if (Record* pLeader = Leader(aGang.Id))
            man.BossId = pLeader->Id;
        History(man, Tone::Dim, "Made a name for " + std::string(Text::Himself(man)) + " in " + std::string(pGang->Name) + ".");
    }
}

void Engine::Succession(GangState& aGang, int64_t aNow)
{
    if (aGang.LeaderId || !aGang.SeatFillAt || aNow < aGang.SeatFillAt)
        return;
    const GangDef* pGang = FindGang(aGang.Id);
    if (!pGang)
        return;

    Record* pHeir = nullptr;
    auto it = m_state.Records.find(aGang.SuccessorId);
    if (it != m_state.Records.end() && it->second.IsAlive() && it->second.Gang == aGang.Id)
        pHeir = &it->second;
    if (!pHeir)
    {
        int best = -1;
        for (Record* pMan : NamedMen(aGang.Id))
        {
            const int score = pMan->Rank * 1000 + pMan->RankXp * 10 + pMan->Ambition;
            if (score > best)
                best = score, pHeir = pMan;
        }
    }
    aGang.SeatFillAt = 0;
    aGang.SuccessorId = 0;
    if (!pHeir)
        return;

    pHeir->Rank = 5;
    pHeir->RankXp = 0;
    pHeir->Camp.clear();
    aGang.LeaderId = pHeir->Id;
    for (auto& [id, other] : m_state.Records)
        if (other.Gang == aGang.Id && id != pHeir->Id && other.Rank >= 4)
            other.BossId = pHeir->Id;
    if (pHeir->Nickname.empty())
        GiveNickname(*pHeir, "rank");
    History(*pHeir, Tone::Gold, "Took the " + std::string(pGang->LeaderTitle) + "'s seat after " + aGang.FallenLeader + ".");
    Remember(*pHeir, Emotion::Pride, 70, "leads the gang");
    Chronicle(EventKind::Succession, pHeir->FullName() + " leads " + pGang->Name, "The seat of " + aGang.FallenLeader + " is filled.", "", aNow).Men.push_back(pHeir->Id);
    WorldNotice(pHeir->FullName() + " now leads " + pGang->Name + ".");
    for (const Grudge& grudge : pHeir->Grudges)
        Notices.push_back({grudge.Player, Wire::kRises, pHeir->FullName() + " leads " + pGang->Name + " now. " + HeShe(*pHeir, true) + " has more men to send."});
    aGang.FallenLeader.clear();
    Identify(*pHeir);
}

// --- wars

void Engine::MaybeDeclareWar(int64_t aNow)
{
    const int live = static_cast<int>(std::count_if(m_state.Wars.begin(), m_state.Wars.end(), [](const War& acWar) { return acWar.End == 0; }));
    if (live >= 3)
        return;
    for (const Relation& relation : m_state.Relations)
    {
        if (relation.Value > -60 || !m_rng.Chance(6))
            continue;
        const GangDef* pA = FindGang(relation.A);
        const GangDef* pB = FindGang(relation.B);
        if (!pA || !pB || (pA->Flags & kNoWars) || (pB->Flags & kNoWars) || !GangActive(relation.A) || !GangActive(relation.B))
            continue;
        if (m_state.Gangs[relation.A].Men < 25 || m_state.Gangs[relation.B].Men < 25 || LiveWar(relation.A, relation.B))
            continue;

        // Over a district they both hold
        std::string district;
        float best = 0.f;
        for (const District& candidate : Districts())
        {
            const float contested = std::min(GroundOf(relation.A, candidate.Key), GroundOf(relation.B, candidate.Key));
            if (contested > best)
                best = contested, district = candidate.Key;
        }
        if (district.empty() || best < 0.05f)
            continue;

        War war;
        war.Id = m_state.NextWarId++;
        war.A = relation.A;
        war.B = relation.B;
        war.District = district;
        war.Goal = "ground";
        war.Start = aNow;
        m_state.Wars.push_back(war);
        const std::string where = FindDistrict(district)->Name;
        Event& event = Chronicle(EventKind::War, "War: " + std::string(pA->Short) + " and " + pB->Short,
                                 std::string(pA->Name) + " and " + pB->Name + " are fighting over " + where + ".", where, aNow);
        event.Gangs = {relation.A, relation.B};
        WorldNotice(std::string("War: ") + pA->Name + " and " + pB->Name + " are fighting over " + where + ".");
        return;
    }
}

void Engine::WarTurn(War& aWar, int64_t aNow)
{
    const GangDef* pA = FindGang(aWar.A);
    const GangDef* pB = FindGang(aWar.B);
    if (!pA || !pB || !GangActive(aWar.A) || !GangActive(aWar.B))
    {
        aWar.End = aNow;
        aWar.Result = "It ended when one side fell apart.";
        return;
    }
    const std::string where = FindDistrict(aWar.District) ? FindDistrict(aWar.District)->Name : aWar.District;

    if (m_rng.Chance(60))
    {
        GangState& a = m_state.Gangs[aWar.A];
        GangState& b = m_state.Gangs[aWar.B];
        const auto strength = [&](const GangState& acGang)
        {
            return acGang.Men * (0.5f + GroundOf(acGang.Id, aWar.District)) * (0.5f + acGang.Supplies / 100.f) * (acGang.LeaderId ? 1.f : 0.8f) * (0.7f + 0.6f * m_rng.Unit());
        };
        const bool aWins = strength(a) >= strength(b);
        GangState& winner = aWins ? a : b;
        GangState& loser = aWins ? b : a;
        const int loserDead = std::max(1, loser.Men * m_rng.Range(3, 8) / 100);
        const int winnerDead = std::max(0, winner.Men * m_rng.Range(1, 4) / 100);
        loser.Men = std::max(0, loser.Men - loserDead);
        winner.Men = std::max(0, winner.Men - winnerDead);
        loser.Supplies = Clamp(loser.Supplies - 10);
        (aWins ? aWar.DeadB : aWar.DeadA) += loserDead;
        (aWins ? aWar.DeadA : aWar.DeadB) += winnerDead;
        ++aWar.Battles;
        aWar.Heat = std::max(0, aWar.Heat - 5);
        ShiftGround(winner.Id, loser.Id, aWar.District, 0.05f + 0.07f * m_rng.Unit());

        // Named men in the battle
        std::string named;
        for (GangState* pSide : {&loser, &winner})
        {
            std::vector<Record*> men = NamedMen(pSide->Id);
            men.erase(std::remove_if(men.begin(), men.end(), [](const Record* apMan) { return apMan->LiveActor || apMan->Rank >= 5; }), men.end());
            for (int i = m_rng.Range(0, 2); i > 0 && !men.empty(); --i)
            {
                Record* pMan = men[static_cast<size_t>(m_rng.Range(0, static_cast<int>(men.size()) - 1))];
                men.erase(std::remove(men.begin(), men.end(), pMan), men.end());
                const std::string enemy = FindGang(pSide == &loser ? winner.Id : loser.Id)->Name;
                OffscreenHurt(*pMan, enemy, pSide == &loser ? 20 : 10, where, aNow);
                if (pMan->IsAlive())
                    GainXp(*pMan, pSide == &winner ? 2 : 1, "fought " + enemy + " at " + where);
                named += (named.empty() ? "" : "; ") + pMan->FullName() + (pMan->State == Status::Dead ? " fell" : " came back hurt");
            }
        }
        Event& event = Chronicle(EventKind::War, std::string(FindGang(winner.Id)->Short) + " win a fight at " + where,
                                 std::string(FindGang(winner.Id)->Name) + " beat " + FindGang(loser.Id)->Name + " (" + std::to_string(loserDead) + " to " +
                                     std::to_string(winnerDead) + " dead)." + (named.empty() ? "" : " " + named + "."),
                                 where, aNow);
        event.Gangs = {aWar.A, aWar.B};
    }

    // The end: one side driven out, a truce, or a stalemate after twenty days
    std::string result;
    if (GroundOf(aWar.A, aWar.District) < 0.03f)
        result = std::string(pB->Name) + " drove " + pA->Name + " out of " + where + ".";
    else if (GroundOf(aWar.B, aWar.District) < 0.03f)
        result = std::string(pA->Name) + " drove " + pB->Name + " out of " + where + ".";
    else if (aWar.Battles >= 6 && aWar.Heat < 25 && m_rng.Chance(25))
    {
        result = std::string("A truce: ") + pA->Name + " and " + pB->Name + " stopped fighting over " + where + ".";
        ShiftRelation(aWar.A, aWar.B, 30, "a truce");
    }
    else if (aNow - aWar.Start > 20 * kDay)
        result = std::string("A stalemate: neither side could hold ") + where + ".";
    if (result.empty())
        return;
    aWar.End = aNow;
    aWar.Result = result;
    Event& event = Chronicle(EventKind::War, "The war over " + where + " is over", result, where, aNow);
    event.Gangs = {aWar.A, aWar.B};
    WorldNotice(result);
}

// --- off-screen events

void Engine::RunEvent(int64_t aNow)
{
    const bool war = std::any_of(m_state.Wars.begin(), m_state.Wars.end(), [](const War& acWar) { return acWar.End == 0; });
    const int clash = war ? 35 : 25;
    const int roll = m_rng.Range(0, clash + 12 + 30 + 8 + 6 + 19 - 1);
    if (roll < clash)
        SimClash(aNow);
    else if (roll < clash + 12)
        SimFeud(aNow);
    else if (roll < clash + 42)
        SimLife(aNow);
    else if (roll < clash + 50)
        SimRivalry(aNow);
    else if (roll < clash + 56)
        SimTurmoil(aNow);
    else
        SimWord(aNow);
}

void Engine::OffscreenHurt(Record& aRecord, const std::string& acBy, int aDeathPercent, const std::string& acPlace, int64_t aNow)
{
    if (aRecord.Protected || aRecord.LiveActor || !aRecord.IsAlive())
        return;
    if (m_rng.Chance(aDeathPercent) && !m_rng.Chance(CheatDeathChance(aRecord) / 2))
    {
        aRecord.State = Status::Dead;
        aRecord.Fate = "Killed by " + acBy + Text::At(acPlace);
        History(aRecord, Tone::Blood, aRecord.Fate + ".");
        for (const Grudge& grudge : aRecord.Grudges)
            Notices.push_back({grudge.Player, Wire::kDeath, "Word is " + aRecord.FullName() + " is dead. " + acBy + " got to " + (IsFemale(aRecord.Body) ? "her" : "him") + " first."});
        KinGrieve(aRecord, "", aNow);
        if (!aRecord.Gang.empty())
        {
            auto gang = m_state.Gangs.find(aRecord.Gang);
            if (gang != m_state.Gangs.end() && gang->second.LeaderId == aRecord.Id)
                SeatFallen(gang->second, aRecord, aNow);
            for (CampHold& camp : m_state.Camps)
                if (camp.OfficerId == aRecord.Id)
                    camp.OfficerId = 0;
        }
        Dirty = true;
        return;
    }

    const std::string wound = Text::WoundFor(m_rng, 0, 0);
    Scar scar;
    scar.Wound = wound;
    scar.Text = Text::ScarText(m_rng, wound);
    scar.Place = acPlace;
    scar.At = aNow;
    aRecord.Scars.push_back(scar);
    if (wound == "leg")
        aRecord.Injuries |= Wire::kLimp;
    else if (wound == "arm")
        aRecord.Injuries |= Wire::kBadArm;
    History(aRecord, Tone::Blood, "Hurt by " + acBy + Text::At(acPlace) + ": " + scar.Text + ".");
    if (aRecord.Nickname.empty() && m_rng.Chance(30))
        GiveNickname(aRecord, "scar:" + wound);
    Dirty = true;
}

void Engine::SimClash(int64_t aNow)
{
    // Two gangs at odds who share ground
    struct Pair
    {
        std::string A, B, District;
    };
    std::vector<Pair> pairs;
    for (const Relation& relation : m_state.Relations)
    {
        if (relation.Value > -20 || !GangActive(relation.A) || !GangActive(relation.B))
            continue;
        for (const District& district : Districts())
            if (GroundOf(relation.A, district.Key) >= 0.05f && GroundOf(relation.B, district.Key) >= 0.05f)
                pairs.push_back({relation.A, relation.B, district.Key});
    }
    if (pairs.empty())
        return SimLife(aNow);
    const Pair pair = m_rng.Pick(pairs);
    const std::string where = FindDistrict(pair.District)->Name;
    GangState& a = m_state.Gangs[pair.A];
    GangState& b = m_state.Gangs[pair.B];
    const bool aWins = a.Men * (0.5f + GroundOf(pair.A, pair.District)) * m_rng.Unit() >= b.Men * (0.5f + GroundOf(pair.B, pair.District)) * m_rng.Unit();
    GangState& winner = aWins ? a : b;
    GangState& loser = aWins ? b : a;
    loser.Men = std::max(0, loser.Men - m_rng.Range(1, 3));
    winner.Men = std::max(0, winner.Men - m_rng.Range(0, 1));
    ShiftGround(winner.Id, loser.Id, pair.District, 0.02f);
    ShiftRelation(pair.A, pair.B, -3, "a clash at " + where);

    std::string named;
    for (GangState* pSide : {&loser, &winner})
    {
        std::vector<Record*> men = NamedMen(pSide->Id);
        men.erase(std::remove_if(men.begin(), men.end(), [](const Record* apMan) { return apMan->LiveActor || apMan->Rank >= 5; }), men.end());
        if (men.empty() || !m_rng.Chance(60))
            continue;
        Record* pMan = men[static_cast<size_t>(m_rng.Range(0, static_cast<int>(men.size()) - 1))];
        const std::string enemy = FindGang(pSide == &loser ? winner.Id : loser.Id)->Name;
        if (pSide == &loser)
            OffscreenHurt(*pMan, enemy, 15, where, aNow);
        if (pMan->IsAlive())
            GainXp(*pMan, pSide == &winner ? 2 : 1, "a clash with " + enemy);
        named += (named.empty() ? "" : " ") + pMan->FullName() + (pMan->State == Status::Dead ? " was killed." : pSide == &winner ? " led the charge." : " was hurt.");
    }
    Event& event = Chronicle(EventKind::Clash, std::string(FindGang(pair.A)->Short) + " and " + FindGang(pair.B)->Short + " clash in " + where,
                             std::string(FindGang(winner.Id)->Name) + " came out on top." + (named.empty() ? "" : " " + named), where, aNow);
    event.Gangs = {pair.A, pair.B};
}

void Engine::SimFeud(int64_t aNow)
{
    std::vector<Record*> men;
    for (auto& [id, record] : m_state.Records)
        if (record.IsAlive() && !record.Protected && !record.LiveActor && !record.Gang.empty() && GangActive(record.Gang))
            men.push_back(&record);
    if (men.size() < 2)
        return;
    Record* pA = m_rng.Pick(men);
    Record* pB = nullptr;
    for (int attempt = 0; attempt < 12 && !pB; ++attempt)
    {
        Record* pOther = m_rng.Pick(men);
        if (pOther != pA && (pOther->Gang != pA->Gang ? RelationOf(pA->Gang, pOther->Gang).Value < 0 : pOther->Ambition >= 65 || pA->Ambition >= 65))
            pB = pOther;
    }
    if (!pB)
        return SimLife(aNow);

    const std::string why = pA->Gang == pB->Gang ? "a seat both wanted" : m_rng.Chance(50) ? "an old debt" : "caps, or pride, or both";
    const bool aWins = pA->Aggression + pA->Nerve + m_rng.Range(0, 60) >= pB->Aggression + pB->Nerve + m_rng.Range(0, 60);
    Record& winner = aWins ? *pA : *pB;
    Record& loser = aWins ? *pB : *pA;
    const District* pDistrict = DistrictAt(loser.World, loser.X, loser.Y);
    const std::string where = pDistrict ? pDistrict->Name : std::string();
    OffscreenHurt(loser, winner.FullName(), 25, where, aNow);
    GainXp(winner, 2, "won a feud with " + loser.Name);
    Remember(winner, Emotion::Pride, 40, "beat " + loser.Name);
    if (loser.IsAlive())
        Remember(loser, Emotion::Anger, 55, "beaten by " + winner.Name);
    History(winner, Tone::Normal, "Settled a feud with " + loser.Name + " over " + why + ".");
    Event& event = Chronicle(EventKind::Feud, winner.FullName() + " settles a score",
                             "With " + loser.FullName() + ", over " + why + ". " + (loser.State == Status::Dead ? loser.Name + " didn't get up." : loser.Name + " lived."),
                             where, aNow);
    event.Men = {winner.Id, loser.Id};
}

void Engine::SimLife(int64_t aNow)
{
    std::vector<Record*> men;
    for (auto& [id, record] : m_state.Records)
        if (record.IsAlive() && !record.Protected && !record.LiveActor)
            men.push_back(&record);
    if (men.empty())
        return;
    Record& man = *m_rng.Pick(men);
    const GangDef* pGang = FindGang(man.Gang);
    const bool settling = std::any_of(man.Goals.begin(), man.Goals.end(), [](const Goal& acGoal) { return acGoal.Type == GoalType::Settle; });
    int hottest = 0;
    for (const Grudge& grudge : man.Grudges)
        hottest = std::max(hottest, grudge.Hate);

    std::string title, text;
    const int roll = m_rng.Range(0, 99);
    if (settling && hottest < 50 && m_rng.Chance(35))
    {
        static const std::vector<std::string> s_lives = {"works caravan guard out of Bunker Hill", "farms near Graygarden", "keeps a stall in Diamond City",
                                                         "went south and didn't come back", "tends bar in Goodneighbor", "joined a settlement up north"};
        man.State = Status::Gone;
        man.Fate = "Went straight; " + m_rng.Pick(s_lives);
        man.Alignment = 1;
        if (!man.Gang.empty())
            GangLosesMan(man.Gang, 1);
        History(man, Tone::Good, man.Fate + ".");
        title = man.FullName() + " went straight";
        text = man.Fate + ".";
        for (const Grudge& grudge : man.Grudges)
            Notices.push_back({grudge.Player, Wire::kNormal, "Word is " + man.FullName() + " went straight."});
    }
    else if (roll < 15)
    {
        Remember(man, Emotion::Joy, 30, "a good drunk");
        title = man.Name + " on a bender";
        text = HeShe(man, true) + " drank away a week's take" + (m_rng.Chance(50) ? " in Goodneighbor." : " and picked a fight " + HeShe(man) + " doesn't remember.");
        if (m_rng.Chance(20))
            OffscreenHurt(man, "a bar fight", 0, "Goodneighbor", aNow);
    }
    else if (roll < 25 && man.Body != Kind::SuperMutant)
    {
        title = man.Name + " got married";
        text = "To someone " + HeShe(man) + " met on the road. The gang threw a party.";
        Remember(man, Emotion::Joy, 60, "married");
        if (m_rng.Chance(30) && !settling)
            man.Goals.push_back({GoalType::Settle, 0, "", "has a family to think of", aNow});
    }
    else if (roll < 33 && man.Faith != "Atom")
    {
        man.Faith = "Atom";
        title = man.Name + " found Atom";
        text = HeShe(man, true) + " came back from the Glowing Sea speaking of the Division.";
    }
    else if (roll < 43)
    {
        title = man.Name + " came down with a fever";
        if (m_rng.Chance(6))
        {
            man.State = Status::Dead;
            man.Fate = "Died of a fever";
            if (!man.Gang.empty())
            {
                GangLosesMan(man.Gang, 1);
                auto gang = m_state.Gangs.find(man.Gang);
                if (gang != m_state.Gangs.end() && gang->second.LeaderId == man.Id)
                    SeatFallen(gang->second, man, aNow);
            }
            KinGrieve(man, "", aNow);
            text = "It took " + std::string(IsFemale(man.Body) ? "her" : "him") + ".";
            for (const Grudge& grudge : man.Grudges)
                Notices.push_back({grudge.Player, Wire::kDeath, "Word is " + man.FullName() + " died of a fever."});
        }
        else
            text = HeShe(man, true) + " pulled through.";
    }
    else if (roll < 53 && (man.Temper == Personality::Greedy || man.Greed >= 60))
    {
        title = man.Name + " struck it rich";
        text = "A cache of pre-war caps" + std::string(pGang ? ", and " + std::string(pGang->Short) + " took their cut." : ".");
        if (pGang)
            m_state.Gangs[man.Gang].Purse += m_rng.Range(100, 400);
        Remember(man, Emotion::Joy, 50, "struck it rich");
    }
    else if (roll < 62 && man.KinId)
    {
        auto kin = m_state.Records.find(man.KinId);
        if (kin == m_state.Records.end() || !kin->second.IsAlive() || kin->second.Protected || kin->second.LiveActor)
            return;
        static const std::vector<std::string> s_deaths = {"a deathclaw", "a radstorm", "a mirelurk queen", "bad water", "a feral horde"};
        const std::string cause = m_rng.Pick(s_deaths);
        kin->second.State = Status::Dead;
        kin->second.Fate = "Killed by " + cause;
        if (!kin->second.Gang.empty())
            GangLosesMan(kin->second.Gang, 1);
        KinGrieve(kin->second, "", aNow);
        title = man.Name + " lost " + std::string(IsFemale(man.Body) ? "her " : "his ") + man.KinRelation;
        text = kin->second.Name + " was killed by " + cause + ".";
    }
    else if (roll < 72 && pGang && man.Loyalty < 40 && man.Rank < 4)
    {
        // Defects to a gang that isn't at odds with his own
        std::vector<std::string> options;
        for (const auto& [id, state] : m_state.Gangs)
        {
            const GangDef* pOther = FindGang(id);
            if (id != man.Gang && !state.Destroyed && pOther && !(pOther->Flags & (kHunters | kLoners | kLawful)) && RelationOf(man.Gang, id).Value > -40 &&
                std::string(pOther->LookPool) == pGang->LookPool)
                options.push_back(id);
        }
        if (options.empty())
            return;
        const std::string to = m_rng.Pick(options);
        title = man.FullName() + " changed sides";
        text = "Left " + std::string(pGang->Name) + " for " + FindGang(to)->Name + ".";
        GangLosesMan(man.Gang, 1);
        m_state.Gangs[to].Men += 1;
        History(man, Tone::Normal, text);
        man.Gang = to;
        man.BossId = 0;
        man.Camp.clear();
        man.Rank = std::max(1, man.Rank - 1);
        ShiftRelation(pGang->Id, to, -5, "took " + man.Name);
    }
    else if (roll < 80 && man.Ambition >= 65 && std::none_of(man.Goals.begin(), man.Goals.end(), [](const Goal& acGoal) { return acGoal.Type == GoalType::Rise; }))
    {
        man.Goals.push_back({GoalType::Rise, 0, "", "wants a bigger seat", aNow});
        title = man.Name + " wants more";
        text = HeShe(man, true) + "'s been eyeing the seat above " + (IsFemale(man.Body) ? "her." : "him.");
    }
    else
    {
        // Moves within the gang's ground
        if (pGang && !pGang->Ground.empty())
        {
            const District* pDistrict = FindDistrict(m_rng.Pick(pGang->Ground).first);
            if (pDistrict)
            {
                man.World = pDistrict->World;
                man.X = pDistrict->X + static_cast<float>(m_rng.Range(-6000, 6000));
                man.Y = pDistrict->Y + static_cast<float>(m_rng.Range(-6000, 6000));
                title = man.Name + " moved on";
                text = "Seen around " + std::string(pDistrict->Name) + " now.";
            }
        }
    }
    if (title.empty())
        return;
    Event& event = Chronicle(EventKind::Life, title, text, "", aNow);
    event.Men.push_back(man.Id);
    if (!man.Gang.empty())
        event.Gangs.push_back(man.Gang);
    Dirty = true;
}

void Engine::SimRivalry(int64_t aNow)
{
    // Two of a player's nemeses racing for the kill
    std::map<std::string, std::vector<Record*>> byPlayer;
    for (auto& [id, record] : m_state.Records)
        if (record.IsAlive() && !record.Protected)
            for (const Grudge& grudge : record.Grudges)
                if (grudge.Hate >= 35)
                    byPlayer[grudge.Player].push_back(&record);
    std::vector<std::string> players;
    for (const auto& [player, men] : byPlayer)
        if (men.size() >= 2)
            players.push_back(player);
    if (players.empty())
        return SimLife(aNow);
    const std::string player = m_rng.Pick(players);
    std::vector<Record*>& men = byPlayer[player];
    Record* pA = m_rng.Pick(men);
    Record* pB = m_rng.Pick(men);
    if (pA == pB)
        return;
    Remember(*pA, Emotion::Pride, 40, "swore to take " + player + " first");
    Remember(*pB, Emotion::Anger, 40, pA->Name + " boasted about " + player);
    if (Grudge* pGrudge = pB->GrudgeOf(player))
        AddHate(*pB, *pGrudge, 5, pGrudge->From);
    Event& event = Chronicle(EventKind::Rivalry, pA->FullName() + " and " + pB->FullName() + " race for " + player,
                             pA->Name + " swore to bring " + player + " down first. " + pB->Name + " didn't take it well.", "", aNow, player);
    event.Men = {pA->Id, pB->Id};
}

void Engine::SimTurmoil(int64_t aNow)
{
    std::vector<std::string> gangs;
    for (const auto& [id, state] : m_state.Gangs)
        if (!state.Destroyed)
            gangs.push_back(id);
    if (gangs.empty())
        return;
    const std::string gangId = m_rng.Pick(gangs);
    GangState& gang = m_state.Gangs[gangId];
    const GangDef* pGang = FindGang(gangId);
    if (!pGang)
        return;
    const int roll = m_rng.Range(0, 99);

    if (roll < 20)
    {
        // An officer murders the leader for the seat
        Record* pLeader = Leader(gangId);
        if (!pLeader || pLeader->Protected || pLeader->LiveActor)
            return SimLife(aNow);
        Record* pAssassin = nullptr;
        for (Record* pMan : NamedMen(gangId))
            if (pMan->Id != pLeader->Id && pMan->Rank >= 3 && pMan->Ambition >= 70 && pMan->Loyalty <= 40)
                pAssassin = pMan;
        if (!pAssassin)
            return SimLife(aNow);
        pLeader->State = Status::Dead;
        pLeader->Fate = "Murdered by " + pAssassin->FullName();
        History(*pLeader, Tone::Blood, pLeader->Fate + ".");
        KinGrieve(*pLeader, "", aNow);
        gang.FallenLeader = pLeader->FullName();
        gang.LeaderId = 0;
        gang.SuccessorId = pAssassin->Id;
        gang.SeatFillAt = aNow;
        for (const Grudge& grudge : pLeader->Grudges)
            Notices.push_back({grudge.Player, Wire::kDeath, "Word is " + pLeader->FullName() + " was murdered by " + pAssassin->FullName() + "."});
        Chronicle(EventKind::Turmoil, "Murder in " + std::string(pGang->Name), pAssassin->FullName() + " killed " + pLeader->FullName() + " for the seat.", "", aNow)
            .Men = {pAssassin->Id, pLeader->Id};
        Succession(gang, aNow);
    }
    else if (roll < 35)
    {
        // A truce in a war
        for (War& war : m_state.Wars)
        {
            if (war.End || (war.A != gangId && war.B != gangId))
                continue;
            war.End = aNow;
            war.Result = std::string("A truce: ") + FindGang(war.A)->Name + " and " + FindGang(war.B)->Name + " shook on it.";
            ShiftRelation(war.A, war.B, 30, "a truce");
            Chronicle(EventKind::War, "Truce", war.Result, "", aNow).Gangs = {war.A, war.B};
            WorldNotice(war.Result);
            return;
        }
        return SimLife(aNow);
    }
    else if (roll < 50)
    {
        // An alliance with a friendly gang
        for (Relation& relation : m_state.Relations)
        {
            if ((relation.A == gangId || relation.B == gangId) && relation.Value >= 10 && GangActive(relation.A) && GangActive(relation.B))
            {
                ShiftRelation(relation.A, relation.B, 20, "an alliance");
                Chronicle(EventKind::Turmoil, "Alliance", std::string(FindGang(relation.A)->Name) + " and " + FindGang(relation.B)->Name + " are working together.", "", aNow)
                    .Gangs = {relation.A, relation.B};
                return;
            }
        }
        return SimLife(aNow);
    }
    else if (roll < 65 && (pGang->Flags & kRaiders))
    {
        // The lawful crack down
        gang.Men = std::max(0, gang.Men - std::max(1, gang.Men * 6 / 100));
        Chronicle(EventKind::Turmoil, "Crackdown on " + std::string(pGang->Short), "Patrols hit " + std::string(pGang->Name) + " hard. They lost men.", "", aNow)
            .Gangs.push_back(gangId);
    }
    else if (roll < 80)
    {
        gang.Men = std::max(0, gang.Men - std::max(1, gang.Men * m_rng.Range(5, 10) / 100));
        Chronicle(EventKind::Turmoil, "Sickness in " + std::string(pGang->Short) + " camps", "A fever went through " + std::string(pGang->Name) + ".", "", aNow)
            .Gangs.push_back(gangId);
    }
    else
    {
        gang.Purse += m_rng.Range(250, 600);
        Chronicle(EventKind::Turmoil, std::string(pGang->Short) + " flush with caps", std::string(pGang->Name) + " pulled off a big score. They're hiring.", "", aNow)
            .Gangs.push_back(gangId);
    }
    Dirty = true;
}

void Engine::SimWord(int64_t aNow)
{
    // A rumour about a man a player knows: his habit or his camp
    struct Rumour
    {
        Record* pRecord;
        std::string Player;
    };
    std::vector<Rumour> rumours;
    for (auto& [id, record] : m_state.Records)
    {
        if (!record.IsAlive() || (record.Habits.empty() && record.Camp.empty()))
            continue;
        for (const auto& [player, known] : record.Known)
        {
            const auto it = m_state.Players.find(player);
            if (it == m_state.Players.end() || !it->second.Online)
                continue;
            if ((!record.Habits.empty() && !(known & kKnowsHabits)) || (!record.Camp.empty() && !(known & kKnowsCamp)))
                rumours.push_back({&record, player});
        }
    }
    if (rumours.empty())
        return;
    const Rumour rumour = m_rng.Pick(rumours);
    Record& record = *rumour.pRecord;
    uint32_t& known = record.Known[rumour.Player];
    std::string text;
    if (!record.Habits.empty() && !(known & kKnowsHabits))
    {
        const Habit& habit = record.Habits.front();
        known |= kKnowsHabits;
        text = "Word is " + record.FullName() + " goes to " + habit.Place + " around " + std::to_string(habit.Hour) + ":00.";
    }
    else
    {
        known |= kKnowsCamp;
        std::string camp = record.Camp;
        for (const CampHold& hold : m_state.Camps)
            if (hold.Location == record.Camp)
                camp = hold.Name;
        text = "Word is " + record.FullName() + " holds " + camp + ".";
    }
    Notices.push_back({rumour.Player, Wire::kNormal, text});
    Chronicle(EventKind::Word, "Word reaches you", text, "", aNow, rumour.Player).Men.push_back(record.Id);
}

// --- helpers

Relation& Engine::RelationOf(const std::string& acA, const std::string& acB)
{
    for (Relation& relation : m_state.Relations)
        if ((relation.A == acA && relation.B == acB) || (relation.A == acB && relation.B == acA))
            return relation;
    int value = 0;
    if (const GangDef* pGang = FindGang(acA))
        for (const auto& [other, start] : pGang->Relations)
            if (acB == other)
                value = start;
    m_state.Relations.push_back({acA, acB, value, "old blood"});
    return m_state.Relations.back();
}

void Engine::ShiftRelation(const std::string& acA, const std::string& acB, int aDelta, const std::string& acWhy)
{
    if (acA == acB || acA.empty() || acB.empty())
        return;
    Relation& relation = RelationOf(acA, acB);
    relation.Value = Clamp(relation.Value + std::max(-20, std::min(20, aDelta)), -100, 100);
    relation.Why = acWhy;
    Dirty = true;
}

float Engine::GroundOf(const std::string& acGang, const std::string& acDistrict) const noexcept
{
    const auto gang = m_state.Gangs.find(acGang);
    if (gang == m_state.Gangs.end() || gang->second.Destroyed)
        return 0.f;
    const auto it = gang->second.Ground.find(acDistrict);
    return it == gang->second.Ground.end() ? 0.f : it->second;
}

void Engine::ShiftGround(const std::string& acWinner, const std::string& acLoser, const std::string& acDistrict, float aShare)
{
    auto winner = m_state.Gangs.find(acWinner);
    auto loser = m_state.Gangs.find(acLoser);
    if (winner == m_state.Gangs.end() || loser == m_state.Gangs.end())
        return;
    float& lost = loser->second.Ground[acDistrict];
    const float moved = std::min(aShare, lost);
    lost -= moved;
    float& won = winner->second.Ground[acDistrict];
    won = std::min(1.f, won + moved);
    Dirty = true;
}

Event& Engine::Chronicle(EventKind aKind, const std::string& acTitle, const std::string& acText, const std::string& acPlace, int64_t aNow, const std::string& acPlayer)
{
    Event event;
    event.Id = m_state.NextEventId++;
    event.At = aNow;
    event.Kind = static_cast<uint8_t>(aKind);
    event.Title = acTitle;
    event.Text = acText;
    event.Place = acPlace;
    event.Player = acPlayer;
    m_state.Chronicle.push_back(event);
    if (m_state.Chronicle.size() > kMaxChronicle)
        m_state.Chronicle.erase(m_state.Chronicle.begin());
    Dirty = true;
    return m_state.Chronicle.back();
}

void Engine::WorldNotice(const std::string& acText)
{
    if (m_settings.WorldNews)
        Notices.push_back({"", Wire::kWorld, acText});
}

std::vector<Record*> Engine::NamedMen(const std::string& acGang, bool aIncludeProtected)
{
    std::vector<Record*> men;
    for (auto& [id, record] : m_state.Records)
        if (record.Gang == acGang && record.IsAlive() && (aIncludeProtected || !record.Protected))
            men.push_back(&record);
    return men;
}

Record* Engine::Leader(const std::string& acGang)
{
    const auto gang = m_state.Gangs.find(acGang);
    if (gang == m_state.Gangs.end())
        return nullptr;
    auto it = m_state.Records.find(gang->second.LeaderId);
    return it == m_state.Records.end() || !it->second.IsAlive() ? nullptr : &it->second;
}

War* Engine::LiveWar(const std::string& acA, const std::string& acB)
{
    for (War& war : m_state.Wars)
        if (!war.End && ((war.A == acA && war.B == acB) || (war.A == acB && war.B == acA)))
            return &war;
    return nullptr;
}

bool Engine::GangActive(const std::string& acGang) const noexcept
{
    const auto gang = m_state.Gangs.find(acGang);
    return gang != m_state.Gangs.end() && !gang->second.Destroyed;
}
} // namespace Nemesis
