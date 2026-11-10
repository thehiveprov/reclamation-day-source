#pragma once

#include <Games/Primitives.h>

struct World;
struct UpdateEvent;
struct TESObjectREFR;
struct TESObjectCELL;
class WorkshopService;

// Settlements load their scenery as separate objects (scrap anything): every frame from the main loop (menus included),
// takes the precombined meshes and visibility off the settlement cells the game read, once it has loaded its data
// (always so in the world); the cells it reads later are done as it reads them
void UncombineSettlementCells(bool aInWorld = false) noexcept;

/**
 * @brief Reclamation Day's build tools (Fallout 4): our own take on the mechanics of Place Everywhere by
 * registrator2000 and Scrap Everything. In build mode:
 * - place anywhere: the game's "can't place here" checks pass (overlapping, floating, outside the build area);
 * - scrap anything: anything in the settlement's build area can be outlined, moved and scrapped like what was built
 *   there, dead bodies too (their items go to the workbench); not the living, players, doors, quest objects or markers.
 *   Settlements load their scenery as separate objects (not merged into combined meshes) for it;
 * - snapping: to other objects' snap points (the game's, on by default) and to the game's own hidden grid (32 units,
 *   45 degrees; off by default), each switched on and off;
 * - precise control of the outlined object: rotate on any axis, nudge, scale, level, redraw, and give it the
 *   position, angles or scale of another object;
 * - undo: the player's last changes at the settlement, one by one (WorkshopService keeps them).
 * What the tools change is shared with the other players like any other building.
 *
 * Keys are kept clear of the game's (letters, number row, arrows, Tab, Space, Shift, Ctrl, Alt, F5, F9, F12, console)
 * and ours (F2 and Right Ctrl overlay, F3 debug, F6 to F8 test builds). They are saved in config\BuildTools.ini and can
 * be changed there or in the overlay (Settings, Build tools), which shows them.
 */
class BuildToolsService
{
public:
    BuildToolsService(World& aWorld, entt::dispatcher& aDispatcher);
    ~BuildToolsService() noexcept;

    TP_NOCOPYMOVE(BuildToolsService);

    // Read by the game hooks (game threads)
    [[nodiscard]] bool IsPlaceAnywhere() const noexcept { return m_placeAnywhere; }
    [[nodiscard]] bool IsScrapAnything() const noexcept { return m_scrapAnything; }
    // Scrap anything: whether a reference that isn't one of the settlement's objects can be treated as one
    [[nodiscard]] bool CanTreatAsWorkshopItem(const TESObjectREFR* apRef) const noexcept { return !WhyNotWorkshopItem(apRef); }
    // Why it can't, null if it can
    [[nodiscard]] const char* WhyNotWorkshopItem(const TESObjectREFR* apRef) const noexcept;
    // Called by the game's scrap (hook) just before it scraps a reference: a dead body's items go to the workbench
    void BeforeScrap(TESObjectREFR* apRef) noexcept;
    // What scrapping an object the game has no scrap recipe for gives (ScenerySalvage): into the workbench, and said
    void GiveSalvage(const Vector<std::pair<uint32_t, uint32_t>>& acSalvage, const std::string& acName) noexcept;
    // Scrap anything: the nearest object of the settlement without collision (hanging vines, ...) that a ray from the
    // crosshair goes through closer than aMaxDistance, and how far along the ray; the game's own pick can't see those
    [[nodiscard]] TESObjectREFR* FindUncollidableTarget(const NiPoint3& acOrigin, const NiPoint3& acDirection, float aMaxDistance, float& aDistance) const noexcept;

    // From the overlay: "get" (send the state), "bind" ("action:virtualKey"), "set" ("placeAnywhere:1"), "reset"
    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;

private:
    using Clock = std::chrono::steady_clock;

