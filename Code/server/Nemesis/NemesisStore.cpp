#include "NemesisEngine.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <type_traits>

// Nemeses: nemeses.txt. Tab-separated lines, one kind per line (see docs/NEMESIS.md), text escaped (\t \n \\).
namespace Nemesis
{
namespace
{
constexpr const char* kHeader = "# Reclamation Day nemeses v1";

std::string Escape(const std::string& acText)
{
    std::string out;
    out.reserve(acText.size());
    for (const char c : acText)
    {
        switch (c)
        {
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\\': out += "\\\\"; break;
        default: out += c;
        }
    }
    return out;
}

std::string Unescape(const std::string& acText)
{
    std::string out;
    out.reserve(acText.size());
    for (size_t i = 0; i < acText.size(); ++i)
    {
        if (acText[i] != '\\' || i + 1 >= acText.size())
        {
            out += acText[i];
            continue;
        }
        const char next = acText[++i];
        out += next == 't' ? '\t' : next == 'n' ? '\n' : next;
    }
    return out;
}

// A man's face (binary, as the game saved it) on one line
std::string ToBase64(const std::string& acData)
{
    static const char s_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((acData.size() + 2) / 3 * 4);
    for (size_t i = 0; i < acData.size(); i += 3)
    {
        uint32_t chunk = static_cast<uint8_t>(acData[i]) << 16;
        if (i + 1 < acData.size())
            chunk |= static_cast<uint8_t>(acData[i + 1]) << 8;
        if (i + 2 < acData.size())
            chunk |= static_cast<uint8_t>(acData[i + 2]);
        out += s_alphabet[(chunk >> 18) & 63];
        out += s_alphabet[(chunk >> 12) & 63];
        out += i + 1 < acData.size() ? s_alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < acData.size() ? s_alphabet[chunk & 63] : '=';
    }
    return out;
}

std::string FromBase64(const std::string& acText)
{
    const auto value = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::string out;
    uint32_t chunk = 0;
    int bits = 0;
    for (const char c : acText)
    {
        if (c == '=')
            break;
        const int v = value(c);
        if (v < 0)
            throw std::runtime_error("bad face");
        chunk = (chunk << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out += static_cast<char>((chunk >> bits) & 0xFF);
        }
    }
    return out;
}

std::vector<std::string> Split(const std::string& acLine)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true)
    {
        const size_t tab = acLine.find('\t', start);
        fields.push_back(Unescape(acLine.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
        if (tab == std::string::npos)
            return fields;
        start = tab + 1;
    }
}

template <class T> std::string JoinIds(const std::vector<T>& acIds)
{
    std::string out;
    for (const T& id : acIds)
        out += (out.empty() ? "" : ",") + std::to_string(id);
    return out;
}

std::string JoinText(const std::vector<std::string>& acTexts)
{
    std::string out;
    for (const std::string& text : acTexts)
        out += (out.empty() ? "" : ",") + text;
    return out;
}

std::vector<std::string> SplitCommas(const std::string& acText)
{
    std::vector<std::string> out;
    std::stringstream stream(acText);
    std::string item;
    while (std::getline(stream, item, ','))
        if (!item.empty())
            out.push_back(item);
    return out;
}

// Fields of a line, as a writer
struct Line
{
    std::string Text;

    template <class T> Line& operator<<(const T& acValue)
    {
        if constexpr (std::is_same_v<T, std::string>)
            Text += "\t" + Escape(acValue);
        else if constexpr (std::is_same_v<T, bool>)
            Text += acValue ? "\t1" : "\t0";
        else if constexpr (std::is_floating_point_v<T>)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "\t%.6g", static_cast<double>(acValue));
            Text += buffer;
        }
        else if constexpr (std::is_enum_v<T>)
            Text += "\t" + std::to_string(static_cast<int>(acValue));
        else
            Text += "\t" + std::to_string(acValue);
        return *this;
    }
};

// Fields of a line, as a reader (throws on a bad number: the line is skipped)
struct Fields
{
    std::vector<std::string> Values;
    size_t Next = 1;

