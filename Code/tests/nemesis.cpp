#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <Messages/NotifyNemesis.h>
#include <Messages/RequestNemesis.h>

#include <Nemesis/NemesisEngine.h>
#include <Nemesis/NemesisWire.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

using namespace TiltedPhoques;

namespace
{
// A face as the game saves it: binary, zeros and separators included
const char kFace[] = {1, 0, -1, 9, 10, 92, 30, 31, 'f', 'a', 'c', 'e'};

// Whether a text is one JSON value and nothing more (the overlay parses what the server sends)
struct JsonCheck
{
    const std::string& Text;
    size_t At = 0;

    void Space()
    {
        while (At < Text.size() && std::isspace(static_cast<unsigned char>(Text[At])))
            ++At;
    }
    bool Literal(const char* acpWord)
    {
        const size_t length = std::strlen(acpWord);
        if (Text.compare(At, length, acpWord) != 0)
            return false;
        At += length;
        return true;
    }
    bool String()
    {
        if (At >= Text.size() || Text[At] != '"')
            return false;
        for (++At; At < Text.size(); ++At)
        {
            const unsigned char c = static_cast<unsigned char>(Text[At]);
            if (c == '"')
                return ++At, true;
            if (c < 0x20)
                return false;
            if (c == '\\' && ++At >= Text.size())
                return false;
        }
        return false;
    }
    bool Number()
    {
        const size_t from = At;
        while (At < Text.size() && (std::isdigit(static_cast<unsigned char>(Text[At])) || std::strchr("+-.eE", Text[At])))
            ++At;
        return At > from;
    }
    bool Value()
    {
        Space();
        if (At >= Text.size())
            return false;
        bool ok;
        switch (Text[At])
        {
        case '{': {
            ++At, Space();
            if (At < Text.size() && Text[At] == '}')
                return ++At, true;
            // A key twice in one object: the overlay would only see the last
            std::set<std::string> keys;
            do
            {
                Space();
                const size_t key = At;
                if (!String() || !keys.insert(Text.substr(key, At - key)).second)
                    return false;
                Space();
                if (At >= Text.size() || Text[At++] != ':' || !Value())
                    return false;
                Space();
            } while (At < Text.size() && Text[At] == ',' && ++At);
            return At < Text.size() && Text[At++] == '}';
        }
        case '[':
            ++At, Space();
            if (At < Text.size() && Text[At] == ']')
                return ++At, true;
            do
            {
                if (!Value())
                    return false;
                Space();
            } while (At < Text.size() && Text[At] == ',' && ++At);
            return At < Text.size() && Text[At++] == ']';
        case '"': return String();
        case 't': return Literal("true");
        case 'f': return Literal("false");
        case 'n': return Literal("null");
        default: ok = Number(); return ok;
        }
    }
};

bool IsJson(const std::string& acText)
{
    JsonCheck check{acText};
    if (!check.Value())
        return false;
    check.Space();
    return check.At == acText.size();
}

int64_t Start(const std::string& acCampaign)
{
    return Nemesis::Text::ToMinutes(acCampaign == "winter-of-atom" ? 2282 : 2287, 9, 23, 9, 0);
}

Nemesis::Engine MakeWorld(const std::string& acCampaign, uint32_t aSeed)
{
    Nemesis::Engine engine(aSeed);
    engine.Start(acCampaign, {"Fallout4.esm", "DLCRobot.esm", "DLCCoast.esm", "DLCNukaWorld.esm", "KARMA.esm"}, Start(acCampaign));
    engine.PlayerJoined("Nate", Start(acCampaign));
    engine.PlayerJoined("Nora", Start(acCampaign));
    engine.SetKarma("Nora", -600, 0);
    return engine;
}

Nemesis::DeedInput Downed(uint32_t aActor)
{
    Nemesis::DeedInput deed;
    deed.Deed = Nemesis::Wire::kDownedYou;
    deed.ActorServerId = aActor;
    deed.Gang = "raiders";
    deed.World = Nemesis::kCommonwealth;
    deed.X = -33000.f;
    deed.Y = 45000.f;
    deed.Appearance = std::string(kFace, sizeof(kFace));
    deed.AppearanceFlags = 0x3000800;
    return deed;
}

// Thirty game days of encounters for both players, ended in turn by each outcome
void Simulate(Nemesis::Engine& aEngine, int64_t aStart)
{
    uint32_t actor = 20000;
    int encounters = 0;
    for (int64_t now = aStart + 30; now < aStart + 30 * Nemesis::kDay; now += 10)
    {
        aEngine.Advance(now);
        for (const char* pWho : {"Nate", "Nora"})
        {
            Nemesis::PlayerSpot spot{pWho, true, Nemesis::kCommonwealth, -20000.f, 10000.f};
            auto plan = aEngine.RollEncounter(spot, now);
            if (!plan)
                continue;
            REQUIRE(!plan->Men.empty());
            REQUIRE(plan->Men.size() <= 5);
            std::vector<uint32_t> ids;
            for (size_t i = 0; i < plan->Men.size(); ++i)
                ids.push_back(++actor);
            aEngine.OnSpawned(plan->EncounterId, ids);
            const int outcome = encounters++ % 5;
            if (outcome == 0 && plan->Men[0].RecordId)
            {
                Nemesis::DeathInput death;
                death.RecordId = plan->Men[0].RecordId;
                death.Flags = Nemesis::Wire::kByLocalPlayer;
                aEngine.OnDeath(pWho, death, now + 5);
            }
            aEngine.OnOutcome(pWho, plan->EncounterId, outcome == 0 ? Nemesis::Wire::kHeKilled : outcome == 1 ? Nemesis::Wire::kHeFled : Nemesis::Wire::kYouLeft, now + 6);
            for (const uint32_t id : ids)
                aEngine.ActorGone(id);
        }
        aEngine.Notices.clear();
        aEngine.Live.clear();
        aEngine.Karma.clear();
        aEngine.EndedEncounters.clear();
    }
    REQUIRE(encounters > 20);
}
} // namespace

