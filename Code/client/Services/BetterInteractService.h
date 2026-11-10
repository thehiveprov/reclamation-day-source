#pragma once

#include <Games/Fallout4/BetterInteractClip.h>

#include <vector>

struct World;
struct UpdateEvent;
struct NotifyMovementAction;
struct Actor;
struct TESForm;
struct TESObjectREFR;

/**
 * @brief Better Interact's hand (Fallout 4, docs/BETTER_INTERACT.md): picking up an item, pressing a button or pushing a door
 * plays the left arm's motion over whatever the character is doing (BetterInteractClip), the player free to move throughout.
 * A picked up item's copy stays where it lay as the game takes the real one, flies to the hand, and is held until the hand goes
 * out of view: a remake of B42 Loot (Fallout: New Vegas, Nexus 82369) for pick-ups.
 *
 * The game's own activation is never blocked or delayed by any of it: the hook only looks before and after. kInteract movement
 * actions tell the others' games, which play the third person motion on their copy of the character. MCM page Better Interact.
 */
struct BetterInteractService
{
    BetterInteractService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~BetterInteractService() noexcept = default;

    TP_NOCOPYMOVE(BetterInteractService);

    // TESObjectREFR's Activate hook, around the game's own: before it (the item's model is still there) and after it
    void BeforeActivate(TESObjectREFR* apObject, TESObjectREFR* apActivator, bool aFromScript, bool aLooping) noexcept;
    void AfterActivate(bool aActivated) noexcept;

private:
    using Kind = BetterInteractClip::Kind;

    struct Settings
    {
        bool Hands{true};
        bool Grab{true};
        bool Press{true};
        bool Push{true};
        bool ItemInHand{true};
        bool Pull{true};
        bool ThirdPerson{true};
        bool Others{true};
    };

    // An activation seen, its hand to start on the next update
    struct Reach
    {
        bool Valid{false};
        Kind Which{Kind::kGrab};
        uint32_t ObjectId{0};
        uint32_t BaseId{0};
        bool FirstPerson{false};
        bool HasTarget{false};
        glm::vec3 Target{0.f};
        BetterInteractClip::Item Holding{};
        // a form whose model goes in the hand instead (a plant's produce: loaded on the update, never in the activation)
        uint32_t ModelFormId{0};
    };

    struct Playing
    {
        uint32_t ActorId{0};
        uint8_t* pRoot{nullptr};
        bool Local{false};
        bool FirstPerson{false};
        // it's been told to fade out
        bool Fading{false};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept;

    void LoadSettings() noexcept;
    void StartLocal(Reach& aReach) noexcept;
    void Share(const Reach& acReach) noexcept;
    // Why this character's arm can't go out now (nullptr: it can)
    [[nodiscard]] const char* WhyNot(const Actor* apActor, uint8_t* apRoot, bool aLocal) const noexcept;

    World& m_world;
    Settings m_settings{};
    float m_settingsCheck{0.f};
    bool m_settingsRead{false};

    uint32_t m_mainThread{0};
    int m_activatesLogged{0};
    Reach m_preparing{};
    std::vector<Reach> m_reaches;
    std::mutex m_otherThreadLock;
    std::vector<Reach> m_otherThreadReaches; // activates on another thread
    std::vector<Playing> m_playing;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_movementActionConnection;
};