    const std::string& Str() { return Next < Values.size() ? Values[Next++] : (Next++, Empty()); }
    int64_t Num() { return std::stoll(Str()); }
    int Int() { return static_cast<int>(Num()); }
    uint32_t Id() { return static_cast<uint32_t>(std::stoull(Str())); }
    float Float() { return std::stof(Str()); }
    bool Bool() { return Str() == "1"; }
    // Fields added after a line's first version: a file written before them still loads
    int IntOr(int aDefault) { return Next < Values.size() ? Int() : (Next++, aDefault); }
    static const std::string& Empty()
    {
        static const std::string s_empty;
        return s_empty;
    }
};
} // namespace

std::string Engine::Save() const
{
    std::string out = std::string(kHeader) + "\n";
    const auto write = [&out](const char* acpKind, const Line& acLine) { out += acpKind + acLine.Text + "\n"; };
    const WorldState& s = m_state;

    write("world", Line{} << s.Campaign << s.Clock << s.NextId << s.NextEventId << s.NextWarId << s.NextEventAt << s.NextDayAt);
    for (const auto& [id, r] : s.Records)
    {
        write("man", Line{} << r.Id << r.Name << r.Nickname << r.Gang << r.Rank << r.RankXp << r.Body << r.AppearanceFlags << r.Temper << r.Aggression << r.Nerve << r.Honesty
                            << r.Greed << r.Ambition << r.Loyalty << r.Wits << r.Alignment << r.Trade << r.Faith << r.Home << r.BornYear << r.KinId << r.KinRelation
                            << r.BossId << r.Camp << r.State << r.Fate << r.BackAt << static_cast<int>(r.Injuries) << r.Protected << r.Seeded << r.BornAt << r.RestUntil
                            << r.LastCameAt << r.TimesCheatedDeath << r.TimesSurrendered << r.Liar << r.World << r.X << r.Y << JoinIds(r.Friends) << r.KilledBy);
        if (!r.Appearance.empty())
            write("look", Line{} << r.Id << ToBase64(r.Appearance));
        for (const Grudge& g : r.Grudges)
            write("grudge", Line{} << r.Id << g.Player << g.From << g.Hate << g.Fear << g.TimesDownedThem << g.TimesMet << g.TimesSparedBy << g.TimesRobbedBy
                                   << g.TimesHumbledBy << g.Place << g.Since << g.LastDealt << g.Mercy << g.Informant << g.OwesThem);
        for (const Scar& scar : r.Scars)
            write("scar", Line{} << r.Id << scar.Wound << scar.Text << scar.Place << scar.ByPlayer << scar.At);
        for (const Wound& w : r.Wounds)
            write("wound", Line{} << r.Id << w.At << w.Where << static_cast<int>(w.Cause) << w.Damage << w.Hits << w.Range << w.Weapon << w.Ammo << w.Place
                                  << w.ByPlayer << w.Crippled << w.Lethal);
        for (const HistoryLine& line : r.History)
            write("hist", Line{} << r.Id << line.At << line.Style << line.Text);
        for (const Memory& memory : r.Memories)
            write("mem", Line{} << r.Id << memory.Feeling << memory.Intensity << memory.Cause << memory.At);
        for (const Goal& goal : r.Goals)
            write("goal", Line{} << r.Id << goal.Type << goal.TargetId << goal.TargetPlayer << goal.Text << goal.Since);
        for (const Habit& habit : r.Habits)
            write("habit", Line{} << r.Id << habit.Place << habit.Location << habit.Hour << habit.Length << habit.DaysMask);
        for (const auto& [player, bits] : r.Known)
            write("known", Line{} << r.Id << player << bits);
    }
    for (const auto& [id, g] : s.Gangs)
    {
        write("gang", Line{} << g.Id << g.Men << g.Purse << g.Supplies << g.Destroyed << g.LeaderId << g.FallenLeader << g.SuccessorId << g.SeatFillAt << g.BrokeSince);
        for (const auto& [player, value] : g.Notoriety)
        {
            const auto at = g.NotorietyAt.find(player);
            write("noto", Line{} << g.Id << player << value << (at == g.NotorietyAt.end() ? int64_t(0) : at->second));
        }
        for (const auto& [district, share] : g.Ground)
            write("ground", Line{} << g.Id << district << share);
    }
    for (const Relation& relation : s.Relations)
        write("rel", Line{} << relation.A << relation.B << relation.Value << relation.Why);
    for (const War& war : s.Wars)
        write("war", Line{} << war.Id << war.A << war.B << war.District << war.Goal << war.Start << war.End << war.Battles << war.DeadA << war.DeadB << war.Heat
                            << war.Result);
    for (const Event& event : s.Chronicle)
        write("event", Line{} << event.Id << event.At << static_cast<int>(event.Kind) << event.Title << event.Text << event.Place << event.Player
                              << JoinText(event.Gangs) << JoinIds(event.Men));
    for (const CampHold& camp : s.Camps)
        write("camp", Line{} << camp.Location << camp.Name << camp.Gang << camp.OfficerId << camp.ClearedAt << camp.RetakeAt);
    for (const Funeral& funeral : s.Funerals)
        write("funeral", Line{} << funeral.DeadId << funeral.Gang << funeral.Place << funeral.Location << funeral.Start << funeral.End);
    for (const auto& [name, player] : s.Players)
        write("player", Line{} << player.Name << player.Karma << player.Alignment << player.NextRollAt << player.Kills << player.GangKills << player.NemesesMade
                                << player.Spared << player.Executed << player.Robbed << player.Stashes << player.Camps << player.Leaders << player.Downed << player.Threats
                                << player.Bribes << player.Hits);
    for (const std::string& name : s.RetiredNames)
        write("retired", Line{} << name);
    return out;
}

