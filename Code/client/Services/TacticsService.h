#pragma once

#include <unordered_map>
#include <utility>
#include <vector>

struct World;
struct UpdateEvent;
struct Actor;
struct TESCombatStyle;

/**
 * @brief Enemy tactics: the fighters this game runs fight like what they are, robots, synths, super mutants or which
 * faction of people, and change as the fight turns: people press an advantage, get shaken or work round a target that
 * dug in, super mutants push in a rage, synths and robots flank; a named man (the nemesis system's) by his own temper
 * and traits on top (docs/ENEMY_TACTICS.md). Only which combat style the game's own combat AI
 * uses: RD_Combat.esp's (Tools/BuildCombatPlugin.js, TacticsTable.h), put on with the game's Actor::SetCombatStyle and
 * given back when the fight ends. Fallout 4 only.
 */
struct TacticsService
{
    TacticsService(World& aWorld, entt::dispatcher& aDispatcher);
    ~TacticsService() noexcept;

    TP_NOCOPYMOVE(TacticsService);

    // From MovementService: the host's switch on RD's MCM page Enemy AI (off gives every style back)
    void SetEnabled(bool aEnabled) noexcept { m_enabled = aEnabled; }

private:
    enum class Mood : uint8_t
    {
        kSteady,
        kPress,
        kShaken,
        kRage,
        kFlank,
    };

    struct Fighter
    {
        int Profile{-1}; // in Tactics::kProfiles; -1: left alone
        uint8_t Variant{0};
        Mood Now{Mood::kSteady};
        Mood Wanted{Mood::kSteady}; // what the last look wanted, and since when: a change needs two looks in a row
        float WantedSince{0.f};
        bool Applied{false};
        float MoodSince{0.f};
        float NextCheck{0.f};
        float LastSeen{0.f};
        float OutOfSight{0.f};
        float HealthMark{1.f};   // health at the last mark, every 3 s
        float HealthMarkAt{0.f};
        float LostRecently{0.f}; // health lost between the last two marks
        int Temper{-1};          // a named man's (NotifyNemesis::Identity), -1 none
    };

    struct Death
    {
        float Position[3];
        float When;
        int Profile;
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void Update(float aDelta) noexcept;
    void Classify(Actor* apActor, Fighter& aFighter) noexcept;
    Mood Decide(Actor* apActor, Fighter& aFighter, const char*& aWhy) noexcept;
    bool Apply(Actor* apActor, Fighter& aFighter) noexcept;
    void GiveBack(Actor* apActor, const char* acpWhy) noexcept;
    void GiveAllBack() noexcept;
    [[nodiscard]] int DeathsNear(const Actor* apActor, int aProfile, float aSeconds, float aRange) const noexcept;
    void CountSides(const Actor* apActor, int& aAllies, int& aEnemies) const noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    std::unordered_map<uint32_t, Fighter> m_fighters;
    std::vector<Death> m_deaths;
    // this look's characters in a fight with their targets, the players with their companions, and who fights them
    // (for the odds)
    std::vector<std::pair<Actor*, Actor*>> m_combatants;
    std::vector<Actor*> m_playerSide;
    std::vector<Actor*> m_theirSide;
    float m_timer{0.f};
    float m_clock{0.f};
    bool m_enabled{true};
    bool m_wasEnabled{true};
};