TEST_CASE("Nemesis messages", "[nemeses.encoding]")
{
    SECTION("RequestNemesis")
    {
        Buffer buffer(1000);
        RequestNemesis sent, received;
        sent.Action = RequestNemesis::kDeed;
        sent.Kind = RequestNemesis::kDownedYou;
        sent.Value = -42;
        sent.RecordId = 7;
        sent.ActorServerId = 0x1234;
        sent.Body = 3;
        sent.World = "Fallout4.esm:3C";
        sent.Position = {1.f, -2.f, 3.5f};
        sent.Text = "raiders";
        sent.Text2 = "Lexington";
        sent.ServerIds = {1, 2, 0};
        sent.Appearance = String(kFace, sizeof(kFace));
        sent.AppearanceFlags = 0x3000800;
        sent.Range = 812.5f;
        sent.Count = 3;

        Buffer::Writer writer(&buffer);
        sent.Serialize(writer);
        Buffer::Reader reader(&buffer);
        uint64_t opcode;
        reader.ReadBits(opcode, 8);
        received.DeserializeRaw(reader);
        REQUIRE(sent == received);
    }

    SECTION("NotifyNemesis spawn")
    {
        Buffer buffer(1000);
        NotifyNemesis sent, received;
        sent.Action = NotifyNemesis::kSpawn;
        sent.Spawn.EncounterId = 9;
        sent.Spawn.Kind = NemesisSpawn::kCrew;
        sent.Spawn.Gang = "gunners";
        sent.Spawn.Taunt = "There you are.";
        sent.Spawn.LocationPlugin = "Fallout4.esm";
        sent.Spawn.LocationLocalId = 0x5A5DD;
        sent.Spawn.Distance = 5000.f;
        NemesisMan man;
        man.RecordId = 3;
        man.Body = NemesisMan::kGhoulFemale;
        man.Appearance = String(kFace, sizeof(kFace));
        man.AppearanceFlags = 0x3000800;
        man.Name = "Hattie Graves the Lame";
        man.Rank = 4;
        man.Injuries = NemesisMan::kLimp;
        man.Alignment = -2;
        man.Legendary = true;
        sent.Spawn.Men = {man, NemesisMan{}};

        Buffer::Writer writer(&buffer);
        sent.Serialize(writer);
        Buffer::Reader reader(&buffer);
        uint64_t opcode;
        reader.ReadBits(opcode, 8);
        received.DeserializeRaw(reader);
        REQUIRE(sent == received);
    }

    SECTION("NotifyNemesis identity: temper and traits for the enemy tactics")
    {
        NotifyNemesis::Identity sent;
        sent.Alignment = -2;
        sent.Injuries = NemesisMan::kLimp | NemesisMan::kOneEye;
        sent.Legendary = true;
        sent.Temper = 6; // Drifter, the last
        sent.Wits = 2;
        sent.Aggression = 74;
        sent.Nerve = 100;
        sent.Loyalty = 0;

        Buffer buffer(1000);
        NotifyNemesis message, received;
        message.Action = NotifyNemesis::kIdentity;
        message.Value = NotifyNemesis::PackIdentity(sent);
        REQUIRE(message.Value > 0);
        Buffer::Writer writer(&buffer);
        message.Serialize(writer);
        Buffer::Reader reader(&buffer);
        uint64_t opcode;
        reader.ReadBits(opcode, 8);
        received.DeserializeRaw(reader);

        const auto got = NotifyNemesis::UnpackIdentity(received.Value);
        REQUIRE(got.Alignment == -2);
        REQUIRE(got.Injuries == (NemesisMan::kLimp | NemesisMan::kOneEye));
        REQUIRE(got.Legendary);
        REQUIRE(got.Temper == 6);
        REQUIRE(got.Wits == 2);
        REQUIRE(got.Aggression == 70);
        REQUIRE(got.Nerve == 100);
        REQUIRE(got.Loyalty == 0);

        // What an older server sent: no temper, the default traits
        const auto old = NotifyNemesis::UnpackIdentity(1 | (NemesisMan::kBadArm << 8));
        REQUIRE(old.Alignment == -1);
        REQUIRE(old.Injuries == NemesisMan::kBadArm);
        REQUIRE(old.Temper == -1);
        REQUIRE(old.Wits == 1);
        REQUIRE(old.Aggression == 50);
    }
}

