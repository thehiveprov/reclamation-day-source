#include <Services/TacticsService.h>

#if TP_FALLOUT4

#include <Events/UpdateEvent.h>
#include <World.h>
#include <Services/PapyrusService.h>
#include <Services/NemesisService.h>

#include <PlayerCharacter.h>
#include <Games/ActorExtension.h>
#include <Games/TES.h>
#include <Forms/TESNPC.h>
#include <Forms/TESRace.h>
#include <Forms/TESFaction.h>
#include <Forms/TESCombatStyle.h>
#include <Forms/ActorValueInfo.h>
#include <Games/Fallout4/NemesisGame.h>
#include <Games/Fallout4/TacticsTable.h>

namespace
{
using Tactics::Kind;
using Tactics::kProfiles;

constexpr float kInterval = 0.5f;        // game seconds between looks
constexpr float kCheckEvery = 1.f;       // between a fighter's mood checks
constexpr float kHold = 6.f;             // a mood lasts at least this
constexpr float kHoldShaken = 10.f;
constexpr float kNear = 2000.f;          // allies, enemies and deaths that count
constexpr float kHealthMarkEvery = 3.f;  // how often a fighter's health is noted, for "hit hard"
constexpr float kForget = 10.f;          // a fighter not seen for this long has unloaded
constexpr float kDeathMemory = 20.f;
constexpr float kDugIn = 3.f;            // a target out of sight this long is dug in
constexpr float kDugInPerson = 5.f;      // for people (a sharp man 3, a cold one 2)
constexpr float kMutantDeathRange = 600.f; // a mutant falling this close (a hothead's or loyal one's: kNear)
constexpr float kMutantDeathSeconds = 8.f;

// A named man's temper (NotifyNemesis::Identity, the server's Nemesis::Personality order)
enum Temper : int
{
    kHothead,
    kCoward,
    kProud,
    kCold,
    kGreedy,
    kLoyal,
    kDrifter,
};
constexpr const char* kTemperNames[] = {"hothead", "coward", "proud", "cold", "greedy", "loyal", "drifter"};

constexpr char kCombatPlugin[] = "RD_Combat.esp";

// Races (Fallout4.esm, the two last DLCRobot.esm)
constexpr uint32_t kHumanRace = 0x13746;
constexpr uint32_t kGhoulRace = 0xEAFB6;
constexpr uint32_t kSuperMutantRace = 0x1A009;
constexpr uint32_t kSynthGen1Race = 0xE8D09;
constexpr uint32_t kSynthGen2Race = 0x10BD65;
constexpr uint32_t kAssaultronRace = 0xD8417;
constexpr uint32_t kProtectronRace = 0xDFB33;
constexpr uint32_t kSentryBotRace = 0xAE0B7;
constexpr uint32_t kHandyRace = 0x359F4;
constexpr uint32_t kEyeBotRace = 0xA563A;
constexpr uint32_t kRoboBrainRace = 0x1129;
constexpr uint32_t kCreateABotRace = 0x27B9;
// Mr. Gutsy's style: a Handy race robot with it is a Gutsy
constexpr uint32_t kMrGutsyStyle = 0x149C9D;
// Factions people are sorted by after the nemesis gangs (Fallout4.esm)
constexpr uint32_t kBrotherhoodFaction = 0x5DE41;
constexpr uint32_t kRailroadFaction = 0x994F6;
constexpr uint32_t kInstituteFaction = 0x5E558;
constexpr uint32_t kGuardFaction = 0xA179C;

// Specialists who keep their own style (Fallout4.esm): snipers, the Gen 2 flamer, the suicider, berserker and behemoth,
// the stealth Assaultron, Liberty Prime, the vertibird, Slag, the all-offence raider
constexpr uint32_t kKeepStyles[] = {0x1E0411, 0x146154, 0x1CFB1A, 0x10C64E, 0x130F03, 0x0E4DD7, 0x1CFB15, 0x188CF0, 0x0F2590, 0x051E4C, 0x03DB05};
// and in DLCNukaWorld.esm the Overboss's three and Oswald's
constexpr uint32_t kKeepNukaWorldStyles[] = {0x00A572, 0x00A532, 0x00A531, 0x04A6C1};

constexpr const char* kMoodNames[] = {"steady", "press", "shaken", "rage", "flank"};
constexpr const char* kPersonVariants[] = {"ranged", "melee", "heavy"};
constexpr const char* kOtherVariants[] = {"ranged", "melee"};

// The game's frame time, what it updates actors by: slowed with the world, none while it's paused
float GameFrameTime() noexcept
{
    POINTER_FALLOUT4(float, s_frameTime, 112551);
    return *s_frameTime.Get();
}

uint32_t PluginFormId(const char* acpPlugin, uint32_t aLocalId) noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(acpPlugin);
    return pMod && pMod->IsLoaded() ? pMod->GetFormId(aLocalId) : 0;
}

