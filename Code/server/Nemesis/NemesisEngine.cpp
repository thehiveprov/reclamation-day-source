#include "NemesisEngine.h"
#include "NemesisWire.h"

#include <algorithm>
#include <cmath>

namespace Nemesis
{
namespace
{
int Clamp(int aValue, int aLo = 0, int aHi = 100) noexcept
{
    return std::max(aLo, std::min(aHi, aValue));
}

int HourOf(int64_t aNow) noexcept
{
    return static_cast<int>((aNow % kDay) / kHour);
}

// The first hate a grudge starts with
int FirstHate(Origin aOrigin) noexcept
{
    switch (aOrigin)
    {
    case Origin::DownedYou: return 80;
    case Origin::LeftForDead: return 75;
    case Origin::Crippled: return 62;
    case Origin::Wounded: return 55;
    case Origin::ComradeKilled: return 48;
    case Origin::Robbed: return 44;
    case Origin::Gunfight: return 35;
    case Origin::Humbled: return 30;
    case Origin::GangOrders: return 55;
    case Origin::Bounty: return 70;
    default: return 30;
    }
}

bool Humiliation(Origin aOrigin) noexcept
{
    return aOrigin == Origin::Humbled || aOrigin == Origin::Robbed || aOrigin == Origin::Crippled;
}

std::string PlaceOr(const std::string& acPlace, const std::string& acWorld, float aX, float aY)
{
    if (!acPlace.empty())
        return acPlace;
    if (const District* pDistrict = DistrictAt(acWorld, aX, aY))
        return pDistrict->Name;
    return {};
}
} // namespace

Engine::Engine(uint32_t aSeed)
    : m_rng(aSeed)
{
}

// --- setup

void Engine::Start(const std::string& acCampaign, const std::vector<std::string>& acPlugins, int64_t aNow)
{
    m_plugins = acPlugins;
    const bool fresh = m_state.Campaign.empty();
    if (fresh)
    {
        m_state.Campaign = acCampaign.empty() ? std::string("2287") : acCampaign;
        m_state.Clock = aNow;
        m_state.NextEventAt = aNow + m_rng.Range(60, 150);
        m_state.NextDayAt = (aNow / kDay + 1) * kDay + 6 * kHour;
    }

    // Gangs of this campaign whose plugin is loaded, the first time it is
    for (const GangDef& gang : Gangs())
    {
        if (!InCampaign(gang, m_state.Campaign) || m_state.Gangs.count(gang.Id))
            continue;
        if (*gang.Plugin && std::find(acPlugins.begin(), acPlugins.end(), std::string(gang.Plugin)) == acPlugins.end())
            continue;
        Seed(gang, aNow);
    }

    // Starting relations between them
    for (const GangDef& gang : Gangs())
    {
        if (!m_state.Gangs.count(gang.Id))
            continue;
        for (const auto& [other, value] : gang.Relations)
        {
            if (!m_state.Gangs.count(other))
                continue;
            bool exists = false;
            for (const Relation& relation : m_state.Relations)
                exists |= (relation.A == gang.Id && relation.B == other) || (relation.A == other && relation.B == gang.Id);
            if (!exists)
                m_state.Relations.push_back({gang.Id, other, value, "old blood"});
        }
    }

    if (fresh)
        Chronicle(EventKind::Word, "The wasteland remembers", "Every gang keeps its own count of who wronged it. Yours starts now.", "", aNow);
    Dirty = true;
}

void Engine::Reset(int64_t aNow)
{
    const std::string campaign = m_state.Campaign;
    for (const auto& [id, encounter] : m_encounters)
        EndedEncounters.emplace_back(encounter.Player, id);
    m_encounters.clear();
    m_strangers.clear();
    std::map<std::string, PlayerState> players = m_state.Players;
    m_state = WorldState{};
    Start(campaign, m_plugins, aNow);
    for (auto& [name, player] : players)
    {
        player.ActiveEncounter = 0;
        player.Kills = 0;
        player.NextRollAt = aNow + 2 * kHour;
        m_state.Players[name] = player;
    }
    Dirty = true;
}

void Engine::PlayerJoined(const std::string& acPlayer, int64_t aNow)
{
    PlayerState& player = m_state.Players[acPlayer];
    player.Name = acPlayer;
    player.ActiveEncounter = 0;
    player.Online = true;
    // Nobody comes in the first hour of a visit
    player.NextRollAt = std::max(player.NextRollAt, aNow + kHour);
    Dirty = true;
}

void Engine::PlayerLeft(const std::string& acPlayer)
{
    auto it = m_state.Players.find(acPlayer);
    if (it == m_state.Players.end())
        return;
    it->second.Online = false;
    if (it->second.ActiveEncounter)
    {
        m_encounters.erase(it->second.ActiveEncounter);
        it->second.ActiveEncounter = 0;
    }
}

void Engine::SetKarma(const std::string& acPlayer, int aKarma, int aAlignment)
{
    PlayerState& player = m_state.Players[acPlayer];
    player.Name = acPlayer;
    player.Karma = aKarma;
    player.Alignment = Clamp(aAlignment, 0, 4);
}

// --- records

Kind Engine::RollBody(const GangDef* apGang)
{
    if (!apGang)
        return m_rng.Chance(30) ? Kind::HumanFemale : Kind::HumanMale;
    if (std::string(apGang->LookPool) == "mutant" || (apGang->Flags & kMutantNames))
        return Kind::SuperMutant;
    const bool female = m_rng.Chance(apGang->FemalePercent);
    if (m_rng.Chance(apGang->GhoulPercent))
        return female ? Kind::GhoulFemale : Kind::GhoulMale;
    return female ? Kind::HumanFemale : Kind::HumanMale;
}

std::set<std::string> Engine::TakenNames() const
{
    std::set<std::string> names(m_state.RetiredNames.begin(), m_state.RetiredNames.end());
    for (const auto& [id, record] : m_state.Records)
        names.insert(record.Name);
    return names;
}

Record& Engine::NewRecord(const std::string& acGang, Kind aBody, int aRank, int64_t aNow)
{
    const GangDef* pGang = FindGang(acGang);
    Record record;
    record.Id = m_state.NextId++;
    record.Gang = acGang;
    record.Body = aBody;
    record.Rank = Clamp(aRank, 1, 5);
    record.Name = Text::NewName(m_rng, aBody, pGang, TakenNames());
    record.BornAt = aNow;
    record.Alignment = pGang ? pGang->Alignment : 0;
    record.World = pGang ? pGang->World : kCommonwealth;
    RollTraits(record);

    // Where he roams: a district of his gang's ground
    if (pGang && !pGang->Ground.empty())
    {
        const auto& [districtKey, share] = pGang->Ground[static_cast<size_t>(m_rng.Range(0, static_cast<int>(pGang->Ground.size()) - 1))];
        if (const District* pDistrict = FindDistrict(districtKey))
        {
            record.World = pDistrict->World;
            record.X = pDistrict->X + static_cast<float>(m_rng.Range(-6000, 6000));
            record.Y = pDistrict->Y + static_cast<float>(m_rng.Range(-6000, 6000));
        }
    }

    auto [it, inserted] = m_state.Records.emplace(record.Id, std::move(record));
    Dirty = true;

    // Too many men kept: let go of the least of them (seeded men nobody met, the long dead)
    if (m_state.Records.size() > kMaxRecords)
    {
        uint32_t worst = 0;
        int worstScore = 1 << 30;
        for (const auto& [id, other] : m_state.Records)
        {
            if (id == it->first || other.BornAt == aNow || other.Protected || other.LiveActor || (!other.Gang.empty() && m_state.Gangs.count(other.Gang) && m_state.Gangs[other.Gang].LeaderId == id))
                continue;
            int score = other.Rank * 10 + static_cast<int>(other.Grudges.size()) * 50 + static_cast<int>(other.History.size());
            if (other.State == Status::Dead || other.State == Status::Gone)
                score -= 40;
            if (score < worstScore)
            {
                worstScore = score;
                worst = id;
            }
        }
        if (worst)
            Retire(worst);
    }
    return it->second;
}

void Engine::RollTraits(Record& aRecord)
{
    const GangDef* pGang = FindGang(aRecord.Gang);
    // Gang men lean loyal and hot; loners drift
    static const std::vector<std::pair<Personality, int>> s_gangOdds = {{Personality::Hothead, 22}, {Personality::Coward, 12}, {Personality::Proud, 14},
                                                                        {Personality::Cold, 15}, {Personality::Greedy, 15}, {Personality::Loyal, 22}};
    static const std::vector<std::pair<Personality, int>> s_lonerOdds = {{Personality::Hothead, 18}, {Personality::Coward, 18}, {Personality::Proud, 15},
                                                                         {Personality::Cold, 13}, {Personality::Greedy, 16}, {Personality::Drifter, 20}};
    const auto& odds = pGang && !(pGang->Flags & kLoners) ? s_gangOdds : s_lonerOdds;
    int total = 0;
    for (const auto& [temper, weight] : odds)
        total += weight;
    int roll = m_rng.Range(0, total - 1);
    for (const auto& [temper, weight] : odds)
    {
        roll -= weight;
        if (roll < 0)
        {
            aRecord.Temper = temper;
            break;
        }
    }

    const auto lean = [this](int aBase) { return Clamp(aBase + m_rng.Range(-20, 20)); };
    switch (aRecord.Temper)
    {
    case Personality::Hothead: aRecord.Aggression = lean(75), aRecord.Nerve = lean(60); break;
    case Personality::Coward: aRecord.Aggression = lean(30), aRecord.Nerve = lean(20); break;
    case Personality::Proud: aRecord.Aggression = lean(60), aRecord.Nerve = lean(65); break;
    case Personality::Cold: aRecord.Aggression = lean(50), aRecord.Nerve = lean(75); break;
    case Personality::Greedy: aRecord.Aggression = lean(45), aRecord.Nerve = lean(45), aRecord.Greed = lean(80); break;
    case Personality::Loyal: aRecord.Aggression = lean(55), aRecord.Nerve = lean(60), aRecord.Loyalty = lean(85); break;
    default: aRecord.Aggression = lean(40), aRecord.Nerve = lean(45), aRecord.Loyalty = lean(30); break;
    }
    if (aRecord.Temper != Personality::Greedy)
        aRecord.Greed = lean(45);
    if (aRecord.Temper != Personality::Loyal && aRecord.Temper != Personality::Drifter)
        aRecord.Loyalty = lean(pGang ? 55 : 35);
    aRecord.Honesty = lean(aRecord.Alignment > 0 ? 65 : 40);
    aRecord.Ambition = lean(aRecord.Temper == Personality::Proud ? 70 : 45);
    const int wits = m_rng.Range(0, 99);
    aRecord.Wits = wits < 22 ? 0 : wits < 80 ? 1 : 2;
    aRecord.BornYear = (m_state.Campaign == "winter-of-atom" ? 2282 : 2287) - m_rng.Range(19, 52);
    if (aRecord.Body == Kind::GhoulMale || aRecord.Body == Kind::GhoulFemale)
        aRecord.BornYear = m_rng.Chance(40) ? 2050 + m_rng.Range(0, 27) : aRecord.BornYear - m_rng.Range(30, 120);
    if (pGang && !pGang->Trades.empty())
        aRecord.Trade = m_rng.Pick(pGang->Trades);
    if (pGang && (pGang->Flags & kAtomNames))
        aRecord.Faith = "Atom";
    else if (m_rng.Chance(12))
        aRecord.Faith = m_rng.Chance(50) ? "Atom" : "the old churches";
    static const std::vector<std::string> s_homes = {"Diamond City", "Goodneighbor", "Bunker Hill", "the Capital Wasteland", "the Pitt", "a vault",
                                                     "a farm near Concord", "Quincy", "Lexington", "Salem", "Far Harbor", "the road", "nowhere anyone will say"};
    aRecord.Home = m_rng.Pick(s_homes);
}

Grudge& Engine::EnsureGrudge(Record& aRecord, const std::string& acPlayer, Origin aOrigin, const std::string& acPlace, int64_t aNow)
{
    if (Grudge* pGrudge = aRecord.GrudgeOf(acPlayer))
        return *pGrudge;

    Grudge grudge;
    grudge.Player = acPlayer;
    grudge.From = aOrigin;
    grudge.Place = acPlace;
    grudge.Since = aNow;
    grudge.LastDealt = aNow;
    aRecord.Grudges.push_back(grudge);
    Grudge& created = aRecord.Grudges.back();

    int hate = FirstHate(aOrigin);
    // His temper sets where he starts
    switch (aRecord.Temper)
    {
    case Personality::Hothead: hate += 10; break;
    case Personality::Coward: hate -= 15; break;
    case Personality::Proud: hate += Humiliation(aOrigin) ? 15 : 0; break;
    case Personality::Cold: hate -= 5; break;
    case Personality::Greedy: hate += aOrigin == Origin::Robbed ? 15 : -8; break;
    case Personality::Loyal: hate += aOrigin == Origin::ComradeKilled ? 15 : 0; break;
    default: hate -= 10; break;
    }
    // Karma picks the enemies: the lawful start hotter against the wicked, raiders against do-gooders
    const auto player = m_state.Players.find(acPlayer);
    const int alignment = player == m_state.Players.end() ? 2 : player->second.Alignment;
    if (const GangDef* pGang = FindGang(aRecord.Gang))
    {
        if ((pGang->Flags & kLawful) && alignment <= 1)
            hate += 15;
        if ((pGang->Flags & kRaiders) && alignment >= 3)
            hate += 8;
    }
    created.Hate = Clamp(static_cast<int>(std::lround(hate * m_settings.HateScale)), 10, 100);
    aRecord.Known[acPlayer] |= kKnowsName;
    aRecord.RestUntil = std::max(aRecord.RestUntil, aNow + kDay);
    Dirty = true;
    return created;
}

float Engine::HateGain(const Record& acRecord, Origin aWhy, const std::string& acPlayer) const noexcept
{
    float gain = 1.f;
    switch (acRecord.Temper)
    {
    case Personality::Hothead: gain = 1.3f; break;
    case Personality::Coward: gain = 0.7f; break;
    case Personality::Proud: gain = Humiliation(aWhy) ? 1.5f : 1.2f; break;
    case Personality::Cold: gain = 0.8f; break;
    case Personality::Greedy: gain = aWhy == Origin::Robbed ? 1.5f : 1.f; break;
    case Personality::Loyal: gain = aWhy == Origin::ComradeKilled ? 1.5f : 1.1f; break;
    default: gain = 0.8f; break;
    }
    gain *= 0.8f + acRecord.Aggression / 250.f;

    // Karma picks the enemies: raiders hate do-gooders more and respect the wicked; the lawful the reverse
    const auto it = m_state.Players.find(acPlayer);
    const int alignment = it == m_state.Players.end() ? 2 : it->second.Alignment;
    if (const GangDef* pGang = FindGang(acRecord.Gang))
    {
        if (pGang->Flags & kRaiders)
            gain *= alignment >= 3 ? 1.25f : alignment <= 1 ? 0.85f : 1.f;
        if (pGang->Flags & kLawful)
            gain *= alignment <= 1 ? 1.4f : alignment >= 3 ? 0.6f : 0.9f;
    }
    return std::max(0.5f, std::min(1.8f, gain));
}

float Engine::FearGain(const Record& acRecord, const std::string& acPlayer) const noexcept
{
    float gain = 1.f;
    switch (acRecord.Temper)
    {
    case Personality::Hothead: gain = 0.7f; break;
    case Personality::Coward: gain = 1.6f; break;
    case Personality::Proud: gain = 0.6f; break;
    case Personality::Cold: gain = 0.8f; break;
    case Personality::Loyal: gain = 0.9f; break;
    case Personality::Drifter: gain = 1.2f; break;
    default: break;
    }
    gain *= 1.5f - acRecord.Nerve / 100.f;

    // Karma colours fear: the wicked frighten more, the good less
    const auto it = m_state.Players.find(acPlayer);
    const int alignment = it == m_state.Players.end() ? 2 : it->second.Alignment;
    gain *= alignment == 0 ? 1.3f : alignment == 1 ? 1.15f : alignment == 4 ? 0.85f : 1.f;
    return std::max(0.4f, std::min(2.f, gain)) * m_settings.FearScale;
}

void Engine::AddHate(Record& aRecord, Grudge& aGrudge, int aDelta, Origin aWhy)
{
    if (aDelta > 0)
    {
        aDelta = std::max(1, static_cast<int>(std::lround(aDelta * HateGain(aRecord, aWhy, aGrudge.Player) * m_settings.HateScale)));
        // Slows near the top
        if (aGrudge.Hate >= 80)
            aDelta = std::max(1, static_cast<int>(std::lround(aDelta * (1.f - aGrudge.Hate / 120.f))));
    }
    aGrudge.Hate = Clamp(aGrudge.Hate + aDelta);
    Dirty = true;
}

void Engine::AddFear(Record& aRecord, Grudge& aGrudge, int aDelta)
{
    if (aDelta > 0)
        aDelta = std::max(1, static_cast<int>(std::lround(aDelta * FearGain(aRecord, aGrudge.Player))));
    aGrudge.Fear = Clamp(aGrudge.Fear + aDelta);
    Dirty = true;
}

void Engine::History(Record& aRecord, Tone aTone, const std::string& acText)
{
    aRecord.History.push_back({m_state.Clock, aTone, acText});
    if (aRecord.History.size() > kMaxHistory)
        aRecord.History.erase(aRecord.History.begin());
    Dirty = true;
}

void Engine::Remember(Record& aRecord, Emotion aFeeling, int aIntensity, const std::string& acCause)
{
    aRecord.Memories.push_back({aFeeling, Clamp(aIntensity), acCause, m_state.Clock});
    if (aRecord.Memories.size() > kMaxMemories)
    {
        // The faintest goes
        auto weakest = std::min_element(aRecord.Memories.begin(), aRecord.Memories.end(), [](const Memory& a, const Memory& b) { return a.Intensity < b.Intensity; });
        aRecord.Memories.erase(weakest);
    }
}

void Engine::GainXp(Record& aRecord, int aAmount, const std::string& acWhy)
{
    if (!aRecord.IsAlive() || aAmount <= 0 || aRecord.Rank >= 5)
        return;
    for (const Goal& goal : aRecord.Goals)
        if (goal.Type == GoalType::Settle)
            return;

    const float ambition = 0.5f + aRecord.Ambition / 100.f;
    aRecord.RankXp += std::max(1, static_cast<int>(std::lround(aAmount * ambition)));
    // Loners rise at once; gang men wait for a seat (Promotions)
    if (aRecord.Gang.empty() || !m_state.Gangs.count(aRecord.Gang))
    {
        while (aRecord.Rank < 5 && aRecord.RankXp >= XpNeed(aRecord.Rank))
        {
            aRecord.RankXp -= XpNeed(aRecord.Rank);
            RankUp(aRecord, acWhy);
        }
    }
    Dirty = true;
}

void Engine::RankUp(Record& aRecord, const std::string& acWhy)
{
    ++aRecord.Rank;
    History(aRecord, Tone::Gold, "Rose to rank " + std::to_string(aRecord.Rank) + ": " + acWhy + ".");
    Remember(aRecord, Emotion::Pride, 45, std::string("made a name for ") + Text::Himself(aRecord));
    if (aRecord.Nickname.empty() && aRecord.Rank >= 3)
        GiveNickname(aRecord, "rank");
    for (const Grudge& grudge : aRecord.Grudges)
        Notices.push_back({grudge.Player, Wire::kRises, aRecord.FullName() + " is making a name: rank " + std::to_string(aRecord.Rank) + "."});
    Identify(aRecord);
}

void Engine::GiveNickname(Record& aRecord, const std::string& acWhy)
{
    if (aRecord.Body == Kind::SuperMutant && acWhy == "temper")
        return;
    aRecord.Nickname = Text::Nickname(m_rng, aRecord, acWhy);
    Identify(aRecord);
}

void Engine::Identify(Record& aRecord)
{
    if (aRecord.LiveActor)
        Live.push_back({LiveChange::kIdentity, aRecord.LiveActor, aRecord.Id});
}

void Engine::Retire(uint32_t aId)
{
    auto it = m_state.Records.find(aId);
    if (it == m_state.Records.end())
        return;
    m_state.RetiredNames.push_back(it->second.Name);
    if (m_state.RetiredNames.size() > 300)
        m_state.RetiredNames.erase(m_state.RetiredNames.begin());
    for (auto& [id, other] : m_state.Records)
    {
        if (other.KinId == aId)
            other.KinId = 0;
        if (other.BossId == aId)
            other.BossId = 0;
        other.Friends.erase(std::remove(other.Friends.begin(), other.Friends.end(), aId), other.Friends.end());
    }
    for (CampHold& camp : m_state.Camps)
        if (camp.OfficerId == aId)
            camp.OfficerId = 0;
    m_state.Records.erase(it);
    Dirty = true;
}

int Engine::LivingGrudges(const std::string& acPlayer) const noexcept
{
    int count = 0;
    for (const auto& [id, record] : m_state.Records)
        if (record.State == Status::Alive || record.State == Status::PresumedDead)
            if (record.GrudgeOf(acPlayer))
                ++count;
    return count;
}

const Record* Engine::Find(uint32_t aId) const noexcept
{
    const auto it = m_state.Records.find(aId);
    return it == m_state.Records.end() ? nullptr : &it->second;
}

uint32_t Engine::RecordOfActor(uint32_t aActorServerId) const noexcept
{
    if (!aActorServerId)
        return 0;
    for (const auto& [id, record] : m_state.Records)
        if (record.LiveActor == aActorServerId)
            return id;
    return 0;
}

// --- the rules

int Engine::NotorietyOf(const std::string& acGang, const std::string& acPlayer) const noexcept
{
    const auto gang = m_state.Gangs.find(acGang);
    if (gang == m_state.Gangs.end())
        return 0;
    const auto it = gang->second.Notoriety.find(acPlayer);
    return it == gang->second.Notoriety.end() ? 0 : it->second;
}

int Engine::EffectiveFear(const Record& acRecord, const Grudge& acGrudge) const noexcept
{
    return Clamp(acGrudge.Fear + NotorietyOf(acRecord.Gang, acGrudge.Player) / 4);
}

Intent Engine::IntentOf(const Record& acRecord, const Grudge& acGrudge) const noexcept
{
    if (!acRecord.IsAlive() || acRecord.Protected)
        return Intent::WontHunt;
    for (const Goal& goal : acRecord.Goals)
        if (goal.Type == GoalType::Settle)
            return Intent::WontHunt;

    const int fear = EffectiveFear(acRecord, acGrudge);
    const bool fiery = acRecord.Temper == Personality::Hothead || acRecord.Temper == Personality::Proud;
    const bool canHire = acRecord.Temper == Personality::Greedy || acRecord.Rank >= 3 || acRecord.Wits >= 2 || acRecord.Greed >= 70;
    const GangDef* pGang = FindGang(acRecord.Gang);

    if (acGrudge.From == Origin::Bounty)
        return Intent::Hunts;
    if (acGrudge.From == Origin::DownedYou)
        return fear < 85 || fiery ? Intent::Hunts : canHire ? Intent::Hires : Intent::WontHunt;
    if (acGrudge.From == Origin::GangOrders)
        return fear < 75 ? Intent::Hunts : Intent::ProvokedOnly;
    if (acGrudge.Informant || acGrudge.OwesThem)
        return Intent::WontHunt;
    if (fear >= 80 && !fiery)
        return canHire && acGrudge.Hate >= 65 ? Intent::Hires : Intent::WontHunt;

    switch (Text::TierOf(acGrudge.Hate))
    {
    case HateTier::Murderous:
        if ((fear >= 60 && !fiery) || (acRecord.Nerve < 30 && acRecord.Aggression < 35))
            return canHire ? Intent::Hires : Intent::WontHunt;
        return Intent::Hunts;
    case HateTier::Vengeful:
        if (fiery && acRecord.Aggression >= 60 && fear < 60)
            return Intent::Hunts;
        if (canHire && fear >= 40)
            return Intent::Hires;
        // The lawful come for the wicked by duty
        if (pGang && (pGang->Flags & kLawful))
        {
            const auto it = m_state.Players.find(acGrudge.Player);
            if (it != m_state.Players.end() && it->second.Alignment <= 1)
                return Intent::Hunts;
        }
        return Intent::ProvokedOnly;
    default:
        return acRecord.Temper == Personality::Hothead && acRecord.Aggression >= 80 ? Intent::Hunts : Intent::ProvokedOnly;
    }
}

int Engine::CheatDeathChance(const Record& acRecord) const noexcept
{
    const int chance = m_settings.CheatDeathBase + m_settings.CheatDeathPerRank * (acRecord.Rank - 1) - 10 * acRecord.TimesCheatedDeath;
    return Clamp(chance, 0, m_settings.CheatDeathMax);
}

// --- deeds

Record* Engine::RecordFor(const DeedInput& acDeed)
{
    if (acDeed.RecordId)
    {
        auto it = m_state.Records.find(acDeed.RecordId);
        if (it != m_state.Records.end())
            return &it->second;
    }
    if (const uint32_t id = RecordOfActor(acDeed.ActorServerId))
        return &m_state.Records[id];
    return nullptr;
}

Record* Engine::StrangerToRecord(const std::string& acPlayer, const DeedInput& acDeed, Origin aOrigin, int64_t aNow)
{
    // A generic raider: the crew holding the ground he was met on
    std::string gang = acDeed.Gang;
    if (gang == "raiders")
    {
        gang.clear();
        float best = 0.f;
        const District* pDistrict = DistrictAt(acDeed.World, acDeed.X, acDeed.Y);
        for (const auto& [id, state] : m_state.Gangs)
        {
            const GangDef* pGang = FindGang(id);
            if (!pGang || !(pGang->Flags & kRaiders) || std::string(pGang->LookPool) != "raider" || state.Destroyed)
                continue;
            const float share = pDistrict ? GroundOf(id, pDistrict->Key) : 0.f;
            if (gang.empty() || share > best)
            {
                gang = id;
                best = share;
            }
        }
    }
    if (!gang.empty() && (!m_state.Gangs.count(gang) || m_state.Gangs[gang].Destroyed))
        gang.clear();

    Record& record = NewRecord(gang, acDeed.Body, 1, aNow);
    if (acDeed.AppearanceFlags && !acDeed.Appearance.empty())
    {
        record.Appearance = acDeed.Appearance;
        record.AppearanceFlags = acDeed.AppearanceFlags;
    }
    record.World = acDeed.World.empty() ? record.World : acDeed.World;
    if (acDeed.X != 0.f || acDeed.Y != 0.f)
    {
        record.X = acDeed.X;
        record.Y = acDeed.Y;
    }
    record.LiveActor = acDeed.ActorServerId;
    if (gang.empty())
        record.Alignment = -1;

    const std::string place = PlaceOr(acDeed.Place, acDeed.World, acDeed.X, acDeed.Y);
    Grudge& grudge = EnsureGrudge(record, acPlayer, aOrigin, place, aNow);
    History(record, Tone::Blood, Text::OriginLine(record, aOrigin, place) + " (" + acPlayer + ")");
    if (gang.empty())
        History(record, Tone::Dim, "Rides alone.");
    else
        History(record, Tone::Dim, "One of " + std::string(FindGang(gang)->Name) + ".");
    if (aOrigin == Origin::DownedYou)
    {
        grudge.TimesDownedThem = 1;
        Remember(record, Emotion::Pride, 60, "put " + acPlayer + " down");
        GiveNickname(record, "downed");
    }

    Notices.push_back({acPlayer, Wire::kRises, Text::BornNotice(record, aOrigin)});
    Event& event = Chronicle(EventKind::Deed, record.FullName() + " swore revenge", Text::OriginLine(record, aOrigin, place), place, aNow, acPlayer);
    event.Men.push_back(record.Id);
    if (!gang.empty())
        event.Gangs.push_back(gang);
    if (aOrigin != Origin::Bounty)
        ++m_state.Players[acPlayer].NemesesMade;
    // The hits he took as a stranger are his wound log's first lines
    if (const auto stranger = m_strangers.find(acDeed.ActorServerId); stranger != m_strangers.end())
    {
        record.Wounds = std::move(stranger->second.Wounds);
        for (const Wound& wound : record.Wounds)
            m_state.Players[wound.ByPlayer].Hits += wound.Hits;
    }
    m_strangers.erase(acDeed.ActorServerId);
    Identify(record);
    return &record;
}

void Engine::OnWound(const std::string& acPlayer, const WoundInput& acWound, int64_t aNow)
{
    PlayerState& player = m_state.Players[acPlayer];
    player.Name = acPlayer;

    Wound wound;
    wound.At = aNow;
    wound.Where = acWound.Where < Part::Count ? acWound.Where : Part::Unknown;
    wound.Cause = acWound.Cause;
    wound.Damage = std::max(0, acWound.Damage);
    wound.Hits = std::max(1, acWound.Hits);
    wound.Range = std::max(0, acWound.Range);
    wound.Weapon = acWound.Weapon;
    wound.Ammo = acWound.Ammo;
    wound.Place = PlaceOr(acWound.Place, acWound.World, acWound.X, acWound.Y);
    wound.ByPlayer = acPlayer;
    wound.Crippled = acWound.Crippled;

    std::vector<Wound>* pWounds = nullptr;
    const uint32_t id = acWound.RecordId ? acWound.RecordId : RecordOfActor(acWound.ActorServerId);
    if (const auto it = id ? m_state.Records.find(id) : m_state.Records.end(); it != m_state.Records.end())
    {
        if (!it->second.IsAlive())
            return;
        pWounds = &it->second.Wounds;
        player.Hits += wound.Hits;
        // Face to face: his name is over him in their game
        it->second.Known[acPlayer] |= kKnowsName;
        Dirty = true;
    }
    else if (acWound.ActorServerId)
    {
        Stranger& stranger = m_strangers[acWound.ActorServerId];
        if (!stranger.ActorServerId)
        {
            stranger.ActorServerId = acWound.ActorServerId;
            stranger.Gang = acWound.Gang;
            stranger.Body = acWound.Body;
            stranger.World = acWound.World;
            stranger.X = acWound.X;
            stranger.Y = acWound.Y;
            stranger.Place = wound.Place;
            stranger.SeenAt = aNow;
        }
        pWounds = &stranger.Wounds;
    }
    if (!pWounds)
        return;

    // The same spot with the same weapon again within a game minute: one line, more hits
    if (!pWounds->empty())
    {
        Wound& last = pWounds->back();
        if (last.ByPlayer == acPlayer && last.Where == wound.Where && last.Weapon == wound.Weapon && aNow - last.At <= 1)
        {
            last.Damage += wound.Damage;
            last.Hits += wound.Hits;
            last.Range = wound.Range;
            last.Crippled = last.Crippled || wound.Crippled;
            last.At = aNow;
            return;
        }
    }
    pWounds->push_back(wound);
    if (pWounds->size() > kMaxWounds)
        pWounds->erase(pWounds->begin());
}

void Engine::OnDeed(const std::string& acPlayer, const DeedInput& acDeed, int64_t aNow)
{
    PlayerState& tallies = m_state.Players[acPlayer];
    tallies.Name = acPlayer;
    switch (acDeed.Deed)
    {
    case Wire::kDownedYou: ++tallies.Downed; break;
    case Wire::kRobbed: ++tallies.Robbed; break;
    case Wire::kLetGo: ++tallies.Spared; break;
    case Wire::kExecuted: ++tallies.Executed; break;
    case Wire::kThreatened: ++tallies.Threats; break;
    case Wire::kPaid: ++tallies.Bribes; break;
    case Wire::kStashRobbed: ++tallies.Stashes; break;
    case Wire::kCampCleared: ++tallies.Camps; break;
    default: break;
    }
    const std::string place = PlaceOr(acDeed.Place, acDeed.World, acDeed.X, acDeed.Y);
    Record* pRecord = RecordFor(acDeed);

    // Gang-wide deeds
    if (acDeed.Deed == Wire::kStashRobbed || acDeed.Deed == Wire::kCampCleared)
    {
        const std::string gang = acDeed.Gang;
        auto it = m_state.Gangs.find(gang);
        if (it == m_state.Gangs.end())
            return;
        if (acDeed.Deed == Wire::kStashRobbed)
        {
            it->second.Purse = it->second.Purse * 4 / 5;
            it->second.Supplies = Clamp(it->second.Supplies - 15);
            AddNotoriety(gang, acPlayer, 3, "robbed their stash", aNow);
            Chronicle(EventKind::Deed, acPlayer + " robbed " + FindGang(gang)->Name, "Their stash at " + place + " is lighter now.", place, aNow, acPlayer).Gangs.push_back(gang);
        }
        else
        {
            AddNotoriety(gang, acPlayer, 6, "cleared their camp", aNow);
            for (CampHold& camp : m_state.Camps)
            {
                if (camp.Gang == gang && camp.Location == acDeed.Place)
                {
                    camp.ClearedAt = aNow;
                    camp.RetakeAt = aNow + m_rng.Range(1, 3) * kDay;
                }
            }
            if (const District* pDistrict = DistrictAt(acDeed.World, acDeed.X, acDeed.Y))
            {
                auto ground = it->second.Ground.find(pDistrict->Key);
                if (ground != it->second.Ground.end())
                    ground->second = std::max(0.f, ground->second - 0.1f);
            }
            Chronicle(EventKind::Deed, acPlayer + " cleared out " + FindGang(gang)->Name, "Their hold at " + place + " is empty, for now.", place, aNow, acPlayer).Gangs.push_back(gang);
        }
        Dirty = true;
        return;
    }

    // A KARMA bounty hunter or a nemesis seen for the first time
    if (acDeed.Deed == Wire::kMet)
    {
        if (pRecord)
        {
            pRecord->Known[acPlayer] |= kKnowsName;
            if (Grudge* pGrudge = pRecord->GrudgeOf(acPlayer))
                ++pGrudge->TimesMet;
            Dirty = true;
            return;
        }
        const GangDef* pGang = FindGang(acDeed.Gang);
        if (pGang && (pGang->Flags & kHunters) && m_state.Gangs.count(acDeed.Gang))
        {
            Record* pHunter = StrangerToRecord(acPlayer, acDeed, Origin::Bounty, aNow);
            pHunter->Rank = 3;
            History(*pHunter, Tone::Normal, "Took the contract on " + acPlayer + ".");
        }
        return;
    }

    // Hits towards a stranger's score: kept while his character exists
    if (!pRecord && acDeed.Deed == Wire::kHit)
    {
        if (!acDeed.ActorServerId)
            return;
        Stranger& stranger = m_strangers[acDeed.ActorServerId];
        stranger.ActorServerId = acDeed.ActorServerId;
        stranger.Gang = acDeed.Gang;
        stranger.Body = acDeed.Body;
        stranger.World = acDeed.World;
        stranger.X = acDeed.X;
        stranger.Y = acDeed.Y;
        stranger.Place = place;
        stranger.SeenAt = aNow;
        stranger.Score[acPlayer] += std::max(1, acDeed.Amount / 10);
        return;
    }

    // Deeds on strangers: some become nemeses
    if (!pRecord)
    {
        Origin origin = Origin::Gunfight;
        int chance = 0;
        switch (acDeed.Deed)
        {
        case Wire::kDownedYou: origin = Origin::DownedYou, chance = 100; break;
        case Wire::kCrippled: origin = Origin::Crippled, chance = 70; break;
        case Wire::kWounded: origin = Origin::Wounded, chance = 50; break;
        case Wire::kComradeKilled: origin = Origin::ComradeKilled, chance = 35; break;
        case Wire::kRobbed: origin = Origin::Robbed, chance = 60; break;
        case Wire::kGunfight: origin = Origin::Gunfight, chance = 18; break;
        case Wire::kSurrendered: origin = Origin::Humbled, chance = 100; break; // questioning needs him on record
        default: return;
        }
        // The more nemeses a player has, the rarer new ones (put-downs always count)
        const int living = LivingGrudges(acPlayer);
        if (origin != Origin::DownedYou && origin != Origin::Humbled)
        {
            if (living >= m_settings.MaxGrudgesPerPlayer)
                chance /= 4;
            else if (living >= m_settings.MaxGrudgesPerPlayer / 2)
                chance = chance * 3 / 5;
            // A stranger the player kept shooting at remembers more
            const auto stranger = m_strangers.find(acDeed.ActorServerId);
            if (stranger != m_strangers.end())
                chance += std::min(20, stranger->second.Score[acPlayer] / 2);
        }
        if (!m_rng.Chance(chance))
            return;
        pRecord = StrangerToRecord(acPlayer, acDeed, origin, aNow);
        if (acDeed.Deed == Wire::kCrippled)
            pRecord->Injuries |= acDeed.Amount >= 2 ? Wire::kBadArm : Wire::kLimp;
        if (acDeed.Deed == Wire::kSurrendered)
        {
            ++pRecord->TimesSurrendered;
            if (Grudge* pGrudge = pRecord->GrudgeOf(acPlayer))
                AddFear(*pRecord, *pGrudge, 20);
        }
        return;
    }

    Record& record = *pRecord;
    if (!record.IsAlive())
        return;

    const auto grudgeFor = [&](Origin aOrigin) -> Grudge& { return EnsureGrudge(record, acPlayer, aOrigin, place, aNow); };

    switch (acDeed.Deed)
    {
    case Wire::kDownedYou: {
        const bool fresh = !record.GrudgeOf(acPlayer);
        Grudge& grudge = grudgeFor(Origin::DownedYou);
        if (!fresh)
        {
            if (grudge.From == Origin::GangOrders || grudge.From == Origin::Gunfight)
                grudge.From = Origin::DownedYou;
            AddHate(record, grudge, 10, Origin::DownedYou);
        }
        ++grudge.TimesDownedThem;
        grudge.LastDealt = aNow;
        Remember(record, Emotion::Pride, 65, "put " + acPlayer + " down");
        if (record.Gang.empty())
        {
            if (record.Rank < 5)
                RankUp(record, "put " + acPlayer + " down");
        }
        else
            GainXp(record, 3, "put " + acPlayer + " down");
        if (record.Nickname.empty() || grudge.TimesDownedThem == 3)
            GiveNickname(record, "downed");
        History(record, Tone::Blood, "Put " + acPlayer + " down" + Text::At(place) + ".");
        Notices.push_back({acPlayer, Wire::kRises, fresh ? Text::BornNotice(record, Origin::DownedYou) : record.FullName() + " got you again. " + Text::GloatLine(m_rng, record)});
        Event& event = Chronicle(EventKind::Deed, record.FullName() + " put " + acPlayer + " down", place.empty() ? std::string() : "At " + place + ".", place, aNow, acPlayer);
        event.Men.push_back(record.Id);
        break;
    }
    case Wire::kWounded:
    case Wire::kCrippled: {
        const Origin origin = acDeed.Deed == Wire::kCrippled ? Origin::Crippled : Origin::Wounded;
        Grudge& grudge = grudgeFor(origin);
        AddHate(record, grudge, acDeed.Deed == Wire::kCrippled ? 20 : 12, origin);
        AddFear(record, grudge, acDeed.Deed == Wire::kCrippled ? 15 : 8);
        if (acDeed.Deed == Wire::kCrippled)
            record.Injuries |= acDeed.Amount >= 2 ? Wire::kBadArm : Wire::kLimp;
        GainXp(record, 2, "lived through a fight with " + acPlayer);
        History(record, Tone::Blood, (acDeed.Deed == Wire::kCrippled ? "Crippled by " : "Wounded by ") + acPlayer + Text::At(place) + ", and lived.");
        Remember(record, Emotion::Anger, 55, "hurt by " + acPlayer);
        grudge.LastDealt = aNow;
        break;
    }
    case Wire::kComradeKilled: {
        Grudge& grudge = grudgeFor(Origin::ComradeKilled);
        AddHate(record, grudge, 12, Origin::ComradeKilled);
        AddFear(record, grudge, 6);
        break;
    }
    case Wire::kRobbed: {
        Grudge& grudge = grudgeFor(Origin::Robbed);
        AddHate(record, grudge, record.Temper == Personality::Greedy ? 28 : 18, Origin::Robbed);
        ++grudge.TimesRobbedBy;
        History(record, Tone::Normal, "Robbed by " + acPlayer + Text::At(place) + ".");
        break;
    }
    case Wire::kGunfight: {
        Grudge& grudge = grudgeFor(Origin::Gunfight);
        AddHate(record, grudge, 6, Origin::Gunfight);
        ++grudge.TimesMet;
        GainXp(record, 1, "lived through a fight with " + acPlayer);
        break;
    }
    case Wire::kSurrendered: {
        Grudge& grudge = grudgeFor(Origin::Humbled);
        ++record.TimesSurrendered;
        AddFear(record, grudge, 20);
        Remember(record, Emotion::Shame, 50, "begged " + acPlayer + " for " + Text::His(record) + " life");
        History(record, Tone::Normal, "Surrendered to " + acPlayer + Text::At(place) + ".");
        break;
    }
    case Wire::kLetGo: {
        Grudge& grudge = grudgeFor(Origin::Humbled);
        // Sparing a man who hates you counts for more than letting a stranger go
        const bool spared = grudge.Hate >= 35 && grudge.From != Origin::Humbled;
        // Amount 1: spared, he turned on the player anyway (OnQuestion's roll); no gratitude in that
        const bool turned = acDeed.Amount == 1;
        grudge.Mercy = true;
        ++grudge.TimesSparedBy;
        if (turned)
        {
            AddHate(record, grudge, 10, Origin::Humbled);
            Remember(record, Emotion::Shame, 50, "turned on " + acPlayer + " after " + Text::He(record) + " was spared");
        }
        else
        {
            int cut = 20;
            if (record.Temper == Personality::Proud)
                cut = 5, Remember(record, Emotion::Shame, 60, "owes " + acPlayer + " " + Text::His(record) + " life");
            else if (record.Temper == Personality::Loyal || record.Temper == Personality::Drifter || record.Temper == Personality::Coward)
                cut = 30;
            AddHate(record, grudge, -cut, Origin::Humbled);
            AddFear(record, grudge, 5);
            if (record.Temper != Personality::Proud && grudge.Hate < 25)
            {
                Remember(record, Emotion::Gratitude, 60, acPlayer + " let " + Text::Him(record) + " live");
                const auto it = m_state.Players.find(acPlayer);
                const int alignment = it == m_state.Players.end() ? 2 : it->second.Alignment;
                if (m_rng.Chance(record.Honesty / 2 + (alignment >= 3 ? 20 : 0)))
                    grudge.Informant = true;
                else
                    grudge.OwesThem = true;
            }
        }
        History(record, turned ? Tone::Blood : Tone::Good,
                acPlayer + " let " + Text::Him(record) + " go" + Text::At(place) + (turned ? ", and " + std::string(Text::He(record)) + " turned on " + acPlayer + "." : "."));
        Karma.push_back({acPlayer, spared ? 40 : 25, false});
        // The game running him: hands down, then he runs, or fights the player who spared him
        Live.push_back({LiveChange::kOrder, record.LiveActor, record.Id, turned ? Wire::kLetGoFights : Wire::kLetGoRuns, turned ? acPlayer : std::string()});
        break;
    }
    case Wire::kThreatened: {
        Grudge& grudge = grudgeFor(Origin::Humbled);
        AddFear(record, grudge, 20);
        AddHate(record, grudge, 5, Origin::Humbled);
        Karma.push_back({acPlayer, -10, true});
        break;
    }
    case Wire::kPaid: {
        Grudge& grudge = grudgeFor(Origin::Humbled);
        AddHate(record, grudge, record.Temper == Personality::Greedy ? -20 : -10, Origin::Humbled);
        if (record.Temper == Personality::Greedy)
            grudge.OwesThem = true;
        break;
    }
    case Wire::kExecuted:
        // Its death comes as kDeath with kWhileSurrendered
        Karma.push_back({acPlayer, -60, false});
        break;
    case Wire::kHit:
        if (Grudge* pGrudge = record.GrudgeOf(acPlayer))
            pGrudge->LastDealt = aNow;
        break;
    default:
        break;
    }
    Dirty = true;
}

// --- deaths

void Engine::OnDeath(const std::string& acPlayer, const DeathInput& acDeath, int64_t aNow)
{
    const bool byPlayer = (acDeath.Flags & Wire::kByLocalPlayer) != 0;
    uint32_t id = acDeath.RecordId ? acDeath.RecordId : RecordOfActor(acDeath.ActorServerId);
    auto it = id ? m_state.Records.find(id) : m_state.Records.end();
    if (byPlayer && (it == m_state.Records.end() || it->second.IsAlive()))
    {
        PlayerState& player = m_state.Players[acPlayer];
        player.Name = acPlayer;
        ++player.GangKills;
    }

    if (it == m_state.Records.end())
    {
        // An unnamed gang man: the gang loses him; the killer's name gets around
        m_strangers.erase(acDeath.ActorServerId);
        std::string gang = acDeath.Gang;
        if (gang == "raiders")
        {
            const District* pDistrict = DistrictAt(acDeath.World, acDeath.X, acDeath.Y);
            float best = -1.f;
            for (const auto& [gangId, state] : m_state.Gangs)
            {
                const GangDef* pGang = FindGang(gangId);
                if (pGang && std::string(pGang->LookPool) == "raider" && (pGang->Flags & kRaiders))
                {
                    const float share = pDistrict ? GroundOf(gangId, pDistrict->Key) : 0.f;
                    if (share > best)
                        best = share, gang = gangId;
                }
            }
        }
        if (!m_state.Gangs.count(gang))
            return;
        GangLosesMan(gang, 1);
        if (byPlayer)
        {
            // A point of notoriety for every three of their men
            const std::string key = gang + "|" + acPlayer;
            if (++m_unnamedKills[key] >= 3)
            {
                m_unnamedKills[key] = 0;
                AddNotoriety(gang, acPlayer, 1, "killed their men", aNow);
            }
        }
        return;
    }

    Record& record = it->second;
    if (!record.IsAlive())
        return;
    const std::string killer = byPlayer ? acPlayer : std::string();
    KillRecord(record, killer, acDeath, aNow);
}

void Engine::KillRecord(Record& aRecord, const std::string& acKiller, const DeathInput& acDeath, int64_t aNow)
{
    const std::string place = PlaceOr(acDeath.Place, acDeath.World, acDeath.X, acDeath.Y);
    const bool executed = (acDeath.Flags & Wire::kWhileSurrendered) != 0;
    const bool final = executed || (acDeath.Flags & Wire::kFinal) || aRecord.Protected;
    const std::string by = acKiller.empty() ? std::string("someone") : acKiller;
    aRecord.LiveActor = 0;

    if (!acKiller.empty())
        aRecord.Known[acKiller] |= kKnowsName;
    // The wound log: the killer's last hit on him, if it was just now, is the one that put him down
    for (auto wound = aRecord.Wounds.rbegin(); !acKiller.empty() && wound != aRecord.Wounds.rend(); ++wound)
    {
        if (wound->ByPlayer != acKiller)
            continue;
        if (aNow - wound->At <= 3)
            wound->Lethal = true;
        break;
    }

    if (!acKiller.empty())
    {
        // Killing a man you once let go takes the mercy back
        if (Grudge* pGrudge = aRecord.GrudgeOf(acKiller))
        {
            if (pGrudge->Mercy && !executed)
                Karma.push_back({acKiller, -25, true});
        }
        if (!aRecord.Gang.empty())
            AddNotoriety(aRecord.Gang, acKiller, std::array<int, 6>{1, 1, 1, 2, 2, 4}[static_cast<size_t>(Clamp(aRecord.Rank, 0, 5))] + (aRecord.Rank >= 5 ? 2 : 0),
                         "killed " + aRecord.Name, aNow);
    }

    if (!final && m_rng.Chance(CheatDeathChance(aRecord)))
    {
        // He lived: presumed dead, back in a few days with a scar to show
        const std::string wound = Text::WoundFor(m_rng, acDeath.Cause, acDeath.Flags);
        Scar scar;
        scar.Wound = wound;
        scar.Text = Text::ScarText(m_rng, wound);
        scar.Place = place;
        scar.ByPlayer = acKiller;
        scar.At = aNow;
        aRecord.Scars.push_back(scar);
        if (wound == "leg")
            aRecord.Injuries |= Wire::kLimp;
        else if (wound == "arm")
            aRecord.Injuries |= Wire::kBadArm;
        else if (wound == "burn")
            aRecord.Injuries |= Wire::kBurned;
        else if (wound == "eye")
            aRecord.Injuries |= Wire::kOneEye;
        else if (wound == "ears")
            aRecord.Injuries |= Wire::kRinging;

        aRecord.State = Status::PresumedDead;
        aRecord.BackAt = aNow + m_rng.Range(2, 5) * kDay;
        ++aRecord.TimesCheatedDeath;
        History(aRecord, Tone::Blood, "Left for dead by " + by + Text::At(place) + ". " + Text::HeWord(aRecord, true) + " lived: " + scar.Text + ".");
        if (!acKiller.empty())
        {
            Grudge& grudge = EnsureGrudge(aRecord, acKiller, Origin::LeftForDead, place, aNow);
            if (grudge.From == Origin::Gunfight || grudge.From == Origin::GangOrders || grudge.From == Origin::Humbled)
                grudge.From = Origin::LeftForDead;
            AddHate(aRecord, grudge, 25, Origin::LeftForDead);
            AddFear(aRecord, grudge, 20);
            grudge.LastDealt = aNow;
            Remember(aRecord, aRecord.Nerve < 50 ? Emotion::Fear : Emotion::Anger, 80, acKiller + " nearly killed " + Text::Him(aRecord));
            GainXp(aRecord, 3, "cheated death");
        }
        if (aRecord.TimesCheatedDeath >= 2 || m_rng.Chance(40))
            GiveNickname(aRecord, aRecord.TimesCheatedDeath >= 2 ? "cheated" : "scar:" + wound);
        // The player saw him fall
        if (!acKiller.empty())
            Notices.push_back({acKiller, Wire::kDeath, aRecord.FullName() + " is dead."});
        Dirty = true;
        return;
    }

    aRecord.State = Status::Dead;
    aRecord.Fate = (executed ? "Executed by " : "Killed by ") + by + Text::At(place);
    aRecord.KilledBy = acKiller;
    History(aRecord, Tone::Blood, aRecord.Fate + ".");
    if (!acKiller.empty())
    {
        ++m_state.Players[acKiller].Kills;
        if (const auto gang = m_state.Gangs.find(aRecord.Gang); gang != m_state.Gangs.end() && gang->second.LeaderId == aRecord.Id)
            ++m_state.Players[acKiller].Leaders;
        Notices.push_back({acKiller, Wire::kDeath, aRecord.FullName() + " is dead."});
        // Everyone else he hated hears it
        for (const Grudge& grudge : aRecord.Grudges)
            if (grudge.Player != acKiller)
                Notices.push_back({grudge.Player, Wire::kDeath, aRecord.FullName() + " is dead. " + acKiller + " got to " + Text::Him(aRecord) + " first."});
    }
    Event& event = Chronicle(EventKind::Deed, aRecord.FullName() + " is dead", aRecord.Fate + ".", place, aNow, acKiller);
    event.Men.push_back(aRecord.Id);
    if (!aRecord.Gang.empty())
        event.Gangs.push_back(aRecord.Gang);

    KinGrieve(aRecord, acKiller, aNow);
    if (!aRecord.Gang.empty())
    {
        GangLosesMan(aRecord.Gang, 1);
        auto gang = m_state.Gangs.find(aRecord.Gang);
        if (gang != m_state.Gangs.end())
        {
            if (gang->second.LeaderId == aRecord.Id)
                SeatFallen(gang->second, aRecord, aNow);
            // Officers are buried by their own
            const GangDef* pDef = FindGang(aRecord.Gang);
            if (aRecord.Rank >= 3 && !Cemeteries().empty() && pDef && !(pDef->Flags & (kHunters | kLoners)))
            {
                const District* pDistrict = DistrictAt(aRecord.World, aRecord.X, aRecord.Y);
                const Cemetery* pCemetery = &Cemeteries()[0];
                for (const Cemetery& cemetery : Cemeteries())
                    if (pDistrict && cemetery.District == std::string(pDistrict->Key))
                        pCemetery = &cemetery;
                Funeral funeral;
                funeral.DeadId = aRecord.Id;
                funeral.Gang = aRecord.Gang;
                funeral.Place = pCemetery->Name;
                funeral.Location = pCemetery->Location;
                funeral.Start = (aNow / kDay + 1) * kDay + 10 * kHour;
                funeral.End = funeral.Start + 5 * kHour;
                m_state.Funerals.push_back(funeral);
                Chronicle(EventKind::Funeral, "Funeral for " + aRecord.FullName(),
                          std::string(FindGang(aRecord.Gang)->Name) + " bury their own at " + funeral.Place + " tomorrow from 10:00 to 15:00.", funeral.Place, aNow)
                    .Gangs.push_back(aRecord.Gang);
            }
        }
        for (CampHold& camp : m_state.Camps)
            if (camp.OfficerId == aRecord.Id)
                camp.OfficerId = 0;
    }
    Dirty = true;
}

void Engine::KinGrieve(const Record& acDead, const std::string& acKiller, int64_t aNow)
{
    for (auto& [id, other] : m_state.Records)
    {
        if (!other.IsAlive() || id == acDead.Id)
            continue;
        std::string relation;
        if (other.KinId == acDead.Id || acDead.KinId == id)
            relation = other.KinId == acDead.Id ? other.KinRelation : "kin";
        else if (std::find(other.Friends.begin(), other.Friends.end(), acDead.Id) != other.Friends.end())
            relation = "friend";
        if (relation.empty())
            continue;

        Remember(other, Emotion::Grief, relation == "friend" ? 60 : 85, acDead.Name + " died");
        History(other, Tone::Blood, "Lost " + std::string(Text::His(other)) + " " + relation + ", " + acDead.Name + (acKiller.empty() ? "." : ", to " + acKiller + "."));
        if (acKiller.empty())
            continue;

        int hate = relation == "friend" ? 18 : 30;
        int fear = relation == "friend" ? 4 : 6;
        if (other.Temper == Personality::Loyal)
            hate = hate * 7 / 5;
        else if (other.Temper == Personality::Coward)
            hate = hate * 2 / 5, fear *= 3;
        else if (other.Temper == Personality::Cold || other.Temper == Personality::Drifter)
            hate = hate * 3 / 5;

        Grudge& grudge = EnsureGrudge(other, acKiller, Origin::ComradeKilled, "", aNow);
        AddHate(other, grudge, hate, Origin::ComradeKilled);
        AddFear(other, grudge, fear);
        if (other.Temper == Personality::Loyal || other.Aggression >= 60)
            other.Goals.push_back({GoalType::Avenge, acDead.Id, acKiller, "avenge " + std::string(Text::His(other)) + " " + relation + " " + acDead.Name, aNow});
        Notices.push_back({acKiller, fear > hate / 2 ? Wire::kNormal : Wire::kRises,
                           other.FullName() + " heard you killed " + Text::His(other) + " " + relation + ". " + Text::HeWord(other, true) +
                               (fear > hate / 2 ? "'s afraid " + std::string(Text::He(other)) + "'s next." : "'s coming for blood.")});
    }
}

void Engine::AddNotoriety(const std::string& acGang, const std::string& acPlayer, int aAmount, const std::string& acWhy, int64_t aNow)
{
    auto it = m_state.Gangs.find(acGang);
    if (it == m_state.Gangs.end() || aAmount <= 0)
        return;
    int& value = it->second.Notoriety[acPlayer];
    const int before = value;
    value = Clamp(value + aAmount);
    it->second.NotorietyAt[acPlayer] = aNow;
    const int level = Text::NotorietyLevel(value);
    if (level > Text::NotorietyLevel(before))
    {
        const GangDef* pGang = FindGang(acGang);
        Notices.push_back({acPlayer, Wire::kRises,
                           std::string(pGang ? pGang->Short : acGang.c_str()) + ": you're " + Text::NotorietyName(level) + " now (" + acWhy + "). More of them will come."});
    }
    Dirty = true;
}

void Engine::GangLosesMan(const std::string& acGang, int aCount)
{
    auto it = m_state.Gangs.find(acGang);
    if (it != m_state.Gangs.end())
        it->second.Men = std::max(0, it->second.Men - aCount);
}

void Engine::SeatFallen(GangState& aGang, const Record& acDead, int64_t aNow)
{
    aGang.FallenLeader = acDead.FullName();
    aGang.LeaderId = 0;
    // The likeliest successor: the highest rank, then XP, ambition and loyalty
    Record* pBest = nullptr;
    int bestScore = -1;
    for (Record* pMan : NamedMen(aGang.Id))
    {
        const int score = pMan->Rank * 1000 + pMan->RankXp * 10 + pMan->Ambition + pMan->Loyalty / 2;
        if (score > bestScore)
            bestScore = score, pBest = pMan;
    }
    aGang.SuccessorId = pBest ? pBest->Id : 0;
    aGang.SeatFillAt = aNow + m_rng.Range(1, 3) * kDay;
    const GangDef* pGang = FindGang(aGang.Id);
    Chronicle(EventKind::Succession, std::string(pGang->Short) + " lose their " + pGang->LeaderTitle,
              acDead.FullName() + " is gone. The seat stands empty, and the " + pGang->Short + " are weaker for it.", "", aNow)
        .Gangs.push_back(aGang.Id);
    WorldNotice(acDead.FullName() + ", " + pGang->LeaderTitle + " of " + pGang->Name + ", is dead.");
}

// --- encounters

bool Engine::InRange(const Record& acRecord, const PlayerSpot& acSpot, const std::string& acDistrict) const noexcept
{
    const GangDef* pGang = FindGang(acRecord.Gang);
    if (pGang && (pGang->Flags & kHunters))
        return true; // hunters follow the mark
    if (acRecord.World != acSpot.World)
        return false;
    const float dx = acRecord.X - acSpot.X;
    const float dy = acRecord.Y - acSpot.Y;
    if (dx * dx + dy * dy < 30000.f * 30000.f)
        return true;
    return !acRecord.Gang.empty() && GroundOf(acRecord.Gang, acDistrict) >= 0.1f;
}

float Engine::EncounterWeight(const Record& acRecord, const Grudge* apGrudge, const std::string& acPlayer, const std::string& acDistrict, int aHour) const noexcept
{
    const GangDef* pGang = FindGang(acRecord.Gang);
    float weight = 1.f;
    if (apGrudge)
    {
        switch (Text::TierOf(apGrudge->Hate))
        {
        case HateTier::Murderous: weight = 1.6f; break;
        case HateTier::Vengeful: weight = 1.f; break;
        default: weight = 0.5f; break;
        }
        weight *= 1.f - std::min(0.8f, EffectiveFear(acRecord, *apGrudge) / 120.f);
    }
    switch (acRecord.Temper)
    {
    case Personality::Hothead: weight *= 1.3f; break;
    case Personality::Proud: weight *= 1.2f; break;
    case Personality::Coward: weight *= 0.5f; break;
    case Personality::Greedy: weight *= 0.9f; break;
    case Personality::Drifter: weight *= 0.8f; break;
    default: break;
    }
    static const float s_rank[6] = {1.f, 1.f, 0.8f, 0.6f, 0.4f, 0.25f};
    weight *= s_rank[Clamp(acRecord.Rank, 0, 5)];
    if (pGang)
    {
        static const float s_notoriety[5] = {0.6f, 1.f, 1.5f, 2.2f, 3.f};
        weight *= pGang->AmbushWeight * s_notoriety[Text::NotorietyLevel(NotorietyOf(acRecord.Gang, acPlayer))];
        weight *= (aHour >= 21 || aHour < 5) ? pGang->NightWeight : 1.f;
        const auto state = m_state.Gangs.find(acRecord.Gang);
        if (state != m_state.Gangs.end() && state->second.LeaderId == 0 && state->second.SeatFillAt)
            weight *= 0.6f;
        // On their own ground
        weight *= 0.7f + GroundOf(acRecord.Gang, acDistrict);

        const auto player = m_state.Players.find(acPlayer);
        const int alignment = player == m_state.Players.end() ? 2 : player->second.Alignment;
        if (pGang->Flags & kRaiders)
            weight *= alignment >= 3 ? 1.3f : 1.f;
        if (pGang->Flags & kLawful)
            weight *= alignment <= 1 ? 1.5f : 0.3f;
    }
    return std::max(0.f, weight);
}

SpawnPlan::Man Engine::ManOf(const Record& acRecord) const
{
    SpawnPlan::Man man;
    man.RecordId = acRecord.Id;
    man.Body = acRecord.Body;
    man.Appearance = acRecord.Appearance;
    man.AppearanceFlags = acRecord.AppearanceFlags;
    man.Name = acRecord.FullName();
    man.Rank = acRecord.Rank;
    man.Injuries = acRecord.Injuries;
    man.Alignment = acRecord.Alignment;
    man.Legendary = acRecord.Rank >= 4;
    return man;
}

SpawnPlan Engine::PlanFor(Record& aRecord, const std::string& acPlayer, uint8_t aKind, int64_t aNow)
{
    SpawnPlan plan;
    plan.Player = acPlayer;
    plan.EncounterId = m_nextEncounterId++;
    plan.Kind = aKind;
    plan.Gang = aRecord.Gang;
    const auto player = m_state.Players.find(acPlayer);
    const int alignment = player == m_state.Players.end() ? 2 : player->second.Alignment;
    const Grudge* pGrudge = aRecord.GrudgeOf(acPlayer);

    Encounter encounter;
    encounter.Id = plan.EncounterId;
    encounter.Player = acPlayer;
    encounter.RecordId = aRecord.Id;
    encounter.Kind = aKind;
    encounter.StartedAt = aNow;

    if (aKind == Wire::kHunters)
    {
        // Hired guns: the gang fitting whoever's paying, one named hunter leading them
        const GangDef* pPayer = FindGang(aRecord.Gang);
        std::string hunters = pPayer && (pPayer->Flags & kLawful) ? "marshals" : "blackledger";
        if (!m_state.Gangs.count(hunters))
            hunters = m_state.Gangs.count("blackledger") ? "blackledger" : "";
        plan.Gang = hunters.empty() ? aRecord.Gang : hunters;
        Record* pLeader = nullptr;
        if (!hunters.empty())
        {
            pLeader = &NewRecord(hunters, RollBody(FindGang(hunters)), 3, aNow);
            EnsureGrudge(*pLeader, acPlayer, Origin::Bounty, "", aNow);
            History(*pLeader, Tone::Normal, "Hired by " + aRecord.FullName() + " to kill " + acPlayer + ".");
            pLeader->RestUntil = 0;
        }
        Record& leader = pLeader ? *pLeader : aRecord;
        plan.Men.push_back(ManOf(leader));
        encounter.Men.push_back(leader.Id);
        for (int i = m_rng.Range(1, 3); i > 0; --i)
        {
            SpawnPlan::Man man;
            man.Rank = 1;
            man.Alignment = leader.Alignment;
            plan.Men.push_back(man);
            encounter.Men.push_back(0);
        }
        plan.Taunt = Text::Taunt(m_rng, leader, leader.GrudgeOf(acPlayer), aKind, alignment) + " " + aRecord.FullName() + " sends " + Text::His(aRecord) + " regards.";
        plan.Distance = static_cast<float>(m_rng.Range(4500, 7000));
        History(aRecord, Tone::Normal, "Paid hunters to go after " + acPlayer + ".");
        if (const GangDef* pGang = FindGang(aRecord.Gang); pGang && m_state.Gangs.count(aRecord.Gang))
            m_state.Gangs[aRecord.Gang].Purse = std::max(0, m_state.Gangs[aRecord.Gang].Purse - 150);
    }
    else
    {
        plan.Men.push_back(ManOf(aRecord));
        encounter.Men.push_back(aRecord.Id);
        // His crew: named underlings first, then unnamed men
        int crew = aKind == Wire::kCrew || aKind == Wire::kCamp || aKind == Wire::kPatrol ? std::min(4, std::max(1, aRecord.Rank - 1)) : 0;
        if (aKind == Wire::kSighting)
            crew = m_rng.Range(0, 2);
        for (auto& [id, other] : m_state.Records)
        {
            if (crew <= 0)
                break;
            if (other.BossId == aRecord.Id && other.IsAlive() && !other.LiveActor && !other.Protected && m_rng.Chance(50))
            {
                plan.Men.push_back(ManOf(other));
                encounter.Men.push_back(id);
                --crew;
            }
        }
        for (; crew > 0; --crew)
        {
            SpawnPlan::Man man;
            man.Rank = 1;
            man.Alignment = aRecord.Alignment;
            plan.Men.push_back(man);
            encounter.Men.push_back(0);
        }
        plan.Taunt = Text::Taunt(m_rng, aRecord, pGrudge, aKind, alignment);
        switch (aKind)
        {
        case Wire::kCallOut: plan.Distance = static_cast<float>(m_rng.Range(3000, 4000)); break;
        case Wire::kSighting: plan.Distance = static_cast<float>(m_rng.Range(7500, 9500)); break;
        default: plan.Distance = static_cast<float>(m_rng.Range(4500, 7000)); break;
        }
    }

    aRecord.LastCameAt = aNow;
    aRecord.RestUntil = aNow + 12 * kHour;
    m_state.Players[acPlayer].ActiveEncounter = plan.EncounterId;
    m_state.Players[acPlayer].LastEncounterAt = aNow;
    m_encounters[plan.EncounterId] = encounter;
    Dirty = true;
    return plan;
}

std::optional<SpawnPlan> Engine::RollEncounter(const PlayerSpot& acSpot, int64_t aNow)
{
    if (!m_settings.Enabled || !IsStarted())
        return std::nullopt;
    PlayerState& player = m_state.Players[acSpot.Player];
    player.Name = acSpot.Player;
    if (player.ActiveEncounter || aNow < player.NextRollAt)
        return std::nullopt;

    // The next roll: sooner the more a gang wants this player
    int topLevel = 0;
    for (const auto& [id, gang] : m_state.Gangs)
        topLevel = std::max(topLevel, Text::NotorietyLevel(NotorietyOf(id, acSpot.Player)));
    const float hours = std::max(1.5f, m_settings.EncounterHours * (1.f - 0.1f * topLevel));
    player.NextRollAt = aNow + static_cast<int64_t>(hours * kHour * (0.75f + 0.5f * m_rng.Unit()));
    if (!acSpot.CanMeet || !m_rng.Chance(m_settings.EncounterChance))
        return std::nullopt;

    const District* pDistrict = DistrictAt(acSpot.World, acSpot.X, acSpot.Y);
    const std::string district = pDistrict ? pDistrict->Key : std::string();
    const int hour = HourOf(aNow);

    struct Candidate
    {
        Record* pRecord;
        float Weight;
        Intent Wants;
    };
    std::vector<Candidate> candidates;
    for (auto& [id, record] : m_state.Records)
    {
        if (!record.IsAlive() || record.LiveActor || record.Protected || aNow < record.RestUntil)
            continue;
        if (record.LastCameAt && aNow - record.LastCameAt < 12 * kHour)
            continue;
        if (!record.Gang.empty() && !GangActive(record.Gang))
            continue;
        if (!InRange(record, acSpot, district))
            continue;

        const Grudge* pGrudge = record.GrudgeOf(acSpot.Player);
        Intent intent = Intent::WontHunt;
        if (pGrudge)
            intent = IntentOf(record, *pGrudge);
        else if (record.Seeded && !record.Gang.empty())
        {
            // Seeded men come by orders once their gang wants this player (the lawful for the wicked)
            const GangDef* pGang = FindGang(record.Gang);
            const int notoriety = NotorietyOf(record.Gang, acSpot.Player);
            const bool orders = pGang && !(pGang->Flags & kProvokedOnly) && notoriety >= 25;
            const bool duty = pGang && (pGang->Flags & kLawful) && player.Alignment <= 1 && notoriety >= 25;
            if ((orders || duty) && record.Rank <= 3)
                intent = Intent::Hunts;
        }
        if (intent != Intent::Hunts && intent != Intent::Hires)
            continue;
        const float weight = EncounterWeight(record, pGrudge, acSpot.Player, district, hour);
        if (weight > 0.f)
            candidates.push_back({&record, weight, intent});
    }
    if (candidates.empty())
        return std::nullopt;

    float total = 0.f;
    for (const Candidate& candidate : candidates)
        total += candidate.Weight;
    float roll = m_rng.Unit() * total;
    const Candidate* pPick = &candidates.back();
    for (const Candidate& candidate : candidates)
    {
        roll -= candidate.Weight;
        if (roll <= 0.f)
        {
            pPick = &candidate;
            break;
        }
    }

    Record& record = *pPick->pRecord;
    const Grudge* pGrudge = record.GrudgeOf(acSpot.Player);
    uint8_t kind = Wire::kAmbush;
    if (pPick->Wants == Intent::Hires)
        kind = Wire::kHunters;
    else if (record.Rank >= 3)
        kind = Wire::kCrew;
    else if (pGrudge && Text::TierOf(pGrudge->Hate) == HateTier::Murderous && (record.Temper == Personality::Hothead || record.Temper == Personality::Proud) && m_rng.Chance(45))
        kind = Wire::kCallOut;
    else if (pGrudge && EffectiveFear(record, *pGrudge) >= 50 && m_rng.Chance(35))
        kind = Wire::kSighting;
    else if (!pGrudge && m_rng.Chance(40))
        kind = Wire::kPatrol;

    if (pGrudge)
    {
        Grudge* pMutable = record.GrudgeOf(acSpot.Player);
        ++pMutable->TimesMet;
    }
    record.Known[acSpot.Player] |= kKnowsName;
    SpawnPlan plan = PlanFor(record, acSpot.Player, kind, aNow);
    Event& event = Chronicle(EventKind::Encounter, record.FullName() + " came for " + acSpot.Player, "", pDistrict ? pDistrict->Name : "", aNow, acSpot.Player);
    event.Men.push_back(record.Id);
    return plan;
}

std::optional<SpawnPlan> Engine::OnPlace(const std::string& acPlayer, const std::string& acLocation, int64_t aNow)
{
    if (!m_settings.Enabled || !IsStarted() || acLocation.empty())
        return std::nullopt;
    PlayerState& player = m_state.Players[acPlayer];
    if (player.ActiveEncounter)
        return std::nullopt;

    // A funeral going on there: the mourners
    for (const Funeral& funeral : m_state.Funerals)
    {
        if (funeral.Location != acLocation || aNow < funeral.Start || aNow > funeral.End)
            continue;
        Record* pMourner = nullptr;
        for (auto& [id, other] : m_state.Records)
        {
            if (other.IsAlive() && !other.LiveActor && other.Gang == funeral.Gang && !other.Protected &&
                (other.KinId == funeral.DeadId || other.Rank >= 2) && (!pMourner || other.Rank > pMourner->Rank))
                pMourner = &other;
        }
        if (!pMourner)
            continue;
        const Record* pDead = Find(funeral.DeadId);
        SpawnPlan plan = PlanFor(*pMourner, acPlayer, Wire::kCamp, aNow);
        plan.Location = acLocation;
        plan.Taunt = pDead ? "We're burying " + pDead->Name + " today. Show some respect, or join " + Text::Him(*pDead) + "." : "";
        return plan;
    }

    // A camp: its officer and his men, once a visit
    for (CampHold& camp : m_state.Camps)
    {
        if (camp.Location != acLocation || !GangActive(camp.Gang))
            continue;
        if (camp.ClearedAt && aNow < camp.RetakeAt)
            continue;
        auto it = m_state.Records.find(camp.OfficerId);
        if (it == m_state.Records.end() || !it->second.IsAlive() || it->second.LiveActor || it->second.Protected)
            continue;
        if (it->second.LastCameAt && aNow - it->second.LastCameAt < 6 * kHour)
            continue;
        it->second.Known[acPlayer] |= kKnowsName | kKnowsCamp;
        SpawnPlan plan = PlanFor(it->second, acPlayer, Wire::kCamp, aNow);
        plan.Location = acLocation;
        return plan;
    }

    // A man keeping a habit there at this hour
    const int hour = HourOf(aNow);
    const int day = static_cast<int>(aNow / kDay) % 7;
    for (auto& [id, record] : m_state.Records)
    {
        if (!record.IsAlive() || record.LiveActor || record.Protected)
            continue;
        for (const Habit& habit : record.Habits)
        {
            const int end = habit.Hour + habit.Length;
            const bool inHours = (hour >= habit.Hour && hour < end) || (end > 24 && hour < end - 24);
            if (habit.Location == acLocation && inHours && (habit.DaysMask & (1 << day)))
            {
                SpawnPlan plan = PlanFor(record, acPlayer, Wire::kCamp, aNow);
                plan.Location = acLocation;
                return plan;
            }
        }
    }
    return std::nullopt;
}

void Engine::OnSpawned(uint32_t aEncounterId, const std::vector<uint32_t>& acServerIds)
{
    auto it = m_encounters.find(aEncounterId);
    if (it == m_encounters.end())
        return;
    it->second.ServerIds = acServerIds;
    for (size_t i = 0; i < it->second.Men.size() && i < acServerIds.size(); ++i)
    {
        const uint32_t recordId = it->second.Men[i];
        if (!recordId || !acServerIds[i])
            continue;
        auto record = m_state.Records.find(recordId);
        if (record == m_state.Records.end())
            continue;
        record->second.LiveActor = acServerIds[i];
        record->second.LiveEncounter = aEncounterId;
        Identify(record->second);
    }
}

void Engine::OnLook(uint32_t aRecordId, uint32_t aActorServerId, const std::string& acAppearance, uint32_t aFlags)
{
    if (!aRecordId)
        aRecordId = RecordOfActor(aActorServerId);
    auto it = m_state.Records.find(aRecordId);
    if (it == m_state.Records.end() || !it->second.Appearance.empty() || acAppearance.empty() || !aFlags)
        return;
    it->second.Appearance = acAppearance;
    it->second.AppearanceFlags = aFlags;
    Dirty = true;
}

void Engine::OnOutcome(const std::string& acPlayer, uint32_t aEncounterId, uint8_t aOutcome, int64_t aNow)
{
    auto it = m_encounters.find(aEncounterId);
    if (it == m_encounters.end())
        return;
    const Encounter encounter = it->second;
    m_encounters.erase(it);
    auto player = m_state.Players.find(encounter.Player);
    if (player != m_state.Players.end() && player->second.ActiveEncounter == aEncounterId)
        player->second.ActiveEncounter = 0;

    auto record = m_state.Records.find(encounter.Men.empty() ? 0 : encounter.Men[0]);
    if (record == m_state.Records.end())
        return;
    Record& man = record->second;
    Grudge* pGrudge = man.GrudgeOf(acPlayer);

    switch (aOutcome)
    {
    case Wire::kHeFled:
        if (man.IsAlive())
        {
            GainXp(man, 1, "got away from " + acPlayer);
            if (pGrudge)
                AddHate(man, *pGrudge, 4, pGrudge->From);
            History(man, Tone::Normal, "Came for " + acPlayer + " and got away.");
            Notices.push_back({acPlayer, Wire::kNormal, man.FullName() + " slipped away. " + (man.Body == Kind::HumanFemale || man.Body == Kind::GhoulFemale ? "She" : "He") + "'ll be back."});
        }
        break;
    case Wire::kBackedDown:
        if (pGrudge)
        {
            AddFear(man, *pGrudge, 10);
            AddHate(man, *pGrudge, -5, pGrudge->From);
        }
        History(man, Tone::Normal, "Backed down from " + acPlayer + ".");
        break;
    case Wire::kYouLeft:
    case Wire::kCancelled:
        // He never got there: he'll try again sooner
        man.RestUntil = aNow + 2 * kHour;
        man.LastCameAt = 0;
        break;
    default:
        break;
    }
    for (const uint32_t id : encounter.Men)
    {
        auto other = m_state.Records.find(id);
        if (other != m_state.Records.end() && other->second.LiveEncounter == aEncounterId)
            other->second.LiveEncounter = 0;
    }
    Dirty = true;
}

void Engine::ActorGone(uint32_t aActorServerId)
{
    m_strangers.erase(aActorServerId);
    for (auto& [id, record] : m_state.Records)
        if (record.LiveActor == aActorServerId)
            record.LiveActor = 0;
}

// --- questioning

std::string Engine::OnQuestion(const std::string& acPlayer, uint32_t aRecordId, uint32_t aActorServerId, uint8_t aQuestion, int aValue, int64_t aNow)
{
    if (!aRecordId)
        aRecordId = RecordOfActor(aActorServerId);
    auto it = m_state.Records.find(aRecordId);
    if (it == m_state.Records.end())
        return R"({"error":"No answer."})";
    Record& record = it->second;
    Grudge& grudge = EnsureGrudge(record, acPlayer, Origin::Humbled, "", aNow);
    const auto playerIt = m_state.Players.find(acPlayer);
    const int alignment = playerIt == m_state.Players.end() ? 2 : playerIt->second.Alignment;
    const GangDef* pGang = FindGang(record.Gang);

    // Will he talk? Fear, loyalty, wits; the wicked scare words out, the good earn them
    int talk = 30 + grudge.Fear / 2 - record.Loyalty * 3 / 10 - (record.Wits >= 2 ? 10 : 0);
    if (alignment <= 1)
        talk += 10;
    if (alignment >= 3 && record.Honesty >= 50)
        talk += 10;
    if (grudge.Informant)
        talk += 45;
    if (grudge.Fear >= 90)
        talk = 100;
    // Will he lie?
    int lie = (100 - record.Honesty) * 2 / 5 - grudge.Fear * 3 / 10 + (record.Wits >= 2 ? 10 : 0);
    if (alignment >= 3)
        lie -= 10;
    if (grudge.Informant)
        lie = 0;

    std::string line;
    std::string revealed; // JSON objects
    const auto reveal = [&revealed](const std::string& acKind, const std::string& acText, const std::string& acLocation = {})
    {
        revealed += std::string(revealed.empty() ? "" : ",") + "{\"kind\":" + Text::Json(acKind) + ",\"text\":" + Text::Json(acText) + ",\"location\":" +
                    Text::Json(acLocation) + "}";
    };
    const bool talks = m_rng.Chance(Clamp(talk, 5, 100));
    const bool lies = talks && m_rng.Chance(Clamp(lie, 0, 60));

    switch (aQuestion)
    {
    case Wire::kAskBoss: {
        const Record* pBoss = record.BossId ? Find(record.BossId) : nullptr;
        if (!pBoss && pGang)
        {
            const auto gang = m_state.Gangs.find(record.Gang);
            if (gang != m_state.Gangs.end())
                pBoss = Find(gang->second.LeaderId);
        }
        if (!talks || !pBoss)
        {
            line = pBoss ? Text::Refusal(m_rng, record.Temper) : "I answer to nobody.";
            break;
        }
        m_state.Records[pBoss->Id].Known[acPlayer] |= kKnowsName;
        line = "I answer to " + pBoss->FullName() + ".";
        reveal("man", pBoss->FullName() + ", " + Text::Standing(*pBoss));
        if (!pBoss->Camp.empty())
        {
            std::string camp = pBoss->Camp;
            if (lies && pGang && pGang->Camps.size() > 1)
                camp = m_rng.Pick(pGang->Camps).first;
            for (const auto& [location, name] : pGang ? pGang->Camps : std::vector<std::pair<const char*, const char*>>{})
            {
                if (camp == location)
                {
                    line += std::string(" Holds ") + name + ".";
                    reveal("camp", name, location);
                }
            }
            if (!lies)
                m_state.Records[pBoss->Id].Known[acPlayer] |= kKnowsCamp;
            else
                record.Liar = true;
        }
        break;
    }
    case Wire::kAskCamp: {
        if (!talks || !pGang || pGang->Camps.empty())
        {
            line = Text::Refusal(m_rng, record.Temper);
            break;
        }
        std::string camp = record.Camp;
        if (camp.empty() || lies)
            camp = m_rng.Pick(pGang->Camps).first;
        for (const auto& [location, name] : pGang->Camps)
        {
            if (camp == location)
            {
                line = std::string("We hold ") + name + ".";
                reveal("camp", name, location);
                for (const CampHold& hold : m_state.Camps)
                    if (hold.Location == location && hold.OfficerId)
                    {
                        m_state.Records[hold.OfficerId].Known[acPlayer] |= kKnowsName | kKnowsCamp;
                        if (const Record* pOfficer = Find(hold.OfficerId))
                            reveal("man", pOfficer->FullName() + " runs it");
                    }
            }
        }
        if (lies)
            record.Liar = true;
        break;
    }
    case Wire::kAskStash: {
        if (!talks || !pGang || pGang->Camps.empty())
        {
            line = Text::Refusal(m_rng, record.Temper);
            break;
        }
        const auto& [location, name] = m_rng.Pick(pGang->Camps);
        line = std::string("The goods are kept at ") + name + ".";
        reveal("stash", name, location);
        if (lies)
            record.Liar = true;
        break;
    }
    case Wire::kAskKin: {
        if (!talks)
        {
            line = Text::Refusal(m_rng, record.Temper);
            break;
        }
        record.Known[acPlayer] |= kKnowsKin;
        std::string names;
        if (const Record* pKin = record.KinId ? Find(record.KinId) : nullptr)
        {
            names = "my " + record.KinRelation + " " + pKin->Name;
            reveal("man", pKin->FullName() + ", " + Text::His(record) + " " + record.KinRelation);
            m_state.Records[pKin->Id].Known[acPlayer] |= kKnowsName;
        }
        for (const uint32_t friendId : record.Friends)
        {
            if (const Record* pFriend = Find(friendId))
            {
                names += std::string(names.empty() ? "" : ", ") + pFriend->Name;
                reveal("man", pFriend->FullName() + ", a friend");
                m_state.Records[friendId].Known[acPlayer] |= kKnowsName;
            }
        }
        line = names.empty() ? "Nobody. Nobody'd miss me." : "There's " + names + ".";
        break;
    }
    case Wire::kAskHabits: {
        if (!talks)
        {
            line = Text::Refusal(m_rng, record.Temper);
            break;
        }
        // A habit of someone in his gang
        for (auto& [id, other] : m_state.Records)
        {
            if (other.Gang != record.Gang || other.Habits.empty() || !other.IsAlive() || id == record.Id)
                continue;
            const Habit& habit = other.Habits.front();
            other.Known[acPlayer] |= kKnowsName | kKnowsHabits;
            line = other.Name + " goes to " + habit.Place + " around " + std::to_string(habit.Hour) + ":00.";
            reveal("habit", other.FullName() + ": " + habit.Place + " from " + std::to_string(habit.Hour) + ":00", habit.Location);
            break;
        }
        if (line.empty())
            line = "Nobody keeps a schedule out here.";
        break;
    }
    case Wire::kAskWhy: {
        const int caps = record.Temper == Personality::Greedy || record.Greed >= 70 ? m_rng.Range(40, 200) : 0;
        line = Text::Bargain(m_rng, record, caps);
        if (caps)
            reveal("offer", std::to_string(caps) + " caps for " + std::string(Text::His(record)) + " life");
        break;
    }
    case Wire::kThreaten: {
        // Whether the threat lands: the player's Charisma (aValue, as the client reads it), what he already fears, his
        // loyalty and temper; held to 10-90 as the game's own holdup holds its chance between bounds
        const int charisma = std::clamp(aValue, 1, 15);
        int chance = 15 + charisma * 6 + grudge.Fear / 4 - record.Loyalty / 5;
        if (record.Temper == Personality::Proud || record.Temper == Personality::Hothead)
            chance -= 10;
        else if (record.Temper == Personality::Coward)
            chance += 15;
        if (m_rng.Chance(Clamp(chance, 10, 90)))
        {
            DeedInput deed;
            deed.Deed = Wire::kThreatened;
            deed.RecordId = record.Id;
            OnDeed(acPlayer, deed, aNow);
            line = grudge.Fear >= 70 ? "Okay! Okay, ask me anything!" : "Alright, alright. Easy with that thing.";
        }
        else
        {
            // It didn't take: no fear gained, and he resents it
            AddHate(record, grudge, 5, Origin::Humbled);
            line = Text::Refusal(m_rng, record.Temper);
        }
        break;
    }
    case Wire::kPay: {
        DeedInput deed;
        deed.Deed = Wire::kPaid;
        deed.RecordId = record.Id;
        deed.Amount = aValue;
        OnDeed(acPlayer, deed, aNow);
        grudge.Fear = Clamp(grudge.Fear + 10);
        line = "Caps talk. Ask.";
        break;
    }
    case Wire::kLetHimGo: {
        // Spared, a third still turn on the player who let them go (the rest run); the game running him is ordered so
        const bool fights = m_rng.Chance(33);
        DeedInput deed;
        deed.Deed = Wire::kLetGo;
        deed.RecordId = record.Id;
        deed.Amount = fights ? 1 : 0;
        OnDeed(acPlayer, deed, aNow);
        if (fights)
            line = record.Temper == Personality::Proud ? "Mistake. Should've finished it." : "Big mistake.";
        else
            line = grudge.Informant ? "I won't forget this. If I hear anything, you'll hear it too." : record.Temper == Personality::Proud ? "This isn't over." : "You won't see me again.";
        break;
    }
    default:
        break;
    }

    if (lies && record.Liar)
        History(record, Tone::Dim, "Lied to " + acPlayer + " when questioned.");
    record.Known[acPlayer] |= kKnowsPersonality;
    Dirty = true;

    // The card
    return "{\"id\":" + std::to_string(record.Id) + ",\"actor\":" + std::to_string(record.LiveActor) + ",\"name\":" + Text::Json(record.FullName()) +
           ",\"standing\":" + Text::Json(Text::Standing(record)) + ",\"temper\":" + Text::Json(Text::PersonalityName(record.Temper)) +
           ",\"fear\":" + std::to_string(grudge.Fear) + ",\"fearWord\":" + Text::Json(Text::FearWord(grudge.Fear)) + ",\"hate\":" + std::to_string(grudge.Hate) +
           ",\"female\":" + (Text::IsFemale(record) ? "true" : "false") + ",\"line\":" + Text::Json(line) + ",\"revealed\":[" + revealed +
           "],\"done\":" + (aQuestion == Wire::kLetHimGo || aQuestion == Wire::kClose || aQuestion == Wire::kFinishHim ? "true" : "false") +
           ",\"bar\":" + GunpointJson(acPlayer, record.Id, 0) + "}";
}
} // namespace Nemesis