TEST_CASE("Nemesis clock", "[nemeses.rules]")
{
    // The server's calendar holds Fallout 4's GameYear: 287 for 2287, 282 for Winter of Atom's 2282
    REQUIRE(Nemesis::Text::ToMinutes(287, 9, 23, 9, 0) == Nemesis::Text::ToMinutes(2287, 9, 23, 9, 0));
    REQUIRE(Nemesis::Text::ToMinutes(282, 9, 23, 9, 0) == Nemesis::Text::ToMinutes(2282, 9, 23, 9, 0));
    REQUIRE(Nemesis::Text::ToMinutes(282, 0, 1, 0, 0) > 0);
}

TEST_CASE("Nemesis rules", "[nemeses.rules]")
{
    Nemesis::Engine engine = MakeWorld("2287", 7);
    const int64_t start = Start("2287");
    REQUIRE(engine.State().Gangs.count("rustdevils"));
    REQUIRE(engine.State().Gangs.count("marshals"));
    REQUIRE_FALSE(engine.State().Gangs.count("atomites"));

    SECTION("a man who downs a player becomes his nemesis, with his face")
    {
        engine.OnDeed("Nate", Downed(9001), start + 10);
        const uint32_t id = engine.RecordOfActor(9001);
        REQUIRE(id != 0);
        const Nemesis::Record* pRecord = engine.Find(id);
        const Nemesis::Grudge* pGrudge = pRecord->GrudgeOf("Nate");
        REQUIRE(pGrudge);
        REQUIRE(pGrudge->From == Nemesis::Origin::DownedYou);
        REQUIRE(pRecord->Appearance == std::string(kFace, sizeof(kFace)));
        REQUIRE(engine.IntentOf(*pRecord, *pGrudge) == Nemesis::Intent::Hunts);
        REQUIRE_FALSE(pRecord->Gang.empty()); // a raider crew holding the ground
    }

    SECTION("letting a man go gives karma and keeps him away")
    {
        engine.OnDeed("Nate", Downed(9002), start + 10);
        const uint32_t id = engine.RecordOfActor(9002);
        Nemesis::DeedInput letGo;
        letGo.Deed = Nemesis::Wire::kLetGo;
        letGo.RecordId = id;
        engine.OnDeed("Nate", letGo, start + 20);
        REQUIRE_FALSE(engine.Karma.empty());
        REQUIRE(engine.Karma.back().Amount > 0);
        const Nemesis::Record* pRecord = engine.Find(id);
        REQUIRE(pRecord->GrudgeOf("Nate")->Mercy);
    }

    SECTION("an execution is final")
    {
        engine.OnDeed("Nate", Downed(9003), start + 10);
        const uint32_t id = engine.RecordOfActor(9003);
        Nemesis::DeathInput death;
        death.RecordId = id;
        death.Flags = Nemesis::Wire::kByLocalPlayer | Nemesis::Wire::kWhileSurrendered | Nemesis::Wire::kFinal;
        engine.OnDeath("Nate", death, start + 30);
        REQUIRE(engine.Find(id)->State == Nemesis::Status::Dead);
    }

    SECTION("the lawful hear of the wicked, slowly")
    {
        for (int day = 1; day <= 40; ++day)
            engine.Advance(start + day * Nemesis::kDay);
        REQUIRE(engine.NotorietyOf("dcsecurity", "Nora") >= 25);
        REQUIRE(engine.NotorietyOf("dcsecurity", "Nora") <= 52);
        REQUIRE(engine.NotorietyOf("dcsecurity", "Nate") == 0);
    }
}