// The style the game fights the actor with (Actor::GetCombatStyle, CommonLibF4 id 1270929): its own, else in power
// armor the power armor one, else its AI's, else its base's
TESCombatStyle* CurrentStyle(Actor* apActor) noexcept
{
    using TGetCombatStyle = TESCombatStyle*(Actor*);
    POINTER_FALLOUT4(TGetCombatStyle, s_getCombatStyle, 1270930);
    return s_getCombatStyle.Get()(apActor);
}

// The reference's own style, set by a script or by us (ExtraDataList::GetCombatStyle, id 1008546), or null
TESCombatStyle* OwnStyle(Actor* apActor) noexcept
{
    using TGetExtraCombatStyle = TESCombatStyle*(ExtraDataList*);
    POINTER_FALLOUT4(TGetExtraCombatStyle, s_getExtraCombatStyle, 1008547);
    return apActor->extraData ? s_getExtraCombatStyle.Get()(apActor->extraData) : nullptr;
}

// Actor::SetCombatStyle (id 33850, all that Papyrus Actor.SetCombatStyle does): sets the reference's own style, or with
// null takes it away, and has its combat controller fight with it at once
void SetStyle(Actor* apActor, TESCombatStyle* apStyle) noexcept
{
    using TSetCombatStyle = void(Actor*, TESCombatStyle*);
    POINTER_FALLOUT4(TSetCombatStyle, s_setCombatStyle, 33851);
    s_setCombatStyle.Get()(apActor, apStyle);
}

// One of RD_Combat.esp's tactics styles
bool IsOurs(const TESCombatStyle* apStyle) noexcept
{
    if (!apStyle)
        return false;
    const uint32_t first = PluginFormId(kCombatPlugin, kProfiles[0].FirstStyle);
    const auto& last = kProfiles[std::size(kProfiles) - 1];
    return first && apStyle->formID >= first && apStyle->formID < first + (last.FirstStyle - kProfiles[0].FirstStyle) + last.VariantCount * last.MoodCount;
}

bool IsKept(const TESCombatStyle* apStyle) noexcept
{
    if (!apStyle)
        return false;
    if (std::find(std::begin(kKeepStyles), std::end(kKeepStyles), apStyle->formID) != std::end(kKeepStyles))
        return true;
    return std::any_of(std::begin(kKeepNukaWorldStyles), std::end(kKeepNukaWorldStyles),
                       [apStyle](uint32_t aLocal) { return apStyle->formID == PluginFormId("DLCNukaWorld.esm", aLocal); });
}

int FindProfile(const char* acpName) noexcept
{
    for (size_t i = 0; i < std::size(kProfiles); ++i)
        if (std::strcmp(kProfiles[i].Name, acpName) == 0)
            return static_cast<int>(i);
    return -1;
}

bool InBaseFaction(const TESNPC* apNpc, uint32_t aFaction) noexcept
{
    const auto& factions = apNpc->actorData.factions;
    for (uint32_t i = 0; i < factions.length; ++i)
        if (factions.data[i].faction && factions.data[i].faction->formID == aFaction && factions.data[i].rank >= 0)
            return true;
    return false;
}

