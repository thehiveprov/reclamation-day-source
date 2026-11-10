#pragma once

#include <unordered_map>

struct World;
struct UpdateEvent;
struct Actor;
struct PlayerCharacter;
struct NotifyPlayerJoined;
struct NotifyPlayerLevel;
struct NotifyPlayerLeft;

/**
 * @brief Floating bars over characters: the name and level, health (and AP if wanted), a star over legendaries and a
 * skull over those far above the player's level. Drawn in the game's own HUD (the HUD movie, in the HUD's colour with
 * its backing, no overlay) as a HudMarker::Layer. Based on Zzyxzz's Actor Widgets (Nexus mod 108919), rewritten.
 *
 * Enemies and friendly characters each have their own rules (always, in combat only, hurt only, only the one aimed at,
 * hidden behind walls), set on Reclamation Day's MCM page (Data/MCM/Config/ReclamationDay); defaults are Actor
 * Widgets' own. Co-op: the other players' characters get a friendly bar with their player name and level, and friendly
 * bars show in combat only by default. Nothing new is synced: the other players' health already arrives, and so do their
 * levels (NotifyPlayerJoined, NotifyPlayerLevel; each game sends its player's when it connects and when it changes).
 *
 * Stays off while Actor Widgets itself is loaded, so nobody sees two bars over everyone. Fallout 4 only.
 */
class ActorBarsService
{
public:
    // When a group's bars show
    struct Rules
    {
        bool Show{true};
        bool AlwaysDisplay{true};
        // The "only" options: while any is on, all of those on have to be met and Always Display doesn't count
        bool CombatOnly{true};
        bool MissingHealthOnly{false};
        // While aiming down the sights, only the bar of the one under the crosshair, over the rules above
        bool AimedTargetOnly{false};
        bool HideBehindWalls{true};
        bool LegendaryStar{true};
        bool Skull{true};
    };

    // MCM's (Data/MCM/Settings/ReclamationDay.ini over these)
    struct Settings
    {
        bool Enabled{true};
        Rules Enemies{};
        Rules Friendly{.LegendaryStar = false, .Skull = false};
        // the other players' characters (friendly rules)
        bool Players{true};
        // enemies' bars in the game's warning red, friendly bars in green (else the HUD's colour)
        bool RedEnemies{true};
        bool FixedFriendlyColour{false};
        bool UppercaseNames{true};
        bool Level{true};
        bool HealthBar{true};
        bool ApBar{false};
        // no AP bar while it's full
        bool HideFullAp{true};
        // the health lost stays a moment, then shrinks over this long (s)
        bool AnimateBars{true};
        float AnimationTime{0.28f};
        bool HideInvisible{true};
        // world units from the camera: no bars further, fading out from FadeStart, no name line beyond UpperHide,
        // smaller from ScaleStart down to MinScale
        float MaxDistance{3000.f};
        bool FadeByDistance{true};
        float FadeStart{2200.f};
        float UpperHide{2600.f};
        bool ScaleByDistance{true};
        float ScaleStart{600.f};
        float ScaleStrength{0.75f};
        float MinScale{0.35f};
        // a dead character's bar stays this long (s)
        float DeadDelay{2.f};
        float Opacity{1.f};
        float Scale{0.75f};
        int MaxBars{50};
    };

    ActorBarsService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~ActorBarsService() noexcept;

    TP_NOCOPYMOVE(ActorBarsService);

    [[nodiscard]] const Settings& GetSettings() const noexcept { return m_settings; }

private:
    // A character that could have a bar, looked at a few times a second (Refresh) and every frame (Show)
    struct Tracked
    {
        // from Refresh
        bool Seen{false};
        bool Enemy{false};
        // another player's character
        bool Player{false};
        bool Dead{false};
        bool InCombat{false};
        bool Invisible{false};
        bool Occluded{false};
        bool Legendary{false};
        bool Skull{false};
        // the name line (name and level)
        std::string Text;
        // which of the head nodes it has (-1: none, the bar goes over its bounds)
        int HeadNode{-1};

        // every frame
        float Alpha{0.f};
        float DeadTime{0.f};
        float LastHealth{-1.f};
        // the health lost: shown from TrailFrom down to the health over the animation, TrailTime after the hit
        float TrailFrom{1.f};
        float TrailTime{0.f};
        float Trail{1.f};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept;
    void OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void UpdateSettings(float aDelta) noexcept;
    void LoadSettings(bool aFileThere) noexcept;
    // Actor Widgets itself loaded: ours stay off
    [[nodiscard]] bool IsStandingDown() noexcept;
    // The legendary keyword and the skull's level difference, once the game's data is there
    void ResolveForms() noexcept;
    // Who could have a bar, what kind, and whether a wall is in the way
    void Refresh(PlayerCharacter* apPlayer, const NiPoint3& acCamera) noexcept;
    // Which bars show where this frame, handed to the HUD
    void Show(PlayerCharacter* apPlayer, const NiPoint3& acCamera, float aDelta) noexcept;
    void Clear() noexcept;
    // Where the character's head is (its node, else the top of its bounds)
    [[nodiscard]] bool GetHead(Actor* apActor, const Tracked& acTracked, NiPoint3& aOut) const noexcept;
    void UpdateTrail(Tracked& aTracked, float aHealth, float aDelta) const noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_playerJoinedConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_playerLeftConnection;

    // the other players' levels, by player ID
    std::unordered_map<uint32_t, uint16_t> m_playerLevels;

    Settings m_settings{};
    std::filesystem::file_time_type m_settingsTime{};
    bool m_settingsLoaded{false};
    float m_settingsTimer{0.f};

    // -1 until looked for
    int m_standingDown{-1};
    bool m_formsResolved{false};
    const void* m_pLegendaryKeyword{nullptr};
    int32_t m_skullDifference{0};

    float m_refreshTimer{0.f};
    // by form ID
    std::unordered_map<uint32_t, Tracked> m_tracked;
    bool m_showing{false};
};