TEST_CASE("Nemesis questioning", "[nemeses.rules]")
{
    Nemesis::Engine engine = MakeWorld("2287", 13);
    const int64_t start = Start("2287");
    uint32_t actor = 30000;
    const auto newMan = [&](Nemesis::Kind aBody = Nemesis::Kind::HumanMale)
    {
        Nemesis::DeedInput deed = Downed(++actor);
        deed.Body = aBody;
        engine.OnDeed("Nate", deed, start + 10);
        return engine.RecordOfActor(actor);
    };

    SECTION("a threat lands more often with more Charisma")
    {
        const auto landed = [&](int aCharisma)
        {
            int count = 0;
            for (int i = 0; i < 150; ++i)
            {
                const std::string card = engine.OnQuestion("Nate", newMan(), 0, Nemesis::Wire::kThreaten, aCharisma, start + 20);
                REQUIRE(IsJson(card));
                if (card.find("Okay! Okay") != std::string::npos || card.find("Alright, alright") != std::string::npos)
                    ++count;
            }
            return count;
        };
        const int low = landed(1);
        const int high = landed(10);
        REQUIRE(low >= 10);   // never below the floor
        REQUIRE(high > low + 30);
    }

    SECTION("a third of the men let go turn on the player, the rest run")
    {
        int fights = 0, runs = 0;
        for (int i = 0; i < 300; ++i)
        {
            engine.Live.clear();
            engine.OnQuestion("Nate", newMan(), 0, Nemesis::Wire::kLetHimGo, 0, start + 20);
            for (const Nemesis::LiveChange& change : engine.Live)
            {
                if (change.What != Nemesis::LiveChange::kOrder)
                    continue;
                if (change.Order == Nemesis::Wire::kLetGoFights)
                {
                    ++fights;
                    REQUIRE(change.ForPlayer == "Nate");
                }
                else if (change.Order == Nemesis::Wire::kLetGoRuns)
                    ++runs;
            }
        }
        REQUIRE(fights + runs == 300);
        REQUIRE(fights >= 60);
        REQUIRE(fights <= 140);
    }

    SECTION("a woman is spoken of as one")
    {
        const uint32_t id = newMan(Nemesis::Kind::HumanFemale);
        REQUIRE(engine.OnQuestion("Nate", id, 0, Nemesis::Wire::kOpen, 0, start + 20).find("\"female\":true") != std::string::npos);
        engine.OnQuestion("Nate", id, 0, Nemesis::Wire::kLetHimGo, 0, start + 30);
        const std::string last = engine.Find(id)->History.back().Text;
        REQUIRE(last.find("let her go") != std::string::npos);
        REQUIRE(last.find(" him") == std::string::npos);
        REQUIRE(engine.OnQuestion("Nate", newMan(), 0, Nemesis::Wire::kOpen, 0, start + 20).find("\"female\":false") != std::string::npos);
    }
}