// What the actor is, as one of the profiles (-1: none, it keeps its style)
int ProfileOf(Actor* apActor, const TESCombatStyle* apStyle) noexcept
{
    const auto* pNpc = Cast<TESNPC>(apActor->baseForm);
    const TESRace* pRace = pNpc ? pNpc->raceForm.race : nullptr;
    if (!pRace)
        return -1;

    switch (pRace->formID)
    {
    case kSuperMutantRace: return FindProfile("Mutant");
    case kSynthGen1Race:
    case kSynthGen2Race: return FindProfile("Synth");
    case kAssaultronRace: return FindProfile("Assaultron");
    case kProtectronRace: return FindProfile("Protectron");
    case kSentryBotRace: return FindProfile("SentryBot");
    case kHandyRace: return FindProfile(apStyle && apStyle->formID == kMrGutsyStyle ? "Gutsy" : "Handy");
    case kEyeBotRace: return FindProfile("Eyebot");
    case kHumanRace:
    case kGhoulRace: break;
    default:
        if (pRace->formID == PluginFormId("DLCRobot.esm", kRoboBrainRace))
            return FindProfile("Robobrain");
        if (pRace->formID == PluginFormId("DLCRobot.esm", kCreateABotRace))
            return FindProfile("Bot");
        return -1;
    }

    // People: the nemesis gangs first, then the factions they don't have
    static const std::pair<const char*, const char*> s_gangs[] = {
        {"raiders", "Raiders"},     {"gunners", "Gunners"}, {"operators", "Operators"},   {"disciples", "Disciples"},
        {"pack", "Pack"},           {"triggermen", "Triggermen"}, {"atom", "Atom"},      {"atomites", "Atom"},
        {"trappers", "Trappers"},   {"rustdevils", "RustDevils"}, {"forged", "Forged"}, {"minutemen", "Minutemen"},
    };
    const std::string gang = NemesisGame::GangOf(apActor, false);
    for (const auto& [pGang, pProfile] : s_gangs)
        if (gang == pGang)
            return FindProfile(pProfile);
    if (InBaseFaction(pNpc, kBrotherhoodFaction))
        return FindProfile("Bos");
    if (InBaseFaction(pNpc, kRailroadFaction))
        return FindProfile("Railroad");
    if (InBaseFaction(pNpc, kInstituteFaction))
        return FindProfile("Courser");
    if (InBaseFaction(pNpc, kGuardFaction))
        return FindProfile("Minutemen");
    return FindProfile("Generic");
}

float HealthOf(Actor* apActor) noexcept
{
    const float most = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    return most > 0.f ? apActor->GetActorValue(ActorValueInfo::kHealth) / most : 1.f;
}

// Down, knocked over or badly hurt: something to press
bool IsVulnerable(Actor* apActor) noexcept
{
    return apActor->actorState.IsBleedingOut() || ((apActor->actorState.flags1 >> 21) & 0xF) != 0 || HealthOf(apActor) < 0.35f;
}

float DistanceSquared(const Actor* apA, const Actor* apB) noexcept
{
    const float dx = apA->position.x - apB->position.x, dy = apA->position.y - apB->position.y, dz = apA->position.z - apB->position.z;
    return dx * dx + dy * dy + dz * dz;
}

// The moods in TacticsTable.h's order: people steady, press, shaken, flank; mutants steady, rage; synths and robots
// steady, flank
uint8_t MoodIndex(Kind aKind, uint8_t aMood) noexcept
{
    constexpr uint8_t kIndex[] = {0, 1, 2, 1, 1};
    return aKind == Kind::kPerson && aMood == 4 ? 3 : kIndex[aMood];
}

const char* VariantName(const Tactics::Profile& acProfile, uint8_t aVariant) noexcept
{
    if (acProfile.Type == Kind::kRobot)
        return "own";
    return acProfile.Type == Kind::kPerson ? kPersonVariants[aVariant] : kOtherVariants[aVariant];
}
} // namespace

TacticsService::TacticsService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&TacticsService::OnUpdate>(this);
}

TacticsService::~TacticsService() noexcept = default;

void TacticsService::OnUpdate(const UpdateEvent&) noexcept
{
    const float delta = GameFrameTime();
    if (delta <= 0.f)
        return;
    m_clock += delta;
    if ((m_timer -= delta) > 0.f)
        return;
    m_timer = kInterval;

    if (!m_enabled)
    {
        if (m_wasEnabled)
            GiveAllBack();
        m_wasEnabled = false;
        return;
    }
    m_wasEnabled = true;
    Update(kInterval);
}

