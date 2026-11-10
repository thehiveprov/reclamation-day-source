#pragma once

#include <Structs/MovementRules.h>

#include <vector>

struct World;
struct UpdateEvent;
struct PlayerCharacter;
struct Actor;
struct bhkCharacterController;
struct FileSound;
struct NotifyMovementAction;

/**
 * @brief Reclamation Day's movement mechanics: sliding, dodging, directional movement speed, AP costs of sprinting,
 * swimming and sneaking, and stumbling after big falls. Our own rewrite of the ideas of Enhanced Movement by bp42s,
 * with the slide animation and sounds by Toounx; pushes actors with Actor Velocity Framework by Bingle. Fallout 4 only.
 */
struct MovementService
{
    // Which mechanics are on and what they cost: RD's MCM page Movement. On a hosted world the host's rules apply to
    // everyone (MovementRules, sent in the server's settings); the camera shake stays each player's own.
    struct Rules
    {
        bool Sliding{true};
        float SlideApCost{0.20f}; // share of max AP
        float SlideCameraShake{0.15f}; // 0 to 1, 0 = none
        bool Dodge{true};
        float DodgeApCost{0.15f};
        bool DirectionalSpeed{true};
        bool CheaperSprintOutOfCombat{true};
        float SprintOutOfCombatCost{0.5f}; // share of the normal AP drain, 0 = free
        bool SwimmingApDrain{true};
        bool SneakingApDrain{true};
        bool LandingStumble{true};
    };

    MovementService(World& aWorld, entt::dispatcher& aDispatcher);
    ~MovementService() noexcept;

    TP_NOCOPYMOVE(MovementService);

    [[nodiscard]] const Rules& GetRules() const noexcept { return m_rules; }

    // Called after the engine animates the local player (from a hook, possibly on an animation thread): poses the
    // first person left arm during first person slides
    void AfterPlayerAnimation(PlayerCharacter* apPlayer) noexcept;

    // An NPC this game runs slides the way (unit x, y) at about the speed it was going, as the player's slide does
    // (ParkourService decides when, docs/ENEMY_TACTICS.md); false if it couldn't. Main thread.
    bool StartNpcSlide(Actor* apActor, float aDirectionX, float aDirectionY, float aSpeed, const char* acpWhy) noexcept;
    // NPC slides under way
    [[nodiscard]] size_t NpcSlideCount() const noexcept { return m_npcSlides.size(); }

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    // Another player slid or stumbled: their character here does it too (animation and sounds; where it goes comes
    // from the movement sync)
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;

private:
    // Tells the other players, whose copy of the local player does it too (RequestMovementAction::Action)
    void ShareAction(uint8_t aAction) noexcept;
    // The rules in force, here and for ParkourService: the MCM page's (read again when MCM saves it), or the host's on
    // a world someone else hosts; the host's game sends its own to the server when they change
    void UpdateRules() noexcept;
    void LoadLocalRules() noexcept;
    // The third person slide animation on an actor (the local player or another player's character)
    bool PlaySlideAnimation(Actor* apActor) noexcept;
    // The landing stumble idle that plays on the actor's graph (third or first person); false if none
    static bool PlayLandingIdle(Actor* apActor) noexcept;

    using Clock = std::chrono::steady_clock;

    void UpdateSprintCost(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void UpdateApDrains(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void UpdateSlide(PlayerCharacter* apPlayer, bhkCharacterController* apController) noexcept;
    bool StartSlide(PlayerCharacter* apPlayer, bhkCharacterController* apController) noexcept;
    void StartSlideTail(PlayerCharacter* apPlayer, float aSpeed) noexcept;
    // Our idle that plays the slide animation on a character with their weapon out, or the one for a character with
    // it away; each made the first time it's needed
    struct TESIdleForm* GetSlideIdle(bool aWeaponDrawn) noexcept;
    // The fastest we moved during the last moments (world units per second)
    [[nodiscard]] float GetRecentPeakSpeed() const noexcept;
    void UpdateLanding(PlayerCharacter* apPlayer, bhkCharacterController* apController) noexcept;
    void PlayLandingStumble(PlayerCharacter* apPlayer, float aDrop) noexcept;
    // NPC slides: the tail's push when the main one ends, then done
    void UpdateNpcSlides() noexcept;

    World& m_world;
    Rules m_rules{};

    struct NpcSlide
    {
        uint32_t ActorId;
        Clock::time_point Start;
        float MainDuration;
        float Direction[2];
        float TailSpeed;
        float From[2];
        bool InTail;
    };
    std::vector<NpcSlide> m_npcSlides;

    // the MCM page's rules (the host's part, and how it feels here), when they were read, and the host's last sent
    MovementRules m_localRules{};
    float m_localCameraShake{0.15f};
    bool m_localMarkers{true};
    bool m_settingsLoaded{false};
    std::filesystem::file_time_type m_settingsTime{};
    Clock::time_point m_lastRulesCheck{};
    std::optional<MovementRules> m_sentRules;
    bool m_usingHostRules{false};

    // how we've been moving (world units per second, smoothed)
    float m_lastPosition[2]{};
    bool m_hasLastPosition{false};
    float m_groundVelocity[2]{};
    struct SpeedSample
    {
        Clock::time_point Time{};
        float Speed{0.f};
    };
    std::array<SpeedSample, 32> m_speedSamples{};
    size_t m_nextSpeedSample{0};

    // sprint AP refund out of combat
    float m_lastActionPoints{-1.f};
    bool m_inCombat{false};
    float m_combatCheckTimer{0.f};

    // slide: crouch pressed while sprinting
    bool m_wasSprinting{false};
    uint32_t m_lastStance{0};
    bool m_sliding{false};
    // a slide is the main push (friction from the speed going in) and then a gentler tail
    Clock::time_point m_slideStart{};
    float m_slideStartSpeed{0.f};
    float m_slideMainDuration{0.f};
    bool m_slideInTail{false};
    Clock::time_point m_slideTailStart{};
    float m_slideDirection[2]{};
    float m_slideStartPosition[2]{};
    // smaller while the slide animation moves the body itself
    float m_slidePushScale{1.f};

    std::unique_ptr<FileSound> m_pSlideHitGroundSound;
    std::unique_ptr<FileSound> m_pSlideMovingSound;
    bool m_loggedSlideSounds{false};

    // first person slide arm: set by the slide, blended and applied after the player's animation
    std::atomic<bool> m_firstPersonSlideArm{false};
    float m_armBlend{0.f};
    Clock::time_point m_lastArmUpdate{};
    bool m_loggedArm{false};
    bool m_loggedWorldOrder{false};
    // with the weapon away, with it out
    struct TESIdleForm* m_pSlideIdles[2]{};

    // landing
    bool m_falling{false};
    float m_lastFallTime{0.f};
    float m_fallApex{0.f};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_movementActionConnection;
};