TEST_CASE("Nemesis world", "[nemeses.world]")
{
    for (const std::string campaign : {"2287", "winter-of-atom"})
    {
        Nemesis::Engine engine = MakeWorld(campaign, 11);
        const int64_t start = Start(campaign);
        engine.OnDeed("Nate", Downed(9001), start + 10);
        Simulate(engine, start);

        for (const auto& [id, record] : engine.State().Records)
        {
            REQUIRE(record.Rank >= 1);
            REQUIRE(record.Rank <= 5);
            REQUIRE(record.History.size() <= Nemesis::kMaxHistory);
        }
        REQUIRE(engine.State().Records.size() <= Nemesis::kMaxRecords);
        REQUIRE(engine.State().Chronicle.size() <= Nemesis::kMaxChronicle);

        // nemeses.txt: what's kept comes back the same
        const std::string saved = engine.Save();
        Nemesis::Engine loaded(1);
        int bad = -1;
        REQUIRE(loaded.Load(saved, bad));
        REQUIRE(bad == 0);
        REQUIRE(loaded.Save() == saved);

        // The Pip-Boy pages: the top page, a page a nemesis, the gangs and the word, at most kPipboyPages
        for (const char* pWho : {"Nate", "Nora"})
        {
            const std::string pages = engine.PipboyPages(pWho, {"Nate", "Nora"});
            size_t count = 0;
            size_t from = 0;
            for (size_t end = pages.find(Nemesis::kPipboyEntrySeparator); end != std::string::npos; from = end + 1, end = pages.find(Nemesis::kPipboyEntrySeparator, from))
            {
                const std::string entry = pages.substr(from, end - from);
                const size_t first = entry.find(Nemesis::kPipboyFieldSeparator);
                const size_t last = entry.rfind(Nemesis::kPipboyFieldSeparator);
                REQUIRE(first != std::string::npos);
                REQUIRE(last != first);
                REQUIRE((entry.substr(last + 1) == "0" || entry.substr(last + 1) == "1"));
                ++count;
            }
            REQUIRE(from == pages.size());
            REQUIRE(count >= 3);
            REQUIRE(count <= Nemesis::kPipboyPages);
            // The first entry opens the full screen (RD_Nemeses.esp's script watches it)
            REQUIRE(pages.rfind("OPEN FULL SCREEN", 0) == 0);
        }

        // Every page of the overlay's screen is JSON, for both players and pages that aren't there
        for (const char* pWho : {"Nate", "Nora"})
        {
            for (const std::string page : {"record", "gang", "world", "wounds", "nonsense", "man:999999", "gang:nobody"})
                REQUIRE(IsJson(engine.ScreenJson(pWho, {"Nate", "Nora"}, page)));
            for (const auto& [id, gang] : engine.State().Gangs)
                REQUIRE(IsJson(engine.ScreenJson(pWho, {"Nate", "Nora"}, "gang:" + id)));
            for (const auto& [id, record] : engine.State().Records)
            {
                REQUIRE(IsJson(engine.ScreenJson(pWho, {"Nate", "Nora"}, "man:" + std::to_string(id))));
                REQUIRE(IsJson(engine.GunpointJson(pWho, id, 0)));
            }
        }
    }
}