void TacticsService::Update(float) noexcept
{
    ProcessLists* pLists = ProcessLists::Get();
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pLists || !pPlayer)
        return;

    PAPYRUS_FUNCTION(bool, Actor, IsPlayerTeammate);

    // The players' side (players, their companions) and who fights it, for the odds
    std::vector<Actor*> actors;
    actors.reserve(pLists->highActorHandleArray.length);
    for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
        if (Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(pLists->highActorHandleArray[i])); pActor && pActor->parentCell)
            actors.push_back(pActor);

    m_playerSide.clear();
    if (!pPlayer->IsDead())
        m_playerSide.push_back(pPlayer);
    for (Actor* pActor : actors)
        if (!pActor->IsDead() && !pActor->actorState.IsBleedingOut() && (pActor->GetExtension()->IsPlayer() || s_pIsPlayerTeammate(pActor)))
            m_playerSide.push_back(pActor);
    m_combatants.clear();
    m_theirSide.clear();
    for (Actor* pActor : actors)
    {
        if (pActor == pPlayer || pActor->IsDead() || !pActor->IsInCombat() || pActor->GetExtension()->IsPlayer())
            continue;
        Actor* pTarget = pActor->GetCombatTarget();
        m_combatants.emplace_back(pActor, pTarget);
        if (pTarget && std::find(m_playerSide.begin(), m_playerSide.end(), pTarget) != m_playerSide.end() &&
            std::find(m_playerSide.begin(), m_playerSide.end(), pActor) == m_playerSide.end())
            m_theirSide.push_back(pActor);
    }

    for (Actor* pActor : actors)
    {
        const auto* pExtension = pActor->GetExtension();
        if (pActor == pPlayer || pExtension->IsPlayer())
            continue;

        auto it = m_fighters.find(pActor->formID);
        if (pActor->IsDead())
        {
            if (it != m_fighters.end())
            {
                if (it->second.Profile >= 0)
                    m_deaths.push_back({{pActor->position.x, pActor->position.y, pActor->position.z}, m_clock, it->second.Profile});
                if (it->second.Applied)
                    GiveBack(pActor, "dead");
                m_fighters.erase(it);
            }
            continue;
        }
        if (!pActor->IsInCombat())
        {
            if (it != m_fighters.end())
            {
                if (it->second.Applied)
                    GiveBack(pActor, "out of combat");
                m_fighters.erase(it);
            }
            continue;
        }

        if (it == m_fighters.end())
        {
            it = m_fighters.emplace(pActor->formID, Fighter{}).first;
            it->second.HealthMark = HealthOf(pActor);
            it->second.HealthMarkAt = m_clock;
            Classify(pActor, it->second);
        }
        Fighter& fighter = it->second;
        fighter.LastSeen = m_clock;
        if (fighter.Profile < 0)
            continue;

        // Another player's game runs it: its combat AI is theirs, and so is its style
        if (pExtension->IsRemote())
        {
            if (fighter.Applied)
            {
                GiveBack(pActor, "another player's game runs it now");
                fighter.Applied = false;
            }
            continue;
        }

        if (m_clock < fighter.NextCheck)
            continue;
        fighter.NextCheck = m_clock + kCheckEvery;

        const char* pWhy = "";
        const Mood want = Decide(pActor, fighter, pWhy);
        if (want != fighter.Wanted)
        {
            fighter.Wanted = want;
            fighter.WantedSince = m_clock;
        }
        // Wanted on two looks in a row (a fight on the edge of a rule doesn't flicker), and the mood held long enough
        const float hold = fighter.Now == Mood::kShaken ? kHoldShaken : kHold;
        const bool change = want != fighter.Now && m_clock - fighter.WantedSince >= kCheckEvery && (fighter.Now == Mood::kSteady || m_clock - fighter.MoodSince >= hold);
        if (change || !fighter.Applied)
        {
            const Mood was = fighter.Now;
            if (change)
            {
                fighter.Now = want;
                fighter.MoodSince = m_clock;
            }
            const auto& profile = kProfiles[fighter.Profile];
            if (Apply(pActor, fighter))
            {
                if (change && was != want)
                    spdlog::info("Tactics: {:X} {} {} -> {}{}{}{}", pActor->formID, profile.Name, VariantName(profile, fighter.Variant),
                                 kMoodNames[static_cast<uint8_t>(want)], *pWhy ? " (" : "", pWhy, *pWhy ? ")" : "");
                else
                    spdlog::info("Tactics: {:X} {} {} {}", pActor->formID, profile.Name, VariantName(profile, fighter.Variant),
                                 kMoodNames[static_cast<uint8_t>(fighter.Now)]);
            }
        }
    }

    // Unloaded fighters: their style back if they're still around, and old deaths forgotten
    for (auto it = m_fighters.begin(); it != m_fighters.end();)
    {
        if (m_clock - it->second.LastSeen <= kForget)
        {
            ++it;
            continue;
        }
        if (it->second.Applied)
            if (Actor* pActor = Cast<Actor>(TESForm::GetById(it->first)))
                GiveBack(pActor, "unloaded");
        it = m_fighters.erase(it);
    }
    std::erase_if(m_deaths, [this](const Death& acDeath) { return m_clock - acDeath.When > kDeathMemory; });
}

