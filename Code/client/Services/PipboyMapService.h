#pragma once

struct World;
struct UpdateEvent;
struct PlayerCharacter;

/**
 * @brief Realtime Pip-Boy Map: raise the Pip-Boy arm on the move and read a live map on its screen, without opening
 * the Pip-Boy menu. Our remake for Fallout 4 of the New Vegas mod by Stentorious, xConfused and Hitman. First person
 * only, the player's own (not shared with other players). Fallout 4 only.
 *
 * M (the game's Quick Map control, so a remapped key works too) raises it: a tap keeps it up until M again, holding it
 * shows it while held. The weapon is out of the hand and the player moves at their own pace while it's up; attacking,
 * aiming, sprinting, holstering, a menu, the Pip-Boy or a third person view lower it. Outside it's the world map, inside
 * the game's local map of the cell, both at one scale that shows what's around and turned the way the player faces,
 * with the quest targets, locations, exits and the other players on them. config/PipboyMap.ini can turn it off (M opens
 * the game's map again).
 *
 * The screen is the game's GenericMenu (the menu the Pip-Boy's boot sequence uses) with our own movie
 * (Interface/ReclamationDay/PipboyMap.swf, an empty movie the map is drawn into from here); its constructor puts it on
 * the Pip-Boy's screen (PipboyManager::AddMenuToPipboy).
 */
struct PipboyMapService
{
    PipboyMapService(World& aWorld, entt::dispatcher& aDispatcher);
    ~PipboyMapService() noexcept;

    TP_NOCOPYMOVE(PipboyMapService);

    // From the game's input handlers (the game's thread), see PipboyMapService.cpp
    void OnMapButton(bool aDown, float aHeldSeconds) noexcept;
    // The arm is up or going up: what lowers it is taken from the game's controls
    [[nodiscard]] bool IsRaised() const noexcept { return m_phase == Phase::kUp; }
    // Anything of it still on the screen or the arm
    [[nodiscard]] bool IsShowing() const noexcept { return m_phase != Phase::kIdle; }
    void Lower() noexcept;
    // All off at once, before the game's own Pip-Boy takes the arm and the screen
    void Abort() noexcept;

    [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }

private:
    enum class Phase
    {
        kIdle,
        // the arm going up or up, the map drawn
        kUp,
        // the arm going down, the map still drawn
        kLowering,
        // the map taken off the screen, the menu closing after it
        kClosing,
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;

    void Raise(PlayerCharacter* apPlayer) noexcept;
    // The Pip-Boy screen's renderer on (PipboyManager::EnablePipboyShader), without the game's background blur
    void TurnScreenOn() noexcept;
    // Why the arm can't be up (nullptr: it can)
    [[nodiscard]] const char* Blocked(PlayerCharacter* apPlayer, bool aRaising) const noexcept;
    void UpdateWanted(PlayerCharacter* apPlayer, float aDelta) noexcept;
    void Finish() noexcept;

    void LoadConfig() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;

    Phase m_phase{Phase::kIdle};
    float m_phaseTime{0.f};
    // raised by the press still down (held long enough, its release lowers it)
    bool m_raisedByPress{false};
    // the view was third person before
    bool m_restoreThirdPerson{false};
    // we made the game's local map (it's ours to free)
    bool m_madeLocalMap{false};
    // we hid the player's weapon (Actor::CullWeapons, shown again in Finish)
    bool m_weaponsCulled{false};
    // the Pip-Boy screen's renderer is on (from third person, once the first person view is in), and since when
    bool m_screenOn{false};
    float m_screenOnTime{0.f};
    // the GenericMenu showing our movie: asked for a moment after the screen's renderer is on, found once it's open
    bool m_menuAsked{false};
    void* m_pMenu{nullptr};

    // config/PipboyMap.ini
    bool m_enabled{true};
};
