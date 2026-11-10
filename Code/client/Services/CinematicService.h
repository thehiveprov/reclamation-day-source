#pragma once

struct World;
struct UpdateEvent;

/**
 * @brief Winter of Atom's cinematics: the camera flown along a path read from a file (the opening first). Papyrus asks
 * for one by setting the global RDWoA_CinematicRequest to its number; when it ends, or Space, Enter or Esc skips it,
 * this sets RDWoA_CinematicDone to that number and the request back to 0. Each game plays its own: nothing is synced.
 * Papyrus keeps the player's controls off meanwhile. Request 100 isn't a cinematic: it turns the player around in place
 * (the face editor's fallback when the face camera idle won't play), without the teleport a Papyrus SetAngle is.
 *
 * The camera is the game's free camera state (the one the console's tfc uses), whose translation (+0x28) and two
 * angles (+0x34, +0x38; from FreeCameraState::GetTranslation and GetRotation in the 1.10.163 exe) are written every
 * frame. Which of the angles turns the view and which tilts it, and which way, is measured when a cinematic starts: a
 * few frames with test angles, reading where the world camera then looks (HudMarker::GetWorldToClip). Those are the
 * game's frames, counted on its main loop (HivemindApp::GetFrameCount): the update event can come several times a
 * frame (10-22: the whole measurement took 15 ms, the camera never moved in it, and every cinematic gave up), and the
 * swap chain's present signal never came (10-22 test 11: the camera stuck at the path's start). A request waits until
 * the game draws again (the script asks right after moving the player, a load); a measurement still unfinished after
 * 3 s gives up, and the skip keys work while it measures.
 *
 * The free camera's input context (kTFC, the mouse buttons and wheel as the free camera's) goes on and off with the
 * free camera, as the game keeps it: a save made during a cinematic otherwise left it on for the rest of the session
 * (no attacking or zooming after the prequel's opening, 10-28). Any found on in plain first or third person is taken off.
 *
 * A path is Data\RDWoA\Cinematics\<name>.txt, one key per line: time in seconds, the camera's position, the point it
 * looks at. A position or point is three numbers (world units) or "player" and three numbers (relative to the player:
 * right, forward, up, turned with the player). The camera passes through every key (Catmull-Rom), eased in and out
 * over the whole path. Fallout 4 only.
 */
struct CinematicService
{
    CinematicService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CinematicService() noexcept = default;

    TP_NOCOPYMOVE(CinematicService);

    // A cinematic is flying the camera
    [[nodiscard]] bool IsPlaying() const noexcept { return m_playing != 0; }

private:
    struct Point
    {
        bool PlayerRelative{false};
        float Value[3]{};
    };

    struct Key
    {
        float Time{0.f};
        Point Position;
        Point Look;
    };

    void OnUpdate(const UpdateEvent& acEvent) noexcept;

    bool Start(int aNumber) noexcept;
    // Measures the free camera's angles over the first frames; false while still measuring
    bool Calibrate() noexcept;
    void Fly() noexcept;
    void Finish(const char* acpReason) noexcept;

    void ToWorld(const Point& acPoint, float aOut[3]) const noexcept;
    void PointAt(float aTime, bool aLook, float aOut[3]) const noexcept;

    World& m_world;
    entt::scoped_connection m_updateConnection;

    double m_sinceCheck{0.0};
    // the game's frame at the last check for a request
    uint64_t m_checkFrame{0};
    int m_playing{0};
    std::vector<Key> m_keys;
    float m_duration{0.f};
    float m_time{0.f};
    uint32_t m_previousState{8};
    // the skip keys were all up once since it began
    bool m_skipArmed{false};
    // the player's body was hidden when it began (shown for it, hidden again after)
    bool m_playerWasHidden{false};

    // calibration: the step, the game frame its reading waits for, the seconds spent measuring, where the camera looked at angles 0, 0
    // (heading and pitch, radians), and per field (0 is +0x34, 1 is +0x38) the change in heading and pitch per radian
    int m_step{0};
    uint64_t m_waitUntil{0};
    float m_measureTime{0.f};
    float m_baseHeading{0.f};
    float m_basePitch{0.f};
    float m_measured[2][2]{};
    int m_yawField{0};
    float m_yawRate{1.f};
    float m_pitchRate{1.f};
};