void TacticsService::Classify(Actor* apActor, Fighter& aFighter) noexcept
{
    aFighter.Profile = -1;

    PAPYRUS_FUNCTION(bool, Actor, IsPlayerTeammate);
    if (s_pIsPlayerTeammate(apActor))
        return;

    // A style a script set is the script's: we'd take it away when we gave ours back
    const TESCombatStyle* pOwn = OwnStyle(apActor);
    if (pOwn && !IsOurs(pOwn))
    {
        spdlog::info("Tactics: {:X} left alone, a script set its style ({:X})", apActor->formID, pOwn->formID);
        return;
    }
    const TESCombatStyle* pStyle = CurrentStyle(apActor);
    if (!pOwn && IsKept(pStyle))
        return;

    const int profile = ProfileOf(apActor, pStyle);
    if (profile < 0)
        return;

    // Melee when its style rates melee weapons over guns (the general data's melee and ranged score multipliers)
    const auto* pGeneral = pStyle ? reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pStyle) + 0x20) : nullptr;
    const bool melee = pGeneral && pGeneral[3] > pGeneral[5];
    switch (kProfiles[profile].Type)
    {
    case Kind::kPerson: aFighter.Variant = apActor->IsInPowerArmor() ? 2 : melee ? 1 : 0; break;
    case Kind::kRobot: aFighter.Variant = 0; break;
    default: aFighter.Variant = melee ? 1 : 0; break;
    }
    aFighter.Profile = profile;
}