TEST_CASE("Nemesis wound log and record", "[nemeses.screen]")
{
    Nemesis::Engine engine = MakeWorld("2287", 5);
    const int64_t start = Start("2287");
    // Nothing cheats death here: the kill is final
    engine.Config().CheatDeathBase = 0;
    engine.Config().CheatDeathPerRank = 0;
    engine.Config().CheatDeathMax = 0;

    Nemesis::WoundInput wound;
    wound.ActorServerId = 9001;
    wound.Where = Nemesis::Part::LeftLeg;
    wound.Damage = 30;
    wound.Range = 1200;
    wound.Weapon = "Pipe Pistol";
    wound.Ammo = ".38 Round";
    wound.World = Nemesis::kCommonwealth;
    wound.X = -33000.f;
    wound.Y = 45000.f;

    SECTION("a stranger's wounds become his when he swears revenge, a kill marks the last")
    {
        engine.OnWound("Nate", wound, start + 1);
        engine.OnDeed("Nate", Downed(9001), start + 2);
        const uint32_t id = engine.RecordOfActor(9001);
        REQUIRE(id != 0);
        REQUIRE(engine.Find(id)->Wounds.size() == 1);
        REQUIRE(engine.State().Players.at("Nate").NemesesMade == 1);
        REQUIRE(engine.State().Players.at("Nate").Downed == 1);

        // The same spot and weapon again within a game minute: one line, two hits
        wound.ActorServerId = 0;
        wound.RecordId = id;
        wound.Where = Nemesis::Part::Head;
        wound.Crippled = true;
        engine.OnWound("Nate", wound, start + 10);
        engine.OnWound("Nate", wound, start + 10);
        const Nemesis::Record& record = *engine.Find(id);
        REQUIRE(record.Wounds.size() == 2);
        REQUIRE(record.Wounds.back().Hits == 2);
        REQUIRE(record.Wounds.back().Damage == 60);
        REQUIRE(record.Wounds.back().Crippled);
        REQUIRE(engine.State().Players.at("Nate").Hits == 3);

        Nemesis::DeathInput death;
        death.RecordId = id;
        death.Flags = Nemesis::Wire::kByLocalPlayer;
        engine.OnDeath("Nate", death, start + 11);
        REQUIRE(record.State == Nemesis::Status::Dead);
        REQUIRE(record.KilledBy == "Nate");
        REQUIRE(record.Wounds.back().Lethal);
        REQUIRE(!record.Wounds.front().Lethal);
        REQUIRE(engine.State().Players.at("Nate").Kills == 1);
        REQUIRE(engine.State().Players.at("Nate").GangKills == 1);

        // Nate's log has him, killed; Nora's has nobody
        const std::string nate = engine.ScreenJson("Nate", {"Nate"}, "wounds:" + std::to_string(id));
        REQUIRE(IsJson(nate));
        REQUIRE(nate.find("\"outcome\":\"killed\"") != std::string::npos);
        REQUIRE(nate.find("Pipe Pistol") != std::string::npos);
        REQUIRE(engine.ScreenJson("Nora", {"Nora"}, "wounds").find("\"victims\":[]") != std::string::npos);
        // His name is among Nate's dead
        const std::string page = engine.ScreenJson("Nate", {"Nate"}, "record");
        REQUIRE(IsJson(page));
        REQUIRE(page.find(Nemesis::Text::Json(record.FullName())) != std::string::npos);

        // Kept: the wounds, who killed him and the tallies come back the same
        const std::string saved = engine.Save();
        Nemesis::Engine loaded(1);
        int bad = -1;
        REQUIRE(loaded.Load(saved, bad));
        REQUIRE(bad == 0);
        REQUIRE(loaded.Save() == saved);
        REQUIRE(loaded.Find(id)->Wounds.size() == 2);
        REQUIRE(loaded.Find(id)->KilledBy == "Nate");
        REQUIRE(loaded.State().Players.at("Nate").Hits == 3);
    }

    SECTION("a stranger who never swears revenge keeps no log")
    {
        engine.OnWound("Nate", wound, start + 1);
        engine.ActorGone(9001);
        REQUIRE(engine.ScreenJson("Nate", {"Nate"}, "wounds").find("\"victims\":[]") != std::string::npos);
        REQUIRE(engine.GunpointJson("Nate", 0, 9001) == "null");
    }

    SECTION("a file written before the log and the tallies still loads")
    {
        const std::string old = "# Reclamation Day nemeses v1\nworld\t2287\t100\t1\t1\t1\t0\t0\nplayer\tNate\t0\t2\t0\t3\n";
        Nemesis::Engine loaded(1);
        int bad = -1;
        REQUIRE(loaded.Load(old, bad));
        REQUIRE(bad == 0);
        REQUIRE(loaded.State().Players.at("Nate").Kills == 3);
        REQUIRE(loaded.State().Players.at("Nate").GangKills == 0);
    }
}