    enum class Action
    {
        TogglePlaceAnywhere,
        ToggleScrapAnything,
        ToggleObjectSnap,
        ToggleGridSnap,
        ChangeRotationAxis,
        RotateLeft,
        RotateRight,
        RotateStepDown,
        RotateStepUp,
        Level,
        MoveUp,
        MoveDown,
        MoveLeft,
        MoveRight,
        MoveCloser,
        MoveAway,
        MoveStepDown,
        MoveStepUp,
        ToggleWorldAxes,
        ScaleDown,
        ScaleUp,
        ScaleStepDown,
        ScaleStepUp,
        ScaleReset,
        Redraw,
        Undo,
        Remember,
        PastePositionX,
        PastePositionY,
        PastePositionZ,
        PasteAngles,
        PasteScale,
        PasteAll,
        Count
    };

    struct ActionInfo
    {
        Action What;
        const char* Name; // in config\BuildTools.ini and the overlay
        int DefaultKey;
        bool Repeats; // held down: the action repeats
    };
    static const std::array<ActionInfo, static_cast<size_t>(Action::Count)> s_actions;

    struct KeyState
    {
        int VirtualKey = 0;
        bool Down = false;
        Clock::time_point Next{};
    };

    struct Transform
    {
        NiPoint3 Position;
        NiPoint3 Rotation;
        float Scale;
    };

    // A cell's objects without collision (form id, the size of its bounds), as found at a time
    struct CellUncollidables
    {
        const void* pObjects = nullptr;
        uint32_t Count = 0;
        Clock::time_point At{};
        Vector<std::pair<uint32_t, float>> Candidates;
    };

    // The last FindUncollidableTarget: its ray, how far, and what it found
    struct UncollidablePick
    {
        Clock::time_point At{};
        NiPoint3 Origin;
        NiPoint3 Direction;
        float MaxDistance = 0.f;
        uint32_t TargetId = 0;
        float Distance = 0.f;
    };

    void ScanUncollidables(TESObjectCELL* apCell, CellUncollidables& aScan, Clock::time_point aNow) const noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    [[nodiscard]] bool KeysActive() const noexcept;
    void Run(Action aAction) noexcept;
    // Changes the outlined object, keeping its previous state for undo and sharing it with the other players
    void ChangeOutlined(const std::function<void(Transform&)>& acChange) noexcept;

    // Keys and switches: config\BuildTools.ini, and the overlay's view of them
    void ResetKeys() noexcept;
    void LoadConfig() noexcept;
    void SaveConfig() const noexcept;
    void SendState(const std::string& acError = {}) const noexcept;
    // The snapping switches into the game: its placement code (snap points) and its grid setting
    void ApplySnapping() const noexcept;
    // The switches as set on our MCM page (Build Tools): one changed there is taken; aAnnounce: saved and sent to the
    // overlay (not while starting)
    void ReadMcmSettings(bool aAnnounce) noexcept;
    // Why a key can't be used for an action, empty if it can
    [[nodiscard]] std::string CheckKey(Action aAction, int aVirtualKey) const noexcept;

    World& m_world;
    WorkshopService& m_workshop;

    bool m_placeAnywhere = true;
    bool m_scrapAnything = true;
    // Snapping to other objects' snap points (the game's way, on) and to the game's own grid (off by default)
    bool m_objectSnap = true;
    bool m_gridSnap = false;
    // The MCM page's values last seen (place anywhere, scrap anything, snap to objects, snap to a grid; -1 not
    // written yet), and when they were looked at
    std::array<int, 4> m_mcmSeen{-1, -1, -1, -1};
    Clock::time_point m_mcmReadAt{};
    bool m_worldAxes = false;
    int m_rotationAxis = 2; // 0 X, 1 Y, 2 Z
    // Steps start where Place Everywhere's do: 1 degree, 1 unit, 10%
    size_t m_rotationStep = 2;
    size_t m_moveStep = 2;
    size_t m_scaleStep = 2;
    bool m_sentState = false;

    std::array<KeyState, static_cast<size_t>(Action::Count)> m_keys{};
    std::optional<Transform> m_remembered;

    // FindUncollidableTarget (the game's main thread): whether objects' 3D has collision, by reference (its 3D then,
    // and whether), the cells' objects without collision, and its last answer
    mutable TiltedPhoques::Map<uint32_t, std::pair<const void*, bool>> m_collisionCache;
    mutable TiltedPhoques::Map<uint32_t, CellUncollidables> m_uncollidables;
    mutable UncollidablePick m_lastUncollidablePick;

    entt::scoped_connection m_updateConnection;
};