TacticsService::Mood TacticsService::Decide(Actor* apActor, Fighter& aFighter, const char*& aWhy) noexcept
{
    const auto& profile = kProfiles[aFighter.Profile];
    const float health = HealthOf(apActor);
    if (m_clock - aFighter.HealthMarkAt >= kHealthMarkEvery)
    {
        aFighter.LostRecently = std::max(0.f, aFighter.HealthMark - health);
        aFighter.HealthMark = health;
        aFighter.HealthMarkAt = m_clock;
    }
    Actor* pTarget = apActor->GetCombatTarget();
    PAPYRUS_FUNCTION(bool, Actor, HasDetectionLOS, TESObjectREFR*);

    // A named man fights by his own temper and traits (the nemesis system's) on top of what he is
    const NotifyNemesis::Identity* pSelf = m_world.ctx().at<NemesisService>().TemperamentOf(apActor->formID);
    if (pSelf && pSelf->Temper >= static_cast<int>(std::size(kTemperNames)))
        pSelf = nullptr;
    const int temper = pSelf ? pSelf->Temper : -1;
    if (temper != aFighter.Temper)
    {
        aFighter.Temper = temper;
        if (pSelf)
            spdlog::info("Tactics: {:X} is a named man, a {} (aggression {}, nerve {}, wits {}, loyalty {})", apActor->formID, kTemperNames[temper],
                         pSelf->Aggression, pSelf->Nerve, pSelf->Wits, pSelf->Loyalty);
    }
    const float aggression = std::clamp(profile.Aggression + (pSelf ? static_cast<float>(pSelf->Aggression - 50) / 250.f : 0.f), 0.f, 1.f);
    const float fear = std::clamp(profile.Fearlessness + (pSelf ? static_cast<float>(pSelf->Nerve - 50) / 250.f : 0.f), 0.f, 1.f);
    const int wits = pSelf ? pSelf->Wits : 1;
    const bool loyal = temper == kLoyal || (pSelf && pSelf->Loyalty >= 70);

    switch (profile.Type)
    {
    case Kind::kMutant:
    {
        // Rage is a push that still takes cover, and it takes a lot: a hard hit, a mutant falling beside it, or badly
        // hurt. A cold one keeps its head, a coward only rages cornered, a hothead (or a proud or very aggressive one)
        // goes sooner
        if (temper == kCold)
            return Mood::kSteady;
        const bool hot = temper == kHothead || temper == kProud || aggression >= 0.85f;
        if (temper != kCoward)
        {
            if (aFighter.LostRecently >= (hot ? 0.15f : 0.25f))
                return aWhy = "hit hard", Mood::kRage;
            if (DeathsNear(apActor, aFighter.Profile, kMutantDeathSeconds, hot || loyal ? kNear : kMutantDeathRange) > 0)
                return aWhy = "a mutant fell beside it", Mood::kRage;
        }
        if (health < (hot ? 0.5f : 0.35f))
            return aWhy = "badly hurt", Mood::kRage;
        return Mood::kSteady;
    }

    case Kind::kSynth:
    case Kind::kRobot:
    {
        aFighter.OutOfSight = pTarget && !s_pHasDetectionLOS(apActor, pTarget) ? aFighter.OutOfSight + kCheckEvery : 0.f;
        if (aFighter.OutOfSight >= kDugIn)
            return aWhy = "its target dug in", Mood::kFlank;
        if (DeathsNear(apActor, aFighter.Profile, 10.f, kNear) > 0)
            return aWhy = "one of its kind went down", Mood::kFlank;
        return Mood::kSteady;
    }

    case Kind::kPerson:
    {
        int allies = 0, enemies = 0;
        CountSides(apActor, allies, enemies);
        const int deaths = DeathsNear(apActor, aFighter.Profile, kDeathMemory, kNear);
        // A proud man won't be seen backing down in front of his own
        const bool proudWithAllies = temper == kProud && allies > 0;

        // A death on their side: the fearless and the loyal want revenge, the rest may lose heart
        if (deaths >= 1 && (fear >= 1.f || loyal) && health >= 0.35f)
            return aWhy = loyal ? "a friend died: revenge" : "one of theirs died, and they want revenge", Mood::kPress;
        if (fear < 1.f)
        {
            if (health < (proudWithAllies ? 0.12f : 0.2f + 0.3f * (1.f - fear)))
                return aWhy = "hurt", Mood::kShaken;
            if (!proudWithAllies && deaths >= 1 + static_cast<int>(std::lround(4.f * fear)))
                return aWhy = "its side is dying around it", Mood::kShaken;
            if (fear < 0.5f && allies == 0 && enemies >= 2)
                return aWhy = "alone against more", Mood::kShaken;
            if (temper == kCoward && enemies > allies + 1)
                return aWhy = "outnumbered, and a coward", Mood::kShaken;
        }

        if (temper == kHothead && aFighter.LostRecently > 0.1f)
            return aWhy = "hit, and a hothead", Mood::kPress;
        if (temper == kProud && aFighter.LostRecently > 0.15f)
            return aWhy = "hit, and too proud to take it", Mood::kPress;

        // Working round a target that dug in: the cunning factions and sharp men, a cold one soonest, never the dim
        if (pTarget && wits > 0 && (profile.Cunning >= 0.7f || wits == 2 || temper == kCold))
        {
            aFighter.OutOfSight = !s_pHasDetectionLOS(apActor, pTarget) ? aFighter.OutOfSight + kCheckEvery : 0.f;
            if (aFighter.OutOfSight >= (temper == kCold ? 2.f : wits == 2 ? 3.f : kDugInPerson))
                return aWhy = "its target dug in", Mood::kFlank;
        }

        // Pressing a target that can't fight back (even a coward will), then the odds: not a coward, a cold or a greedy
        // man, sooner for a hothead or an aggressive one
        if (pTarget && IsVulnerable(pTarget))
            return aWhy = temper == kGreedy ? "its target is down, and greedy" : "its target is down or hurt", Mood::kPress;
        if (temper == kCoward || temper == kCold || temper == kGreedy)
            return Mood::kSteady;
        const int side = allies + 1;
        const bool outnumber = temper == kHothead ? allies >= 1 && side >= enemies
                                                  : side >= 3 && static_cast<float>(side) >= (aggression >= 0.75f ? 1.5f : 2.f) * static_cast<float>(enemies);
        if (outnumber)
            return aWhy = "they outnumber them", Mood::kPress;
        return Mood::kSteady;
    }
    }
    return Mood::kSteady;
}