bool Engine::Load(const std::string& acText, int& aBadLines)
{
    aBadLines = 0;
    std::stringstream stream(acText);
    std::string line;
    if (!std::getline(stream, line) || line.rfind("# Reclamation Day nemeses", 0) != 0)
        return false;

    WorldState s;
    const auto record = [&s](uint32_t aId) -> Record&
    {
        auto it = s.Records.find(aId);
        if (it == s.Records.end())
            throw std::runtime_error("no such man");
        return it->second;
    };

    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        Fields f{Split(line)};
        const std::string& kind = f.Values[0];
        try
        {
            if (kind == "world")
            {
                s.Campaign = f.Str();
                s.Clock = f.Num();
                s.NextId = f.Id();
                s.NextEventId = f.Id();
                s.NextWarId = f.Id();
                s.NextEventAt = f.Num();
                s.NextDayAt = f.Num();
            }
            else if (kind == "man")
            {
                Record r;
                r.Id = f.Id();
                r.Name = f.Str();
                r.Nickname = f.Str();
                r.Gang = f.Str();
                r.Rank = f.Int();
                r.RankXp = f.Int();
                r.Body = static_cast<Kind>(f.Int());
                r.AppearanceFlags = f.Id();
                r.Temper = static_cast<Personality>(f.Int());
                r.Aggression = f.Int();
                r.Nerve = f.Int();
                r.Honesty = f.Int();
                r.Greed = f.Int();
                r.Ambition = f.Int();
                r.Loyalty = f.Int();
                r.Wits = f.Int();
                r.Alignment = f.Int();
                r.Trade = f.Str();
                r.Faith = f.Str();
                r.Home = f.Str();
                r.BornYear = f.Int();
                r.KinId = f.Id();
                r.KinRelation = f.Str();
                r.BossId = f.Id();
                r.Camp = f.Str();
                r.State = static_cast<Status>(f.Int());
                r.Fate = f.Str();
                r.BackAt = f.Num();
                r.Injuries = static_cast<uint8_t>(f.Int());
                r.Protected = f.Bool();
                r.Seeded = f.Bool();
                r.BornAt = f.Num();
                r.RestUntil = f.Num();
                r.LastCameAt = f.Num();
                r.TimesCheatedDeath = f.Int();
                r.TimesSurrendered = f.Int();
                r.Liar = f.Bool();
                r.World = f.Str();
                r.X = f.Float();
                r.Y = f.Float();
                for (const std::string& id : SplitCommas(f.Str()))
                    r.Friends.push_back(static_cast<uint32_t>(std::stoul(id)));
                r.KilledBy = f.Str();
                if (r.Rank < 1 || r.Rank > 5 || static_cast<int>(r.Temper) >= static_cast<int>(Personality::Count))
                    throw std::runtime_error("bad man");
                s.Records[r.Id] = r;
            }
            else if (kind == "grudge")
            {
                Record& r = record(f.Id());
                Grudge g;
                g.Player = f.Str();
                g.From = static_cast<Origin>(f.Int());
                g.Hate = f.Int();
                g.Fear = f.Int();
                g.TimesDownedThem = f.Int();
                g.TimesMet = f.Int();
                g.TimesSparedBy = f.Int();
                g.TimesRobbedBy = f.Int();
                g.TimesHumbledBy = f.Int();
                g.Place = f.Str();
                g.Since = f.Num();
                g.LastDealt = f.Num();
                g.Mercy = f.Bool();
                g.Informant = f.Bool();
                g.OwesThem = f.Bool();
                r.Grudges.push_back(g);
            }
            else if (kind == "look")
            {
                Record& r = record(f.Id());
                r.Appearance = FromBase64(f.Str());
            }
            else if (kind == "scar")
            {
                Record& r = record(f.Id());
                Scar scar;
                scar.Wound = f.Str();
                scar.Text = f.Str();
                scar.Place = f.Str();
                scar.ByPlayer = f.Str();
                scar.At = f.Num();
                r.Scars.push_back(scar);
            }
            else if (kind == "wound")
            {
                Record& r = record(f.Id());
                Wound w;
                w.At = f.Num();
                w.Where = static_cast<Part>(std::clamp(f.Int(), 0, static_cast<int>(Part::Unknown)));
                w.Cause = static_cast<uint8_t>(f.Int());
                w.Damage = f.Int();
                w.Hits = f.Int();
                w.Range = f.Int();
                w.Weapon = f.Str();
                w.Ammo = f.Str();
                w.Place = f.Str();
                w.ByPlayer = f.Str();
                w.Crippled = f.Bool();
                w.Lethal = f.Bool();
                if (r.Wounds.size() < kMaxWounds)
                    r.Wounds.push_back(w);
            }
            else if (kind == "hist")
            {
                Record& r = record(f.Id());
                HistoryLine entry;
                entry.At = f.Num();
                entry.Style = static_cast<Tone>(f.Int());
                entry.Text = f.Str();
                r.History.push_back(entry);
            }
            else if (kind == "mem")
            {
                Record& r = record(f.Id());
                Memory memory;
                memory.Feeling = static_cast<Emotion>(f.Int());
                memory.Intensity = f.Int();
                memory.Cause = f.Str();
                memory.At = f.Num();
                r.Memories.push_back(memory);
            }
            else if (kind == "goal")
            {
                Record& r = record(f.Id());
                Goal goal;
                goal.Type = static_cast<GoalType>(f.Int());
                goal.TargetId = f.Id();
                goal.TargetPlayer = f.Str();
                goal.Text = f.Str();
                goal.Since = f.Num();
                r.Goals.push_back(goal);
            }
            else if (kind == "habit")
            {
                Record& r = record(f.Id());
                Habit habit;
                habit.Place = f.Str();
                habit.Location = f.Str();
                habit.Hour = f.Int();
                habit.Length = f.Int();
                habit.DaysMask = f.Int();
                r.Habits.push_back(habit);
            }
            else if (kind == "known")
            {
                Record& r = record(f.Id());
                const std::string player = f.Str();
                r.Known[player] = f.Id();
            }
            else if (kind == "gang")
            {
                GangState g;
                g.Id = f.Str();
                g.Men = f.Int();
                g.Purse = f.Int();
                g.Supplies = f.Int();
                g.Destroyed = f.Bool();
                g.LeaderId = f.Id();
                g.FallenLeader = f.Str();
                g.SuccessorId = f.Id();
                g.SeatFillAt = f.Num();
                g.BrokeSince = f.Num();
                s.Gangs[g.Id] = g;
            }
            else if (kind == "noto")
            {
                GangState& g = s.Gangs.at(f.Str());
                const std::string player = f.Str();
                g.Notoriety[player] = f.Int();
                g.NotorietyAt[player] = f.Num();
            }
            else if (kind == "ground")
            {
                GangState& g = s.Gangs.at(f.Str());
                const std::string district = f.Str();
                g.Ground[district] = f.Float();
            }
            else if (kind == "rel")
            {
                Relation relation;
                relation.A = f.Str();
                relation.B = f.Str();
                relation.Value = f.Int();
                relation.Why = f.Str();
                s.Relations.push_back(relation);
            }
            else if (kind == "war")
            {
                War war;
                war.Id = f.Id();
                war.A = f.Str();
                war.B = f.Str();
                war.District = f.Str();
                war.Goal = f.Str();
                war.Start = f.Num();
                war.End = f.Num();
                war.Battles = f.Int();
                war.DeadA = f.Int();
                war.DeadB = f.Int();
                war.Heat = f.Int();
                war.Result = f.Str();
                s.Wars.push_back(war);
            }
            else if (kind == "event")
            {
                Event event;
                event.Id = f.Id();
                event.At = f.Num();
                event.Kind = static_cast<uint8_t>(f.Int());
                event.Title = f.Str();
                event.Text = f.Str();
                event.Place = f.Str();
                event.Player = f.Str();
                event.Gangs = SplitCommas(f.Str());
                for (const std::string& id : SplitCommas(f.Str()))
                    event.Men.push_back(static_cast<uint32_t>(std::stoul(id)));
                s.Chronicle.push_back(event);
            }
            else if (kind == "camp")
            {
                CampHold camp;
                camp.Location = f.Str();
                camp.Name = f.Str();
                camp.Gang = f.Str();
                camp.OfficerId = f.Id();
                camp.ClearedAt = f.Num();
                camp.RetakeAt = f.Num();
                s.Camps.push_back(camp);
            }
            else if (kind == "funeral")
            {
                Funeral funeral;
                funeral.DeadId = f.Id();
                funeral.Gang = f.Str();
                funeral.Place = f.Str();
                funeral.Location = f.Str();
                funeral.Start = f.Num();
                funeral.End = f.Num();
                s.Funerals.push_back(funeral);
            }
            else if (kind == "player")
            {
                PlayerState player;
                player.Name = f.Str();
                player.Karma = f.Int();
                player.Alignment = f.Int();
                player.NextRollAt = f.Num();
                player.Kills = f.Int();
                for (int* pTally : {&player.GangKills, &player.NemesesMade, &player.Spared, &player.Executed, &player.Robbed, &player.Stashes, &player.Camps,
                                    &player.Leaders, &player.Downed, &player.Threats, &player.Bribes, &player.Hits})
                    *pTally = f.IntOr(0);
                s.Players[player.Name] = player;
            }
            else if (kind == "retired")
                s.RetiredNames.push_back(f.Str());
            else
                ++aBadLines;
        }
        catch (...)
        {
            ++aBadLines;
        }
    }

    // Ids stay ahead of what's kept
    for (const auto& [id, r] : s.Records)
        s.NextId = std::max(s.NextId, id + 1);
    for (const Event& event : s.Chronicle)
        s.NextEventId = std::max(s.NextEventId, event.Id + 1);
    for (const War& war : s.Wars)
        s.NextWarId = std::max(s.NextWarId, war.Id + 1);
    m_state = std::move(s);
    m_strangers.clear();
    m_encounters.clear();
    Dirty = false;
    return true;
}
} // namespace Nemesis
