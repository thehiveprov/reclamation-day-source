#pragma once

#include <Interface/GameHud.h>

#include <array>
#include <string>
#include <vector>

struct World;
struct UpdateEvent;
struct OverlayRestartedEvent;

/**
 * @brief The HUD in the overlay: the compass, health with radiation, AP, the enemy's bar, ammo and grenades drawn by
 * the overlay in its own style, the game's own pieces told to stay hidden (GameHud). Each piece shows exactly when the
 * game would have shown its own (the game is still asked), with the game's data (HUDDataModel) and the player's actor
 * values. Sent to the overlay as the "hud" event whenever something changed. MCM: Reclamation Day > HUD (off: the game's
 * HUD as it was). Fallout 4 only; docs/HUD.md.
 */
class HudService
{
public:
    HudService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~HudService() noexcept = default;

    TP_NOCOPYMOVE(HudService);

private:
    struct Settings
    {
        bool Enabled{true};
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnOverlayRestarted(const OverlayRestartedEvent&) noexcept;
    void LoadSettings() noexcept;
    [[nodiscard]] std::string BuildState() noexcept;
    // The facts docs/HUD.md leaves to the game: a few lines in the log after each load (temporary)
    void Probe() noexcept;
    void Clear() noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_overlayRestartedConnection;

    Settings m_settings;
    double m_settingsTimer{0.0};
    double m_markersTimer{0.0};
    double m_probeTimer{0.0};
    double m_colorTimer{0.0};
    double m_colorSendTimer{0.0}; // until the colour goes to the overlay again (ThemeService), changed or not
    // The game's HUD colour (its settings' sliders), once read
    std::array<int, 3> m_color{};
    bool m_hasColor{false};
    int m_probesLeft{0};
    bool m_hadPlayer{false};
    // The weapon drawn in the right hand: its name (empty: none drawn), whether it fires ammo
    double m_weaponTimer{0.0};
    std::string m_weaponName;
    bool m_weaponHasAmmo{false};
    std::vector<GameHud::CompassMarker> m_markers;
    std::string m_sent;
};