bool TacticsService::Apply(Actor* apActor, Fighter& aFighter) noexcept
{
    const auto& profile = kProfiles[aFighter.Profile];
    const uint32_t local = profile.FirstStyle + aFighter.Variant * profile.MoodCount + MoodIndex(profile.Type, static_cast<uint8_t>(aFighter.Now));
    auto* pStyle = Cast<TESCombatStyle>(TESForm::GetById(PluginFormId(kCombatPlugin, local)));
    if (!pStyle)
    {
        static bool s_logged = false;
        if (!s_logged)
            spdlog::warn("Tactics: style {:X} isn't in {} (an old plugin?), enemies keep their own", local, kCombatPlugin);
        s_logged = true;
        aFighter.Profile = -1;
        return false;
    }
    SetStyle(apActor, pStyle);
    aFighter.Applied = true;
    return true;
}

void TacticsService::GiveBack(Actor* apActor, const char* acpWhy) noexcept
{
    if (!IsOurs(OwnStyle(apActor)))
        return;
    SetStyle(apActor, nullptr);
    spdlog::info("Tactics: {:X} gives its style back ({})", apActor->formID, acpWhy);
}

void TacticsService::GiveAllBack() noexcept
{
    for (const auto& [id, fighter] : m_fighters)
        if (fighter.Applied)
            if (Actor* pActor = Cast<Actor>(TESForm::GetById(id)))
                GiveBack(pActor, "enemy tactics switched off");
    m_fighters.clear();
    m_deaths.clear();
}

// Who a fighter is up against and who with, within kNear. Its foes: its target, whoever targets it, and when it fights
// the players, their whole side; its allies: whoever else fights one of its foes. Right in a three-way fight too
// (raiders against settlers and a player).
void TacticsService::CountSides(const Actor* apActor, int& aAllies, int& aEnemies) const noexcept
{
    std::vector<const Actor*> foes;
    const auto addFoe = [&foes, apActor](const Actor* apOther) {
        if (apOther && apOther != apActor && std::find(foes.begin(), foes.end(), apOther) == foes.end())
            foes.push_back(apOther);
    };
    for (const auto& [pOther, pTarget] : m_combatants)
    {
        if (pOther == apActor)
            addFoe(pTarget);
        else if (pTarget == apActor)
            addFoe(pOther);
    }
    if (std::find(m_theirSide.begin(), m_theirSide.end(), apActor) != m_theirSide.end())
        for (const Actor* pOther : m_playerSide)
            addFoe(pOther);

    const float nearSquared = kNear * kNear;
    aEnemies = 0;
    for (const Actor* pFoe : foes)
        aEnemies += DistanceSquared(pFoe, apActor) < nearSquared;
    aAllies = 0;
    for (const auto& [pOther, pTarget] : m_combatants)
        aAllies += pOther != apActor && pTarget && std::find(foes.begin(), foes.end(), pOther) == foes.end() &&
                   std::find(foes.begin(), foes.end(), pTarget) != foes.end() && DistanceSquared(pOther, apActor) < nearSquared;
}

int TacticsService::DeathsNear(const Actor* apActor, int aProfile, float aSeconds, float aRange) const noexcept
{
    // People count their own faction's dead; the others, their own kind's
    const auto& profile = kProfiles[aProfile];
    int count = 0;
    for (const Death& death : m_deaths)
    {
        if (m_clock - death.When > aSeconds)
            continue;
        const bool same = profile.Type == Kind::kPerson ? death.Profile == aProfile : kProfiles[death.Profile].Type == profile.Type;
        const float dx = death.Position[0] - apActor->position.x, dy = death.Position[1] - apActor->position.y, dz = death.Position[2] - apActor->position.z;
        count += same && dx * dx + dy * dy + dz * dz < aRange * aRange;
    }
    return count;
}

#endif