// Not a test: the overlay mock's sample pages (skyrim_ui src/app/mock/nemesis-samples.ts, Tools/NemesisMockSamples.js),
// a month of the 2287 world for Nate. Run as TPTests "[.nemeses.dump]" with NEMESIS_DUMP set to a folder.
TEST_CASE("Nemesis screen samples", "[.nemeses.dump]")
{
    const char* pFolder = std::getenv("NEMESIS_DUMP");
    REQUIRE(pFolder);
    Nemesis::Engine engine = MakeWorld("2287", 11);
    const int64_t start = Start("2287");
    engine.OnDeed("Nate", Downed(9001), start + 10);
    Simulate(engine, start);
    const int64_t now = start + 30 * Nemesis::kDay;

    // Wounds on four named Gunners, one killed by the last
    const char* weapons[] = {"Combat Rifle", "10mm Pistol", "Double-Barrel Shotgun", "Laser Musket"};
    const char* ammo[] = {".45 Round", "10mm Round", "Shotgun Shell", "Fusion Cell"};
    int shot = 0;
    uint32_t killed = 0;
    for (const auto& [id, record] : engine.State().Records)
    {
        if (!record.IsAlive() || record.Gang != "gunners" || record.Protected || shot >= 4)
            continue;
        for (int hit = 0; hit < 2 + shot % 3; ++hit)
        {
            Nemesis::WoundInput wound;
            wound.RecordId = id;
            wound.Where = static_cast<Nemesis::Part>((shot * 3 + hit) % 6);
            wound.Damage = 12 + 17 * hit + 5 * shot;
            wound.Hits = 1 + hit % 2;
            wound.Range = 900 + 700 * hit + 1300 * shot;
            wound.Cause = shot == 3 ? 4 : 0;
            wound.Crippled = hit == 1 && shot == 1;
            wound.Weapon = weapons[shot];
            wound.Ammo = ammo[shot];
            wound.World = Nemesis::kCommonwealth;
            wound.X = -20000.f + 3000.f * shot;
            wound.Y = 10000.f;
            engine.OnWound("Nate", wound, now - 400 + 90 * shot + 2 * hit);
        }
        if (shot == 2)
            killed = id;
        ++shot;
    }
    // A few raids: two gangs know Nate now
    for (const char* pGang : {"gunners", "rustjaws", "gunners"})
    {
        Nemesis::DeedInput raid;
        raid.Deed = Nemesis::Wire::kCampCleared;
        raid.Gang = pGang;
        raid.World = Nemesis::kCommonwealth;
        raid.X = -20000.f;
        raid.Y = 10000.f;
        engine.OnDeed("Nate", raid, now - 600);
        raid.Deed = Nemesis::Wire::kStashRobbed;
        engine.OnDeed("Nate", raid, now - 590);
        for (int man = 0; man < 6; ++man)
        {
            Nemesis::DeathInput death;
            death.ActorServerId = 50000 + man;
            death.Gang = pGang;
            death.Flags = Nemesis::Wire::kByLocalPlayer;
            engine.OnDeath("Nate", death, now - 580);
        }
    }
    engine.Config().CheatDeathBase = 0;
    engine.Config().CheatDeathPerRank = 0;
    engine.Config().CheatDeathMax = 0;
    if (killed)
    {
        Nemesis::DeathInput death;
        death.RecordId = killed;
        death.Flags = Nemesis::Wire::kByLocalPlayer;
        engine.OnDeath("Nate", death, now - 400 + 90 * 2 + 4);
    }
    engine.Advance(now);

    const auto write = [pFolder](const std::string& acName, const std::string& acJson)
    {
        REQUIRE(IsJson(acJson));
        std::ofstream(std::string(pFolder) + "/" + acName + ".json", std::ios::binary) << acJson;
    };
    const std::vector<std::string> party = {"Nate", "Nora"};
    write("record", engine.ScreenJson("Nate", party, "record"));
    write("world", engine.ScreenJson("Nate", party, "world"));
    write("wounds", engine.ScreenJson("Nate", party, "wounds"));
    for (const auto& [id, gang] : engine.State().Gangs)
        write("gang_" + id, engine.ScreenJson("Nate", party, "gang:" + id));
    int men = 0;
    for (const auto& [id, record] : engine.State().Records)
    {
        if (!record.GrudgeOf("Nate") && !record.Protected)
            continue;
        write("man_" + std::to_string(id), engine.ScreenJson("Nate", party, "man:" + std::to_string(id)));
        if (++men == 1)
        {
            write("gunpoint", engine.GunpointJson("Nate", id, 0));
            write("card", engine.OnQuestion("Nate", id, 0, Nemesis::Wire::kOpen, 0, now));
        }
    }
}
